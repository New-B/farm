// Copyright (c) 2018 The GAM Authors 

#include <cstring>
#include <arpa/inet.h>
#include "server.h"
#include "log.h"
#include "kernel.h"
#include "util.h"

Client* Server::NewClient() {
  Client* c = NewClient(IsMaster());
  return c;
}

Client* Server::NewClient(const char* rdmaConn) {
  return NewClient(IsMaster(), rdmaConn);
}

Client* Server::NewClient(bool isMaster, const char* rdmaConn) {
  try{
    Client* c = new Client(resource, isMaster, rdmaConn);
    uint32_t qp = c->GetQP();
    qpCliMap[qp] = c;//qpCliMap是工作节点用于保存与其他工作节点或主节点连接的核心数据结构

    // //将客户端的RDMA连接参数存储到新的成员变量中。
    // if (isMaster) {
    //   int workerId = c->GetWorkerId(); // 获取工作节点的 Worker ID
    //   workerRdmaParams[workerId] = std::string(rdmaConn); // 插入 RDMA 参数
    // }
    return c;
  } catch (int err){
    epicLog(LOG_WARNING, "Unable to create new client\n");
    return NULL;
  }
}

// std::string Server::GetWorkerRdmaParam(int workerId) {
//   try {
//     return workerRdmaParams.at(workerId);
//   } catch (const std::out_of_range& oor) {
//     epicLog(LOG_WARNING, "Cannot find RDMA param for worker %d (%s)", workerId, oor.what());
//     return "";
//   }
// }
/*功能：处理RDMA请求事件
 * 作用：从RDMA完成队列中轮选事件、根据事件类型(opcode)处理RDMA请求、响应远程请求或更新本地状态、提交新的接收请求，确保通信的连续性
 */
void Server::ProcessRdmaRequest() {
  void *ctx;
  int ne; //记录接收事件的数量
  ibv_wc wc[MAX_CQ_EVENTS]; //定义一个工作完成结构体数组wc，用于存储从RDMA完成队列中轮询到的事件 wc:work completion工作完成项
  ibv_cq *cq = resource->GetCompQueue(); //获取RDMA资源的完成队列
  Client *cli;  //定义一个指向Client对象的指针cli，用于指向触发事件的客户端对象
  uint32_t immdata, id;
  int recv_c = 0;

  epicLog(LOG_DEBUG, "received RDMA event\n"); //记录日志，表示收到RDMA事件
  /*
   * to get notified in the event-loop,
   * we need ibv_req_notify_cq -> ibv_get_cq_event -> ibv_ack_cq_events seq -> ibv_req_notify_cq!!
   */
  if (likely(resource->GetCompEvent())) { //检查是否有新的RDMA事件通知，如果有时间通知，进入处理逻辑
    do {
      ne = ibv_poll_cq(cq, MAX_CQ_EVENTS, wc);  //调用ibv_poll_cq从完成队列中轮询事件，最多获取MAX_CQ_EVENTS个事件
      if (unlikely(ne < 0)) { //如果轮询失败，记录错误日志并跳转到out标签 
        epicLog(LOG_FATAL, "Unable to poll cq\n");
        goto out;
      }

      for (int i = 0; i < ne; ++i) { //遍历轮询到的工作完成项
        /*
         * FIXME
         * 1) check whether the wc is initiated from the local host (ibv_post_send)
         * 2) if caused by ibv_post_send, then clear some stat used for selective signal
         *    otherwise, find the client, check the op code, process, and response if needed.
         */
        //查找对应的客户端
        cli = FindClient(wc[i].qp_num); //根据队列对编号(qp_num)查找对应的客户端对象
        if (unlikely(!cli)) { //如果找不到对应的客户端，记录警告日志并继续处理下一个事件 
          epicLog(LOG_WARNING, "cannot find the corresponding client for qp %d\n", wc[i].qp_num);
          continue;
        }
        //检查工作完成项状态是否成功
        if(wc[i].status != IBV_WC_SUCCESS) { //如果工作完成项状态不是成功，记录警告日志并继续处理下一个事件
          epicLog(LOG_WARNING, "Completion with error, op = %d (%d:%s)", wc[i].opcode, wc[i].status, ibv_wc_status_str(wc[i].status));
          continue;
        }

        epicLog(LOG_DEBUG, "transferred %d (qp_num %d, src_qp %d)", wc[i].byte_len, wc[i].qp_num, wc[i].src_qp);
        //处理工作完成项，根据操作码(opcode)执行不同的操作 
        switch (wc[i].opcode) {
          case IBV_WC_SEND: //发送操作完成事件
            epicLog(LOG_DEBUG, "get send completion event"); //记录发送完成事件
            id = cli->SendComp(wc[i]); //调用Client::SendComp方法处理发送完成事件，并获取工作请求ID 
            FarmResumeTxn(cli); //调用FarmResumeTxn方法恢复事务处理 
            break;
          case IBV_WC_RECV: //接收操作完成事件 
            {

              epicLog(LOG_DEBUG, "Get recv completion event"); //记录接收完成事件 
              char* data = cli->RecvComp(wc[i]); //调用Client::RecvComp方法处理接收完成事件，并获取接收到的数据指针 
              FarmProcessRemoteRequest(cli, data, wc[i].byte_len); //调用FarmProcessRemoteRequest方法处理远程请求
              recv_c++; //增加接收事件计数器，表示有新的接收请求
              break;
            }
          //处理其他事件
          case IBV_WC_RDMA_WRITE:
          case IBV_WC_RECV_RDMA_WITH_IMM:
          default:
            epicLog(LOG_WARNING, "unknown opcode received %d\n", wc[i].opcode); //记录未知或未处理的操作码
            break;
        }
      }
    } while (ne == MAX_CQ_EVENTS);

    if(recv_c) {//如果有接收事件
      //epicAssert(recv_c == resource->ClearRecv(low, high));
      int n = resource->PostRecv(recv_c); //调用RdmaResource::PostRecv方法提交新的接收请求
      epicAssert(recv_c == n);//确保提交的接收请求数量与接收事件数量一致
    }
  }

out:
  return;
}

Client* Server::FindClient(uint32_t qpn) {
  Client* cli = nullptr;
  try {
    cli = qpCliMap.at(qpn);
  } catch (const std::out_of_range& oor) {
    epicLog(LOG_WARNING, "cannot find the client for qpn %d (%s)", qpn, oor.what());
  }
  return cli;
}
/* 功能：更新widClienMap(工作节点ID到客户端对象的映射)
 * qpClienMap是队列对编号QP到客户端对象的映射
 * widCliMap是工作节点ID到客户端对象的映射
 * 该函数的目的是确保widCliMap包含所有已知的工作节点ID和对应的客户端对象 
*/
void Server::UpdateWidMap() {
  //确定需要更新的工作节点数
  /*如果当前服务器是主节点，则工作节点的数量等于qpCliMap.size()。
    如果当前服务器是工作节点，则需要忽略与主节点的连接，因此工作节点的数量是qpCliMap.size()-1*/
  int workers = IsMaster() ? qpCliMap.size() : qpCliMap.size()-1;
  //检查是否需要更新widCliMap，如果widCliMap中映射数量小于实际的工作节点数量，则需要更新
  //避免重复更新，只有在widCliMap不完整时才进行更新操作
  if(widCliMap.size() < workers) {
    //遍历qpCliMap中所有的队列对编号QP和对应的客户端对象
    //从qpCliMap中提取客户端对象，并根据其工作节点ID更新widCliMap
    for(auto it = qpCliMap.begin(); it != qpCliMap.end(); it++) {
      int wid = it->second->GetWorkerId();//获取客户端对应的工作节点ID，确定当前客户端对象所属的工作节点
      if(wid == GetWorkerId()) { //如果客户端的工作节点ID等于当前服务器的工作节点ID，则忽略该客户端，因为当前服务器不需要处理自己的连接
        epicAssert(!IsMaster()); //避免将与主节点自身的连接加入到widCliMap中 
        continue; //ignore the client to the master
      }
      if(!widCliMap.count(wid)) { //如果widCliMap中尚未包涵该工作节点ID的映射，则将该客户端对象添加到widCliMap中
        widCliMap[wid] = it->second;
      }
    }
    epicAssert(widCliMap.size() == workers); //使用断言检查更新后的widCliMap的大小是否与实际工作节点数量一致 
  }
}

Client* Server::FindClientWid(int wid) {
  UpdateWidMap();

  Client* cli = nullptr;
  try {
    cli = widCliMap.at(wid);
  } catch (const std::out_of_range& oor) {
    epicLog(LOG_WARNING, "cannot find the client for worker %d (%s)", wid, oor.what());
  }

  return cli;
}

void Server::RmClient(Client* c) {
  qpCliMap.erase(c->GetQP());
}

