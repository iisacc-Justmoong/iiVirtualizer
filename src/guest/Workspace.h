#pragma once
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

namespace ii::virtualizer::guest {
inline constexpr char container_root[]="/data/Society.societycontainer";
inline constexpr char workspace_root[]="/data/Society.societycontainer/Files";
inline constexpr char filesystem_profile[]="society-v1";
inline constexpr unsigned workload_uid=1000,workload_gid=1000;
struct Workspace final {
    std::filesystem::path container,files,home,temporary,state;
};
[[nodiscard]] Workspace workspace_paths(const std::filesystem::path& container=container_root);
// Trusted boot preparation, before any workload starts. Reuses the SDK's
// directory-package validator and never imports/moves sibling user data.
[[nodiscard]] std::expected<Workspace,std::string> prepare_workspace(const std::filesystem::path& parent);
[[nodiscard]] std::expected<std::uint64_t,std::string> advance_boot(const Workspace&);
} // namespace ii::virtualizer::guest
