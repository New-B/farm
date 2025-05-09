// Copyright (c) 2018 The GAM Authors 
/*文件定义了与RDMA相关的结构和常量*/

/*这些宏定义用于防止头文件被多次包含*/
#ifndef RDMA_H_
#define RDMA_H_

/*这些头文件提供了标准库和第三方库的功能，例如容器、数据类型、输入输出操作以及InfiniBand verbs库*/
#include <map>
#include <unordered_map>
#include <cstdint>
#include <vector>
#include <cstdio>
#include <infiniband/verbs.h>
#include <queue>
#include "settings.h"
#include "log.h"

/*这些前向声明用于在定义类之前引用它们*/
class RdmaResource;
class RdmaContext;
/*定义了一个类型raddr，表示注册的地址*/
typedef void* raddr; //raddr means registered addr

/*这些宏定义了与RDMA连接字符串长度、位掩码、接收槽步长和其他计算相关的常量和宏*/
#define MASTER_RDMA_CONN_STRLEN 22 /** 4 bytes for lid + 8 bytes qpn
                                    * + 8 bytes for psn
                                    * seperated by 2 colons */


#define WORKER_RDMA_CONN_STRLEN (MASTER_RDMA_CONN_STRLEN + 8 + 16 + 2)

#define MAX_CONN_STRLEN (WORKER_RDMA_CONN_STRLEN+4+1) //4: four-digital wid, 1: ':', 1: \0

#define HALF_BITS 0xffffffff
#define QUARTER_BITS 0xffff

#define RECV_SLOT_STEP 100
#define BPOS(s) (s/RECV_SLOT_STEP)
#define BOFF(s) ((s%RECV_SLOT_STEP)*MAX_REQUEST_SIZE)
#define RMINUS(a, b, rz) ((a) >= (b) ? (a) - (b) : (a) + (rz) - (b))

/*结构体包含了RDMA连接的相关信息，如缓冲区地址、QP号、包序列号、远程秘钥和IB端口的LID*/
struct RdmaConnection {          /* information of IB conn */
    uint64_t        vaddr;      /* buffer address */
    uint32_t        qpn;        /* QP number */
    uint32_t        psn;        /* packet sequence number */
    uint32_t        rkey;       /* remote key */
    uint32_t        lid;        /* LID of the IB port */
};

/*这个类包含了RDMA资源的成员变量和方法，用于管理RDMA资源*/
class RdmaResource{
        ibv_device *device;        /* 指向RDMA设备的指针 */
        ibv_context *context;      /*RDMA设备的上下文*/
        ibv_comp_channel *channel;  /*完成事件通道*/
        ibv_pd *pd;                 /*保护域*/
        ibv_cq *cq;                     /* global comp queue全局完成队列 */
        ibv_srq *srq;                   /* share receive queue 共享接收队列*/
        const char *devName = NULL;   /*RDMA设备的名称*/
        ibv_port_attr portAttribute;    /*IB端口属性*/
        int ibport = 1;                  /* TODO: dual-port support IB端口号，默认值为1*/
        uint32_t psn;            /* packet sequence number 包序列号*/
        bool isForMaster;    /*是否用于Master节点*/
        friend class RdmaContext; /*RdmaContext是RdmaResource的友元类*/

        //the follow three variables are only used for comm among workers
        void* base; //the base addr for the local memory   /*本地内存的基地址*/
        size_t size;    //the size of the local memory  /*本地内存的大小*/
        struct ibv_mr *bmr;     //the memory region for the local memory  /*本地内存的内存区域*/

        //node-wide communication buf used for receive request
        std::vector<struct ibv_mr*> comm_buf;   //the memory region for the comm buf /*通信缓冲区的内存区域，用于接收请求的通信缓冲区*/
        //size_t buf_size; buf_size = slots.size() * MAX_REQUEST_SIZE
        int slot_head; //current slot head     /*当前槽头*/
        int slot_inuse; //number of slots in use    /* 正在使用的槽的数量 */
        /*
         * TODO: check whether head + tail is enough
         */
        std::vector<bool> slots; //the states of all the allocated slots (true: occupied, false: free) /* 所有分配槽的状态，true占用，false空闲 */
        //current created RdmaContext
        int rdma_context_counter; /*当前创建的RdmaContext计数器*/

    public:
        /*
         * isForMaster: true -- used to communicate with/by Master (no need to reg whole mem space)
         * 				false -- used to communicate among workers (reg whole mem space + comm buf)
         */
        RdmaResource(ibv_device *device, bool isForMaster); //构造函数，初始化RDMA资源
        ~RdmaResource(); //析构函数，释放资源
        inline const char *GetDevname() const {return this->devName;}  //获取设备名称
        inline bool IsMaster() const {return isForMaster;} //判断是否为主节点
        //int regHtableMemory(const void *htable, size_t size);
        
        inline ibv_cq* GetCompQueue() const {return cq;}    //获取完成队列
        inline int GetChannelFd() const {return channel->fd;}   //获取完成事件通道的文件描述符
        bool GetCompEvent() const; //获取完成事件
        int RegLocalMemory(void *base, size_t sz);  //注册本地内存

        int RegCommSlot(int); //注册通信槽
        char* GetSlot(int s); //get the starting addr of the slot   //获取槽的起始地址
        int PostRecv(int n); //post n RR to the srq //向共享接收队列发布接收请求
        //int ClearRecv(int low, int high);
        inline void ClearSlot(int s) {slots.at(s) = false;} //清除槽状态
        
        RdmaContext* NewRdmaContext(bool isForMaster); //创建新的RdmaContext
        void DeleteRdmaContext(RdmaContext* ctx); //删除RdmaContext
        inline int GetCounter() {return rdma_context_counter;} //获取RdmaContext计数器

};
/*该结构体包含了RDMA请求相关的信息*/
struct RdmaRequest {
	ibv_wr_opcode op;
	const void* src;
	size_t len;
	unsigned int id;
	bool signaled;
	void* dest;
	uint32_t imm;
	uint64_t oldval;
	uint64_t newval;
};
/*这个类包含了RDMA上下文的成员变量和方法，用于管理RDMA操作*/
class RdmaContext{
private:
	ibv_qp *qp; //队列对    
	ibv_mr* send_buf; //send buf //发送缓冲区
	int slot_head; //槽头
	int slot_tail; //槽尾
	bool full; //to differentiate between all free and all occupied slot_head == slot_tail  标识槽是否已满

	uint64_t vaddr = 0; /* for remote rdma read/write */ //远程RDMA读写的地址
	uint32_t rkey = 0; //远程密钥

	RdmaResource *resource; //指向RdmaResource资源的指针
	bool isForMaster;   //是否用于Master节点
	int max_pending_msg;    //最大待处理消息数
	int max_unsignaled_msg; //最大未标记消息数
	int pending_msg = 0; //including both RDMA send and write/read that don't use the send buf  包括RDMA发送和不使用发送缓冲区的写/读   //待处理消息数
	int pending_send_msg = 0; //including only send msg 只包括发送消息  //待处理发送消息数
	int to_signaled_send_msg; //in order to proceed the slot_tail   //为了处理槽尾而标记的发送消息数    //标记发送消息数
	int to_signaled_w_r_msg;    //in order to proceed the slot_tail   //为了处理槽尾而标记的写/读消息数    //标记写/读消息数
	struct ibv_sge sge_list;    //scatter gather element list  //分散收集元素列表
	struct ibv_send_wr wr;  //send work request  //发送工作请求

	queue<RdmaRequest> pending_requests;    //待处理请求队列

    char *msg;  //the buffer for receiving msg  //用于接收消息的缓冲区

	ssize_t Rdma(ibv_wr_opcode op, const void* src, size_t len,  unsigned int id = 0, bool signaled =
			false, void* dest = nullptr, uint32_t imm = 0, uint64_t oldval = 0,
			uint64_t newval = 0);

	inline ssize_t Rdma(RdmaRequest& r) {
		epicLog(LOG_DEBUG, "process pending rdma request");
		return Rdma(r.op, r.src, r.len, r.id, r.signaled, r.dest, r.imm, r.oldval, r.newval);
	}
public:
	RdmaContext(RdmaResource *, bool isForMaster); //构造函数，初始化RDMA上下文
	inline bool IsMaster() {    //判断是否为主节点
		return isForMaster;
	}
	const char* GetRdmaConnString();    //获取RDMA连接字符串
	int SetRemoteConnParam(const char *remConn);    //设置远程连接参数

	inline uint32_t GetQP() {   //获取队列对号
		return qp->qp_num;
	}
	inline void* GetBase() {return (void*)vaddr;}   //获取基地址

	unsigned int SendComp(ibv_wc& wc);  //处理发送完成事件
	unsigned int WriteComp(ibv_wc& wc); //处理写完成事件
	char* RecvComp(ibv_wc& wc); //处理接收完成事件
    char* GetFreeSlot();    //获取空闲槽
    bool IsRegistered(const void* addr);    //判断地址是否已注册

	ssize_t Send(const void* ptr, size_t len, unsigned int id = 0, bool signaled = false);  //发送消息数据
	inline int PostRecv(int n) {    //发布接收请求
		return resource->PostRecv(n);
	}
	int Recv();     //接收消息

	void ProcessPendingRequests(int n); //处理待处理请求

	/*
	 * @dest: dest addr at remote node
	 * @src: src addr at local node
	 */
	ssize_t Write(raddr dest, raddr src, size_t len, unsigned int id = 0, bool signaled = false);   //写消息数据
	ssize_t WriteWithImm(raddr dest, raddr src, size_t len, uint32_t imm, unsigned int id = 0, bool signaled = false);  //带立即数的写消息数据 写数据并立即发送？
	/*
	 * @dest: dest addr at local node
	 * @src: src addr at remote node
	 */
	ssize_t Read(raddr dest, raddr src, size_t len, unsigned int id = 0, bool signaled = false);    //读消息数据

	ssize_t Cas(raddr src, uint64_t oldval, uint64_t newval, unsigned int id = 0, bool signaled = false);   //执行比较并交换操作

	~RdmaContext(); //析构函数，释放资源
};
/*这个类用于创建和管理RdmaResource对象*/
class RdmaResourceFactory {
    /* TODO: thread safety */
    private:
        static std::vector<RdmaResource *> resources;   /*静态向量，保存所有RdmaResource资源的向量*/
        static const char *defaultDevname;  /*默认设备名称*/
        static const char *workerDevname;  /*默认工作节点名称*/
        static RdmaResource* GetRdmaResource(bool isServer, const char *devName);   //获取RdmaResource资源对象
    public:
        inline static RdmaResource* getMasterRdmaResource(const char *devName = NULL) {   //获取主节点RdmaResource资源对象
            return GetRdmaResource(true, devName);
        }
        inline static RdmaResource* getWorkerRdmaResource(const char *devName = NULL) {  //获取工作节点RdmaResource资源对象
            return GetRdmaResource(false, devName);
        }

        ~RdmaResourceFactory() {    //析构函数，释放资源    //释放所有RdmaResource资源
            for (std::vector<RdmaResource *>::iterator i = resources.begin(); i != resources.end(); ++i)   {
                delete (*i);
            } 
        }
};
#endif
