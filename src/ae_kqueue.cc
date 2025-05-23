/* Kqueue(2)-based ae.c module
 *
 * Copyright (C) 2009 Harish Mallipeddi - harish.mallipeddi@gmail.com
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

#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

typedef struct aeApiState {
  int kqfd; /*kqueue文件描述符，kqueue文件描述符石通过调用kqueue()函数创建的，用于标识和管理kqueue实例。
              它是kqueueu系统调用的入口，通过它可以像kqueue注册、修改和删除事件*/
  struct kevent *events; /*事件数组，用于存储事件。events数组用于存储kqueue返回的事件。每个事件由struct kevent结构体标识，
                            包含了事件的详细信息，如文件描述符、事件类型、过滤器等。事件数组的大小通常与事件循环的setsize参数相同，
                            表示可以同时处理的最大事件数量。*/
} aeApiState;
/*aeApiCreate函数是基于kqueue的事件驱动编程模块中的一个函数，用于初始化kqueue相关的数据结构和资源。它的主要作用是创建一个新的kqueue实例，并为事件循环分配必要的资源。*/
static int aeApiCreate(aeEventLoop *eventLoop) {
  aeApiState *state = zmalloc(sizeof(aeApiState));  //使用zmalloc分配一个aeApiState结构体，用于存储kqueue相关的状态信息

  if (!state)
    return -1; //如果分配失败，返回-1
  state->events = zmalloc(sizeof(struct kevent) * eventLoop->setsize);//使用zmalloc分配一个kevent结构体数组，大小为setsize，用于存储事件
  if (!state->events) {//如果分配失败，释放state结构体，返回-1表示错误
    zfree(state);
    return -1;
  }
  state->kqfd = kqueue(); //调用kqueue函数创建一个新的kqueue实例，并将返回的文件描述符存储在state结构体的kqfd字段中
  if (state->kqfd == -1) {
    zfree(state->events);
    zfree(state);
    return -1;
  }
  eventLoop->apidata = state;//将初始化后的state结构体赋值给事件循环的apidata字段，表示事件循环使用kqueue作为事件驱动模块
  return 0;
}

static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
  aeApiState *state = eventLoop->apidata;

  state->events = zrealloc(state->events, sizeof(struct kevent) * setsize);
  return 0;
}

static void aeApiFree(aeEventLoop *eventLoop) {
  aeApiState *state = eventLoop->apidata;

  close(state->kqfd);
  zfree(state->events);
  zfree(state);
}

static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
  aeApiState *state = eventLoop->apidata;
  struct kevent ke;

  if (mask & AE_READABLE) {
    EV_SET(&ke, fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    if (kevent(state->kqfd, &ke, 1, NULL, 0, NULL) == -1)
      return -1;
  }
  if (mask & AE_WRITABLE) {
    EV_SET(&ke, fd, EVFILT_WRITE, EV_ADD, 0, 0, NULL);
    if (kevent(state->kqfd, &ke, 1, NULL, 0, NULL) == -1)
      return -1;
  }
  return 0;
}

static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int mask) {
  aeApiState *state = eventLoop->apidata;
  struct kevent ke;

  if (mask & AE_READABLE) {
    EV_SET(&ke, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    kevent(state->kqfd, &ke, 1, NULL, 0, NULL);
  }
  if (mask & AE_WRITABLE) {
    EV_SET(&ke, fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    kevent(state->kqfd, &ke, 1, NULL, 0, NULL);
  }
}

static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
  aeApiState *state = eventLoop->apidata;
  int retval, numevents = 0;

  if (tvp != NULL) {
    struct timespec timeout;
    timeout.tv_sec = tvp->tv_sec;
    timeout.tv_nsec = tvp->tv_usec * 1000;
    retval = kevent(state->kqfd, NULL, 0, state->events, eventLoop->setsize,
                    &timeout);
  } else {
    retval = kevent(state->kqfd, NULL, 0, state->events, eventLoop->setsize,
    NULL);
  }

  if (retval > 0) {
    int j;

    numevents = retval;
    for (j = 0; j < numevents; j++) {
      int mask = 0;
      struct kevent *e = state->events + j;

      if (e->filter == EVFILT_READ)
        mask |= AE_READABLE;
      if (e->filter == EVFILT_WRITE)
        mask |= AE_WRITABLE;
      eventLoop->fired[j].fd = e->ident;
      eventLoop->fired[j].mask = mask;
    }
  }
  return numevents;
}

static char *aeApiName(void) {
  return "kqueue";
}
