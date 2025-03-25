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
    sscanf(conn, "%x:", &wid);
  } else if(!resource->IsMaster()) { //in the worker thread, and connected to worker
    sscanf(conn, "%x:", &wid);
  } else {
    epicLog(LOG_WARNING, "undefined cases");
  }
  p = strchr(conn, ':');
  p++;
  return ctx->SetRemoteConnParam(p);
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
