#pragma once
#include "Store.h"

namespace ii::virtualizer::detail {
[[nodiscard]] Result<Json> qmp(const Instance&, std::string_view command, std::chrono::milliseconds timeout);
[[nodiscard]] Result<std::string> agent(const Instance&, std::string_view command, std::chrono::milliseconds timeout);
[[nodiscard]] Result<FileReply> files_agent(const Instance&, const FileRequest&,std::chrono::milliseconds timeout);
[[nodiscard]] Result<ProcessReply> processes_agent(const Instance&,const ProcessRequest&,std::chrono::milliseconds);
} // namespace ii::virtualizer::detail
