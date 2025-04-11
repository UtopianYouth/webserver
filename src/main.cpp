#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include<string.h>
#include<arpa/inet.h>
#include<errno.h>
#include<fcntl.h>
#include<sys/epoll.h>
#include<signal.h>
#include"../include/locker.h"
#include"../include/thread_pool.h"
#include"../include/http_connection.h"

#define MAX_FD 65536        //支持最大的文件描述符个数（最大的连接客户端数）
#define MAX_EVENT_NUMBER 10000      // epoll 监听的最大的 IO 事件数量

// 添加信号捕捉
void addsig(int sig, void(handler)(int)) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    sigfillset(&sa.sa_mask);    // 设置阻塞信号集
    assert(sigaction(sig, &sa, NULL) != -1);
}

// 添加文件描述符到 epoll 对象中
extern void add_fd_epoll(int epoll_fd, int fd, bool one_shot);

// 从 epoll 对象中删除文件描述符
extern void remove_fd_epoll(int epoll_fd, int fd);

// 修改 epoll 对象中的文件描述符
extern void modify_fd_epoll(int epoll_fd, int fd, int event_num);

int main(int argc, char* argv[]) {
    if (argc <= 1) {
        printf("按照如下格式运行：%s port_number\n", basename(argv[0]));
        exit(-1);
    }

    // 获取端口号
    int port = atoi(argv[1]);

    // 对 SIGPIPE 信号进行处理
    addsig(SIGPIPE, SIG_IGN);

    // 创建线程池，初始化线程池
    ThreadPool<HttpConnection>* pool = NULL;
    try {
        pool = new ThreadPool<HttpConnection>;
    }
    catch (...) {
        // 创建线程池失败，参数 ... 表示捕获所有类型的异常
        exit(-1);
    }

    // 创建一个数组用于保存所有的客户端信息
    HttpConnection* users = new HttpConnection[MAX_FD];

    // 创建监听用的文件描述符
    int listen_fd = socket(PF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        perror("socket");
        exit(-1);
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    // 设置端口复用
    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 绑定监听用的文件描述符
    int ret1 = bind(listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (ret1 == -1) {
        perror("bind");
        exit(-1);
    }

    // 监听
    int ret2 = listen(listen_fd, 64);
    if (ret2 == -1) {
        perror("listen");
        exit(-1);
    }

    // 创建 epoll 对象，事件数组，添加
    epoll_event events[MAX_EVENT_NUMBER];
    int epoll_fd = epoll_create(5);

    // 初始化 HttpConnection 的 static 参数
    HttpConnection::m_epoll_fd = epoll_fd;

    // 将监听的文件描述符添加到 epoll 对象中，监听的文件描述符不需要 EPOLLONESHOT
    add_fd_epoll(epoll_fd, listen_fd, false);


    // 检测 epoll 对象中的 IO 缓冲区变化
    while (1) {
        int num = epoll_wait(epoll_fd, events, MAX_EVENT_NUMBER, -1);
        if ((num < 0) && (errno != EINTR)) {
            // 被中断，或者 epoll_wait() 出错
            printf("epoll failure.\n");
            break;
        }

        // 循环遍历 epoll 对象的 IO 事件数组
        for (int i = 0;i < num;++i) {

            int sockfd = events[i].data.fd;

            if (sockfd == listen_fd) {
                // 有新的客户端连接
                struct sockaddr_in client_addr;
                socklen_t addr_len = sizeof(client_addr);
                int communication_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &addr_len);
                if (communication_fd == -1) {
                    perror("accept");
                    exit(-1);
                }

                if (HttpConnection::m_user_count > MAX_FD) {
                    // 目前客户端的连接数已满，给客户端发送提示
                    close(communication_fd);
                    continue;
                }

                // 将新的客户端连接数据初始化，在数组中保存客户端的连接信息
                users[communication_fd].init(communication_fd, client_addr);
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                // 客户端发生异常断开或者错误等事件
                users[sockfd].close_connection();
            }
            else if (events[i].events & EPOLLIN) {
                // 通信文件描述符读缓冲区有数据
                if (users[sockfd].read()) {
                    // 一次性把所有数据读完，users + sockfd 找到发生 EPOLLIN 事件的文件描述符所在内存地址
                    pool->append(users + sockfd);
                }
                else {
                    users[sockfd].close_connection();
                }
            }
            else if (events[i].events & EPOLLOUT) {
                if (!users[sockfd].write()) {
                    users[sockfd].close_connection();
                }
            }

        }

    }

    close(epoll_fd);
    close(listen_fd);

    delete[] users;
    delete pool;

    return 0;
}