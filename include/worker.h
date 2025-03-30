// Copyright (c) 2018 The GAM Authors 
//文件定义了一个与工作节点相关的Worker类及其工厂类WorkerFactory
#ifndef INCLUDE_WORKER_H_
#define INCLUDE_WORKER_H_

#include <boost/lockfree/queue.hpp>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <queue>
#include <thread>
#include <utility>
#include <mutex>
#include <atomic>
#include <list>
#include "settings.h"
#include "structure.h"
#include "client.h"
#include "workrequest.h"
#include "server.h"
#include "ae.h"
#include "slabs.h"

#include "farm_txn.h"

/*该结构体的设计意义在于管理和跟踪分布式事务的提交状态，它在分布式系统中用于协调多个工作节点之间的事务提交过程
在分布式事务中，常用的提交协议是两阶段提交协议，TxnCommitStatus可以很好地支持这一协议：
第一阶段（准备阶段）：每个工作节点执行事务操作并返回准备状态；progress_用于记录每个节点是否已准备好；remaining_workers_用于跟踪尚未准备返回状态的节点数量。
第二阶段（提交阶段）：若所有节点都返回准备状态，则协调者通知所有节点提交事务；若有任何节点返回失败，则协调者通知所有节点回滚事务；success记录事务是否成功。*/
struct TxnCommitStatus{
  std::unordered_map<uint16_t, int> progress_;  //worker id to progress 记录每个工作节点的事务提交进度，键：工作节点id；值：该节点的事务提交进度
  int remaining_workers_; //记录当前事务中尚未完成事务的工作节点数量，为0时，表示所有几点都已完成事务提交，可以进行下一步操作
  int success;  //事务是否成功标志
  bool local; //是否为本地事务的标志
};
//这些宏定义了请求的类型和标志
#define REQUEST_WRITE_IMM 1
#define REQUEST_SEND 1 << 1
#define REQUEST_READ 1 << 2
#define REQUEST_SIGNALED 1 << 3
#define REQUEST_NO_ID 1 << 4
#define ADD_TO_PENDING 1 << 5

class Worker: public Server { //Worker类继承自Server类，表示工作节点服务器  

  //the handle to the worker thread
  thread* st; //服务线程

  /*
   * TODO: two more efficient strategies
   * 1) use pipe for each thread directly to transfer the pointer (then wqueue is not needed)
   * -- too many wakeups?
   * 2) use a single pipe for all the threads (thread->worker), and process all the requests once it is waked up
   * -- too much contention in the single pipe?
   * NOTE: both strategies require one pipe for each thread in (worker->thread) direction
   * in order to wake up individual thread
   */
  boost::lockfree::queue<WorkRequest*>* wqueue; //work queue used to communicate with local threads 工作队列，用于与本地线程通信
  Client* master; //指向主节点的客户端
  unordered_map<int, int> pipes; //worker pipe fd to app thread pipe fd 工作节点管道文件描述符到应用线程管道文件描述符的映射
  unsigned int wr_psn; //we assume the pending works will not exceed INT_MAX 假设待处理的工作不会超过INT_MAX

  /*
   * the pending work requests from remote nodes
   * because some states are in intermediate state
   */
  unordered_map<GAddr, queue<pair<Client*, WorkRequest*>>> to_serve_requests; //待处理的远程节点请求

  /*
   * the pending work requests from local nodes
   * because some states are in intermediate state
   */
  unordered_map<GAddr, queue<WorkRequest*>> to_serve_local_requests; //待处理的本地节点请求

  void* base; //base addr 基地址
  Size size;  //大小

  Size ghost_size; //the locally allocated size that is not synced with Master  本地分配但未与主节点同步的大小

#ifndef USE_BOOST_QUEUE
  list<volatile int*> nbufs;  //通知缓冲区列表（如果未使用Boost队列）
#endif

#ifdef DEBUG
  map<GAddr, Size> alloc; //分配的内存映射（仅在调试模式下）
#endif

  std::unordered_map<uint32_t, TxnContext*> local_txns_;  //本地事务上下文映射
  std::unordered_map<uint32_t, std::unique_ptr<TxnCommitStatus>> tx_status_;  //事务提交状态映射

  /* contain for each client the farm requests/replies that are ready to 
   * send to this client
   */
  std::unordered_map<Client*, std::list<TxnContext*>> client_tasks_;  //每个客户端的任务列表

  /* record the TxnContext information for each remote txn; 
   * only used for comit phase
   * */
  std::unordered_map<uint64_t, std::unique_ptr<TxnContext>> remote_txns_;//远程事务上下文映射
  std::unordered_map<uint64_t, uint32_t> nobj_processed;  //处理的对象数量映射

  unordered_map<uint64_t, pair<void*, Size>> kvs; //键值存储
//这些方法用于处理事务的提交、验证、提交或中止、远程请求处理、内存分配等
  int FarmSubmitRequest(Client* cli, WorkRequest* wr);  //提交工作请求给客户端cli

  void FarmAddTask(Client*, TxnContext*); //添加任务到客户端的任务列表

  /* prepare local transaction */
  void FarmPrepare(TxnContext*, TxnCommitStatus*);  //准备事务上下文和提交状态
  void FarmPrepare(Client* c, TxnContext*);//为客户端c准备事务上下文
  void FarmResumePrepare(TxnContext*, TxnCommitStatus* = nullptr); //恢复准备事务

  /* validate local transaction */
  void FarmValidate(TxnContext*, TxnCommitStatus*);//验证事务上下文和提交状态
  void FarmValidate(Client* c, TxnContext*);  //为客户端c验证事务上下文
  void FarmResumeValidate(TxnContext*, TxnCommitStatus* = nullptr);//恢复验证事务

  /* commit/abort local transactions */
  void FarmCommitOrAbort(TxnContext*, TxnCommitStatus*); //提交或终止事务上下文和提交状态
  void FarmCommitOrAbort(Client* c, TxnContext*); //为客户端c提交或终止事务上下文
//完成事务
  void FarmFinalizeTxn(TxnContext*, TxnCommitStatus*);//完成事务上下文和提交状态
  void FarmFinalizeTxn(Client*, TxnContext*); //为客户端完成事务上下文
//处理远程请求
  void FarmProcessRemoteRequest(Client*, const char* msg, uint32_t len);
  void FarmProcessPrepare(Client*, TxnContext*);//处理准备事务请求
  void FarmProcessValidate(Client*, TxnContext*);  //处理验证事务请求
  void FarmProcessCommit(Client*, TxnContext*);//处理提交事务请求
  void FarmProcessAbort(Client*, TxnContext*);  //处理中止事务请求
  void FarmProcessPrepareReply(Client*, TxnContext*); //处理准备事务回复
  void FarmProcessValidateReply(Client*, TxnContext*);  //处理验证事务回复
  void FarmProcessAcknowledge(Client*, TxnContext*);  //处理确认消息事务
  void FarmProcessMalloc(Client*, TxnContext*); //处理内存分配请求
  void FarmProcessMallocReply(Client*, TxnContext*);  //处理内存分配请求的回复
  void FarmProcessRead(Client*, TxnContext*); //处理读取请求
  void FarmProcessReadReply(Client*, TxnContext*);  //处理读取请求的回复
  void FarmWrite(std::unordered_map<GAddr, std::shared_ptr<Object>>&);  //写入对象

  void FarmResumeTxn(Client*);  //恢复事务

  void FarmProcessPendingReads(TxnContext*);  //处理待处理的读取请求
  void FarmProcessPendingReads(WorkRequest*);

  /* worker serves as the coodinator for local commit requests */
  int FarmCommit(WorkRequest*); //提交工作请求

  bool FarmAddressLocked(GAddr addr); //判断地址是否被锁
  bool FarmAddressRLocked(GAddr addr);  //判断地址是否被读锁
  bool FarmAddressWLocked(GAddr addr);  //判断地址是否被写锁
  bool FarmRLock(GAddr addr); //对地址进行读锁定
  bool FarmWLock(GAddr addr); //对地址进行写锁定
  void FarmUnRLock(GAddr addr); //解除地址的读锁定
  void FarmUnWLock(GAddr addr); //解除地址的写锁定

  void* FarmMalloc(osize_t, osize_t = 1); //分配内存
  void FarmFree(GAddr); //释放内存
  inline osize_t FarmAllocSize(char* addr) {  //获取分配内存大小
    return *(osize_t*)(addr - sizeof(osize_t));
  }

  void FarmAllocateTxnId(WorkRequest*); //分配事务ID

  public:

  /* process requests PreparePreparePreparePrepareissued by local threads */
  void FarmProcessLocalRequest(WorkRequest*);  //处理本地请求
  void FarmProcessLocalMalloc(WorkRequest*);  //处理本地内存分配请求
  void FarmProcessLocalRead(WorkRequest*); //处理本地读取请求
  void FarmProcessLocalCommit(WorkRequest*);  //处理本地提交请求

  SlabAllocator sb;
  /*
   * 1) init local address and register with the master
   * 2) get a cached copy of the whole picture about the global memory allocator
   */
  Worker(const Conf& conf, RdmaResource* res = nullptr); //构造函数，初始化工作节点服务器
  inline void Join() {st->join();}  //等待服务线程结束

  inline bool IsMaster() {return false;}  //判断是否为主节点，返回false
  inline int GetWorkerId() {return master->GetWorkerId();}  //获取工作节点ID

  /*
   * register the worker handle with this worker
   * return: app thread-side fd
   */
  int RegisterHandle(int fd, aeFileProc* handle = ProcessLocalRequest); //注册处理句柄
  void DeRegisterHandle(int fd); //取消注册处理句柄

#ifndef USE_BOOST_QUEUE
  int RegisterNotifyBuf(volatile int* notify_buf);//注册通知缓冲区（如果未使用Boost队列
  void DeRegisterNotifyBuf(volatile int* notify_buf);//取消注册通知缓冲区（如果未使用Boost队列
#endif

  inline boost::lockfree::queue<WorkRequest*>* GetWorkQ() {return wqueue;}  //获取工作队列
  inline unsigned int GetWorkPsn() {    //获取工作序列号
    ++wr_psn; 
    if (wr_psn == 0) ++wr_psn;
    return wr_psn;
  }

  static void ProcessLocalRequest(aeEventLoop *el, int fd, void *data, int mask); //处理本地请求
  void ProcessRequest(Client*, WorkRequest*) override {} //处理请求（重载

  //post process after connect to master
  int PostConnectMaster(int fd, void* data); //连接到主节点后的处理
  void RegisterMemory(void* addr, Size s);//注册内存

  /*
   * if addr == nullptr, return a random remote client
   * otherwise, return the client for the worker maintaining the addr
   */
  Client* GetClient(GAddr addr = Gnullptr); //获取客户端
  inline bool IsLocal(GAddr addr) {return WID(addr) == GetWorkerId();} //判断地址是否为本地地址
  inline void* ToLocal(GAddr addr) {epicAssert(IsLocal(addr)); return TO_LOCAL(addr, base);}  //将全局地址转化为本地地址
  inline GAddr ToGlobal(void* ptr) {return TO_GLOB(ptr, base, GetWorkerId());} //将本地地址转换为全局地址

  void SyncMaster(Work op = UPDATE_MEM_STATS, WorkRequest* parent = nullptr);//与主节点同步

  static int LocalRequestChecker(struct aeEventLoop *eventLoop, long long id, void *clientData); //本地请求检查器

  int Notify(WorkRequest* wr); //通知请求

  static void StartService(Worker* w);//启动服务

  ~Worker();//析构函数，释放资源
};

class WorkerFactory { //用于创建和管理Worker对象
  static Worker *server;  //静态指针，指向唯一的Worker实例
  public:
  static Server* GetServer() {  //获取Worker实例，如果不存在则抛出异常
    if (server)
      return server;
    else
      throw SERVER_NOT_EXIST_EXCEPTION;
  }
  static Worker* CreateServer(const Conf& conf) {//创建Worker实例，如果已存在则抛出异常
    if (server)
      throw SERVER_ALREADY_EXIST_EXCEPTION;
    server = new Worker(conf);
    return server;
  }
  ~WorkerFactory() {//析构函数，删除Worker实例
    if (server)
      delete server;
  }
};

#endif /* INCLUDE_WORKER_H_ */
