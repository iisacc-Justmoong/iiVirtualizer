#include <iiVirtualizer/Runtime.h>
#include <iiVirtualizer/Qemu.h>
#include <iiVirtualizer/GuestProtocol.h>
#include <fstream>
#include <algorithm>
#include <iostream>
#include <map>
#include <stdexcept>

using namespace ii::virtualizer;
namespace core=ii::core;
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class T> T take(Result<T> value) {
    if (!value) throw std::runtime_error(value.error().message);
    return std::move(*value);
}
int main(int argc, char** argv) {
    try {
        check(argc == 2, "test requires an explicit build directory");
        fs::path root = fs::absolute(argv[1]) / ("r"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()%1000000));
        fs::create_directories(root);
        const std::string uuid="00000000-0000-4000-8000-000000000001";
        check(take(guest_request("IIV1 "+uuid+" info",uuid))==GuestCommand::info,"guest info parser");
        check(!guest_request("IIV1 wrong shutdown",uuid),"foreign guest identity accepted");
        check(!guest_request("IIV1 "+uuid+" exec",uuid),"unknown guest command accepted");
        check(!guest_request(std::string(5000,'x'),uuid),"unbounded guest command accepted");
        auto kernel = root / "kernel", initrd = root / "initrd";
        std::ofstream(kernel) << "fixture";
        std::ofstream(initrd) << "fixture";
        std::map<std::string, State> states;
        bool refuse = false;
        unsigned launches = 0;
        Backend backend {
            [&](const Instance& vm) -> Result<void> {
                if (refuse) return core::fail(core::ErrorCode::failed, "launch refused");
                ++launches; states[vm.uuid] = State::running; return {};
            },
            [&](const Instance& vm, Command command) -> Result<State> {
                auto& state = states.try_emplace(vm.uuid, State::stopped).first->second;
                if (command == Command::pause) state = State::paused;
                if (command == Command::resume) state = State::running;
                if (command == Command::terminate) state = State::stopped;
                return state;
            },
            [&](const Instance& vm, std::string_view command) -> Result<std::string> {
                if (command == "shutdown") states[vm.uuid] = State::stopped;
                return "IIV1 INFO pid=1 runtime=iiSDKCore";
            }
        };
        Runtime runtime(root / "state", backend);
        InstanceSpec spec{.id="demo", .kernel=kernel, .initramfs=initrd};
        for (const auto* id : {"", "../outside", "bad/id", ".", "UPPER", "has space"}) {
            auto bad=spec; bad.id=id; check(!runtime.create(bad), "unsafe id accepted");
        }
        auto bad=spec; bad.cpus=0; check(!runtime.create(bad), "zero CPU accepted");
        bad=spec; bad.memory_mib=0; check(!runtime.create(bad), "zero memory accepted");
        bad=spec; bad.network=false; bad.forwards={{8080,80}};
        check(!runtime.create(bad), "forward without network accepted");
        auto created=take(runtime.create(spec));
        check(created.state==State::stopped, "initial state");
        check(fs::file_size(created.paths.disk)==128ULL*1024*1024, "disk size");
        check(!runtime.create(spec), "duplicate overwrote instance");
        check(take(runtime.list()).size()==1, "list inventory");
        refuse=true; check(!runtime.start("demo"), "launch failure swallowed"); refuse=false;
        check(take(runtime.describe("demo")).state==State::stopped, "failed launch state");
        check(take(runtime.start("demo")).state==State::running, "start");
        check(!runtime.start("demo"), "duplicate launch accepted");
        check(launches==1, "duplicate backend launch");
        check(!runtime.remove("demo"), "removed live VM");
        check(take(runtime.pause("demo")).state==State::paused, "pause");
        check(take(runtime.resume("demo")).state==State::running, "resume");
        check(take(runtime.guest_info("demo")).find("iiSDKCore")!=std::string::npos, "guest contract");
        { Runtime reopened(root / "state", backend); check(take(reopened.describe("demo")).uuid==created.uuid, "persistent identity"); }
        auto args=take(qemu_arguments(created));
        QemuOptions invalid_deadline;invalid_deadline.startup_timeout=std::chrono::milliseconds(0);
        check(!qemu_arguments(created,invalid_deadline),"nonpositive launch deadline accepted");
        check(std::find(args.begin(),args.end(),"-daemonize")!=args.end(), "persistent QEMU invocation");
        check(std::find(args.begin(),args.end(),"-nodefaults")!=args.end(), "implicit devices");
        check(std::find(args.begin(),args.end(),"-nic")!=args.end(), "network policy");
        check(take(runtime.stop("demo")).state==State::stopped, "graceful stop");
        check(take(runtime.start("demo")).state==State::running, "restart");
        check(take(runtime.stop("demo",true)).state==State::stopped, "force stop");
        take(runtime.create(InstanceSpec{.id="persist",.kernel=kernel,.initramfs=initrd}));
        auto removed=runtime.remove("demo"); check(bool(removed), "remove stopped");
        check(!fs::exists(created.paths.directory), "instance storage remains");
        check(fs::exists(kernel), "removed source kernel");
        check(!runtime.describe("demo"), "missing instance accepted");
        std::cout << "iiVirtualizer contracts passed\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
