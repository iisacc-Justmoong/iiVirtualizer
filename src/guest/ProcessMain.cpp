#include "WorkloadSandbox.h"
#include "FilesStore.h"
#include "ProcessBootstrap.h"
#include "../iiVirtualizer/Processes.h"
#include <iiFileProviderStorage.h>
#include <elf.h>
#include <algorithm>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cstring>

extern char** environ;
namespace v=ii::virtualizer;namespace g=v::guest;namespace fs=std::filesystem;
namespace {
int directory(std::string_view path) {
    int fd=iiFileProvider::native::openFile(g::workspace_root,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)throw std::runtime_error("Files root unavailable");
    while(!path.empty()) {
        const auto end=path.find('/');const std::string part(path.substr(0,end));const int next=iiFileProvider::native::openFileAt(fd,part.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);::close(fd);
        if(next<0)throw std::runtime_error("Working directory must be a direct Files directory");fd=next;if(end==path.npos)break;path.remove_prefix(end+1);
    }
    return fd;
}
void write_all(int fd,ii::core::ByteView bytes) {while(!bytes.empty()){const auto count=::write(fd,bytes.data(),bytes.size());if(count<0&&errno==EINTR)continue;if(count<=0)throw std::runtime_error("Cannot prepare the process image");bytes=bytes.subspan(static_cast<std::size_t>(count));}}
int image(std::string_view path) {
    g::FilesStore files(g::workspace_root);v::FileRequest request;request.action=v::FileAction::stat;request.path=path;
    auto stat=files.execute(request);if(!stat||!stat->entry||stat->entry->kind!=v::FileKind::regular||stat->entry->size<64||stat->entry->size>32*1024*1024)throw std::runtime_error("Files executable must be a bounded regular static ELF");
    const int fd=::memfd_create("iiv-program",MFD_CLOEXEC|MFD_ALLOW_SEALING);if(fd<0)throw std::runtime_error("Cannot allocate a sealed process image");
    std::vector<std::byte> header;
    request.action=v::FileAction::read;
    while(request.offset<stat->entry->size) {
        request.length=static_cast<std::size_t>(std::min<std::uint64_t>(v::file_chunk_limit,stat->entry->size-request.offset));auto block=files.execute(request);
        if(!block||block->data.size()!=request.length){::close(fd);throw std::runtime_error("Executable changed or could not be read");}
        if(header.size()<65536){const auto count=std::min<std::size_t>(65536-header.size(),block->data.size());header.insert(header.end(),block->data.begin(),block->data.begin()+static_cast<std::ptrdiff_t>(count));}
        write_all(fd,block->data);request.offset+=block->data.size();
    }
    Elf64_Ehdr elf{};std::memcpy(&elf,header.data(),sizeof(elf));
#if defined(__aarch64__)
    constexpr auto machine=EM_AARCH64;
#elif defined(__x86_64__)
    constexpr auto machine=EM_X86_64;
#else
#error An explicit process ELF machine is required.
#endif
    bool valid=std::memcmp(elf.e_ident,ELFMAG,SELFMAG)==0&&elf.e_ident[EI_CLASS]==ELFCLASS64&&elf.e_ident[EI_DATA]==ELFDATA2LSB&&elf.e_machine==machine&&(elf.e_type==ET_EXEC||elf.e_type==ET_DYN)&&elf.e_phnum&&elf.e_phnum<=512&&elf.e_phentsize==sizeof(Elf64_Phdr)&&elf.e_phoff<=header.size()&&static_cast<std::uint64_t>(elf.e_phnum)*sizeof(Elf64_Phdr)<=header.size()-elf.e_phoff;
    if(valid)for(unsigned i=0;i<elf.e_phnum;++i){Elf64_Phdr ph{};std::memcpy(&ph,header.data()+elf.e_phoff+i*sizeof(ph),sizeof(ph));if(ph.p_type==PT_INTERP)valid=false;}
    if(!valid||::fchmod(fd,0500)!=0||::fcntl(fd,F_ADD_SEALS,F_SEAL_WRITE|F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL)!=0){::close(fd);throw std::runtime_error("Executable must be a sealed static ELF for this guest architecture");}return fd;
}
}
int main(int argc,char** argv) {
    try {
        if(argc<3||argc>67||::getppid()!=1||::geteuid()!=0)throw std::runtime_error("Process launcher requires the trusted boot supervisor");
        const std::string id=std::getenv("IIV_PROCESS_ID")?std::getenv("IIV_PROCESS_ID"):"";if(!v::process_key(id))throw std::runtime_error("Missing owned process identity");
        v::ProcessLaunch request;request.key="validated";request.executable=argv[1];request.working_directory=argv[2];
        for(int i=3;i<argc;++i)request.arguments.emplace_back(argv[i]);
        for(char** item=environ;item&&*item;++item){const std::string_view entry=*item;const auto end=entry.find('=');if(end==entry.npos)throw std::runtime_error("Invalid bootstrap environment");const auto key=entry.substr(0,end);if(key!="IIV_PROCESS_ID"&&key!=g::process_environment_key)throw std::runtime_error("Unexpected privileged bootstrap environment key");}
        const char* encoded=std::getenv(g::process_environment_key);if(!encoded)throw std::runtime_error("Missing application environment payload");
        auto environment=g::process_bootstrap_environment(encoded);if(!environment)throw std::runtime_error("Invalid application environment payload");request.environment=std::move(*environment);
        if(!v::validate(request))throw std::runtime_error("Invalid process launch contract");
        {iiFileProvider::native::OutputFile control(fs::path("/sys/fs/cgroup/iiv")/id/"cgroup.procs");control<<"0";control.flush();if(!control)throw std::runtime_error("Cannot join the owned process tree");}
        const bool registered=request.executable.starts_with('@');const auto executable=registered?(request.executable=="@service"?fs::path("/app/service"):fs::path("/app/programs")/request.executable.substr(1)):fs::path("/sbin/ii-process");
        g::confine_workload({.runtime_root="/run/iiv-process",.executable=executable,.workspace=g::workspace_paths(),.require_home=false});
        if(::getuid()!=g::workload_uid||::geteuid()!=g::workload_uid||::prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)!=1||::prctl(PR_GET_SECCOMP,0,0,0,0)!=2)throw std::runtime_error("Process confinement was not retained");
        if(::clearenv()!=0||::setenv("PATH","/app",1)!=0||::setenv("HOME",(fs::path(g::workspace_root)/".home").c_str(),1)!=0||::setenv("TMPDIR",(fs::path(g::workspace_root)/".tmp").c_str(),1)!=0||::setenv("IIV_FILESYSTEM_PROFILE",g::filesystem_profile,1)!=0||::setenv("IIV_CONTAINER_ROOT",g::container_root,1)!=0)throw std::runtime_error("Cannot set process environment");
        for(const auto& [key,value]:request.environment)if(::setenv(key.c_str(),value.c_str(),1)!=0)throw std::runtime_error("Cannot set application environment");
        const int fd=directory(request.working_directory);if(::fchdir(fd)!=0){::close(fd);throw std::runtime_error("Cannot enter the Files working directory");}::close(fd);
        std::vector<std::string> arguments{registered?"/app/service":request.executable};arguments.insert(arguments.end(),request.arguments.begin(),request.arguments.end());std::vector<char*> pointers;for(auto& arg:arguments)pointers.push_back(arg.data());pointers.push_back(nullptr);
        if(registered)::execv("/app/service",pointers.data());
        else {const int binary=image(request.executable);(void)::syscall(SYS_execveat,binary,"",pointers.data(),environ,AT_EMPTY_PATH);::close(binary);}
        throw std::runtime_error("Cannot execute the confined process image");
    }catch(const std::exception& error){std::cerr<<"ii-process: blocked: "<<error.what()<<'\n';return 125;}
}
