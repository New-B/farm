#!/usr/bin/env bash

# 配置选项
no_node=2
node1="172.16.33.32"
node2="172.16.33.35"
obj_size=1024
no_thread=4

# 可执行程序路径
exec="/sharenvme/usershome/wangbo/projects/farm/test/test_cluster"

# 节点列表
nodes=($node1 $node2)

# 遍历每个节点并启动程序
for (( i = 0; i < no_node; i++ )); do
    node_id=$i
    worker=${nodes[$i]}
    is_master=$((i == 0 ? 1 : 0)) # 第一个节点为master，其余为worker
    log="/sharenvme/usershome/wangbo/projects/farm/log/farm-$worker.log"

    # 构建命令
    cmd="$exec --ip_master $node1 --ip_worker $worker --node_id $node_id --is_master $is_master --no_node $no_node --no_thread $no_thread --obj_size $obj_size"

    # 在远程节点上启动程序
    ssh $worker "$cmd 1>$log 2>$log &"

    # 如果是master节点，稍作延迟
    if [ $is_master -eq 1 ]; then
        sleep 1
    fi
done