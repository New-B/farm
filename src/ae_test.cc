#include "ae.h"
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

// 读事件处理函数
void readFileProc(aeEventLoop *eventLoop, int fd, void *clientData, int mask) {
    char buffer[128];
    int n = read(fd, buffer, sizeof(buffer));
    if (n > 0) {
        buffer[n] = '\0';
        printf("Read from file: %s\n", buffer);
    } else {
        // 读取完成或出错，删除文件事件
        aeDeleteFileEvent(eventLoop, fd, AE_READABLE);
        close(fd);
    }
}

// 时间事件处理函数
int timeEventProc(aeEventLoop *eventLoop, long long id, void *clientData) {
    printf("Time event triggered: %lld\n", id);
    return 1000; // 1秒后再次触发
}

// 时间事件终结器函数
void timeEventFinalizerProc(aeEventLoop *eventLoop, void *clientData) {
    printf("Time event finalized\n");
}

int main() {
    // 创建事件循环
    aeEventLoop *eventLoop = aeCreateEventLoop(1024);

    // 打开文件
    int fd = open("test.txt", O_RDONLY);
    if (fd == -1) {
        perror("open");
        return 1;
    }

    // 注册读事件
    if (aeCreateFileEvent(eventLoop, fd, AE_READABLE, readFileProc, NULL) == AE_ERR) {
        fprintf(stderr, "Could not create file event.\n");
        close(fd);
        return 1;
    }

    // 注册时间事件
    long long id = aeCreateTimeEvent(eventLoop, 1000, timeEventProc, NULL, timeEventFinalizerProc);
    if (id == AE_ERR) {
        fprintf(stderr, "Could not create time event.\n");
        return 1;
    }

    // 启动事件循环
    aeMain(eventLoop);

    // 删除事件循环
    aeDeleteEventLoop(eventLoop);

    return 0;
}