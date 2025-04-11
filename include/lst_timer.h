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
    sockaddr_in address;    // 客户端 socket 地址
    int sockfd;             // socket 文件描述符
    char buf[BUFFER_SIZE];  // 读缓存
    UtilTimer* timer;
}ClientData;


/*
    为不活跃的客户端连接创建定时器类
    - 当客户端没有向服务器发送请求时，定时器一直计时
        - 超时，关闭与客户端的 TCP 连接，释放文件描述符资源
    - 直到客户端重新向服务器发送请求，定时器重新计时
*/
class UtilTimer {
public:
    UtilTimer() : prev(NULL), next(NULL) {}
public:
    time_t expire;      // 任务超时时间，这里使用绝对时间
    void(*cb_func)(ClientData*);    // 函数指针，任务回调函数，回调函数处理的客户数据，由定时器的执行者传递给回调函数
    ClientData* user_data;
    UtilTimer* prev;    // 指向前一个定时器
    UtilTimer* next;    // 指向后一个定时器

};

// 定时器链表，它是一个升序、双向链表，且带有头结点和尾结点
class SortTimerLst {
public:
    SortTimerLst() :head(NULL), tail(NULL) {}
    // 链表被销毁时，删除其中所有的定时器
    ~SortTimerLst() {
        UtilTimer* tmp = this->head;
        while (tmp) {
            UtilTimer* del = tmp;
            tmp = tmp->next;
            delete del;
        }
    }

    // 将目标定时器 Timer 添加到链表中
    void add_tiemr(UtilTimer* timer) {
        if (timer == NULL) {
            return;
        }
        if (this->head == NULL) {
            this->head = timer;
            this->tail = this->head;
            return;
        }

        /*
            如果目标定时器的超时时间小于当前链表中所有定时器的超时时间，则把该定时器插入到链表头部，作为链表新的头结点
            否则就需要调用重载函数 add_timer() ，把它插入链表中合适的位置，以保证链表的升序特性
        */
        if (timer->expire <= this->head->expire) {
            timer->next = this->head;
            this->head->prev = timer;
            this->head = timer;
            return;
        }

        this->add_timer(timer, this->head);
    }

    /*
        当某个定时任务发生变化时，调整对应的定时器在链表中的位置。这个函数只考虑被调整的
        定时器的超时时间延长的情况，即该定时器需要往链表的尾部移动
    */
    void adjust_timer(UtilTimer* timer) {
        if (timer == NULL) {
            return;
        }
        UtilTimer* tmp = timer->next;

        /*
            如果被调整的目标定时器处在链表的尾部，或者该定时器新的超时时间值
            仍然小于其下一个定时器的超时时间，则不用调整
        */
        if (tmp->next == NULL || (timer->expire < tmp->expire)) {
            return;
        }

        if (timer == this->head) {
            // 如果目标定时器是链表的头结点，则将该定时器从链表中取出，并重新插入链表
            this->head = this->head->next;
            this->head->prev = NULL;
            timer->next = NULL;
            this->add_timer(timer, this->head);
        }
        else {
            // 如果目标定时器不是链表的头结点，则将该定时器从链表中取出，然后插入其原来所在位置后的部分链表中
            timer->prev->next = timer->next;
            timer->next->prev = timer->prev;
            timer->next = timer->prev = NULL;   // 取下的结点指针指向置 NULL
            this->add_timer(timer, tmp);
        }
    }

    // 将目标定时器 timer 从链表中删除
    void del_timer(UtilTimer* timer) {
        if (timer == NULL) {
            return;
        }

        // 链表中只有一个定时器，即目标定时器
        if (timer == this->head && timer == this->tail) {
            delete timer;
            this->head = this->tail = NULL;
            return;
        }

        /*
            如果链表中至少有两个定时器，且目标定时器是链表的头结点，
            则将链表的头结点重置为原头结点的下一个结点，然后删除目标定时器
        */
        if (timer == this->head) {
            this->head = this->head->next;
            this->head->prev = NULL;
            delete timer;
            return;
        }

        /*
            如果链表中至少有两个定时器，且目标定时器是链表的尾结点，
            则将链表的尾结点重置为原尾结点的前一个结点，然后删除目标定时器
        */
        if (timer == this->tail) {
            this->tail = this->tail->prev;
            this->tail->next = NULL;
            delete timer;
            return;
        }

        // 如果目标定时器位于链表的中间，则把它前后的定时器串联起来，然后删除目标定时器
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        delete timer;
    }

    /*
        SIGALRM 信号每次被触发就在其信号处理函数中执行一次 tick() 函数，以处理链表上到期的任务
    */
    void tick() {
        if (this->head == NULL) {
            // 没有需要处理的定时器任务
            return;
        }
        printf("timer tick.\n");
        time_t cur = time(NULL);    // 获取当前系统时间
        UtilTimer* tmp = this->head;

        // 从头结点开始，依次处理每个定时器，直到遇到一个尚未到期的定时器
        while (tmp) {
            /*
                因为每个定时器都使用绝对时间作为超时值，所以可以把定时器的超时值和
                系统当前时间比较，判断定时器是否到期
            */
            if (cur < tmp->expire) {
                break;
            }

            // 调用定时器的回调函数，以执行定时任务
            tmp->cb_func(tmp->user_data);

            // 执行完定时器中的定时任务之后，就将它从链表中删除，并重置链表头结点
            this->head = tmp->next;
            if (this->head != NULL) {
                this->head->prev = NULL;
            }
            delete tmp;
            tmp = this->head;     // tmp 重新赋值为 this->head
        }
    }

private:
    /*
        一个重载的辅助函数，它被公有的 add_timer 函数和 adjust_timer 函数调用
        该函数表示将目标定时器 timer 添加到结点 head 之后的部分链表中
    */
    void add_timer(UtilTimer* timer, UtilTimer* head) {
        UtilTimer* prev = head;     // 在一个结点的前面插入结点，需要记录该结点的前一个结点
        UtilTimer* tmp = prev->next;

        /*
            遍历 head 结点之后的部分链表，直到找到一个超时时间，大于目标定时器的超时时间结点，
            并将目标定时器插入该结点之前
        */
        while (tmp) {
            if (timer->expire < tmp->expire) {
                prev->next = tmp;
                timer->next = tmp;
                timer->prev = prev;
                tmp->prev = timer;
                break;
            }
            prev = tmp;
            tmp = tmp->next;
        }

        /*
            如果遍历完 head 结点之后的部分链表，仍未找到超时时间大于目标定时器的超时时间的结点，
            则将目标定时器插入到链表尾部，并把它设置为链表新的尾结点
        */
        if (tmp == NULL) {
            prev->next = timer;
            timer->next = NULL;
            timer->prev = prev;
            // 更新尾指针
            this->tail = timer;
        }
    }
private:
    UtilTimer* head;    // 头结点
    UtilTimer* tail;    // 尾结点
};

#endif 