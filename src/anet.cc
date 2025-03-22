/* anet.c -- Basic TCP socket stuff made a bit less boring
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

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <netdb.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>

#include "anet.h"
#include <arpa/inet.h> // 添加头文件以使用 inet_ntop 函数
#include "log.h"

static void anetSetError(char *err, const char *fmt, ...) {
  va_list ap;

  if (!err)
    return;
  va_start(ap, fmt);
  vsnprintf(err, ANET_ERR_LEN, fmt, ap);
  va_end(ap);
}

int anetSetBlock(char *err, int fd, int non_block) {
  int flags;

  /* Set the socket blocking (if non_block is zero) or non-blocking.
   * Note that fcntl(2) for F_GETFL and F_SETFL can't be
   * interrupted by a signal. */
  if ((flags = fcntl(fd, F_GETFL)) == -1) {
    anetSetError(err, "fcntl(F_GETFL): %s", strerror(errno));
    return ANET_ERR;
  }

  if (non_block)
    flags |= O_NONBLOCK;
  else
    flags &= ~O_NONBLOCK;

  if (fcntl(fd, F_SETFL, flags) == -1) {
    anetSetError(err, "fcntl(F_SETFL,O_NONBLOCK): %s", strerror(errno));
    return ANET_ERR;
  }
  return ANET_OK;
}

int anetNonBlock(char *err, int fd) {
  return anetSetBlock(err, fd, 1);
}

int anetBlock(char *err, int fd) {
  return anetSetBlock(err, fd, 0);
}

/* Set TCP keep alive option to detect dead peers. The interval option
 * is only used for Linux as we are using Linux-specific APIs to set
 * the probe send time, interval, and count. */
int anetKeepAlive(char *err, int fd, int interval) {
  int val = 1;

  if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val)) == -1) {
    anetSetError(err, "setsockopt SO_KEEPALIVE: %s", strerror(errno));
    return ANET_ERR;
  }

#ifdef __linux__
  /* Default settings are more or less garbage, with the keepalive time
   * set to 7200 by default on Linux. Modify settings to make the feature
   * actually useful. */

  /* Send first probe after interval. */
  val = interval;
  if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &val, sizeof(val)) < 0) {
    anetSetError(err, "setsockopt TCP_KEEPIDLE: %s\n", strerror(errno));
    return ANET_ERR;
  }

  /* Send next probes after the specified interval. Note that we set the
   * delay as interval / 3, as we send three probes before detecting
   * an error (see the next setsockopt call). */
  val = interval / 3;
  if (val == 0)
    val = 1;
  if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &val, sizeof(val)) < 0) {
    anetSetError(err, "setsockopt TCP_KEEPINTVL: %s\n", strerror(errno));
    return ANET_ERR;
  }

  /* Consider the socket in error state after three we send three ACK
   * probes without getting a reply. */
  val = 3;
  if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &val, sizeof(val)) < 0) {
    anetSetError(err, "setsockopt TCP_KEEPCNT: %s\n", strerror(errno));
    return ANET_ERR;
  }
#else
  ((void) interval); /* Avoid unused var warning for non Linux systems. */
#endif

  return ANET_OK;
}

static int anetSetTcpNoDelay(char *err, int fd, int val) {
  if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val)) == -1) {
    anetSetError(err, "setsockopt TCP_NODELAY: %s", strerror(errno));
    return ANET_ERR;
  }
  return ANET_OK;
}

int anetEnableTcpNoDelay(char *err, int fd) {
  return anetSetTcpNoDelay(err, fd, 1);
}

int anetDisableTcpNoDelay(char *err, int fd) {
  return anetSetTcpNoDelay(err, fd, 0);
}

int anetSetSendBuffer(char *err, int fd, int buffsize) {
  if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffsize, sizeof(buffsize))
      == -1) {
    anetSetError(err, "setsockopt SO_SNDBUF: %s", strerror(errno));
    return ANET_ERR;
  }
  return ANET_OK;
}

int anetTcpKeepAlive(char *err, int fd) {
  int yes = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes)) == -1) {
    anetSetError(err, "setsockopt SO_KEEPALIVE: %s", strerror(errno));
    return ANET_ERR;
  }
  return ANET_OK;
}

/* Set the socket send timeout (SO_SNDTIMEO socket option) to the specified
 * number of milliseconds, or disable it if the 'ms' argument is zero. */
int anetSendTimeout(char *err, int fd, long long ms) {
  struct timeval tv;

  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == -1) {
    anetSetError(err, "setsockopt SO_SNDTIMEO: %s", strerror(errno));
    return ANET_ERR;
  }
  return ANET_OK;
}

/* anetGenericResolve() is called by anetResolve() and anetResolveIP() to
 * do the actual work. It resolves the hostname "host" and set the string
 * representation of the IP address into the buffer pointed by "ipbuf".
 *
 * If flags is set to ANET_IP_ONLY the function only resolves hostnames
 * that are actually already IPv4 or IPv6 addresses. This turns the function
 * into a validating / normalizing function. */
int anetGenericResolve(char *err, char *host, char *ipbuf, size_t ipbuf_len,
                       int flags) {
  struct addrinfo hints, *info;
  int rv;

  memset(&hints, 0, sizeof(hints));
  if (flags & ANET_IP_ONLY)
    hints.ai_flags = AI_NUMERICHOST;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM; /* specify socktype to avoid dups */

  if ((rv = getaddrinfo(host, NULL, &hints, &info)) != 0) {
    anetSetError(err, "%s", gai_strerror(rv));
    return ANET_ERR;
  }
  if (info->ai_family == AF_INET) {
    struct sockaddr_in *sa = (struct sockaddr_in *) info->ai_addr;
    inet_ntop(AF_INET, &(sa->sin_addr), ipbuf, ipbuf_len);
  } else {
    struct sockaddr_in6 *sa = (struct sockaddr_in6 *) info->ai_addr;
    inet_ntop(AF_INET6, &(sa->sin6_addr), ipbuf, ipbuf_len);
  }

  freeaddrinfo(info);
  return ANET_OK;
}

int anetResolve(char *err, char *host, char *ipbuf, size_t ipbuf_len) {
  return anetGenericResolve(err, host, ipbuf, ipbuf_len, ANET_NONE);
}

int anetResolveIP(char *err, char *host, char *ipbuf, size_t ipbuf_len) {
  return anetGenericResolve(err, host, ipbuf, ipbuf_len, ANET_IP_ONLY);
}

static int anetSetReuseAddr(char *err, int fd) {
  int yes = 1;
  /* Make sure connection-intensive things like the redis benckmark
   * will be able to close/open sockets a zillion of times */
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
    anetSetError(err, "setsockopt SO_REUSEADDR: %s", strerror(errno));
    return ANET_ERR;
  }
  return ANET_OK;
}

static int anetCreateSocket(char *err, int domain) {
  int s;
  if ((s = socket(domain, SOCK_STREAM, 0)) == -1) {
    anetSetError(err, "creating socket: %s", strerror(errno));
    return ANET_ERR;
  }

  /* Make sure connection-intensive things like the redis benchmark
   * will be able to close/open sockets a zillion of times */
  if (anetSetReuseAddr(err, s) == ANET_ERR) {
    close(s);
    return ANET_ERR;
  }
  return s;
}

#define ANET_CONNECT_NONE 0
#define ANET_CONNECT_NONBLOCK 1
#define ANET_CONNECT_BE_BINDING 2 /* Best effort binding. */
static int anetTcpGenericConnect(char *err, char *addr, int port,
                                 char *source_addr, int flags) {
  int s = ANET_ERR, rv;
  char portstr[6]; /* strlen("65535") + 1; */
  struct addrinfo hints, *servinfo, *bservinfo, *p, *b;

  snprintf(portstr, sizeof(portstr), "%d", port);
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  if ((rv = getaddrinfo(addr, portstr, &hints, &servinfo)) != 0) {
    anetSetError(err, "%s", gai_strerror(rv));
    return ANET_ERR;
  }
  for (p = servinfo; p != NULL; p = p->ai_next) {
    /* Try to create the socket and to connect it.
     * If we fail in the socket() call, or on connect(), we retry with
     * the next entry in servinfo. */
    if ((s = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1)
      continue;
    if (anetSetReuseAddr(err, s) == ANET_ERR)
      goto error;
    if (flags & ANET_CONNECT_NONBLOCK && anetNonBlock(err, s) != ANET_OK)
      goto error;
    if (source_addr) {
      int bound = 0;
      /* Using getaddrinfo saves us from self-determining IPv4 vs IPv6 */
      if ((rv = getaddrinfo(source_addr, NULL, &hints, &bservinfo)) != 0) {
        anetSetError(err, "%s", gai_strerror(rv));
        goto error;
      }
      for (b = bservinfo; b != NULL; b = b->ai_next) {
        if (bind(s, b->ai_addr, b->ai_addrlen) != -1) {
          bound = 1;
          break;
        }
      }
      freeaddrinfo(bservinfo);
      if (!bound) {
        anetSetError(err, "bind: %s", strerror(errno));
        goto error;
      }
    }
    if (connect(s, p->ai_addr, p->ai_addrlen) == -1) {
      /* If the socket is non-blocking, it is ok for connect() to
       * return an EINPROGRESS error here. */
      if (errno == EINPROGRESS && flags & ANET_CONNECT_NONBLOCK)
        goto end;
      close(s);
      s = ANET_ERR;
      continue;
    }

    /* If we ended an iteration of the for loop without errors, we
     * have a connected socket. Let's return to the caller. */
    goto end;
  }
  if (p == NULL)
    anetSetError(err, "creating socket: %s", strerror(errno));

  error: if (s != ANET_ERR) {
    close(s);
    s = ANET_ERR;
  }

  end: freeaddrinfo(servinfo);

  /* Handle best effort binding: if a binding address was used, but it is
   * not possible to create a socket, try again without a binding address. */
  if (s == ANET_ERR && source_addr && (flags & ANET_CONNECT_BE_BINDING)) {
    return anetTcpGenericConnect(err, addr, port, NULL, flags);
  } else {
    return s;
  }
}

int anetTcpConnect(char *err, char *addr, int port) {
  return anetTcpGenericConnect(err, addr, port, NULL, ANET_CONNECT_NONE);
}

int anetTcpNonBlockConnect(char *err, char *addr, int port) {
  return anetTcpGenericConnect(err, addr, port, NULL, ANET_CONNECT_NONBLOCK);
}

int anetTcpNonBlockBindConnect(char *err, char *addr, int port,
                               char *source_addr) {
  return anetTcpGenericConnect(err, addr, port, source_addr,
  ANET_CONNECT_NONBLOCK);
}

int anetTcpNonBlockBestEffortBindConnect(char *err, char *addr, int port,
                                         char *source_addr) {
  return anetTcpGenericConnect(err, addr, port, source_addr,
  ANET_CONNECT_NONBLOCK | ANET_CONNECT_BE_BINDING);
}

int anetUnixGenericConnect(char *err, char *path, int flags) {
  int s;
  struct sockaddr_un sa;

  if ((s = anetCreateSocket(err, AF_LOCAL)) == ANET_ERR)
    return ANET_ERR;

  sa.sun_family = AF_LOCAL;
  strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
  if (flags & ANET_CONNECT_NONBLOCK) {
    if (anetNonBlock(err, s) != ANET_OK)
      return ANET_ERR;
  }
  if (connect(s, (struct sockaddr*) &sa, sizeof(sa)) == -1) {
    if (errno == EINPROGRESS && flags & ANET_CONNECT_NONBLOCK)
      return s;

    anetSetError(err, "connect: %s", strerror(errno));
    close(s);
    return ANET_ERR;
  }
  return s;
}

int anetUnixConnect(char *err, char *path) {
  return anetUnixGenericConnect(err, path, ANET_CONNECT_NONE);
}

int anetUnixNonBlockConnect(char *err, char *path) {
  return anetUnixGenericConnect(err, path, ANET_CONNECT_NONBLOCK);
}

/* Like read(2) but make sure 'count' is read before to return
 * (unless error or EOF condition is encountered) */
int anetRead(int fd, char *buf, int count) {
  int nread, totlen = 0;
  while (totlen != count) {
    nread = read(fd, buf, count - totlen);
    if (nread == 0)
      return totlen;
    if (nread == -1)
      return -1;
    totlen += nread;
    buf += nread;
  }
  return totlen;
}

/* Like write(2) but make sure 'count' is read before to return
 * (unless error is encountered) */
int anetWrite(int fd, char *buf, int count) {
  int nwritten, totlen = 0;
  while (totlen != count) {
    nwritten = write(fd, buf, count - totlen);
    if (nwritten == 0)
      return totlen;
    if (nwritten == -1)
      return -1;
    totlen += nwritten;
    buf += nwritten;
  }
  return totlen;
}

static int anetListen(char *err, int s, struct sockaddr *sa, socklen_t len,
                      int backlog) {
  if (bind(s, sa, len) == -1) {
    anetSetError(err, "bind: %s", strerror(errno));
    close(s);
    return ANET_ERR;
  }

  if (listen(s, backlog) == -1) {
    anetSetError(err, "listen: %s", strerror(errno));
    close(s);
    return ANET_ERR;
  }
  return ANET_OK;
}

static int anetV6Only(char *err, int s) {
  int yes = 1;
  if (setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof(yes)) == -1) {
    anetSetError(err, "setsockopt: %s", strerror(errno));
    close(s);
    return ANET_ERR;
  }
  return ANET_OK;
}
/*这个函数的主要功能是创建一个TCP服务器套接字，绑定到指定的地址和端口，并设置监听队列的最大长度。
如果任何步骤失败，函数会设置错误信息并返回ANET_ERR。成功时，返回创建的套接字描述符。
函数参数：
char *err：用于存储错误信息的字符数组
int port：服务器绑定的端口号
char *bindaddr：服务器绑定的地址，如果为nullptr，则绑定到所有地址
int af：地址族，AF_INET或AF_INET6
int backlog：监听队列的最大长度
*/
static int _anetTcpServer(char *err, int port, char *bindaddr, int af,
                          int backlog) {
  int s, rv;//定义套接字描述符和返回值
  char _port[6]; /* strlen("65535") */ //定义一个字符数组，用于存储端口号字符串
  struct addrinfo hints, *servinfo, *p;  //定义addrinfo结构体hints以及指向addrinfo结构的指针servinfo和p

  snprintf(_port, 6, "%d", port); //将端口号转换为字符串并存储在_port中
  memset(&hints, 0, sizeof(hints)); //将hints结构的所有字节设置为0
  hints.ai_family = af; //设置地址族（IPv4   IPv6）
  hints.ai_socktype = SOCK_STREAM; //设置套接字类型为流式套接字
  hints.ai_flags = AI_PASSIVE; /* No effect if bindaddr != NULL */ //设置AI_PASSIVE,表示返回的套接字地址用于绑定

  if ((rv = getaddrinfo(bindaddr, _port, &hints, &servinfo)) != 0) {  //获取地址信息，并将结果存储在servinfo链表中
    anetSetError(err, "%s", gai_strerror(rv)); //如果获取地址信息失败，设置错误信息并返回ANET_ERR
    return ANET_ERR;
  }
  for (p = servinfo; p != NULL; p = p->ai_next) { //遍历servinfo链表中的每个地址信息
    if ((s = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) //尝试创建套接字
      continue; //如果创建套接字失败，则继续尝试下一个地址

    if (af == AF_INET6 && anetV6Only(err, s) == ANET_ERR)  //如果地址族为IPv6，则设置套接字选项IPV6_V6 ONLY
      goto error; //如果设置失败，则跳转到error标签
    if (anetSetReuseAddr(err, s) == ANET_ERR) //设置套接字地址重用项
      goto error; //如果设置失败，则跳转到error标签
    if (anetListen(err, s, p->ai_addr, p->ai_addrlen, backlog) == ANET_ERR)   //绑定并监听套接字，设置监听队列的最大长度
      goto error; //如果绑定或监听失败，则跳转到error标签

    // 打印监听的 IP 地址和端口号到日志
    char ipstr[INET_ADDRSTRLEN];
    void *addr;
    if (p->ai_family == AF_INET) { // IPv4
      struct sockaddr_in *ipv4 = (struct sockaddr_in *)p->ai_addr;
      addr = &(ipv4->sin_addr);
    } 
    inet_ntop(p->ai_family, addr, ipstr, sizeof(ipstr));
    if (strcmp(ipstr, "0.0.0.0") == 0) {
      epicLog(LOG_FATAL, "Error: Listening on invalid address %s:%d", ipstr, port);
      goto error;
    }
    epicLog(LOG_INFO, "Listening on %s:%d", ipstr, port);

    goto end; //如果绑定和监听成功，则跳转到end标签
  }
  if (p == NULL) {  //如果遍历完所有地址后仍未成功绑定，设置错误信息
    anetSetError(err, "unable to bind socket");
    goto error; //跳转到error标签
  }

  error: s = ANET_ERR; //错误处理标签，将套接字描述符设置为ANET_ERR
  end: freeaddrinfo(servinfo); //结束处理标签，释放地址信息链表servinfo
  return s; //返回套接字描述符s
}
/*AF_INET是一个宏定义，用于指定地址族（Address Family）。在网络编程中，AF_INET表示IPv4地址族，
通常用于创建IPv4de套接字。地址族用于指定套接字所使用的协议族。*/
int anetTcpServer(char *err, int port, char *bindaddr, int backlog) {
  return _anetTcpServer(err, port, bindaddr, AF_INET, backlog);
}

int anetTcp6Server(char *err, int port, char *bindaddr, int backlog) {
  return _anetTcpServer(err, port, bindaddr, AF_INET6, backlog);
}

int anetUnixServer(char *err, char *path, mode_t perm, int backlog) {
  int s;
  struct sockaddr_un sa;

  if ((s = anetCreateSocket(err, AF_LOCAL)) == ANET_ERR)
    return ANET_ERR;

  memset(&sa, 0, sizeof(sa));
  sa.sun_family = AF_LOCAL;
  strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
  if (anetListen(err, s, (struct sockaddr*) &sa, sizeof(sa),
                 backlog) == ANET_ERR)
    return ANET_ERR;
  if (perm)
    chmod(sa.sun_path, perm);
  return s;
}

static int anetGenericAccept(char *err, int s, struct sockaddr *sa,
                             socklen_t *len) {
  int fd;
  while (1) {
    fd = accept(s, sa, len);
    if (fd == -1) {
      if (errno == EINTR)
        continue;
      else {
        anetSetError(err, "accept: %s", strerror(errno));
        return ANET_ERR;
      }
    }
    break;
  }
  return fd;
}

int anetTcpAccept(char *err, int s, char *ip, size_t ip_len, int *port) {
  int fd;
  struct sockaddr_storage sa;
  socklen_t salen = sizeof(sa);
  if ((fd = anetGenericAccept(err, s, (struct sockaddr*) &sa, &salen)) == -1)
    return ANET_ERR;

  if (sa.ss_family == AF_INET) {
    struct sockaddr_in *s = (struct sockaddr_in *) &sa;
    if (ip)
      inet_ntop(AF_INET, (void*) &(s->sin_addr), ip, ip_len);
    if (port)
      *port = ntohs(s->sin_port);
  } else {
    struct sockaddr_in6 *s = (struct sockaddr_in6 *) &sa;
    if (ip)
      inet_ntop(AF_INET6, (void*) &(s->sin6_addr), ip, ip_len);
    if (port)
      *port = ntohs(s->sin6_port);
  }
  return fd;
}

int anetUnixAccept(char *err, int s) {
  int fd;
  struct sockaddr_un sa;
  socklen_t salen = sizeof(sa);
  if ((fd = anetGenericAccept(err, s, (struct sockaddr*) &sa, &salen)) == -1)
    return ANET_ERR;

  return fd;
}

int anetPeerToString(int fd, char *ip, size_t ip_len, int *port) {
  struct sockaddr_storage sa;
  socklen_t salen = sizeof(sa);

  if (getpeername(fd, (struct sockaddr*) &sa, &salen) == -1)
    goto error;
  if (ip_len == 0)
    goto error;

  if (sa.ss_family == AF_INET) {
    struct sockaddr_in *s = (struct sockaddr_in *) &sa;
    if (ip)
      inet_ntop(AF_INET, (void*) &(s->sin_addr), ip, ip_len);
    if (port)
      *port = ntohs(s->sin_port);
  } else if (sa.ss_family == AF_INET6) {
    struct sockaddr_in6 *s = (struct sockaddr_in6 *) &sa;
    if (ip)
      inet_ntop(AF_INET6, (void*) &(s->sin6_addr), ip, ip_len);
    if (port)
      *port = ntohs(s->sin6_port);
  } else if (sa.ss_family == AF_UNIX) {
    if (ip)
      strncpy(ip, "/unixsocket", ip_len);
    if (port)
      *port = 0;
  } else {
    goto error;
  }
  return 0;

  error: if (ip) {
    if (ip_len >= 2) {
      ip[0] = '?';
      ip[1] = '\0';
    } else if (ip_len == 1) {
      ip[0] = '\0';
    }
  }
  if (port)
    *port = 0;
  return -1;
}

int anetSockName(int fd, char *ip, size_t ip_len, int *port) {
  struct sockaddr_storage sa;
  socklen_t salen = sizeof(sa);

  if (getsockname(fd, (struct sockaddr*) &sa, &salen) == -1) {
    if (port)
      *port = 0;
    ip[0] = '?';
    ip[1] = '\0';
    return -1;
  }
  if (sa.ss_family == AF_INET) {
    struct sockaddr_in *s = (struct sockaddr_in *) &sa;
    if (ip)
      inet_ntop(AF_INET, (void*) &(s->sin_addr), ip, ip_len);
    if (port)
      *port = ntohs(s->sin_port);
  } else {
    struct sockaddr_in6 *s = (struct sockaddr_in6 *) &sa;
    if (ip)
      inet_ntop(AF_INET6, (void*) &(s->sin6_addr), ip, ip_len);
    if (port)
      *port = ntohs(s->sin6_port);
  }
  return 0;
}
