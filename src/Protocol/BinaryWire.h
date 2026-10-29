#pragma once
#include <iiSDKCore/Core/Bytes/Bytes.h>
#include <string>
#include <string_view>
namespace ii::virtualizer::wire_detail {
[[nodiscard]] std::string encode_binary(core::ByteView);
[[nodiscard]] core::Bytes decode_binary(std::string_view,std::size_t limit);
}
