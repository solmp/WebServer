#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <signal.h>

#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"

#define MAX_FD 65535                // 最大的文件描述符个数
#define MAX_EVENT_NUMER 10000      // 监听的最大事件数
// 增加信号捕捉
void add_sig(int sig, void(handle)(int)) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sigfillset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

// 增加文件标识符到epoll中
extern void addfd(int epollfd, int fd, bool one_shot);
// 从epoll中删除文件描述符
extern void removefd(int epollfd, int fd);
// 修改文件描述符
extern void modifyfd(int epollfd, int fd, int ev);

int main(int argc, char* argv[]) {
    if(argc <= 1) {
        printf("按照以下格式运行：%s port_number\n", basename(argv[0]));
        exit(-1);
    }
    // 获取端口号
    int port = atoi(argv[1]);
    
    // 对SIGPIE信号进行处理
    add_sig(SIGPIPE, SIG_IGN);

    // 创建线程池，初始化线程池
    // 任务、信息都放在http_conn中，分开更好
    threadpool<http_conn> *pool = NULL;
    try {
        pool = new threadpool<http_conn>();  
    } catch(...) {
        exit(-1);
    }

    // 创建一个数组用于保存所有的客户端信息
    http_conn *users = new http_conn[MAX_FD];
    
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);

    // 设置端口复用 - 绑定前
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 绑定
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    bind(listenfd, (struct sockaddr*)&address, sizeof(address));

    // 监听
    listen(listenfd, 5);

    // 创建epoll对象，事件数组，添加
    epoll_event events[MAX_EVENT_NUMER];
    int epollfd = epoll_create(5);

    // 将监听的文件描述符添加到epoll对象中，后续也需要添加别的描述符
    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;

    // 主线程不断循环检测事件
    while(true) {
        int num = epoll_wait(epollfd, events, MAX_EVENT_NUMER, -1);  
        if(num < 0 && errno != EINTR) {
            printf("epoll failure\n");
            break;
        }

        // 循环遍历事件数组
        for(int i=0; i<num; ++i) {
            int sockfd = events[i].data.fd;
            if(sockfd == listenfd) {
                // 有客户端连接进来
                struct sockaddr_in client_address;
                socklen_t client_addrlen = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_addrlen);
                
                if(http_conn::m_user_count >= MAX_FD) {
                    // 目前连接数已满，给客户端响应信息（响应报文，如：服务器内部正忙）
                    close(connfd);
                    continue;
                }

                // 将新客户数据初始化后放入数组
                users[connfd].init(connfd, client_address);
            } else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                // 对方异常断开或错误等事件
                users[sockfd].close_conn();
            } else if(events[i].events & EPOLLIN) {
                if(users[sockfd].read()) {
                    // 一次性读完所有数据
                    pool->append(users + sockfd);
                }else {
                    users[sockfd].close_conn();
                }
            } else if(events[i].events & EPOLLOUT) {
                // 一次性写完所有数据
                if(!users[sockfd].write()) {
                    users[sockfd].close_conn();
                }
            }
        }
    }

    close(epollfd);
    close(listenfd);
    delete[] users;
    delete pool;
    return 0;
}