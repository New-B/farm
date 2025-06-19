// Copyright (c) 2018 The GAM Authors

#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <climits>
#include <arpa/inet.h>

#include "rdma.h"
#include "settings.h"
#include "zmalloc.h"
#include "log.h"
#include "kernel.h"

static int page_size = 4096;
int MAX_RDMA_INLINE_SIZE = 256;
/*RdmaResource类的构造函数，用于初始化一个RDMA资源对象，有两个参数：
1.一个指向RDMA设备的指针
2.一个bool值master，表示是否是为主节点创建*/
RdmaResource::RdmaResource (ibv_device *dev, bool master) :
  device (dev), isForMaster(master),
  base (nullptr), bmr(nullptr), size(0),
  rdma_context_counter (0), slot_inuse(0), slot_head(0) {

  int rx_depth;

  if (!(context = ibv_open_device(dev))) { //打开RDMA设备上下文
    epicLog(LOG_FATAL, "unable to get context for %s\n",
        ibv_get_device_name (dev));
    return;
  }

  if (!(channel = ibv_create_comp_channel(this->context))) { //创建完成事件通道
    epicLog(LOG_FATAL, "Unable to create comp channel\n");
    goto clean_ctx;
  }

  if (!(pd = ibv_alloc_pd(this->context))) {  //分配保护域
    epicLog(LOG_FATAL, "Unable to allocate pd\n");
    goto clean_channel;
  }

  rx_depth = (isForMaster) ?  //根据是否是主节点设置接收深度
    MASTER_RDMA_SRQ_RX_DEPTH : WORKER_RDMA_SRQ_RX_DEPTH;

  if (!(cq = ibv_create_cq(this->context, //创建完成队列
          (rx_depth << 1) + 1, NULL, this->channel, 0))) {
    epicLog(LOG_FATAL, "Unable to create cq\n");
    goto clean_pd;
  }

  { //创建共享接收队列：初始化共享接收队列的属性，并尝试创建共享接收队列。
    ibv_srq_init_attr attr = {};
    attr.attr.max_wr = rx_depth;
    attr.attr.max_sge = 1;

    if (!(srq = ibv_create_srq(this->pd, &attr))) {
      epicLog(LOG_FATAL, "Unable to create srq\n");
      goto clean_cq;
    }
  }

  if (ibv_query_port(context, ibport, &portAttribute)) {//查询端口属性
    epicLog(LOG_FATAL, "Unable to query port %d\n", ibport);
    goto clean_srq;
  }

  devName = ibv_get_device_name (this->device); //获取RDMA设备的名称
  srand48(time (NULL));
  psn = lrand48() & 0xffffff; //生成随机数作为包序列号

  /* Request notification upon the next completion event */
  if (ibv_req_notify_cq(cq, 0)) { //请求完成队列通知
    fprintf (stderr, "Couldn't request CQ notification\n");
    return;
  }

  epicLog(LOG_DEBUG, "new rdma resource\n");

  return;

clean_srq: //清理和异常处理：如果在初始化过程中发生错误，执行相应的清理操作并抛出异常
  ibv_destroy_srq(this->srq);
clean_cq: 
  ibv_destroy_cq(this->cq);
clean_pd:
  ibv_dealloc_pd(this->pd);
clean_channel:
  ibv_destroy_comp_channel (this->channel);
clean_ctx:
  ibv_close_device (this->context);

  throw RDMA_RESOURCE_EXCEPTION;
  epicPanic("Unable to acquire RDMA resource");
}

RdmaResource::~RdmaResource () {
  ibv_destroy_srq(this->srq);
  ibv_destroy_cq(this->cq);
  ibv_dealloc_pd(this->pd);
  ibv_destroy_comp_channel (this->channel);
  ibv_close_device (this->context);
  for (auto mr: comm_buf) {
    ibv_dereg_mr (mr);
    zfree (mr->addr);
  }
}

/*
 * 1.将制定的内存区域注册为RDMA的本地内存区域(Memory Region, MR)
 * 2.注册的内存区域可以被RDMA操作访问，包括本地写入、远程写入和远程读取
 * 3.注册的内存区域的大小和基地址由参数sz和base指定 
 * 4.返回值为0表示注册成功，-1表示注册失败
 */
int RdmaResource::RegLocalMemory(void *base, size_t sz) {
  int ret = -1;
  /* 
   * this->base检查是否已经有内存区域被注册，如果base不为空，说明已经注册过内存区域
   * this->bmr检查是否已经有内存区域被注册，如果bmr不为空，说明已经注册过内存区域 
   * isForMaster检查是否是主节点，如果是主节点，则不允许注册内存区域
   */
  if (this->base || this->bmr || isForMaster) { 
    epicLog(LOG_WARNING, "An mr has already be registered or I am a master\n");
    return ret;
  }
  //调用ibv_reg_mr函数将制定的内存区域注册为RDMA的本地内存区域
  bmr = ibv_reg_mr (this->pd, const_cast<void *>(base), sz, //保护域pd：用于管理RDMA资源；base：要注册的内存区域的起始地址；sz：要注册的内存区域的大小
      IBV_ACCESS_LOCAL_WRITE //允许本地写入
      | IBV_ACCESS_REMOTE_WRITE //允许远程写入
      | IBV_ACCESS_REMOTE_READ); //允许远程读取
  if (!bmr) { //如果注册成功，ibv_reg_mr返回一个指向ibv_mr的指针；如果注册失败，返回nullptr 
    epicLog(LOG_FATAL, "Unable to register mr for hash table");
    return ret;
  }
  epicLog(LOG_INFO, "registered local memory region at %p with size %ld\n", base, sz);

  this->base = base;
  return 0;
}
/*
 * 1.为RDMA通信分配和注册通信槽(communication slots)
 * 2.如果现有的槽数量不足，则动态分配更多的槽，并将其注册为RDMA的本地内存区域(Memory Region, MR)
 * 3.返回值为0表示注册成功，-1表示注册失败
 */
int RdmaResource::RegCommSlot(int slot) {
  epicLog(LOG_DEBUG, "trying to register %d slots", slot);

  if (slots.size() - slot_inuse >= slot) {
    epicLog(LOG_DEBUG, "no need to register: inuse = %d, current slots = %d\n",
        slot_inuse, slots.size ());
    slot_inuse += slot;
    return 0;
  } else {
    slot_inuse += slot; //如果需要分配更多槽，首先需要更新slot_inuse的值，增加需要注册的槽数量
    int i = slots.size ();
    for (; i < slot_inuse; i += RECV_SLOT_STEP) { //从当前槽总数slots.size()开始，每次分配RECV_SLOT_STEP个槽，直到满足slot_inuse的要求 
      int sz = roundup(RECV_SLOT_STEP*MAX_REQUEST_SIZE, page_size); //计算需要分配的内存大小，确保对齐到页面大小
      void* buf = zmalloc(sz); //调用zmalloc函数分配内存，用于存储通信槽
      struct ibv_mr *mr = ibv_reg_mr (this->pd, buf, sz, //调用ibv_reg_mr将分配的内存注册为RDMA的本地内存区域
          IBV_ACCESS_LOCAL_WRITE
          | IBV_ACCESS_REMOTE_WRITE
          | IBV_ACCESS_REMOTE_READ);
      if (!mr) {
        epicLog(LOG_FATAL, "Unable to register mr for communication slots");
        return -1;
      }
      comm_buf.push_back(mr); //将注册的ibv_mr对象存储到comm_buf向量中
      epicAssert(mr->addr == buf && mr->length == sz);
    }
    slots.reserve (i); //调用reserve()函数预留足够的空间以存储新的槽状态
    for (int j = slots.size(); j < i; j++) {  //将新分配的槽状态初始化为false，表示这些槽尚未被使用
      slots.push_back(false);
    }

    epicLog(LOG_DEBUG, "registered %d, enlarge to %d with inuse = %d\n",
        slot, slots.size (), slot_inuse);
    epicAssert(slots.size () % RECV_SLOT_STEP == 0);
    return 0;
  }
}

char* RdmaResource::GetSlot(int slot) {
  epicAssert(slots.at(slot) == true && slot < slot_inuse);
  // TODO: check slot == tail
  return (char*) ((uintptr_t)comm_buf[BPOS(slot)]->addr + BOFF(slot));
}
/* 主要功能：
 * 1.发布RDMA接收请求：为RDMA通信准备接收缓冲区；将接收请求提交到共享接收队列(SRQ)
 * 2.管理接收缓冲区：使用slots数据结构管理接收缓冲区的状态(是否被占用)；动态分配接收缓冲区并注册为RDMA内存区域。
 * 3.处理接收请求失败的情况：如果接收请求提交失败，清理相关资源并更新缓冲区状态。
 */
int RdmaResource::PostRecv(int n) { //n为最大待处理消息数
  ibv_recv_wr rr[n];  //定义接收请求数组rr
  memset(rr, 0, sizeof (ibv_recv_wr)*n);  //初始化接收请求
  ibv_sge sge[n];  //定义缓冲区描述符数组sge
  int i, ret;
  int head_init = slot_head;  //记录初始的接收槽头部索引，用于后续循环中检查是否遍历完所有接收槽
  //遍历接收槽并准备接收请求
  for (i = 0; i < n;) { //遍历接收槽，找到空闲的接收槽
    if (slots.at(slot_head) == true) { //检查当前槽是否被占用
      if (++slot_head == slot_inuse) slot_head = 0; //如果到达槽尾部，循环回到槽头部
      if (slot_head == head_init) { //如果遍历完所有接收槽仍没有找到空闲槽，记录错误日志并退出
        epicLog(LOG_FATAL, "cannot find free recv slot (%d)", n);
        break;
      }
      continue; //如果当前槽已被占用，继续检查下一个槽
    }
    int bpos = BPOS(slot_head); //计算缓冲区基地址
    int boff = BOFF(slot_head); //计算缓冲区偏移量
    //为每个接收槽构造缓冲区描述符sge和接收请求rr
    sge[i].length = MAX_REQUEST_SIZE;  //设置缓冲区长度
    sge[i].lkey = comm_buf[bpos]->lkey; //获取缓冲区的本地密钥 
    sge[i].addr = (uintptr_t)comm_buf[bpos]->addr + boff; //获取缓冲区的实际地址

    rr[i].wr_id = slot_head; //设置接收请求的ID为当前槽的索引 
    rr[i].num_sge = 1; //设置缓冲区描述符数量，接收请求的SGE数量为1
    rr[i].sg_list = &sge[i]; //将缓冲区描述符指针赋值给接收请求的sg_list字段，设置缓冲区描述符列表
    if (i+1 < n) rr[i].next = &rr[i+1]; //设置接收请求链表的下一个节点。如果不是最后一个接收请求，将下一个接收请求的指针赋值给当前接收请求的next字段

    //advance the slot_head by 1
    slots.at(slot_head) = true; //将当前槽的状态设置为已占用
    if (++slot_head == slot_inuse) slot_head = 0;  //更新接收槽头部索引。如果到达槽尾部，循环回到槽头部
    i++; //增加成功构造的接收请求数量
  }
  ret = i;  //保存成功构造的接收请求数量
  //提交接收请求
  if (i > 0) { //如果成功构造了接收请求 
    rr[i-1].next = nullptr; //将最后一个接收请求的next字段设置为nullptr，表示这是接收请求链表的末尾 
    ibv_recv_wr* bad_rr;
    if (ibv_post_srq_recv(srq, rr, &bad_rr)) { //调用ibv_post_srq_recv将接收请求提交到共享队列SRQ 
      epicLog(LOG_WARNING, "post recv request failed (%d:%s)\n", //如果提交失败，记录警告日志并清理相关资源
          errno, strerror (errno));
      int s = bad_rr->wr_id; //获取提交失败的接收请求的ID
      ret -= RMINUS(slot_head, s, slot_inuse);  //更新成功提交的接收请求数量
      while (s != slot_head) { //清理失败的接收槽状态
        slots.at(s) = false; //将失败的接收槽标记为未占用
        if (++s == slot_inuse) s = 0;  //更新接收槽索引
      }
      slot_head = s; //更新接收槽头部索引
    }
  }
  return ret;
}

const char *RdmaResourceFactory::defaultDevname = NULL;
const char *RdmaResourceFactory::workerDevname = NULL;
/*定义了一个静态成员变量，作为RdmaResourceFactory的全局资源管理容器。
resources是RdmaResourceFactory类的一个静态成员变量，用于存储所有已创建的RdmaResource对象的指针。
它是一个全局共享的容器，所有RdmaResourceFactory的实例都可以访问和管理这些RDMA资源。*/
std::vector<RdmaResource *> RdmaResourceFactory::resources;
/*RdmaResourceFactory类的一个静态成员函数，用于获取或创建一个RDMA资源对象*/
RdmaResource* RdmaResourceFactory::GetRdmaResource (
    bool isMaster, const char *devName) {
  if (!devName) {  //如果设备名称devName为空，则使用默认设备名称defaultDevname
    if (isMaster) {
        devName = defaultDevname; // 主节点使用默认设备名称
    } else {
        devName = workerDevname; // 工作节点使用单独的设备名称
    }
  }
  //查找已有的RDMA资源
  if (devName) {//如果设备名称不为空，遍历resources向量，查找是否已有匹配的RDMA资源对象
    for (std::vector<RdmaResource *>::iterator it = resources.begin();
        it != resources.end(); ++it) {
      if (!strcmp((*it)->GetDevname(), devName) //匹配条件是设备名称相同且isMaster标志相同
          && (*it)->IsMaster () == isMaster)
        return (*it); //如果找到匹配的RDMA资源对象，则返回该对象
    }
  }

  ibv_device **list = ibv_get_device_list(NULL); //调用ibv_get_device_list()获取设备列表

  if (!devName && list[0]){
    for (int i = 0; list[i]; ++i) {
      epicLog(LOG_INFO, "RDMA device: %s\n", ibv_get_device_name(list[i]));

      struct ibv_context *context = ibv_open_device(list[i]);
      if (!context) {
        epicLog(LOG_WARNING, "Unable to open device %s\n", ibv_get_device_name(list[i]));
        continue;// 如果设备无法打开，跳过该设备
      }

      struct ibv_port_attr port_attr;
      if (ibv_query_port(context, 1, &port_attr)) { // 查询端口 1 的属性
        epicLog(LOG_WARNING, "Unable to query port for device %s", ibv_get_device_name(list[i]));
        ibv_close_device(context);
        continue; // 如果查询端口失败，跳过该设备
      }

      if (port_attr.state != IBV_PORT_ACTIVE) { // 检查端口状态是否为活动状态
        epicLog(LOG_WARNING, "Device %s is not active (state: %d)", ibv_get_device_name(list[i]), port_attr.state);
        ibv_close_device(context);
        continue; // 如果端口不是活动状态，跳过该设备
      }

      // devName = defaultDevname = ibv_get_device_name (list[i]);
      if (isMaster) {
          devName = defaultDevname = ibv_get_device_name(list[i]); // 主节点设置默认设备名称
      } else {
          devName = workerDevname = ibv_get_device_name(list[i]); // 工作节点设置单独的设备名称
      }

      try { // 如果找到匹配的设备且设备可用，尝试创建新的 RdmaResource 对象
        RdmaResource *ret = new RdmaResource(list[i], isMaster);
        resources.push_back(ret); // 如果创建成功，将新创建的 RdmaResource 对象添加到 resources 向量中
        ibv_close_device(context); // 关闭设备上下文
        return ret; // 并返回该对象
      } catch (int err) { // 如果创建失败，记录日志并返回 NULL
        epicLog(LOG_FATAL, "Unable to get RDMA resource for device %s", ibv_get_device_name(list[i]));
        ibv_close_device(context);
        return NULL;
      }
    }
  }
  return NULL;
}

/*
 * TODO: check whether it is necessary if we already use the epoll mechanism
 */
/*
 * 功能：从RDMA完成事件通道中获取完成队列事件；确认完成队列事件，允许队列继续处理新的事件；请求下一次完成队列事件通知，确保事件处理的连续性。并设置通知机制以便处理后续事件
 * 返回值：true-成功获取事件并设置通知；false-获取事件或设置通知失败
 * RDMA使用完成队列CQ和完成事件通道Completion Event Channel来处理异步事件。
 * GetCompEvent函数是事件处理的一部份，确保完成队列事件能够被正确处理和获取。该函数是RDMA通信的核心函数之一，确保完成队列事件处理的连续性和可靠性。
 */
bool RdmaResource::GetCompEvent() const {
  //获取完成队列事件
  struct ibv_cq *ev_cq;
  void *ev_ctx;
  int ret;
  ret = ibv_get_cq_event(channel, &ev_cq, &ev_ctx); //调用ibv_get_cq_event从RDMA完成事件通道中获取完成队列事件
  /*channel-完成事件通道；ev_cq-指向触发事件的完成队列；ev_ctx-事件上下文。*/
  if (ret) { //如果获取事件失败（ret!=0）。记录日志并返回false
    epicLog(LOG_FATAL, "Failed to get cq_event\n");
    return false;
  }
  /* Ack the event 确认事件*/
  ibv_ack_cq_events(ev_cq, 1); //调用ibv_ack_cq_events确认完成队列事件。参数：ev_cq-指向触发事件的完成队列；1-确认的事件数量。
  //确认事件后，完成队列可以继续处理新的事件

  /* Request notification upon the next completion event */
  //请求下一次事件通知
  ret = ibv_req_notify_cq(ev_cq, 0); //调用ibv_req_notify_cq请求完成队列的下一次事件通知。参数：ev_cq-指向触发事件的完成队列；1-表示仅在新的完成事件发生时通知。
  if (ret) { //如果请求通知失败，记录错误并返回false
    fprintf (stderr, "Couldn't request CQ notification\n");
    return false;
  }
  return true; //返回成功状态
}

RdmaContext* RdmaResource::NewRdmaContext(bool isForMaster) {
  rdma_context_counter++;

  int s = isForMaster ? MAX_MASTER_PENDING_MSG : MAX_WORKER_PENDING_MSG;
  if (RegCommSlot(s)) {
    epicLog(LOG_WARNING, "unable to register more communication slots\n");
    return nullptr;
  }
  epicLog(LOG_DEBUG, "new RdmaContext: %d\n", rdma_context_counter);
  return new RdmaContext(this, isForMaster);
}

void RdmaResource::DeleteRdmaContext(RdmaContext* ctx) {
  rdma_context_counter--;
  // TODO: de-regsiter the slots
  epicLog(LOG_DEBUG, "delete RdmaContext: %d\n", rdma_context_counter);
  delete ctx;
}

RdmaContext::RdmaContext(RdmaResource *res, bool master):
  resource (res), isForMaster(master), msg() {
  // check either master == true,
  // or both isMaster () in RdmaContext and RdmaResouce are fals
  epicAssert(IsMaster () || IsMaster() == res->IsMaster());

  max_pending_msg = IsMaster () ?
    MAX_MASTER_PENDING_MSG : MAX_WORKER_PENDING_MSG;
  int max_buf_size = IsMaster () ?
    MASTER_BUFFER_SIZE : WORKER_BUFFER_SIZE;

  void* buf = zmalloc(roundup(max_buf_size, page_size));
  if (unlikely(!buf)) {
    epicLog(LOG_WARNING, "Unable to allocate memeory\n");
    goto send_buf_err;
  }

  //init the send buf
  send_buf = ibv_reg_mr (res->pd, buf, max_buf_size,
      IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE);
  if (unlikely(!send_buf)) {
    epicLog(LOG_WARNING, "Unable to register mr\n");
    goto send_mr_err;
  }

  slot_head = slot_tail = 0;
  pending_msg = to_signaled_send_msg = to_signaled_w_r_msg  = 0;
  max_unsignaled_msg = MAX_UNSIGNALED_MSG > max_pending_msg ?
    max_pending_msg : MAX_UNSIGNALED_MSG;
  // because we're using uint16_t to represent
  // currently to_be_signalled msg
  epicAssert(max_unsignaled_msg <= USHRT_MAX);
  full = false;

  {
    ibv_qp_init_attr attr = {};
    attr.srq = res->srq;
    attr.send_cq = res->cq;
    attr.recv_cq = res->cq;
    attr.qp_type = IBV_QPT_RC;
    attr.cap.max_send_wr = max_pending_msg;
    attr.cap.max_send_sge = 1;
    attr.sq_sig_all = 0;
    //		attr.cap.max_recv_wr = 1;
    //		attr.cap.max_recv_sge = 1;
    attr.cap.max_inline_data = MAX_RDMA_INLINE_SIZE;

    qp = ibv_create_qp(res->pd, &attr);
  }

  if (unlikely(!qp)) {
    epicLog(LOG_WARNING, "Unable to create QP (%d:%s)\n", errno, strerror (errno));
    goto clean_mr;
  }

  {
    // set qp to init status
    ibv_qp_attr qattr = {};
    qattr.qp_state = IBV_QPS_INIT;
    qattr.pkey_index = 0;
    qattr.port_num = res->ibport;
    qattr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE |
      IBV_ACCESS_REMOTE_WRITE |
      IBV_ACCESS_REMOTE_ATOMIC |
      IBV_ACCESS_REMOTE_READ;

    if (ibv_modify_qp(qp, &qattr,
          IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT
          | IBV_QP_ACCESS_FLAGS)) {
      epicLog(LOG_WARNING, "Unable to modify qp to init status\n");
      goto clean_qp;
    }
  }

  {
    ibv_qp_init_attr attr = {};
    ibv_qp_attr qattr = {};
    if (ibv_query_qp(qp, &qattr, IBV_QP_CAP, &attr)) {
      epicLog(LOG_WARNING, "Unable to query qp");
      goto clean_qp;
    }

    epicLog(LOG_INFO, "qattr.cap.max_inline_data = %u, attr.cap.max_inline_data = %u",
        attr.cap.max_inline_data, qattr.cap.max_inline_data);
    if (attr.cap.max_inline_data == 0) {
      epicLog(LOG_WARNING, "Do NOT support inline data");
      MAX_RDMA_INLINE_SIZE = 0;
    }
  }

  return;

clean_qp:
  ibv_destroy_qp(qp);
clean_mr:
  ibv_dereg_mr (send_buf);
send_mr_err:
  zfree (buf);
send_buf_err:
  throw RDMA_CONTEXT_EXCEPTION;
}

int RdmaContext::SetRemoteConnParam(const char *conn) {
  int ret;
  uint32_t rlid, rpsn, rqpn, rrkey; //远程LID(Local Identifier)、远程队列对编号、远程包序列号、远程内存区域的密钥
  uint64_t rvaddr; //远程内存区域的虚拟地址 
  //解析远程RDMA连接参数conn
  if (IsMaster()) {   //
    /* conn should be of the format "lid:qpn:psn" */
    sscanf (conn, "%x:%x:%x", &rlid, &rqpn, &rpsn); 
  } else {
    /* conn should be of the format "lid:qpn:psn:rkey:vaddr" */
    sscanf (conn, "%x:%x:%x:%x:%lx", &rlid, &rqpn, &rpsn, &rrkey, &rvaddr);
    this->rkey = rrkey;
    this->vaddr = rvaddr;
  }
  //根据解析的参数，修改本地队列对QP的状态，使其进入RTR和RTS状态 
  /* modify qp to RTR state */
  {
    ibv_qp_attr attr = {}; //zero init the POD value (DON'T FORGET!!!!) 初始化QP属性结构体
    attr.qp_state = IBV_QPS_RTR; // 设置 QP 状态为 RTR
    attr.path_mtu = IBV_MTU_2048; // 设置路径的最大传输单元（MTU）
    attr.dest_qp_num = rqpn; // 设置目标队列对编号
    attr.rq_psn = rpsn; // 设置目标包序列号
    attr.max_dest_rd_atomic = 1; // 设置最大目标原子操作数
    attr.min_rnr_timer = 12; // 设置最小 RNR 超时时间
    attr.ah_attr.is_global = 0; // 设置地址句柄为本地
    attr.ah_attr.dlid = rlid; // 设置目标 LID
    attr.ah_attr.src_path_bits = 0; // 设置源路径位
    //attr.ah_attr.sl = 1;
    attr.ah_attr.port_num = resource->ibport; // 设置端口号

    ret = ibv_modify_qp(this->qp, &attr,
        IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN
        | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC
        | IBV_QP_MIN_RNR_TIMER);
    if (unlikely(ret)) {
      epicLog(LOG_WARNING, "Unable to modify qp to RTR (%d:%s)\n", errno,
          strerror (errno));
      return 1;
    }
  }

  {
    ibv_qp_attr attr = {};
    /* modify qp to rts state */
    attr.qp_state = IBV_QPS_RTS; // 设置 QP 状态为 RTS
    attr.timeout = 14; // 设置超时时间
    attr.retry_cnt = 7; // 设置重试次数
    attr.rnr_retry = 7; // 设置 RNR 重试次数
    attr.sq_psn = this->resource->psn; // 设置发送包序列号
    attr.max_rd_atomic = 1; // 设置最大原子操作数
    ret = ibv_modify_qp(this->qp, &attr,
        IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT
        | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN
        | IBV_QP_MAX_QP_RD_ATOMIC);

    if (unlikely(ret)) {
      epicLog(LOG_WARNING, "Unable to modify qp to RTS state\n");
      return 1;
    }
  }

  resource->PostRecv(max_pending_msg); //为接收操作预先发布接收请求，max_pending_msg表示最大刮起消息数
  return 0; //如果所有操作成功，返回0，表示设置远程连接参数成功。
}

const char* RdmaContext::GetRdmaConnString() {
  if (!msg) {
    if (IsMaster())
      msg = (char *) zmalloc(MASTER_RDMA_CONN_STRLEN + 1); //1 for \0
    else
      msg = (char *) zmalloc(WORKER_RDMA_CONN_STRLEN + 1);
  }

  if (unlikely(!msg)) {
    epicLog(LOG_WARNING, "Unable to allocate memory\n");
    goto out;
  }

  /*
   * we use RDMA send/recv to do communication
   * for communication among workers, we also allow direct access to the whole memory space so that we expose the base addr and rkey
   */
  if (IsMaster()) {
    sprintf (msg, "%04x:%08x:%08x", this->resource->portAttribute.lid,
        this->qp->qp_num, this->resource->psn);
  } else {
    sprintf (msg, "%04x:%08x:%08x:%08x:%016lx",
        this->resource->portAttribute.lid, this->qp->qp_num,
        this->resource->psn, this->resource->bmr->rkey,
        (uintptr_t) this->resource->base);
  }
out:
  epicLog(LOG_DEBUG, "msg = %s\n", msg);
  return msg;
}
/* GetFreeSlot函数用于获取一个空闲的发送缓冲区槽，用于RDMA操作（如发送数据）、如果没有空闲槽，则返回nullptr。
 * 动态管理发送缓冲区槽：使用slot_head和slot_tail来管理缓冲区的状态，确保缓冲区的循环使用，避免资源浪费
 * 提高RDMA操作效率：快速获取空闲槽地址，用于RDMA操作(如发送数据)；如果缓冲区已满，即使返回nullptr，避免资源冲突
 * 支持高并发RDMA通信：通过槽的动态管理，支持高并发的RDMA通信，允许多个RDMA操作同时进行而不会阻塞。确保缓冲区的高效使用。
 * 数据结构和关键变量：
 * - send_buf：发送缓冲区的内存区域，使用ibv_mr结构体表示。RDMA注册的发送缓冲区，存储所有发送槽
 * - slot_head：指向当前可用的发送缓冲区槽的头部索引。指向当前可用的发送缓冲区，每次获取空闲槽后，向前移动。
 * - slot_tail：指向发送缓冲区槽的尾部索引。指向当前已完成的发送缓冲区槽，在发送完成事件中更新
 * - max_pending_msg：最大挂起消息数，表示发送缓冲区的容量。限制发送缓冲区的使用。
 * - pending_msg：当前挂起的消息数，表示已发送但未完成的消息数量。
 * - full：表示发送缓冲区是否已满的标志。
 * - MAX_REQUEST_SIZE：每个发送缓冲区槽的大小，表示每个槽可以存储的数据量。
 * 
 * 流程：计算可用槽数量-检查槽是否可用-获取空闲槽的地址-更新槽状态-返回槽地址
 * 功能：获取一个空闲的发送缓冲区槽，用于RDMA操作；动态管理发送缓冲区，支持高效通信
 */
char* RdmaContext::GetFreeSlot() {
  //计算当前可用的发送缓冲区槽数量
  //如果slot_tail>=slot_head，则可用槽数量为slot_tail - slot_head
  //如果slot_tail<slot_head，则可用槽数量为slot_tail + max_pending_msg - slot_head 
  int avail = RMINUS(slot_tail, slot_head, max_pending_msg); //slot_head <= slot_tail ? slot_tail-slot_head : slot_tail+max_pending_msg-slot_head;
  if (!avail && !full) avail = max_pending_msg; //如果当前没有可用的槽且缓冲区未满，则将可用槽数量设置为最大挂起消息数max_pending_msg ，既认为所有槽都是可用的
  epicLog(LOG_DEBUG, "avail = %d, pending_msg = %d", avail, pending_msg);
  if (avail <= 0 || pending_msg >= max_pending_msg) { //如果没有可用槽， 或者 挂起的消息数达到最大，则记录日志并返回nullptr 
    epicLog(LOG_INFO, "all the slots are busy\n");
    return nullptr;
  }
  //epicLog(LOG_DEBUG, "get one free slot (to_signaled_send_msg = %d)", to_signaled_send_msg);
  //获取空闲槽的地址
  char* s = (char*)send_buf->addr + slot_head*MAX_REQUEST_SIZE; //send_buf->addr指向发送缓冲区的起始地址；slot_head*MAX_REQUEST_SIZE用于计算当前槽的偏移地址；每个槽的大小为MAX_REQUEST_SIZE。 
  if (++slot_head == max_pending_msg) slot_head = 0; //将slot_head指针向前移动一个槽，如果slot_head到达缓冲区末尾，则循环回到缓冲区的起始位置。
  if (slot_head == slot_tail) full = true; // //如果slot_head指针与slot_tail指针相等，则表示发送缓冲区已满，设置full标志为true。 
  return s; //返回当前槽的地址，用于RDMA操作
}

bool RdmaContext::IsRegistered(const void* addr) {
  return ( (uintptr_t)addr >= (uintptr_t)send_buf->addr) && ((uintptr_t)addr < (uintptr_t)send_buf->addr+send_buf->length);
}
/* Rdma函数是RDMA操作的核心实现，目前只支持一种RDMA操作(IBV_WR_SEND、)，它负责构造RDMA请求并将其提交到对列队QP
 * 参数说明：
 * op: RDMA操作类型，IBV_WR_SEND表示发送操作
 * src: 源数据缓冲区地址，表示要发送的数据
 * len: 源数据缓冲区的长度，表示要发送的数据长度
 * id: 工作请求的ID，用于标识该请求
 * signaled: 是否需要发送完成事件，表示是否需要在发送完成后通知应用程序
 * dest: 目标地址，表示数据要发送到的远程地址，仅用于写操作
 * imm: 立即数值，仅用于写操作，表示要发送的立即数
 * oldval: 旧值，仅用于比较和交换操作，表示要比较的旧值
 * newval: 新值，仅用于比较和交换操作，表示要设置的新值
 * 返回值：
 * 如果操作成功，返回发送的数据长度；如果操作失败，返回-1或其他错误码
 */
ssize_t RdmaContext::Rdma(ibv_wr_opcode op , const void* src, size_t len, unsigned int id,
    bool signaled, void* dest, uint32_t imm, uint64_t oldval, uint64_t newval) {
  epicAssert(pending_msg < max_pending_msg);  //确保当前挂起的消息数未超过允许的最大值

  if (op == IBV_WR_SEND) { //检查操作类型，目前仅支持发送操作 
    if (!IsRegistered(src) && len > MAX_RDMA_INLINE_SIZE) { //处理发送缓冲区：如果源缓冲区未注册且数据长度超过最大内联大小MAX_RDMA_INLINE_SIZE，则需要将数据复制到一个空闲的发送缓冲区
      if (len > MAX_REQUEST_SIZE) { //确保数据长度未超过允许的最大请求大小，如果超过，记录警告日志并触发断言
        epicLog(LOG_WARNING, "len = %d, MAX_REQUEST_SIZE = %d, src = %s\n", len, MAX_REQUEST_SIZE, src);
        epicAssert(false);
      }
      char* sbuf = GetFreeSlot(); //获取一个空闲的发送缓冲区，如果没有可用的缓冲区，则返回nullptr 
      epicAssert(sbuf); //如果获取到的缓冲区为空，表示没有可用的发送缓冲区，触发断言
      memcpy(sbuf, src, len); //将源缓冲区的数据复制到发送缓冲区
      zfree ((void*)src); //释放源缓冲区的内存
      sge_list.addr = (uintptr_t)sbuf; //设置SGE列表的地址sge_list.addr为发送缓冲区的地址
      pending_send_msg++; //增加待发送消息计数器pending_send_msg，表示有一个新的消息待发送
    } else {
      //epicLog(LOG_DEBUG, "Registered mem");
      if (IsRegistered(src)) pending_send_msg++;  //如果源缓冲区已注册，则直接使用源缓冲区 
      sge_list.addr = (uintptr_t)src; //设置SGE列表的地址sge_list.addr为源缓冲区的地址
    }
    sge_list.lkey = send_buf->lkey; //设置SGE列表的本地密钥sge_list.lkey为发送缓冲区的本地密钥

  } else {
    epicLog(LOG_WARNING, "unsupported RDMA OP");
    return -1;
  }
  //构造工作请求
  sge_list.length = len;
  //设置工作请求的操作码、工作请求ID、SGE列表、SGE数量、下一个工作请求指针、发送标志等属性 
  wr.opcode = op;
  wr.wr_id = -1;
  wr.sg_list = &sge_list;
  wr.num_sge = len == 0 ? 0 : 1;
  wr.next = nullptr;
  wr.send_flags = 0;
  //如果数据长度小于等于最大内联大小，则设置发送标志为IBV_SEND_INLINE，表示使用内联发送
  if (len <= MAX_RDMA_INLINE_SIZE) wr.send_flags = IBV_SEND_INLINE;

  pending_msg++; //更新挂起消息计数，计算当前需要发送完成事件的消息数
  uint16_t curr_to_signaled_send_msg = pending_send_msg - to_signaled_send_msg;
  uint16_t curr_to_signaled_w_r_msg = pending_msg - pending_send_msg - to_signaled_w_r_msg;
  //检查是否需要发送完成事件
  if (curr_to_signaled_send_msg + curr_to_signaled_w_r_msg == max_unsignaled_msg || signaled) { //we signal msg for every max_unsignaled_msg
    //如果达到最大未发送完成事件的消息数，或者显示要求发送完成事件，则设置发送标志为IBV_SEND_SIGNALED
    wr.send_flags |= IBV_SEND_SIGNALED;
    if (wr.opcode == IBV_WR_SEND) {
      epicLog(LOG_DEBUG, "signaled %s\n", (char*)sge_list.addr);
    } else {
      epicLog(LOG_INFO, "signaled, op = %d", wr.opcode);
    }

    to_signaled_send_msg += curr_to_signaled_send_msg;
    epicAssert(to_signaled_send_msg == pending_send_msg);
    to_signaled_w_r_msg += curr_to_signaled_w_r_msg;
    epicAssert(to_signaled_send_msg + to_signaled_w_r_msg == pending_msg);
    epicAssert(curr_to_signaled_send_msg + curr_to_signaled_w_r_msg <= max_unsignaled_msg);

    /*
     * higher to lower: send_msg(16), w_r_msg(16), workid(32)
     */
    /*
     * FIXME: only such work requests have their wr_id set, but it seems
     * that the wr_id of each completed work request will be checked
     * against to see if there are any pending invalidate WRs.
     */
    //设置工作请求ID，包含消息ID和挂起消息计数。 wr_id的高16位表示当前待发送消息数，低32位表示当前待发送和待写入消息数
    wr.wr_id = (id & HALF_BITS) + ( (uint64_t)(curr_to_signaled_send_msg & QUARTER_BITS) << 48) + ((uint64_t)(curr_to_signaled_w_r_msg & QUARTER_BITS) << 32);
  }
  //提交发送请求
  struct ibv_send_wr *bad_wr;
  if (ibv_post_send(qp, &wr, &bad_wr)) { //调用ibv_post_send函数将工作请求提交到队列对QP中
    epicLog(LOG_WARNING, "ibv_post_send failed (%d:%s)\n", errno, strerror (errno)); //如果提交失败，记录警告日志并返回错误码
    return -2;
  }

  return len;
}

ssize_t RdmaContext::Send(const void* ptr, size_t len, unsigned int id, bool signaled) {
  return Rdma(IBV_WR_SEND, ptr, len, id, signaled);
}

int RdmaContext::Recv() {return 0;}

ssize_t RdmaContext::Write (raddr dest, raddr src, size_t len, unsigned int id, bool signaled) {
  return Rdma(IBV_WR_RDMA_WRITE, src, len, id, signaled, dest);
}

ssize_t RdmaContext::WriteWithImm(raddr dest, raddr src, size_t len, uint32_t imm, unsigned int id, bool signaled) {
  return Rdma(IBV_WR_RDMA_WRITE_WITH_IMM, src, len, id, signaled, dest, imm);
}

ssize_t RdmaContext::Read(raddr dest, raddr src, size_t len, unsigned int id, bool signaled) {
  return Rdma(IBV_WR_RDMA_READ, src, len, id, signaled, dest);
}

ssize_t RdmaContext::Cas(raddr src, uint64_t oldval, uint64_t newval, unsigned int id, bool signaled) {
  return Rdma(IBV_WR_ATOMIC_CMP_AND_SWP, src, sizeof (uint64_t), id, signaled, nullptr, 0, oldval, newval);
}

void RdmaContext::ProcessPendingRequests(int n) {
  //process pending rdma requests
  int size = pending_requests.size ();
  epicLog(LOG_DEBUG, "pending_requests %d", size);
  //we must iterate all the current pending requests 
  //in order to ensure the original order
  int i = 0, j = -1;

  for (i = 0; i < n && i < size; i++) {
    RdmaRequest& r = pending_requests.front();
    int ret = Rdma(r);
    epicAssert(ret != -1);
    pending_requests.pop();
  }

  for (;i < size; i++) {
    RdmaRequest& r = pending_requests.front();
    pending_requests.push (r);
    pending_requests.pop();
  }
}
/*
 * 功能：处理RDMA发送完成事件；更新发送缓冲区槽状态；更新消息计数和缓冲区状态；返回工作请求的ID
 * 参数：wc：RDMA工作完成项，包含发送完成事件的相关信息。
 * 返回值：id:工作请求的ID，用于标识发送完成的请求
 *
 * */
unsigned int RdmaContext::SendComp(ibv_wc& wc) {
  //提取工作请求ID和消息计数
  unsigned int id = wc.wr_id  & HALF_BITS; //从wc.wr_id提取工作请求的ID和消息计数：id：低32位，用于标识工作请求。
  uint16_t curr_to_signaled_send_msg = wc.wr_id >> 48; //curr_to_signaled_send_msg：高16位，表示当前发送完成的消息数
  uint16_t curr_to_signaled_w_r_msg = wc.wr_id >> 32 & QUARTER_BITS; //curr_to_signaled_w_r_msg：中间16位，表示当前写完成的消息数

  epicLog(LOG_DEBUG, "id = %u, %u, %u", id, curr_to_signaled_send_msg, curr_to_signaled_w_r_msg); //记录日志，输出提取的值
  //更新发送缓冲区槽状态
  slot_tail += curr_to_signaled_send_msg; //更新发送缓冲区槽的尾部索引slot_tail：增加当前发送完成的消息数。slot_tail：发送缓冲区槽的尾部索引，指向已完成的发送缓冲区槽
  if (slot_tail >= max_pending_msg) slot_tail -= max_pending_msg; //如果slot_tail超过缓冲区的最大消息数max_pending_msg，则循环回到缓冲区的起始位置
  //更新消息计数
  to_signaled_send_msg -= curr_to_signaled_send_msg; //to_signaled_send_msg：减少当前发送完成的消息数。to_signaled_send_msg：待发送完成事件的消息数
  to_signaled_w_r_msg -= curr_to_signaled_w_r_msg; //to_signaled_w_r_msg：减少当前写完成的消息数。to_signaled_w_r_msg：待写完成事件的消息数
  pending_msg -= (curr_to_signaled_send_msg + curr_to_signaled_w_r_msg); //pending_msg：减少当前发送完成和写完成的消息数。pending_msg：挂起的消息总数
  pending_send_msg -= curr_to_signaled_send_msg; //pending_send_msg：减少当前发送完成的消息数。pending_send_msg：挂起的发送消息数

  //断言检查：检查消息计数是否符合预期
  //当前发送完成和写完成的消息数不超过最大未发送完成事件的消息数max_unsignaled_msg
  epicAssert(curr_to_signaled_send_msg + curr_to_signaled_w_r_msg <= max_unsignaled_msg);
  //已发送和已写完成的消息数不超过挂起的消息数pending_msg
  epicAssert(to_signaled_send_msg + to_signaled_w_r_msg <= pending_msg);
  //更新缓冲区满状态：如果缓冲区之前处于满状态(full=true)，且当前有发送完成的消息，则将缓冲区状态更新为非满状态
  if (full && curr_to_signaled_send_msg) full = false;  //full：缓冲区是否已满的标志
  //检查挂起请求队列：确保挂起的请求队列pending_requests已清空
  epicAssert(pending_requests.empty());
  //返回工作请求ID
  return id;
}

unsigned int RdmaContext::WriteComp(ibv_wc& wc) {
  return SendComp(wc);
}

char* RdmaContext::RecvComp(ibv_wc& wc) {
  //FIXME: thread-safe
  //what if others grab this slot before the current thread finish its job
  char* ret = resource->GetSlot(wc.wr_id);
  resource->ClearSlot(wc.wr_id);
  return ret;
}

RdmaContext::~RdmaContext() {
  ibv_destroy_qp(qp);
  ibv_dereg_mr(send_buf);
  zfree(send_buf->addr);
  zfree(msg);
}
