# #!/bin/bash

# # 客户端参数
# GID_IDX=3
# DEVICE=mlx5_1
# MTU=1024
# MSG_SIZE=20485760
# QP_NUM=8
# SERVER_IP=13.13.13.2

# # 运行客户端程序
# ./build/bin/RDMA_RC_minLat_a -g $GID_IDX -d $DEVICE -m $MTU -s $MSG_SIZE -q $QP_NUM $SERVER_IP


#!/bin/bash

# 设置默认参数
DEFAULT_GID_IDX=3
DEFAULT_DEVICE="mlx5_1"
DEFAULT_MTU=1024
DEFAULT_MSG_SIZE=2097152
DEFAULT_QP_NUM=8
DEFAULT_SERVER_IP="14.14.14.2"
DEFAULT_TEST_OPTION=2

# 如果传入了参数则使用传入的参数，否则使用默认参数
MTU=${1:-$DEFAULT_MTU}
GID_IDX=${2:-$DEFAULT_GID_IDX}
DEVICE=${3:-$DEFAULT_DEVICE}
MSG_SIZE=${4:-$DEFAULT_MSG_SIZE}
QP_NUM=${5:-$DEFAULT_QP_NUM}
TEST_OPTION=${6:-$DEFAULT_TEST_OPTION}
SERVER_IP=${7:-$DEFAULT_SERVER_IP}


# # 输出使用的参数
# echo "Using GID_IDX: $GID_IDX"
# echo "Using DEVICE: $DEVICE"
# echo "Using MTU: $MTU"
# echo "Using MSG_SIZE: $MSG_SIZE"
# echo "Using QP_NUM: $QP_NUM"
# echo "Using TEST_OPTION : $TEST_OPTION"
# echo "Using SERVER_IP: $SERVER_IP"


# 运行客户端程序
./build/bin/RDMA_RC_minLat_a -m $MTU -g $GID_IDX -d $DEVICE  -s $MSG_SIZE -q $QP_NUM  -t $TEST_OPTION $SERVER_IP

