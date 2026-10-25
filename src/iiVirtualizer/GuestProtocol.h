#pragma once
#include "Types.h"

namespace ii::virtualizer {
enum class GuestCommand { info, ping, shutdown };
[[nodiscard]] Result<GuestCommand> guest_request(std::string_view line, std::string_view uuid);
[[nodiscard]] std::string guest_message(std::string_view uuid, GuestCommand);
} // namespace ii::virtualizer
