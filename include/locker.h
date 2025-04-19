#ifndef LOCKER_H
#define LOCKER_H

#include<pthread.h>
#include<semaphore.h>
#include<exception>

// 线程同步机制封装类

// 互斥锁类
class locker {
private:
    pthread_mutex_t m_mutex;    // 互斥锁

public:
    // 初始化互斥锁
    locker() {
        if (pthread_mutex_init(&this->m_mutex, NULL) != 0) {
            throw std::exception();
        }
    }

    // 释放互斥锁
    ~locker() {
        pthread_mutex_destroy(&this->m_mutex);
    }

    // 对互斥锁加锁
    bool lock() {
        return pthread_mutex_lock(&this->m_mutex) == 0;
    }

    // 对互斥锁解锁
    bool unlock() {
        return pthread_mutex_unlock(&this->m_mutex) == 0;
    }

    // 获得互斥锁
    pthread_mutex_t* get_mutex() {
        return &this->m_mutex;
    }
};

// 条件变量类
class condition {
private:
    pthread_cond_t m_cond;      // 条件变量
public:
    // 初始化条件变量
    condition() {
        if (pthread_cond_init(&this->m_cond, NULL) != 0) {
            throw std::exception();
        }
    }

    // 释放条件变量
    ~condition() {
        pthread_cond_destroy(&this->m_cond);
    }

    // 通过条件变量阻塞线程
    bool wait(pthread_mutex_t* mutex) {
        return pthread_cond_wait(&this->m_cond, mutex) == 0;
    }

    // 通过条件变量阻塞线程指定的时间
    bool timedwait(pthread_mutex_t* mutex, struct timespec* abstime) {
        return pthread_cond_timedwait(&this->m_cond, mutex, abstime) == 0;
    }

    // 通过条件变量唤醒阻塞的线程
    bool signal() {
        return pthread_cond_signal(&this->m_cond) == 0;
    }

    // 通过条件变量唤醒所有阻塞的线程
    bool broadcast() {
        return pthread_cond_broadcast(&this->m_cond) == 0;
    }

};

// 信号量类
class semaphore {
private:
    sem_t m_sem;    // 信号量
public:
    // 初始化信号量1，实现默认构造函数
    semaphore() {
        if (sem_init(&this->m_sem, 0, 0) != 0) {
            throw std::exception();
        }
    }

    // 初始化信号量2
    semaphore(int num) {
        if (sem_init(&this->m_sem, 0, num) != 0) {
            throw std::exception();
        }
    }

    // 释放信号量
    ~semaphore() {
        sem_destroy(&this->m_sem);
    }

    // 申请信号量控制的共享资源
    bool wait() {
        return sem_wait(&this->m_sem) == 0;
    }

    // 释放信号量控制的共享资源
    bool post() {
        return sem_post(&this->m_sem) == 0;
    }
};

#endif