#include "pgasapi.h"
#include <thread>
#include <mutex>

static const Conf* conf = nullptr;
static std::mutex init_lock;
static std::mutex map_lock; //用于保护映射表
GAlloc** alloc;
static int no_thread = 0;
stctic std::unordered_map<std::thread::id, int> thread_to_alloc_map; //线程ID到分配器索引的映射表

// void InitSystem(const char* conf_file) {
//     std::lock_guard<std::mutex> guard(init_lock);
//     if (!conf) {
//         conf = GAllocFactory::ParseConf(conf_file);
//         GAllocFactory::SetConf(const_cast<Conf*>(conf));
//     }
// }
void InitSystem(const Conf* c){
    std::lock_guard<std::mutex> guard(init_lock);
    GAllocFactory::InitSystem(c);
    sleep(2);
    no_thread = c->no_thread; //获取线程数
    alloc = new GAlloc*[no_thread];
    for (int i = 0; i < no_thread; ++i) {
        alloc[i] = GAllocFactory::CreateAllocator();
    }
}
// 获取当前线程对应的分配器索引
static int GetAllocIndexForThread() {
    std::thread::id thread_id = std::this_thread::get_id();
    {
        std::lock_guard<std::mutex> guard(map_lock);
        auto it = thread_to_alloc_map.find(thread_id);
        if (it != thread_to_alloc_map.end()) {
            // 如果映射已存在，返回对应的索引
            return it->second;
        }

        // 如果映射不存在，创建新的映射
        int new_index = thread_to_alloc_map.size();
        if (new_index >= no_thread) {
            throw std::runtime_error("Exceeded maximum number of threads");
        }
        thread_to_alloc_map[thread_id] = new_index;
        return new_index;
    }
}

GAddr dsmMalloc(Size size) {
    int index = GetAllocIndexForThread(); // 获取当前线程对应的分配器索引 
    //thread_local GAlloc* allocator = GAllocFactory::CreateAllocator();
    return alloc[index]->Malloc(size);
}

int dsmRead(GAddr addr, void* buf, Size count) {
    int index = GetAllocIndexForThread(); // 获取当前线程对应的分配器索引 
    //thread_local GAlloc* allocator = GAllocFactory::CreateAllocator();
    return alloc[index]->Read(addr, buf, count);
}

int dsmWrite(GAddr addr, void* buf, Size count) {
    int index = GetAllocIndexForThread(); // 获取当前线程对应的分配器索引 
    //thread_local GAlloc* allocator = GAllocFactory::CreateAllocator();
    return alloc[index]->Write(addr, buf, count);
}

void dsmFree(GAddr addr) {
    int index = GetAllocIndexForThread(); // 获取当前线程对应的分配器索引 
    //thread_local GAlloc* allocator = GAllocFactory::CreateAllocator();
    alloc[index]->Free(addr);
}

void dsm_finalize() {
    std::lock_guard<std::mutex> guard(init_lock);
    GAllocFactory::FreeResouce();
    for (int i = 0; i < no_thread; ++i) {
        delete alloc[i];
    }
    delete[] alloc;
    conf = nullptr;
}