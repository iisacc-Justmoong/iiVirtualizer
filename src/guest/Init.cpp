#include "../iiVirtualizer/GuestProtocol.h"
#include "Workspace.h"
#include "ProcessPlatform.h"
#include "../iiVirtualizer/ProcessWire.h"
#include "../iiVirtualizer/FilesWire.h"
#include <iiFileProviderStorage.h>
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
#include <stop_token>
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
    iiFileProvider::native::createDirectories(target);
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
    if(::mount("/dev/vda","/data","ext4",MS_NOATIME|MS_NOSUID|MS_NODEV,nullptr)!=0) {
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
        std::optional<v::guest::Workspace> workspace;
        if(volume) {
            auto prepared=v::guest::prepare_workspace("/data");
            if(prepared) {
                auto count=v::guest::advance_boot(*prepared);
                if(count) {
                    boots=*count;
                    auto owner=[](const fs::path& path,unsigned uid,unsigned gid){
                        if(::chown(path.c_str(),uid,gid)!=0)throw std::system_error(errno,std::generic_category(),"workspace owner");
                    };
                    owner(prepared->container,0,0);owner(prepared->state,0,0);
                    for(const auto& path:{prepared->files,prepared->home,prepared->temporary})owner(path,v::guest::workload_uid,v::guest::workload_gid);
                    workspace.emplace(std::move(*prepared));
                }else std::cerr<<"ii-init: workspace blocked: "<<count.error()<<'\n';
            }else std::cerr<<"ii-init: workspace blocked: "<<prepared.error()<<'\n';
        }
        ii::runtime::Runtime workers({.workers=2,.capacity=32});
        std::atomic<unsigned> tasks{0};
        for(unsigned i=0;i<4;++i)if(!workers.post([&]{tasks.fetch_add(1);}))throw std::runtime_error("iiSDK worker admission failed");
        if(!workers.wait_idle())throw std::runtime_error("iiSDK worker startup failed");
        ii::runtime::EventLoop loop(32);
        const auto boot=std::chrono::steady_clock::now();std::uint64_t ticks=0;
        std::optional<ii::process::Process> workload;
        struct FilePending final {std::atomic<bool> done=false;std::stop_source stop;std::string response;};
        std::shared_ptr<FilePending> pending_file;
        const bool files_ready=workspace&&fs::is_regular_file(fs::symlink_status("/sbin/ii-files"));
        const bool process_ready=workspace&&fs::is_regular_file(fs::symlink_status("/sbin/ii-process"));
        v::guest::ProcessPlatform process_platform;
        v::guest::ProcessSupervisor processes(process_platform.launcher(),process_platform.hooks());
        if(fs::is_regular_file("/app/service")) {
            if(!workspace||!fs::is_regular_file(fs::symlink_status("/sbin/ii-workload")))
                std::cerr<<"ii-init: workload blocked: Society workspace and sandbox launcher are required\n";
            else {
                ii::process::Launch launch;launch.arguments={"/sbin/ii-workload"};launch.working_directory="/";
                launch.inherit_environment=false;launch.environment["IIV_BOOT_COUNT"]=std::to_string(boots);
                launch.input=ii::process::Input::closed;launch.output=ii::process::Output::inherit;launch.error=ii::process::ErrorOutput::inherit;
                auto started=ii::process::Process::start(launch);
                if(!started)std::cerr<<"ii-init: workload start failed: "<<started.error().message<<'\n';else workload.emplace(std::move(*started));
            }
        }
        ii::runtime::QueryPort<v::GuestCommand,std::string> query([&](const v::GuestCommand& request) {
            std::string reply="IIV1 "+uuid+" ";
            if(request==v::GuestCommand::shutdown) {stopping=1;if(pending_file)pending_file->stop.request_stop();return reply+"STOPPING";}
            if(request==v::GuestCommand::ping)return reply+"PONG";
            struct sysinfo info{};(void)::sysinfo(&info);
            return reply+"INFO os=iiOS runtime=iiSDKCore pid=1 id="+id+" cpus="+std::to_string(::sysconf(_SC_NPROCESSORS_ONLN))+
                " memory_kib="+std::to_string(info.totalram*static_cast<std::uint64_t>(info.mem_unit)/1024)+
                " uptime_ms="+std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-boot).count())+
                " workers="+std::to_string(workers.stats().workers)+" tasks="+std::to_string(tasks.load())+
                " ticks="+std::to_string(ticks)+" volume="+(volume?"mounted":"unavailable")+" boot="+std::to_string(boots)+
                " workspace="+(workspace?workspace->container.string():"unavailable")+
                " filesystem_profile="+v::guest::filesystem_profile+" files_api="+(files_ready?"1":"0")+" processes_api="+(process_ready?"1":"0")+" workload="+(workload?"running":"none");
        });
        int agent=-1;std::string incoming,outgoing;
        auto pump=loop.every(std::chrono::milliseconds(20),[&] {
            ++ticks;processes.tick();
            if(agent<0&&fs::exists("/sys/class/virtio-ports")) {
                for(const auto& entry:fs::directory_iterator("/sys/class/virtio-ports")) {
                    std::ifstream name(entry.path()/"name");std::string value;std::getline(name,value);
                    if(value=="org.iisacc.agent")agent=::open((fs::path("/dev")/entry.path().filename()).c_str(),O_RDWR|O_NONBLOCK|O_CLOEXEC);
                }
            }
            if(agent>=0) {
                std::array<char,4096> bytes{};
                for(unsigned count=0;count<16;++count){auto n=::read(agent,bytes.data(),bytes.size());if(n>0)incoming.append(bytes.data(),static_cast<std::size_t>(n));else break;if(incoming.size()>v::files_detail::frame_limit)break;}
                for(unsigned count=0;count<8;++count) {
                    auto end=incoming.find('\n');if(end==std::string::npos)break;
                    auto line=incoming.substr(0,end);incoming.erase(0,end+1);
                    if(line.starts_with("IIVP1 ")) {
                        auto request=v::process_detail::decode_request(line,uuid);if(!request)continue;
                        ii::core::Result<v::ProcessReply> result=ii::core::fail(ii::core::ErrorCode::unsupported,"Guest process workspace/launcher is unavailable");
                        if(process_ready&&!stopping) {
                            if(request->request.action==v::ProcessAction::monitor){v::ProcessReply reply;reply.monitor=process_platform.monitor(processes,ticks,workers.stats(),files_ready);result=std::move(reply);}
                            else result=processes.execute(request->request);
                        }
                        outgoing+=v::process_detail::encode_reply(uuid,request->id,request->request.action,result);continue;
                    }
                    if(line.starts_with("IIVF1 ")) {
                        auto envelope=v::files_detail::decode_request(line,uuid);if(!envelope)continue;
                        auto failure=[&](ii::core::ErrorCode code,const char* message){return v::files_detail::encode_reply(uuid,envelope->id,envelope->request.action,ii::core::fail(code,message));};
                        if(!files_ready||stopping){outgoing+=failure(ii::core::ErrorCode::unsupported,"Guest Files workspace/worker is unavailable");continue;}
                        if(pending_file){outgoing+=failure(ii::core::ErrorCode::busy,"A guest Files operation is already pending");continue;}
                        auto pending=std::make_shared<FilePending>();const auto request=*envelope;
                        auto submitted=workers.post([pending,request,frame=std::move(line),uuid]() {
                            auto fail=[&](const char* message){return v::files_detail::encode_reply(uuid,request.id,request.request.action,ii::core::fail(ii::core::ErrorCode::failed,message));};
                            try {
                                ii::process::Launch launch;launch.arguments={"/sbin/ii-files"};launch.working_directory="/";
                                launch.inherit_environment=false;launch.environment["IIV_INSTANCE_UUID"]=uuid;
                                auto process=ii::process::Process::start(launch);
                                if(!process)pending->response=fail("Guest Files worker could not start");
                                else {
                                    const auto input=frame+"\n";ii::process::CommunicationOptions options;
                                    options.deadline=ii::core::Deadline::after(request.timeout);options.stop=pending->stop.get_token();
                                    options.output_limit=v::files_detail::frame_limit;options.error_limit=4096;
                                    auto result=process->communicate(ii::core::byte_view(input),options);
                                    if(!result||result->exit.reason!=ii::process::ExitReason::exited||result->exit.code||result->output_truncated||result->error_truncated)
                                        pending->response=fail("Guest Files worker failed or timed out; inspect state before retrying");
                                    else {
                                        const std::string output(reinterpret_cast<const char*>(result->output.data()),result->output.size());
                                        auto parsed=v::files_detail::decode_reply(output,uuid,request.id,request.request);
                                        pending->response=v::files_detail::encode_reply(uuid,request.id,request.request.action,parsed);
                                    }
                                }
                            }catch(...){pending->response=fail("Guest Files operation could not complete; inspect state before retrying");}
                            pending->done.store(true,std::memory_order_release);
                        });
                        if(submitted)pending_file=std::move(pending);else outgoing+=failure(ii::core::ErrorCode::busy,"Guest executor has no Files capacity");
                        continue;
                    }
                    auto command=v::guest_request(line,uuid);
                    if(command) {auto response=query.send(*command);if(response)outgoing+=*response+"\n";}
                }
                if(pending_file&&pending_file->done.load(std::memory_order_acquire)) {if(!stopping)outgoing+=pending_file->response;pending_file.reset();}
                if(incoming.size()>v::files_detail::frame_limit||outgoing.size()>2*v::files_detail::frame_limit) {incoming.clear();outgoing.clear();}
                if(!outgoing.empty()) {auto nwrite=::write(agent,outgoing.data(),outgoing.size());if(nwrite>0)outgoing.erase(0,static_cast<std::size_t>(nwrite));}
            }
            if(workload) {auto state=workload->poll();if(state&&*state) {std::cout<<"ii-init: workload exited code="<<(**state).code<<'\n';workload.reset();}}
            // Process owners reap their own children; PID 1 also collects orphans.
            for(;;) {
                // A Files job owns a Process on an executor thread. Do not
                // steal that child's exit while its communicate()/destructor
                // is still responsible for reaping it, including spawn races.
                if(pending_file)break;
                siginfo_t orphan{};
                if(::waitid(P_ALL,0,&orphan,WEXITED|WNOHANG|WNOWAIT)!=0||orphan.si_pid==0)break;
                if((workload&&static_cast<std::uint64_t>(orphan.si_pid)==workload->id())||processes.owns(static_cast<std::uint64_t>(orphan.si_pid)))break;
                int status=0;(void)::waitpid(orphan.si_pid,&status,WNOHANG);
            }
            if(stopping&&outgoing.empty()&&!pending_file)loop.request_stop();
        });
        if(!pump)throw std::runtime_error("iiSDK EventLoop admission failed");
        std::cout<<"IIVIRTUALIZER_READY os=iiOS runtime=iiSDKCore pid=1 id="<<id<<" uuid="<<uuid<<" workers="<<workers.stats().workers<<" tasks="<<tasks.load()<<" volume="<<(volume?"mounted":"unavailable")<<" boot="<<boots<<" workspace="<<(workspace?workspace->container.string():"unavailable")<<" filesystem_profile="<<v::guest::filesystem_profile<<std::endl;
        loop.run();
        if(agent>=0)::close(agent);
        processes.shutdown();workload.reset();
        (void)query.close();(void)workers.join();
        ::sync();if(volume)(void)::umount2("/data",0);
        std::cout<<"IIVIRTUALIZER_POWEROFF"<<std::endl;
        ::sync();(void)::reboot(RB_POWER_OFF);
    } catch(const std::exception& error) {std::cerr<<"ii-init: fatal: "<<error.what()<<std::endl;}
    for(;;)::pause();
}
