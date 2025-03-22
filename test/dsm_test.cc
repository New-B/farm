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
#include "dsmapi.h"
#include "gallocator.h"
#include <sys/time.h>

#define NUM_THREADS 4
#define ALLOC_SIZE 1024

#define DEBUG_LEVEL LOG_INFO

int is_master = 1;
string ip_master = "172.16.33.30";
string ip_worker = "172.16.33.32";
int port_master = 12345;
int port_worker = 12346;
int obj_size = 100;
int num_obj = 1000000;
int no_thread = 4;
int no_node = 1;
int node_id = 0;
int write_ratio = 50;
int sync_key; 
int iteration = 10000;
int txn_nobj = 40;
Conf conf;

void* threadFunc(void* arg) {
    long thread_id = (long)arg;
    printf("Thread %ld started\n", thread_id);

    // 分配内存
    GAddr addr = dsmMalloc(ALLOC_SIZE);
    if (addr == Gnullptr) {
        fprintf(stderr, "Thread %ld: dsmMalloc failed\n", thread_id);
        pthread_exit(NULL);
    }

    // 写入数据
    char data[ALLOC_SIZE];
    snprintf(data, ALLOC_SIZE, "Hello from thread %ld", thread_id);
    if (dsmWrite(addr, data, ALLOC_SIZE) != 0) {
        fprintf(stderr, "Thread %ld: dsmWrite failed\n", thread_id);
        pthread_exit(NULL);
    }

    // 读取数据
    char buffer[ALLOC_SIZE];
    if (dsmRead(addr, buffer, ALLOC_SIZE) != 0) {
        fprintf(stderr, "Thread %ld: dsmRead failed\n", thread_id);
        pthread_exit(NULL);
    }
    printf("Thread %ld read data: %s\n", thread_id, buffer);

    // 释放内存
    dsmFree(addr);

    pthread_exit(NULL);
}

int main(int argc, char* argv[]) {
    //the first argument should be the program name
    ///sharenvme/usershome/wangbo/projectsFromPapers/farm/gam/test/farm_cluster_test 
    //--ip_master 172.16.33.30 --no_node 2 --no_thread 4 --write_ratio 50 --num_obj 100000 --obj_size 100 --iteration 10000 --txn_nobj 40 --ip_worker 172.16.33.32 --node_id 1 --is_master 0
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

    InitSystem(&conf);

    pthread_t threads[NUM_THREADS];
    for (long i = 0; i < NUM_THREADS; i++) {
        int rc = pthread_create(&threads[i], NULL, threadFunc, (void*)i);
        if (rc) {
            fprintf(stderr, "Error: unable to create thread, %d\n", rc);
            exit(-1);
        }
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        int rc = pthread_join(threads[i], NULL);
        if (rc) {
            fprintf(stderr, "Error: unable to join thread, %d\n", rc);
        }
    }

    // 释放分布式内存系统资源
    dsm_finalize();

    return 0;


}