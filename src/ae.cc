/* A simple event-driven programming library. Originally I wrote this code
 * for the Jim's event-loop (Jim is a Tcl interpreter) but later translated
 * it in form of a library for easy reuse.
 *
 * Copyright (c) 2006-2010, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "ae.h"
#include "zmalloc.h"
#include "config.h"

/* Include the best multiplexing layer supported by this system.
 * The following should be ordered by performances, descending. */
#ifdef HAVE_EVPORT
#include "ae_evport.cc"
#else
#ifdef HAVE_EPOLL
#include "ae_epoll.cc"
#else
#ifdef HAVE_KQUEUE
#include "ae_kqueue.cc"
#else
#include "ae_select.cc"
#endif
#endif
#endif

aeEventLoop *aeCreateEventLoop(int setsize) {
  aeEventLoop *eventLoop;
  int i;

  if ((eventLoop = (aeEventLoop *) zmalloc(sizeof(*eventLoop))) == NULL) //为aeEventLoop结构体分配内存
    goto err;
  eventLoop->events = (aeFileEvent *) zmalloc(sizeof (aeFileEvent) * setsize); //为文件事件数组events分配内存，大小为setsize
  eventLoop->fired = (aeFiredEvent *) zmalloc(sizeof (aeFiredEvent) * setsize); //为触发事件数组fired分配内存，大小为setsize
  if (eventLoop->events == NULL || eventLoop->fired == NULL) //检查数组空间的分配是否成功
    goto err;
  eventLoop->setsize = setsize; //设置事件循环的大小，初始化setsize为传入的参数值
  eventLoop->lastTime = time (NULL); //初始化lastTime为当前事件，用于检测系统时钟偏移
  eventLoop->timeEventHead = NULL;//初始化时间事件链表头指针为NULL，表示当前没有注册任何时间事件
  eventLoop->timeEventNextId = 0; //初始化timeEventNextId时间事件ID为0，用于为新创建的时间事件分配唯一的标识符
  eventLoop->stop = 0; //初始化stop为0，表示事件循环未停止
  eventLoop->maxfd = -1; //初始化maxfd为-1，表示当前没有文件描述符
  eventLoop->beforesleep = NULL; // 初始化beforesleep为NULL，表示睡眠前处理函数为空
  if (aeApiCreate(eventLoop) == -1)
    goto err;
  /* Events with mask == AE_NONE are not set. So let's initialize the
   * vector with it. */
  for (i = 0; i < setsize; i++)
    eventLoop->events[i].mask = AE_NONE;
  return eventLoop;

  err: if (eventLoop) {
    zfree (eventLoop->events);
    zfree (eventLoop->fired);
    zfree (eventLoop);
  }
  return NULL;
}

/* Return the current set size. */
int aeGetSetSize (aeEventLoop *eventLoop) {
  return eventLoop->setsize;
}

/* Resize the maximum set size of the event loop.
 * If the requested set size is smaller than the current set size, but
 * there is already a file descriptor in use that is >= the requested
 * set size minus one, AE_ERR is returned and the operation is not
 * performed at all.
 *
 * Otherwise AE_OK is returned and the operation is successful. */
int aeResizeSetSize (aeEventLoop *eventLoop, int setsize) {
  int i;

  if (setsize == eventLoop->setsize)
    return AE_OK;
  if (eventLoop->maxfd >= setsize)
    return AE_ERR;
  if (aeApiResize(eventLoop, setsize) == -1)
    return AE_ERR;

  eventLoop->events = (aeFileEvent *) zrealloc(eventLoop->events,
                                               sizeof (aeFileEvent) * setsize);
  eventLoop->fired = (aeFiredEvent *) zrealloc(eventLoop->fired,
                                               sizeof (aeFiredEvent) * setsize);
  eventLoop->setsize = setsize;

  /* Make sure that if we created new slots, they are initialized with
   * an AE_NONE mask. */
  for (i = eventLoop->maxfd + 1; i < setsize; i++)
    eventLoop->events[i].mask = AE_NONE;
  return AE_OK;
}

void aeDeleteEventLoop(aeEventLoop *eventLoop) {
  aeApiFree (eventLoop);
  zfree (eventLoop->events);
  zfree (eventLoop->fired);
  zfree (eventLoop);
}

void aeStop(aeEventLoop *eventLoop) {
  eventLoop->stop = 1;
}

int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask,
                      aeFileProc *proc, void *clientData) {
  if (fd >= eventLoop->setsize) {
    errno = ERANGE;
    return AE_ERR;
  }
  aeFileEvent *fe = &eventLoop->events[fd];

  if (aeApiAddEvent(eventLoop, fd, mask) == -1)
    return AE_ERR;
  fe->mask |= mask;
  if (mask & AE_READABLE)
    fe->rfileProc = proc;
  if (mask & AE_WRITABLE)
    fe->wfileProc = proc;
  fe->clientData = clientData;
  if (fd > eventLoop->maxfd)
    eventLoop->maxfd = fd;
  return AE_OK;
}

void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask) {
  if (fd >= eventLoop->setsize)
    return;
  aeFileEvent *fe = &eventLoop->events[fd];
  if (fe->mask == AE_NONE)
    return;

  aeApiDelEvent(eventLoop, fd, mask);
  fe->mask = fe->mask & (~mask);
  if (fd == eventLoop->maxfd && fe->mask == AE_NONE) {
    /* Update the max fd */
    int j;

    for (j = eventLoop->maxfd - 1; j >= 0; j--)
      if (eventLoop->events[j].mask != AE_NONE)
        break;
    eventLoop->maxfd = j;
  }
}

int aeGetFileEvents(aeEventLoop *eventLoop, int fd) {
  if (fd >= eventLoop->setsize)
    return 0;
  aeFileEvent *fe = &eventLoop->events[fd];

  return fe->mask;
}

static void aeGetTime (long *seconds, long *milliseconds) {
  struct timeval tv;

  gettimeofday(&tv, NULL);
  *seconds = tv.tv_sec;
  *milliseconds = tv.tv_usec / 1000;
}

static void aeAddMillisecondsToNow (long long milliseconds, long *sec,
                                   long *ms) {
  long cur_sec, cur_ms, when_sec, when_ms;

  aeGetTime (&cur_sec, &cur_ms);
  when_sec = cur_sec + milliseconds / 1000;
  when_ms = cur_ms + milliseconds % 1000;
  if (when_ms >= 1000) {
    when_sec++;
    when_ms -= 1000;
  }
  *sec = when_sec;
  *ms = when_ms;
}

long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,
                            aeTimeProc *proc, void *clientData,
                            aeEventFinalizerProc *finalizerProc) {
  long long id = eventLoop->timeEventNextId++;
  aeTimeEvent *te;

  te = (aeTimeEvent *) zmalloc(sizeof (*te));
  if (te == NULL)
    return AE_ERR;
  te->id = id;
  aeAddMillisecondsToNow (milliseconds, &te->when_sec, &te->when_ms);
  te->timeProc = proc;
  te->finalizerProc = finalizerProc;
  te->clientData = clientData;
  te->next = eventLoop->timeEventHead;
  eventLoop->timeEventHead = te;
  return id;
}

int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id) {
  aeTimeEvent *te, *prev = NULL;

  te = eventLoop->timeEventHead;
  while (te) {
    if (te->id == id) {
      if (prev == NULL)
        eventLoop->timeEventHead = te->next;
      else
        prev->next = te->next;
      if (te->finalizerProc)
        te->finalizerProc(eventLoop, te->clientData);
      zfree (te);
      return AE_OK;
    }
    prev = te;
    te = te->next;
  }
  return AE_ERR; /* NO event with the specified ID found */
}

/* Search the first timer to fire.
 * This operation is useful to know how many time the select can be
 * put in sleep without to delay any event.
 * If there are no timers NULL is returned.
 *
 * Note that's O(N) since time events are unsorted.
 * Possible optimizations (not needed by Redis so far, but...):
 * 1) Insert the event in order, so that the nearest is just the head.
 *    Much better but still insertion or deletion of timers is O(N).
 * 2) Use a skiplist to have this operation as O(1) and insertion as O(log(N)).
 */
static aeTimeEvent *aeSearchNearestTimer (aeEventLoop *eventLoop) {
  aeTimeEvent *te = eventLoop->timeEventHead;
  aeTimeEvent *nearest = NULL;

  while (te) {
    if (!nearest || te->when_sec < nearest->when_sec
        || (te->when_sec == nearest->when_sec && te->when_ms < nearest->when_ms))
      nearest = te;
    te = te->next;
  }
  return nearest;
}

/* Process time events */
static int processTimeEvents(aeEventLoop *eventLoop) {
  int processed = 0;
  aeTimeEvent *te;
  long long maxId;
  time_t now = time (NULL);

  /* If the system clock is moved to the future, and then set back to the
   * right value, time events may be delayed in a random way. Often this
   * means that scheduled operations will not be performed soon enough.
   *
   * Here we try to detect system clock skews, and force all the time
   * events to be processed ASAP when this happens: the idea is that
   * processing events earlier is less dangerous than delaying them
   * indefinitely, and practice suggests it is. */
  if (now < eventLoop->lastTime) {
    te = eventLoop->timeEventHead;
    while (te) {
      te->when_sec = 0;
      te = te->next;
    }
  }
  eventLoop->lastTime = now;

  te = eventLoop->timeEventHead;
  maxId = eventLoop->timeEventNextId - 1;
  while (te) {
    long now_sec, now_ms;
    long long id;

    if (te->id > maxId) {
      te = te->next;
      continue;
    }
    aeGetTime (&now_sec, &now_ms);
    if (now_sec > te->when_sec
        || (now_sec == te->when_sec && now_ms >= te->when_ms)) {
      int retval;

      id = te->id;
      retval = te->timeProc(eventLoop, id, te->clientData);
      processed++;
      /* After an event is processed our time event list may
       * no longer be the same, so we restart from head.
       * Still we make sure to don't process events registered
       * by event handlers itself in order to don't loop forever.
       * To do so we saved the max ID we want to handle.
       *
       * FUTURE OPTIMIZATIONS:
       * Note that this is NOT great algorithmically. Redis uses
       * a single time event so it's not a problem but the right
       * way to do this is to add the new elements on head, and
       * to flag deleted elements in a special way for later
       * deletion (putting references to the nodes to delete into
       * another linked list). */
      if (retval != AE_NOMORE) {
        aeAddMillisecondsToNow (retval, &te->when_sec, &te->when_ms);
      } else {
        aeDeleteTimeEvent(eventLoop, id);
      }
      te = eventLoop->timeEventHead;
    } else {
      te = te->next;
    }
  }
  return processed;
}

/* Process every pending time event, then every pending file event
 * (that may be registered by time event callbacks just processed).
 * Without special flags the function sleeps until some file event
 * fires, or when the next time event occurs (if any).
 *
 * If flags is 0, the function does nothing and returns.
 * if flags has AE_ALL_EVENTS set, all the kind of events are processed.
 * if flags has AE_FILE_EVENTS set, file events are processed.
 * if flags has AE_TIME_EVENTS set, time events are processed.
 * if flags has AE_DONT_WAIT set the function returns ASAP until all
 * the events that's possible to process without to wait are processed.
 *
 * The function returns the number of events processed. */
/*函数是事件驱动编程中的核心函数，用于处理事件循环中的所有事件。它根据传入的标志位处理时间事件和文件事件，并返回处理的事件数量。
通过这种设计，系统能够高效地处理各种事件，实现异步和并发操作。
设计意义：
高效事件处理：aeProcessEvents函数通过统一的接口处理文件事件和时间事件，提高了事件处理的效率。
灵活性：通过标志位控制，可以灵活地选择处理哪些类型的时间，以及是否等待事件发生。
事件驱动模型：该函数是事件驱动编程模型的核心，允许程序在事件发生时进行处理，而不是阻塞等待，提高了系统的并发性和响应速度。
1.参数说明：aeEventLoop *eventLoop——事件循环的指针，包含事件循环的状态信息。int flags——标志位，用于指定要处理的事件类型和行为。
2.返回值：返回处理的事件数量。
3.标志位说明：
#define AE_FILE_EVENTS 1  //表示文件事件的常量
#define AE_TIME_EVENTS 2    //表示时间事件的常量
#define AE_ALL_EVENTS (AE_FILE_EVENTS|AE_TIME_EVENTS) //表示所有事件的常量
#define AE_DONT_WAIT 4  //表示不等待的常量*/
int aeProcessEvents(aeEventLoop *eventLoop, int flags) {
  int processed = 0, numevents;

  /* Nothing to do? return ASAP */
  /*检查是否有事件需要处理，如果既没有时间事件也没有文件事件需要处理，立即返回。*/
  if (!(flags & AE_TIME_EVENTS) && !(flags & AE_FILE_EVENTS))
    return 0;

  /* Note that we want call select() even if there are no
   * file events to process as long as we want to process time
   * events, in order to sleep until the next time event is ready
   * to fire. */
  if (eventLoop->maxfd != -1
      || ((flags & AE_TIME_EVENTS) && !(flags & AE_DONT_WAIT))) {
    int j;
    aeTimeEvent *shortest = NULL;
    struct timeval tv, *tvp;
    //计算等待时间，如果有时间事件且不设置AE_DONT_WAIT标志，则找到最近的时间事件并计算等待时间
    if (flags & AE_TIME_EVENTS && !(flags & AE_DONT_WAIT))
      shortest = aeSearchNearestTimer (eventLoop);
    if (shortest) {
      long now_sec, now_ms;

      /* Calculate the time missing for the nearest
       * timer to fire. */
      aeGetTime (&now_sec, &now_ms);
      tvp = &tv;
      tvp->tv_sec = shortest->when_sec - now_sec;
      if (shortest->when_ms < now_ms) {
        tvp->tv_usec = ( (shortest->when_ms + 1000) - now_ms) * 1000;
        tvp->tv_sec--;
      } else {
        tvp->tv_usec = (shortest->when_ms - now_ms) * 1000;
      }
      if (tvp->tv_sec < 0)
        tvp->tv_sec = 0;
      if (tvp->tv_usec < 0)
        tvp->tv_usec = 0;
    } else {
      /* If we have to check for events but need to return
       * ASAP because of AE_DONT_WAIT we need to set the timeout
       * to zero */
      //如果没有时间事件或设置了AE_DONT_WAIT标志，则设置超时时间为0或无限等待
      if (flags & AE_DONT_WAIT) {
        tv.tv_sec = tv.tv_usec = 0;
        tvp = &tv;
      } else {
        /* Otherwise we can block */
        tvp = NULL; /* wait forever */
      }
    }
    //调用aeApiPoll函数等待并处理文件事件
    numevents = aeApiPoll (eventLoop, tvp);
    //遍历所有触发的文件事件，调用相应的处理函数
    for (j = 0; j < numevents; j++) {
      aeFileEvent *fe = &eventLoop->events[eventLoop->fired[j].fd];
      int mask = eventLoop->fired[j].mask;
      int fd = eventLoop->fired[j].fd;
      int rfired = 0;

      /* note the fe->mask & mask & ... code: maybe an already processed
       * event removed an element that fired and we still didn't
       * processed, so we check if the event is still valid. */
      if (fe->mask & mask & AE_READABLE) {
        rfired = 1;
        fe->rfileProc(eventLoop, fd, fe->clientData, mask);
      }
      if (fe->mask & mask & AE_WRITABLE) {
        if (!rfired || fe->wfileProc != fe->rfileProc)
          fe->wfileProc(eventLoop, fd, fe->clientData, mask);
      }
      processed++;
    }
  }
  /* Check time events */
  //处理时间事件，如果设置了AE_TIME_EVENTS标志，则调用processTimeEvents函数处理时间事件
  if (flags & AE_TIME_EVENTS)
    processed += processTimeEvents(eventLoop);
  //返回处理的文件事件和时间事件的总数量
  return processed; /* return the number of processed file/time events */
}

/* Wait for milliseconds until the given file descriptor becomes
 * writable/readable/exception */
int aeWait(int fd, int mask, long long milliseconds) {
  struct pollfd pfd;
  int retmask = 0, retval;

  memset(&pfd, 0, sizeof (pfd));
  pfd.fd = fd;
  if (mask & AE_READABLE)
    pfd.events |= POLLIN;
  if (mask & AE_WRITABLE)
    pfd.events |= POLLOUT;

  if ((retval = poll(&pfd, 1, milliseconds)) == 1) {
    if (pfd.revents & POLLIN)
      retmask |= AE_READABLE;
    if (pfd.revents & POLLOUT)
      retmask |= AE_WRITABLE;
    if (pfd.revents & POLLERR)
      retmask |= AE_WRITABLE;
    if (pfd.revents & POLLHUP)
      retmask |= AE_WRITABLE;
    return retmask;
  } else {
    return retval;
  }
}
/*函数是事件驱动编程中的核心循环，用于处理事件循环中的所有事件。它不断地检查和处理事件，直到事件循环被停止。
aeEventLoop结构体包含事件循环的状态信息，包括停止标志stop和睡眠前处理函数beforesleep。
aeMain函数运行事件循环，不断地调用beforesleep函数和aeProcessEvents函数，直到stop标志被设置为非零值。*/
void aeMain(aeEventLoop *eventLoop) {
  eventLoop->stop = 0;  //将事件循环的停止标志stop初始化为0，表示事件循环继续运行。
  while (!eventLoop->stop) {   //当stop标志为0时，事件循环继续运行。事件循环的核心是一个while循环，它不断地检查和处理事件，直到stop标志被设置为非零值
    if (eventLoop->beforesleep != NULL)  //如果beforesleep函数指针不为空，则在进入睡眠之前调用该函数
      eventLoop->beforesleep(eventLoop); //beforesleep函数通常用于在事件循环进入阻塞等待之前执行一些清理或准备工作
    aeProcessEvents(eventLoop, AE_ALL_EVENTS);  //调用aeProcessEvents函数处理所有类型的事件。aeProcessEvents函数是事件循环的核心，它负责检查和处理所有已注册的事件（时间、文件事件等。）
  }
}

char *aeGetApiName (void) {
  return aeApiName ();
}

void aeSetBeforeSleepProc(aeEventLoop *eventLoop,
                          aeBeforeSleepProc *beforesleep) {
  eventLoop->beforesleep = beforesleep;
}

void startEventLoop(aeEventLoop *el) {
  //start epoll
  aeMain(el);

  //end the service
  aeDeleteEventLoop(el);
}
