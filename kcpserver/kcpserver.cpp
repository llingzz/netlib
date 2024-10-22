// kcpserver.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//

#include <iostream>
#include <map>
#include <queue>
#include <asio.hpp>
#include "ikcp.h"
using asio::ip::udp;

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

class server;
class session {
public:
    session(uint32_t conv, std::shared_ptr<server> server) :
        conv_(conv), server_(server) {
        p_kcp_ = ikcp_create(conv, (void*)this);
        p_kcp_->output = &session::output;
        ikcp_nodelay(p_kcp_, 1, 20, 2, 1);
    }

    ~session() {

    }

    void input(char* udp_data, size_t bytes_recvd)
    {
        ikcp_input(p_kcp_, udp_data, bytes_recvd);
        char kcp_buf[2048] = "";
        int kcp_recvd_bytes = ikcp_recv(p_kcp_, kcp_buf, sizeof(kcp_buf));
        if (kcp_recvd_bytes > 0) {
            std::cout << "recv:" << kcp_buf << std::endl;
        }
    }

    static int output(const char* buf, int len, ikcpcb* kcp, void* user);

    void send_udp_package(const char* buf, int len);
    void send_kcp_message(const std::string& str)
    {
        int send_ret = ikcp_send(p_kcp_, str.c_str(), str.size());
        if (send_ret < 0) {
            std::cout << "send_ret<0: " << send_ret << std::endl;
        }
    }

    ikcpcb* p_kcp_;
    uint32_t conv_;
    std::shared_ptr<server> server_;
};

class server : public std::enable_shared_from_this<server> {
public:
    server(asio::io_context& io_context, short port)
        : socket_(io_context, udp::endpoint(udp::v4(), port)), conv(0)
    {
        do_receive();
        thread_ = std::thread([this] {
            while (true) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                auto clock = iclock();
                for (auto& iter : sessions_) {
                    if (!iter.second) { continue; }
                    ikcp_update(iter.second->p_kcp_, clock);
                }
            }
        });
    }

    void do_receive()
    {
        socket_.async_receive_from(asio::buffer(data_, max_length), sender_endpoint_,
            [this](asio::error_code ec, std::size_t bytes_recvd) {
                if (!ec && bytes_recvd > 0) {
                    std::string str(data_, bytes_recvd);
                    if ("UdpConnectPack conv" == str) {
                        std::string back = std::string("UdpConnectPack conv got:") + std::to_string(++conv);
                        socket_.send_to(asio::buffer(back.c_str(), back.size()), sender_endpoint_);
                        sessions_[conv] = std::make_shared<session>(conv, shared_from_this());
                        std::cout << "IP: " << sender_endpoint_.address() << ", Port: " << sender_endpoint_.port() << std::endl;
                    }
                    else {
                        int ret = ikcp_getconv(data_);
                        if (ret != 0) {
                            if (sessions_.find(ret) != sessions_.end() && sessions_[ret]) {
                                sessions_[ret]->input(data_, bytes_recvd);
                            }
                        }
                    }
                }
                else {
                    printf("error: %s, bytes_recvd: %ld\n", ec.message().c_str(), bytes_recvd);
                }
                memset(data_, 0, max_length);
                do_receive();
            }
        );
    }

    void do_send(const std::string& msg)
    {
        //socket_.send_to(asio::buffer(msg), sender_endpoint_);
        socket_.async_send_to(asio::buffer(msg.c_str(), msg.size()), sender_endpoint_,
            [this](asio::error_code ec, std::size_t bytes_sent) {
                if (ec) {
                    printf("error: %s, bytes_sent: %ld\n", ec.message().c_str(), bytes_sent);
                }
            }
        );
    }

    void send_message(uint32_t conv, const std::string& str)
    {
        if (str.size() <= 0 || conv <= 0) {
            return;
        }
        if (sessions_.find(conv) == sessions_.end() || !sessions_[conv]) {
            return;
        }
        auto session_ = sessions_[conv];
        session_->send_kcp_message(str);
    }

private:
    udp::socket socket_;
    udp::endpoint sender_endpoint_;
    enum { max_length = 2048 };
    char data_[max_length];
    int32_t conv;
    std::map<int32_t, std::shared_ptr<session>> sessions_;
    std::thread thread_;
};

int session::output(const char* buf, int len, ikcpcb* kcp, void* user)
{
    ((session*)user)->send_udp_package(buf, len);
    return 0;
}

void session::send_udp_package(const char* buf, int len)
{
    server_->do_send(std::string(buf, len));
}

int main()
{
    asio::io_context io_context;
    std::shared_ptr<server> s(new server(io_context, 8888));
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
            s->send_message(1, "test");
        }
    } while (ch != 'Q');
    return 0;
}
