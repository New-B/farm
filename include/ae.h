/* A simple event-driven programming library. Originally I wrote this code
 * for the Jim's event-loop (Jim is a Tcl interpreter) but later translated
 * it in form of a library for easy reuse.
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __AE_H__
#define __AE_H__

#include <time.h>
//定义了一些常量，用于表示事件处理的结果
#define AE_OK 0     //表示操作成功
#define AE_ERR -1  //表示操作失败

#define AE_NONE 0  //表示事件类型的常量
#define AE_READABLE 1
#define AE_WRITABLE 2
#define AE_EDEGE 4

#define AE_FILE_EVENTS 1  //表示文件事件的常量
#define AE_TIME_EVENTS 2    //表示时间事件的常量
#define AE_ALL_EVENTS (AE_FILE_EVENTS|AE_TIME_EVENTS) //表示所有事件的常量
#define AE_DONT_WAIT 4  //表示不等待的常量

#define AE_NOMORE -1 //表示没有更多事件的常量

/* Macros */
#define AE_NOTUSED(V) ((void) V)  //用于标记未使用的变量，避免编译器警告 

struct aeEventLoop; //事件循环结构体的前向声明

/* Types and data structures */
typedef void aeFileProc(struct aeEventLoop *eventLoop, int fd, void *clientData,
                        int mask);  //文件事件处理函数的指针类型
typedef int aeTimeProc(struct aeEventLoop *eventLoop, long long id,
                       void *clientData); //时间事件处理函数的指针类型
typedef void aeEventFinalizerProc(struct aeEventLoop *eventLoop,
                                  void *clientData);  //事件终结器函数的指针类型
typedef void aeBeforeSleepProc(struct aeEventLoop *eventLoop);  //睡眠前处理函数的指针类型

/* File event structure */
typedef struct aeFileEvent {
  int mask; /* one of AE_(READABLE|WRITABLE) */ //事件掩码，表示事件类型
  aeFileProc *rfileProc;  //读事件处理函数指针
  aeFileProc *wfileProc;  //写事件处理函数指针
  void *clientData; //客户端数据
} aeFileEvent;

/* Time event structure */
typedef struct aeTimeEvent {
  long long id; /* time event identifier. */  //时间事件标识符
  long when_sec; /* seconds */  //事件触发的时间——秒
  long when_ms; /* milliseconds */  //事件触发的时间——毫秒
  aeTimeProc *timeProc; //时间事件处理函数指针
  aeEventFinalizerProc *finalizerProc;  //事件终结器函数指针
  void *clientData; //客户端数据
  struct aeTimeEvent *next; //指向下一个时间事件的指针
} aeTimeEvent;

/* A fired event */
typedef struct aeFiredEvent {
  int fd; //文件描述符
  int mask; //事件掩码，表示事件类型
} aeFiredEvent;

/* State of an event based program */
typedef struct aeEventLoop {
  int maxfd; /* highest file descriptor currently registered */ //当前注册的最高文件描述符，用于跟踪已经注册的文件描述符的最大值，以便在事件循环中高效地便利文件描述符
  int setsize; /* max number of file descriptors tracked */ //跟踪的最大文件描述符数，表示事件循环可以处理的最大文件描述符数量，通常在创建事件循环时指定。
  long long timeEventNextId;  //下一个时间事件的ID，用于为新创建的时间事件分配唯一的标识符。确保每一个时间事件都有一个唯一的ID
  time_t lastTime; /* Used to detect system clock skew */ //用于检测系统时钟偏移的时间戳。记录上一次处理时间事件的时间，用于检测系统时钟的变化，以便在系统时钟发生偏移时进行相应地调整
  aeFileEvent *events; /* Registered events */  //注册的文件事件数组，用于存储所有已注册的文件事件，每个文件事件包含文件描述符及其对应的读写事件处理函数
  aeFiredEvent *fired; /* Fired events */ //触发的事件数组，用于存储所有已触发的事件，每个触发事件包含文件描述符及其对应的事件掩码
  aeTimeEvent *timeEventHead; //时间事件链表的头指针，用于管理所有已注册的时间事件，通过链表将所有时间事件串联起来
  int stop; //停止事件循环的标志，用于指示是否停止事件循环。当设置为非零值时，事件循环将停止运行
  void *apidata; /* This is used for polling API specific data */ //用于轮询特定API数据的指针，用于存储与底层轮询API（select、poll、epoll）相关的特定数据，以便在事件循环中使用
  aeBeforeSleepProc *beforesleep; //睡眠前处理函数指针，用于在事件循环进入睡眠之前执行一些操作。例如在进入阻塞等待之前执行一些清理或准备工作。
} aeEventLoop;

/* Prototypes */  //函数原型声明
aeEventLoop *aeCreateEventLoop(int setsize);  //创建事件循环
void aeDeleteEventLoop(aeEventLoop *eventLoop); //删除事件循环
void aeStop(aeEventLoop *eventLoop);  //停止事件循环
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask,
                      aeFileProc *proc, void *clientData);    //创建文件事件
void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask); //删除文件事件
int aeGetFileEvents(aeEventLoop *eventLoop, int fd);  //获取文件事件
long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,
                            aeTimeProc *proc, void *clientData,
                            aeEventFinalizerProc *finalizerProc); //创建时间事件
int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id);    //删除时间事件
int aeProcessEvents(aeEventLoop *eventLoop, int flags); //处理事件
int aeWait(int fd, int mask, long long milliseconds); //等待事件
void aeMain(aeEventLoop *eventLoop);  //事件循环主函数  主事件循环
char *aeGetApiName (void);  //获取API名称
void aeSetBeforeSleepProc(aeEventLoop *eventLoop,
                          aeBeforeSleepProc *beforesleep);  //设置睡眠前处理函数
int aeGetSetSize (aeEventLoop *eventLoop);  //获取事件循环的大小
int aeResizeSetSize (aeEventLoop *eventLoop, int setsize);  //调整事件循环的大小
void startEventLoop(aeEventLoop *el); //启动事件循环

#endif
