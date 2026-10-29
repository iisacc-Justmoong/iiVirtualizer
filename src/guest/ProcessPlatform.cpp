#include "ProcessPlatform.h"
#include "ProcessBootstrap.h"
#include "Workspace.h"
#include <iiFileProviderStorage.h>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <time.h>
#include <unistd.h>

namespace ii::virtualizer::guest {
namespace {
const std::filesystem::path cgroups="/sys/fs/cgroup",groups=cgroups/"iiv";
core::Result<void> write_control(const std::filesystem::path& path,std::string_view value) {
    const int fd=iiFileProvider::native::openFile(path.c_str(),O_WRONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)return core::fail(core::ErrorCode::failed,"Process cgroup control is unavailable");
    const auto n=::write(fd,value.data(),value.size());const int error=::close(fd);
    if(n!=static_cast<ssize_t>(value.size())||error)return core::fail(core::ErrorCode::failed,"Process cgroup control failed");return {};
}
std::filesystem::path group(std::string_view id) {
    if(!process_key(id))throw std::invalid_argument("Invalid owned process group");return groups/std::string(id);
}
std::uint64_t counter(const std::filesystem::path& path) {std::ifstream stream(path);std::uint64_t value=0;stream>>value;return value;}
}
ProcessPlatform::ProcessPlatform() {
    try {
        if(::getpid()!=1||::geteuid()!=0)return;
        iiFileProvider::native::createDirectories(cgroups);
        if(::mount("none",cgroups.c_str(),"cgroup2",MS_NOSUID|MS_NODEV|MS_NOEXEC,nullptr)!=0&&errno!=EBUSY)return;
        if(!write_control(cgroups/"cgroup.subtree_control","+cpu +memory +pids"))return;
        iiFileProvider::native::createDirectory(groups);if(::chmod(groups.c_str(),0700)!=0)return;
        if(!write_control(groups/"cgroup.subtree_control","+cpu +memory +pids"))return;
        iiFileProvider::native::createDirectory("/run/iiv-process");available_=true;
    }catch(...){available_=false;}
}
ProcessSupervisor::Launcher ProcessPlatform::launcher()const {
    return [this](const ProcessLaunch& request)->core::Result<ii::process::Launch> {
        if(!available_)return core::fail(core::ErrorCode::unsupported,"Required process cgroup containment is unavailable");
        if(request.executable.starts_with('@')) {
            const auto program=request.executable=="@service"?std::filesystem::path("/app/service"):std::filesystem::path("/app/programs")/request.executable.substr(1);
            if(!std::filesystem::is_regular_file(std::filesystem::symlink_status(program))||std::filesystem::canonical(program)!=program)return core::fail(core::ErrorCode::not_found,"Registered process program is unavailable");
        }
        return process_bootstrap_launch(request);
    };
}
ProcessSupervisor::Hooks ProcessPlatform::hooks()const {
    return {
        [](std::string_view id,const ProcessLaunch& launch)->core::Result<void>{
            const auto path=group(id);
            if(::mkdir(path.c_str(),0700)!=0)return core::fail(core::ErrorCode::failed,"Cannot prepare an owned process cgroup");
            for(const auto& [file,value]:std::vector<std::pair<std::string,std::string>>{{"memory.max",std::to_string(static_cast<std::uint64_t>(launch.memory_mib)*1024*1024)},
                {"memory.oom.group","1"},{"pids.max",std::to_string(launch.pids)}})if(auto result=write_control(path/file,value);!result){(void)::rmdir(path.c_str());return result;}
            if(!std::filesystem::exists(path/"cgroup.kill")){(void)::rmdir(path.c_str());return core::fail(core::ErrorCode::unsupported,"The kernel has no process-tree kill control");}return {};
        },
        [](std::string_view id){return write_control(group(id)/"cgroup.kill","1");},
        [](std::string_view id,ProcessSnapshot& value){const auto path=group(id);std::ifstream stream(path/"cpu.stat");std::string key;std::uint64_t count=0;while(stream>>key>>count)if(key=="usage_usec")value.cpu_usec=std::max(value.cpu_usec,count);
            value.memory_bytes=counter(path/"memory.current");value.peak_memory_bytes=std::max(value.peak_memory_bytes,counter(path/"memory.peak"));value.tasks=counter(path/"pids.current");},
        [](std::string_view id){const auto path=group(id);if(::rmdir(path.c_str())==0)return true;return errno==ENOENT;}
    };
}
VmMonitor ProcessPlatform::monitor(const ProcessSupervisor& processes,std::uint64_t sequence,const ii::runtime::RuntimeStats& workers,bool files)const {
    VmMonitor value;value.boot_id=processes.boot_id();value.filesystem_profile=filesystem_profile;value.sequence=sequence;
    timespec now{};if(::clock_gettime(CLOCK_BOOTTIME,&now)==0)value.uptime_ms=static_cast<std::uint64_t>(now.tv_sec)*1000+static_cast<std::uint64_t>(now.tv_nsec)/1000000;
    value.cpus=static_cast<std::uint64_t>(::sysconf(_SC_NPROCESSORS_ONLN));value.clock_ticks=static_cast<std::uint64_t>(::sysconf(_SC_CLK_TCK));
    struct sysinfo info{};if(::sysinfo(&info)==0){value.memory_total=info.totalram*static_cast<std::uint64_t>(info.mem_unit);value.memory_available=info.freeram*static_cast<std::uint64_t>(info.mem_unit);}
    std::ifstream memory("/proc/meminfo");std::string line;while(std::getline(memory,line))if(line.starts_with("MemAvailable:")){std::istringstream data(line.substr(13));std::uint64_t kib=0;if(data>>kib)value.memory_available=std::min(value.memory_total,kib*1024);break;}
    std::ifstream cpu("/proc/stat");if(std::getline(cpu,line)){std::istringstream data(line);std::string label;data>>label;for(unsigned i=0;i<8;++i){std::uint64_t ticks=0;if(!(data>>ticks))break;value.cpu_total_ticks+=ticks;if(i==3||i==4)value.cpu_idle_ticks+=ticks;}}
    value.running=processes.running();value.retained=processes.retained();value.workers=workers.workers;value.worker_pending=workers.pending;
    value.processes_available=available_;value.files_available=files;return value;
}
}
