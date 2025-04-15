#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <pthread.h>
#include "../include/lst_timer.h"

#define FD_LIMIT 65535
#define MAX_EVENT_NUMBER 1024
#define TIMESLOT 5

static int pipefd[2];               // 管道，0是读端，1是写端
static SortTimerLst timer_lst;      // 定时器链表
static int epollfd = 0;

// 设置文件描述符非阻塞
int set_non_blocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}


// 往 epoll 对象中添加监听的文件描述符
void add_epoll_fd(int epollfd, int fd) {
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET;   // 设置边沿触发
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    set_non_blocking(fd);
}

// 信号处理函数
void sig_handler(int sig) {
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char*)&msg, 1, 0);     // 向管道的写端写入数据
    errno = save_errno;
}

// 添加检测的信号
void addsig(int sig) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = sig_handler;
    sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

void timer_handler() {
    // 定时处理任务，实际上就是调用 tick() 函数
    timer_lst.tick();

    //  因为一次 alarm 调用只会引起一次 SIGALRM 信号，所以我们要重新定时，以不断触发 SIGALRM 信号
    alarm(TIMESLOT);
}

// 定时器回调函数，删除非活动连接 socket 上的注册事件，并关闭
void cb_func(ClientData* user_data) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    printf("close fd %d.\n", user_data->sockfd);
}

int main(int argc, char* argv[]) {
    if (argc <= 1) {
        printf("usage: %s port_number\n", basename(argv[0]));
        return 1;
    }

    int port = atoi(argv[1]);
    int ret = 0;

    int listen_fd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listen_fd >= 0);

    struct sockaddr_in server_addr;
    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    ret = bind(listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    assert(ret != -1);

    ret = listen(listen_fd, 5);
    assert(ret != -1);

    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    assert(epollfd != -1);
    add_epoll_fd(epollfd, listen_fd);

    // 创建管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    set_non_blocking(pipefd[1]);
    add_epoll_fd(epollfd, pipefd[0]);

    // 设置信号处理函数
    addsig(SIGALRM);
    addsig(SIGTERM);
    bool stop_server = false;

    ClientData* users = new ClientData[FD_LIMIT];
    bool timeout = false;
    alarm(TIMESLOT);        // 定时，5秒后产生 SIGALRM 信号

    while (!stop_server) {
        // 检测发生变化的文件描述符数量
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR) {
            printf("epoll failure.\n");
            break;
        }

        for (int i = 0;i < number;++i) {
            int sockfd = events[i].data.fd;
            if (sockfd == listen_fd) {
                struct sockaddr_in client_addr;
                socklen_t client_addr_len = sizeof(client_addr);
                int communication_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_addr_len);
                add_epoll_fd(epollfd, communication_fd);
                users[communication_fd].address = client_addr;
                users[communication_fd].sockfd = communication_fd;

                // 创建定时器，设置其回调函数与超时时间，然后绑定定时器与用户数据，最后将定时器添加到链表 timer_lst 中
                UtilTimer* timer = new UtilTimer;
                timer->user_data = &users[communication_fd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);    // 获取当前系统时间
                timer->expire = cur + 3 * TIMESLOT;
                users[communication_fd].timer = timer;
                timer_lst.add_tiemr(timer);
            }
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN)) {
                // 处理信号
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1) {
                    continue;
                }
                else if (ret == 0) {
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
                memset(users[sockfd].buf, '\0', BUFFER_SIZE);
                ret = recv(sockfd, users[sockfd].buf, BUFFER_SIZE - 1, 0);
                printf("get %d bytes of client data %s from data %d.\n", ret, users[sockfd].buf, sockfd);
                UtilTimer* timer = users[sockfd].timer;
                if (ret < 0) {
                    // 如果发生读错误，则关闭连接，并移除对应的定时器
                    if (errno != EAGAIN) {
                        cb_func(&users[sockfd]);
                        if (timer) {
                            timer_lst.del_timer(timer);
                        }
                    }
                }
                else if (ret == 0) {
                    // 如果对方已经关闭连接，则我们也关闭连接，并移除对应的定时器
                    cb_func(&users[sockfd]);
                    if (timer) {
                        timer_lst.del_timer(timer);
                    }
                }
                else {
                    // 如果某个客户端上有数据可读，则我们要调整该连接对应的定时器，以延迟该连接被关闭的时间（客户端还在活跃）
                    if (timer) {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        printf("adjust timer once.\n");
                        timer_lst.adjust_timer(timer);
                    }
                }
            }
        }

        // 最后处理定时事件，因为 I/O 事件有更高的优先级。当然，这样做将导致定时任务不能精准的按照预定的时间执行
        if (timeout) {
            timer_handler();
            timeout = false;
        }
    }

    close(listen_fd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete[] users;
    return 0;
}

