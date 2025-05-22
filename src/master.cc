// Copyright (c) 2018 The GAM Authors 

#include <thread>
#include <unistd.h>
#include <cstring>
#include "zmalloc.h"
#include "rdma.h"
#include "master.h"
#include "anet.h"
#include "log.h"
#include "ae.h"
#include "tcp.h"
#include "settings.h"
#include "structure.h"

Master* MasterFactory::server = nullptr;

Master::Master(const Conf& conf): st(nullptr), workers(), unsynced_workers() {  //master的创建过程

  this->conf = &conf;

  //get the RDMA resource
  resource = RdmaResourceFactory::getMasterRdmaResource();

  //create the event loop
  el = aeCreateEventLoop(conf.maxthreads+conf.maxclients+EVENTLOOP_FDSET_INCR);

  //open the socket for listening to the connections from workers to exch rdma resouces
  /*这段代码的作用是创建一个TCP服务器，并绑定到指定的地址和端口。如果绑定失败，则记录错误日志并退出程序，以下是代码的详细解释*/
  char neterr[ANET_ERR_LEN];  //定义一个用于存储错误信息的字符数组neterr,ANET_ERR_LEN是错误信息数组的长度
  //获取绑定地址，如果conf.master_bindaddr为空，则bind_addr为nullptr，否则为conf.master_bindaddr的C字符串形式
  /*检查配置中的master_bindaddr是否为空。
  如果master_bindaddr为空，则将bind_addr设置为nullptr。
  如果master_bindaddr不为空，则将其转换为C字符串，并将bind_addr设置为该字符串的指针。
  master_bindaddr是一个std::string对象，c_str()返回一个指向该字符串内容的常量字符指针(const char*),即C风格的字符串。
  const_cast是C++中的一个类型转换操作符，用于移除指针或引用的常量性，const_cast<char*>将const char*类型转换为char*类型。
  c_str()方法返回的是一个const char*，表示这个指针指向的内容是只读的，不能修改。
  在某些情况下，可能需要将这个只读的字符指针转换为可修改的字符指针（char*）,例如传递给需要char*类型参数的函数。
  使用const_cast可以移除常量性，但需要确保在转换后不会修改字符串内容，否则会导致未定义行为*/
  char* bind_addr = conf.master_ip.length() == 0 ? nullptr : const_cast<char*>(conf.master_ip.c_str());  
  //char* bind_addr = conf.master_bindaddr.length() == 0 ? nullptr : const_cast<char*>(conf.master_bindaddr.c_str());  
  //创建一个TCP服务器，绑定到指定的地址和端口，并设置监听队列的最大长度
  sockfd = anetTcpServer(neterr, conf.master_port, bind_addr, conf.backlog); //backlog=511
  //return _anetTcpServer(err, port, bindaddr, AF_INET, backlog);
  if (sockfd < 0) {//检查是否成功创建服务器
    //如果创建服务器失败，则记录错误日志并退出程序
    //epicLog(LOG_WARNING, "Opening port %s:%d (%s)", conf.master_bindaddr.c_str(), conf.master_port, neterr);
    epicLog(LOG_WARNING, "Opening port %s:%d (%s)", conf.master_ip.c_str(), conf.master_port, neterr);
    exit(1);
  }

  //register tcp event for rdma parameter exchange
  /*为TCP服务器套接字注册一个文件事件，以便在有新的客户端连接时进行处理，如果注册失败，则记录错误日志并退出程序。
  检查套接字描述符是否有效。
  调用aeCreateFileEvent函数为套接字描述符sockfd注册一个文件事件。
  el是事件循环的指针；
  sockfd是要注册事件的套接字描述符；
  AE_READABLE表示要注册的时间是可读事件，即当有新的客户端连接时触发事件；
  AcceptTcpClientHandle是事件处理函数，当事件触发时调用该函数处理新的客户端连接；
  this是传递给事件处理函数的用户数据，通常是当前对象的指针。
  如果aeCreateFileEvent返回AE_ERR，表示注册文件事件失败。*/
  if (sockfd > 0 && aeCreateFileEvent(el, sockfd,
        AE_READABLE, AcceptTcpClientHandle, this) == AE_ERR) {
    epicPanic("Unrecoverable error creating sockfd file event.");
  }

  //register rdma event
  /*这几行代码的作用是为RDMA通道文件描述符注册一个文件事件，以便在有新的RDMA请求时进行处理。
  如果注册文件事件失败，则记录致命错误并终止程序，
  检查RDMA通道文件描述符是否有效：获取RDMA通道的文件描述符，是否大于0，以确保文件描述符有效。
  调用aeCreateFileEvent函数为RDMA通道文件描述符注册一个文件事件。
  el是事件循环的指针。
  resource->GetChannelFd()是要注册事件的文件描述符。
  AE_READABLE表示要注册的时间类型是可读事件，即当有新的RDMA请求时触发事件。
  ProcessRdmaRequestHandle是事件处理函数，当事件触发时调用该函数处理新的RDMA请求。
  this是传递给事件处理函数的用户数据，通常是当前对象的指针。
  如果aeCreateFileEvent返回,AE_ERR，表示注册文件事件失败。
  epicPanic函数记录致命错误并终止程序。*/
  if (resource->GetChannelFd() > 0 && aeCreateFileEvent(el, 
        resource->GetChannelFd(), AE_READABLE, ProcessRdmaRequestHandle, this) == AE_ERR) {
    epicPanic("Unrecoverable error creating sockfd file event.");
  }
  //epicLog函数记录一条信息日志，表示开始主事件循环
  epicLog(LOG_INFO, "start master eventloop\n");
  //create the Master thread to start service
  /*this->st是一个指向std::thread对象的指针，表示当前对象的线程成员。
  new thread(startEventLoop, el)创建一个新的线程，并执行stratEventLoop函数。
  startEventLoop是线程函数，用于启动事件循环。
  el是传递给startEventLoop函数的参数，表示事件循环的指针。*/
  this->st = new thread(startEventLoop, el);
}

int Master::PostAcceptWorker(int fd, void* data) {
  if(worker_ips.length() == 0) {
    if(1 != write(fd, " ", 1)) {
      epicLog(LOG_WARNING, "Unable to send worker ip list\n");
      return -1;
    }
  } else {
    if(worker_ips.length() != write(fd, worker_ips.c_str(), worker_ips.length())) {
      epicLog(LOG_WARNING, "Unable to send worker ip list\n");
      return -1;
    }
    epicLog(LOG_DEBUG, "send: %s", worker_ips.c_str());

    worker_ips.append(",");
  }

  char msg[MAX_WORKERS_STRLEN+1];
  int n = read(fd, msg, MAX_WORKERS_STRLEN);
  if(n <= 0) {
    epicLog(LOG_WARNING, "Unable to receive worker ip:port\n");
    return -2;
  }
  worker_ips.append(msg, n);
  msg[n] = '\0';

  epicLog(LOG_DEBUG, "received: %s, now worker list is %s (len=%d)\n", 
      msg, worker_ips.c_str(), worker_ips.length());
  return 0;
}

Master::~Master() {
  aeDeleteEventLoop(el);
  delete st;
}


void Master::FarmProcessRemoteRequest(Client* client, const char* msg, uint32_t size) {
  WorkRequest* wr = new WorkRequest;
  int len;
  if (wr->Deser(msg, len)) {
    epicLog(LOG_WARNING, "de-serialize the work request failed\n");
  } else {
    epicLog(LOG_DEBUG, "Master receives a %s msg with size %d (%d) from client %d", 
        workToStr(wr->op), size, len, client->GetWorkerId());
    epicAssert(len == size);
    ProcessRequest(client, wr);
  }
}

void Master::ProcessRequest(Client* client, WorkRequest* wr) {
  switch(wr->op) {
    case UPDATE_MEM_STATS:
      {
        Size curr_free = client->GetFreeMem();
        client->SetMemStat(wr->size, wr->free);
        unsynced_workers.push(client);

        if(unsynced_workers.size() == conf->unsynced_th) {
          WorkRequest lwr{};
          lwr.op = BROADCAST_MEM_STATS;
          char buf[conf->unsynced_th*MAX_MEM_STATS_SIZE+1]; //op + list + \0
          char send_buf[MAX_REQUEST_SIZE];

          int n = 0;
          while(!unsynced_workers.empty()) {
            Client* lc = unsynced_workers.front();
            n += sprintf(buf+n, "%d:%ld:%ld", lc->GetWorkerId(), lc->GetTotalMem(), lc->GetFreeMem());
            unsynced_workers.pop();
          }
          lwr.size = conf->unsynced_th;
          lwr.ptr = buf;
          int len = 0;
          lwr.Ser(send_buf, len);
          Broadcast(send_buf, len);
        }
        delete wr;
        break;
      }
    case FETCH_MEM_STATS:
      {
        UpdateWidMap();
        if(widCliMap.size() == 1) { //only have the info of the worker, who sends the request
          break;
        }
        WorkRequest lwr{};
        lwr.op = FETCH_MEM_STATS_REPLY;
        char buf[(widCliMap.size()-1) * MAX_MEM_STATS_SIZE + 1]; //op + list + \0
        char* send_buf = client->GetFreeSlot();
        bool busy = false;
        if(send_buf == nullptr) {
          busy = true;
          send_buf = (char *)zmalloc(MAX_REQUEST_SIZE);
          epicLog(LOG_INFO, "We don't have enough slot buf, we use local buf instead");
        }

        int n = 0, i = 0;
        for (auto entry: widCliMap) {
          if(entry.first == client->GetWorkerId()) continue;
          Client* lc = entry.second;
          n += sprintf(buf + n, "%d:%ld:%ld:", lc->GetWorkerId(),
              lc->GetTotalMem(), lc->GetFreeMem());
          i++;
        }
        lwr.size = widCliMap.size()-1;
        epicAssert(widCliMap.size()-1 == i);
        lwr.ptr = buf;
        int len = 0, ret;
        lwr.Ser(send_buf, len);
        if((ret = client->Send(send_buf, len)) != len) {
          epicAssert(ret == -1);
          epicLog(LOG_INFO, "slots are busy");
        }
        epicAssert((busy && ret == -1) || !busy);
        delete wr;
        break;
      }
    case PUT:
      {
        void* ptr = zmalloc(wr->size);
        memcpy(ptr, wr->ptr, wr->size);
        kvs[wr->key] = pair<void*, Size>(ptr, wr->size);

        //epicLog(LOG_WARNING, "key = %d, value = %lx", wr->key, *(GAddr*)kvs[wr->key].first);

        // send reply back
        wr->op = PUT_REPLY;
        wr->status = SUCCESS;
        char* send_buf = client->GetFreeSlot();
        bool busy = false;
        if(send_buf == nullptr) {
          busy = true;
          send_buf = (char *)zmalloc(MAX_REQUEST_SIZE);
          epicLog(LOG_INFO, "We don't have enough slot buf, we use local buf instead");
        }

        int len = 0, ret;
        wr->Ser(send_buf, len);
        if((ret = client->Send(send_buf, len)) != len) {
          epicAssert(ret == -1);
          epicLog(LOG_INFO, "slots are busy");
        }

        if(to_serve_kv_request.count(wr->key)) {
          int size = to_serve_kv_request[wr->key].size();
          for(int i = 0; i < size; i++) {
            auto& to_serve = to_serve_kv_request[wr->key].front();
            epicAssert(to_serve.second->op == GET);
            epicLog(LOG_DEBUG, "processed to-serve remote kv request for key %ld", to_serve.second->key);
            to_serve.second->flag |= TO_SERVE;
            to_serve_kv_request[wr->key].pop();
            ProcessRequest(to_serve.first, to_serve.second);
          }
          epicAssert(to_serve_kv_request[wr->key].size() == 0);
          to_serve_kv_request.erase(wr->key);
        }
        delete wr;
        break;
      }
    case GET:
      {
        if(kvs.count(wr->key)) {
          wr->ptr = kvs.at(wr->key).first;
          wr->size = kvs.at(wr->key).second;
          wr->op = GET_REPLY;
          wr->status = SUCCESS;
          char* send_buf = client->GetFreeSlot();
          bool busy = false;
          if(send_buf == nullptr) {
            busy = true;
            send_buf = (char *)zmalloc(MAX_REQUEST_SIZE);
            epicLog(LOG_INFO, "We don't have enough slot buf, we use local buf instead");
          }

          int len = 0, ret;
          wr->Ser(send_buf, len);
          if((ret = client->Send(send_buf, len)) != len) {
            epicAssert(ret == -1);
            epicLog(LOG_INFO, "slots are busy");
          }
          epicAssert((busy && ret == -1) || !busy);
          delete wr;
        } else {
          to_serve_kv_request[wr->key].push(pair<Client*, WorkRequest*>(client, wr));
        }

        break;
      }
    default:
      epicLog(LOG_WARNING, "unrecognized work request %d", wr->op);
      break;
  }
}

void Master::Broadcast(const char* buf, size_t len) {
  for(auto entry: qpCliMap) {
    char* send_buf = entry.second->GetFreeSlot();
    bool busy = false;
    if(send_buf == nullptr) {
      busy = true;
      send_buf = (char *)zmalloc(MAX_REQUEST_SIZE);
      epicLog(LOG_INFO, "We don't have enough slot buf, we use local buf instead");
    }
    memcpy(send_buf, buf, len);

    size_t sent = entry.second->Send(send_buf, len);
    epicAssert((busy && sent == -1) || !busy);
    if(len != sent) {
      epicAssert(sent == -1);
      epicLog(LOG_INFO, "broadcast to %d failed (expected %d, but %d)\n", len, sent);
    }
  }
}
