#ifndef LST_TIMER
#define LST_TIMER

#include<stdio.h>
#include<time.h>
#include<signal.h>
#include<arpa/inet.h>

#define BUFFER_SIZE 64
class UtilTimer;        // 前向声明

// 用户数据结构
typedef struct ClientData {
    struct sockaddr_in address;    // 客户端 socket 地址
    int sockfd;             // socket 文件描述符
    char buf[BUFFER_SIZE];  // 读缓存
    UtilTimer* timer;       // 每一个客户端连接对应一个定时器
}ClientData;


/*
    为不活跃的客户端连接创建定时器类
    - 当客户端没有向服务器发送请求时，定时器一直计时
        - 超时，关闭与客户端的 TCP 连接，释放文件描述符资源
    - 直到客户端重新向服务器发送请求，定时器重新计时
*/
class UtilTimer {
public:
    UtilTimer* prev;    // 指向前一个定时器
    UtilTimer* next;    // 指向后一个定时器
    time_t expire;      // 任务超时时间，这里使用绝对时间
    ClientData* user_data;  // 客户端连接信息
public:
    UtilTimer() : prev(NULL), next(NULL) {}
public:
    void(*cb_func)(ClientData*);    // 函数指针，任务回调函数，回调函数处理的客户数据，由定时器的执行者传递给回调函数

};

// 定时器链表，它是一个带有头尾节点的升序、双向链表
class SortTimerLst {
private:
    UtilTimer* head;     // 头节点指针
    UtilTimer* tail;     // 尾节点指针

public:
    SortTimerLst();

    // 链表被销毁时，删除其中所有的定时器
    ~SortTimerLst();

    // 将目标定时器 Timer 添加到链表中
    void add_tiemr(UtilTimer* timer);

    /*
        当某个定时任务发生变化时，调整对应的定时器在链表中的位置。这个函数只考虑被调整的
        定时器的超时时间延长的情况，即该定时器需要往链表的尾部移动
    */
    void adjust_timer(UtilTimer* timer);

    // 将目标定时器 timer 从链表中删除
    void del_timer(UtilTimer* timer);

    /*
        SIGALRM 信号每次被触发就在其信号处理函数中执行一次 tick() 函数，以处理链表上到期的任务
    */
    void tick();

private:
    /*
        一个重载的辅助函数，它被公有的 add_timer 函数和 adjust_timer 函数调用
        该函数表示将目标定时器 timer 添加到结点 head 之后的部分链表中
    */
    void add_timer(UtilTimer* timer, UtilTimer* head);
};

#endif 