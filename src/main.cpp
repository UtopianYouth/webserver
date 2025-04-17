#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include<string.h>
#include<arpa/inet.h>
#include<errno.h>
#include<fcntl.h>
#include<sys/epoll.h>
#include<signal.h>
#include"../include/thread_pool.h"
#include"../include/http_connection.h"
#include "../include/lst_timer.h"

#define MAX_FD 65536                //支持最大的文件描述符个数（最大的连接客户端数）
#define MAX_EVENT_NUMBER 10000      // epoll 监听的最大的 IO 事件数量
#define TIMESLOT 5                  // 定时器发送信号的间隔时间（秒）


static int pipefd[2];               // 定时器发送的信号通过管道传输，0 是读端，1是写端
static SortTimerLst timer_lst;      // 定时器双向链表，一个客户端的 TCP 连接对应一个定时器
static int epoll_fd = 0;            // epoll 事件
HttpConnection* users = new HttpConnection[MAX_FD];     // 客户端的 TCP 连接任务类对象
ClientData* lst_users = new ClientData[MAX_FD];         // 定时器客户端信息类对象

// 添加信号捕捉
void addsig(int sig, void(handler)(int)) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    sigfillset(&sa.sa_mask);    // 设置阻塞信号集
    assert(sigaction(sig, &sa, NULL) != -1);
}

// 添加信号处理函数，管道写端
void sig_handler(int sig) {
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char*)&msg, 1, 0);
    errno = save_errno;
}

// SIGALARM 信号处理函数
void timer_handler() {
    // 定时处理任务，实际上就是调用tick()函数
    timer_lst.tick();
    // 因为一次 alarm 调用只会引起一次SIGALARM 信号，所以我们要重新定时，以不断触发 SIGALARM信号。
    alarm(TIMESLOT);
}

// 定时器回调函数，删除超时连接的 socket 上的注册事件
extern void cb_func(ClientData* user_data) {
    users[user_data->sockfd].close_connection();
}

// 设置文件描述符非阻塞
extern int set_non_blocking(int fd);

// 添加文件描述符到 epoll 对象中
extern void add_fd_epoll(int epoll_fd, int fd, bool one_shot);

// 从 epoll 对象中删除文件描述符
extern void remove_fd_epoll(int epoll_fd, int fd);

// 修改 epoll 对象中的文件描述符
extern void modify_fd_epoll(int epoll_fd, int fd, int event_num);


int main(int argc, char* argv[]) {
    if (argc <= 1) {
        printf("Usage: %s port_number\n", basename(argv[0]));
        exit(-1);
    }

    // 获取端口号
    int port = atoi(argv[1]);

    // 对 SIGPIPE 信号进行处理
    // SIGPIPE: Broken pipe 向一个没有读端的管道写数据
    addsig(SIGPIPE, SIG_IGN);
    addsig(SIGALRM, sig_handler);
    addsig(SIGTERM, sig_handler);
    bool stop_server = false;

    // 创建 epoll 对象，事件数组，添加
    epoll_event events[MAX_EVENT_NUMBER];

    // 创建 epoll 对象，参数可以是任何大于 0 的值
    epoll_fd = epoll_create(5);

    // 创建线程池，初始化线程池
    ThreadPool<HttpConnection>* pool = NULL;
    try {
        pool = new ThreadPool<HttpConnection>;
    }
    catch (...) {
        // 创建线程池失败，参数 ... 表示捕获所有类型的异常
        exit(-1);
    }

    // 创建管道
    int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    set_non_blocking(pipefd[1]);
    add_fd_epoll(epoll_fd, pipefd[0], false);

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
    int ret2 = listen(listen_fd, 1024);
    if (ret2 == -1) {
        perror("listen");
        exit(-1);
    }

    // 将监听的文件描述符添加到 epoll 对象中，监听的文件描述符不需要 EPOLLONESHOT
    add_fd_epoll(epoll_fd, listen_fd, false);

    // 初始化 HttpConnection 的 static 参数
    HttpConnection::m_epoll_fd = epoll_fd;

    bool timeout = false;
    alarm(TIMESLOT);

    // 检测 epoll 对象中的 IO 缓冲区变化
    while (!stop_server) {
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
                if (communication_fd < 0) {
                    perror("accept");
                    printf("errno is %d.\n", errno);
                    continue;
                }

                if (HttpConnection::m_user_count >= MAX_FD) {
                    // 客户端的连接数已满
                    close(communication_fd);
                    continue;
                }

                // 将新的客户端连接数据初始化，在数组中保存客户端的连接信息
                users[communication_fd].init(communication_fd, client_addr);

                // 定时器需要的 ClientData 初始化
                lst_users[communication_fd].address = client_addr;
                lst_users[communication_fd].sockfd = communication_fd;

                // 创建定时器，设置其回调函数与超时时间，然后绑定定时器与用户数据，最后将定时器添加到链表 timer_lst 中
                UtilTimer* timer = new UtilTimer;
                timer->user_data = &lst_users[communication_fd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);    // 获取当前系统时间
                timer->expire = cur + 3 * TIMESLOT;
                lst_users[communication_fd].timer = timer;
                timer_lst.add_timer(timer);

                printf("communication_fd = %d, addr = %s.\n", communication_fd, inet_ntoa(client_addr.sin_addr));
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                // 客户端发生异常断开或者错误等事件
                users[sockfd].close_connection();
            }
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN)) {
                // 处理信号
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1 || ret == 0) {
                    continue;
                }
                else {
                    for (int i = 0;i < ret;++i) {
                        switch (signals[i]) {
                        case SIGALRM:
                            // 用 timeout 标记有定时任务需要处理，但不立即处理定时任务
                            // 这是因为定时任务的优先级不是很高，程序优先处理其它更重要的任务
                            timeout = true;
                            break;
                        case SIGTERM:
                            stop_server = true;
                        }
                    }
                }
            }
            else if (events[i].events & EPOLLIN) {
                // 通信文件描述符读缓冲区有数据
                UtilTimer* timer = lst_users[sockfd].timer;
                if (users[sockfd].read()) {
                    // 一次性把所有数据读完，users + sockfd 找到发生 EPOLLIN 事件（发送 HTTP 请求）的客户端
                    pool->append(users + sockfd);

                    // 成功读取数据，调整该连接对应的定时器，以延迟该连接被关闭的时间（客户端还在活跃）
                    if (timer) {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        //printf("adjust timer once.\n");
                        timer_lst.adjust_timer(timer);
                    }
                }
                else {
                    // 移除定时器
                    if (timer) {
                        timer_lst.del_timer(timer);
                    }
                    users[sockfd].close_connection();
                }
            }
            else if (events[i].events & EPOLLOUT) {
                if (!users[sockfd].write()) {
                    // 如果客户端的 keep-alive = false，只写一次 HTTP 响应
                    users[sockfd].close_connection();
                }
            }

        }

        // 最后处理定时事件，因为 I/O 事件有更高的优先级，当然，这样做会存在定时误差
        if (timeout) {
            timer_handler();
            timeout = false;
        }

    }

    close(epoll_fd);
    close(listen_fd);
    close(pipefd[1]);
    close(pipefd[0]);

    // 工作任务对象数组
    delete[] users;

    // 定时器用户信息对象数组
    delete[] lst_users;

    // 线程池对象
    delete pool;

    return 0;
}