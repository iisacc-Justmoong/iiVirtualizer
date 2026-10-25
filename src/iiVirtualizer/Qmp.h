#pragma once
#include "Store.h"

namespace ii::virtualizer::detail {
[[nodiscard]] Result<Json> qmp(const Instance&, std::string_view command, std::chrono::milliseconds timeout);
[[nodiscard]] Result<std::string> agent(const Instance&, std::string_view command, std::chrono::milliseconds timeout);
} // namespace ii::virtualizer::detail
