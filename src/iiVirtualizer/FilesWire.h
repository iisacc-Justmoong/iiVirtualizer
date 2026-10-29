#pragma once
#include "Files.h"
#include <iiJson/Json.h>
#include <chrono>
namespace ii::virtualizer::files_detail {
inline constexpr std::size_t frame_limit=49152;
struct Envelope final {std::string uuid,id;FileRequest request;std::chrono::milliseconds timeout;};
[[nodiscard]] std::string nonce();
[[nodiscard]] ii::json::Value request_payload(const FileRequest&);
[[nodiscard]] core::Result<FileRequest> parse_request(const ii::json::Value&,FileAction);
[[nodiscard]] ii::json::Value reply_payload(const FileReply&);
[[nodiscard]] core::Result<FileReply> parse_reply(const ii::json::Value&,const FileRequest&);
[[nodiscard]] std::string encode_request(std::string_view uuid,std::string_view id,const FileRequest&,std::chrono::milliseconds);
[[nodiscard]] core::Result<Envelope> decode_request(std::string_view frame,std::string_view uuid);
[[nodiscard]] std::string encode_reply(std::string_view uuid,std::string_view id,FileAction,const core::Result<FileReply>&);
[[nodiscard]] core::Result<FileReply> decode_reply(std::string_view frame,std::string_view uuid,std::string_view id,const FileRequest&);
}
