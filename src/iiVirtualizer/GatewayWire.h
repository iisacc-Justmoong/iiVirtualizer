#pragma once
#include "Gateway.h"
#include "Store.h"

namespace ii::virtualizer::gateway_detail {
inline constexpr std::size_t request_limit=65536,reply_limit=2*1024*1024,batch_limit=128;
struct SecretText final {
    std::string value;
    explicit SecretText(std::string text):value(std::move(text)) {}
    ~SecretText();
};
struct Envelope final {GatewayRequest request;GatewayCredential credential;};
[[nodiscard]] bool key(std::string_view,std::size_t maximum=128);
[[nodiscard]] GatewayResult<void> validate(const GatewayRequest&);
[[nodiscard]] GatewayResult<void> validate(const GatewayCredential&);
[[nodiscard]] std::string encode_request(const GatewayRequest&,const GatewayCredential&);
[[nodiscard]] GatewayResult<Envelope> decode_request(std::string_view);
[[nodiscard]] std::string encode_reply(const GatewayReply&);
[[nodiscard]] std::string encode_error(std::string_view request,std::string_view runtime,GatewayError);
[[nodiscard]] GatewayResult<GatewayReply> decode_reply(std::string_view,const GatewayRequest&);
[[nodiscard]] GatewayResult<GatewayRequest> public_request(const detail::Json&);
} // namespace ii::virtualizer::gateway_detail
