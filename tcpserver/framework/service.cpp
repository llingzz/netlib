#include "service.h"
#include "../core/server.h"
#include "../core/session.h"
#include "codec/length_prefixed.h"

tcp_service::tcp_service()
    : codec_(std::make_unique<length_prefixed_codec>())
{
}

tcp_service::~tcp_service() = default;

void tcp_service::reply(session* sess, std::string_view payload)
{
    auto frame = codec_->encode(payload);
    server_->send_data(sess->token(), frame.data(), frame.size());
}

void tcp_service::reply_raw(session* sess, std::string_view data)
{
    server_->send_data(sess->token(), data.data(), data.size());
}

void tcp_service::send(uint64_t token, std::string_view payload)
{
    auto frame = codec_->encode(payload);
    server_->send_data(token, frame.data(), frame.size());
}
