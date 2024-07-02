#!/bin/bash
set -e

# 定义变量
SERVER_USER="yaqiyun"
CLIENT1_ADDRESS="14.14.14.1"
SERVER1_ADDRESS="14.14.14.2"
CLIENT2_ADDRESS="14.14.14.5"
SERVER2_ADDRESS="14.14.14.6"

CLIENT_SHELL_PATH="/home/yaqiyun/cr/rdma_benchmark/run_client.sh"
SERVER_SHELL_PATH="/home/yaqiyun/cr/rdma_benchmark/run_server.sh"

mesg_size=2097152
qp_num=8
mtu=1024
gid_idx=3
device=mlx5_1
test_option=0

# 定义清理函数
cleanup() {
    echo "Cleaning up..."
    ssh ${SERVER_USER}@${CLIENT2_ADDRESS} "pkill -f 'ib_write_bw'"
    ssh ${SERVER_USER}@${SERVER2_ADDRESS} "pkill -f 'ib_write_bw'"
    echo "Cleanup completed."
}

# 设置trap，捕捉EXIT信号
trap cleanup EXIT


# 进度条函数
show_progress() {
    local current_step=$1
    local total_steps=100
    local progress_bar_width=50
    local terminal_width=78
    local progress=$(echo "$current_step * $progress_bar_width / $total_steps" | bc)
    local remaining=$(echo "$progress_bar_width - $progress" | bc)
    local progress_bar="["
    for ((j=0; j<progress; j++)); do
        progress_bar="${progress_bar}#"
    done
    for ((j=0; j<remaining; j++)); do
        progress_bar="${progress_bar}-"
    done
    progress_bar="${progress_bar}]"
    local percentage="$current_step%"
    local padding=$(( (terminal_width - progress_bar_width - ${#percentage} - 2) / 2 ))
    printf "\r%*s%s %s" $padding "" "$progress_bar" "$percentage"
    if [ $current_step -eq $total_steps ]; then
        echo ""
    fi
}









echo "=============================================================================="
echo "                           Interference flow starting                         "
# echo "=============================================================================="

# 初始化进度
progress=0
show_progress $progress

# 在SERVER2_ADDRESS上执行ib_write_bw命令
ssh ${SERVER_USER}@${SERVER2_ADDRESS} "nohup ib_write_bw -c UC -d mlx5_0 -p 18515 -D 100 -s 1000000 > server_output.log 2>&1 &"

if [ $? -ne 0 ]; then
    echo "Failed to start ib_write_bw on ${SERVER2_ADDRESS}"
    exit 1
fi


# 更新进度条到30%
progress=30
show_progress $progress

# 等待一段时间以确保前面的命令启动完成
sleep 2

# 在CLIENT2_ADDRESS上执行ib_write_bw命令
ssh ${SERVER_USER}@${CLIENT2_ADDRESS} "nohup ib_write_bw -c UC -d mlx5_0 -p 18515 -D 100 -s 1000000 ${SERVER2_ADDRESS} > client_output.log 2>&1 &"

if [ $? -ne 0 ]; then
    echo "Failed to start ib_write_bw on ${CLIENT2_ADDRESS}"
    exit 1
fi
# 更新进度条到60%
progress=60
show_progress $progress

# 等待一段时间以确保前面的命令启动完成
sleep 2

# 更新进度条到100%
progress=100
show_progress $progress
sleep 1


# echo "                  Interference flow started successfully                      "
echo "=============================================================================="

sleep 1

echo "                           RDMA benchmark starting                             "
echo "=============================================================================="



# 在SERVER1_ADDRESS上执行run_server.sh
ssh ${SERVER_USER}@${SERVER1_ADDRESS} "nohup ${SERVER_SHELL_PATH} ${mtu} ${gid_idx} ${device} ${mesg_size} ${qp_num} ${test_option} > server_output.log 2>&1 &"

if [ $? -ne 0 ]; then
    echo "Failed to execute server script on ${SERVER1_ADDRESS}"
    exit 1
fi

# 等待一段时间以确保前面的命令启动完成
sleep 3

# 在CLIENT1_ADDRESS(本地）上执行run_client.sh
${CLIENT_SHELL_PATH} ${mtu} ${gid_idx} ${device} ${mesg_size}  ${qp_num} ${test_option} ${SERVER1_ADDRESS}

if [ $? -ne 0 ]; then
    echo "Failed to execute client script on ${CLIENT1_ADDRESS}"
    exit 1
fi

# 成功完成后，退出脚本
exit 0
