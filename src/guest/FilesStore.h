#pragma once
#include "../Files/Files.h"
#include <filesystem>
namespace ii::virtualizer::guest {
// Caller must already be confined to a stable, Files-only mount namespace.
// Directory descriptors and O_NOFOLLOW retain the boundary during traversal.
class FilesStore final {
public:
    explicit FilesStore(const std::filesystem::path& root);
    ~FilesStore();
    FilesStore(const FilesStore&)=delete;
    FilesStore& operator=(const FilesStore&)=delete;
    [[nodiscard]] core::Result<FileReply> execute(const FileRequest&) const;
private:
    int root_=-1;
};
}
