// Copyright (c) 2018 The GAM Authors 

#ifndef INCLUDE_STRUCTURE_H_
#define INCLUDE_STRUCTURE_H_

#include <cstdlib>
#include <cstdint>
#include <cstddef>
#include <string>
#include "settings.h"
#include "log.h"

typedef size_t Size;
typedef unsigned char byte;

#define DEFAULT_SPLIT_CHAR ':'	//默认分隔符

#define ALLOCATOR_ALREADY_EXIST_EXCEPTION 1	//分配器已存在异常
#define ALLOCATOR_NOT_EXIST_EXECEPTION 2	//分配器不存在异常

//struct GAddr {
////	void *laddr; //local address
////	int rid; //region id
////	Size size;
//	uint64_t rid:16; //currently rid = wid
//	uint64_t off:48; //the offset to the local base addr
//};

typedef uint64_t ptr_t ;	//定义ptr_t为uint64_t类型
//全局地址GAddr被定义为一个64位的无符号整数(uint64_t)，其中高16位用于存储节点标识符(wid)，低48位用于存储偏移量(off)。
//这样设计的目的是为了在全局地址中同时包含节点标识符和偏移量，以便在分布式系统中方便地定位数据。
//基地址：基地址通常是指某个内存区域的起始地址，在分布式系统重，每个节点可能有一个或多个内存区域，每个区域都有一个基地址。基地址用于计算相对于该区域的偏移量。
//本地地址：是指在当前节点上的实际内存地址，
typedef uint64_t Key;	//定义Key为uint64_t类型
typedef uint64_t GAddr;	//定义GAddr为uint64_t类型
#define OFF_MASK 0xFFFFFFFFFFFFL	//定义OFF_MASK为0xFFFFFFFFFFFFL	位移掩码，48个1，L后缀表示该常量是long类型。使用L后缀可以确保常量的类型和大小符合预期，特别是在未操作和掩码操作中
#define WID(gaddr) ((gaddr) >> 48)	//获取全局地址的工作节点ID
#define OFF(gaddr) ((gaddr) & OFF_MASK)	//获取全局地址的偏移量
#define TO_GLOB(addr, base, wid) ((ptr_t)(addr) - (ptr_t)(base) + ((ptr_t)(wid) << 48))	//将本地地址转换为全局地址
#define EMPTY_GLOB(wid) ((ptr_t)(wid) << 48)	//创建空全局地址

#define GADD(addr, off) ((addr)+(off)) //for now, we don't have any check for address overflow	//全局地址相加上偏移
#define GMINUS(a, b) ((a)-(b)) //need to guarantee WID(a) == WID(b)	//两个全局地址相减
#define TOBLOCK(x) (((ptr_t)x) & BLOCK_MASK)	//将地址转换为块地址
#define BLOCK_ALIGNED(x) (!((x) & ~BLOCK_MASK))	//判断地址是否为块对齐
#define BADD(addr, i) TOBLOCK((addr) + (i)*BLOCK_SIZE) //return an addr	//块地址加上偏移
#define BMINUS(i, j) (((i)-(j))>>BLOCK_POWER)	//两个块地址相减
#define TO_LOCAL(gaddr, base)  (void*)(OFF(gaddr) + (ptr_t)(base))	//将全局地址转换为本地地址
#define Gnullptr 0	//全局空指针

struct Conf {
	int no_node = 1;	//number of nodes	//工作节点数
	bool is_master = true; //mark whether current process is the master (obtained from conf and the current ip)	//标记当前进程是否为主节点
	int master_port = 12345;	//主节点端口
	std::string master_ip = "localhost";	//主节点IP地址
	//std::string master_bindaddr;	//主节点绑定地址
	int worker_port = 12346;	//工作节点端口
	//std::string worker_bindaddr;	//工作节点绑定地址
	std::string worker_ip = "localhost";	//工作节点IP地址
	Size size = 1024*1024L*512; //per-server size of memory pre-allocated	//每个服务器预分配的内存大小
	Size ghost_th = 1024*1024;	//幽灵阈值
	double cache_th = 0.15; //if free mem is below this threshold, we start to allocate memory from remote nodes	//缓存阈值，如果空闲内存低于此阈值，我们将开始从远程节点分配内存
	int unsynced_th = 1;//未同步阈值	
	double factor = 1.25;	//增长因子
	int maxclients = 1024;	//最大客户端数
	int no_thread = 1;	//实际线程数
	int maxthreads = 10;	//最大线程数
	int backlog = TCP_BACKLOG;	//TCP监听队列长度 连接的等待队列长度
	int loglevel = LOG_DEBUG;	//日志级别
	std::string* logfile = nullptr;	//日志文件
	int timeout = 10; //ms	//超时时间（毫秒）
};

typedef int PostProcessFunc(int, void*);

#endif /* INCLUDE_STRUCTURE_H_ */
