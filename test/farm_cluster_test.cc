// Copyright (c) 2018 The GAM Authors 

#include <thread>
#include <ctime>
#include <atomic>
#include <stdlib.h>
#include <unistd.h>
#include <iostream>
#include <fstream>
#include <cstring>
#include <mutex>
#include <cstdlib>
#include "zmalloc.h"
#include "util.h"
#include "gallocator.h"
#include <sys/time.h>

#define POPULATE 1  
#define TEST 2
#define DONE 3  //定义了三个宏，分别代表添加数据、测试和完成

#define DEBUG_LEVEL LOG_INFO

int is_master = 0;
string ip_master = get_local_ip("eth0");
string ip_worker = get_local_ip("eth0");
int port_master = 12345;
int port_worker = 12346;
int obj_size = 100;
int num_obj = 1000000;
int no_thread = 10;
int no_node = 1;
int node_id = 0;
int write_ratio = 50;
int sync_key; 
int iteration = 5000;
int txn_nobj = 40;
Conf conf;

GAlloc** alloc; //多个GAlloc对象的指针，保存在一个数组里。定义了一个指向GAlloc类指针的指针，用于保存多个GAlloc对象的地址，每个线程一个GAlloc对象，用于分配内存。这种定义通常用于需要动态分配和管理多个GAlloc对象的场景，例如可以用它来创建一个指针数组，每个元素指向一个GAlloc对象。
char *v; //定义一个字符指针，用于指向动态分配的字符数组

static void sync(int phase, int tid) {  //定义一个静态函数sync，参数为阶段phase和线程ID
  fprintf(stdout, "Thread %d leaves phase %d, start snyc now\n", tid, phase); //打印线程离开上一阶段并开始同步的信息
  uint64_t base = num_obj * no_node * no_thread; //计算base的值，基于对象数量、节点数量和线程数量
  sync_key = base + node_id * no_thread + tid;  //计算同步键，基于base、节点ID和线程ID
  alloc[tid]->Put(sync_key, &phase, sizeof(int)); //将当前阶段值存储到同步键位置
  if (phase != DONE || is_master) //如果阶段不是DONE或当前节点不是主节点
    for (int i = 0; i < no_thread * no_node; ++i) //遍历所有线程和节点
    {
      //if (i == node_id * no_thread + tid) continue;
      int t; //定义一个整形变量t
      do {//开始一个循环
        alloc[tid]->Get(base + i, &t); //从base地址加i位置获取阶段值并存储到t
        if (t == DONE) break; //如果t等于DONE，跳出循环
        usleep(50000); //休眠50毫秒
      } while(t != phase); //如果t不等于当前阶段，继续循环
    }
}


static void farm_test(int tid) { // 定义一个静态函数 farm_test，参数为线程 ID (tid)
  int total = num_obj * no_node * no_thread;// 计算总对象数，基于对象数量、节点数量和线程数量
  char t[obj_size]; // 定义一个字符数组 t，大小为 obj_size
  srand(node_id);  //将node_id作为生成随机数的种子，每个线程生成的随机数序列将是相同的。这对于在不同节点上生成一致的随机数序列或在调试时重现相同的随机数序列非常有用。

  GAddr *a = new GAddr[total];// 动态分配一个大小为 total 的 GAddr 数组，并将指针赋值给 a
  for (int i = 0; i < total; ++i)// 循环 total 次
  {
    if(alloc[tid]->Get(i, &a[i]) == -1) // 从分配器获取地址，如果失败
    {
      fprintf(stdout, "tid %d, i %d\n", tid, i);// 打印线程 ID 和索引
      exit(0); //退出程序
    }
    int wid = a[i]>>48;// 取出地址的高 16 位作为 wid
    if (wid <= 0 || wid > no_node) {  // 检查 wid 是否在有效范围内
      fprintf(stdout, "tid %d, i %d, addr %lx\n", tid, i, a[i]); // 打印线程 ID、索引和地址
      assert(wid > 0 && wid <= no_node); // 如果 wid 不在有效范围内，则触发断言
    }
  }

  fprintf(stdout, "Thread %d start testing now\n", tid); //打印线程开始测试的信息
  uint64_t nr_commit = 0; //定义并初始化提交计数器
  uint64_t nr_abort = 0; //定义并初始化终止计数器
  struct timeval start, end; //定义时间结构体
  gettimeofday(&start, NULL);//获取当前时间并存储到start
  for (int j = 0; j < iteration; ++j) //循环iteration次
  {
    //if (j % 10 == 0)
    //    fprintf(stdout, "Thread %d: iteration %d\n", tid, j);

    alloc[tid]->txBegin(); //开始一个事务
    for (int i = 0; i < txn_nobj; ++i) //循环txn_nobj次
    {
      int r = rand(); //生成一个随机数
      //alloc[tid]->txRead(a[r%total], v, obj_size);
      if (r % 100 < write_ratio) //如果随机数模100小于写入比例
        alloc[tid]->txWrite(a[r%total], v, obj_size); //执行写操作
      else {
        alloc[tid]->txRead(a[r%total], t, obj_size); //执行读操作
        //assert(strncmp(v, t, obj_size) == 0);
        //memset(t, 0, obj_size);
      }
    }

    if (alloc[tid]->txCommit() == SUCCESS) //尝试提交事务，如果成功
      nr_commit++; //增加提交计数器
    else nr_abort++; //增加终止计数器
  }

  gettimeofday(&end, NULL); //获取当前时间并存储到end
  double time = 1000000 * (end.tv_sec - start.tv_sec) + end.tv_usec - start.tv_usec; //计算运行时间，单位为微妙
  fprintf(stdout, "time:%f s, nr_commit = %ld, nr_abort = %ld, ratio = %f, thruput = %f\n", 
      (time)/1000000, //打印运行时间，单位为秒
      nr_commit, //打印提交计数
      nr_abort, //打印终止计数
      (float)nr_commit/(nr_commit + nr_abort), //打印提交比例
      (nr_abort + nr_commit)*1000000/time); //打印吞吐量

  alloc[tid]->txCommit(); //提交事务

  delete a; //释放动态分配的GAddr数组
  sync(TEST, tid); //同步操作，传入TEST和线程ID
  sync(DONE, tid); //同步操作，传入DONE和线程ID
}

/*在分布式系统或某些特定的内存管理方案中，地址的高位部分通常用于存储节点或区域的标识符，而低位部分用于存储具体的偏移量或地址。
因此地址的高位部分大于0可以用来确保地址包含有效的节点或区域标识符，从而认为该地址是有效的。*/
static void populate(int tid) {   //添加数据，输入数据，定义一个静态函数populate，参数为线程ID
  v = new char[obj_size];  //动态分配obj_size大小的字符数组，并将指针赋值给v
  memset(v, 0, obj_size);  //将 v 指向的内存区域的前 obj_size 个字节全部填充为 0

  fprintf(stdout, "thread %d start populate\n", tid);  //打印线程开始populate的信息
  GAddr* a = new GAddr[num_obj];//动态分配一个大小为num_obj的GAddr数组，并将指针赋值给a
  alloc[tid]->txBegin();//开始一个事物
  for (int i = 0; i < num_obj; ++i) //循环num_obj次
  {
    a[i] = alloc[tid]->txAlloc(obj_size); //为每个对象分配大小为obj_size的内存，并将地址存储在a[i]中
    int wid = a[i]>>48;  //取出地址的高16位作为wid
    if (wid <= 0 || wid > no_node)  //检查wid是否在有效范围内 ***** 如何确保wid在有效范围内
      assert(wid > 0 && wid <= no_node);  //如果wid不在有效范围内，则触发断言
    alloc[tid]->txWrite(a[i], v, obj_size); //将v指向的数据写入到a[i]指向的内存区域中
  }

  if (!alloc[tid]->txCommit()) { //尝试提交事物，如果失败
    for (int i = 0; i < num_obj; ++i) //循环num_obj次
    { //将a[i]存储到某个位置，如果失败
      if(-1 == alloc[tid]->Put(node_id * num_obj * no_thread + tid * num_obj + i, &a[i], sizeof(GAddr)))
        exit(0); //退出程序
    }
  } else { //如果事务提交成功
    exit(0); //直接退出程序
  }
  sync(POPULATE, tid); //同步操作，传入POPULATE和线程ID
  delete a; //释放动态分配的GAddr数组
}

int main(int argc, char* argv[]) {
  //the first argument should be the program name
  for(int i = 1; i < argc; i++) {
    if(strcmp(argv[i], "--ip_master") == 0) {
      ip_master = string(argv[++i]);
    } else if(strcmp(argv[i], "--ip_worker") == 0) {
      ip_worker = string(argv[++i]);
    } else if (strcmp(argv[i], "--port_master") == 0) {
      port_master = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--iface_master") == 0) {
      ip_master = get_local_ip(argv[++i]);
    } else if (strcmp(argv[i], "--port_worker") == 0) {
      port_worker = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--iface_worker") == 0) {
      ip_worker = get_local_ip(argv[++i]);
    } else if (strcmp(argv[i], "--iface") == 0) {
      ip_worker = get_local_ip(argv[++i]);
      ip_master = get_local_ip(argv[i]);
    } else if (strcmp(argv[i], "--is_master") == 0) {
      is_master = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--obj_size") == 0) {
      obj_size = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--num_obj") == 0) {
      num_obj = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--no_node") == 0) {
      no_node = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--node_id") == 0) {
      node_id = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--write_ratio") == 0) {
      write_ratio = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--iteration") == 0) {
      iteration = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--txn_nobj") == 0) {
      txn_nobj = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--no_thread") == 0) {
      no_thread = atoi(argv[++i]);
    } else {
      fprintf(stdout, "Unrecognized option %s for benchmark\n", argv[i]);
    }
  }

  conf.loglevel = DEBUG_LEVEL;
  conf.is_master = is_master;
  conf.master_ip = ip_master;
  conf.master_port = port_master;
  conf.worker_ip = ip_worker;
  conf.worker_port = port_worker;
  conf.size = 1024 * 1024 * 1024;

  /*动态分配一个指针数组，该数组包含no_thread个指向GAlloc对象的指针。
  alloc是一个GAlloc**类型的变量，它指向这个指针数组的起始位置。
  这样做的目的是为了能够动态管理多个GAlloc对象，例如，可以在后续代码中为每个元素分配一个GAlloc对象
  **在主函数中，每个线程都会调用CreateAllocator来创建一个GAlloc分配器。由于CreateAllocator函数使用了
  静态变量和互斥锁来确保线程安全，并且只会初始化一次Master和Worker对象，因此对于单个节点来说，无论有多少个线程调用CreateAllocator
  都只会创建一个Master和一个Worker对象。*/
  alloc = new GAlloc*[no_thread];
  for (int i = 0; i < no_thread; ++i)
  {
    alloc[i] = GAllocFactory::CreateAllocator(&conf);
    //为每个指针数组元素分配一个GAlloc对象，并将其地址存储到alloc[i]中
    //&conf是一个Conf*类型的指针，它指向一个Conf对象，即表示conf对象的地址，用于初始化GAlloc对象
  }

  sleep(2);

  thread* th[no_thread];
  for (int i = 0; i < no_thread; ++i)
  {
    th[i] = new thread(populate, i);
  }

  for (int i = 0; i < no_thread; ++i)
  {
    th[i]->join();
  }

  for (int i = 0; i < no_thread; ++i)
  {
    th[i] = new thread(farm_test, i);
  }

  for (int i = 0; i < no_thread; ++i)
  {
    th[i]->join();
  }
}
