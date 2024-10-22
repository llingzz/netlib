// udpclient.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//

#include <iostream>
#include <queue>
#include "asio.hpp"

using asio::ip::udp;
class client {
public:
    client(asio::io_context& io_context, const std::string& ip, short serverport, short port)
        : socket_(io_context, udp::endpoint(udp::v4(), port)), io_context_(io_context), server_port_(serverport), server_ip_(ip)
    {
        memset(data_, 0, max_length);
        do_receive();
    }

    void do_receive()
    {
        socket_.async_receive_from(asio::buffer(data_, max_length), sender_endpoint_,
            [this](asio::error_code ec, std::size_t bytes_recvd) {
                if (!ec && bytes_recvd > 0) {
                    std::cout << "recv:" << data_ << std::endl;
                }
                memset(data_, 0, max_length);
                do_receive();
            }
        );
    }

    void do_send()
    {
        udp::resolver resolver(io_context_);
        udp::resolver::results_type endpoints = resolver.resolve(udp::v4(), server_ip_, std::to_string(server_port_));
        socket_.async_send_to(asio::buffer(send_msg_queue_.front().data(), send_msg_queue_.front().length()), *endpoints,
            [this](asio::error_code ec, std::size_t /*length*/)
            {
                if (!ec)
                {
                    send_msg_queue_.pop();
                    if (!send_msg_queue_.empty())
                    {
                        do_send();
                    }
                }
            }
        );
    }

    void send_message(const std::string& str)
    {
        asio::post(io_context_,
            [this, str]()
            {
                bool write_in_progress = !send_msg_queue_.empty();
                send_msg_queue_.push(str);
                if (!write_in_progress)
                {
                    do_send();
                }
            }
        );
    }

private:
    asio::io_context& io_context_;
    udp::socket socket_;
    udp::endpoint sender_endpoint_;
    enum { max_length = 1024 };
    char data_[max_length];
    struct sockaddr_in servaddr_;
    int server_port_;
    std::string server_ip_;
    std::queue<std::string> send_msg_queue_;
};

int main()
{
    asio::io_context io_context;
    client c(io_context, "127.0.0.1", 8888, 8889);
    std::thread t([&io_context]() { io_context.run(); });

    int ch = 0;
    do {
        ch = getchar();
        ch = toupper(ch);
        if ('A' == ch)
        {
            c.send_message("test");
        }
    } while (ch != 'Q');
}
