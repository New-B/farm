// Copyright (c) 2018 The GAM Authors 

#ifndef INCLUDE_WORKREQUEST_H_
#define INCLUDE_WORKREQUEST_H_

#include <type_traits>
#include "structure.h"

enum Work { //定义工作类型，定义了各种工作请求类型，包括内存统计、读写操作、事务操作等
  FETCH_MEM_STATS = 1,
  UPDATE_MEM_STATS,
  BROADCAST_MEM_STATS,
  PUT,
  GET,
  KV_PUT,
  KV_GET,
  FARM_MALLOC,
  FARM_READ,
  PREPARE,
  VALIDATE,
  COMMIT,
  ABORT,
  //set the value of REPLY so that we can test op & REPLY
  //to check whether it is a reply workrequest or not
  REPLY = 1 << 16,  //REPLY及其后续值用于标识恢复类型的工作请求。
  FARM_MALLOC_REPLY,
  FARM_READ_REPLY,
  VALIDATE_REPLY,
  PREPARE_REPLY,
  ACKNOWLEDGE,
  FETCH_MEM_STATS_REPLY,
  GET_REPLY,
  PUT_REPLY,
};

enum Status {//定义了各种状态码，用于表示工作请求的结果
  SUCCESS = 0,
  ALLOC_ERROR,
  READ_SUCCESS,
  READ_ERROR,
  WRITE_ERROR,
  UNRECOGNIZED_OP,
  LOCK_FAILED,
  PREPARE_FAILED,
  VALIDATE_FAILED,
  COMMIT_FAILED,
  NOT_EXIST
};


class TxnContext; //类的前向声明
using wtype = std::underlying_type<Work>::type;//分别定义为Work和Status枚举的底层类型
using stype = std::underlying_type<Status>::type;
const char* workToStr(Work);//函数声明，用于将Work枚举转换为字符串

using Flag = int; //定义Flag为int类型

#define REMOTE 1  //定义了一些标志，用于标识工作请求的属性
#define SPREAD 1 << 1
#define CACHED 1 << 2 
#define ASYNC 1 << 3
#define REPEATED 1 << 4
#define REQUEST_DONE 1 << 5
#define LOCKED 1 << 6
#define TRY_LOCK 1 << 7
#define TO_SERVE 1 << 8
#define ALIGNED 1 << 9

#define MASK_ID 1 << 0 //定义了一些掩码，用于标识工作请求的属性
#define MASK_OP 1 << 1
#define MASK_ADDR 1 << 2
#define MASK_FREE 1 << 3
#define MASK_SIZE 1 << 4
#define MASK_STATUS 1 << 5
#define MASK_FLAG 1 << 6
#define MASK_PTR 1 << 7
#define MASK_FD 1 << 8
#define MASK_WID 1 << 9
#define MASK_COUNTER 1 << 10

/*
 * TODO: try to shrink the size of WorkRequest structure
 * use union?
 * 结构体用于表示一个工作请求，包含了处理请求所需的各种信息。
 * 包括请求的标识符、操作类型、相关数据（键、地址、大小等）、状态码、标志、指针、文件描述符、用纸缓冲区、条件变量等。
 * 1.请求标识：id,pid,pwid用于标识工作请求及其父请求和父工作节点。
 * 2.操作类型：op表示工作请求的操作类型，如读、写、分配内存等。
 * 3.数据存储：联合体key、addr、free用于存储与请求相关的数据。
 * 4.状态和标志：status和flag用于 表示请求的状态和标志。
 * 5.同步机制：notify_buf，cond_lock，cond用于实现请求的同步机制。
 * 6.链表结构：parent和next用于将工作请求组织成链表结构，便于管理和处理。
 * 7.序列化和反序列化：Ser和Deser用于将工作请求WorkRequest序列化和反序列化，便于在网络上传输或持久化存储。
 * 通过这些设计WorkRequest结构体可以灵活地表示和处理各种工作请求，支持多种操作类型和数据存储方式，并提供了同步机制和链表结构，便于在多线程和分布式环境中使用。
 * 以下是对WorkerRequest结构体的详细解释：
 */
struct WorkRequest {
  uint32_t id;  //工作请求的唯一标识符ID
  union { //nobj和tx联合体，分别表示对象数量和事务上下文指针
    uint32_t nobj;
    TxnContext* tx;
  };

  unsigned int pid; //identifier of the parent work request (used for FORWARD request)  //父工作请求的标识符（用于FORWARD请求）
  int pwid; //identifier of the parent worker //父工作节点的标识符
  enum Work op; //工作请求的操作类型

  union { //key、addr、free联合体，分别表示键、地址、空闲内存大小
    uint64_t key;
    GAddr addr;
    Size free;
  };
  Size size;  //大小
  int status; //状态码

  Flag flag;  //标志
  void* ptr;  //指针

  int fd; //file descriptor  //文件描述符
#if	!(defined(USE_PIPE_W_TO_H) && defined(USE_PIPE_H_TO_W))
  volatile int* notify_buf; //notification buffer  //通知缓冲区 （如果未使用管道）
#endif
#ifdef USE_PTHREAD_COND
  pthread_mutex_t* cond_lock; //condition lock  //条件锁 条件变量的互斥锁和条件变量（如果使用pthread条件变量
  pthread_cond_t* cond; //条件变量
#endif

  int wid;  //worker id  //工作节点的标识符

  int counter; //maybe negative in Write Case 4 //在写入情况4中可能为负数 //计数器

  WorkRequest* parent;  //parent work request  //父工作请求 指向父工作请求的指针
  WorkRequest* next; //下一个工作请求，指向下一个工作请求的指针

  //构造函数，初始化工作请求，初始化WorkRequest对象的成员变量
  WorkRequest(): fd(), id(-1), pid(), pwid(), op(), addr(), size(), status(),
  flag(), ptr(), wid(), counter(), parent(), next() {
#if !(defined(USE_PIPE_H_TO_W) && defined(USE_PIPE_W_TO_H))
    notify_buf = nullptr;
#endif
  };  
  WorkRequest(WorkRequest& wr); //拷贝构造函数，用于创建对象的副本
  bool operator==(const WorkRequest& wr); //重载==运算符，比较运算符，比较两个WorkRequest对象是否相等
  int Ser(char* buf, int& len); //序列化函数，将WorkRequest对象序列化为字符串
  int Deser(const char* buf, int& len); //反序列化函数，将字符串反序列化为WorkRequest对象

  ~WorkRequest(); //析构函数，释放资源
};


#endif /* INCLUDE_WORKREQUEST_H_ */
