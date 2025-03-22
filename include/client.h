// Copyright (c) 2018 The GAM Authors 
//文件定义了一个与客户端相关的类及其方法
#ifndef CLIENT_H
#define CLIENT_H

#include "rdma.h"
#include "structure.h"

class Server;   //前向声明，在定义类之前引用它

//TODO: consider to replace Client by RdmaContext
class Client{
    private:
//        union{
//            int fd;             /* unix socket (local clients) NOT USED */
            RdmaContext *ctx;   /* remote client */ //指向RDMA上下文的指针，用于远程客户端    
//        };

        int lastMsgTime;    //记录最后一条消息的时间
        RdmaResource* resource; //指向RDMA资源的指针
        char* connstr = nullptr;    //连接字符串

        //remote worker info
        /*
         * when connected to master, wid is its own wid
         * otherwise, it's the id of the remote pair
         */
        int wid;    //工作节点ID
        Size size;  //总内存大小
        Size free;  //空闲内存大小

    public:
        Client(RdmaResource* res, bool isForMaster, const char *rdmaConnStr = nullptr); //构造函数  //创建客户端
        //Client(int fd); /* for local clients */

        //used only among workers
        int ExchConnParam(const char* ip, int port, Server* server);    //交换连接参数
        const char* GetConnString(int workerid = 0);    //获取连接字符串
        int SetRemoteConnParam(const char *conn);   //设置远程连接参数

        inline bool IsForMaster() {   //判断是否为主节点
        	return ctx->IsMaster();
        }
        inline int GetWorkerId() {return wid;}  //获取工作节点ID
        inline void SetMemStat(int size, int free) {this->size = size; this->free = free;}  //设置内存状态
        inline Size GetFreeMem() {return this->free;}   //获取空闲内存大小
        inline Size GetTotalMem() {return this->size;}  //获取总内存大小
        inline void* ToLocal(GAddr addr) {return TO_LOCAL(addr, ctx->GetBase());}   //将全局地址转换为本地地址
        inline GAddr ToGlobal(void* ptr) {  //将本地地址转换为全局地址
        	if(ptr) {
        		return TO_GLOB(ptr, ctx->GetBase(), wid);
        	} else {
        		return EMPTY_GLOB(wid);
        	}
        }

        inline uint32_t GetQP() {return ctx->GetQP();}  //获取队列对号

        inline ssize_t Send(const void* buf, size_t len, unsigned int id = 0, bool signaled = false) {  //发送消息数据
        	return ctx->Send(buf, len, id, signaled);
        }
        inline ssize_t Write(raddr dest, raddr src, size_t len, unsigned int id = 0, bool signaled = false) {   //写消息数据    
        	return ctx->Write(dest, src, len, id, signaled);
        }
        inline ssize_t WriteWithImm(raddr dest, raddr src, size_t len, uint32_t imm, unsigned int id = 0, bool signaled = false) {  //带立即数的写消息数据？写数据并发送立即数据
        	return ctx->WriteWithImm(dest, src, len, imm, id, signaled);
        }

        inline int PostRecv(int n) {return ctx->PostRecv(n);}   //向共享接收队列发布接收请求

        inline char* GetFreeSlot() {return ctx->GetFreeSlot();} //获取空闲槽
        inline char* RecvComp(ibv_wc& wc) {return ctx->RecvComp(wc);}   //处理接收完成事件
        inline unsigned int SendComp(ibv_wc& wc) {return ctx->SendComp(wc);}    //处理发送完成事件
        inline unsigned int WriteComp(ibv_wc& wc) {return ctx->WriteComp(wc);}  //处理写完成事件

        ~Client();  //析构函数，释放资源
};
#endif
