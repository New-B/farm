// Copyright (c) 2018 The GAM Authors 

#ifndef FARM_H_
#define FARM_H_ 

#include "worker.h"
#include "worker_handle.h"
#include "farm_txn.h"
//Farm类实现了一个分布式系统中的事务管理器，提供了事务的开始、提交、中止、
//内存分配和释放、数据读写以及键值对存储和获取的功能
/*Farm类用于管理事务与工作节点的交互。
它提供了一些列方法来处理事务的开始、内存分配、读取、写入、提交和中止操作。主要用途：
1.事务管理：提供了开始、分配内存、释放内存、读取、写入、提交和中止事务的方法。
2.事务检查：提供了检查事务是否是本地事务的方法。
3.键值对存储和获取：提供了存储和获取键值对的方法，包括存储到指定节点和从指定节点获取键值对*/
class Farm {
    private:
        std::unique_ptr<WorkerHandle> wh_; //WorkerHandle的智能指针，用于管理Worker的句柄
        std::unique_ptr<TxnContext> rtx_; //TxnContext的智能指针，用于管理事务上下文，指向事务上下文对象
        TxnContext* tx_; //TxnContext的普通指针，用于指向当前事务上下文
        Worker* w_; //Worker的原始指针，用于当前Worker

    public:
        Farm(Worker*); //构造函数，接受一个Worker指针，用于初始化Farm对象
        int txBegin(); //开始事务
        GAddr txAlloc(size_t size, GAddr a = 0); //分配事务内存
        void txFree(GAddr); //释放事务内存
        osize_t txRead(GAddr, char*, osize_t); //事务读取
        osize_t txWrite(GAddr, const char*, osize_t);  //事务写入
        osize_t txPartialRead(GAddr, osize_t, char*, osize_t); //部份事务读取
        osize_t txPartialWrite(GAddr, osize_t, const char*, osize_t); //部份事务写入
        int txCommit(); //提交事务
        int txAbort();  //中止事务

        bool txnIsLocal() { //检查事务是否是本地事务
            std::vector<uint16_t> wid, rid; //定义两个向量用于存储写对象和读对象的Worker ID
            tx_->getWidForRobj(rid); //获取读对象的Worker ID
            tx_->getWidForWobj(wid); //获取写对象的Worker ID
            uint16_t wr = w_->GetWorkerId(); //获取当前Worker的ID
            bool ret = ((wid.size() == 0 || (wid.size() == 1 && wid[0] == wr))
                        && (rid.size() == 0 || (rid.size() == 1 && rid[0] == wr))); //检查事务是否是本地事务
            return ret; //返回结果
        }

        int put(uint64_t key, const void* value, size_t count) ; //存储键值对
        int get(uint64_t key, void* value) ; //获取键值对
        int kv_put(uint64_t key, const void* value, size_t count, int node_id) ; //存储键值对到指定节点
        int kv_get(uint64_t key, void* value, int node_id) ; //从指定节点获取键值对
};
#endif
