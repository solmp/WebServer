#include <sys/epoll.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/uio.h>

#include "http_conn.h"
#include "locker.h"

const char *doc_root = "/root/codes/webserver/root";    // 网站根目录

//定义HTTP响应的些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "error_400_form: BAD_REQUEST\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "error_403_form: FORBIDDED_REQUEST\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server. \n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "error_500_form: INTERNAL_ERROR\n";


// 设置文件描述符非阻塞
int setnonblocking(int fd) {
    int old_flag = fcntl(fd, F_GETFL);
    int new_flag = old_flag | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_flag);
}

// 向epoll中增加需要监听的文件标识符
void addfd(int epollfd, int fd, bool one_shot) {
    epoll_event event;
    event.data.fd = fd;
    // 触发模式：水平触发，边缘触发
    // EPOLLRDHUP：异常断开时不需要提交上层
    // event.events = EPOLLIN | EPOLLRDHUP;
    event.events = EPOLLIN | EPOLLRDHUP;  // 边缘触发，防止一直读数据，主线程一般不用
    // EPOLLONESHOT事件：5.6第六节 37.30
    if(one_shot) {
        event.events | EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    // 设置文件描述符非阻塞 
    setnonblocking(fd);
}

// 从epoll中移除监听的文件描述符
void removefd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 修改文件描述符，重置socket上的EPOLLONESHOT事件，以确保下一次可读时EPOLLIN事件能被触发
void modifyfd(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}


int http_conn::m_epollfd = -1;
int http_conn::m_user_count = 0;

// 初始化连接
void http_conn::init(int sockfd, struct sockaddr_in &addr){
    m_sockfd = sockfd;
    m_address = addr;

    // 端口复用
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 增加到epoll对象中
    addfd(m_epollfd, m_sockfd, true);
    ++m_user_count;   

    // 初始化连接其余信息
    init();
} 

// 初始化连接其余信息
void http_conn::init() {
    m_check_state = CHECK_STATE_REQUESTLINE; // 初始化状态为解析请求首行

    bzero(m_read_buf, READ_BUFFER_SIZE);
    m_read_index = 0;
    m_start_line = 0;
    m_checked_index = 0;

    bzero(m_real_file, MAX_FILE_PATH_SIZE);
    bzero(&m_file_stat, sizeof(m_file_stat));
    m_file_address = 0;

    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_host = 0;
    m_linger = false;
    m_content_length = 0;
    m_content = 0;

    bzero(m_write_buf, WRITE_BUFFER_SIZE);
    bzero(m_iv, 2);
    m_write_idx = 0;
    m_iv_count = 0;
}

//关闭连接
void http_conn::close_conn() {
    if(m_sockfd != -1) {
        removefd(m_sockfd, m_sockfd);
        m_sockfd = -1;
        --m_user_count;
    }
}

// 循环读取客户数据，直到无数据可读或对方关闭连接
bool http_conn::read() {
    if(m_read_index >= READ_BUFFER_SIZE) {
        return false;
    }

    // 读取到的字节
    int bytes_read = 0;
    while(true) {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_index, READ_BUFFER_SIZE - m_read_index, 0);
        if(bytes_read == -1) {
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                // 数据已读完
                break;
            }
            return false;
        }else if(bytes_read == 0) {
            return false;
        }
        m_read_index += bytes_read;
    }
    printf("读取到了数据：\n%s\n", m_read_buf);
    return true;
}

// 写http响应
bool http_conn::write() {
    int tmp = 0;                        
    int bytes_have_end = 0;             // 已经发送的字节数
    int bytes_to_send = m_write_idx;    // 将要发送的字节数

    if(bytes_to_send == 0) {
        // 将要发送的字节数为0，本次响应结束
        modifyfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }
    while(true) {
        // 分散写
        tmp = writev(m_sockfd, m_iv, m_iv_count);
        if(tmp <= -1) {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件
            if(errno == EAGAIN) {
                modifyfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }
        bytes_to_send -= tmp;
        bytes_have_end += tmp;
        if(bytes_to_send <= bytes_have_end) {
            // 发送http响应成功，根据connection字段决定是否立即断开连接
            unmap();
            if(m_linger) {
                init();
                modifyfd(m_epollfd, m_sockfd, EPOLLIN);
                return true;
            }else {
                modifyfd(m_epollfd, m_sockfd, EPOLLIN);
                return false;
            }
        }
    }
    return true;
}

//往写缓存中写入待发送的数据
bool http_conn::add_response(const char *format, ...) {
    if(m_write_idx >= WRITE_BUFFER_SIZE) {
        return false;
    }

    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf+m_write_idx, WRITE_BUFFER_SIZE-1-m_write_idx, format, arg_list);
    if(len >= WRITE_BUFFER_SIZE-1-m_write_idx) {
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);
    return true;
}
bool http_conn::add_status_line(int status, const char *title) {
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
bool http_conn::add_headers(int content_len) {
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
}
bool http_conn::add_content_length(int content_len) {
    return add_response("Content-Length: %d\r\n", content_len);
}
bool http_conn::add_content_type() {
    return add_response("Content-Type: %s\r\n", "text/html");
}
bool http_conn::add_linger() {
    return add_response("Connection: %s\r\n", m_linger ? "keep-alive" : "close");
}
bool http_conn::add_blank_line() {
    return add_response("%s", "\r\n");
}
bool http_conn::add_content(const char *content) {
    return add_response("%s\r\n", content);
}

// 由线程池中的工作线程调用，是处理http请求的入口函数
void http_conn::process() {
    // 解析http请求
    HTTP_CODE read_ret =  process_read();
    if(read_ret == NO_REQUEST) {
        modifyfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }

    // 生成响应
    bool write_ret = process_write(read_ret);
    if(!write_ret) {
        close_conn();
    }
    modifyfd(m_epollfd, m_sockfd, EPOLLOUT);
}

// 解析http请求
http_conn::HTTP_CODE http_conn::process_read() {

    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    while((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) ||
          (line_status = parse_line()) == LINE_OK) {
        // 解析请求体或解析到了完整一行数据
        text = get_line();
        m_start_line = m_checked_index;
        // printf("get 1 http line: %s\n", text);
        switch(m_check_state) {
            case CHECK_STATE_REQUESTLINE: {
                ret = parse_request_line(text);
                if(ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER:
                ret = parse_header(text);
                if(ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                }else if(ret == GET_REQUEST) {
                    return do_request();
                }
                break;
            case CHECK_STATE_CONTENT:
                printf("\nCHECK_STATE_CONTENT\n");
                ret = parse_content(text);
                if(ret == GET_REQUEST) {
                    return do_request();
                }
                line_status = LINE_OPEN;
                break;
            default:
                return NO_REQUEST;
        }
    }
    return NO_REQUEST;
}

// 解析请求首行: 请求方法、目标url，http版本
http_conn::HTTP_CODE http_conn::parse_request_line(char *text) {
    // GET /index.html HTTP/1.1
    m_url = strpbrk(text, " \t");
    *m_url++ = '\0';
    
    char *method = text;
    if(strcasecmp(method, "GET") == 0) {
        m_method = GET;
    }else {
        return BAD_REQUEST;
    }

    m_version = strpbrk(m_url, " \t");
    if(!m_version) {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    
    if(strcasecmp(m_version, "HTTP/1.1") != 0) {
        return BAD_REQUEST;
    }

    if(strncasecmp(m_url, "http://", 7) == 0) {
        m_url += 7;
        m_url = strchr(m_url, '/'); // /index.html
    }

    if(!m_url || m_url[0] != '/') {
        return BAD_REQUEST;
    }

    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

// 解析请求头
http_conn::HTTP_CODE http_conn::parse_header(char *text) {
    // Host: 8.142.217.82:9006
    // Connection: keep-alive
    // Content-Length: 1076
    if(text[0] == '\0') {
        if(m_content_length != 0) {
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }else if(strncasecmp(text, "Host:", 5) == 0) {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }else if(strncasecmp(text, "Connection:", 11) == 0) {
        text += 11;
        text += strspn(text, " \t");
        if(strcasecmp(text, "keep-alive") == 0) {
            m_linger = true;
        }
    }else if(strncasecmp(text, "Content-Length:", 15) == 0) {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }else {
        printf("unknow header: %s\n", text);
    }
    
    return NO_REQUEST;
}

// 解析请求体
http_conn::HTTP_CODE http_conn::parse_content(char *text) {
    // if(m_read_index >= (m_content_length + m_checked_index)) {
    //     m_check_state = CHECK_STATE_REQUESTLINE;
    //     text[m_content_length] = '\0';
    //     return GET_REQUEST;
    // }

    int len = strlen(m_content);
    int text_len = strlen(text);
    if(len + text_len >= m_content_length) {
        m_check_state = CHECK_STATE_REQUESTLINE;
        strncat(m_content, text, m_content_length-len);
        printf("\ncontent: \n%s\n", m_content);
        return GET_REQUEST;
    }
    strcat(m_content, text);
    return NO_REQUEST;
}

// 解析一行数据，判断依据 \r\n
http_conn::LINE_STATUS http_conn::parse_line() {
    char tmp;
    for(; m_checked_index < m_read_index; ++m_checked_index) {
        tmp = m_read_buf[m_checked_index];
        if(tmp == '\r') {
            if(m_checked_index + 1 == m_read_index) {
                return LINE_OPEN;
            } else if(m_read_buf[m_checked_index + 1] == '\n') {
                m_read_buf[m_checked_index++] = '\0';
                m_read_buf[m_checked_index++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }else if(tmp == '\n') {
            if(m_checked_index > 1 && m_read_buf[m_checked_index-1] == '\r') {
                // 未读完继续读时遇到\n
                m_read_buf[m_checked_index-1] = '\0';
                m_read_buf[m_checked_index++] = '\0';
                return LINE_OK;
            }   
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

// 分析目标文件属性，文件存在、有权限、非目录时，将其用mmap映射到内存地址m_file_address处
http_conn::HTTP_CODE http_conn::do_request() {
    // /index.html
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    strncpy(m_real_file+len, m_url, MAX_FILE_PATH_SIZE-len-1);
    printf("m_real_file=%s\n", m_real_file);
    // 获取m_real_file文件的相关状态信息，-1失败、0成功
    if(stat(m_real_file, &m_file_stat) < 0) {
        return NO_RESOURCE;
    }

    // 判断访问权限
    if(!(m_file_stat.st_mode & S_IROTH)) {
        return FORBIDDED_REQUEST;
    }

    // 判断是否目录
    if(S_ISDIR(m_file_stat.st_mode)) {
        return BAD_REQUEST;
    }

    // 只读方式打开文件
    int fd = open(m_real_file, O_RDONLY);
    // 创建内存映射
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

// 对内存映射执行munmap操作
void http_conn::unmap() {
    if(m_file_address) {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

// 生成响应
bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret) {
        case INTERNAL_ERROR:
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if(!add_content(error_500_form)) {
                return false;
            }
            break;
        case BAD_REQUEST:
            add_status_line(400, error_400_title);
            add_headers(strlen(error_400_form));
            if(!add_content(error_400_form)) {
                return false;
            }
            break;
        case NO_RESOURCE:
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if(!add_content(error_404_form)) {
                return false;
            }
            break;
        case FORBIDDED_REQUEST:
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if(!add_content(error_403_form)) {
                return false;
            }
            break;
        case FILE_REQUEST:
            add_status_line(200, ok_200_title);
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            return true;
        default:
            return false;

    }
}