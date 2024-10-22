// tcpclient.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//

#include <deque>
#include <iostream>
#include <thread>
#include <asio.hpp>

using asio::ip::tcp;
typedef std::deque<std::string> chat_message_queue;

class chat_client
{
public:
    chat_client(asio::io_context& io_context, const tcp::resolver::results_type& endpoints)
        : io_context_(io_context), socket_(io_context)
    {
        do_connect(endpoints);
    }

    void write(const std::string& msg)
    {
        asio::post(io_context_,
            [this, msg]()
            {
                bool write_in_progress = !write_msgs_.empty();
                char head[9] = { 0 };
                sprintf_s(head, sizeof(head), "%08d\0", msg.size());
                write_msgs_.push_back(std::string(head) + msg);
                if (!write_in_progress)
                {
                    do_write();
                }
            }
        );
    }

    void close()
    {
        asio::post(io_context_, [this]() { socket_.close(); });
    }

private:
    void do_connect(const tcp::resolver::results_type& endpoints)
    {
        asio::async_connect(socket_, endpoints,
            [this](asio::error_code ec, tcp::endpoint)
            {
                if (!ec)
                {
                    do_read_header();
                }
            }
        );
    }

    void do_read_header()
    {
        asio::async_read(socket_, asio::buffer(data_, head),
            [this](asio::error_code ec, std::size_t /*length*/)
            {
                if (!ec)
                {
                    auto size = atoi(data_);
                    memset(data_, 0, max_length);
                    do_read_body(size);
                }
                else
                {
                    socket_.close();
                }
            }
        );
    }

    void do_read_body(int size)
    {
        asio::async_read(socket_, asio::buffer(data_, size),
            [this](asio::error_code ec, std::size_t /*length*/)
            {
                if (!ec)
                {
                    std::cout << "recv: " << data_ << std::endl;
                    memset(data_, 0, max_length);
                    do_read_header();
                }
                else
                {
                    socket_.close();
                }
            }
        );
    }

    void do_write()
    {
        asio::async_write(socket_, asio::buffer(write_msgs_.front().data(), write_msgs_.front().length()),
            [this](asio::error_code ec, std::size_t /*length*/)
            {
                if (!ec)
                {
                    write_msgs_.pop_front();
                    if (!write_msgs_.empty())
                    {
                        do_write();
                    }
                }
                else
                {
                    socket_.close();
                }
            }
        );
    }

private:
    asio::io_context& io_context_;
    tcp::socket socket_;
    enum { max_length = 1024, head = 8 };
    char data_[max_length];
    chat_message_queue write_msgs_;
};

int main()
{
    try {
        asio::io_context io_context;
        tcp::resolver resolver(io_context);
        auto endpoints = resolver.resolve("127.0.0.1", "8888");
        chat_client c(io_context, endpoints);
        std::thread t([&io_context]() { io_context.run(); });

        int ch = 0;
        do {
            ch = getchar();
            ch = toupper(ch);
            if ('A' == ch)
            {
                c.write("test");
            }
        } while (ch != 'Q');

        c.close();
        t.join();
    }
    catch (std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
    }
    return 0;
}
