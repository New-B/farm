// Copyright (c) 2018 The GAM Authors 

#include <cstring>
#include "worker_handle.h"
#include "zmalloc.h"
#include "util.h"

mutex WorkerHandle::lock;

WorkerHandle::WorkerHandle(Worker* w): worker(w), wqueue(w->GetWorkQ()) {
#if !defined(USE_PIPE_W_TO_H) || (!defined(USE_BOOST_QUEUE) && !defined(USE_PIPE_H_TO_W)) //0||(1&&0)=0
	int notify_buf_size = sizeof(WorkRequest)+sizeof(int);
	int ret = posix_memalign((void**)&notify_buf, HARDWARE_CACHE_LINE, notify_buf_size);
	epicAssert((uint64_t)notify_buf % HARDWARE_CACHE_LINE == 0 && !ret);
	*notify_buf = 2;
#endif
#ifdef USE_PTHREAD_COND   //0
    pthread_mutex_init(&cond_lock, NULL);
    pthread_cond_init(&cond, NULL);
	pthread_mutex_lock(&cond_lock);
#endif
	/*函数体内部，只执行这一行代码，用于初始化当前线程与工作线程之间的通信机制。通过管道或者通知缓冲区，
	  当前线程可以向工作线程发送请求或接收工作线程的处理结果。*/
	RegisterThread();   
}

WorkerHandle::~WorkerHandle() {
	DeRegisterThread();
#if !defined(USE_PIPE_W_TO_H) || (!defined(USE_BOOST_QUEUE) && !defined(USE_PIPE_H_TO_W))
	free((void*)notify_buf);
#endif
#ifdef USE_PTHREAD_COND
	pthread_mutex_unlock(&cond_lock);
#endif
}
/*函数的主要功能是为当前线程注册通信机制，以便与工作线程(Worker)进行交互。它通过创建管道(pipe)和注册通知缓冲区(notify_buf)
来实现线程间的通信和通知机制*/
void WorkerHandle::RegisterThread() {
	if(pipe(send_pipe)) {//用于向工作线程发送通知
		epicLog(LOG_WARNING, "create send pipe failed");
	}
	if(pipe(recv_pipe)) {//用于从工作线程接收通知
		epicLog(LOG_WARNING, "create recv pipe failed");
	}
/*将send_pipe[0]注册到工作线程,send_pipe[0]是管道的读取端，工作线程可以通过它接收通知RegisterHandle将管道的读取端注册到工作线程中，
便于工作线程监听来自当前线程的通知。*/
#if defined(USE_PIPE_W_TO_H) || defined(USE_PIPE_H_TO_W) || defined(USE_PTHREAD_COND)
	worker->RegisterHandle(send_pipe[0]);//在事件循环中注册一个事件，用于监听管道的读取端
#endif
#if !(defined(USE_BOOST_QUEUE) || (defined(USE_PIPE_H_TO_W) && defined(USE_PIPE_W_TO_H)))//!(0||(1&&1))=0
	worker->RegisterNotifyBuf(notify_buf);//将通知缓冲区注册到工作线程，Notify_buf是一个共享的内存缓冲区，用于线程间的通知机制
#endif
}

void WorkerHandle::DeRegisterThread() {
#if defined(USE_PIPE_W_TO_H) || defined(USE_PIPE_H_TO_W) || defined(USE_PTHREAD_COND)
	worker->DeRegisterHandle(send_pipe[0]);
#endif
#if !(defined(USE_BOOST_QUEUE) || (defined(USE_PIPE_H_TO_W) && defined(USE_PIPE_W_TO_H)))
	worker->DeRegisterNotifyBuf(notify_buf);
#endif
	if(close(send_pipe[0])) {
		epicLog(LOG_WARNING, "close send_pipe[0] (%d) failed: %s (%d)", send_pipe[0], strerror(errno), errno);
	}
	if(close(send_pipe[1])) {
		epicLog(LOG_WARNING, "close send_pipe[1] (%d) failed: %s (%d)", send_pipe[1], strerror(errno), errno);
	}
	if(close(recv_pipe[0])) {
		epicLog(LOG_WARNING, "close recv_pipe[0] (%d) failed: %s (%d)", recv_pipe[0], strerror(errno), errno);
	}
	if(close(recv_pipe[1])) {
		epicLog(LOG_WARNING, "close recv_pipe[1] (%d) failed: %s (%d)", recv_pipe[1], strerror(errno), errno);
	}
}
/*SendRequest函数用于将工作请求发送给工作线程，并根据不同的配置选项（如使用管道、条件变量或缓冲区）来通知工作线程处理请求
工作请求的发送通过将请求推送到工作队列并使用不同的通知机制来通知工作线程处理请求*/
int WorkerHandle::SendRequest(WorkRequest* wr) { //WorkRequest* wr:工作请求指针
	WorkRequest* to = wr; //将工作请求指针复制给to
	char buf[1]; //定义一个字符缓冲区，用于管道通信
	buf[0] = 's'; //将字符's'存储在缓冲区中

//根据不同的配置选项，将工作请求to推送到工作队列wqueue，并设置相应地通知机制。
#ifdef USE_PIPE_W_TO_H 	//使用管道通知
	to->fd = recv_pipe[1];
	wqueue->push(to);  //将工作请求to推送到工作队列wqueue
#elif defined(USE_PTHREAD_COND) //使用条件变量通知——未定义
	to->cond_lock = &cond_lock;
	to->cond = &cond;
	to->fd = recv_pipe[1];
	wqueue->push(to);
#else
#ifdef USE_BOOST_QUEUE  //使用缓冲区通知——未定义
    __atomic_store_n(notify_buf, 1, __ATOMIC_RELAXED);
	//*notify_buf = 1; //not useful to set it to 1 if boost_queue is enabled
	epicAssert(*(int*)notify_buf == 1);
	to->fd = recv_pipe[1]; //for legacy code
	to->notify_buf = this->notify_buf;
	wqueue->push(to);
#elif defined(USE_BUF_ONLY)  //使用缓冲区通知——未定义
	*(WorkRequest**)(notify_buf+1) = to;
	to->fd = recv_pipe[1]; //for legacy code
	to->notify_buf = this->notify_buf;
	*notify_buf = 1; //put last since it will trigger the process
#else
	*notify_buf = 1; //put first since notify_buf = 1 may happen after notify_buf = 2 by worker
	epicAssert(*(int*)notify_buf == 1);
	to->fd = recv_pipe[1]; //for legacy code
	to->notify_buf = this->notify_buf;
	wqueue->push(to);
#endif
#endif

//异步请求处理，如果请求时异步的（to->flag & ASYNC），则直接返回SUCCESS，不等待工作线程处理完成
#ifndef USE_BUF_ONLY //otherwise, we have to return after the worker copy the data from notify_buf
	//we return directly without writing to the pipe to notify the worker thread
	if(to->flag & ASYNC) {
		epicLog(LOG_DEBUG, "asynchronous request");
		return SUCCESS;
	}
#endif
//使用管道通知工作线程处理请求
#ifdef USE_PIPE_H_TO_W
#ifdef WH_USE_LOCK //如果定义了WH_USE_LOCK，则使用锁保护管道写操作——未定义
	if(lock.try_lock()) {
#endif
	if (1 != write(send_pipe[1], buf, 1)) {
		epicLog(LOG_WARNING, "write to pipe failed (%d:%s)", errno,
				strerror(errno));
	}

	//we write twice in order to reduce the epoll latency
	if (1 != write(send_pipe[1], buf, 1)) {
		epicLog(LOG_WARNING, "write to pipe failed (%d:%s)", errno,
				strerror(errno));
	}

#ifdef WH_USE_LOCK
		lock.unlock();
	}
#endif
#endif

//根据不同的配置选项，等待工作线程完成处理
#ifdef USE_PIPE_W_TO_H //使用管道读取通知 
	if(1 != read(recv_pipe[0], buf, 1)) { //blocking 请求发起方通过读取管道来等待工作线程处理完请求
		epicLog(LOG_WARNING, "read notification from worker failed");
	} else {
		epicLog(LOG_DEBUG, "request returned %c", buf[0]);
		if(wr->status) {
			epicLog(LOG_INFO, "request failed %d\n", wr->status);
		}
	}
#elif defined(USE_PTHREAD_COND)
	int ret = pthread_cond_wait(&cond, &cond_lock);
	epicAssert(!ret);
#else
    while(__atomic_load_n(notify_buf, __ATOMIC_ACQUIRE) != 2);
	//while(*notify_buf != 2);
//	while(atomic_read(notify_buf) != 2);
	epicLog(LOG_DEBUG, "get notified via buf");
#endif
	return wr->status; //返回工作请求的状态
}



