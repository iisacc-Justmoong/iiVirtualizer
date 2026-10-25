#pragma once
#include "Types.h"

namespace ii::virtualizer {
struct QemuOptions final {
    fs::path aarch64_executable = "qemu-system-aarch64";
    fs::path x86_64_executable = "qemu-system-x86_64";
    std::chrono::milliseconds timeout = std::chrono::seconds(5);
    std::chrono::milliseconds startup_timeout = std::chrono::seconds(30);
};
[[nodiscard]] Result<std::vector<std::string>> qemu_arguments(const Instance&, const QemuOptions& = {});
[[nodiscard]] Backend qemu_backend(QemuOptions = {});
} // namespace ii::virtualizer
