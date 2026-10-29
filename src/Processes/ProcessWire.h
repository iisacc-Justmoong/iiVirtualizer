#pragma once
#include <iiVirtualizer/Processes.h>
#include <iiJson/Json.h>

namespace ii::virtualizer::process_detail {
inline constexpr std::size_t frame_limit=49152;
struct Envelope final {std::string uuid,id;ProcessRequest request;};
[[nodiscard]] ii::json::Value request_payload(const ProcessRequest&);
[[nodiscard]] core::Result<ProcessRequest> parse_request(const ii::json::Value&,ProcessAction);
[[nodiscard]] ii::json::Value reply_payload(const ProcessReply&);
[[nodiscard]] core::Result<ProcessReply> parse_reply(const ii::json::Value&,const ProcessRequest&);
[[nodiscard]] std::string encode_request(std::string_view uuid,std::string_view id,const ProcessRequest&);
[[nodiscard]] core::Result<Envelope> decode_request(std::string_view,std::string_view uuid);
[[nodiscard]] std::string encode_reply(std::string_view uuid,std::string_view id,ProcessAction,const core::Result<ProcessReply>&);
[[nodiscard]] core::Result<ProcessReply> decode_reply(std::string_view,std::string_view uuid,std::string_view id,const ProcessRequest&);
}
