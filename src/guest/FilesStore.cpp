#include "FilesStore.h"
#include <iiFileProviderStorage.h>
#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <limits>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#if defined(__linux__)
#include <sys/syscall.h>
#include <linux/fs.h>
#endif

namespace ii::virtualizer::guest {
namespace {
struct Fd final {
    int value=-1;
    explicit Fd(int fd):value(fd){if(fd<0)throw std::system_error(errno,std::generic_category(),"Files descriptor");}
    ~Fd(){if(value>=0)::close(value);}
    Fd(Fd&& other) noexcept:value(std::exchange(other.value,-1)){}
    Fd(const Fd&)=delete;Fd& operator=(const Fd&)=delete;
};
struct Parent final {Fd directory;std::string leaf;};
void require(int value,const char* operation){if(value<0)throw std::system_error(errno,std::generic_category(),operation);}
Fd directory(int root,std::string_view path) {
    // A dup would share the directory stream offset across independent pages.
    Fd current(iiFileProvider::native::openFileAt(root,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
    struct stat origin{};require(::fstat(root,&origin),"Files root metadata");
    while(!path.empty()) {
        const auto separator=path.find('/');const auto component=std::string(path.substr(0,separator));
        Fd child(iiFileProvider::native::openFileAt(current.value,component.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK));
        struct stat info{};require(::fstat(child.value,&info),"Files directory metadata");
        if(info.st_dev!=origin.st_dev)throw std::invalid_argument("Files traversal cannot cross a filesystem boundary");
        std::swap(current.value,child.value);
        if(separator==std::string_view::npos)break;path.remove_prefix(separator+1);
    }
    return current;
}
Parent parent(int root,const std::string& path) {
    const auto separator=path.rfind('/');
    return {directory(root,separator==std::string::npos?std::string_view{}:std::string_view(path).substr(0,separator)),
        separator==std::string::npos?path:path.substr(separator+1)};
}
FileEntry entry(const std::string& path,const struct stat& info) {
    FileEntry value;value.path=path;value.name=path.substr(path.rfind('/')==std::string::npos?0:path.rfind('/')+1);
    value.kind=S_ISREG(info.st_mode)?FileKind::regular:S_ISDIR(info.st_mode)?FileKind::directory:S_ISLNK(info.st_mode)?FileKind::symlink:FileKind::other;
    value.size=static_cast<std::uint64_t>(std::max<off_t>(0,info.st_size));
#if defined(__APPLE__)
    const auto seconds=static_cast<std::int64_t>(info.st_mtimespec.tv_sec),nanos=static_cast<std::int64_t>(info.st_mtimespec.tv_nsec);
#else
    const auto seconds=static_cast<std::int64_t>(info.st_mtim.tv_sec),nanos=static_cast<std::int64_t>(info.st_mtim.tv_nsec);
#endif
    constexpr auto maximum=std::numeric_limits<std::int64_t>::max(),minimum=std::numeric_limits<std::int64_t>::min();
    if(seconds>maximum/1000000000)value.modified_ns=maximum;
    else if(seconds<minimum/1000000000)value.modified_ns=minimum;
    else {const auto base=seconds*1000000000;value.modified_ns=base>maximum-nanos?maximum:base+nanos;}
    return value;
}
FileEntry metadata(int root,const std::string& path) {
    struct stat info{};
    if(path.empty())require(::fstat(root,&info),"Files root metadata");
    else {auto location=parent(root,path);require(::fstatat(location.directory.value,location.leaf.c_str(),&info,AT_SYMLINK_NOFOLLOW),"Files entry metadata");}
    return entry(path,info);
}
void write_all(int descriptor,std::uint64_t offset,core::ByteView bytes) {
    while(!bytes.empty()) {
        const auto n=::pwrite(descriptor,bytes.data(),bytes.size(),static_cast<off_t>(offset));
        if(n<0){if(errno==EINTR)continue;require(-1,"Files write may be partially applied; inspect before retrying");}
        if(!n)throw std::runtime_error("Files write accepted no bytes");
        offset+=static_cast<std::uint64_t>(n);bytes=bytes.subspan(static_cast<std::size_t>(n));
    }
}
void move_exclusive(const Parent& source,const Parent& target) {
#if defined(__linux__)
    require(static_cast<int>(::syscall(SYS_renameat2,source.directory.value,source.leaf.c_str(),target.directory.value,target.leaf.c_str(),RENAME_NOREPLACE)),"Files no-replace move");
#elif defined(__APPLE__)
    require(::renameatx_np(source.directory.value,source.leaf.c_str(),target.directory.value,target.leaf.c_str(),RENAME_EXCL),"Files no-replace move");
#else
    (void)source;(void)target;throw std::invalid_argument("Atomic no-replace moves are unavailable on this platform");
#endif
}
}
FilesStore::FilesStore(const std::filesystem::path& root) {
    if(!root.is_absolute()||std::filesystem::canonical(root)!=root||std::filesystem::is_symlink(root))
        throw std::invalid_argument("Files root must be a direct canonical directory");
    root_=iiFileProvider::native::openFile(root.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(root_<0)throw std::system_error(errno,std::generic_category(),"open Files root");
}
FilesStore::~FilesStore(){if(root_>=0)::close(root_);}
core::Result<FileReply> FilesStore::execute(const FileRequest& request) const {
    if(auto valid=validate(request);!valid)return std::unexpected(valid.error());
    try {
        FileReply reply;
        if(request.action==FileAction::list) {
            auto folder=directory(root_,request.path);
            DIR* raw=::fdopendir(std::exchange(folder.value,-1));if(!raw)throw std::system_error(errno,std::generic_category(),"enumerate Files");
            struct Directory final {DIR* value;~Directory(){::closedir(value);}} owner{raw};
            std::map<std::string,FileEntry,std::less<>> selected;std::size_t scanned=0;
            for(;;) {
                errno=0;auto item=::readdir(raw);if(!item){if(errno)throw std::system_error(errno,std::generic_category(),"read Files directory");break;}
                const std::string leaf=item->d_name;if(leaf=="."||leaf=="..")continue;
                if(++scanned>100000)throw std::runtime_error("Files directory exceeds the enumeration budget");
                if((!request.include_hidden&&leaf.starts_with('.'))||leaf<=request.cursor)continue;
                if(auto valid=validate_file_path(leaf);!valid)throw std::invalid_argument("Directory contains a name outside the Files path contract");
                const auto path=request.path.empty()?leaf:request.path+"/"+leaf;
                if(auto valid=validate_file_path(path);!valid)throw std::invalid_argument("Directory entry exceeds the Files path limit");
                struct stat info{};if(::fstatat(::dirfd(raw),leaf.c_str(),&info,AT_SYMLINK_NOFOLLOW)<0){if(errno==ENOENT)continue;require(-1,"Files list metadata");}
                selected.emplace(leaf,entry(path,info));
                if(selected.size()>request.limit+1)selected.erase(std::prev(selected.end()));
            }
            if(selected.size()>request.limit){selected.erase(std::prev(selected.end()));reply.next_cursor=selected.rbegin()->first;}
            for(auto& [leaf,value]:selected){(void)leaf;reply.entries.push_back(std::move(value));}
            return reply;
        }
        if(request.action==FileAction::stat){reply.entry=metadata(root_,request.path);return reply;}
        auto location=parent(root_,request.path);
        if(request.action==FileAction::make_directory) {
            require(iiFileProvider::native::makeDirectoryAt(location.directory.value,location.leaf.c_str(),0700),"Files create directory");
            require(::fsync(location.directory.value),"Files persist directory");reply.entry=metadata(root_,request.path);return reply;
        }
        if(request.action==FileAction::move) {
            auto target=parent(root_,request.destination);move_exclusive(location,target);
            require(::fsync(location.directory.value),"Files persist move source");require(::fsync(target.directory.value),"Files persist move target");
            reply.entry=metadata(root_,request.destination);return reply;
        }
        if(request.action==FileAction::remove) {
            struct stat info{};require(::fstatat(location.directory.value,location.leaf.c_str(),&info,AT_SYMLINK_NOFOLLOW),"Files delete metadata");
            require(::unlinkat(location.directory.value,location.leaf.c_str(),S_ISDIR(info.st_mode)?AT_REMOVEDIR:0),"Files remove (directories must be empty)");
            require(::fsync(location.directory.value),"Files persist deletion");reply.removed=true;return reply;
        }
        const bool reading=request.action==FileAction::read;
        int flags=(reading?O_RDONLY:O_WRONLY)|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK;
        if(request.action==FileAction::create)flags|=O_CREAT|O_EXCL;
        Fd file(iiFileProvider::native::openFileAt(location.directory.value,location.leaf.c_str(),flags,0600));
        struct stat info{};require(::fstat(file.value,&info),"Files file metadata");
        if(!S_ISREG(info.st_mode))throw std::invalid_argument("Files content operations require a regular non-symlink file");
        if(reading) {
            reply.offset=request.offset;reply.data.resize(request.length);
            for(;;) {
                const auto count=::pread(file.value,reply.data.data(),reply.data.size(),static_cast<off_t>(request.offset));
                if(count<0){if(errno==EINTR)continue;require(-1,"Files read");}
                reply.data.resize(static_cast<std::size_t>(count));break;
            }
            reply.eof=request.offset+reply.data.size()>=static_cast<std::uint64_t>(info.st_size);
        }else {
            if(request.offset>static_cast<std::uint64_t>(info.st_size))return core::fail(core::ErrorCode::out_of_range,"Files writes cannot create sparse gaps");
            write_all(file.value,request.offset,request.data);
            if(request.truncate)require(::ftruncate(file.value,static_cast<off_t>(request.offset+request.data.size())),"Files truncate");
            require(::fsync(file.value),"Files persist file");
            if(request.action==FileAction::create)require(::fsync(location.directory.value),"Files persist file entry");
        }
        require(::fstat(file.value,&info),"Files resulting metadata");reply.entry=entry(request.path,info);return reply;
    }catch(const std::invalid_argument& error){return core::fail(core::ErrorCode::invalid_argument,error.what());}
    catch(const std::system_error& error) {
        const int value=error.code().value();const auto code=value==ENOENT?core::ErrorCode::not_found:value==EEXIST?core::ErrorCode::already_exists:
            (value==ELOOP||value==ENOTDIR||value==EACCES||value==EPERM||value==EINVAL)?core::ErrorCode::invalid_argument:
            (value==ENOTEMPTY||value==EBUSY)?core::ErrorCode::busy:core::ErrorCode::failed;
        return core::fail(code,error.what());
    }catch(const std::exception& error){return core::fail(core::ErrorCode::failed,error.what());}
}
}
