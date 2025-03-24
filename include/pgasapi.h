#ifndef PGASAPI_H_
#define PGASAPI_H_

#include "gallocator.h"

#ifdef __cplusplus
extern "C" {
#endif

// static void InitSystem(const std::string& conf_file);

void InitSystem(const Conf* c = nullptr);

// 分配内存
GAddr dsmMalloc(Size size);

// 读取数据
int dsmRead(GAddr addr, void* buf, Size count);

// 写入数据
int dsmWrite(GAddr addr, void* buf, Size count);

// 释放内存
void dsmFree(GAddr addr);

void dsm_finalize();

#ifdef __cplusplus
}
#endif

#endif // PGASAPI_H_