#include "../iiVirtualizer/GuestProtocol.h"
#include <iiSDKCore/Runtime/EventLoop.h>
#include <iiSDKCore/Runtime/Runtime.h>
#include <iiSDKCore/Runtime/Query.h>
#include <iiSDKCore/Process/Process.h>
#include <array>
#include <atomic>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs=std::filesystem;
namespace v=ii::virtualizer;
static volatile std::sig_atomic_t stopping=0;
static void stop_signal(int) { stopping=1; }
static bool run(std::vector<std::string> arguments,bool quiet=false) {
    ii::process::Launch launch;launch.arguments=std::move(arguments);launch.working_directory="/";
    launch.input=ii::process::Input::closed;
    if(!quiet) {launch.output=ii::process::Output::inherit;launch.error=ii::process::ErrorOutput::inherit;}
    auto process=ii::process::Process::start(launch);
    if(!process) {std::cerr<<"ii-init: "<<process.error().message<<'\n';return false;}
    ii::process::CommunicationOptions options;options.deadline=ii::core::Deadline::after(std::chrono::seconds(10));
    auto result=process->communicate({},options);
    return result&&result->exit.reason==ii::process::ExitReason::exited&&result->exit.code==0;
}
static bool busybox(std::initializer_list<std::string> arguments,bool quiet=false) {
    std::vector<std::string> command{"/bin/busybox"};command.insert(command.end(),arguments.begin(),arguments.end());return run(std::move(command),quiet);
}
static void mount_fs(const char* source,const char* target,const char* type,unsigned long flags=0,const char* options=nullptr) {
    fs::create_directories(target);
    if(::mount(source,target,type,flags,options)!=0&&errno!=EBUSY)throw std::system_error(errno,std::generic_category(),std::string("mount ")+target);
}
static std::string parameter(const std::string& commandline,std::string_view key) {
    std::istringstream tokens(commandline);std::string token;const auto prefix=std::string(key)+"=";
    while(tokens>>token)if(token.starts_with(prefix))return token.substr(prefix.size());return {};
}
static bool mount_data(bool initialize) {
    if(!fs::exists("/dev/vda"))return false;
    (void)busybox({"modprobe","crc32c_generic"},true);
    (void)busybox({"modprobe","ext4"},true);
    std::array<char,4096> sector{};
    std::ifstream disk("/dev/vda",std::ios::binary);disk.read(sector.data(),sector.size());
    bool blank=disk.gcount()==static_cast<std::streamsize>(sector.size());
    for(char c:sector)blank=blank&&c==0;
    if(blank && (!initialize||!run({"/sbin/mke2fs","-q","-t","ext4","-F","/dev/vda"})))return false;
    if(::mount("/dev/vda","/data","ext4",MS_NOATIME,nullptr)!=0) {
        std::cerr<<"ii-init: data volume mount failed: "<<std::strerror(errno)<<'\n';return false;
    }
    return true;
}
int main() {
    if(::getpid()!=1) {std::cerr<<"ii-init must run as Linux PID 1\n";return 1;}
    try {
        mount_fs("devtmpfs","/dev","devtmpfs",MS_NOSUID);
        int console=::open("/dev/console",O_RDWR|O_CLOEXEC);
        if(console>=0) {for(int n=0;n<3;++n)(void)::dup2(console,n);if(console>2)::close(console);}
        mount_fs("proc","/proc","proc",MS_NOSUID|MS_NODEV|MS_NOEXEC);
        mount_fs("sysfs","/sys","sysfs",MS_NOSUID|MS_NODEV|MS_NOEXEC);
        mount_fs("tmpfs","/run","tmpfs",MS_NOSUID|MS_NODEV,"mode=755,size=16m");
        mount_fs("tmpfs","/tmp","tmpfs",MS_NOSUID|MS_NODEV,"mode=1777,size=32m");
        std::ifstream cmd("/proc/cmdline");std::string commandline;std::getline(cmd,commandline);
        const auto uuid=parameter(commandline,"ii.uuid"),id=parameter(commandline,"ii.id");
        if(uuid.empty()||id.empty())throw std::runtime_error("Missing instance identity");
        (void)::sethostname(id.c_str(),id.size());
        (void)::setenv("PATH","/bin:/sbin:/usr/bin:/usr/sbin",1);
        (void)::signal(SIGTERM,stop_signal);(void)::signal(SIGINT,stop_signal);
        (void)::signal(SIGPIPE,SIG_IGN);
        (void)busybox({"modprobe","virtio_blk"},true);
        (void)busybox({"modprobe","virtio_net"},true);
        (void)busybox({"ip","link","set","lo","up"},true);
        // No default NIC is present unless the host explicitly opted in.
        if(fs::exists("/sys/class/net/eth0")) {
            (void)busybox({"modprobe","af_packet"},true);
            (void)busybox({"ip","link","set","eth0","up"});
            (void)busybox({"udhcpc","-i","eth0","-s","/etc/dhcp.script","-n","-q","-t","3"});
        }
        const bool volume=mount_data(parameter(commandline,"ii.format_data")=="1");
        std::uint64_t boots=0;
        if(volume) {
            {std::ifstream previous("/data/boot-count");previous>>boots;}
            ++boots;std::ofstream next("/data/boot-count");next<<boots<<'\n';next.flush();
        }
        ii::runtime::Runtime workers({.workers=2,.capacity=32});
        std::atomic<unsigned> tasks{0};
        for(unsigned i=0;i<4;++i)if(!workers.post([&]{tasks.fetch_add(1);}))throw std::runtime_error("iiSDK worker admission failed");
        if(!workers.wait_idle())throw std::runtime_error("iiSDK worker startup failed");
        ii::runtime::EventLoop loop(32);
        const auto boot=std::chrono::steady_clock::now();std::uint64_t ticks=0;
        std::optional<ii::process::Process> workload;
        if(fs::is_regular_file("/app/service")) {
            ii::process::Launch launch;launch.arguments={"/app/service"};launch.working_directory=volume?"/data":"/app";
            launch.input=ii::process::Input::closed;launch.output=ii::process::Output::inherit;launch.error=ii::process::ErrorOutput::inherit;
            auto started=ii::process::Process::start(launch);
            if(!started)std::cerr<<"ii-init: workload start failed: "<<started.error().message<<'\n';else workload.emplace(std::move(*started));
        }
        ii::runtime::QueryPort<v::GuestCommand,std::string> query([&](const v::GuestCommand& request) {
            std::string reply="IIV1 "+uuid+" ";
            if(request==v::GuestCommand::shutdown) {stopping=1;return reply+"STOPPING";}
            if(request==v::GuestCommand::ping)return reply+"PONG";
            struct sysinfo info{};(void)::sysinfo(&info);
            return reply+"INFO os=iiOS runtime=iiSDKCore pid=1 id="+id+" cpus="+std::to_string(::sysconf(_SC_NPROCESSORS_ONLN))+
                " memory_kib="+std::to_string(info.totalram*static_cast<std::uint64_t>(info.mem_unit)/1024)+
                " uptime_ms="+std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-boot).count())+
                " workers="+std::to_string(workers.stats().workers)+" tasks="+std::to_string(tasks.load())+
                " ticks="+std::to_string(ticks)+" volume="+(volume?"mounted":"unavailable")+" boot="+std::to_string(boots)+
                " workload="+(workload?"running":"none");
        });
        int agent=-1;std::string incoming,outgoing;
        auto pump=loop.every(std::chrono::milliseconds(20),[&] {
            ++ticks;
            if(agent<0&&fs::exists("/sys/class/virtio-ports")) {
                for(const auto& entry:fs::directory_iterator("/sys/class/virtio-ports")) {
                    std::ifstream name(entry.path()/"name");std::string value;std::getline(name,value);
                    if(value=="org.iisacc.agent")agent=::open((fs::path("/dev")/entry.path().filename()).c_str(),O_RDWR|O_NONBLOCK|O_CLOEXEC);
                }
            }
            if(agent>=0) {
                std::array<char,4096> bytes{};auto n=::read(agent,bytes.data(),bytes.size());
                if(n>0)incoming.append(bytes.data(),static_cast<std::size_t>(n));
                for(unsigned count=0;count<8;++count) {
                    auto end=incoming.find('\n');if(end==std::string::npos)break;
                    auto line=incoming.substr(0,end);incoming.erase(0,end+1);
                    auto command=v::guest_request(line,uuid);
                    if(command) {auto response=query.send(*command);if(response)outgoing+=*response+"\n";}
                }
                if(incoming.size()>4096||outgoing.size()>16384) {incoming.clear();outgoing.clear();}
                if(!outgoing.empty()) {auto nwrite=::write(agent,outgoing.data(),outgoing.size());if(nwrite>0)outgoing.erase(0,static_cast<std::size_t>(nwrite));}
            }
            if(workload) {auto state=workload->poll();if(state&&*state) {std::cout<<"ii-init: workload exited code="<<(**state).code<<'\n';workload.reset();}}
            // Process owners reap their own children; PID 1 also collects orphans.
            for(;;) {
                siginfo_t orphan{};
                if(::waitid(P_ALL,0,&orphan,WEXITED|WNOHANG|WNOWAIT)!=0||orphan.si_pid==0)break;
                if(workload&&static_cast<std::uint64_t>(orphan.si_pid)==workload->id())break;
                int status=0;(void)::waitpid(orphan.si_pid,&status,WNOHANG);
            }
            if(stopping&&outgoing.empty())loop.request_stop();
        });
        if(!pump)throw std::runtime_error("iiSDK EventLoop admission failed");
        std::cout<<"IIVIRTUALIZER_READY os=iiOS runtime=iiSDKCore pid=1 id="<<id<<" uuid="<<uuid<<" workers="<<workers.stats().workers<<" tasks="<<tasks.load()<<" volume="<<(volume?"mounted":"unavailable")<<" boot="<<boots<<std::endl;
        loop.run();
        if(agent>=0)::close(agent);
        workload.reset();
        (void)query.close();(void)workers.join();
        ::sync();if(volume)(void)::umount2("/data",0);
        std::cout<<"IIVIRTUALIZER_POWEROFF"<<std::endl;
        ::sync();(void)::reboot(RB_POWER_OFF);
    } catch(const std::exception& error) {std::cerr<<"ii-init: fatal: "<<error.what()<<std::endl;}
    for(;;)::pause();
}
