#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

class http_conn {
public:
    static int m_epollfd;                               // 所有的socket上的事件都被注册到同一个epoll对象中
    static int m_user_count;                            // 统计用户的数量

    http_conn(){};
    ~http_conn(){};
    void process();                                     // 处理客户端请求，并进行响应
    void init(int sockfd, struct sockaddr_in &addr);    // 初始化新接收的连接
    void close_conn();                                  //关闭连接
    bool read();                                        // 非阻塞地读
    bool write();                                       // 非阻塞地写

    
private:
    static const int READ_BUFFER_SIZE = 2048;           // 读缓冲大小
    static const int WRITE_BUFFER_SIZE = 1024;          // 写缓冲大小
    static const int MAX_FILE_PATH_SIZE = 256;          // 最大路径长度

    // 解析客户端请求时，主状态机的状态
    enum CHECK_STATE {
        CHECK_STATE_REQUESTLINE=0,  // 请求行
        CHECK_STATE_HEADER,         // 头部字段
        CHECK_STATE_CONTENT         // 请求体
    };  

    // 从状态机的三种可能状态
    enum LINE_STATUS { 
        LINE_OK=0,  // 读取到完整一行
        LINE_BAD,   // 行出错
        LINE_OPEN   // 行数据尚不完整
    };

    // http请求方法，但代码中只支持GET
    enum METHOD { GET=0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT };
    
    // 服务器处理http请求的可能结果
    enum HTTP_CODE {
        NO_REQUEST,             // 请求不完整，需要继续读取客户端数据
        GET_REQUEST,            // 获得了一个完整的客户请求
        BAD_REQUEST,            // 客户请求语法错误
        NO_RESOURCE,            // 服务器没有资源
        FORBIDDED_REQUEST,      // 客户对资源没有足够的访问权限
        FILE_REQUEST,           // 文件请求，获取文件成功
        INTERNAL_ERROR,         // 表示服务器内部错误
        CLOSE_CONNECTION        // 表示客户端已关闭连接
    };

    int m_sockfd;                           // 该http连接的socket
    sockaddr_in m_address;                  // 通信的socket地址

    CHECK_STATE m_check_state;              // 主状态机所处的状态

    char m_read_buf[READ_BUFFER_SIZE];      // 读缓冲区
    int m_read_index;                       // 标识读缓冲区未读数据起始位置
    int m_start_line;                       // 当前正在解析的行的起始位置
    int m_checked_index;                    // 当前正在分析的字符在读缓冲区的位置

    char m_real_file[MAX_FILE_PATH_SIZE];   // 客户请求目标文件完整路径
    struct stat m_file_stat;                // m_real_file文件的相关状态信息
    char *m_file_address;                   // 客户请求的目标文件被mmap映射到内存中的起始位置

    METHOD m_method;        // 请求方法
    char *m_url;            // 请求目标文件
    char *m_version;        // 请求协议版本，http1.1
    char *m_host;           // 主机名
    bool m_linger;          // http请求是否要保持连接
    int m_content_length;   // http请求的消息总长度
    char *m_content;        // 请求体

    int m_write_idx;                        // 待写数据长度
    char m_write_buf[WRITE_BUFFER_SIZE];    // 写缓冲区
    struct iovec m_iv[2];                   // 采用writev来执行写操作
    int m_iv_count;                         // 被写内存块数量
    

    void init();                                // 初始化连接其余信息
    HTTP_CODE process_read();                   // 解析http请求
    HTTP_CODE parse_request_line(char *text);   // 解析请求首行
    HTTP_CODE parse_header(char *text);         // 解析请求头
    HTTP_CODE parse_content(char *text);        // 解析请求体 TODO
    LINE_STATUS parse_line();                   // 解析一行数据
    HTTP_CODE do_request();                     // 做具体处理
    void unmap();                               // 释放内存映射
    char *get_line() { return m_read_buf + m_start_line;} // 获取一行数据

    bool process_write(HTTP_CODE read_ret);     // 生成响应
    bool add_response(const char *format, ...);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_len);
    bool add_content_length(int content_len);
    bool add_content_type();
    bool add_linger();
    bool add_blank_line();
    bool add_content(const char *content);
};

#endif // HTTPCONNECTION_H