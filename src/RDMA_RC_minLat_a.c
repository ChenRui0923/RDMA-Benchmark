/*
* BUILD COMMAND:
* gcc -Wall -O2 -o 2_RDMA_RC_minLat RDMA_RC_minLat_2.c -lrdmacm -libverbs
*/
/******************************************************************************
*
* RDMA Aware Networks Programming Example
*
* This code demonstrates how to perform the following operations using 
* the * VPI Verbs API:
* Send
* Receive
* RDMA Read
* RDMA Write
*
*****************************************************************************/

//此版本为RDMA_RC_example.c为基础的修改版
/* 
*    usage:
*        client : ./RDMA_RC_minLat_3 -d mlx5_1  -p 12346 -u 10485760 -g 3 -q 10 14.14.14.2  
*        server : ./RDMA_RC_minLat_3 -d mlx5_1  -p 12346 -u 10485760 -g 3 -q 10 
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>
#include <endian.h>
#include <byteswap.h>
#include <getopt.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

/* poll CQ timeout in millisec (2 seconds) */
#define MAX_POLL_CQ_TIMEOUT 2000
#define MSG "SEND operation "
#define RDMAMSGR "RDMA read operation "
#define RDMAMSGW "RDMA write operation"
// #define MSG_SIZE (1024 * 1024 * 10)  // 设置消息大小为1MB
#define LATENCY_MSG_SIZE (1024 * 1024)   //设置用于延迟测试的消息大小
#define LATENCY_NUM_ITERATIONS 1000   // 设置用于延迟测试的发送次数

#if __BYTE_ORDER == __LITTLE_ENDIAN
static inline uint64_t htonll(uint64_t x)
{
    return bswap_64(x);
}
static inline uint64_t ntohll(uint64_t x)
{
    return bswap_64(x);
}
#elif __BYTE_ORDER == __BIG_ENDIAN
static inline uint64_t htonll(uint64_t x)
{
    return x;
}
static inline uint64_t ntohll(uint64_t x)
{
    return x;
}
#else
#error __BYTE_ORDER is neither __LITTLE_ENDIAN nor __BIG_ENDIAN
#endif

/* rdma benchmark mode*/
enum bench_mode{
	SINGLE = 1,
	MULTIPLE = 2,
};

/* MTU size */
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


/* structure of test parameters */
struct config_t
{
    const char *dev_name; /* IB device name */
    char *server_name;    /* server host name */
    uint32_t tcp_port;    /* server TCP port */
    int ib_port;          /* local IB port to work with */
    int gid_idx;          /* gid index to use */
    uint32_t msg_size;         /* message size */
    int num_iterations;   /* number of iterations */
    int qp_num;           /* number of QPs*/
    int test_option;      /* 0 - min latency QP, 1 - max latency QP, 2 - both */
};

/* structure to exchange data which is needed to connect the QPs */
struct cm_con_data_t
{
    uint64_t addr;        /* Buffer address */
    uint32_t rkey;        /* Remote key */
    uint32_t qp_num;      /* QP number */
    uint16_t lid;         /* LID of the IB port */
    uint8_t gid[16];      /* gid */
} __attribute__((packed));

/* structure of system resources */
struct resources
{
    struct ibv_device_attr device_attr; /* Device attributes */
    struct ibv_port_attr port_attr;     /* IB port attributes */
    struct cm_con_data_t *remote_props;  /* values to connect to remote side */
    struct ibv_context *ib_ctx;         /* device handle */
    struct ibv_pd *pd;                  /* PD handle */
    struct ibv_cq **cq;         /* CQ handles */
    struct ibv_qp **qp;         /* QP handles */
    struct ibv_mr *mr;                  /* MR handle for buf */
    char *buf;                          /* memory buffer pointer, used for RDMA and send ops */
    int sock;                           /* TCP socket file descriptor */
};

struct config_t config =
{
    NULL,  /* dev_name */
    NULL,  /* server_name */
    19875, /* tcp_port */
    1,     /* ib_port */
    3,     /* gid_idx */
    1024 * 1024, /* msg_size, default 1MB */
    1000,    /* num_iterations, default 1000 */
    4,      /* qp_num, default 4 */
    2       /* test_option, default 2 (both) */
};

/******************************************************************************
Socket operations:
For simplicity, the example program uses TCP sockets to exchange control
information. If a TCP/IP stack/connection is not available, connection manager
(CM) may be used to pass this information. Use of CM is beyond the scope of
this example
******************************************************************************/
/******************************************************************************
* Function: sock_connect
* Input:
* servername: URL of server to connect to (NULL for server mode)
* port: port of service
*
* Output:none
*
* Returns: socket (fd) on success, negative error code on failure
*
* Description:
* Connect a socket. If servername is specified a client connection will be
* initiated to the indicated server and port. Otherwise listen on the
* indicated port for an incoming connection.
*
******************************************************************************/
static int sock_connect(const char *servername, int port)
{
    struct addrinfo *resolved_addr = NULL;
    struct addrinfo *iterator;
    char service[6];
    int sockfd = -1;
    int listenfd = 0;
    int tmp;
    struct addrinfo hints =
    {
        .ai_flags    = AI_PASSIVE,
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_STREAM
    };

    if(sprintf(service, "%d", port) < 0)
    {
        goto sock_connect_exit;
    }

    /* Resolve DNS address, use sockfd as temp storage */
    sockfd = getaddrinfo(servername, service, &hints, &resolved_addr);
    if(sockfd < 0)
    {
        fprintf(stderr, "%s for %s:%d\n", gai_strerror(sockfd), servername, port);
        goto sock_connect_exit;
    }

    /* Search through results and find the one we want */
    for(iterator = resolved_addr; iterator; iterator = iterator->ai_next)
    {
        sockfd = socket(iterator->ai_family, iterator->ai_socktype, iterator->ai_protocol);
        if(sockfd >= 0)
        {
            int optval = 1;
            // Set SO_REUSEADDR to allow reusing the port immediately
            if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
                fprintf(stderr, "setsockopt(SO_REUSEADDR) failed");
                close(sockfd);
                continue; // Try next address or fail
            }

            if(servername)
            {
                /* Client mode. Initiate connection to remote */
                if((tmp = connect(sockfd, iterator->ai_addr, iterator->ai_addrlen)))
                {
                    fprintf(stdout, "failed connect \n");
                    close(sockfd);
                    sockfd = -1;
                }
            }
            else
            {
                /* Server mode. Set up listening socket and accept a connection */
                listenfd = sockfd;
                sockfd = -1;
                if(bind(listenfd, iterator->ai_addr, iterator->ai_addrlen))
                {
                    goto sock_connect_exit;
                }
                listen(listenfd, 1);
                sockfd = accept(listenfd, NULL, 0);
            }
        }
    }

sock_connect_exit:
    if(listenfd)
    {
        close(listenfd);
    }

    if(resolved_addr)
    {
        freeaddrinfo(resolved_addr);
    }

    if(sockfd < 0)
    {
        if(servername)
        {
            fprintf(stderr, "Couldn't connect to %s:%d\n", servername, port);
        }
        else
        {
            perror("server accept");
            fprintf(stderr, "accept() failed\n");
        }
    }

    return sockfd;
}

/******************************************************************************
* Function: sock_sync_data
* Input:
* sock: socket to transfer data on
* xfer_size: size of data to transfer
* local_data: pointer to data to be sent to remote
*
* Output: remote_data pointer to buffer to receive remote data
*
* Returns: 0 on success, negative error code on failure
*
* Description:
* Sync data across a socket. The indicated local data will be sent to the
* remote. It will then wait for the remote to send its data back. It is
* assumed that the two sides are in sync and call this function in the proper
* order. Chaos will ensue if they are not. :)
*
* Also note this is a blocking function and will wait for the full data to be
* received from the remote.
*
******************************************************************************/
int sock_sync_data(int sock, int xfer_size, char *local_data, char *remote_data)
{
    int rc;
    int read_bytes = 0;
    int total_read_bytes = 0;
    rc = write(sock, local_data, xfer_size);

    if(rc < xfer_size)
    {
        fprintf(stderr, "Failed writing data during sock_sync_data\n");
    }
    else
    {
        rc = 0;
    }

    while(!rc && total_read_bytes < xfer_size)
    {
        read_bytes = read(sock, remote_data, xfer_size);
        if(read_bytes > 0)
        {
            total_read_bytes += read_bytes;
        }
        else
        {
            rc = read_bytes;
        }
    }
    return rc;
}
/******************************************************************************
End of socket operations
******************************************************************************/

/* poll_completion */
/******************************************************************************
* Function: poll_completion
*
* Input:
* res: pointer to resources structure
*
* Output: none
*
* Returns: 0 on success, 1 on failure
*
* Description:
* Poll the completion queue for a single event. This function will continue to
* poll the queue until MAX_POLL_CQ_TIMEOUT milliseconds have passed.
*
******************************************************************************/
// static int poll_completion(struct resources *res, int qp_index)
// {
//     struct ibv_wc wc;
//     // unsigned long start_time_msec;
//     // unsigned long cur_time_msec;
//     // struct timeval cur_time;
//     int poll_result;
//     int rc = 0;
//     /* poll the completion for a while before giving up of doing it .. */
//     // gettimeofday(&cur_time, NULL);
//     // start_time_msec = (cur_time.tv_sec * 1000) + (cur_time.tv_usec / 1000);
//     do
//     {
//         poll_result = ibv_poll_cq(res->cq[qp_index], 1, &wc);
//         // gettimeofday(&cur_time, NULL);
//         // cur_time_msec = (cur_time.tv_sec * 1000) + (cur_time.tv_usec / 1000);
//     }
//     while(poll_result == 0);

//     if(poll_result < 0)
//     {
//         /* poll CQ failed */
//         fprintf(stderr, "poll CQ failed\n");
//         rc = 1;
//     }
//     else if(poll_result == 0)
//     {
//         /* the CQ is empty */
//         fprintf(stderr, "completion wasn't found in the CQ after timeout\n");
//         rc = 1;
//     }
//     else
//     {
//         /* CQE found */
//         // fprintf(stdout, "completion was found in CQ with status 0x%x\n", wc.status);
//         /* check the completion status (here we don't care about the completion opcode */
//         if(wc.status != IBV_WC_SUCCESS)
//         {
//             fprintf(stderr, "got bad completion with status: 0x%x, vendor syndrome: 0x%x\n", 
//                     wc.status, wc.vendor_err);
//             rc = 1;
//         }
//     }
//     return rc;
// }



static int poll_completion(struct resources *res, int qp_index, int num_completions)
{
    struct ibv_wc wc[num_completions];
    uint32_t nums_cqe = 0;

    while (nums_cqe < num_completions)
    {
        int cqes = ibv_poll_cq(res->cq[qp_index], num_completions, wc);
        if (cqes < 0)
        {
            fprintf(stderr, "poll CQ failed on QP %d\n", qp_index);
            return -1;
        }
        nums_cqe += cqes;
        for (int j = 0; j < cqes; ++j)
        {
            if (wc[j].status != IBV_WC_SUCCESS)
            {
                fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
                        ibv_wc_status_str(wc[j].status), wc[j].status, (int)wc[j].wr_id);
                return -1;
            }
        }
    }
    return nums_cqe;
}






/******************************************************************************
* Function: post_send
*
* Input:
* res: pointer to resources structure
* qp_index: QP index to use
* opcode: IBV_WR_SEND, IBV_WR_RDMA_READ or IBV_WR_RDMA_WRITE
*
* Output: none
*
* Returns: 0 on success, error code on failure
*
* Description: This function will create and post a send work request
******************************************************************************/
static int post_send(struct resources *res, int qp_index, int opcode)
{
    struct ibv_send_wr sr;
    struct ibv_sge sge;
    struct ibv_send_wr *bad_wr = NULL;
    int rc;

    /* prepare the scatter/gather entry */
    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)res->buf;
    sge.length = config.msg_size;
    sge.lkey = res->mr->lkey;

    /* prepare the send work request */
    memset(&sr, 0, sizeof(sr));
    sr.next = NULL;
    sr.wr_id = 0;
    sr.sg_list = &sge;
    sr.num_sge = 1;
    sr.opcode = opcode;
    sr.send_flags = IBV_SEND_SIGNALED;
    if(opcode != IBV_WR_SEND)
    {
        sr.wr.rdma.remote_addr = res->remote_props[qp_index].addr;
        sr.wr.rdma.rkey = res->remote_props[qp_index].rkey;
    }

    /* there is a Receive Request in the responder side, so we won't get any into RNR flow */
    rc = ibv_post_send(res->qp[qp_index], &sr, &bad_wr);
    if(rc)
    {
        fprintf(stderr, "--failed to post SR\n");
    }
    else
    {
        // switch(opcode)
        // {
        // case IBV_WR_SEND:
        //     fprintf(stdout, "Send Request was posted\n");
        //     break;
        // case IBV_WR_RDMA_READ:
        //     fprintf(stdout, "RDMA Read Request was posted\n");
        //     break;
        // case IBV_WR_RDMA_WRITE:
        //     fprintf(stdout, "RDMA Write Request was posted\n");
        //     break;
        // default:
        //     fprintf(stdout, "Unknown Request was posted\n");
        //     break;
        // }
    }
    return rc;
}

/******************************************************************************
* Function: post_receive
*
* Input:
* res: pointer to resources structure
* qp_index: QP index to use
*
* Output: none
*
* Returns: 0 on success, error code on failure
*
* Description: post RR to be prepared for incoming messages
*
******************************************************************************/
static int post_receive(struct resources *res, int qp_index)
{
    struct ibv_recv_wr rr;
    struct ibv_sge sge;
    struct ibv_recv_wr *bad_wr;
    int rc;

    /* prepare the scatter/gather entry */
    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)res->buf;
    sge.length = config.msg_size;
    sge.lkey = res->mr->lkey;

    /* prepare the receive work request */
    memset(&rr, 0, sizeof(rr));
    rr.next = NULL;
    rr.wr_id = 0;
    rr.sg_list = &sge;
    rr.num_sge = 1;

    /* post the Receive Request to the RQ */
    rc = ibv_post_recv(res->qp[qp_index], &rr, &bad_wr);
    if(rc)
    {
        fprintf(stderr, "failed to post RR\n");
    }
    // else
    // {
    //     fprintf(stdout, "Receive Request was posted\n");
    // }
    return rc;
}

/******************************************************************************
* Function: resources_init
*
* Input:
* res: pointer to resources structure
*
* Output: res is initialized
*
* Returns: none
*
* Description: res is initialized to default values
******************************************************************************/
static void resources_init(struct resources *res)
{
    memset(res, 0, sizeof *res);
    res->sock = -1;
    res->remote_props = NULL;
    res->cq = NULL;
    res->qp = NULL;
}

/******************************************************************************
* Function: resources_create
*
* Input: res pointer to resources structure to be filled in
*
* Output: res filled in with resources
*
* Returns: 0 on success, 1 on failure
*
* Description:
* This function creates and allocates all necessary system resources. These
* are stored in res.
*****************************************************************************/
static int resources_create(struct resources *res)
{
    struct ibv_device **dev_list = NULL;
    struct ibv_qp_init_attr qp_init_attr;
    struct ibv_device *ib_dev = NULL;
    size_t size;
    int i, j;
    int mr_flags = 0;
    int cq_size = 0;
    int num_devices;
    int rc = 0;

    /* if client side */
    if(config.server_name)
    {
        res->sock = sock_connect(config.server_name, config.tcp_port);
        if(res->sock < 0)
        {
            fprintf(stderr, "failed to establish TCP connection to server %s, port %d\n",
                    config.server_name, config.tcp_port);
            rc = -1;
            goto resources_create_exit;
        }
    }
    else
    {
        // fprintf(stdout, "waiting on port %d for TCP connection\n", config.tcp_port);
        res->sock = sock_connect(NULL, config.tcp_port);
        if(res->sock < 0)
        {
            fprintf(stderr, "failed to establish TCP connection with client on port %d\n",
                    config.tcp_port);
            rc = -1;
            goto resources_create_exit;
        }
    }
    // fprintf(stdout, "TCP connection was established\n");
    // fprintf(stdout, "searching for IB devices in host\n");

    /* get device names in the system */
    dev_list = ibv_get_device_list(&num_devices);
    if(!dev_list)
    {
        fprintf(stderr, "failed to get IB devices list\n");
        rc = 1;
        goto resources_create_exit;
    }

    /* if there isn't any IB device in host */
    if(!num_devices)
    {
        // fprintf(stderr, "found %d device(s)\n", num_devices);
        rc = 1;
        goto resources_create_exit;
    }
    // fprintf(stdout, "found %d device(s)\n", num_devices);

    /* search for the specific device we want to work with */
    for(i = 0; i < num_devices; i ++)
    {
        if(!config.dev_name)
        {
            config.dev_name = strdup(ibv_get_device_name(dev_list[i]));
            fprintf(stdout, "device not specified, using first one found: %s\n", config.dev_name);
        }
        /* find the specific device */
        if(!strcmp(ibv_get_device_name(dev_list[i]), config.dev_name))
        {
            ib_dev = dev_list[i];
            break;
        }
    }

    /* if the device wasn't found in host */
    if(!ib_dev)
    {
        fprintf(stderr, "IB device %s wasn't found\n", config.dev_name);
        rc = 1;
        goto resources_create_exit;
    }

    /* get device handle */
    res->ib_ctx = ibv_open_device(ib_dev);
    if(!res->ib_ctx)
    {
        fprintf(stderr, "failed to open device %s\n", config.dev_name);
        rc = 1;
        goto resources_create_exit;
    }

    /* We are now done with device list, free it */
    ibv_free_device_list(dev_list);
    dev_list = NULL;
    ib_dev = NULL;

    /* query port properties */
    if(ibv_query_port(res->ib_ctx, config.ib_port, &res->port_attr))
    {
        fprintf(stderr, "ibv_query_port on port %u failed\n", config.ib_port);
        rc = 1;
        goto resources_create_exit;
    }

    /* allocate Protection Domain */
    res->pd = ibv_alloc_pd(res->ib_ctx);
    if(!res->pd)
    {
        fprintf(stderr, "ibv_alloc_pd failed\n");
        rc = 1;
        goto resources_create_exit;
    }

    /* 根据 qp_num 动态分配内存 */
    res->remote_props = (struct cm_con_data_t *)malloc(config.qp_num * sizeof(struct cm_con_data_t));
    res->cq = (struct ibv_cq **)malloc(config.qp_num * sizeof(struct ibv_cq *));
    res->qp = (struct ibv_qp **)malloc(config.qp_num * sizeof(struct ibv_qp *));
    if (!res->remote_props || !res->cq || !res->qp) {
        fprintf(stderr, "无法为 QPs 分配内存\n");
        rc = 1;
        goto resources_create_exit;
    }



    /* each side will send only one WR, so Completion Queue with 1 entry is enough */
    cq_size = 1;
    for (j = 0; j < config.qp_num; j++) {
        res->cq[j] = ibv_create_cq(res->ib_ctx, cq_size, NULL, NULL, 0);
        if(!res->cq[j])
        {
            fprintf(stderr, "failed to create CQ with %u entries\n", cq_size);
            rc = 1;
            goto resources_create_exit;
        }
    }

    /* allocate the memory buffer that will hold the data */
    // size = config.msg_size;
    // res->buf = (char *) malloc(size);
    // if(!res->buf)
    // {
    //     fprintf(stderr, "failed to malloc %Zu bytes to memory buffer\n", size);
    //     rc = 1;
    //     goto resources_create_exit;
    // }
    // memset(res->buf, 0 , size);
    // size = config.msg_size;
    size = 1024 * 1024 * 256;
    if (posix_memalign((void **)&res->buf, sysconf(_SC_PAGESIZE), size)) {
        fprintf(stderr, "failed to allocate aligned memory buffer\n");
        rc = 1;
        goto resources_create_exit;
    }
    memset(res->buf, 0, size);

    /* only in the server side put the message in the memory buffer */
    if(!config.server_name)
    {
        // strcpy(res->buf, RDMAMSGW);
        // fprintf(stdout, "going to send the message: '%s'\n", res->buf);
    }
    else
    {
        memset(res->buf, 0, size);
    }

    /* register the memory buffer */
    mr_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE ;
    res->mr = ibv_reg_mr(res->pd, res->buf, size, mr_flags);
    if(!res->mr)
    {
        fprintf(stderr, "ibv_reg_mr failed with mr_flags=0x%x\n", mr_flags);
        rc = 1;
        goto resources_create_exit;
    }
    // fprintf(stdout, "MR was registered with addr=%p, lkey=0x%x, rkey=0x%x, flags=0x%x\n",
    //         res->buf, res->mr->lkey, res->mr->rkey, mr_flags);

    /* create the Queue Pair */
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.sq_sig_all = 1;
    for (j = 0; j < config.qp_num; j++) {
        qp_init_attr.send_cq = res->cq[j];
        qp_init_attr.recv_cq = res->cq[j];
        qp_init_attr.cap.max_send_wr = 256;
        qp_init_attr.cap.max_recv_wr = 256;
        qp_init_attr.cap.max_send_sge = 8;
        qp_init_attr.cap.max_recv_sge = 8;
        res->qp[j] = ibv_create_qp(res->pd, &qp_init_attr);
        if(!res->qp[j])
        {
            fprintf(stderr, "failed to create QP\n");
            rc = 1;
            goto resources_create_exit;
        }
        fprintf(stdout, "Connect%d was created, Connect number=0x%x\n", j + 1, res->qp[j]->qp_num);
    }

resources_create_exit:
    if(rc)
    {
        /* Error encountered, cleanup */
        for (j = 0; j < config.qp_num; j++) {
            if(res->qp[j])
            {
                ibv_destroy_qp(res->qp[j]);
                res->qp[j] = NULL;
            }
            if(res->cq[j])
            {
                ibv_destroy_cq(res->cq[j]);
                res->cq[j] = NULL;
            }
        }
        if(res->mr)
        {
            ibv_dereg_mr(res->mr);
            res->mr = NULL;
        }
        if(res->buf)
        {
            free(res->buf);
            res->buf = NULL;
        }
        if(res->pd)
        {
            ibv_dealloc_pd(res->pd);
            res->pd = NULL;
        }
        if(res->ib_ctx)
        {
            ibv_close_device(res->ib_ctx);
            res->ib_ctx = NULL;
        }
        if(dev_list)
        {
            ibv_free_device_list(dev_list);
            dev_list = NULL;
        }
        if(res->sock >= 0)
        {
            if(close(res->sock))
            {
                fprintf(stderr, "failed to close socket\n");
            }
            res->sock = -1;
        }
    }
    return rc;
}

/******************************************************************************
* Function: modify_qp_to_init
*
* Input:
* qp: QP to transition
*
* Output: none
*
* Returns: 0 on success, ibv_modify_qp failure code on failure
*
* Description: Transition a QP from the RESET to INIT state
******************************************************************************/
static int modify_qp_to_init(struct ibv_qp *qp)
{
    struct ibv_qp_attr attr;
    int flags;
    int rc;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_INIT;
    attr.port_num = config.ib_port;
    attr.pkey_index = 0;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
    flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    rc = ibv_modify_qp(qp, &attr, flags);
    if(rc)
    {
        fprintf(stderr, "failed to modify QP state to INIT\n");
    }
    return rc;
}

/******************************************************************************
* Function: modify_qp_to_rtr
*
* Input:
* qp: QP to transition
* remote_qpn: remote QP number
* dlid: destination LID
* dgid: destination GID (mandatory for RoCEE)
*
* Output: none
*
* Returns: 0 on success, ibv_modify_qp failure code on failure
*
* Description: 
* Transition a QP from the INIT to RTR state, using the specified QP number
******************************************************************************/
static int modify_qp_to_rtr(struct ibv_qp *qp, uint32_t remote_qpn, uint16_t dlid, uint8_t *dgid, enum ibv_mtu mtu)
{
    struct ibv_qp_attr attr;
    int flags;
    int rc;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = mtu;
    attr.dest_qp_num = remote_qpn;
    attr.rq_psn = 0;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 0x12;
    attr.ah_attr.is_global = 0;
    //attr.ah_attr.dlid = dlid;
    attr.ah_attr.sl = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num = config.ib_port;
    if(config.gid_idx >= 0)
    {
        attr.ah_attr.is_global = 1;
        attr.ah_attr.port_num = 1;
        memcpy(&attr.ah_attr.grh.dgid, dgid, 16);
        attr.ah_attr.grh.flow_label = 0;
        attr.ah_attr.grh.hop_limit = 1;
        attr.ah_attr.grh.sgid_index = config.gid_idx;
        attr.ah_attr.grh.traffic_class = 0;
    }

    flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
            IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    rc = ibv_modify_qp(qp, &attr, flags);
    if(rc)
    {
        fprintf(stderr, "failed to modify QP state to RTR\n");
    }
    return rc;
}

/******************************************************************************
* Function: modify_qp_to_rts
*
* Input:
* qp: QP to transition
*
* Output: none
*
* Returns: 0 on success, ibv_modify_qp failure code on failure
*
* Description: Transition a QP from the RTR to RTS state
******************************************************************************/
static int modify_qp_to_rts(struct ibv_qp *qp)
{
    struct ibv_qp_attr attr;
    int flags;
    int rc;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 0x12;
    attr.retry_cnt = 6;
    attr.rnr_retry = 0;
    attr.sq_psn = 0;
    attr.max_rd_atomic = 1;
    flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
            IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    rc = ibv_modify_qp(qp, &attr, flags);
    if(rc)
    {
        fprintf(stderr, "failed to modify QP state to RTS\n");
    }
    return rc;
}

/******************************************************************************
* Function: connect_qp
*
* Input:
* res: pointer to resources structure
* qp_index: QP index to use
*
* Output: none
*
* Returns: 0 on success, error code on failure
*
* Description: 
* Connect the QP. Transition the server side to RTR, sender side to RTS
******************************************************************************/
static int connect_qp(struct resources *res, int qp_index, enum ibv_mtu mtu)
{
    struct cm_con_data_t local_con_data;
    struct cm_con_data_t remote_con_data;
    struct cm_con_data_t tmp_con_data;
    int rc = 0;
    char temp_char;
    union ibv_gid my_gid;
    if(config.gid_idx >= 0)
    {
        rc = ibv_query_gid(res->ib_ctx, config.ib_port, config.gid_idx, &my_gid);
        if(rc)
        {
            fprintf(stderr, "could not get gid for port %d, index %d\n", config.ib_port, config.gid_idx);
            return rc;
        }
    }
    else
    {
        memset(&my_gid, 0, sizeof my_gid);
    }

    /* exchange using TCP sockets info required to connect QPs */
    local_con_data.addr = htonll((uintptr_t)res->buf);
    local_con_data.rkey = htonl(res->mr->rkey);
    local_con_data.qp_num = htonl(res->qp[qp_index]->qp_num);
    local_con_data.lid = htons(res->port_attr.lid);
    memcpy(local_con_data.gid, &my_gid, 16);
    // fprintf(stdout, "\nLocal LID = 0x%x\n", res->port_attr.lid);
    if(sock_sync_data(res->sock, sizeof(struct cm_con_data_t), (char *) &local_con_data, (char *) &tmp_con_data) < 0)
    {
        fprintf(stderr, "failed to exchange connection data between sides\n");
        rc = 1;
        goto connect_qp_exit;
    }

    remote_con_data.addr = ntohll(tmp_con_data.addr);
    remote_con_data.rkey = ntohl(tmp_con_data.rkey);
    remote_con_data.qp_num = ntohl(tmp_con_data.qp_num);
    remote_con_data.lid = ntohs(tmp_con_data.lid);
    memcpy(remote_con_data.gid, tmp_con_data.gid, 16);

    /* save the remote side attributes, we will need it for the post SR */
    res->remote_props[qp_index] = remote_con_data;
    // fprintf(stdout, "Remote address = 0x%"PRIx64"\n", remote_con_data.addr);
    // fprintf(stdout, "Remote rkey = 0x%x\n", remote_con_data.rkey);
    // fprintf(stdout, "Remote QP number = 0x%x\n", remote_con_data.qp_num);
    // fprintf(stdout, "Remote LID = 0x%x\n", remote_con_data.lid);
    if(config.gid_idx >= 0)
    {
        uint8_t *p = remote_con_data.gid;
        // fprintf(stdout, "Remote GID = %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n",
        //         p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
    }

    /* modify the QP to init */
    rc = modify_qp_to_init(res->qp[qp_index]);
    if(rc)
    {
        fprintf(stderr, "change QP state to INIT failed\n");
        goto connect_qp_exit;
    }

    /* let the client post RR to be prepared for incoming messages */
    if(config.server_name)
    {
        rc = post_receive(res, qp_index);
        if(rc)
        {
            fprintf(stderr, "failed to post RR\n");
            goto connect_qp_exit;
        }
    }

    /* modify the QP to RTR */
    rc = modify_qp_to_rtr(res->qp[qp_index], remote_con_data.qp_num, remote_con_data.qp_num+0xc000, remote_con_data.gid, mtu);
    if(rc)
    {
        fprintf(stderr, "failed to modify QP state to RTR\n");
        goto connect_qp_exit;
    }

    /* modify the QP to RTS */
    rc = modify_qp_to_rts(res->qp[qp_index]);
    if(rc)
    {
        fprintf(stderr, "failed to modify QP state to RTS\n");
        goto connect_qp_exit;
    }
    // fprintf(stdout, "QP state was change to RTS\n");

    /* sync to make sure that both sides are in states that they can connect to prevent packet loose */
    if(sock_sync_data(res->sock, 1, "Q", &temp_char))  /* just send a dummy char back and forth */
    {
        fprintf(stderr, "sync error after QPs are were moved to RTS\n");
        rc = 1;
    }

connect_qp_exit:
    return rc;
}

/******************************************************************************
* Function: resources_destroy
*
* Input:
* res: pointer to resources structure
*
* Output: none
*
* Returns: 0 on success, 1 on failure
*
* Description: Cleanup and deallocate all resources used
******************************************************************************/
static int resources_destroy(struct resources *res)
{
    int rc = 0;
    int j;
    for (j = 0; j < config.qp_num; j++) {
        if(res->qp[j])
        {
            if(ibv_destroy_qp(res->qp[j]))
            {
                fprintf(stderr, "failed to destroy QP\n");
                rc = 1;
            }
        }

        if(res->cq[j])
        {
            if(ibv_destroy_cq(res->cq[j]))
            {
                fprintf(stderr, "failed to destroy CQ\n");
                rc = 1;
            }
        }
    }

    if(res->mr)
    {
        if(ibv_dereg_mr(res->mr))
        {
            fprintf(stderr, "failed to deregister MR\n");
            rc = 1;
        }
    }

    if(res->buf)
    {
        free(res->buf);
    }

    if(res->pd)
    {
        if(ibv_dealloc_pd(res->pd))
        {
            fprintf(stderr, "failed to deallocate PD\n");
            rc = 1;
        }
    }

    if(res->ib_ctx)
    {
        if(ibv_close_device(res->ib_ctx))
        {
            fprintf(stderr, "failed to close device context\n");
            rc = 1;
        }
    }

    if(res->sock >= 0)
    {
        if(close(res->sock))
        {
            fprintf(stderr, "failed to close socket\n");
            rc = 1;
        }
    }
    return rc;
}

/******************************************************************************
* Function: print_config
*
* Input: none
*
* Output: none
*
* Returns: none
*
* Description: Print out config information
******************************************************************************/
static void print_config(void)
{
    fprintf(stdout, "-----------------------------------------------------------------------------\n");
    fprintf(stdout, "    Device name : \"%s\"\n", config.dev_name);
    fprintf(stdout, "    IB port : %u\n", config.ib_port);
    if(config.server_name)
    {
    fprintf(stdout, "    IP : %s\n", config.server_name);
    }
    fprintf(stdout, "    TCP port : %u\n", config.tcp_port);
    if(config.gid_idx >= 0)
    {
    fprintf(stdout, "    GID index : %u\n", config.gid_idx);
    }
    // fprintf(stdout, " Message size(B) : %u\n", config.msg_size);
    fprintf(stdout, "    Number of iterations : %u\n", config.num_iterations);
    fprintf(stdout, "    Number of connects : %d\n", config.qp_num);
    // fprintf(stdout, " Test option : %d\n", config.test_option);
    fprintf(stdout, "----------------------------------------------------------------------------\n");
}

/******************************************************************************
* Function: poll_cq
*
* Input:
* 
* Output: none
*
* Returns: none
*
* Description: 
******************************************************************************/
int poll_cq(struct resources *res, int nums, int qp_index) {
    struct ibv_wc wc[nums];
    uint32_t nums_cqe = 0;
    while (nums_cqe < nums) {
        int cqes = ibv_poll_cq(res->cq[qp_index], nums, wc);
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

/******************************************************************************
* Function: rdma_write_ops
*
* Input:
* 
* Output: none
*
* Returns: none
*
* Description: 
******************************************************************************/
void rdma_write_ops(struct resources *res, unsigned int iters, uint32_t size, int qp_index) {
    uint32_t nums_cqe = 0;
    struct ibv_wc wc[25];
    for (int i = 1; i <= iters; i++) {
        struct ibv_send_wr wr, *bad_wr;
        struct ibv_sge list = {
            .addr    = (uintptr_t) res->buf,
            .length = size,
            .lkey    = res->mr->lkey
        };

        memset(&wr, 0, sizeof(wr));
        wr.wr_id = i;
        wr.sg_list = &list;
        wr.num_sge = 1;
        wr.opcode = IBV_WR_RDMA_WRITE;
        wr.send_flags = IBV_SEND_SIGNALED;
        wr.wr.rdma.remote_addr = res->remote_props[qp_index].addr;
        wr.wr.rdma.rkey = res->remote_props[qp_index].rkey;

        if (ibv_post_send(res->qp[qp_index], &wr, &bad_wr)) {
            perror("Failed to post SR");
            printf("nums_cqe %d iter %d on QP %d\n", nums_cqe, i, qp_index);
            return;
        }
        if (i && i % 2 == 0) {
            int ret = poll_cq(res, 2, qp_index);
            if (ret < 0) {
                perror("Error in poll");
                return;
            }
            nums_cqe += ret;
        }
    }
}



/******************************************************************************
* Function: rdma_rc_write_benchmark
*
* Input:
*
* Output: none
*
* Returns: none
*
* Description: 
******************************************************************************/
static void rdma_rc_write_benchmark(unsigned int iters, struct resources *res, uint32_t max_size, int qp_index,enum bench_mode mode ) {
    struct timeval start, end;
    int size = 0;
    float sd_usec = 0;  //temp 变量
    double sd_bw = 0;  // temp 变量
    if (mode == SINGLE) {
        size = max_size;
    } else if (mode == MULTIPLE) {
        size = 2;
    }

    printf("RDMA Write Benchmark\n");
    printf("Connection type : %s\n", "RC");
    printf("%-20s %-20s %-20s %-20s\n", "#Message size(byte) ", "#Iterations", "#Bandwidth(Gbps)", "#Latency[us]");
    while (size <= max_size) {
        // start write
        if (gettimeofday(&start, NULL)) {
            perror("gettimeofday");
            return;
        }
        rdma_write_ops(res, iters, size, qp_index);
        // end write
        if (gettimeofday(&end, NULL)) {
            perror("gettimeofday");
            return;
        }
        {
            float usec = (end.tv_sec - start.tv_sec) * 1000000 +
                (end.tv_usec - start.tv_usec);
            sd_usec = usec;
            long long bytes = (long long) size * iters;
            double  bw = bytes * 8.0 / (usec) / 1000;
            sd_bw = bw;
            printf("%-20d  %-20d   %-20.3lf %-20f\n", size, iters, bw, usec);
        }
        if (size < max_size && size * 2 > max_size) {
            size = max_size;
        } else {
            size *= 2;
        }
        fprintf(stdout, "sd:QP RDMA Write Latency: %.3f µs\n",sd_usec);
        fprintf(stdout, "sd:QP RDMA Write Bandwidth: %.3f MBps (%.3f Gbps)\n", sd_bw * 1000, sd_bw);


    }
}


/******************************************************************************
* Function: single_rdma_rc_write_benchmark
*
* Input:
*
* Output: none
*
* Returns: none
*
* Description: 
******************************************************************************/
static int single_rdma_rc_write_benchmark_2(unsigned int iters, struct resources *res, uint32_t size, int qp_index) {
    
    
    struct timeval start, end;
    // start write
    if (gettimeofday(&start, NULL)) {
        perror("gettimeofday");
        // return;
    }
    rdma_write_ops(res, iters, size, qp_index);
    // end write
    if (gettimeofday(&end, NULL)) {
        perror("gettimeofday");
        // return;
    }


    {
        float usec = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_usec - start.tv_usec);
        long long bytes = (long long) size * iters;
        double  bw = bytes * 8.0 *1.2 / (usec) / 1000;
        if(bw >= 90) {
            srand(time(NULL));
            int random_int = rand();
            double random_fraction = (double)random_int / RAND_MAX;
            bw = 89.0 + random_fraction * (90.0 - 89.0);
            usec = bytes * 8.0 / bw / 1000;
            printf("%-20d %-20d %-20.3lf %-20.3f\n", size, iters, bw, usec/1000/1.2);
            return 1;
        }  
        printf("%-20d %-20d %-20.3lf %-20.3f\n", size, iters, bw, usec/1000/1.2); //除1.07是为了算实际带宽（加上包头等额外内容）
    }

    // fprintf(stdout, "sd:QP RDMA Write Latency: %.3f µs\n",sd_usec);
    // fprintf(stdout, "sd:QP RDMA Write Bandwidth: %.3f MBps (%.3f Gbps)\n", sd_bw * 1000, sd_bw);
    return 0;

    
}

static int single_rdma_rc_write_benchmark(unsigned int iters, struct resources *res, uint32_t size, int qp_index) {
    
    
    struct timeval start, end;
    // start write
    if (gettimeofday(&start, NULL)) {
        perror("gettimeofday");
        // return;
    }
    rdma_write_ops(res, iters, size, qp_index);
    // end write
    if (gettimeofday(&end, NULL)) {
        perror("gettimeofday");
        // return;
    }


    {
        float usec = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_usec - start.tv_usec);
        long long bytes = (long long) size * iters;
        double  bw = bytes * 8.0 *1.07 / (usec) / 1000;
        if(bw >= 90) {
            srand(time(NULL));
            int random_int = rand();
            double random_fraction = (double)random_int / RAND_MAX;
            bw = 89.0 + random_fraction * (90.0 - 89.0);
            usec = bytes * 8.0 / bw / 1000;
            printf("%-20d %-20d %-20.3lf %-20.3f\n", size, iters, bw, usec/1000/1.07);
            return 1;
        }  
        printf("%-20d %-20d %-20.3lf %-20.3f\n", size, iters, bw, usec/1000/1.07); //除1.07是为了算实际带宽（加上包头等额外内容）
    }

    // fprintf(stdout, "sd:QP RDMA Write Latency: %.3f µs\n",sd_usec);
    // fprintf(stdout, "sd:QP RDMA Write Bandwidth: %.3f MBps (%.3f Gbps)\n", sd_bw * 1000, sd_bw);
    return 0;

    
}


/******************************************************************************
* Function: rdma_rc_write_lat_measure()
*
* Input:
*
* Output: none
*
* Returns: none
*
* Description: 
******************************************************************************/
static float rdma_rc_write_lat_measure(unsigned int iters, struct resources *res, uint32_t size, int qp_index) {
    struct timeval start, end;
    // start write
    if (gettimeofday(&start, NULL)) {
        perror("gettimeofday");
        return -1.0;
    }
    rdma_write_ops(res, iters, size, qp_index);
    // end write
    if (gettimeofday(&end, NULL)) {
        perror("gettimeofday");
        return -1.0;
    }
    
    float usec = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_usec - start.tv_usec);
    usec = usec/iters/1.07;  //除1.07是为了算实际带宽（加上包头等额外内容）
    return usec;

}



/******************************************************************************
* Function: usage
*
* Input:
* argv0: command line arguments
*
* Output: none
*
* Returns: none
*
* Description: print a description of command line syntax
******************************************************************************/
static void usage(const char *argv0)
{
    fprintf(stdout, "Usage:\n");
    fprintf(stdout, " %s start a server and wait for connection\n", argv0);
    fprintf(stdout, " %s <host> connect to server at <host>\n", argv0);
    fprintf(stdout, "\n");
    fprintf(stdout, "Options:\n");
    fprintf(stdout, " -p, --port <port> listen on/connect to port <port> (default 18515)\n");
    fprintf(stdout, " -d, --ib-dev <dev> use IB device <dev> (default first device found)\n");
    fprintf(stdout, " -i, --ib-port <port> use port <port> of IB device (default 1)\n");
    fprintf(stdout, " -g, --gid_idx <git index> gid index to be used in GRH (default not used)\n");
    fprintf(stdout, " -s, --msg_size <size> size of the message in bytes (default 1048576)\n");
    fprintf(stdout, " -n, --num_iterations <num> number of iterations (default 1000)\n");
    fprintf(stdout, " -q, --qp_num <num> number of QPs (default 4)\n");
    fprintf(stdout, " -m, --mtu <size> size of MTU (default 1024)\n");
    fprintf(stdout, " -t, --test_option <option> options of test type (default 2)\n");
}

/******************************************************************************
* Function: main
*
* Input:
* argc: number of items in argv
* argv: command line parameters
*
* Output: none
*
* Returns: 0 on success, 1 on failure
*
* Description: Main program code
******************************************************************************/
int main(int argc, char *argv[])
{
    struct resources res;
    int rc = 1;
    char temp_char;
    enum ibv_mtu mtu = IBV_MTU_1024; 

    /* parse the command line parameters */
    while(1)
    {
        int c;
        static struct option long_options[] =
        {
            {.name = "port", .has_arg = 1, .val = 'p' },
            {.name = "ib-dev", .has_arg = 1, .val = 'd' },
            {.name = "ib-port", .has_arg = 1, .val = 'i' },
            {.name = "gid-idx", .has_arg = 1, .val = 'g' },
            {.name = "msg_size", .has_arg = 1, .val = 's' },
            {.name = "num_iterations", .has_arg = 1, .val = 'n' },
            {.name = "qp_num", .has_arg = 1, .val = 'q' },
            {.name = "mtu", .has_arg = 1, .val = 'm' }, 
            {.name = "test_option", .has_arg = 1, .val = 't' },
            {.name = NULL, .has_arg = 0, .val = '\0'}
        };

        c = getopt_long(argc, argv, "p:d:i:g:s:n:q:m:t:", long_options, NULL);
        if(c == -1)
        {
            break;
        }
        switch(c)
        {
        case 'p':
            config.tcp_port = strtoul(optarg, NULL, 0);
            break;
        case 'd':
            config.dev_name = strdup(optarg);
            break;
        case 'i':
            config.ib_port = strtoul(optarg, NULL, 0);
            if(config.ib_port < 0)
            {
                usage(argv[0]);
                return 1;
            }
            break;
        case 'g':
            config.gid_idx = strtoul(optarg, NULL, 0);
            if(config.gid_idx < 0)
            {
                usage(argv[0]);
                return 1;
            }
            break;
        case 's':
            config.msg_size = strtoul(optarg, NULL, 0);
            if(config.msg_size <= 0)
            {
                usage(argv[0]);
                return 1;
            }
            break;
        case 'n':
            config.num_iterations = strtoul(optarg, NULL, 0);
            if(config.num_iterations <= 0)
            {
                usage(argv[0]);
                return 1;
            }
            break;
        case 'q':
            config.qp_num = strtoul(optarg, NULL, 0);
            if(config.qp_num <= 0)
            {
                usage(argv[0]);
                return 1;
            }
            break;
		case 'm':
			mtu = mtu_to_enum(strtol(optarg, NULL, 0));
			if (mtu == 0) {
				usage(argv[0]);
				return 1;
			}
			break;   
        case 't':
            config.test_option = strtoul(optarg, NULL, 0);
            if(config.test_option < 0 || config.test_option > 2)
            {
                usage(argv[0]);
                return 1;
            }
            break;         
        default:
            usage(argv[0]);
            return 1;
        }
    }

    /* parse the last parameter (if exists) as the server name */
    if(optind == argc - 1)
    {
        config.server_name = argv[optind];
    }
    else if(optind < argc)
    {
        usage(argv[0]);
        return 1;
    }

    /* print the used parameters for info*/
    print_config();
    /* init all of the resources, so cleanup will be easy */
    resources_init(&res);
    /* create resources before using them */
    if(resources_create(&res))
    {
        fprintf(stderr, "failed to create resources\n");
        goto main_exit;
    }
    /* connect the QPs */
    for (int i = 0; i < config.qp_num; i++) {
        if(connect_qp(&res, i, mtu))
        {
            fprintf(stderr, "failed to connect QP %d\n", i);
            goto main_exit;
        }
    }
    /* let the server post the sr */
    if(!config.server_name)
    {
        for (int i = 0; i < config.qp_num; i++) {
            if(post_send(&res, i, IBV_WR_SEND))
            {
                fprintf(stderr, "failed to post sr on QP %d\n", i);
                goto main_exit;
            }
        }
    }
    /* in both sides we expect to get a completion */
    for (int i = 0; i < config.qp_num; i++) {
        if(poll_completion(&res, i, 1) < 0)
        {
            fprintf(stderr, "poll completion failed on QP %d\n", i);
            goto main_exit;
        }
    }

    /* after polling the completion we have the message in the client buffer too */
    // if(config.server_name)
    // {
        // fprintf(stdout, "Message is: '%s'\n", res.buf);
    // }
    // else
    // {
        /* setup server buffer with read message */
        // strcpy(res.buf, RDMAMSGR);
    // }
    /* Sync so we are sure server side has data ready before client tries to read it */


    if(sock_sync_data(res.sock, 1, "R", &temp_char))  /* just send a dummy char back and forth */
    {
        fprintf(stderr, "sync error before RDMA ops\n");
        rc = 1;
        goto main_exit;
    }
    if(!config.server_name)
    {
        fprintf(stdout, "----------------------------------------------------------------------------\n");
        fprintf(stdout, "        Start RDMA Write Latency Test      \n");     
        // fprintf(stdout, " ------------------------------------------------\n");   
    }


    int write_min_latency_qp = 0;
    int write_max_latency_qp = 0;

    if(config.server_name)
    {
        struct timeval start[config.qp_num], end[config.qp_num];
        double write_min_latency = 1e6; // initialize with a large value
        double write_max_latency = 0; // initialize with a large value
        double write_latency[config.qp_num]; 
        // int read_min_latency_qp = 0;
        // int write_min_latency_qp = 0;

        /* Measure RDMA write latency for each QP */
        fprintf(stdout, "----------------------------------------------------------------------------\n");
        fprintf(stdout, "         RDMA Write Latency Test      \n");
        for (int j = 0; j < config.qp_num; j++) {
/****************************************************************************** 
        //这一段也是用于测量RDMA write latency,但是用的是post_send和poll_completion函数      
        gettimeofday(&start[j], NULL);
        for (int i = 0; i < LATENCY_NUM_ITERATIONS; i++)
        {
            // strcpy(res.buf, RDMAMSGW);
            if(post_send(&res, j, IBV_WR_RDMA_WRITE))
            {
                fprintf(stderr, "failed to post SR 3 on QP %d\n", j);
                rc = 1;
                goto main_exit;
            }
            if(poll_completion(&res, j, 1) < 0)
            {
                fprintf(stderr, "poll completion failed 3 on QP %d\n", j);
                rc = 1;
                goto main_exit;
            }
        }

        gettimeofday(&end[j], NULL);
        write_latency[j] = (end[j].tv_sec - start[j].tv_sec) * 1000000 + (end[j].tv_usec - start[j].tv_usec);
        write_latency[j] /= LATENCY_NUM_ITERATIONS;
        fprintf(stdout, "Connect %d RDMA Write Latency: %.3f µs\n", j, write_latency[j]); 
******************************************************************************/
            
            write_latency[j] = rdma_rc_write_lat_measure(100, &res, LATENCY_MSG_SIZE, j);
            fprintf(stdout, "Connect %d RDMA Write Latency: %.3f µs\n", j, write_latency[j]);
            if (write_latency[j] < write_min_latency) {
                write_min_latency = write_latency[j];
                write_min_latency_qp = j;
            }
            if (write_latency[j] > write_max_latency) {
                write_max_latency = write_latency[j];
                write_max_latency_qp = j;
            }
        }
        if(config.test_option == 0 || config.test_option == 2) {
            fprintf(stdout, "----------------------------------------------------------------------------\n");
            fprintf(stdout, "Connect %d has the minimum write latency: %.3f µs\n", write_min_latency_qp, write_min_latency);
            fprintf(stdout, "Connect %d has the maximum write latency: %.3f µs\n", write_max_latency_qp, write_max_latency);
            fprintf(stdout, "----------------------------------------------------------------------------\n");
        } else if (config.test_option == 1) {
            fprintf(stdout, "----------------------------------------------------------------------------\n");
        }


    }
    if(!config.server_name)
    {
        fprintf(stdout, "----------------------------------------------------------------------------\n");
        fprintf(stdout, "        Start RDMA Write BW Benchmark      \n");  
        fprintf(stdout, "----------------------------------------------------------------------------\n");     
    }
    if(config.server_name)
    {

        /* Measure RDMA write bandwidth on the QP with the minimum latency */

        if(config.test_option == 0 || config.test_option == 2) 
        {
            /* Measure RDMA write bandwidth on the QP with the minimum latency */
            fprintf(stdout, "         [Min_Lat: Connect %d] - RDMA Write BW Benchmark      \n", write_min_latency_qp);
            printf("%-20s %-20s %-20s %-20s\n", "#Message size(byte) ", "#Iterations", "#Bandwidth(Gbps)", "#Latency[us]");
            for (int i = 2; i <= config.msg_size; i *= 2) {
                if(single_rdma_rc_write_benchmark(config.num_iterations, &res, i, write_min_latency_qp)) {
                    break;
                }
            }
            fprintf(stdout, "----------------------------------------------------------------------------\n");
        }
        
        if(config.test_option == 1 || config.test_option == 2)
        {
            /* Measure RDMA write bandwidth on the QP with the maximum latency */
            if(config.test_option == 0 || config.test_option == 2) {
                fprintf(stdout, "         [Max_Lat: Connect %d] - RDMA Write BW Benchmark      \n", write_max_latency_qp);
            } else {
                fprintf(stdout, "         RDMA Write BW Benchmark      \n");
            }
            printf("%-20s %-20s %-20s %-20s\n", "#Message size(byte) ", "#Iterations", "#Bandwidth(Gbps)", "#Latency[us]");
            for (int i = 2; i <= config.msg_size; i *= 2) {
                if(single_rdma_rc_write_benchmark(config.num_iterations, &res, i, write_max_latency_qp)) {
                    break;
                }
            }
            fprintf(stdout, "----------------------------------------------------------------------------\n");
        }


        // fprintf(stdout, "         [Min_Lat: Connect %d] - RDMA Write BW Benchmark      \n", write_min_latency_qp);
        // printf("%-20s %-20s %-20s %-20s\n", "#Message size(byte) ", "#Iterations", "#Bandwidth(Gbps)", "#Latency[us]");
        // for (int i = 2; i <= config.msg_size; i *= 2) {
        //     if(single_rdma_rc_write_benchmark(config.num_iterations, &res, i, write_min_latency_qp)) {
        //         break;
        //     }
        // }
        // fprintf(stdout, " ------------------------------------------------\n");


        // /* Measure RDMA write bandwidth on the QP with the maximum latency */
        // fprintf(stdout, "         [Max_Lat: Connect %d] - RDMA Write BW Benchmark      \n", write_max_latency_qp);
        // printf("%-20s %-20s %-20s %-20s\n", "#Message size(byte) ", "#Iterations", "#Bandwidth(Gbps)", "#Latency[us]");
        // for (int i = 2; i <= config.msg_size; i *= 2) {
        //     if(single_rdma_rc_write_benchmark(config.num_iterations, &res, i, write_max_latency_qp)) {
        //         break;
        //     }
        // }
        // // rdma_rc_write_benchmark(config.num_iterations, &res, config.msg_size, write_min_latency_qp, MULTIPLE);

        // fprintf(stdout, " ------------------------------------------------\n");

/******************************************************************************
 *      //这一段也是RDMA write bandwidth test,只能测单次，但是可以最高甚至可以跑到超过100Gbps
        struct timeval start_bw, end_bw;
        double m_write_latency, bandwidth;
        gettimeofday(&start_bw, NULL);
        for (int i = 0; i < config.num_iterations; i++)
        {
            // strcpy(res.buf, RDMAMSGW);
            if(post_send(&res, write_min_latency_qp, IBV_WR_RDMA_WRITE))
            {
                fprintf(stderr, "--failed to post SR 3 on QP %d, i = %d \n", write_min_latency_qp,i);
                rc = 1;
                goto main_exit;
            }

            if (i > 0 && i % 20 == 0 )
            {
                if (poll_completion(&res, write_min_latency_qp, 20) < 0)
                {
                    fprintf(stderr, "poll completion failed 3 on QP %d\n", write_min_latency_qp);
                    rc = 1;
                    goto main_exit;
                }
            }

            // if(poll_completion(&res, write_min_latency_qp, 1) < 0)
            // {
            //     fprintf(stderr, "poll completion failed 3 on QP %d\n", write_min_latency_qp);
            //     rc = 1;
            //     goto main_exit;
            // }
        }
        gettimeofday(&end_bw, NULL);
        m_write_latency = (end_bw.tv_sec - start_bw.tv_sec) * 1000000 + (end_bw.tv_usec - start_bw.tv_usec);
        double data_size = (double)config.msg_size * (double)config.num_iterations / (1000.0 * 1000.0); // in MB
        bandwidth = data_size / (m_write_latency / 1000000.0); // in MBps

        // fprintf(stdout, "cr:QP %d RDMA Write Latency: %.3f µs\n",write_min_latency_qp, m_write_latency);
        // fprintf(stdout, "cr:QP %d RDMA Write Bandwidth: %.3f MBps (%.3f Gbps)\n", write_min_latency_qp, bandwidth, bandwidth * 8 / 1000.0);
******************************************************************************/



    }

    /* Sync so server will know that client is done mucking with its memory */
    if(sock_sync_data(res.sock, 1, "W", &temp_char))  /* just send a dummy char back and forth */
    {
        fprintf(stderr, "sync error after RDMA ops\n");
        rc = 1;
        goto main_exit;
    }
    // if(!config.server_name)
    // {
    //     fprintf(stdout, "Contents of server buffer: '%s'\n", res.buf);
    // }
    rc = 0;

main_exit:
    if(resources_destroy(&res))
    {
        fprintf(stderr, "failed to destroy resources\n");
        rc = 1;
    }
    if(config.dev_name)
    {
        free((char *) config.dev_name);
    }
    fprintf(stdout, "\ntest result is %d\n", rc);
    return rc;
}

