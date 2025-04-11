#ifndef THREADPOOL_H
#define THREADPOOL_H

#include<pthread.h>
#include<exception>
#include<cstdio>
#include<list>
#include"locker.h"

// 线程池类，定义成模板类是为了代码复用，模板参数 T 是任务类
template<typename T>
class ThreadPool {
public:
    // thread_number 是线程池中线程的数量， max_requests 是请求队列中最多允许的、等待处理的请求的数量 
    ThreadPool(int thread_number = 8, int max_requests = 10000);
    ~ThreadPool();
    bool append(T* request);

private:
    // cpp 中线程的逻辑函数必须是静态的，工作线程运行函数，不断从工作队列中取出任务并执行
    static void* worker(void* arg);
    // 线程池运行
    void run();
private:
    // 线程池中线程的数量
    int m_thread_number;

    // 线程池数组，大小为 m_thread_number
    pthread_t* m_threads;

    // 请求队列中，最多允许等待处理的请求数量
    int m_max_requests;

    // 请求队列
    std::list<T*>m_workqueue;

    // 保护请求队列的互斥锁
    locker m_queuelocker;

    // 信号量判断是否有任务需要处理
    semaphore m_queuestat;

    // 是否结束线程
    bool m_stop;
};

template<typename T>
ThreadPool<T>::ThreadPool(int thread_number, int max_requests) {
    // 初始化线程池属性
    if (thread_number <= 0 || max_requests <= 0) {
        throw std::exception();
    }

    this->m_thread_number = thread_number;
    this->m_max_requests = max_requests;
    this->m_stop = false;
    this->m_threads = new pthread_t[this->m_thread_number];

    if (!this->m_threads) {
        throw std::exception();
    }

    // 创建 thread_number 个线程，并将它们设置为线程脱离
    for (int i = 0;i < thread_number;++i) {
        printf("create the %d thread.\n", i + 1);
        if (pthread_create(this->m_threads + i, NULL, worker, this)) {
            delete[] this->m_threads;
            throw std::exception();
        }

        // 设置线程分离
        if (pthread_detach(this->m_threads[i])) {
            delete[] this->m_threads;
            throw std::exception();
        }
    }

}

template<typename T>
ThreadPool<T>::~ThreadPool() {
    // 释放线程池资源
    delete[] this->m_threads;
    this->m_stop = true;
}

template<typename T>
bool ThreadPool<T>::append(T* request) {
    // 将任务类对象加入请求队列，操作工作队列时一定要加锁，因为它被所有线程共享
    this->m_queuelocker.lock();
    if (this->m_workqueue.size() > this->m_max_requests) {
        this->m_queuelocker.unlock();
        return false;
    }
    this->m_workqueue.push_back(request);
    this->m_queuelocker.unlock();
    this->m_queuestat.post();
    return true;
}

template<typename T>
void* ThreadPool<T>::worker(void* arg) {
    // static 修饰的成员函数不能访问成员变量，线程逻辑只能通过 arg 传递过来的 this 指针访问
    ThreadPool* pool = (ThreadPool*)arg;
    pool->run();
    return pool;
}

template<typename T>
void ThreadPool<T>::run() {
    while (!this->m_stop) {
        // 线程从任务队列中取出一个任务运行
        this->m_queuestat.wait();
        this->m_queuelocker.lock();
        if (this->m_workqueue.empty()) {
            // 任务队列中没有需要处理的任务对象
            this->m_queuelocker.unlock();
            continue;
        }

        T* request = this->m_workqueue.front();
        this->m_workqueue.pop_front();
        this->m_queuelocker.unlock();

        if (!request) {
            continue;
        }

        // 运行请求队列中的任务
        request->process();
    }
}

#endif