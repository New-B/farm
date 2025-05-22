#include <iostream>
#include <string>
#include <cstdlib>
#include <cstring>
#include "gallocator.h"
#include "dsmapi.h"
#include "util.h"

using namespace std;

#define DEBUG_LEVEL LOG_DEBUG

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
GAlloc** alloc;

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
    conf.loglevel = DEBUG_LEVEL;
    conf.is_master = is_master;
    conf.master_ip = ip_master;
    conf.master_port = port_master;
    conf.worker_ip = ip_worker;
    conf.worker_port = port_worker;
    conf.size = 1024 * 1024 * 1024; // 1GB

    // 系统初始化
    InitSystem(&conf);
    sleep(2);
    // // 分配 GAlloc 对象数组
    // alloc = new GAlloc*[no_thread];
    // for (int i = 0; i < no_thread; ++i) {
    //     alloc[i] = GAllocFactory::CreateAllocator(&conf);
    // }

    cout << "System initialized successfully!" << endl;
}

int main(int argc, char* argv[]) {
    // 初始化系统
    parse_conf(argc, argv);

    // 其他逻辑可以在这里继续实现，例如启动线程、执行任务等
    // 示例：打印配置信息
    cout << "Master IP: " << ip_master << endl;
    cout << "Worker IP: " << ip_worker << endl;
    cout << "Node ID: " << node_id << endl;
    cout << "Number of Threads: " << no_thread << endl;
    cout << "Object Size: " << obj_size << endl;
    cout << "Number of Nodes: " << no_node << endl;

    // 清理资源
    for (int i = 0; i < no_thread; ++i) {
        delete alloc[i];
    }
    delete[] alloc;

    return 0;
}