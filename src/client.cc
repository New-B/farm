// Copyright (c) 2018 The GAM Authors 

#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <cerrno>
#include "rdma.h"
#include "anet.h"
#include "log.h"
#include "client.h"
#include "server.h"
#include "zmalloc.h"

/* 将resource用在Client对象的初始化中，主要考虑：
共享RDMA资源：
  RDMA通信需要在客户端和服务器之间建立连接；客户端需要访问服务器的RDMA资源(如上线、队列对等)来完成RDMA操作。
  因此，resource被传递给Client，以便客户端能够共享服务器的RDMA资源。
创建RDMA上下文：
  Client对象需要一个RDMA上下文(ctx)来进行RDMA操作，客户端通过resource调用NewRdmaContext方法创建一个新的RDMA上下文。
  RDMA上下文是RDMA通信的核心，它封装了RDMA连接的状态和配置。每个客户端需要一个独立的RDMA上下文来管理与远程服务器的RDMA通信。
统一管理RDMA资源：
  通过将resource传递给Client，可以确保RDMA资源的管理是统一的。例如，RDMA上下文的创建和销毁都由resource负责，
  避免了资源泄漏或重复分配的问题。

**ctx是Client类的私有变量，类型为RdmaContext*，表示客户端的RDMA上下文，设计意图是：
每个客户端需要独立的RDMA上下文：
  RDMA通信是基于队列对(QP)的，每个队列对对应一个RDMA上下文。每个客户端需要一个独立的RDMA上下文来管理与远程服务器的通信状态。
  通过ctx，客户端可以：设置远程连接参数；获取本地连接参数；执行RDMA操作(读、写、发送、接收)
动态创建和销毁RDMA上下文：
  在Client的构造函数中，ctx是通过res->NewRdmaContext动态创建的。在client的析构函数中，ctx会被销毁(resource->DeleteRdmaContext(ctx))。
  这种设计确保了RDMA上下文的生命周期与Client对象的生命周期一致，避免了资源浪费。
支持多种通信场景：
  RDMA通信可能发生在以下场景：工作节点和主节点之间；工作节点之间。ctx的初始化通过isForMaster参数区分不同的通信场景：
  - 如果isForMaster为true，表示当前客户端是连接到主节点的工作节点。
  - 如果isForMaster为false，表示当前客户端是连接到其他工作节点的工作节点。
提供连接参数管理：
  ctx提供了管理连接参数的方法，例如：SetRemoteConnParam用于设置远程连接参数；GetRdmaConnString用于获取本地连接字符串。

*/
Client::Client(RdmaResource* res, bool isForMaster, const char* rdmaConnStr): lastMsgTime(0), resource(res) {
  wid = free = size = 0;
  this->ctx = res->NewRdmaContext(isForMaster);
  if(rdmaConnStr) this->SetRemoteConnParam(rdmaConnStr);
}
/*函数的主要功能是与远程服务器交换连接参数。它通过TCP连接到远程服务器，发送本地连接参数，并接收远程服务器的连接参数*/
int Client::ExchConnParam(const char* ip, int port, Server* server) {
  //open the socket to exch rdma resouces 打开用于交换RDMA资源的套接字
  char neterr[ANET_ERR_LEN];
  int sockfd = anetTcpConnect(neterr, const_cast<char *>(ip), port); //连接到指定的IP地址和端口，如果连接失败，记录错误日志并退出程序
  if (sockfd < 0) {
    epicLog(LOG_WARNING, "Connecting to %s:%d %s", ip, port, neterr);
    exit(1);
  }
  //获取本地连接字符串
  const char* conn_str = GetConnString(server->GetWorkerId());
  int conn_len = strlen(conn_str); //获取连接字符串的长度
  //发送本地连接参数到远程服务器
  if (write(sockfd, conn_str, conn_len) != conn_len) { //使用write函数将本地连接参数发送到远程服务器，如果失败返回-1
    return -1;
  }
  //等待服务器的响应
  char msg[conn_len+1];
  /* waiting for server's response */
  int n = read(sockfd, msg, conn_len);//使用read函数读取服务器的响应，存储在msg数组中。
  if (n != conn_len) { //如果读取的字节数不等于连接字符串的长度，记录错误日志并返回-1
    epicLog(LOG_WARNING, "Failed to read conn param from server (%s; read %d bytes)\n", strerror(errno), n);
    return -1;
  }
  msg[n] = '\0';
  epicLog(LOG_INFO, "received conn string %s\n", msg);
  //设置远程连接参数
  SetRemoteConnParam(msg);
  //如果是连接到主服务器，执行特定操作
  if(IsForMaster()) server->PostConnectMaster(sockfd, server);
  //关闭套接字
  close(sockfd);
  return 0;
}

int Client::SetRemoteConnParam(const char *conn) {
  const char* p = conn;
  if(resource->IsMaster()) { //in the Master thread, connected to worker
    wid = resource->GetCounter();
  } else if(IsForMaster()) { //in the worker thread, but connected to Master
    sscanf(conn, "%x:", &wid); //使用sscanf从连接字符串conn中解析出工作节点ID，并将其存储到我i的中。
  } else if(!resource->IsMaster()) { //in the worker thread, and connected to worker
    sscanf(conn, "%x:", &wid);
  } else {
    epicLog(LOG_WARNING, "undefined cases");
  }
  p = strchr(conn, ':'); //查找连接字符串中第一个':'字符的位置，返回值是一个指向冒号的指针，并将其赋值给p
  p++; //跳过worker ID部分,定为到RDMA连接字符串的起始位置。
  return ctx->SetRemoteConnParam(p);//ctx是当前客户端的RDMA上下文对象，将解析出RDMA连接字符串p传递给RDMA上下文对象的SetRemoteConnParam方法
}

const char* Client::GetConnString(int workerid) {
  const char* rdmaConn = ctx->GetRdmaConnString();
  if(!connstr) connstr = (char*)zmalloc(MAX_CONN_STRLEN+1);

  if(resource->IsMaster()) { //in the Master thread
    sprintf(connstr, "%04x:%s", wid, rdmaConn); //wid is already set
    epicLog(LOG_DEBUG, "master to worker here");
  } else if(IsForMaster()) { //in the worker thread, but connected to Master
    sprintf(connstr, "%04x:%s", 0, rdmaConn);
    epicLog(LOG_DEBUG, "worker to master here");
  } else if(!resource->IsMaster()) { //in the worker thread, and connected to worker
    epicAssert(workerid != 0);
    sprintf(connstr, "%04x:%s", workerid, rdmaConn); //wid is the current worker id (not the remote pair's)
  } else {
    epicLog(LOG_WARNING, "undefined cases");
  }
  epicLog(LOG_DEBUG, "conn str %s\n", connstr);

  /* FIXME: rdmaConn does not get freed? rdmaConn is not null-terminated
   * string; will printingt it using %s format cause problems? */
  return connstr;
}

Client::~Client() {
  resource->DeleteRdmaContext(ctx);
}
