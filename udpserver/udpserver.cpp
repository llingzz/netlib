// udpserver.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//

#include <iostream>
#include <asio.hpp>

using asio::ip::udp;
class server {
public:
    server(asio::io_context& io_context, short port)
        : socket_(io_context, udp::endpoint(udp::v4(), port))
    {
        do_receive();
    }

    void do_receive()
    {
        socket_.async_receive_from(asio::buffer(data_, max_length), sender_endpoint_,
            [this](asio::error_code ec, std::size_t bytes_recvd) {
                if (!ec && bytes_recvd > 0) {
                    do_send(bytes_recvd);
                }
                else {
                    do_receive();
                }
            }
        );
    }

    void do_send(std::size_t length)
    {
        socket_.async_send_to(asio::buffer(data_, length), sender_endpoint_,
            [this](asio::error_code /*ec*/, std::size_t /*bytes_sent*/) {
                do_receive();
            }
        );
    }

private:
    udp::socket socket_;
    udp::endpoint sender_endpoint_;
    enum { max_length = 1024 };
    char data_[max_length];
};

int main()
{
    asio::io_context io_context;
    server s(io_context, 8888);
    io_context.run();
    auto ch = getchar();
    return 0;
}
