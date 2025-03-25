#!/usr/bin/env bash

master="172.16.33.32"
no_node=2
no_thread=4
write_ratio=50
num_obj=100000
obj_size=100
iteration=10000
txn_nobj=40

exec="/sharenvme/usershome/wangbo/projects/farm/test/farm_cluster_test --ip_master $master --no_node $no_node --no_thread $no_thread --write_ratio $write_ratio --num_obj $num_obj --obj_size $obj_size --iteration $iteration --txn_nobj $txn_nobj"

for (( i = 0; i < no_node; i++)); do
    #worker="172.16.33.$((i + 30))"

    let node_id=i
    if [ $i -eq 0 ]; then
        worker="172.16.33.$((i + 32))"
        is_master=1
    else
        worker="172.16.33.$((i + 34))"
        is_master=0
    fi
    log="/sharenvme/usershome/wangbo/projects/farm/log/farm-$worker"".log"
    
    cmd="$exec --ip_worker $worker --node_id $node_id --is_master $is_master"

    ssh $worker "$cmd 1>$log 2>$log &"

    if [ $i -eq 0 ]; then
        sleep 1;
    fi
done
