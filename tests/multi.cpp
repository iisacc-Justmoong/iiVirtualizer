#include <iiVirtualizer/Runtime.h>
#include <iiSDKCore/Runtime/Runtime.h>
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace v=ii::virtualizer;
using namespace std::chrono_literals;
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
template<class T> T take(v::Result<T> value) {if(!value)throw std::runtime_error(value.error().message);return std::move(*value);}
struct Engine final {
    std::mutex mutex;
    std::condition_variable changed;
    std::map<std::string,v::State> states;
    unsigned entered=0,active=0,peak=0,launches=0;
    bool hold=false,released=false;
    v::Backend backend() {
        return {
            [this](const v::Instance& vm) -> v::Result<void> {
                std::unique_lock lock(mutex);++launches;
                if(vm.spec.id=="fail")return ii::core::fail(ii::core::ErrorCode::failed,"injected launch failure");
                if(hold) {
                    ++entered;++active;peak=std::max(peak,active);changed.notify_all();
                    (void)changed.wait_for(lock,4s,[&]{return released;});--active;
                }
                states[vm.uuid]=v::State::running;return {};
            },
            [this](const v::Instance& vm,v::Command command) -> v::Result<v::State> {
                std::lock_guard lock(mutex);
                auto& state=states.try_emplace(vm.uuid,v::State::stopped).first->second;
                if(command==v::Command::pause)state=v::State::paused;
                if(command==v::Command::resume)state=v::State::running;
                if(command==v::Command::terminate)state=v::State::stopped;
                return state;
            },
            [this](const v::Instance& vm,std::string_view command) -> v::Result<std::string> {
                std::lock_guard lock(mutex);
                if(command=="shutdown")states[vm.uuid]=v::State::stopped;
                return "IIV1 "+vm.uuid+" INFO id="+vm.spec.id;
            }
        };
    }
    bool await(unsigned count) {std::unique_lock lock(mutex);return changed.wait_for(lock,2s,[&]{return entered>=count;});}
    void release() {std::lock_guard lock(mutex);released=true;hold=false;changed.notify_all();}
};
int main(int argc,char** argv) {
    try {
        check(argc==2,"build directory required");
        auto root=v::fs::absolute(argv[1])/("r"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()%1000000));
        v::fs::create_directories(root);auto kernel=root/"kernel",initrd=root/"initramfs";
        std::ofstream(kernel)<<"fixture";std::ofstream(initrd)<<"fixture";
        Engine engine;
        v::Runtime runtime(root/"state",engine.backend(),{.workers=4,.capacity=8});
        check(runtime.execution_stats().workers==4,"one runtime must own the configured worker pool");
        auto create=[&](std::string id) {return take(runtime.create({.id=std::move(id),.cpus=1,.memory_mib=64,.disk_mib=16,.kernel=kernel,.initramfs=initrd}));};
        auto alpha=create("alpha"),beta=create("beta");create("idle");
        check(alpha.uuid!=beta.uuid && alpha.paths.disk!=beta.paths.disk && alpha.paths.qmp!=beta.paths.qmp,"VM ownership must be distinct");
        engine.hold=true;
        auto first=runtime.submit(v::Operation::start,"alpha");
        auto second=runtime.submit(v::Operation::start,"beta");
        check(engine.await(2),"different VM launches must overlap on the same runtime");
        check(take(runtime.describe("idle")).state==v::State::stopped,"unrelated VM blocked by another launch");
        auto duplicate=runtime.submit(v::Operation::start,"alpha");
        check(duplicate.wait_for(1s)==ii::runtime::FutureWait::ready,"same-VM contention occupied a worker indefinitely");
        auto refused=duplicate.get();check(!refused&&refused.error().code==ii::core::ErrorCode::busy,"overlapping same-VM operation must return busy");
        {v::Runtime peer(root/"state",engine.backend());auto blocked=peer.describe("alpha");check(!blocked&&blocked.error().code==ii::core::ErrorCode::busy,"separate runtime did not respect instance lock");check(!peer.remove("alpha"),"another client deleted a starting VM");}
        auto snapshot=take(runtime.snapshot());check(snapshot.size()==3,"snapshot lost a busy VM");
        unsigned busy=0,healthy=0;
        for(const auto& row:snapshot) {if(row.result)++healthy;else if(row.result.error().code==ii::core::ErrorCode::busy)++busy;}
        check(busy==2&&healthy==1,"snapshot must preserve busy and healthy rows");
        engine.release();
        check(take(first.get()).state==v::State::running&&take(second.get()).state==v::State::running,"concurrent launch results");
        check(engine.peak>=2,"backend execution never overlapped");
        create("gamma");create("fail");create("untouched");
        auto batch=take(runtime.apply(v::Operation::start,{"gamma","fail","missing"}));
        check(batch.size()==3&&batch[0].id=="gamma"&&batch[1].id=="fail"&&batch[2].id=="missing","batch input order lost");
        check(batch[0].result&&batch[0].result->state==v::State::running,"successful VM rolled back by another failure");
        check(!batch[1].result&&batch[1].result.error().code==ii::core::ErrorCode::failed,"backend failure hidden");
        check(!batch[2].result&&batch[2].result.error().code==ii::core::ErrorCode::not_found,"missing VM failure hidden");
        check(!runtime.apply(v::Operation::start,{"untouched","untouched"}),"duplicate batch ids accepted");
        check(!runtime.apply(v::Operation::start,{"untouched","../escape"}),"unsafe batch id accepted");
        check(take(runtime.describe("untouched")).state==v::State::stopped,"invalid batch had partial effects");
        check(take(runtime.apply(v::Operation::describe,{})).empty(),"empty batch contract");
        check(take(runtime.stop("alpha")).state==v::State::stopped,"independent shutdown failed");
        check(take(runtime.describe("beta")).state==v::State::running,"stopping alpha affected beta");
        check(take(runtime.describe("beta")).uuid==beta.uuid&&v::fs::exists(beta.paths.disk),"another VM's identity or volume changed");
        auto stopped=take(runtime.apply(v::Operation::force_stop,{"beta","gamma"}));
        check(stopped[0].result&&stopped[1].result,"batch shutdown failed");
        std::atomic<bool> wrong_executor=false;
        ii::runtime::Runtime caller({.workers=1,.capacity=4});
        check(bool(caller.post([&]{auto value=runtime.apply(v::Operation::start,{"untouched"});wrong_executor=!value&&value.error().code==ii::core::ErrorCode::wrong_executor;})),"executor test submission");
        check(bool(caller.wait_idle())&&wrong_executor,"blocking batch must reject worker callers before dispatch");
        check(take(runtime.describe("untouched")).state==v::State::stopped,"wrong executor submitted work before rejecting");
        // One busy VM cannot consume every queue slot with lock waiters.
        Engine bounded_engine;bounded_engine.hold=true;
        v::Runtime bounded(root/"bounded",bounded_engine.backend(),{.workers=1,.capacity=2});
        for(auto id:{"a","b","c"})take(bounded.create({.id=id,.cpus=1,.memory_mib=64,.disk_mib=16,.kernel=kernel,.initramfs=initrd}));
        auto running=bounded.submit(v::Operation::start,"a");check(bounded_engine.await(1),"bounded startup did not enter backend");
        auto queued=bounded.submit(v::Operation::start,"b");auto full=bounded.submit(v::Operation::start,"c");
        check(full.wait_for(1s)==ii::runtime::FutureWait::ready,"full queue did not settle future");
        auto rejected=full.get();check(!rejected&&rejected.error().code==ii::core::ErrorCode::full,"queue saturation not reported");
        bounded_engine.release();take(running.get());take(queued.get());
        take(bounded.apply(v::Operation::force_stop,{"a","b"}));
        check(take(bounded.describe("c")).state==v::State::stopped,"rejected operation executed");
        // Batch size may exceed executor capacity: admission is windowed.
        auto many=take(bounded.apply(v::Operation::describe,{"a","b","c"}));
        check(many.size()==3&&many[0].result&&many[1].result&&many[2].result,"large batch overflowed bounded executor");
        Engine tiny_engine;
        v::Runtime tiny(root/"tiny",tiny_engine.backend(),{.workers=1,.capacity=1});
        for(auto id:{"a","b","c"})take(tiny.create({.id=id,.cpus=1,.memory_mib=64,.disk_mib=16,.kernel=kernel,.initramfs=initrd}));
        auto tiny_batch=take(tiny.apply(v::Operation::start,{"a","b","c"}));
        check(tiny_batch.size()==3&&tiny_batch[0].result&&tiny_batch[1].result&&tiny_batch[2].result,"single-slot batch lost work at future completion");
        Engine retiring;retiring.hold=true;
        ii::runtime::Future<v::Instance> active_ticket,queued_ticket;
        auto owner=std::make_unique<v::Runtime>(root/"retiring",retiring.backend(),v::RuntimeOptions{.workers=1,.capacity=2});
        auto live=take(owner->create({.id="live",.cpus=1,.memory_mib=64,.disk_mib=16,.kernel=kernel,.initramfs=initrd}));
        auto idle=take(owner->create({.id="idle",.cpus=1,.memory_mib=64,.disk_mib=16,.kernel=kernel,.initramfs=initrd}));
        active_ticket=owner->submit(v::Operation::start,"live");check(retiring.await(1),"retirement test never entered backend");
        queued_ticket=owner->submit(v::Operation::start,"idle");
        std::jthread release_retiring([&]{std::this_thread::sleep_for(20ms);retiring.release();});
        owner.reset();release_retiring.join();
        check(active_ticket.wait_for(1s)==ii::runtime::FutureWait::ready&&queued_ticket.wait_for(1s)==ii::runtime::FutureWait::ready,"owner destruction left an unsettled future");
        {std::lock_guard lock(retiring.mutex);check(retiring.launches==1&&retiring.states[live.uuid]==v::State::running,"retirement terminated the VM or executed queued work");check(!retiring.states.contains(idle.uuid)||retiring.states[idle.uuid]==v::State::stopped,"retired queued operation executed");}
        std::cout<<"Multi-VM overlap, independent locks, partial results, ownership and bounded admission passed\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
