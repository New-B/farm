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

// int Master::PostAcceptWorker(int fd, void* data) {
//   char msg[MAX_WORKERS_STRLEN+1];
//   int n = read(fd, msg, MAX_WORKERS_STRLEN);
//   if(n <= 0) {
//     epicLog(LOG_WARNING, "Unable to receive worker ip:port\n");
//     return -2;
//   }
//   worker_ips.append(msg, n);
//   workers++;
//   epicLog(LOG_INFO, "worker %d connected, now worker list is %s (len=%d)\n", 
//       workers, worker_ips.c_str(), worker_ips.length());
  
//   if (workers == conf->no_node) {
//     BroadcastWorkerList();
//   } else{
//     worker_ips.append(",");
//   }
  
//   return 0;
// }

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

// void Master::BroadcastWorkerList() {
//   std::string allWorkerData = "WorkerList:";
//   allWorkerData.append(worker_ips); //worker_ips is a string containing all worker IPs and ports 已包含所有工作节点的IP:Port信息
//   allWorkerData += ";RdmaParams:";

//   // Append RDMA parameters for each worker 序列化所有工作节点的RDMA连接参数
//   for (const auto& pair : workerRdmaParams) {
//     allWorkerData += std::to_string(pair.first) + ":" + pair.second + ",";
//   }
//   if (!allWorkerData.empty() && allWorkerData.back() == ',') {
//     allWorkerData.pop_back(); // 移除最后一个逗号
//   }

//   // Broadcast the worker list and RDMA parameters to all clients
//   for (const auto& pair : qpCliMap) {
//     Client* client = pair.second;
//     client->Send(allWorkerData.c_str(), allWorkerData.length());
//   }
//   epicLog(LOG_INFO, "Broadcasted worker list and RDMA parameters to all clients: %s", allWorkerData.c_str());
//   //WorkerList:192.168.1.1:12345,192.168.1.2:12345;RDMAParams:1:rdma_param_1,2:rdma_param_2
// }

Master::~Master() {
  aeDeleteEventLoop(el);
  delete st;
}

/* 功能：解析来自远程客户端的请求消息-将消息内容反序列化为工作请求对象-调用对应的处理函数处理请求
 * 参数：client：发送请求的客户端对象；msg：接收到的消息内容；size：消息大小
 * 数据结构和关键变量：WorkRequest：工作请求对象，包括操作类型、数据地址、大小等信息
 * 该函数是主节点和远程客户端通信的核心函数，确保请求的正确解析和处理
 * */
void Master::FarmProcessRemoteRequest(Client* client, const char* msg, uint32_t size) {
  WorkRequest* wr = new WorkRequest; //创建一个新的工作请求对象wr
  int len; //定义一个变量len，用于存储反序列化后的消息长度
  if (wr->Deser(msg, len)) { //调用WorkRequest::Deser方法，将消息内容反序列化为工作请求对象
    epicLog(LOG_WARNING, "de-serialize the work request failed\n"); //如果返序列化失败，记录警告日志
  } else { //如果返序列化成功，记录接收到的消息的详细信息，包括操作类型、消息大小和发送方的工作节点id
    epicLog(LOG_DEBUG, "Master receives a %s msg with size %d (%d) from client %d", 
        workToStr(wr->op), size, len, client->GetWorkerId());
    epicAssert(len == size); //检查返反列化后的消息长度是否与接收到的消息大小一致
    ProcessRequest(client, wr); //调用请求处理函数
  }
}
/* 功能：根据操作类型处理请求；更新客户端状态或存储键值对；构造回复消息并发送给客户端 
 * 参数：client：发送请求的客户端对象；wr：工作请求对象，包含操作类型、数据地址、大小等信息
 * 动态管理资源：使用挂起队列to_serve_kv_request动态管理未完成的键值存储请求。使用缓冲区槽优化消息发送
 *
 * */
void Master::ProcessRequest(Client* client, WorkRequest* wr) {
  switch(wr->op) { //根据工作请求的操作类型(op)执行不同的处理逻辑 
    case UPDATE_MEM_STATS: //处理更新内存统计信息的请求-更新客户端的内存统计信息-如果未同步的工作节点数达到阈值，广播内存统计信息给所有客户端  
    /* 数据结构和关键变量：unsynced_workers-未同步的工作节点队列，存储所有未同步的工作节点
     * conf->unsynced_th：未同步的工作节点的阈值；buf：存储广播消息内容的缓冲区；send_buf：存储序列化后的消息内容的缓冲区
     * 
     */
      {
        Size curr_free = client->GetFreeMem(); //获取客户端当前的空闲内存大小
        client->SetMemStat(wr->size, wr->free); //更新客户端的内存统计信息，包括总内存大小和空闲内存大小
        unsynced_workers.push(client); //将客户端添加到未同步的工作节点队列unsynced_workers中 

        if(unsynced_workers.size() == conf->unsynced_th) { //如果未同步的工作节点数达到阈值conf->unsynced_th，广播内存信息给所有客户端
          //构造广播消息
          WorkRequest lwr{}; //创建一个新的工作请求对象lwr
          lwr.op = BROADCAST_MEM_STATS; //设置操作类型为BROADCAST_MEM_STATS 
          char buf[conf->unsynced_th*MAX_MEM_STATS_SIZE+1]; //op + list + \0  //定义缓冲区buf用于存储广播消息内容
          char send_buf[MAX_REQUEST_SIZE]; //定义发送缓冲区send_buf用于序列化消息
          //遍历未同步的工作节点
          int n = 0;
          while(!unsynced_workers.empty()) { //遍历未同步的工作节点队列unsynced_workers
            Client* lc = unsynced_workers.front(); //获取队列头部的工作节点
            n += sprintf(buf+n, "%d:%ld:%ld", lc->GetWorkerId(), lc->GetTotalMem(), lc->GetFreeMem()); //使用sprintf将工作节点的ID、总内存和空闲内存格式化为字符串并追加到缓冲区buf中
            unsynced_workers.pop(); //从队列中移除已处理的工作节点
          }
          //设置广播消息的内容
          lwr.size = conf->unsynced_th; //设置工作请求对象lwr的大小为未同步的工作节点数conf->unsynced_th
          lwr.ptr = buf; //设置工作请求对象lwr的指针指向消息内容缓冲区buf
          int len = 0; //定义一个变量len，用于存储序列化后的消息长度
          lwr.Ser(send_buf, len); //调用WorkRequest::Ser方法，将工作请求对象lwr序列化到发送缓冲区send_buf中，并记录序列化后的长度
          Broadcast(send_buf, len); //使用Broadcast函数将内存统计信息广播给所有客户端 
          // 插入日志信息，记录广播成功
          epicLog(LOG_INFO, "Successfully broadcasted memory stats to all clients.");
        } //删除工作请求对象
        // 插入日志信息，记录更新内存统计信息成功
        epicLog(LOG_INFO, "Successfully updated memory stats for client %d.", client->GetWorkerId());
        delete wr; //删除当前处理的工作请求wr，释放内存
        break; //退出case UPDATE_MEM_STATS的处理逻辑
      }
    case FETCH_MEM_STATS: //处理获取内存统计信息的请求-构造内存统计信息回复消息并发送给请求的客户端
    /* 数据结构和关键变量：widCliMap:工作节点映射，存储所有工作节点的ID和对应的客户端对象
     * buf：存储回复消息内容的缓冲区；send_buf：存储序列化后的消息内容的缓冲区
     * 
     */ 
      {
        UpdateWidMap(); //更新工作节点映射widCliMap。确保widCliMap包含最新的工作节点信息
        if(widCliMap.size() == 1) { //only have the info of the worker, who sends the request
          break; //如果工作节点映射中只有发送请求的工作节点，则无需回复，直接退出处理逻辑。避免发送冗余的内存统计信息
        }
        //构造回复消息
        WorkRequest lwr{};  //创建一个新的工作请求lwr，。构造内存统计信息回复消息并发送给请求的客户端
        lwr.op = FETCH_MEM_STATS_REPLY; //设置操作类型为FETCH_MEM_STATS_REPLY
        char buf[(widCliMap.size()-1) * MAX_MEM_STATS_SIZE + 1]; //op + list + \0 //定义缓冲区buf用于存储回复消息内容，大小为其他工作节点的数量乘以每个工作节点的最大内存统计信息大小
        char* send_buf = client->GetFreeSlot(); //获取客户端的空闲缓冲区槽send_buf，用于发送回复消息
        bool busy = false; //定义一个布尔变量busy，初始化为false，表示缓冲区未被临时分配。
        //检查缓冲区是否可用
        if(send_buf == nullptr) {  //如果没有可用的发送缓冲区槽
          busy = true; //设置busy标志为true，记录日志说明使用了临时缓冲区
          send_buf = (char *)zmalloc(MAX_REQUEST_SIZE); //使用中zamalloc动态分配一个临时缓冲区send_buf，大小为MAX_REQUEST_SIZE
          epicLog(LOG_INFO, "We don't have enough slot buf, we use local buf instead");
        } 

        int n = 0, i = 0;
        for (auto entry: widCliMap) { //遍历工作节点映射widCliMap
          if(entry.first == client->GetWorkerId()) continue; //跳过发送请求的工作节点
          Client* lc = entry.second;
          n += sprintf(buf + n, "%d:%ld:%ld:", lc->GetWorkerId(), //将每个工作节点的ID、总内存和空闲内存格式化为字符串并追加到缓冲区buf中
              lc->GetTotalMem(), lc->GetFreeMem());
          i++; //记录处理的工作节点数量i
        } //生成内存统计信息的回复内容
        //设置回复消息的内容
        lwr.size = widCliMap.size()-1; //设置工作请求对象lwr的大小为工作节点数量减去1（排除发送请求的工作节点）
        epicAssert(widCliMap.size()-1 == i); //使用断言检查处理的工作节点数量是否与预期一致
        lwr.ptr = buf; //设置工作请求对象lwr的指针指向消息内容缓冲区buf
        //序列化工作请求对象lwr到发送缓冲区send_buf中
        int len = 0, ret;
        lwr.Ser(send_buf, len); //调用WorkRequest::Ser方法，将工作请求对象lwr序列化到发送缓冲区send_buf中，并记录序列化后的长度len
        if((ret = client->Send(send_buf, len)) != len) { //调用Client::Send方法，将序列化后的消息发送给客户端
          epicAssert(ret == -1); //如果发送失败或部分发送，记录日志并断言发送失败的原因是缓冲区槽繁忙(ret==-1)
          epicLog(LOG_INFO, "slots are busy");
        }
        epicAssert((busy && ret == -1) || !busy); //断言发送结果是否符合预期：
        //如果缓冲区是临时分配(busy==true)，发送失败(ret==-1)是可以接受的，如果缓冲区不是临时分配的(busy==false)，则发送应该成功(ret!=-1)。
        // 插入日志信息，记录内存统计信息回复成功
        epicLog(LOG_INFO, "Successfully sent memory stats reply to client %d.", client->GetWorkerId());
        delete wr; //删除当前处理的工作请求wr，释放内存
        break;
      }
    case PUT: //键值存储PUT
      {
        void* ptr = zmalloc(wr->size);
        memcpy(ptr, wr->ptr, wr->size);
        kvs[wr->key] = pair<void*, Size>(ptr, wr->size); //将键值对存储到kvs中

        //epicLog(LOG_WARNING, "key = %d, value = %lx", wr->key, *(GAddr*)kvs[wr->key].first);

        // send reply back  构造回复消息
        wr->op = PUT_REPLY; //
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
    case GET: //键值存储GET
      {
        if(kvs.count(wr->key)) { //如果键值存在，构造回复消息并发送给客户端
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
        } else { //如果键值不存在，将请求加入挂起队列to_serve_kv_request
          to_serve_kv_request[wr->key].push(pair<Client*, WorkRequest*>(client, wr));
        }

        break;
      }
    default: //如果操作类型未知，记录警告日志
      epicLog(LOG_WARNING, "unrecognized work request %d", wr->op);
      break;
  }
}
/* 功能：将消息广播给所有客户端。通过遍历qpCliMap尝试使用客户端的空闲缓冲区发送消息。如果没有空闲缓冲区，则动态分配一个临时缓冲区
 * 参数：buf：要发送的消息内容；len：消息的长度
 * 数据结构和关键变量：qpCliMap：客户端映射表，存储所有客户端的连接信息
 * 函数包含了多种检查和断言，确保消息发送的正确性，并记录日志以便调试
 * */
void Master::Broadcast(const char* buf, size_t len) {
  for(auto entry: qpCliMap) {//遍历qpCliMap(一个unordered_map，存储了队列对编号qpn和对应客户端对象Client*的映射关系)中的每个客户端，对每个客户端执行广播操作
    char* send_buf = entry.second->GetFreeSlot();// 调用客户端对象的GetFreeSlot方法获取一个可用的发送缓冲区，如果没有可用的发送缓冲区，则返回nullptr 
    bool busy = false; 
    if(send_buf == nullptr) {//检查是否成功获取到空闲的发送缓冲区槽
      busy = true;
      send_buf = (char *)zmalloc(MAX_REQUEST_SIZE); //如果没有空闲槽，则需要分配一个新的缓冲区。使用zmalloc分配一个新的缓冲区，并记录日志
      epicLog(LOG_INFO, "We don't have enough slot buf, we use local buf instead");
    }
    memcpy(send_buf, buf, len); //将消息内容复制到发送缓冲区中。目标：准备好要发送的数据

    size_t sent = entry.second->Send(send_buf, len); //调用客户端的Send方法，将消息发送给客户端。目标：实际执行消息发送操作
    //为确保程序逻辑的正确性
    epicAssert((busy && sent == -1) || !busy); //断言发送操作的结果是否符合预期：如果缓冲区临时分配(busy==true)且发送失败(sent==-1)，则断言成立；如果缓冲区不是临时分配(busy==false)，发送应该成功。
    if(len != sent) { //检查时机发送的字节数是否与预期的消息长度一致。如果发送失败或部分发送，记录日志并进行断言
      epicAssert(sent == -1); //如果发送失败，sent应该为-1
      epicLog(LOG_INFO, "broadcast to %d failed (expected %d, but %d)\n", len, sent);
    } //结束当前客户端的广播处理，继续处理下一个客户端
  }
}
