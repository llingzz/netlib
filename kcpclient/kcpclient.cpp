// kcpclient.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//

#include <iostream>
#include <queue>
#include <asio.hpp>
#include "ikcp.h"

void itimeofday(long* sec, long* usec)
{
#ifdef _WIN32
    time_t clock;
    struct tm tm;
    SYSTEMTIME wtm;
    GetLocalTime(&wtm);
    tm.tm_year = wtm.wYear - 1900;
    tm.tm_mon = wtm.wMonth - 1;
    tm.tm_mday = wtm.wDay;
    tm.tm_hour = wtm.wHour;
    tm.tm_min = wtm.wMinute;
    tm.tm_sec = wtm.wSecond;
    tm.tm_isdst = -1;
    clock = mktime(&tm);
    if (sec) {
        *sec = (long)clock;
    }
    if (usec) {
        *usec = wtm.wMilliseconds * 1000;
    }
#elif __APPLE__
    struct timeval time;
    gettimeofday(&time, NULL);
    if (sec) {
        *sec = time.tv_sec;
    }
    if (usec) {
        *usec = time.tv_usec;
    }
#endif
}
uint64_t iclock64(void)
{
    long s, u;
    uint64_t value;
    itimeofday(&s, &u);
    value = ((uint64_t)s) * 1000 + (u / 1000);
    return value;
}
static inline uint32_t iclock()
{
    return (uint32_t)(iclock64() & 0xfffffffful);
}

using asio::ip::udp;
class client {
public:
    client(asio::io_context& io_context, const std::string& ip, short serverport, short port)
        : socket_(io_context, udp::endpoint(udp::v4(), port)), connected(false), server_port_(serverport), server_ip_(ip)
    {
        memset(data_, 0, max_length);
        udp::resolver resolver(io_context);
        udp::resolver::results_type endpoints = resolver.resolve(udp::v4(), ip, std::to_string(serverport));
        std::string str("UdpConnectPack conv");
        socket_.send_to(asio::buffer(str.c_str(), str.size()), *endpoints.begin());
        do_receive();
        thread_ = std::thread([this] {
            while (true) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                if (p_kcp_) {
                    ikcp_update(p_kcp_, iclock());
                    do_send();
                }
            }
        });
    }

    static int output(const char* buf, int len, ikcpcb* kcp, void* user);

    void do_receive()
    {
        socket_.async_receive_from(asio::buffer(data_, max_length), sender_endpoint_,
            [this](asio::error_code ec, std::size_t bytes_recvd) {
                if (!ec && bytes_recvd > 0) {
                    std::string str(data_);
                    if (str.size() > 24 && str.substr(0, 24) == "UdpConnectPack conv got:") {
                        int32_t conv = atoi(str.substr(24, str.size() - 1).c_str());
                        p_kcp_ = ikcp_create(conv, (void*)this);
                        p_kcp_->output = &client::output;
                        ikcp_nodelay(p_kcp_, 1, 20, 2, 1);
                    }
                    else {
                        ikcp_input(p_kcp_, data_, bytes_recvd);
                        while (true) {
                            char kcp_buf[max_length] = "";
                            const int kcp_recvd_bytes = ikcp_recv(p_kcp_, kcp_buf, sizeof(kcp_buf));
                            if (kcp_recvd_bytes > 0) {
                                std::cout << "recv:" << kcp_buf << std::endl;
                                continue;
                            }
                            break;
                        }
                    }
                }
                memset(data_, 0, max_length);
                do_receive();
            }
        );
    }

    void do_send()
    {
        std::queue<std::string> msgs;
        {
            std::unique_lock<std::mutex> lk(m_lock);
            std::swap(msgs, send_msg_queue_);
        }
        while (msgs.size() > 0) {
            std::string msg = msgs.front();
            const int send_ret = ikcp_send(p_kcp_, msg.c_str(), msg.size());
            if (send_ret < 0) {
                std::cerr << "send_ret<0: " << send_ret << std::endl;
            }
            msgs.pop();
        }
    }

    void send_udp_package(const char* buf, int len)
    {
        socket_.send_to(asio::buffer(buf, len), sender_endpoint_);
    }

    void send_message(const std::string& str)
    {
        if (str.size() > max_length) {
            std::cerr << "send_msg error: send size too large. " << str.size() << std::endl;
            return;
        }
        std::unique_lock<std::mutex> lk(m_lock);
        send_msg_queue_.push(str);
    }

private:
    udp::socket socket_;
    udp::endpoint sender_endpoint_;
    enum { max_length = 1024 };
    char data_[max_length];
    std::thread thread_;
    bool connected;
    struct sockaddr_in servaddr_;
    int server_port_;
    std::string server_ip_;
    ikcpcb* p_kcp_;
    std::mutex m_lock;
    std::queue<std::string> send_msg_queue_;
};

int client::output(const char* buf, int len, ikcpcb* kcp, void* user)
{
    if (!user) {
        return 0;
    }
    (static_cast<client*>(user))->send_udp_package(buf, len);
    return 0;
}

int main()
{
    asio::io_context io_context;
    client c(io_context, "127.0.0.1", 8888, 8889);
    std::thread sub = std::thread([&] {
            io_context.run();
        }
    );
    int ch = 0;
    do {
        ch = getchar();
        ch = toupper(ch);
        if ('A' == ch)
        {
            c.send_message("test");
        }
    } while (ch != 'Q');
    return 0;
}
