#include "pgasapi.h"
#include <thread>
#include <mutex>

static const Conf* conf = nullptr;
static std::mutex init_lock;
GAlloc** alloc;
static int no_thread = 0;

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

GAddr dsmMalloc(Size size) {
    thread_local GAlloc* allocator = GAllocFactory::CreateAllocator();
    return allocator->Malloc(size);
}

int dsmRead(GAddr addr, void* buf, Size count) {
    thread_local GAlloc* allocator = GAllocFactory::CreateAllocator();
    return allocator->Read(addr, buf, count);
}

int dsmWrite(GAddr addr, void* buf, Size count) {
    thread_local GAlloc* allocator = GAllocFactory::CreateAllocator();
    return allocator->Write(addr, buf, count);
}

void dsmFree(GAddr addr) {
    thread_local GAlloc* allocator = GAllocFactory::CreateAllocator();
    allocator->Free(addr);
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