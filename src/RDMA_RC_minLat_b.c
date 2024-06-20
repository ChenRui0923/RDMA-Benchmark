#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <malloc.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <time.h>
#include <inttypes.h>
#include <infiniband/verbs.h>
#include <errno.h>

enum {
    RECV_WRID = 1,
    SEND_WRID = 2,
};

static int page_size;

struct context {
    struct ibv_context    *context;
    struct ibv_comp_channel *channel;
    struct ibv_pd        *pd;
    struct ibv_mr        *mr;
    struct ibv_dm        *dm;
    struct ibv_cq        **cq;  // 修改为指向CQ数组的指针
    struct ibv_qp        **qp;  // 修改为指向QP数组的指针
    struct ibv_qp_ex    *qpx;
    char            *buf;
    int             size;
    int             send_flags;
    int             rx_depth;
    int             pending;
    int             sockfd;
    struct ibv_port_attr     portinfo;
    uint64_t         completion_timestamp_mask;
    struct dest *remote_info; // 修改为指向数组的指针
    int             qp_count;
};

struct dest {
    int lid;
    int qpn;
    int psn;
    union ibv_gid gid;
    uint64_t dest_addr;
    uint32_t rkey;
};

void wire_gid_to_gid(const char *wgid, union ibv_gid *gid)
{
    char tmp[9];
    __be32 v32;
    int i;
    uint32_t tmp_gid[4];

    for (tmp[8] = 0, i = 0; i < 4; ++i) {
        memcpy(tmp, wgid + i * 8, 8);
        sscanf(tmp, "%x", &v32);
        tmp_gid[i] = be32toh(v32);
    }
    memcpy(gid, tmp_gid, sizeof(*gid));
}

void gid_to_wire_gid(const union ibv_gid *gid, char wgid[])
{
    uint32_t tmp_gid[4];
    int i;

    memcpy(tmp_gid, gid, sizeof(tmp_gid));
    for (i = 0; i < 4; ++i)
        sprintf(&wgid[i * 8], "%08x", htobe32(tmp_gid[i]));
}

enum ibv_mtu mtu_to_enum(int mtu)
{
    switch (mtu) {
    case 256:  return IBV_MTU_256;
    case 512:  return IBV_MTU_512;
    case 1024: return IBV_MTU_1024;
    case 2048: return IBV_MTU_2048;
    case 4096: return IBV_MTU_4096;
    default:   return IBV_MTU_1024;
    }
}
enum bench_mode{
    SINGLE = 1,
    MULTIPLE = 2,
};

static int connect_ctx(struct context *ctx, int port, int my_psn,
              enum ibv_mtu mtu, int sl,
              struct dest *dest, int sgid_idx, int qp_index)
{
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = mtu;
    attr.dest_qp_num = dest->qpn;
    attr.rq_psn = dest->psn;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    attr.ah_attr.is_global = 0;
    attr.ah_attr.dlid = dest->lid;
    attr.ah_attr.sl = sl;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num = port;

    if (dest->gid.global.interface_id) {
        attr.ah_attr.is_global = 1;
        attr.ah_attr.grh.hop_limit = 1;
        attr.ah_attr.grh.dgid = dest->gid;
        attr.ah_attr.grh.sgid_index = sgid_idx;
    }
    if (ibv_modify_qp(ctx->qp[qp_index], &attr,
              IBV_QP_STATE              |
              IBV_QP_AV                 |
              IBV_QP_PATH_MTU           |
              IBV_QP_DEST_QPN           |
              IBV_QP_RQ_PSN             |
              IBV_QP_MAX_DEST_RD_ATOMIC |
              IBV_QP_MIN_RNR_TIMER)) {
        fprintf(stderr, "Failed to modify QP %d to RTR\n", qp_index);
        return 1;
    } else {
        // printf("Modified QP %d to RTR\n", qp_index);
    }

    attr.qp_state        = IBV_QPS_RTS;
    attr.timeout        = 14;
    attr.retry_cnt        = 7;
    attr.rnr_retry        = 7;
    attr.sq_psn        = my_psn;
    attr.max_rd_atomic  = 1;
    if (ibv_modify_qp(ctx->qp[qp_index], &attr,
              IBV_QP_STATE              |
              IBV_QP_TIMEOUT            |
              IBV_QP_RETRY_CNT          |
              IBV_QP_RNR_RETRY          |
              IBV_QP_SQ_PSN             |
              IBV_QP_MAX_QP_RD_ATOMIC)) {
        fprintf(stderr, "Failed to modify QP %d to RTS\n", qp_index);
        return 1;
    } else {
        // printf("Modified QP %d to RTS\n", qp_index);
    }

    return 0;
}

static struct dest *client_exch_dest(struct context *ctx,const char *servername, int port,
                        const struct dest *my_dest, int qp_index)
{
    struct addrinfo *res, *t;
    struct addrinfo hints = {
        .ai_family   = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM
    };
    char *service;
    char msg[sizeof "0000:000000:000000:00000000:0000000000000000:00000000000000000000000000000000"];
    int n;
    int sockfd = -1;
    struct dest *rem_dest = NULL;
    char gid[33];

    if (asprintf(&service, "%d", port) < 0)
        return NULL;

    n = getaddrinfo(servername, service, &hints, &res);

    if (n < 0) {
        fprintf(stderr, "%s for %s:%d\n", gai_strerror(n), servername, port);
        free(service);
        return NULL;
    }

    for (t = res; t; t = t->ai_next) {
        sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
        if (sockfd >= 0) {
            if (!connect(sockfd, t->ai_addr, t->ai_addrlen))
                break;
            close(sockfd);
            sockfd = -1;
        }
    }

    freeaddrinfo(res);
    free(service);

    if (sockfd < 0) {
        fprintf(stderr, "Couldn't connect to %s:%d\n", servername, port);
        return NULL;
    }
    ctx->sockfd = sockfd;

    gid_to_wire_gid(&my_dest->gid, gid);
    sprintf(msg, "%04x:%06x:%06x:%08x:%16lx:%s", my_dest->lid, my_dest->qpn,
                            my_dest->psn, my_dest->rkey, my_dest->dest_addr, gid);
    if (write(sockfd, msg, sizeof msg) != sizeof msg) {
        fprintf(stderr, "Couldn't send local address\n");
        goto out;
    }

    if (read(sockfd, msg, sizeof msg) != sizeof msg ||
        write(sockfd, "done", sizeof "done") != sizeof "done") {
        perror("client read/write");
        fprintf(stderr, "Couldn't read/write remote address\n");
        goto out;
    }

    rem_dest = (struct dest*)malloc(sizeof *rem_dest);
    if (!rem_dest)
        goto out;

    sscanf(msg, "%x:%x:%x:%x:%lx:%s", &rem_dest->lid, &rem_dest->qpn,
                        &rem_dest->psn, &rem_dest->rkey, &rem_dest->dest_addr, gid);
    wire_gid_to_gid(gid, &rem_dest->gid);

    // Store remote info for the current QP index
    ctx->remote_info[qp_index] = *rem_dest;

out:
    return rem_dest;
}

static struct dest *server_exch_dest(struct context *ctx,
                         int ib_port, enum ibv_mtu mtu,
                         int port, int sl,
                         const struct dest *my_dest,
                         int sgid_idx, int qp_index)
{
    struct addrinfo *res, *t;
    struct addrinfo hints = {
        .ai_flags    = AI_PASSIVE,
        .ai_family   = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM
    };
    char *service;
    char msg[sizeof "0000:000000:000000:00000000:0000000000000000:00000000000000000000000000000000"];
    int n;
    int sockfd = -1, connfd;
    struct dest *rem_dest = NULL;
    char gid[33];

    if (asprintf(&service, "%d", port) < 0)
        return NULL;

    n = getaddrinfo(NULL, service, &hints, &res);

    if (n < 0) {
        fprintf(stderr, "%s for port %d\n", gai_strerror(n), port);
        free(service);
        return NULL;
    }

    for (t = res; t; t = t->ai_next) {
        sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
        if (sockfd >= 0) {
            n = 1;

            setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &n, sizeof n);

            if (!bind(sockfd, t->ai_addr, t->ai_addrlen))
                break;
            close(sockfd);
            sockfd = -1;
        }
    }

    freeaddrinfo(res);
    free(service);

    if (sockfd < 0) {
        fprintf(stderr, "Couldn't listen to port %d\n", port);
        return NULL;
    }

    listen(sockfd, 1);
    connfd = accept(sockfd, NULL, NULL);
    close(sockfd);
    if (connfd < 0) {
        fprintf(stderr, "accept() failed\n");
        return NULL;
    }
    ctx->sockfd = connfd;

    n = read(connfd, msg, sizeof msg);
    if (n != sizeof msg) {
        perror("server read");
        fprintf(stderr, "%d/%d: Couldn't read remote address\n", n, (int) sizeof msg);
        goto out;
    }

    rem_dest = (struct dest*) malloc(sizeof *rem_dest);
    if (!rem_dest)
        goto out;

    sscanf(msg, "%x:%x:%x:%x:%lx:%s", &rem_dest->lid, &rem_dest->qpn,
                            &rem_dest->psn, &rem_dest->rkey, &rem_dest->dest_addr, gid);
    wire_gid_to_gid(gid, &rem_dest->gid);

    // Store remote info for the current QP index
    ctx->remote_info[qp_index] = *rem_dest;

    if (connect_ctx(ctx, ib_port, my_dest->psn, mtu, sl, rem_dest,
                                sgid_idx, qp_index)) {
        fprintf(stderr, "Couldn't connect to remote QP %d\n", qp_index);
        free(rem_dest);
        rem_dest = NULL;
        goto out;
    }

    gid_to_wire_gid(&my_dest->gid, gid);
    sprintf(msg, "%04x:%06x:%06x:%08x:%16lx:%s", my_dest->lid, my_dest->qpn,
                            my_dest->psn, my_dest->rkey, my_dest->dest_addr, gid);
    if (write(connfd, msg, sizeof msg) != sizeof msg ||
        read(connfd, msg, sizeof msg) != sizeof "done") {
        fprintf(stderr, "Couldn't send/recv local address\n");
        free(rem_dest);
        rem_dest = NULL;
        goto out;
    }

out:
    return rem_dest;
}

static struct context *init_ctx(struct ibv_device *ib_dev, int size,
                        int rx_depth, int port, int qp_count)
{
    struct context *ctx;
    int access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE ;

    ctx = (struct context *)calloc(1, sizeof *ctx);
    if (!ctx)
        return NULL;

    ctx->size       = size;
    ctx->send_flags = IBV_SEND_SIGNALED;
    ctx->rx_depth   = rx_depth;
    ctx->qp_count   = qp_count;

    ctx->buf = (char *)memalign(page_size, size);
    if (!ctx->buf) {
        fprintf(stderr, "Couldn't allocate work buf.\n");
        goto clean_ctx;
    }

    memset(ctx->buf, 0x7b, size);

    ctx->context = ibv_open_device(ib_dev);
    if (!ctx->context) {
        fprintf(stderr, "Couldn't get context for %s\n",
            ibv_get_device_name(ib_dev));
        goto clean_buffer;
    }

    ctx->channel = NULL;

    ctx->pd = ibv_alloc_pd(ctx->context);
    if (!ctx->pd) {
        fprintf(stderr, "Couldn't allocate PD\n");
        goto clean_comp_channel;
    }

    ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, size, access_flags);

    if (!ctx->mr) {
        fprintf(stderr, "Couldn't register MR\n");
        goto clean_dm;
    }

    ctx->cq = (struct ibv_cq **)malloc(qp_count * sizeof(struct ibv_cq *));
    ctx->qp = (struct ibv_qp **)malloc(qp_count * sizeof(struct ibv_qp *));
    ctx->remote_info = (struct dest *)malloc(qp_count * sizeof(struct dest));
    if (!ctx->cq || !ctx->qp || !ctx->remote_info) {
        fprintf(stderr, "Couldn't allocate CQ/QP/Remote Info array\n");
        goto clean_cq_qp;
    }

    for (int i = 0; i < qp_count; i++) {
        
        ctx->cq[i] = ibv_create_cq(ctx->context, rx_depth + 1, NULL, ctx->channel, 0);
        
        if (!ctx->cq[i]) {
            fprintf(stderr, "Couldn't create CQ %d\n", i);
            goto clean_cq_qp;
        } 
        // else {
        //     printf("create cq success on QP%d\n", i);
        // }

        {
            struct ibv_qp_attr attr;
		    memset(&attr, 0, sizeof(attr));
            struct ibv_qp_init_attr init_attr;
            memset(&init_attr, 0, sizeof(init_attr));
            init_attr.send_cq = ctx->cq[i];
            init_attr.recv_cq = ctx->cq[i];
            init_attr.cap.max_send_wr  = 128;
            init_attr.cap.max_recv_wr  = rx_depth;
            init_attr.cap.max_send_sge = 16;
            init_attr.cap.max_recv_sge = 16;
            init_attr.qp_type = IBV_QPT_RC;

            ctx->qp[i] = ibv_create_qp(ctx->pd, &init_attr);
            if (!ctx->qp[i]) {
                fprintf(stderr, "Couldn't create QP %d\n", i);
                goto clean_cq_qp;
            } else {
                printf("create qp success on QP%d\n", i);
            }
            // ibv_query_qp(ctx->qp[i], &attr, IBV_QP_CAP, &init_attr);
            // if (init_attr.cap.max_inline_data >= size)
            //     ctx->send_flags |= IBV_SEND_INLINE;
        }
        {
            struct ibv_qp_attr attr;
            memset(&attr, 0, sizeof(attr));
            attr.qp_state        = IBV_QPS_INIT;
            attr.pkey_index      = 0;
            attr.port_num        = port;
            attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;

            if (ibv_modify_qp(ctx->qp[i], &attr,
                    IBV_QP_STATE              |
                    IBV_QP_PKEY_INDEX         |
                    IBV_QP_PORT               |
                    IBV_QP_ACCESS_FLAGS)) {
                fprintf(stderr, "Failed to modify QP %d to INIT\n", i);
                goto clean_cq_qp; 
            }
        }
    }

    return ctx;

clean_cq_qp:
    if (ctx->qp) {
        for (int i = 0; i < qp_count; i++) {
            if (ctx->qp[i]) {
                ibv_destroy_qp(ctx->qp[i]);
            }
        }
        free(ctx->qp);
    }
    if (ctx->cq) {
        for (int i = 0; i < qp_count; i++) {
            if (ctx->cq[i]) {
                ibv_destroy_cq(ctx->cq[i]);
            }
        }
        free(ctx->cq);
    }

    if (ctx->remote_info) {
        free(ctx->remote_info);
    }

clean_mr:
    ibv_dereg_mr(ctx->mr);

clean_dm:
    if (ctx->dm)
        ibv_free_dm(ctx->dm);

clean_pd:
    ibv_dealloc_pd(ctx->pd);

clean_comp_channel:
    if (ctx->channel)
        ibv_destroy_comp_channel(ctx->channel);

clean_device:
    ibv_close_device(ctx->context);

clean_buffer:
    free(ctx->buf);

clean_ctx:
    free(ctx);

    return NULL;
}

static int close_ctx(struct context *ctx)
{
    for (int i = 0; i < ctx->qp_count; i++) {
        if (ibv_destroy_qp(ctx->qp[i])) {
            fprintf(stderr, "Couldn't destroy QP %d\n", i);
            return 1;
        }
    }

    for (int i = 0; i < ctx->qp_count; i++) {
        if (ibv_destroy_cq(ctx->cq[i])) {
            fprintf(stderr, "Couldn't destroy CQ %d\n", i);
            return 1;
        }
    }

    if (ibv_dereg_mr(ctx->mr)) {
        fprintf(stderr, "Couldn't deregister MR\n");
        return 1;
    }

    if (ctx->dm) {
        if (ibv_free_dm(ctx->dm)) {
            fprintf(stderr, "Couldn't free DM\n");
            return 1;
        }
    }

    if (ibv_dealloc_pd(ctx->pd)) {
        fprintf(stderr, "Couldn't deallocate PD\n");
        return 1;
    }

    if (ctx->channel) {
        if (ibv_destroy_comp_channel(ctx->channel)) {
            fprintf(stderr, "Couldn't destroy completion channel\n");
            return 1;
        }
    }

    if (ibv_close_device(ctx->context)) {
        fprintf(stderr, "Couldn't release context\n");
        return 1;
    }

    free(ctx->buf);
    free(ctx->qp);
    free(ctx->cq);
    free(ctx->remote_info);
    free(ctx);

    return 0;
}

int poll_cq(struct context *ctx, int nums, int qp_index) {
    struct ibv_wc wc[nums];
    uint32_t nums_cqe = 0;
    while (nums_cqe < nums) {
        int cqes = ibv_poll_cq(ctx->cq[qp_index], nums, wc);
        if (cqes < 0) {
            fprintf(stderr, "Poll CQ error on QP %d. nums_cqe %d\n", qp_index, nums_cqe);
            return -1;
        }
        nums_cqe += cqes;
        for (int j = 0; j < cqes; ++j) {
            if (wc[j].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "Failed status %s (%d) for wr_id %d on QP %d\n",
                        ibv_wc_status_str(wc[j].status), wc[j].status, (int)wc[j].wr_id, qp_index);
                return -1;
            }
        }
    }
    return nums_cqe;
}

void rdma_write_ops(struct context *ctx, unsigned int iters, uint32_t size, int qp_index) {
    uint32_t nums_cqe = 0;
    struct ibv_wc wc[25];
    for (int i = 1; i <= iters; i++) {
        struct ibv_send_wr wr, *bad_wr;
        struct ibv_sge list = {
            .addr    = (uintptr_t) ctx->buf,
            .length = size,
            .lkey    = ctx->mr->lkey
        };

        memset(&wr, 0, sizeof(wr));
        wr.wr_id = i;
        wr.sg_list = &list;
        wr.num_sge = 1;
        wr.opcode = IBV_WR_RDMA_WRITE;
        wr.send_flags = IBV_SEND_SIGNALED;
        wr.wr.rdma.remote_addr = ctx->remote_info[qp_index].dest_addr;
        wr.wr.rdma.rkey = ctx->remote_info[qp_index].rkey;

        if (ibv_post_send(ctx->qp[qp_index], &wr, &bad_wr)) {
            perror("Failed to post SR");
            printf("nums_cqe %d iter %d on QP %d\n", nums_cqe, i, qp_index);
            return;
        }
        if (i && i % 50 == 0) {
            int ret = poll_cq(ctx, 50, qp_index);
            if (ret < 0) {
                perror("Error in poll");
                return;
            }
            nums_cqe += ret;
        }
    }
}

void measure_latency(struct context *ctx, int qp_index, unsigned int iters, uint32_t size, struct timeval *latency) {
    struct timeval start, end;
    // Measure latency
    if (gettimeofday(&start, NULL)) {
        perror("gettimeofday");
        return;
    }
    rdma_write_ops(ctx, iters, size, qp_index);
    if (gettimeofday(&end, NULL)) {
        perror("gettimeofday");
        return;
    }
    timersub(&end, &start, latency);
}

void rdma_write_benchmark(struct context *ctx, unsigned int iters, uint32_t max_size, enum bench_mode mode, int qp_index) {
    struct timeval start, end;
    int size = 0;
    if (mode == SINGLE) {
        size = max_size;
    } else if (mode == MULTIPLE) {
        size = 2;
    }

    printf("RDMA Write Benchmark\n");
    printf("Connection type : %s\n", "RC");
    printf("%-20s %-20s %-20s \n", "Message size(byte) ", "Iterations", "Bandwidth(Gbps)");
    while (size <= max_size) {
        // start write
        if (gettimeofday(&start, NULL)) {
            perror("gettimeofday");
            return;
        }
        rdma_write_ops(ctx, iters, size, qp_index);
        // end write
        if (gettimeofday(&end, NULL)) {
            perror("gettimeofday");
            return;
        }
        {
            float usec = (end.tv_sec - start.tv_sec) * 1000000 +
                (end.tv_usec - start.tv_usec);
            long long bytes = (long long) size * iters;
            double  bw = bytes * 8.0 / (usec) / 1000;
            printf("%-20d  %-20d   %-20.3lf \n", size, iters, bw);
        }
        if (size < max_size && size * 2 > max_size) {
            size = max_size;
        } else {
            size *= 2;
        }
    }
}

static void usage(const char *argv0)
{
    printf("Usage:\n");
    printf("  %s            start a server and wait for connection\n", argv0);
    printf("  %s <host>     connect to server at <host>\n", argv0);
    printf("\n");
    printf("Options:\n");
    printf("  -p, --port=<port>      listen on/connect to port <port> (default 18515)\n");
    printf("  -d, --ib-dev=<dev>     use IB device <dev> (default first device found)\n");
    printf("  -i, --ib-port=<port>   use port <port> of IB device (default 1)\n");
    printf("  -s, --size=<size>      size of message to exchange (default 4096)\n");
    printf("  -m, --mtu=<size>       path MTU (default 1024)\n");
    printf("  -r, --rx-depth=<dep>   number of receives to post at a time (default 500)\n");
    printf("  -n, --iters=<iters>    number of exchanges (default 1000)\n");
    printf("  -l, --sl=<sl>          service level value\n");
    printf("  -g, --gid-idx=<gid index> local port gid index\n");
    printf("  -u, --max-size=<size>  max size of message to exchange (default 0)\n");
    printf("  -q, --qp-count=<count> number of QPs to create (default 2)\n");
}

int main(int argc, char *argv[]) {
    struct ibv_device      **dev_list;
    struct ibv_device    *ib_dev;
    struct context *ctx;
    // struct dest     my_dest;
    struct dest    *rem_dest;
    struct timeval           start, end;
    char                    *ib_devname = NULL;
    char                    *servername = NULL;
    unsigned int             port = 18515;
    int                      ib_port = 1;
    unsigned int             size = 0;
    unsigned int             max_size = 4096;
    enum ibv_mtu         mtu = IBV_MTU_1024;
    unsigned int             rx_depth = 500;
    unsigned int             iters = 1000;
    int                      sl = 0;
    int             gidx = -1;
    char             gid[33];
    char                     sync_message[sizeof("done")];
    int                      qp_count = 2;

    srand48(getpid() * time(NULL));

    while (1) {
        int c;

        static struct option long_options[] = {
            { "port",     1, NULL, 'p' },
            { "ib-dev",   1, NULL, 'd' },
            { "ib-port",  1, NULL, 'i' },
            { "size",     1, NULL, 's' },
            { "mtu",      1, NULL, 'm' },
            { "rx-depth", 1, NULL, 'r' },
            { "iters",    1, NULL, 'n' },
            { "sl",       1, NULL, 'l' },
            { "max-size", 1, NULL, 'u' },
            { "qp-count", 1, NULL, 'q' },
            { "gid-idx",  1, NULL, 'g' },
            { NULL,       0, NULL, 0 }
        };

        // c = getopt_long(argc, argv, "p:d:i:s:m:r:n:l:u:g:q:",
        //         long_options, NULL);
        c = getopt_long(argc, argv, "p:d:i:s:m:r:n:l:u:q:eg:oOPtcjN",
                long_options, NULL);

        if (c == -1)
            break;

        switch (c) {
        case 'p':
            port = strtoul(optarg, NULL, 0);
            if (port > 65535) {
                usage(argv[0]);
                return 1;
            }
            break;

        case 'd':
            ib_devname = strdup(optarg);
            break;

        case 'i':
            ib_port = strtol(optarg, NULL, 0);
            if (ib_port < 1) {
                usage(argv[0]);
                return 1;
            }
            break;

        case 's':
            size = strtoul(optarg, NULL, 0);
            break;

        case 'm':
            mtu = mtu_to_enum(strtol(optarg, NULL, 0));
            if (mtu == 0) {
                usage(argv[0]);
                return 1;
            }
            break;

        case 'r':
            rx_depth = strtoul(optarg, NULL, 0);
            break;

        case 'n':
            iters = strtoul(optarg, NULL, 0);
            break;

        case 'l':
            sl = strtol(optarg, NULL, 0);
            break;

        case 'g':
            gidx = strtol(optarg, NULL, 0);
            break;

        case 'u':
            max_size = strtol(optarg, NULL, 0);
            break;

        case 'q':
            qp_count = strtol(optarg, NULL, 0);
            break;

        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (optind == argc - 1)
        servername = strdup(argv[optind]);
    else if (optind < argc) {
        usage(argv[0]);
        return 1;
    }

    page_size = sysconf(_SC_PAGESIZE);

    dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        perror("Failed to get IB devices list");
        return 1;
    }

    if (!ib_devname) {
        ib_dev = *dev_list;
        if (!ib_dev) {
            fprintf(stderr, "No IB devices found\n");
            return 1;
        }
    } else {
        int i;
        for (i = 0; dev_list[i]; ++i)
            if (!strcmp(ibv_get_device_name(dev_list[i]), ib_devname))
                break;
        ib_dev = dev_list[i];
        if (!ib_dev) {
            fprintf(stderr, "IB device %s not found\n", ib_devname);
            return 1;
        }
    }

    ctx = init_ctx(ib_dev, size <= 0 ? max_size : size, rx_depth, ib_port, qp_count);
    if (!ctx)
        return 1;

    if (ibv_query_port(ctx->context, ib_port, &ctx->portinfo)) {
        fprintf(stderr, "Couldn't get port info\n");
        return 1;
    }

    // my_dest.lid = ctx->portinfo.lid;
    // if (ctx->portinfo.link_layer != IBV_LINK_LAYER_ETHERNET &&
    //                         !my_dest.lid) {
    //     fprintf(stderr, "Couldn't get local LID\n");
    //     return 1;
    // }

    // if (gidx >= 0) {
    //     if (ibv_query_gid(ctx->context, ib_port, gidx, &my_dest.gid)) {
    //         fprintf(stderr, "can't read sgid of index %d\n", gidx);
    //         return 1;
    //     }
    // } else
    //     memset(&my_dest.gid, 0, sizeof my_dest.gid);

    // my_dest.qpn = ctx->qp[0]->qp_num;  // Use the first QP for initial exchange
    // my_dest.psn = lrand48() & 0xffffff;
    // inet_ntop(AF_INET6, &my_dest.gid, gid, sizeof gid);
    // my_dest.rkey = ctx->mr->rkey;
    // my_dest.dest_addr = (uint64_t)ctx->mr->addr;
    // printf("  local address:  LID 0x%04x, QPN 0x%06x, PSN 0x%06x,KEY 0x%08x,ADDR 0x%lx,GID %s\n",
    //        my_dest.lid, my_dest.qpn, my_dest.psn, my_dest.rkey, my_dest.dest_addr, gid);

    // if (servername) {
    //     for (int i = 0; i < qp_count; i++) {
    //         rem_dest = client_exch_dest(ctx, servername, port, &my_dest, i);
    //         if (!rem_dest)
    //             return 1;

    //         int my_psn = lrand48() & 0xffffff;  // Ensure each QP has a unique PSN
    //         if (connect_ctx(ctx, ib_port, my_psn, mtu, sl, rem_dest, gidx, i)) {
    //             fprintf(stderr, "Couldn't connect QP %d\n", i);
    //             return 1;
    //         } else {
    //             printf("Successfully connected QP %d\n", i);
    //         }
    //     }
    // } else {
    //     for (int i = 0; i < qp_count; i++) {
    //         rem_dest = server_exch_dest(ctx, ib_port, mtu, port, sl, &my_dest, gidx, i);
    //         if (!rem_dest)
    //             return 1;
    //     }
    // }
for (int i = 0; i < qp_count; i++) {
    struct dest my_dest; // 每次循环初始化一个新的 my_dest 结构体

    // 初始化 my_dest 的 LID 和 GID
    my_dest.lid = ctx->portinfo.lid;
    if (ctx->portinfo.link_layer != IBV_LINK_LAYER_ETHERNET && !my_dest.lid) {
        fprintf(stderr, "Couldn't get local LID\n");
        return 1;
    }
    if (gidx >= 0) {
        if (ibv_query_gid(ctx->context, ib_port, gidx, &my_dest.gid)) {
            fprintf(stderr, "can't read sgid of index %d\n", gidx);
            return 1;
        }
    } else {
        memset(&my_dest.gid, 0, sizeof(my_dest.gid));
    }

    // 初始化 my_dest 的 QPN 和 PSN
    my_dest.qpn = ctx->qp[i]->qp_num;
    my_dest.psn = lrand48() & 0xffffff;

    // 初始化 my_dest 的 rkey 和地址
    my_dest.rkey = ctx->mr->rkey;
    my_dest.dest_addr = (uint64_t)ctx->mr->addr;

    // 打印本地地址信息
    char gid[33];
    inet_ntop(AF_INET6, &my_dest.gid, gid, sizeof(gid));
    printf("--QP %d local address:  LID 0x%04x, QPN 0x%06x, PSN 0x%06x, KEY 0x%08x, ADDR 0x%lx, GID %s\n",
           i, my_dest.lid, my_dest.qpn, my_dest.psn, my_dest.rkey, my_dest.dest_addr, gid);

    // 客户端和服务器交换地址信息并连接QP
    // if (servername) {
    //     rem_dest = client_exch_dest(ctx, servername, port, &my_dest, i);

    if (servername)
		rem_dest = client_exch_dest(ctx,servername, port, &my_dest, i);
	else
		rem_dest = server_exch_dest(ctx, ib_port, mtu, port, sl,
								&my_dest, gidx, i);
    
    if (!rem_dest) 
        return 1;

    inet_ntop(AF_INET6, &rem_dest->gid, gid, sizeof gid);
	printf(" --QP %d remote address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x,KEY 0x%08x,ADDR 0x%lx,GID %s\n",
	       i, rem_dest->lid, rem_dest->qpn, rem_dest->psn,rem_dest->rkey,rem_dest->dest_addr, gid);
        
    if (servername)
		if (connect_ctx(ctx, ib_port, my_dest.psn, mtu, sl, rem_dest,
					gidx, i))
			return 1;

	ctx->pending = RECV_WRID;


        
    // int my_psn = lrand48() & 0xffffff;  // 确保每个QP都有唯一的PSN
    //     if (connect_ctx(ctx, ib_port, my_psn, mtu, sl, rem_dest, gidx, i)) {
    //         fprintf(stderr, "Couldn't connect QP %d\n", i);
    //         return 1;
    //     } else {
    //         // printf("Successfully connected QP %d\n", i);
    //     }
    // } else {
    //     rem_dest = server_exch_dest(ctx, ib_port, mtu, port, sl, &my_dest, gidx, i);
    //     if (!rem_dest) {
    //         return 1;
    //     }
    // }

    inet_ntop(AF_INET6, &rem_dest->gid, gid, sizeof(gid));
    printf("--QP %d remote address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x, KEY 0x%08x, ADDR 0x%lx, GID %s\n",
           i, rem_dest->lid, rem_dest->qpn, rem_dest->psn, rem_dest->rkey, rem_dest->dest_addr, gid);
}



    
    struct timeval latencies[qp_count];
    memset(latencies, 0, sizeof(latencies));

    if (servername) {    // is client 

        // /*RDMA latency test for each qp*/
        for (int i = 0; i < qp_count; i++) {
            int m_size = 1048576;
            measure_latency(ctx, i, iters, m_size, &latencies[i]);
            printf("QP %d latency: %ld.%06lds\n", i, (long int)latencies[i].tv_sec, (long int)latencies[i].tv_usec);
        }

        /*Determine the best qp*/
        int best_qp = 0;
        for (int i = 1; i < qp_count; i++) {
            if (timercmp(&latencies[i], &latencies[best_qp], <)) {
                best_qp = i;
            }
        }
        printf("Best QP: %d with latency %ld.%06lds\n", best_qp, (long int)latencies[best_qp].tv_sec, (long int)latencies[best_qp].tv_usec);


        /*RDMA bw test for the best qp*/
        if (size == 0) {
            rdma_write_benchmark(ctx, iters, max_size, MULTIPLE, best_qp);
        } else {
            rdma_write_benchmark(ctx, iters, size, SINGLE, best_qp);
        }

        memcpy(sync_message, "done", sizeof("done"));
        int ret = write(ctx->sockfd, sync_message, sizeof(sync_message));
        if (ret < 0) {
            printf("Client socket sync error %d: %s\n", ret, strerror(errno));
        }
    } else {    // is server
        int ret = read(ctx->sockfd, sync_message, sizeof(sync_message));
        if (ret < 0) {
            printf("Server socket sync error %d: %s\n", ret, strerror(errno));
        }
    }


    if (close_ctx(ctx))
        return 1;

    ibv_free_device_list(dev_list);
    free(rem_dest);

    return 0;
}
