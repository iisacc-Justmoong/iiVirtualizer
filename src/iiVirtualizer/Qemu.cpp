#include "Qemu.h"
#include "Qmp.h"
#include <iiSDKCore/Process/Process.h>
#include <algorithm>
#include <fstream>
#include <limits>
#if defined(__unix__) || defined(__APPLE__)
#include <csignal>
#include <unistd.h>
#include <sys/un.h>
#endif

namespace ii::virtualizer {
static Result<State> absent_endpoint(const Instance& vm) {
    // A missing endpoint alone does not prove that the volume is unused. PID
    // liveness is used only to refuse unsafe mutations, never to send a signal.
    if(!fs::exists(vm.paths.pid))return State::stopped;
    std::ifstream file(vm.paths.pid);std::uint64_t pid=0;file>>pid;
    if(!file||pid<2||pid>static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
        return core::fail(core::ErrorCode::busy,"Invalid owner pidfile; cannot prove that the instance is stopped");
#if defined(__unix__) || defined(__APPLE__)
    if(::kill(static_cast<pid_t>(pid),0)==0||errno!=ESRCH)
        return core::fail(core::ErrorCode::busy,"VM owner process exists but QMP is unavailable; refusing to treat it as stopped");
#endif
    return State::stopped;
}
static std::string option_path(const fs::path& path) {
    std::string result;
    for(char c:path.string()) {result+=c;if(c==',')result+=',';}
    return result;
}
Result<std::vector<std::string>> qemu_arguments(const Instance& vm,const QemuOptions& options) {
    if(options.timeout.count()<=0||options.startup_timeout.count()<=0)return core::fail(core::ErrorCode::invalid_argument,"QEMU deadlines must be positive");
    if(auto v=validate(vm.spec);!v)return std::unexpected(v.error());
    if(!detail::valid_uuid(vm.uuid))return core::fail(core::ErrorCode::invalid_argument,"Invalid VM UUID");
    const auto& s=vm.spec;
    auto accelerator=select_accelerator(s.architecture,s.accelerator);
    if(accelerator!=Accelerator::tcg && s.architecture!=host_architecture())
        return core::fail(core::ErrorCode::unsupported,"Hardware virtualization requires matching host and guest architectures; select tcg explicitly for emulation");
#if !defined(__APPLE__)
    if(accelerator==Accelerator::hvf)return core::fail(core::ErrorCode::unsupported,"HVF requires macOS");
#endif
#if !defined(__linux__)
    if(accelerator==Accelerator::kvm)return core::fail(core::ErrorCode::unsupported,"KVM requires Linux");
#endif
#if defined(__unix__) || defined(__APPLE__)
    if(vm.paths.qmp.string().size()>=sizeof(sockaddr_un{}.sun_path)||vm.paths.agent.string().size()>=sizeof(sockaddr_un{}.sun_path))
        return core::fail(core::ErrorCode::invalid_argument,"State directory makes AF_UNIX paths too long");
#else
    return core::fail(core::ErrorCode::unsupported,"Persistent QEMU adapter currently supports macOS and Linux");
#endif
    auto executable=s.architecture==Architecture::aarch64?options.aarch64_executable:options.x86_64_executable;
    std::vector<std::string> args{executable.string(),"-name",s.id,"-uuid",vm.uuid,"-nodefaults","-display","none","-monitor","none",
        "-machine",s.architecture==Architecture::aarch64?"virt,gic-version=3":"q35",
        "-accel",std::string(name(accelerator)),"-cpu",s.cpu_model.empty()?(accelerator==Accelerator::tcg?(s.architecture==Architecture::aarch64?"cortex-a72":"max"):"host"):s.cpu_model,
        "-smp",std::to_string(s.cpus),"-m",std::to_string(s.memory_mib)};
    if(s.boot_mode==BootMode::kernel) {
        auto line=s.kernel_command_line.empty()?std::string(s.architecture==Architecture::aarch64?"console=ttyAMA0":"console=ttyS0")+
            " rdinit=/init panic=-1 reboot=k ii.id="+s.id+" ii.uuid="+vm.uuid+
            " ii.format_data="+(s.initialize_disk?"1":"0"):s.kernel_command_line;
        args.insert(args.end(),{"-kernel",s.kernel.string(),"-initrd",s.initramfs.string(),"-append",std::move(line)});
    } else {
        if(s.architecture==Architecture::x86_64)
            args.insert(args.end(),{"-boot",s.boot_iso.empty()?"order=c":s.boot_disk_first?"order=cd,menu=off":"order=c,once=d,menu=off"});
        if(!s.firmware_vars.empty()) {
            args.insert(args.end(),{"-drive","if=pflash,format=raw,unit=0,readonly=on,file="+option_path(s.firmware),
                "-drive","if=pflash,format=raw,unit=1,file="+option_path(s.firmware_vars)});
        } else if(!s.firmware.empty())args.insert(args.end(),{"-bios",s.firmware.string()});
    }
    args.insert(args.end(),{"-qmp","unix:"+option_path(vm.paths.qmp)+",server=on,wait=off"});
    if(s.external_tpm) {
        args.insert(args.end(),{"-chardev","socket,id=tpm,path="+option_path(vm.paths.directory/"tpm.sock"),
            "-tpmdev","emulator,id=tpm0,chardev=tpm","-device",
            s.architecture==Architecture::aarch64?"tpm-tis-device,tpmdev=tpm0":"tpm-tis,tpmdev=tpm0"});
    }
    auto channel="socket,id=agent,path="+option_path(vm.paths.agent)+",server=on,wait=off";
    if(s.guest_channel==GuestChannel::serial) {
        channel+=",logfile="+option_path(vm.paths.console)+",logappend=off";
        args.insert(args.end(),{"-chardev",channel,"-serial","chardev:agent"});
    } else {
        args.insert(args.end(),{"-chardev","file,id=console,path="+option_path(vm.paths.console),"-serial","chardev:console",
            "-chardev",channel,"-device","virtio-serial-pci","-device","virtserialport,chardev=agent,name=org.iisacc.agent"});
    }
    auto block=[&](std::string id,const fs::path& path,bool readonly) {
        args.insert(args.end(),{"-blockdev",detail::json_text(detail::Json(detail::Json::Object{{"driver",detail::Json("raw")},
            {"node-name",detail::Json(id)},{"read-only",detail::Json(readonly)},
            {"file",detail::Json(detail::Json::Object{{"driver",detail::Json("file")},{"filename",detail::Json(path.string())}})}}))});
    };
    block("data",vm.paths.disk,false);
    auto disk=s.disk_bus==DiskBus::sata?std::string("ide-hd,drive=data,bus=ide.0"):
        s.disk_bus==DiskBus::nvme?"nvme,drive=data,serial=iiv"+vm.uuid.substr(0,8):std::string("virtio-blk-pci,drive=data");
    if(s.architecture==Architecture::aarch64 && s.boot_mode==BootMode::disk)
        disk+=(s.boot_iso.empty()||s.boot_disk_first)?",bootindex=1":",bootindex=2";
    args.insert(args.end(),{"-device",std::move(disk)});
    if(s.architecture==Architecture::aarch64 && (!s.boot_iso.empty() || !s.payload_iso.empty() || s.graphics==Graphics::ramfb))
        args.insert(args.end(),{"-device","qemu-xhci,id=usb","-device","usb-kbd,bus=usb.0","-device","usb-tablet,bus=usb.0"});
    auto media=[&](const char* id,const fs::path& path,const char* x86device,bool boot) {
        if(path.empty())return;
        if(s.architecture==Architecture::aarch64) {
            args.insert(args.end(),{"-drive","if=none,media=cdrom,format=raw,readonly=on,id="+std::string(id)+",file="+option_path(path),
                "-device","usb-storage,bus=usb.0,drive="+std::string(id)+(boot?(s.boot_disk_first?",bootindex=2":",bootindex=1"):"")});
        } else {block(id,path,true);args.insert(args.end(),{"-device",x86device});}
    };
    media("bootiso",s.boot_iso,"ide-cd,drive=bootiso,bus=ide.1",true);
    media("payload",s.payload_iso,"ide-cd,drive=payload,bus=ide.2",false);
    if(!s.allow_reboot)args.push_back("-no-reboot");
    args.insert(args.end(),{"-daemonize","-pidfile",vm.paths.pid.string()});
    if(s.graphics==Graphics::virtio_gpu)args.insert(args.end(),{"-device","virtio-gpu-pci"});
    if(s.graphics==Graphics::vga)args.insert(args.end(),{"-device","VGA"});
    if(s.graphics==Graphics::ramfb)args.insert(args.end(),{"-device","ramfb"});
    if(s.network) {
        std::string net="user,id=net,restrict=off";
        for(auto p:s.forwards)net+=",hostfwd=tcp:127.0.0.1:"+std::to_string(p.host)+"-:"+std::to_string(p.guest);
        // User networking is opt-in. Forwarded services bind only to loopback.
        args.insert(args.end(),{"-netdev",net,"-device","virtio-net-pci,netdev=net"});
    } else args.insert(args.end(),{"-nic","none"});
    return args;
}
Backend qemu_backend(QemuOptions options) {
    return {
        [options](const Instance& vm) -> Result<void> {
            auto args=qemu_arguments(vm,options);if(!args)return std::unexpected(args.error());
            if((vm.spec.boot_mode==BootMode::kernel&&(!fs::is_regular_file(vm.spec.kernel)||!fs::is_regular_file(vm.spec.initramfs)))||!fs::is_regular_file(vm.paths.disk))
                return core::fail(core::ErrorCode::not_found,"Instance boot artifacts or volume are missing");
            for(const auto& media:{vm.spec.boot_iso,vm.spec.payload_iso,vm.spec.firmware,vm.spec.firmware_vars})if(!media.empty()&&!fs::is_regular_file(media))
                return core::fail(core::ErrorCode::not_found,"Instance boot media is missing");
            // Replacing stale endpoints is allowed only after Runtime has
            // confirmed that the prior QMP endpoint does not own a live VM.
            for(auto p:{vm.paths.qmp,vm.paths.agent,vm.paths.pid}) {std::error_code e;fs::remove(p,e);if(e)return core::fail(core::ErrorCode::failed,e.message());}
            ii::process::Launch launch;launch.arguments=*args;launch.working_directory=vm.paths.directory;
            launch.input=ii::process::Input::closed;
            auto child=ii::process::Process::start(launch);if(!child)return std::unexpected(child.error());
            ii::process::CommunicationOptions communication;
            communication.deadline=ii::core::Deadline::after(options.startup_timeout);
            communication.output_limit=65536;communication.error_limit=65536;
            auto done=child->communicate({},communication);if(!done)return std::unexpected(done.error());
            if(done->exit.reason!=ii::process::ExitReason::exited||done->exit.code!=0) {
                std::string message(reinterpret_cast<const char*>(done->error.data()),done->error.size());
                return core::fail(core::ErrorCode::failed,"QEMU launch failed (reason="+std::to_string(static_cast<int>(done->exit.reason))+
                    ", code="+std::to_string(done->exit.code)+", signal="+std::to_string(done->exit.signal)+"): "+message);
            }
            auto ready=detail::qmp(vm,"query-status",options.timeout);
            if(!ready)return std::unexpected(ready.error());
            return {};
        },
        [options](const Instance& vm,Command command) -> Result<State> {
            std::string_view qcommand="query-status";
            if(command==Command::pause)qcommand="stop";
            if(command==Command::resume)qcommand="cont";
            if(command==Command::power_button)qcommand="system_powerdown";
            if(command==Command::terminate)qcommand="quit";
            auto response=detail::qmp(vm,qcommand,options.timeout);
            if(!response) {
                if(command==Command::inspect&&response.error().code==core::ErrorCode::not_found)return absent_endpoint(vm);
                return std::unexpected(response.error());
            }
            if(command==Command::terminate)return State::stopped;
            if(command==Command::pause)return State::paused;
            if(command==Command::resume)return State::running;
            if(command==Command::power_button)return State::running;
            auto running=detail::field(*response,"running").get<bool>();
            if(!running)return core::fail(core::ErrorCode::failed,"QMP running state has an invalid type");
            return running->get()?State::running:State::paused;
        },
        [options](const Instance& vm,std::string_view command) -> Result<std::string> {
            auto identity=detail::qmp(vm,"query-status",options.timeout);if(!identity)return std::unexpected(identity.error());
            return detail::agent(vm,command,options.timeout);
        },
        [options](const Instance& vm,const FileRequest& request,std::chrono::milliseconds timeout) -> Result<FileReply> {
            auto identity=detail::qmp(vm,"query-status",options.timeout);if(!identity)return std::unexpected(identity.error());
            return detail::files_agent(vm,request,timeout);
        },
        [options](const Instance& vm,const ProcessRequest& request,std::chrono::milliseconds timeout) -> Result<ProcessReply> {
            auto identity=detail::qmp(vm,"query-status",options.timeout);if(!identity)return std::unexpected(identity.error());
            return detail::processes_agent(vm,request,timeout);
        }
    };
}
} // namespace ii::virtualizer
