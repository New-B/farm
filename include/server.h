// Copyright (c) 2018 The GAM Authors 
//文件定义了一个与服务器相关的类及其方法
#ifndef SERVER_H_
#define SERVER_H_ 

#include <unordered_map>

#include "client.h"
#include "settings.h"
#include "rdma.h"
#include "workrequest.h"
#include "structure.h"
#include "ae.h"

//Server类是一个抽象类，定义了服务器的基本属性和方法，此处，在类定义前引用它们
class ServerFactory;
class Server;
class Client;

class Server{
  private:
    unordered_map<uint32_t, Client*> qpCliMap; /* rdma clients */ //map from qpn to region存储RDMA客户端的映射  /*从qpn到区域的映射*/   
    unordered_map<int, Client*> widCliMap; //map from worker id to region //从worker id到区域的映射   从worker ID到客户端的映射
    unordered_map<int, std::string> workerRdmaParams; //worker RDMA参数映射  存储worker的RDMA参数映射  //从worker ID到RDMA参数的映射
    RdmaResource* resource; //RDMA资源 指向RDMA资源的指针
    aeEventLoop* el;  //event loop 事件循环
    int sockfd; //socket fd  socket文件描述符
    const Conf* conf; //配置指针  指向配置的指针  
    //这些类可以访问Server类的私有成员
    friend class ServerFactory;
    friend class Master;
    friend class Worker;
    friend class Cache;

  public:
    Client* NewClient(bool isMaster, const char* rdmaConn = nullptr); //创建新客户端的重载方法
    Client* NewClient(const char*);
    Client* NewClient();

    std::string GetWorkerRdmaParam(int workerId); //获取工作节点的RDMA参数  根据worker ID获取RDMA参数

    virtual bool IsMaster() = 0;  //判断是否为主节点  //纯虚函数
    virtual int GetWorkerId() = 0;  //获取工作节点ID  //纯虚函数

    void RmClient(Client *);  //删除客户端

    Client* FindClient(uint32_t qpn); //查找客户端  根据QP号查找客户端
    void UpdateWidMap();  //更新WidMap 更新worker ID的映射
    Client* FindClientWid(int wid); //查找客户端  根据worker ID查找客户端

    void ProcessRdmaRequest();  //处理RDMA请求
    virtual int PostAcceptWorker(int, void*) {return 0;}  //接受工作者连接的虚函数
    virtual int PostConnectMaster(int, void*) {return 0;} //连接主节点的虚函数
    virtual void ProcessRequest(Client* client, WorkRequest* wr) = 0; //处理请求的纯虚函数
    virtual void FarmProcessRemoteRequest(Client* client, const char* msg, uint32_t size) = 0;  //处理远程请求的纯虚函数
    virtual void FarmResumeTxn(Client*) = 0;  //恢复事务的纯虚函数
    virtual void ProcessRequest(Client* client, unsigned int id) {};  //处理请求的虚函数
    virtual void CompletionCheck(unsigned int id) {}; //完成检查的虚函数

    inline const string& GetIP() {return conf->worker_ip;}  //获取IP地址
    inline int GetPort() {return conf->worker_port;}  //获取端口号

    virtual ~Server() {aeDeleteEventLoop(el);}; //析构函数,删除事件循环
};
#endif
