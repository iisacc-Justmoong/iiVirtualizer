#include <iiVirtualizer/Runtime.h>
#include <iiVirtualizer/Qemu.h>
#include "../Runtime/Store.h"
#include <iiSDKCore/Process/Process.h>
#include <charconv>
#include <fstream>
#include <iostream>
#include <set>
#include <thread>

namespace v=ii::virtualizer;
using Json=ii::json::Value;
template<class T> static T take(v::Result<T> value) {
    if(!value)throw std::runtime_error(value.error().message);
    return std::move(*value);
}
static void require(v::Result<void> value) {if(!value)throw std::runtime_error(value.error().message);}
static std::uint64_t number(std::string_view value) {
    std::uint64_t n=0;auto [end,error]=std::from_chars(value.data(),value.data()+value.size(),n);
    if(error!=std::errc{}||end!=value.data()+value.size())throw std::invalid_argument("Invalid integer: "+std::string(value));return n;
}
static void print(const Json& value) {std::cout<<v::detail::json_text(value)<<'\n';}
static Json outcome_json(const v::InstanceOutcome& row,bool flat=false) {
    if(row.result) {
        if(flat)return v::detail::instance_json(*row.result);
        return Json(Json::Object{{"id",Json(row.id)},{"ok",Json(true)},{"instance",v::detail::instance_json(*row.result)}});
    }
    return Json(Json::Object{{"id",Json(row.id)},{"ok",Json(false)},{"state",Json("unavailable")},
        {"error_code",Json(static_cast<int>(row.result.error().code))},{"error",Json(row.result.error().message)}});
}
static void help() {
    std::cout<<"iiVirtualizer "<<IIVIRTUALIZER_VERSION<<" - Linux + iiSDK virtual machine runtime\n"
        "Usage: iivirtualizer COMMAND [ID...] [OPTIONS]\n"
        "Commands: doctor, create, list, describe, start, pause, resume, stop, remove,\n"
        "          guest-info, wait-ready, console, plan\n"
        "          start-many, describe-many, pause-many, resume-many, stop-many\n"
        "Global: --root PATH --qemu-aarch64 PATH --qemu-x86_64 PATH --workers N --startup-timeout SECONDS --control-timeout SECONDS\n"
        "Create: --kernel PATH --initramfs PATH [--arch aarch64|x86_64]\n"
        "        [--accel auto|hvf|kvm|tcg] [--cpus N] [--cpu-model MODEL] [--memory MiB] [--disk MiB]\n"
        "        [--base-disk RAW] [--clone-base] [--network] [--forward HOST:GUEST]\n"
        "        [--kernel-args TEXT] [--graphics none|virtio-gpu|vga|ramfb]\n"
        "        [--boot kernel|disk] [--boot-iso ISO] [--payload-iso ISO] [--firmware FILE] [--firmware-vars FILE]\n"
        "        [--disk-bus virtio|sata|nvme] [--guest-channel virtio|serial] [--allow-reboot] [--external-tpm] [--boot-disk-first]\n"
        "Stop:   [--force] [--timeout SECONDS]\n"
        "Ready:  [--timeout SECONDS]\n";
}
int main(int argc,char** argv) {
    try {
        if(argc<2||std::string_view(argv[1])=="--help"||std::string_view(argv[1])=="help") {help();return 0;}
        const std::string command=argv[1];
        if(command=="--version") {std::cout<<IIVIRTUALIZER_VERSION<<'\n';return 0;}
        const std::set<std::string> commands{"doctor","create","list","describe","start","pause","resume","stop","remove","guest-info","wait-ready","console","plan","start-many","describe-many","pause-many","resume-many","stop-many"};
        if(!commands.contains(command))throw std::invalid_argument("Unknown command: "+command);
        v::fs::path root="build/instances";v::QemuOptions engine;
        v::RuntimeOptions runtime_options;std::vector<std::string> ids;const bool many=command.ends_with("-many");
        v::InstanceSpec spec;spec.architecture=v::host_architecture();
        bool force=false;std::uint64_t timeout=command=="wait-ready"?30:10;
        std::set<std::string> seen;
        for(int i=2;i<argc;++i) {
            std::string key=argv[i];
            if(!key.starts_with("--")) {if(many)ids.push_back(key);else {if(!spec.id.empty())throw std::invalid_argument("Unexpected argument: "+key);spec.id=key;}continue;}
            if(key!="--forward"&&!seen.insert(key).second)throw std::invalid_argument("Duplicate option: "+key);
            if(key=="--force") {if(command!="stop"&&command!="stop-many")throw std::invalid_argument("--force is only valid for stop or stop-many");force=true;continue;}
            if(key=="--network") {if(command!="create")throw std::invalid_argument("--network is only valid for create");spec.network=true;continue;}
            if(key=="--allow-reboot") {if(command!="create")throw std::invalid_argument("--allow-reboot is only valid for create");spec.allow_reboot=true;continue;}
            if(key=="--boot-disk-first") {if(command!="create")throw std::invalid_argument("--boot-disk-first is only valid for create");spec.boot_disk_first=true;continue;}
            if(key=="--external-tpm") {if(command!="create")throw std::invalid_argument("--external-tpm is only valid for create");spec.external_tpm=true;continue;}
            if(key=="--clone-base") {if(command!="create")throw std::invalid_argument("--clone-base is only valid for create");spec.clone_base_disk=true;continue;}
            if(i+1>=argc)throw std::invalid_argument("Missing value for "+key);
            std::string value=argv[++i];
            if(key=="--root")root=value;
            else if(key=="--qemu-aarch64")engine.aarch64_executable=value;
            else if(key=="--qemu-x86_64")engine.x86_64_executable=value;
            else if(key=="--startup-timeout") {auto n=number(value);if(n<1||n>3600)throw std::invalid_argument("Startup timeout must be 1..3600 seconds");engine.startup_timeout=std::chrono::seconds(n);}
            else if(key=="--control-timeout") {auto n=number(value);if(n<1||n>3600)throw std::invalid_argument("Control timeout must be 1..3600 seconds");engine.timeout=std::chrono::seconds(n);}
            else if(key=="--workers") {auto n=number(value);if(n<1||n>256)throw std::invalid_argument("Worker count must be 1..256");runtime_options.workers=static_cast<std::size_t>(n);}
            else if(key=="--timeout") {if(command!="stop"&&command!="stop-many"&&command!="wait-ready")throw std::invalid_argument("--timeout requires stop, stop-many or wait-ready");timeout=number(value);if(timeout>3600)throw std::invalid_argument("Timeout exceeds one hour");}
            else {
                if(command!="create")throw std::invalid_argument("Create option used with "+command+": "+key);
                if(key=="--kernel")spec.kernel=v::fs::absolute(value);
                else if(key=="--cpu-model") {if(value.empty())throw std::invalid_argument("CPU model cannot be empty");spec.cpu_model=value;}
                else if(key=="--initramfs")spec.initramfs=v::fs::absolute(value);
                else if(key=="--base-disk")spec.base_disk=v::fs::absolute(value);
                else if(key=="--kernel-args")spec.kernel_command_line=value;
                else if(key=="--boot-iso")spec.boot_iso=v::fs::absolute(value);
                else if(key=="--payload-iso")spec.payload_iso=v::fs::absolute(value);
                else if(key=="--firmware")spec.firmware=v::fs::absolute(value);
                else if(key=="--firmware-vars")spec.firmware_vars=v::fs::absolute(value);
                else if(key=="--boot") {if(value=="kernel")spec.boot_mode=v::BootMode::kernel;else if(value=="disk")spec.boot_mode=v::BootMode::disk;else throw std::invalid_argument("Unknown boot mode");}
                else if(key=="--disk-bus") {if(value=="virtio")spec.disk_bus=v::DiskBus::virtio;else if(value=="sata")spec.disk_bus=v::DiskBus::sata;else if(value=="nvme")spec.disk_bus=v::DiskBus::nvme;else throw std::invalid_argument("Unknown disk bus");}
                else if(key=="--guest-channel") {if(value=="virtio")spec.guest_channel=v::GuestChannel::virtio;else if(value=="serial")spec.guest_channel=v::GuestChannel::serial;else throw std::invalid_argument("Unknown guest channel");}
                else if(key=="--graphics") {if(value=="none")spec.graphics=v::Graphics::none;else if(value=="virtio-gpu")spec.graphics=v::Graphics::virtio_gpu;else if(value=="vga")spec.graphics=v::Graphics::vga;else if(value=="ramfb")spec.graphics=v::Graphics::ramfb;else throw std::invalid_argument("Unknown graphics device");}
                else if(key=="--cpus"||key=="--memory") {auto n=number(value);if(n>1048576)throw std::invalid_argument("Resource limit exceeded");if(key=="--cpus")spec.cpus=static_cast<unsigned>(n);else spec.memory_mib=static_cast<unsigned>(n);}
                else if(key=="--disk")spec.disk_mib=number(value);
                else if(key=="--arch") {if(value=="aarch64")spec.architecture=v::Architecture::aarch64;else if(value=="x86_64")spec.architecture=v::Architecture::x86_64;else throw std::invalid_argument("Unknown architecture");}
                else if(key=="--accel") {if(value=="auto")spec.accelerator=v::Accelerator::automatic;else if(value=="hvf")spec.accelerator=v::Accelerator::hvf;else if(value=="kvm")spec.accelerator=v::Accelerator::kvm;else if(value=="tcg")spec.accelerator=v::Accelerator::tcg;else throw std::invalid_argument("Unknown accelerator");}
                else if(key=="--forward") {auto colon=value.find(':');if(colon==std::string::npos)throw std::invalid_argument("Forward must be HOST:GUEST");auto host=number(std::string_view(value).substr(0,colon)),guest=number(std::string_view(value).substr(colon+1));if(host>65535||guest>65535)throw std::invalid_argument("Forward port exceeds 65535");spec.forwards.push_back({static_cast<unsigned>(host),static_cast<unsigned>(guest)});}
                else throw std::invalid_argument("Unknown option: "+key);
            }
        }
        if((many&&ids.empty())||(!many&&command!="list"&&command!="doctor"&&spec.id.empty()))throw std::invalid_argument("Instance id is required");
        if((command=="list"||command=="doctor")&&!spec.id.empty())throw std::invalid_argument("This command does not accept an instance id");
        if(command=="doctor") {
            auto executable=v::host_architecture()==v::Architecture::aarch64?engine.aarch64_executable:engine.x86_64_executable;
            ii::process::Launch launch;launch.arguments={executable.string(),"--version"};
            launch.input=ii::process::Input::closed;
            auto child=ii::process::Process::start(launch);bool available=false;std::string version;
            if(child) {ii::process::CommunicationOptions options;options.deadline=ii::core::Deadline::after(std::chrono::seconds(3));auto done=child->communicate({},options);if(done){available=done->exit.reason==ii::process::ExitReason::exited&&done->exit.code==0;version.assign(reinterpret_cast<const char*>(done->output.data()),done->output.size());}}
            print(Json(Json::Object{{"architecture",Json(v::name(v::host_architecture()))},{"native_accelerator",Json(v::name(v::native_accelerator()))},
                {"iisdk_process",Json(ii::process::capabilities().launch)},{"qemu_available",Json(available)},{"qemu",Json(executable.string())},{"version",Json(version)},
                {"hardware_boot_verified",Json(false)}}));return available?0:2;
        }
        v::Runtime runtime(root,v::qemu_backend(engine),runtime_options);
        if(many) {
            auto operation=v::Operation::describe;
            if(command=="start-many")operation=v::Operation::start;
            if(command=="pause-many")operation=v::Operation::pause;
            if(command=="resume-many")operation=v::Operation::resume;
            if(command=="stop-many")operation=force?v::Operation::force_stop:v::Operation::stop;
            auto results=take(runtime.apply(operation,std::move(ids),std::chrono::seconds(timeout)));
            Json::Array array;bool failed=false;
            for(const auto& row:results) {failed|=!row.result;array.push_back(outcome_json(row));}
            print(Json(std::move(array)));return failed?1:0;
        }
        if(command=="list") {
            Json::Array array;bool failed=false;
            for(const auto& row:take(runtime.snapshot())) {failed|=!row.result;array.push_back(outcome_json(row,true));}
            print(Json(std::move(array)));return failed?1:0;
        }
        else if(command=="create")print(v::detail::instance_json(take(runtime.create(spec))));
        else if(command=="describe")print(v::detail::instance_json(take(runtime.describe(spec.id))));
        else if(command=="start")print(v::detail::instance_json(take(runtime.start(spec.id))));
        else if(command=="pause")print(v::detail::instance_json(take(runtime.pause(spec.id))));
        else if(command=="resume")print(v::detail::instance_json(take(runtime.resume(spec.id))));
        else if(command=="stop")print(v::detail::instance_json(take(runtime.stop(spec.id,force,std::chrono::seconds(timeout)))));
        else if(command=="remove") {require(runtime.remove(spec.id));print(Json(Json::Object{{"removed",Json(spec.id)}}));}
        else if(command=="guest-info")print(Json(Json::Object{{"response",Json(take(runtime.guest_info(spec.id)))}}));
        else if(command=="wait-ready") {
            auto end=std::chrono::steady_clock::now()+std::chrono::seconds(timeout);
            std::string error="Guest is not ready";
            do {
                auto info=runtime.guest_info(spec.id);
                if(info) {print(Json(Json::Object{{"response",Json(*info)}}));return 0;}
                error=info.error().message;std::this_thread::sleep_for(std::chrono::milliseconds(100));
            } while(std::chrono::steady_clock::now()<end);
            throw std::runtime_error("Guest readiness timed out: "+error);
        } else if(command=="plan") {
            Json::Array array;for(const auto& arg:take(v::qemu_arguments(take(runtime.describe(spec.id)),engine)))array.emplace_back(arg);print(Json(std::move(array)));
        } else if(command=="console") {
            auto vm=take(runtime.describe(spec.id));std::ifstream file(vm.paths.console,std::ios::binary);
            if(!file)throw std::runtime_error("Console log is not available");
            auto size=v::fs::file_size(vm.paths.console);if(size>1024*1024)file.seekg(static_cast<std::streamoff>(size-1024*1024));std::cout<<file.rdbuf();
        }
        return 0;
    } catch(const std::exception& e) {std::cerr<<"iivirtualizer: "<<e.what()<<'\n';return 1;}
}
