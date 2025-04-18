#include"../include/http_connection.h"

// 定义 HTTP 响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

// 初始化网站的根目录
const char* doc_root = "/home/utopianyouth/webserver/resources";

// 静态成员变量需要初始化
int HttpConnection::m_epoll_fd = -1;        // 主线程会创建 epoll 对象并且对其赋值
int HttpConnection::m_user_count = 0;

// 设置文件描述符非阻塞
int set_non_blocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// 添加需要监听的文件描述符到 epoll 对象中
void add_fd_epoll(int epoll_fd, int fd, bool one_shot) {
    // 注册 epoll 对象监听的 IO 事件
    epoll_event event;
    event.data.fd = fd;

    event.events = EPOLLIN | EPOLLRDHUP | EPOLLET;    // EPOLLRDHUP 事件属性可以检测文件描述符对应的客户端断开连接，交给内核处理

    if (one_shot) {
        // EPOLLNONESHOT 事件属性限制一个线程操作一个 socket
        event.events |= EPOLLONESHOT;
    }

    // 添加
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event);

    // 设置文件描述符非阻塞
    set_non_blocking(fd);
}

// 从 epoll 对象中删除文件描述符
void remove_fd_epoll(int epoll_fd, int fd) {
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    //printf("close fd = %d.\n", fd);
    close(fd);
}

// 修改 epoll 对象中的文件描述符，重置 socket 上的 EPOLLONESHOT 事件，以确保下一次可读时，EPOLLIN 事件能被触发
void modify_fd_epoll(int epoll_fd, int fd, int event_num) {
    epoll_event event;
    event.data.fd = fd;
    event.events = event_num | EPOLLONESHOT | EPOLLRDHUP | EPOLLET;

    // 修改
    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event);
}

// 关闭客户端连接
void HttpConnection::close_connection() {
    if (this->m_sockfd != -1) {
        remove_fd_epoll(this->m_epoll_fd, this->m_sockfd);
        this->m_sockfd = -1;
        --this->m_user_count;       // 连接的客户端总数量减一
    }
}

// 初始化新接收的客户端连接，主线程中调用初始化 socket 地址
void HttpConnection::init(int sockfd, const sockaddr_in& client_addr) {
    this->m_sockfd = sockfd;
    this->m_client_addr = client_addr;

    // 设置端口复用
    int reuse = 1;
    setsockopt(this->m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 添加到 epoll 对象中，指定 EPOLLONESHOT，一个线程处理一个 socket 通信
    add_fd_epoll(this->m_epoll_fd, this->m_sockfd, true);
    ++this->m_user_count;       // 连接的客户端数量 + 1

    // 初始化其余信息
    this->init();
}

// 初始化其余的信息
void HttpConnection::init() {
    this->bytes_to_send = 0;
    this->bytes_have_send = 0;

    this->m_check_state = CHECK_STATE_REQUESTLINE;      // 初始化状态为解析请求首行
    this->m_keep_alive = false;         // 默认不保持连接  Connection: keep-alive 保持连接

    this->m_method = GET;               // 默认 HTTP 请求方式为 GET
    this->m_url = 0;                    // 请求目标文件的文件名
    this->m_version = 0;
    this->m_content_length = 0;
    this->m_host = 0;
    this->m_start_line = 0;
    this->m_checked_index = 0;
    this->m_read_index = 0;
    this->m_write_index = 0;

    bzero(this->m_read_buf, READ_BUFFER_SIZE);
    bzero(this->m_write_buf, WRITE_BUFFER_SIZE);
    bzero(this->m_real_file, FILENAME_LEN);         // 目标文件的完整路径
}

// 循环读取客户端数据，直到无数据可读或者对方关闭连接
bool HttpConnection::read() {
    // m_read_index 记录 m_read_buf 数组的遍历情况
    if (this->m_read_index >= READ_BUFFER_SIZE) {
        // 没有数据可读
        return false;
    }

    int bytes_read = 0;     // 记录读取到的字节数

    while (true) {
        bytes_read = recv(this->m_sockfd, this->m_read_buf + this->m_read_index, this->READ_BUFFER_SIZE - this->m_read_index, 0);
        if (bytes_read == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 返回 EAGAIN 或 EWOULDBLOCK 表示没有数据可读
                break;
            }
            else {
                // 其它错误，直接返回
                return false;
            }
        }
        else if (bytes_read == 0) {
            // 对方关闭连接
            return false;
        }
        this->m_read_index += bytes_read;
    }
    // 输出每一次读取到的数据
    //printf("read data:\n%s", this->m_read_buf);
    return true;
}

// 获取 HTTP 请求的一行数据（解析一行，判断依据 \r\n）
HttpConnection::LINE_STATUS HttpConnection::parse_line_data() {
    char temp;
    // 遍历读取到的字节流数据
    for (; this->m_checked_index < this->m_read_index; ++this->m_checked_index) {

        temp = this->m_read_buf[this->m_checked_index];   // 当前检查的字符

        if (temp == '\r') {
            if ((this->m_checked_index + 1) == this->m_read_index) {
                // 指针指向地址比较，行数据最后一个字符是 '\r'，行数据不完整
                return LINE_OPEN;
            }
            else if (this->m_read_buf[this->m_checked_index + 1] == '\n') {
                // 一行完整数据，将 '\r' 和 '\n' 换成 '\0'
                this->m_read_buf[this->m_checked_index++] = '\0';
                this->m_read_buf[this->m_checked_index++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n') {
            if ((this->m_checked_index > 1) && (this->m_read_buf[this->m_checked_index - 1] == '\r')) {
                // 一行完整数据，将 '\r' 和 '\n' 换成 '\0'
                this->m_read_buf[this->m_checked_index - 1] = '\0';
                this->m_read_buf[this->m_checked_index++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }

    // 如果检测到的读缓冲区最后一个字符不是 '\n' , 说明这一行数据不完整
    return LINE_OPEN;
}

// 解析 HTTP 请求行，获得请求方法，目标 URL，HTTP 版本
HttpConnection::HTTP_CODE HttpConnection::parse_request_line(char* text) {
    // GET /index.html HTTP/1.1
    this->m_url = strpbrk(text, " \t");         // 判断第二个参数中的字符哪个在 text 中最先出现
    if (this->m_url == NULL) {
        return BAD_REQUEST;
    }

    // GET\0/index.html HTTP/1.1
    *this->m_url++ = '\0';

    char* method = text;
    if (strcasecmp(method, "GET") == 0) {       // 不区分大小，比较两个字符串，只支持 GET 请求
        this->m_method = GET;
    }
    else {
        return BAD_REQUEST;
    }

    // /index.html HTTP/1.1
    this->m_version = strpbrk(this->m_url, " \t");
    if (this->m_version == NULL) {
        return BAD_REQUEST;
    }

    // /index.html\0HTTP/1.1
    *this->m_version++ = '\0';
    if (strcasecmp(this->m_version, "HTTP/1.1") != 0) {
        // 只支持 HTTP1.1 
        return BAD_REQUEST;
    }

    // http://192.168.1.1:10000/index.html
    if (strncasecmp(this->m_url, "http://", 7) == 0) {      // 不区分大小写，比较前 n 个字符
        this->m_url += 7;   // 192.168.1.1:10000/index.html
        this->m_url = strchr(this->m_url, '/');     // /index.html (查找指定字符第一次出现的位置)
    }

    if ((this->m_url == NULL) || (this->m_url[0] != '/')) {
        return BAD_REQUEST;
    }

    this->m_check_state = CHECK_STATE_HEADER;       // 主状态机的检查状态变成检查请求头

    return NO_REQUEST;      // 继续解析 HTTP 请求内容
}

// 解析 HTTP 请求头信息
HttpConnection::HTTP_CODE HttpConnection::parse_request_headers(char* text) {
    // 遇到空行，表示头部字段解析完毕
    if (text[0] == '\0') {
        if (this->m_content_length != 0) {
            // 如果 HTTP 请求有请求体，则还需要读取 m_content_length 字节的请求体
            // 状态机转移到 CHECK_STATE_CONTENT 状态            
            this->m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        else {
            // 否则说明我们已经得到了一个完整的 HTTP 请求
            return GET_REQUEST;
        }
    }
    else if (strncasecmp(text, "Connection:", 11) == 0) {
        // 处理 Connection 头部字段，Connection: keep-alive
        text += 11;
        text += strspn(text, " \t");    // 计算前缀长度
        if (strcasecmp(text, "keep-alive") == 0) {
            this->m_keep_alive = true;
        }
    }
    else if (strncasecmp(text, "Proxy-Connection:", 17) == 0) {
        // 处理 Proxy-Connection 头部字段，考虑代理服务器的行为（但是）
        text += 17;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0) {
            this->m_keep_alive = true;
        }
    }
    else if (strncasecmp(text, "Content-Length:", 15) == 0) {
        // 处理 Content-Length 头部字段
        text += 15;
        text += strspn(text, " \t");
        this->m_content_length = atol(text);    // 将字符串转换为长整型
    }
    else if (strncasecmp(text, "Host:", 5) == 0) {
        // 处理 Host 头部字段
        text += 5;
        text += strspn(text, " \t");
        this->m_host = text;
    }
    else {
        // 请求体中的其它类型
        //printf("unknow header %s\n", text);
    }
    return NO_REQUEST;  // 继续解析 HTTP 请求内容
}

// 这里并没有真正解析 HTTP 请求体信息，只是判断它是否被完整的读入了
HttpConnection::HTTP_CODE HttpConnection::parse_request_content(char* text) {
    if (this->m_read_index >= (this->m_content_length + this->m_checked_index)) {
        text[this->m_content_length] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;      // 没有被完全读入
}

// 主状态机，解析 HTTP 请求
HttpConnection::HTTP_CODE HttpConnection::process_read() {

    // 从状态机初始化为读取到完整的一行
    LINE_STATUS line_status = LINE_OK;

    // HTTP请求初始化为不完整
    HTTP_CODE ret = NO_REQUEST;

    char* text = 0;
    while (((this->m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK)) ||
        ((line_status = parse_line_data()) == LINE_OK)) {
        // 解析到了一行完整的数据，或者解析到了请求体，也是完整的数据

        // 获取一行数据
        text = this->get_line();
        this->m_start_line = this->m_checked_index;
        //printf("got 1 http line: %s\n", text);

        switch (this->m_check_state) {
        case CHECK_STATE_REQUESTLINE:
            ret = this->parse_request_line(text);
            if (ret == BAD_REQUEST) {
                return BAD_REQUEST;
            }
            break;
        case CHECK_STATE_HEADER:
            ret = this->parse_request_headers(text);
            if (ret == BAD_REQUEST) {
                return BAD_REQUEST;
            }
            else if (ret == GET_REQUEST) {
                return this->do_request();      // 表示获取一个完整的客户端请求，向客户端响应请求的内容
            }
            break;
        case CHECK_STATE_CONTENT:
            ret = parse_request_content(text);
            if (ret == GET_REQUEST) {
                return this->do_request();
            }
            else {
                line_status = LINE_OPEN;        // 请求体数据没有被完全读入
            }
            break;
        default:
            return INTERNAL_ERROR;              // 主状态机其它状态，内部错误
        }
    }
    return NO_REQUEST;
}

/*
    当得到一个完整、正确的 HTTP 请求时，我们就分析目标文件的属性，
    如果目标文件存在、对所有用户可读，且不是目录，则使用 mmap 将
    其映射到内存地址 m_file_address 处，并告诉调用者获取文件成功
*/
HttpConnection::HTTP_CODE HttpConnection::do_request() {
    // "/home/utopiayouth/linux_study/webserver/resources"
    strcpy(this->m_real_file, doc_root);
    int len = strlen(doc_root);

    // 请求资源的路径拼接, FILENAME_LEN - len - 1 多一个减一是因为字符串结束符 '\0'
    strncpy(this->m_real_file + len, this->m_url, FILENAME_LEN - len - 1);

    // 获取 m_real_file 文件相关的状态信息，存储到 struct stat 结构体中，-1 表示失败，0 表示成功
    if (stat(this->m_real_file, &this->m_file_stat) < 0) {
        return NO_RESOURCE;     // 没有找到请求的文件
    }

    // 判断访问权限
    if (!(m_file_stat.st_mode & S_IROTH)) {
        return FORBIDDEN_REQUEST;
    }

    // 判断是否是目录
    if (S_ISDIR(this->m_file_stat.st_mode)) {
        return BAD_REQUEST;
    }

    // 以只读方式打开文件
    int fd = open(this->m_real_file, O_RDONLY);
    if (fd == -1) {
        return BAD_REQUEST;
    }

    // 对待响应的文件创建内存映射
    this->m_file_address = (char*)mmap(NULL, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (!this->m_file_address) {
        return BAD_REQUEST;
    }

    close(fd);

    return FILE_REQUEST;    // 文件请求，获取文件成功
}

// 对内存映射区执行 munmap 操作，释放内存映射
void HttpConnection::unmap() {
    if (this->m_file_address) {
        munmap(this->m_file_address, this->m_file_stat.st_size);
        m_file_address = NULL;
    }
}

// 写 HTTP 响应
bool HttpConnection::write() {
    int tmp = 0;
    if (this->bytes_to_send == 0) {
        // 将要发送的字节为 0，这一次响应结束
        modify_fd_epoll(this->m_epoll_fd, this->m_sockfd, EPOLLIN);
        this->init();
        return true;
    }

    while (1) {
        // 分散写，m_iv[2] 表示有两块内存区被分散写（同时操作两块内存区）
        // 本项目操作的第一块内存区（即 this->m_write_buf, 存储了响应状态行, 响应头）
        // 本项目操作的第二块内存区（即解析 HTTP 请求成功后创建的内存映射区, 是存储在 web 服务器上，发送给客户端的资源文件）
        tmp = writev(this->m_sockfd, this->m_iv, this->m_iv_count);
        if (tmp <= -1) {
            /*
                如果 TCP 写缓冲区没有空间，则等待下一轮 EPOLLOUT 事件，重新调用 modify_fd_epoll() 是有必要的，
                以便主线程在 epoll_wait() 时，可以检测到 web 程序触发了 EPOLLOUT 事件，需要向 TCP 写缓冲区中写数据,
                在此期间，服务器无法立即接收到同一客户端的下一个请求（没有注册 EPOLLIN 事件），但可以保证连接的完整性。
            */
            if (errno == EAGAIN) {
                modify_fd_epoll(this->m_epoll_fd, this->m_sockfd, EPOLLOUT);
                return true;
            }
            this->unmap();
            return false;
        }

        this->bytes_have_send += tmp;
        this->bytes_to_send -= tmp;

        if (this->bytes_have_send >= this->m_iv[0].iov_len) {
            // 响应状态行和响应头发送完毕，发送响应体
            this->m_iv[0].iov_len = 0;
            this->m_iv[1].iov_base = this->m_file_address + (this->bytes_have_send - this->m_write_index);
            this->m_iv[1].iov_len = this->bytes_to_send;
        }
        else {
            // 继续发送响应状态行和响应头
            this->m_iv[0].iov_base = this->m_write_buf + this->bytes_have_send;
            this->m_iv[0].iov_len = this->m_iv[0].iov_len - tmp;
        }

        if (this->bytes_to_send <= 0) {
            // 没有数据要发送了
            this->unmap();
            modify_fd_epoll(this->m_epoll_fd, this->m_sockfd, EPOLLIN);

            if (this->m_keep_alive) {
                this->init();
                return true;
            }
            else {
                // 只响应一次
                return false;
            }
        }
    }
}

// 往写缓冲区中写入待发送的数据，format 参数表示格式化参数列表，和 printf 的第一个参数类似
bool HttpConnection::add_response(const char* format, ...) {
    if (this->m_write_index >= WRITE_BUFFER_SIZE) {
        return false;       // 写缓冲区满
    }

    va_list arg_list;           // 存储可变参数列表的信息 
    va_start(arg_list, format); // format 确定可变参数列表的起始位置

    // 将可变的参数列表内容写入到缓冲区中，如 add_response("%s %s", "xi", "xi");
    int len = vsnprintf(this->m_write_buf + this->m_write_index, WRITE_BUFFER_SIZE - 1 - this->m_write_index, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - this->m_write_index)) {
        return false;
    }
    this->m_write_index += len;

    va_end(arg_list);           // 清理参数列表变量
    return true;
}

// 响应状态行
bool HttpConnection::add_status_line(int status_num, const char* status_content) {
    return this->add_response("%s %d %s\r\n", "HTTP/1.1", status_num, status_content);
}

// 响应头
void HttpConnection::add_headers(int content_len) {
    this->add_content_length(content_len);      // 如果请求资源成功，content_length 表示资源的大小（响应体大小）
    this->add_content_type();
    this->add_keep_alive();
    this->add_blank_line();
}

// 响应头：响应体长度
bool HttpConnection::add_content_length(int content_len) {
    return this->add_response("Content-Length: %d\r\n", content_len);
}

// 响应头：是否保持连接
bool HttpConnection::add_keep_alive() {
    return this->add_response("Connection: %s\r\n", (this->m_keep_alive == true) ? "keep-alive" : "close");
}

// 响应头：空白行
bool HttpConnection::add_blank_line() {
    return this->add_response("%s", "\r\n");
}

// 响应体
bool HttpConnection::add_content(const char* content) {
    return this->add_response("%s", content);
}

// 响应体类型
bool HttpConnection::add_content_type() {
    return this->add_response("Content-Type: %s\r\n", "text/html");
}

// 根据服务器处理 HTTP 请求的结果，决定返回给客户端的内容
bool HttpConnection::process_write(HTTP_CODE ret) {
    switch (ret) {
    case INTERNAL_ERROR:
        this->add_status_line(500, error_500_title);
        this->add_headers(strlen(error_500_form));
        if (this->add_content(error_500_form) == false) {
            return false;
        }
        break;
    case BAD_REQUEST:
        this->add_status_line(400, error_400_title);
        this->add_headers(strlen(error_400_form));
        if (this->add_content(error_400_form) == false) {
            return false;
        }
        break;
    case NO_RESOURCE:
        this->add_status_line(404, error_404_title);
        this->add_headers(strlen(error_404_form));
        if (this->add_content(error_404_form) == false) {
            return false;
        }
        break;
    case FORBIDDEN_REQUEST:
        this->add_status_line(403, error_403_title);
        this->add_headers(strlen(error_403_form));
        if (this->add_content(error_403_form) == false) {
            return false;
        }
        break;
    case FILE_REQUEST:
        // 请求服务器资源文件成功
        // 也需要返回对应的响应状态行，响应头（基于HTTP协议），这样返回的服务器资源才能正确地被运行 HTTP 协议的浏览器解析
        this->add_status_line(200, ok_200_title);
        this->add_headers(this->m_file_stat.st_size);
        // 分散写对象初始化，涉及到两块内存区
        this->m_iv[0].iov_base = this->m_write_buf;
        this->m_iv[0].iov_len = this->m_write_index;
        this->m_iv[1].iov_base = this->m_file_address;
        this->m_iv[1].iov_len = this->m_file_stat.st_size;
        this->m_iv_count = 2;

        this->bytes_to_send = this->m_write_index + this->m_file_stat.st_size;
        return true;
    default:
        return false;
    }

    // 状态码为 200 以外的，需要返回给客户端的内容
    this->m_iv[0].iov_base = this->m_write_buf;
    this->m_iv[0].iov_len = this->m_write_index;
    this->m_iv_count = 1;
    this->bytes_to_send = this->m_write_index;
    return true;
}

// 由线程池中的工作线程调用，这是处理 HTTP 请求的入口函数
void HttpConnection::process() {
    // 解析 HTTP 请求
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST) {
        // NO_REQUEST: 需要继续读取客户端请求的内容
        modify_fd_epoll(this->m_epoll_fd, this->m_sockfd, EPOLLIN);
        return;
    }

    // 生成响应
    bool write_ret = process_write(read_ret);
    if (!write_ret) {
        close_connection();
    }

    // 监测文件描述符写事件 
    modify_fd_epoll(this->m_epoll_fd, this->m_sockfd, EPOLLOUT);
}


HttpConnection::HttpConnection() {

}

HttpConnection::~HttpConnection() {

}