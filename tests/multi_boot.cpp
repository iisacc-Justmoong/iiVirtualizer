#include <iiVirtualizer/Runtime.h>
#include <iiVirtualizer/Qemu.h>
#include "../src/iiVirtualizer/Store.h"
#include <fstream>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace v=ii::virtualizer;
using Json=ii::json::Value;
using namespace std::chrono_literals;
template<class T> T take(v::Result<T> value) {if(!value)throw std::runtime_error(value.error().message);return std::move(*value);}
void check(bool condition,const std::string& message) {if(!condition)throw std::runtime_error(message);}
struct Owned final {
    v::Runtime& runtime;
    std::vector<std::string> ids;
    ~Owned() {for(const auto& id:ids)try{(void)runtime.stop(id,true);}catch(...) {}}
};
std::string wait_guest(v::Runtime& runtime,const std::string& id) {
    const auto end=std::chrono::steady_clock::now()+45s;std::string last;
    do {
        auto response=runtime.guest_info(id);
        if(response) {
            check(response->find("volume=mounted")!=std::string::npos,*response);
            check(response->find("pid=1")!=std::string::npos,*response);
            check(response->find("id="+id+" ")!=std::string::npos,"Guest identity did not match its VM");
            return *response;
        }
        last=response.error().message;std::this_thread::sleep_for(20ms);
    }while(std::chrono::steady_clock::now()<end);
    throw std::runtime_error("Guest readiness timed out for "+id+": "+last);
}
Json outcomes(const std::vector<v::InstanceOutcome>& rows) {
    Json::Array values;
    for(const auto& row:rows) {
        Json::Object value{{"id",Json(row.id)},{"ok",Json(bool(row.result))}};
        if(row.result)value.emplace("instance",v::detail::instance_json(*row.result));
        else value.emplace("error",Json(row.result.error().message));
        values.emplace_back(std::move(value));
    }
    return Json(std::move(values));
}
int main(int argc,char** argv) {
    try {
        check(argc==9,"Expected STATE QEMU KERNEL INITRAMFS ACCEL PREFIX PORT_A PORT_B");
#if defined(__linux__)
        const auto marker=std::getenv("IIV_TEST_CONTAINER");
        check(marker && std::string(marker)=="1" && !v::fs::exists("/dev/kvm") && std::string(argv[5])=="tcg",
              "Real VM tests require a TCG-only Linux test container without /dev/kvm");
#else
        check(false,"Real VM tests require the dedicated Linux test container");
#endif
        v::QemuOptions options;options.aarch64_executable=v::fs::absolute(argv[2]);options.timeout=1s;
        // This is the only VM Runtime constructed by this controller process.
        v::Runtime runtime(argv[1],v::qemu_backend(options),{.workers=4,.capacity=16});
        Owned owned{runtime,{}};
        const std::string prefix=argv[6],a=prefix+"-a",b=prefix+"-b",c=prefix+"-c";
        const unsigned port_a=static_cast<unsigned>(std::stoul(argv[7])),port_b=static_cast<unsigned>(std::stoul(argv[8]));
        v::Accelerator accelerator;
        const std::string selected=argv[5];
        if(selected=="hvf")accelerator=v::Accelerator::hvf;
        else if(selected=="kvm")accelerator=v::Accelerator::kvm;
        else if(selected=="tcg")accelerator=v::Accelerator::tcg;
        else throw std::invalid_argument("Unknown accelerator");
        auto create=[&](const std::string& id,unsigned port,unsigned cpus,unsigned memory) {
            v::InstanceSpec spec;spec.id=id;spec.architecture=v::Architecture::aarch64;spec.accelerator=accelerator;
            spec.cpus=cpus;spec.memory_mib=memory;spec.disk_mib=64;
            spec.kernel=v::fs::absolute(argv[3]);spec.initramfs=v::fs::absolute(argv[4]);
            spec.network=true;spec.forwards={{port,8080}};
            auto created=take(runtime.create(spec));owned.ids.push_back(id);return created;
        };
        auto first=create(a,port_a,1,256),second=create(b,port_b,2,384);
        auto emit=[&](std::string_view event,Json::Object extra=Json::Object{}) {
            Json::Array instances;
            for(const auto& row:take(runtime.snapshot())) {
                check(bool(row.result),row.result?"":row.result.error().message);
                const auto& vm=*row.result;
                Json::Object value{{"instance",v::detail::instance_json(vm)}};
                std::uint64_t pid=0;std::ifstream(vm.paths.pid)>>pid;value.emplace("qemu_pid",Json(pid));
                if(vm.state==v::State::running)value.emplace("guest",Json(wait_guest(runtime,row.id)));
                instances.emplace_back(std::move(value));
            }
            extra.emplace("event",Json(event));extra.emplace("instances",Json(std::move(instances)));
            extra.emplace("controller_pid",Json(static_cast<std::uint64_t>(::getpid())));
            extra.emplace("runtime_instances",Json(1));extra.emplace("workers",Json(runtime.execution_stats().workers));
            std::cout<<v::detail::json_text(Json(std::move(extra)))<<std::endl;
        };
        auto started=take(runtime.apply(v::Operation::start,{a,b}));
        for(const auto& row:started)check(bool(row.result),row.result?"":row.result.error().message);
        emit("ready",{{"batch",outcomes(started)}});
        std::string command;
        while(std::getline(std::cin,command)) {
            if(command=="pause-first") {
                check(take(runtime.pause(a)).state==v::State::paused,"Pause failed");emit("paused");
            }else if(command=="resume-first") {
                check(take(runtime.resume(a)).state==v::State::running,"Resume failed");emit("resumed");
            }else if(command=="stop-first") {
                check(take(runtime.stop(a)).state==v::State::stopped,"Stop failed");emit("first-stopped");
            }else if(command=="restart-with-failure") {
                // The new VM deliberately conflicts with B's bound host port.
                // A restarts in the same batch while B continues serving.
                create(c,port_b,1,256);
                auto result=take(runtime.apply(v::Operation::start,{c,a}));
                check(!result[0].result&&bool(result[1].result),"Expected independent failed and successful batch rows");
                check(take(runtime.describe(b)).state==v::State::running,"Failed VM affected B");
                emit("partial-failure",{{"batch",outcomes(result)}});
            }else if(command=="quit") {
                auto result=take(runtime.apply(v::Operation::stop,owned.ids,15s));
                for(const auto& row:result)check(bool(row.result),row.result?"":row.result.error().message);
                emit("stopped",{{"batch",outcomes(result)}});return 0;
            }else throw std::invalid_argument("Unknown controller command");
        }
        throw std::runtime_error("Controller input closed before verification completed");
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
