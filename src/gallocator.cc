// Copyright (c) 2018 The GAM Authors 

#include <stdio.h>
#include "gallocator.h"
#include "workrequest.h"
#include "zmalloc.h"
#include <cstring>

const Conf* GAllocFactory::conf = nullptr; //定义并初始化GAllocFactory类的静态成员变量conf
Worker* GAllocFactory::worker; //定义GAllocFactory类的静态成员变量worker的指针
Master* GAllocFactory::master; //定义并初始化GAllocFactory类的静态成员变量master
mutex GAllocFactory::lock; //定义GAllocFactory类的静态成员变量lock

GAddr GAlloc::Malloc(const Size size, Flag flag){ //定义GAlloc类的Malloc成员函数
	GAddr addr = 0; //初始化地址为0
    while (1){ //无限循环
        this->txBegin(); //开始事务
	    addr = this->txAlloc(size); //分配内存
 	    int ret = this->txCommit(); //提交事务
        if (ret == 0) break; //如果提交成功，跳出循环
    }
    return addr; //返回分配的地址
}

GAddr GAlloc::AlignedMalloc(const Size size, Flag flag){ //定义GAlloc类的AlignedMalloc成员函数
    return this->Malloc(size, flag); //调用Malloc函数
}

int GAlloc::Read(const GAddr addr, void* buf, const Size count, Flag flag){ //定义GAlloc类的read成员函数
	while (1){//无限循环
        this->txBegin(); //开始事务
	    this->txRead(addr, reinterpret_cast<char*>(buf), count); //读取数据
	    int ret = this->txCommit(); //提交事务
        if (ret == 0) break; //如果提交成功，跳出循环
    }
    return 0; //返回0表示成功
}

int GAlloc::Read(const GAddr addr, const Size offset, void* buf, const Size count, Flag flag){
	while (1){ //无限循环
        this->txBegin(); //开始事务
	    this->farm->txPartialRead(addr, offset, reinterpret_cast<char*>(buf), count); //部份读取数据
	    int ret = this->txCommit(); //提交事务
        if (ret == 0) break;//如果提交成功。跳出循环
    }
    return 0; //返回0表示成功
}
int GAlloc::Write(const GAddr addr, void* buf, const Size count, Flag flag){ //定义GAlloc类的Write成员函数
	while (1){ //无限循环
        this->txBegin(); //开始事务
	    this->txWrite(addr, reinterpret_cast<char*>(buf), count); //写入数据
	    int ret = this->txCommit(); //提交事务
        if (ret == 0) break; //如果提交成功，跳出循环
    }
    return 0; //返回0表示成功
}
int GAlloc::Write(const GAddr addr, const Size offset, void* buf, const Size count, Flag flag){
	while (1){
        this->txBegin();
	    this->farm->txPartialWrite(addr, offset, reinterpret_cast<char*>(buf), count); //部份写入数据
	    int ret = this->txCommit();
        if (ret == 0) break;
    }
    return 0;
}
void GAlloc::Free(const GAddr addr){ //定义GAlloc类的Free成员函数
	while (1){ //无限循环
        this->txBegin(); //开始事务
	    this->txFree(addr); //释放内存
	    int ret = this->txCommit(); //提交事务
        if (ret == 0) break; //如果提交成功，跳出循环
    }
}

void GAlloc:: txBegin(){ //定义GAlloc类的txBegin成员函数
	farm->txBegin(); //调用farm的txbegin函数
}
GAddr GAlloc::txAlloc(size_t size, GAddr a){ //定义GAlloc类的txAlloc成员函数
	return farm->txAlloc(size, a); //调用farm的txFree函数
}
void GAlloc::txFree(GAddr addr){ // 定义 GAlloc 类的 txFree 成员函数
	farm->txFree(addr); // 调用 farm 的 txFree 函数
}
int GAlloc::txRead(GAddr addr, void* ptr, osize_t sz){ // 定义 GAlloc 类的 txRead 成员函数
	return farm->txRead(addr, reinterpret_cast<char*>(ptr), sz); // 调用 farm 的 txRead 函数
}
int GAlloc::txRead(GAddr addr, const Size offset, void* ptr, osize_t sz){// 定义 GAlloc 类的 txRead 成员函数
    return farm->txPartialRead(addr, offset, reinterpret_cast<char*>(ptr), sz); //调用farm的txPartialRead函数
}
/*在C++中，void*是一种通用指针类型，可以指向任何类型的数据。因此，void*可以接收来自任何类型的指针，包括char*。
reinterpret_cast是zC++中的一种类型转换运算符，用于在不同类型的指针之间进行转换。
在这个函数中，reinterpret_cast<char*>将void*类型的指针转换为char*类型的指针。
这种转换是必要的，因为farm->txWrite函数的第二个参数需要char*类型。*/
int GAlloc::txWrite(GAddr addr, void* ptr, osize_t sz){ //定义GAlloc类的txWrite成员函数
	return farm->txWrite(addr, reinterpret_cast<char*>(ptr), sz); //调用farm的txWrite函数
}
int GAlloc::txWrite(GAddr addr, const Size offset, void* ptr, osize_t sz){//定义GAlloc类的txWrite成员函数
    return farm->txPartialWrite(addr, offset, reinterpret_cast<char*>(ptr), sz);// 调用 farm 的 txPartialWrite 函数
}
int GAlloc::txAbort(){ //定义GAlloc类的txAbort成员函数
    return farm->txAbort(); //调用farm的txAbort函数
}
int GAlloc::txCommit(){ //定义Galloc类的txCommit成员函数
	return farm->txCommit(); //调用farm的txCommit函数
}

Size GAlloc::Put(uint64_t key, const void* value, Size count) { //定义GAlloc类的Put成员函数
    return farm->put(key, value, count); //调用farm的Put函数
}

Size GAlloc::Get(uint64_t key, void* value) { //定义GAlloc类的Get成员函数
    return farm->get(key, value); //调用farm的get函数
}

int GAlloc::txKVGet(uint64_t key, void* value, int node_id){// 定义 GAlloc 类的 txKVGet 成员函数
	return farm->kv_get(key, value, node_id) < 0;// 调用 farm 的 kv_get 函数
}
int GAlloc::txKVPut(uint64_t key, const void* value, size_t count, int node_id){// 定义 GAlloc 类的 txKVPut 成员函数
	return farm->kv_put(key, value, count, node_id) < 0;// 调用 farm 的 kv_put 函数
}

int GAlloc::KVGet(uint64_t key, void *value, int node_id){// 定义 GAlloc 类的 KVGet 成员函数
	return farm->kv_get(key, value, node_id) < 0;// 调用 farm 的 kv_get 函数
}
int GAlloc::KVPut(uint64_t key, const void* value, size_t count, int node_id){// 定义 GAlloc 类的 KVPut 成员函数
	return farm->kv_put(key, value, count, node_id) < 0;// 调用 farm 的 kv_put 函数
}

