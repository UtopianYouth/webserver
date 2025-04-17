#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/uio.h>
#include "locker.h"

// 任务类，每一个对象处理客户端的一个 HTTP 请求
class HttpConnection {
public:
    static int m_epoll_fd;      // 所有客户端通信对应 socket 上的事件都被注册到同一个 epoll 对象中，所以设置成静态的
    static int m_user_count;    // 统计客户端的数量

    static const int READ_BUFFER_SIZE = 4096;   // 读缓冲区大小
    static const int WRITE_BUFFER_SIZE = 2048;  // 写缓冲区大小
    static const int FILENAME_LEN = 200;        // 文件名的最大长度

    // HTTP 请求方法，目前只支持 GET
    enum METHOD {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT
    };


    /*
        解析客户端请求时，主状态机的状态
        - CHECK_STATE_REQUESTLINE: 当前正在分析请求行
        - CHECK_STATE_HEADER: 当前正在分析请求头部字段
        - CHECK_STATE_CONTENT: 当前正在解析请求体
    */
    enum CHECK_STATE {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    };

    /*
        服务器处理 HTTP 请求的可能结果，报文解析的结果
        - NO_REQUEST: 请求不完整，需要继续读取客户端数据
        - GET_REQUEST: 表示获得了一个完整的客户端请求
        - BAD_REQUEST: 表示客户端请求语法错误
        - NO_RESOURCE: 表示服务器没有资源
        - FORBIDDEN_REQUEST: 表示客户端对资源没有足够的访问权限
        - FILE_REQUEST: 文件请求，获取文件成功
        - INTERNAL_ERROR: 表示服务器内部错误
        - CLOSED_CONNECTION: 表示客户端已经关闭连接了
    */
    enum HTTP_CODE {
        NO_REQUEST = 0,
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION
    };

    /*
        从状态机的三种可能状态，即行的读取状态，分别表示：
        - LINE_OK: 读取到一个完整的行
        - LINE_BAD: 行出错
        - LINE_OPEN: 行数据尚且不完整（没有读到完整的行，还需要继续读取）
    */
    enum LINE_STATUS {
        LINE_OK = 0,
        LINE_BAD,
        LINE_OPEN
    };

private:
    int m_sockfd;               // 客户端 HTTP 连接对应的文件描述符
    struct sockaddr_in m_client_addr;   // 客户端通信的 socket 地址
    char m_read_buf[READ_BUFFER_SIZE];  // 读缓冲区
    int m_read_index;           // 记录从读缓冲区已经读取的数据字节的下一个位置
    int m_checked_index;        // 当前正在分析的字符，在读缓冲区的位置
    int m_start_line;           // 当前正在解析的行的起始位置

    CHECK_STATE m_check_state;  // 主状态机当前所处的状态
    METHOD m_method;            // 请求方法

    char m_real_file[FILENAME_LEN];     // 客户端请求目标文件的完整路径，其内容等于 doc_root + m_url, doc_root 是网站的根目录
    char* m_url;                // 请求目标文件的文件名
    char* m_version;            // HTTP 协议版本，只支持 HTTP1.1
    char* m_host;               // 主机名
    long long m_content_length; // HTTP 请求体对应的总长度
    bool m_keep_alive;          // HTTP 请求是否要求保持连接

    char m_write_buf[WRITE_BUFFER_SIZE];    // 写缓冲区
    int m_write_index;          // 写缓冲区中待发送的字节数
    char* m_file_address;       // 客户请求的目标文件被 mmap 到内存中的起始位置
    struct stat m_file_stat;    // 目标文件的状态，通过它我们可以判断文件是否存在、是否为目录、是否可读，并获取文件大小等信息
    struct iovec m_iv[2];       // 我们将采用 writev 来执行写操作，所以定义下面两个成员，其中 m_iv_count 表示被写内存块的数量
    int m_iv_count;

    int bytes_to_send;          // 将要发送的数据的字节数
    int bytes_have_send;        // 已经发送的字节数

public:
    HttpConnection();
    ~HttpConnection();
    void init(int sockfd, const sockaddr_in& client_addr);      // 初始化新接收的客户端连接
    void close_connection();    // 关闭客户端的连接
    bool read();                // 非阻塞读
    bool write();               // 非阻塞写
    void process();             // 响应并且处理客户端的请求

private:
    void init();                                    // 初始化其余的数据
    HTTP_CODE process_read();                       // 解析 HTTP 请求
    bool process_write(HTTP_CODE ret);              // 响应（填充） HTTP 应答

    // 下面这一组函数被 process_read 调用以分析 HTTP 请求
    HTTP_CODE parse_request_line(char* text);       // 解析请求首行
    HTTP_CODE parse_request_headers(char* text);    // 解析请求头
    HTTP_CODE parse_request_content(char* text);    // 解析请求体    
    HTTP_CODE do_request();
    char* get_line() { return this->m_read_buf + this->m_start_line; }  // 获取一行数据
    LINE_STATUS parse_line_data();                       // 获取 HTTP 请求的一行数据   

    // 下面这一组函数被 process_write 调用以填充 HTTP 响应
    void unmap();                                           // 释放内存映射 
    bool add_response(const char* format, ...);             // 添加响应内容（通用函数）
    bool add_content(const char* content);                  // 添加响应体
    bool add_content_type();                                // 添加响应类型
    bool add_status_line(int status_num, const char* status_content);   // 添加响应状态行
    void add_headers(int content_length);                   // 添加响应头
    bool add_content_length(int content_length);            // 添加响应体长度
    bool add_keep_alive();                                  // 添加是否保持连接
    bool add_blank_line();                                  // 添加响应空白行
};

#endif