#include <chrono>
#include <iostream>
#include <string>
#include <cstdlib>
#include <cstring>
#include "gallocator.h"
#include "pgasapi.h"
#include "util.h"

using namespace std;

#define DEBUG_LEVEL LOG_INFO
#define ALLOC_SIZE 1024 // 定义分配的内存大小

// 全局变量
int is_master = 0;
string ip_master;
string ip_worker;
int port_master = 12345;
int port_worker = 12346;
int obj_size = 1024;
int num_obj = 1000000;
int no_thread = 4;
int no_node = 2;
int node_id = 0;
Conf conf;
//GAlloc** alloc;

void parse_conf(int argc, char* argv[]) {
    // 解析命令行参数
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ip_master") == 0) {
            ip_master = string(argv[++i]);
        } else if (strcmp(argv[i], "--ip_worker") == 0) {
            ip_worker = string(argv[++i]);
        } else if (strcmp(argv[i], "--port_master") == 0) {
            port_master = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--port_worker") == 0) {
            port_worker = atoi(argv[++i]);
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
        } else if (strcmp(argv[i], "--no_thread") == 0) {
            no_thread = atoi(argv[++i]);
        } else {
            cerr << "Unrecognized option: " << argv[i] << endl;
            exit(EXIT_FAILURE);
        }
    }

    // 配置初始化
    conf.no_node = no_node; 
    conf.loglevel = DEBUG_LEVEL;
    conf.is_master = is_master;
    conf.master_ip = ip_master;
    conf.master_port = port_master;
    conf.worker_ip = ip_worker;
    conf.worker_port = port_worker;
    conf.no_thread = no_thread;
    conf.size = 1024 * 1024 * 1024; // 1GB

    

    // // 分配 GAlloc 对象数组
    // alloc = new GAlloc*[no_thread];
    // for (int i = 0; i < no_thread; ++i) {
    //     alloc[i] = GAllocFactory::CreateAllocator(&conf);
    // }
}

void testLocalMallocFreePerfromance(Size size, int iteration) {
    GAddr* addr = new GAddr[iteration];
    char* buffer = new char[size]();
    //分配内存
    auto startMalloc = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iteration; ++i) {
        addr[i] = dsmMalloc(size);
        if (addr[i] == Gnullptr) {
            cerr << "Error: Failed to allocate memory on iteration " << i << endl;
        }
    }
    auto endMalloc = std::chrono::high_resolution_clock::now();
    int duration = std::chrono::duration_cast<std::chrono::microseconds>(endMalloc - startMalloc).count();
    // 输出分配时间 
    std::cout << "Local Malloc " << iteration << " times for size: " << size
                << " took " << duration << " microseconds." << std::endl;

    //写入数据
    auto startWrite = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iteration; ++i) {
        if (dsmWrite(addr[i], buffer, size) < 0) {
            cerr << "Error: Failed to write data to address " << addr[i] << " on iteration " << i << endl;
        }
    }
    auto endWrite = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast<std::chrono::microseconds>(endWrite - startWrite).count();
    // 输出写入时间
    std::cout << "Local Write " << iteration << " times for size: " << size
                << " took " << duration << " microseconds." << std::endl;
    
    //读取数据
    auto startRead = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iteration; ++i) {
        if (dsmRead(addr[i], buffer, size) < 0) {
            cerr << "Error: Failed to read data from address " << addr[i] << " on iteration " << i << endl;
        }
    }
    auto endRead = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast<std::chrono::microseconds>(endRead - startRead).count();
    // 输出读取时间
    std::cout << "Local Read " << iteration << " times for size: " << size
                << " took " << duration << " microseconds." << std::endl;
   
    //释放内存
    auto startFree = std::chrono::high_resolution_clock::now();          
    for (int i = 0; i < iteration; ++i) {
        dsmFree(addr[i]);
    }
    auto endFree = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast<std::chrono::microseconds>(endFree - startFree).count();
    // 输出释放时间
    std::cout << "Local Free " << iteration << " times for size: " << size
                << " took " << duration << " microseconds." << std::endl;

    delete[] addr;

}

void testRemoteMallocFreePerfromance(Size size, int iteration) {
    GAddr* addr = new GAddr[iteration];
    char* buffer = new char[size]();
    //分配内存
    auto startMalloc = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iteration; ++i) {
        addr[i] = dsmMalloc(size, 2);
        if (addr[i] == Gnullptr) {
            cerr << "Error: Failed to allocate memory on iteration " << i << endl;
        }
    }
    auto endMalloc = std::chrono::high_resolution_clock::now();
    int duration = std::chrono::duration_cast<std::chrono::microseconds>(endMalloc - startMalloc).count();
    // 输出分配时间 
    std::cout << "Remote Malloc " << iteration << " times for size: " << size
                << " took " << duration << " microseconds." << std::endl;

    //写入数据
    auto startWrite = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iteration; ++i) {
        if (dsmWrite(addr[i], buffer, size) < 0) {
            cerr << "Error: Failed to write data to address " << addr[i] << " on iteration " << i << endl;
        }
    }
    auto endWrite = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast<std::chrono::microseconds>(endWrite - startWrite).count();
    // 输出写入时间
    std::cout << "Remote Write " << iteration << " times for size: " << size
                << " took " << duration << " microseconds." << std::endl;
    
    //读取数据
    auto startRead = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iteration; ++i) {
        if (dsmRead(addr[i], buffer, size) < 0) {
            cerr << "Error: Failed to read data from address " << addr[i] << " on iteration " << i << endl;
        }
    }
    auto endRead = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast<std::chrono::microseconds>(endRead - startRead).count();
    // 输出读取时间
    std::cout << "Remote Read " << iteration << " times for size: " << size
                << " took " << duration << " microseconds." << std::endl;
   
    //释放内存
    auto startFree = std::chrono::high_resolution_clock::now();          
    for (int i = 0; i < iteration; ++i) {
        dsmFree(addr[i]);
    }
    auto endFree = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast<std::chrono::microseconds>(endFree - startFree).count();
    // 输出释放时间
    std::cout << "Remote Free " << iteration << " times for size: " << size
                << " took " << duration << " microseconds." << std::endl;

    delete[] addr;

}

int main(int argc, char* argv[]) {
    // 参数解析
    parse_conf(argc, argv);
    // 系统初始化
    InitSystem(&conf);
    cout << "System initialized successfully!" << endl;

    // 其他逻辑可以在这里继续实现，例如启动线程、执行任务等
    // 示例：打印配置信息
    cout << "Master IP: " << ip_master << endl;
    cout << "Worker IP: " << ip_worker << endl;
    cout << "Node ID: " << node_id << endl;
    cout << "Number of Threads: " << no_thread << endl;
    cout << "Object Size: " << obj_size << endl;
    cout << "Number of Nodes: " << no_node << endl;
    sleep(5); // 等待工作节点连接
    // 判断是否为主节点
    if (is_master) {
        
        // 本地内存操作性能测试
        cout << "Latency test of local malloc and free ..." << endl;
        testLocalMallocFreePerfromance(ALLOC_SIZE, 10000);
        cout << "Latency test of local malloc and free finished!" << endl;

        // 远程内存操作性能测试
        cout << "Latency test of remote malloc and free ..." << endl;
        testRemoteMallocFreePerfromance(ALLOC_SIZE, 1000);
        cout << "Latency test of remote malloc and free finished!" << endl;


    } else {
        cout << "This is a worker node, skipping local malloc and free performance test." << endl;
    }
    // 释放资源
    dsm_finalize();

    return 0;
}