// Copyright (c) 2018 The GAM Authors 
//2024.12.13
//文件实现了一个简单的日志记录系统，支持记录不同级别的日志消息，并提供了堆栈跟踪功能。
//日志消息可以输出到标准输出、标准错误或者指定的文件中。
//堆栈跟踪功能可以在程序发生错误时帮助开发者定位问题。
#include <cstdarg>
#include <unistd.h>
#include <syscall.h>
#include <sys/time.h>
#include <execinfo.h>
#include "log.h"
#include "gallocator.h"

/*用于记录原始日志消息
根据日志级别选择输出目标（标准输出、标准错误或文件）
获取当前时间并格式化为字符串
将日志消息写入目标文件或输出流、并刷新缓冲区*/
void _epicLogRaw(int level, const char* msg) {
  const char *c = ".-*#";
  FILE *fp;
  char buf[128];

  fp =
      (GAllocFactory::LogFile() == nullptr) ?
          (level <= LOG_FATAL ? stderr : stdout) :
          fopen(GAllocFactory::LogFile()->c_str(), "a");
  if (!fp)  
    return;

  int off;
  struct timeval tv;

  gettimeofday(&tv, NULL);
  off = strftime(buf, sizeof(buf), "%d %b %H:%M:%S.", localtime(&tv.tv_sec));
  snprintf(buf + off, sizeof(buf) - off, "%03d", (int) tv.tv_usec / 1000);
  //fprintf(fp,"[%d] %s %c %s\n",(int)getpid(),buf,c[level],msg);
  fprintf(fp, "[%d] %s %c %s\n", (int) syscall(SYS_gettid), buf, c[level], msg);

  fflush(fp);

  if (GAllocFactory::LogFile())
    fclose(fp);
}

/*用于记录格式化的日志消息
检查日志级别是否需要记录
使用可变参数列表格式化日志消息，包括文件名、行号和函数名
调用_epicLogRaw函数记录格式化后的日志消息*/
void _epicLog(char* file, char* func, int lineno, int level, const char *fmt,
              ...) {
  if (level > GAllocFactory::LogLevel())
    return;

  va_list ap;
  char msg[MAX_LOGMSG_LEN];

  int n = sprintf(msg, "[%s:%d-%s()] ", file, lineno, func);
  va_start(ap, fmt);
  vsnprintf(msg + n, MAX_LOGMSG_LEN - n, fmt, ap);
  va_end(ap);

  _epicLogRaw(level, msg);
}

/*用于打印当前的堆栈跟踪
使用backtrace函数获取当前调用堆栈的地址
使用backtrace_symbols函数将地址转换为可读的符号信息
打印堆栈跟踪信息，包括每个调用帧的符号信息*/
void PrintStackTrace() {
  printf("\n***************Start Stack Trace******************\n");
  int size = 100;
  void *buffer[100];
  char **strings;
  int j, nptrs;
  nptrs = backtrace(buffer, size);
  printf("backtrace() returned %d addresses\n", nptrs);
  strings = backtrace_symbols(buffer, nptrs);
  if (strings == NULL) {
    perror("backtrace_symbols");
    exit(EXIT_FAILURE);
  }
  for (j = 0; j < nptrs; j++) {
    printf("%s\n", strings[j]);
  }
  free(strings);

  printf("\n***************End Stack Trace******************\n");
}

