#ifndef LOCKER_H
#define LOCKER_H

#include<pthread.h>
#include<semaphore.h>
#include<exception>

// 线程同步机制封装类

// 互斥锁类
class locker {
public:
    locker() {
        // 初始化互斥锁
        if (pthread_mutex_init(&this->m_mutex, NULL) != 0) {
            throw std::exception();
        }
    }

    ~locker() {
        // 释放互斥锁
        pthread_mutex_destroy(&this->m_mutex);
    }

    bool lock() {
        // 判断互斥锁是否已经使用
        return pthread_mutex_lock(&this->m_mutex) == 0;
    }

    bool unlock() {
        // 判断互斥锁是否已经被释放
        return pthread_mutex_unlock(&this->m_mutex) == 0;
    }

    pthread_mutex_t* get_mutex() {
        // 获得互斥锁
        return &this->m_mutex;
    }

private:
    pthread_mutex_t m_mutex;
};

// 条件变量类
class condition {
public:
    condition() {
        // 初始化条件变量
        if (pthread_cond_init(&this->m_cond, NULL) != 0) {
            throw std::exception();
        }
    }

    ~condition() {
        // 释放条件变量
        pthread_cond_destroy(&this->m_cond);
    }

    bool wait(pthread_mutex_t* mutex) {
        // 通过条件变量阻塞线程
        return pthread_cond_wait(&this->m_cond, mutex) == 0;
    }

    bool timedwait(pthread_mutex_t* mutex, struct timespec* abstime) {
        // 通过条件变量阻塞线程指定的时间
        return pthread_cond_timedwait(&this->m_cond, mutex, abstime) == 0;
    }

    bool signal() {
        // 通过条件变量唤醒阻塞的线程
        return pthread_cond_signal(&this->m_cond) == 0;
    }

    bool broadcast() {
        // 通过条件变量唤醒所有阻塞的线程
        return pthread_cond_broadcast(&this->m_cond) == 0;
    }

private:
    pthread_cond_t m_cond;
};

// 信号量类
class semaphore {
public:
    semaphore() {
        // 初始化信号量1
        if (sem_init(&this->m_sem, 0, 0) != 0) {
            throw std::exception();
        }
    }

    semaphore(int num) {
        // 初始化信号量2
        if (sem_init(&this->m_sem, 0, num) != 0) {
            throw std::exception();
        }
    }

    ~semaphore() {
        // 释放信号量
        sem_destroy(&this->m_sem);
    }

    bool wait() {
        // 申请信号量控制的共享资源
        return sem_wait(&this->m_sem) == 0;
    }

    bool post() {
        // 释放信号量控制的共享资源
        return sem_post(&this->m_sem) == 0;
    }

private:
    sem_t m_sem;
};

#endif