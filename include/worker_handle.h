// Copyright (c) 2018 The GAM Authors 
//文件定义了WorkerHandle类及其相关的方法和成员变量
#ifndef INCLUDE_WORKER_HANDLE_H_
#define INCLUDE_WORKER_HANDLE_H_

#include <mutex>
#include "worker.h"
#include "workrequest.h"
/*WorkerHandle类用于管理与工作节点进行通信。他提供了一些方法来发送工作请求、注册和取消注册线程。并维护了与工作节点通信所需的资源。
1.工作队列：wqueue是一个无锁队列，用于存储和传递工作请求。
2.工作节点指针：worker指向关联的工作节点对象。
3.管道文件描述符：send_pipe和recv_pipe用于在应用线程和工作线程之间传递数据。
4.线程同步：lock是一个互斥锁，用于线程同步。hcond_loc和cond是条件变量的互斥锁和条件变量，用于线程间的同步（如果使用pthread条件变量）。
5.通知缓冲区：notify_buf和notify_buf_size用于通知缓冲区，用于在未使用Boost队列和管道时通知工作节点。
* */
class WorkerHandle {
  boost::lockfree::queue<WorkRequest*>* wqueue; //work queue used to communicate with worker 工作队列，用于与工作节点通信的工作队列
  Worker* worker; //指向工作节点的指针
  //app-side pipe fd  应用程序端的管道文件描述符
  int send_pipe[2]; //app thread to worker thread 用于应用线程到工作线程的管道文件描述符 应用程序端的发送管道文件描述符
  int recv_pipe[2]; //worker thread to app thread 用于工作线程到应用线程的管道文件描述符 应用程序端的接收管道文件描述符
  static mutex lock;  //静态互斥锁，用于线程同步  
#ifdef USE_PTHREAD_COND
  pthread_mutex_t cond_lock;  //条件变量的互斥锁  （如果使用pthread条件变量）
  pthread_cond_t cond;  //条件变量  （如果使用pthread条件变量）
#endif
#if !defined(USE_PIPE_W_TO_H) || (!defined(USE_BOOST_QUEUE) && !defined(USE_PIPE_H_TO_W))
  volatile int* notify_buf; //通知缓冲区（如果未使用Boost队列和管道）
  int notify_buf_size;  //通知缓冲区大小  （如果未使用Boost队列和管道）
#endif
public:
  WorkerHandle(Worker* w);  //构造函数，初始化WorkerHandle对象，设置工作节点指针
  void RegisterThread();  //注册线程
  void DeRegisterThread();  //取消注册线程
  int SendRequest(WorkRequest* wr); //发送工作请求
  inline int GetWorkerId() {return worker->GetWorkerId();}  //获取工作节点ID
  ~WorkerHandle();  //析构函数，释放资源
};



#endif /* INCLUDE_WORKER_HANDLE_H_ */
