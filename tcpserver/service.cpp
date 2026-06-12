#include "service.h"
#include "core/server.h"
#include "core/session.h"

void tcp_service::reply(session* sess, std::string_view data)
{
    server_->send_message(sess->token(), std::string(data));
}

void tcp_service::send(uint64_t token, std::string_view data)
{
    server_->send_message(token, std::string(data));
}
