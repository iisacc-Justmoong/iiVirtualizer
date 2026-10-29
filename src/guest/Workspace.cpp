#include "Workspace.h"
#include <DirectoryStorage.h>
#include <iiFileProviderStorage.h>
#include <charconv>
#include <fstream>
#include <limits>
#include <map>
#include <sys/stat.h>

namespace ii::virtualizer::guest {
namespace fs=std::filesystem;
namespace {
void direct_directory(const fs::path& path,fs::perms permissions) {
    const auto status=fs::symlink_status(path);
    if(status.type()==fs::file_type::not_found)iiFileProvider::native::createDirectory(path);
    if(!fs::is_directory(fs::symlink_status(path))||fs::canonical(path)!=path)
        throw std::runtime_error("Workspace directories must not be redirected");
    fs::permissions(path,permissions,fs::perm_options::replace);
}
void check_hard_links(const fs::path& root) {
    // A prebuilt raw volume may contain an inode linked both inside and outside
    // the package. Such an alias bypasses path-only confinement; reject it.
    struct Links {std::uintmax_t expected=0,inside=0;};
    std::map<std::pair<std::uintmax_t,std::uintmax_t>,Links> links;
    std::size_t count=0;
    for(const auto& entry:fs::recursive_directory_iterator(root)) {
        if(++count>1000000)throw std::runtime_error("Workspace validation exceeds the entry budget");
        struct stat info{};if(::lstat(entry.path().c_str(),&info)!=0)throw std::runtime_error("Cannot inspect workspace entry");
        if(S_ISREG(info.st_mode)&&info.st_nlink>1) {
            auto& row=links[{info.st_dev,info.st_ino}];row.expected=info.st_nlink;++row.inside;
        }
    }
    for(const auto& [id,row]:links) {(void)id;if(row.inside!=row.expected)throw std::runtime_error("A workspace hard link refers outside the package");}
}
std::uint64_t read_counter(const fs::path& path) {
    struct stat info{};
    if(::lstat(path.c_str(),&info)!=0) {
        if(errno==ENOENT)return 0;
        throw std::runtime_error("Cannot inspect boot metadata");
    }
    if(!S_ISREG(info.st_mode)||info.st_nlink!=1||info.st_size<1||info.st_size>32)
        throw std::runtime_error("Boot metadata must be a small direct regular file");
    std::ifstream input(path,std::ios::binary);std::string bytes((std::istreambuf_iterator<char>(input)),{});
    if(!input||bytes.empty())throw std::runtime_error("Cannot read boot metadata");
    while(!bytes.empty()&&(bytes.back()=='\n'||bytes.back()=='\r'))bytes.pop_back();
    std::uint64_t value=0;auto [end,error]=std::from_chars(bytes.data(),bytes.data()+bytes.size(),value);
    if(error!=std::errc{}||end!=bytes.data()+bytes.size()||value==std::numeric_limits<std::uint64_t>::max())
        throw std::runtime_error("Invalid boot metadata");
    return value;
}
}
Workspace workspace_paths(const fs::path& container) {
    Workspace value;value.container=container;value.files=container/"Files";value.state=container/".ii-runtime";
    value.home=value.files/".home";value.temporary=value.files/".tmp";return value;
}
std::expected<Workspace,std::string> prepare_workspace(const fs::path& parent) {
    try {
        auto package=iiSocietyContainer::DirectoryStorage::prepare(parent);
        if(!package)return std::unexpected(package.error());
        if(!iiSocietyContainer::DirectoryStorage::isPackageDirectory(package->path))
            throw std::runtime_error("A canonical Society directory package is required");
        check_hard_links(package->path);
        auto value=workspace_paths(package->path);
        constexpr auto directory=fs::perms::owner_all|fs::perms::group_read|fs::perms::group_exec|fs::perms::others_read|fs::perms::others_exec;
        direct_directory(value.container,directory);direct_directory(value.state,fs::perms::owner_all);
        direct_directory(value.files,directory);direct_directory(value.home,fs::perms::owner_all);direct_directory(value.temporary,fs::perms::owner_all);
        check_hard_links(value.files);
        return value;
    }catch(const std::exception& error){return std::unexpected(std::string(error.what()));}
}
std::expected<std::uint64_t,std::string> advance_boot(const Workspace& value) {
    try {
        if(!iiSocietyContainer::DirectoryStorage::isPackageDirectory(value.container)||value.state!=value.container/".ii-runtime"||
            !fs::is_directory(fs::symlink_status(value.state))||fs::canonical(value.state)!=value.state)
            throw std::runtime_error("Invalid trusted workspace metadata root");
        const auto path=value.state/"boot-count";
        const bool exists=fs::symlink_status(path).type()!=fs::file_type::not_found;
        const auto previous=read_counter(exists?path:value.container.parent_path()/"boot-count");
        iiFileProvider::native::OutputFile next(path,std::ios::binary|std::ios::trunc);
        next<<previous+1<<'\n';next.flush();if(!next)throw std::runtime_error("Cannot persist boot metadata");
        fs::permissions(path,fs::perms::owner_read|fs::perms::owner_write,fs::perm_options::replace);
        return previous+1;
    }catch(const std::exception& error){return std::unexpected(std::string(error.what()));}
}
}
