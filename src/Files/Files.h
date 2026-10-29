#pragma once
#include <iiSDKCore/Core/Bytes/Bytes.h>
#include <iiSDKCore/Core/Result/Result.h>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ii::virtualizer {
inline constexpr std::size_t file_chunk_limit=16384,file_page_limit=16;
inline constexpr std::uint64_t file_offset_limit=16ULL*1024*1024*1024*1024;
enum class FileAction { list, stat, read, create, write, make_directory, move, remove };
enum class FileKind { regular, directory, symlink, other };
[[nodiscard]] std::string_view name(FileAction);
[[nodiscard]] std::string_view name(FileKind);
[[nodiscard]] core::Result<FileAction> file_action(std::string_view);
struct FileRequest final {
    FileAction action=FileAction::list;
    // Always relative to this VM's Society.societycontainer/Files. Empty means
    // the root for list/stat only. No absolute paths, dot segments or links.
    std::string path,destination;
    core::Bytes data;
    std::uint64_t offset=0;
    std::size_t length=file_chunk_limit,limit=file_page_limit;
    std::string cursor;
    bool include_hidden=false,truncate=false;
};
struct FileEntry final {
    std::string path,name;
    FileKind kind=FileKind::other;
    std::uint64_t size=0;
    std::int64_t modified_ns=0;
};
struct FileReply final {
    std::optional<FileEntry> entry;
    std::vector<FileEntry> entries;
    core::Bytes data;
    std::string next_cursor;
    std::uint64_t offset=0;
    bool eof=true,removed=false;
};
[[nodiscard]] core::Result<void> validate_file_path(std::string_view,bool allow_root=false);
[[nodiscard]] core::Result<void> validate(const FileRequest&);
} // namespace ii::virtualizer
