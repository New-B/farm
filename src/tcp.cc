// Copyright (c) 2018 The GAM Authors 

#include "tcp.h"
#include "rdma.h"
#include "log.h"
#include "server.h"
#include "client.h"
#include "anet.h"
#include "kernel.h"
#include "zmalloc.h"

#include <unistd.h>
#include <cerrno>
#include <cstring>

/*
 * 1.接收新的TCP客户端连接
 * 2.读取客户端发送的连接字符串
 * 3.创建一个新的客户端对象(Client)
 * 4.将连接参数发送回客户端
 * 5.如果当前服务器时主节点(Master)，执行额外的操作
 * 调用时机：函数被注册到事件循环中的回调函数，用于处理新的TCP客户端连接，当监听套接字上有新的客户端连接时，事件循环会调用函数处理该连接。
 * 典型调用场景：客户端尝试连接主节点或工作节点；主节点或工作节点需要与客户端交换连接参数。
 * 在函数中，data被转换为具体的类型Server*，表示当前的服务器对象，以便访问其成员和方法。server是接收TCP连接请求的服务器对象。它可以是
 * 主节点或工作节点，具体取决于当前服务器的角色
 */
void AcceptTcpClientHandle (aeEventLoop *el, int fd, void *data, int mask) {
	epicAssert(data != nullptr); //确保data不为空
    Server *server = (Server*)(data); //将data转换为Server类型的指针，表示当前的服务器对象 
    char msg[MAX_CONN_STRLEN+1];
    int n;
    const char *p;
    Client *cli;
	char neterr[ANET_ERR_LEN];
	char cip[IP_STR_LEN];
	int cfd, cport;

	cfd = anetTcpAccept(neterr, fd, cip, sizeof(cip), &cport); //调用anetTcpAccept接受新的TCP连接，fd是监听套接字，cip和cport分别用于存储客户端的IP地址和端口号
    //cfd是新连接的文件描述符，cip是客户端IP地址，cport是客户端端口号
	if (cfd == ANET_ERR) { //如果接受连接失败，记录错误日志 
		if (errno != EWOULDBLOCK)
			epicLog(LOG_WARNING, "Accepting client connection: %s", neterr);
		return;
	}
	epicLog(LOG_INFO, "Accepted %s:%d", cip, cport); //如果接收成功，记录客户端的IP地址和端口号 
    //读取客户端发送的连接字符串
    //mask是事件类型，AE_READABLE表示可读事件
    if (mask & AE_READABLE) { //检查事件掩码mask是否包含AE_READABLE，表示套接字可读
        n = read(cfd, msg, sizeof msg); //调用read从客户端读取连接字符串，存储到msg中
        if(unlikely(n <= 0)) { //如果读取失败，记录错误日志并跳转到out标签关闭连接
            epicLog(LOG_WARNING, "Unable to read conn string\n");
            goto out;
        }
        msg[n] = '\0'; //如果读取成功，将读取的字符串打印到日志中
        epicLog(LOG_INFO, "conn string %s\n", msg);
    }
    //创建新的客户端对象
    if (unlikely(!(cli = server->NewClient(msg)))) { //调用server的NewClient方法创建一个新的客户端对象，传入读取到的连接字符串msg
        goto out; //如果创建失败，跳转到out标签关闭连接
    }
    //获取连接参数并发送回客户端
    if (unlikely(!(p = cli->GetConnString(server->GetWorkerId())))) {  //调用GetConnString获取连接参数，server->GetWorkerId()返回当前服务器的工作节点ID
        goto out; //如果获取失败，跳转到out标签关闭连接
    }

    n = write(cfd, p, strlen(p)); //将连接参数发送回客户端 

	if (unlikely(n < strlen(p))) { //如果发送失败，记录警告日志并移除客户端对象
		epicLog(LOG_WARNING, "Unable to send conn string\n");
		server->RmClient(cli);
	}
    //如果是主节点，执行额外操作
	if(server->IsMaster()) server->PostAcceptWorker(cfd, server); //调用PostAcceptWorker方法执行额外的操作 

out:
    close(cfd); //关闭连接，无论是否成功处理连接，都会在out标签处关闭客户端套接字cfd
}

void ProcessRdmaRequestHandle (aeEventLoop *el, int fd, void *data, int mask) {
    ((Server *)data)->ProcessRdmaRequest();
}
