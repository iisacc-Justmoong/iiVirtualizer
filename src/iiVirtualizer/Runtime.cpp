#include "Runtime.h"
#include "Store.h"
#include <iiSDKCore/Runtime/Concurrent.h>
#include <iiSDKCore/Runtime/HostLoop.h>
#include <algorithm>
#include <optional>
#include <set>
#include <system_error>
#include <thread>

namespace ii::virtualizer {
template<class F> static auto guarded(F&& fn) -> decltype(fn()) {
    try { return fn(); }
    catch(const std::invalid_argument& e) {return core::fail(core::ErrorCode::invalid_argument,e.what());}
    catch(const std::system_error& e) {
        auto busy=e.code()==std::errc::resource_unavailable_try_again||e.code()==std::errc::device_or_resource_busy;
        return core::fail(busy?core::ErrorCode::busy:core::ErrorCode::failed,e.what());
    }
    catch(const std::exception& e) { return core::fail(core::ErrorCode::failed,e.what()); }
}
struct Runtime::Impl final {
    detail::Store store;
    Backend backend;
    ii::runtime::Runtime workers;
    ii::runtime::Concurrent operations;
    Impl(fs::path root, Backend engine, RuntimeOptions options)
        :store(std::move(root)),backend(std::move(engine)),
         workers({.workers=options.workers,.capacity=options.capacity}),operations(workers) {
        if(!backend.start||!backend.control||!backend.guest) throw std::invalid_argument("Backend requires start, control and guest functions");
    }
    ~Impl() {(void)operations.close();}
    Result<Instance> inspect(std::string_view id) {
        auto vm=store.load(id);if(!vm) return vm;
        auto state=backend.control(*vm,Command::inspect);
        if(!state) return std::unexpected(state.error());
        vm->state=*state;return vm;
    }
    Result<Instance> transition(std::string_view id, State from, Command command) {
        auto vm=inspect(id);if(!vm) return vm;
        if(vm->state!=from) return core::fail(core::ErrorCode::busy,"Instance state does not allow this operation");
        auto changed=backend.control(*vm,command);if(!changed) return std::unexpected(changed.error());
        vm->state=*changed;return vm;
    }
    Result<Instance> start(std::string_view id) {
        auto vm=inspect(id);if(!vm)return vm;
        if(vm->state!=State::stopped) return core::fail(core::ErrorCode::busy,"Instance is already active");
        auto started=backend.start(*vm);if(!started)return std::unexpected(started.error());
        return inspect(id);
    }
    Result<Instance> stop(std::string_view id,bool force,std::chrono::milliseconds timeout) {
        if(timeout.count()<0)return core::fail(core::ErrorCode::invalid_argument,"Shutdown timeout must not be negative");
        auto vm=inspect(id);if(!vm)return vm;
        if(vm->state==State::stopped) return vm;
        if(force) { auto stopped=backend.control(*vm,Command::terminate);if(!stopped) return std::unexpected(stopped.error()); }
        else {
            if(vm->state==State::paused) return core::fail(core::ErrorCode::busy,"Resume the paused instance before graceful shutdown, or use force");
            auto shutdown=backend.guest(*vm,"shutdown");
            if(!shutdown) return std::unexpected(shutdown.error());
        }
        const auto end=std::chrono::steady_clock::now()+timeout;
        do {
            auto observed=inspect(id);
            if(observed&&observed->state==State::stopped)return observed;
            if(!observed&&observed.error().code!=core::ErrorCode::busy)return observed;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } while(std::chrono::steady_clock::now()<end);
        return core::fail(core::ErrorCode::busy,"Shutdown timed out; the instance remains active. Force must be explicit");
    }
    Result<Instance> perform(Operation operation,std::string_view id,std::chrono::milliseconds timeout) {
        return guarded([&]() -> Result<Instance> {
            detail::Store::Lock lock(store,id);
            switch(operation) {
                case Operation::describe:return inspect(id);
                case Operation::start:return start(id);
                case Operation::pause:return transition(id,State::running,Command::pause);
                case Operation::resume:return transition(id,State::paused,Command::resume);
                case Operation::stop:return stop(id,false,timeout);
                case Operation::force_stop:return stop(id,true,timeout);
            }
            return core::fail(core::ErrorCode::invalid_argument,"Unknown instance operation");
        });
    }
};
Runtime::Runtime(fs::path root,Backend backend):Runtime(std::move(root),std::move(backend),{}) {}
Runtime::Runtime(fs::path root,Backend backend,RuntimeOptions options)
    :impl_(std::make_unique<Impl>(std::move(root),std::move(backend),options)) {}
Runtime::~Runtime()=default;
Result<Instance> Runtime::create(const InstanceSpec& spec) {
    return guarded([&]{detail::Store::Lock lock(impl_->store,spec.id);return impl_->store.create(spec);});
}
Result<Instance> Runtime::describe(std::string_view id) {return impl_->perform(Operation::describe,id,{});}
Result<Instance> Runtime::start(std::string_view id) {return impl_->perform(Operation::start,id,{});}
Result<Instance> Runtime::pause(std::string_view id) {return impl_->perform(Operation::pause,id,{});}
Result<Instance> Runtime::resume(std::string_view id) {return impl_->perform(Operation::resume,id,{});}
Result<Instance> Runtime::stop(std::string_view id,bool force,std::chrono::milliseconds timeout) {
    return impl_->perform(force?Operation::force_stop:Operation::stop,id,timeout);
}
Result<std::vector<Instance>> Runtime::list() {
    return guarded([&]() -> Result<std::vector<Instance>> {
        auto ids=impl_->store.ids();if(!ids)return std::unexpected(ids.error());
        std::vector<Instance> result;
        for(const auto& id:*ids) {
            auto vm=describe(id);if(!vm)return std::unexpected(vm.error());
            result.push_back(std::move(*vm));
        }
        return result;
    });
}
Result<void> Runtime::remove(std::string_view id) {
    return guarded([&]() -> Result<void> {
        detail::Store::Lock lock(impl_->store,id);auto vm=impl_->inspect(id);if(!vm) return std::unexpected(vm.error());
        if(vm->state!=State::stopped) return core::fail(core::ErrorCode::busy,"Stop the instance before removing its volume");
        return impl_->store.erase(*vm);
    });
}
Result<std::string> Runtime::guest_info(std::string_view id) {
    return guarded([&]() -> Result<std::string> {
        detail::Store::Lock lock(impl_->store,id);auto vm=impl_->inspect(id);if(!vm) return std::unexpected(vm.error());
        if(vm->state!=State::running) return core::fail(core::ErrorCode::busy,"Guest information requires a running instance");
        return impl_->backend.guest(*vm,"info");
    });
}
ii::runtime::Future<Instance> Runtime::submit(Operation operation,std::string id,std::chrono::milliseconds timeout) {
    return impl_->operations.run_with_promise<Instance>(
        [owner=impl_.get(),operation,id=std::move(id),timeout](ii::runtime::Promise<Instance>& promise) {
            auto result=owner->perform(operation,id,timeout);
            if(!result) {(void)promise.fail(std::move(result.error()));return;}
            auto added=promise.add_result(std::move(*result));
            if(!added)(void)promise.fail(std::move(added.error()));
        },{.max_results=1});
}
Result<std::vector<InstanceOutcome>> Runtime::apply(Operation operation,std::vector<std::string> ids,std::chrono::milliseconds timeout) {
    return guarded([&]() -> Result<std::vector<InstanceOutcome>> {
        if(ii::runtime::Runtime::is_worker_thread()||ii::runtime::HostLoop::is_current_thread())
            return core::fail(core::ErrorCode::wrong_executor,"Blocking batch operations require a caller thread; use submit on an executor");
        std::set<std::string> unique;
        for(const auto& id:ids) {
            InstanceSpec key;key.id=id;key.kernel="kernel";key.initramfs="initramfs";
            if(auto valid=validate(key);!valid)return std::unexpected(valid.error());
            if(!unique.insert(id).second)return core::fail(core::ErrorCode::invalid_argument,"Batch instance ids must be unique");
        }
        if(ids.empty())return std::vector<InstanceOutcome>{};
        const auto stats=impl_->workers.stats();const auto width=std::min({stats.workers,stats.capacity,ids.size()});
        if(width==0)return core::fail(core::ErrorCode::closed,"Runtime executor has no capacity");
        struct Batch final {
            std::vector<std::string> ids;
            std::vector<std::optional<Result<Instance>>> values;
            explicit Batch(std::vector<std::string> input):ids(std::move(input)),values(ids.size()) {}
        };
        auto data=std::make_shared<Batch>(std::move(ids));
        std::vector<ii::runtime::Future<void>> pending;pending.reserve(width);
        for(std::size_t lane=0;lane<width;++lane) {
            pending.push_back(impl_->operations.run_with_promise<void>(
                [owner=impl_.get(),data,lane,width,operation,timeout](ii::runtime::Promise<void>& promise) {
                    for(std::size_t i=lane;i<data->ids.size();i+=width) {
                        if(auto ready=promise.checkpoint();!ready) {(void)promise.fail(ready.error());return;}
                        // Different lanes write distinct elements of a fixed
                        // vector. All futures settle before any result is read.
                        data->values[i]=owner->perform(operation,data->ids[i],timeout);
                    }
                }));
        }
        std::vector<Result<void>> completed;completed.reserve(width);
        for(const auto& future:pending)completed.push_back(future.get());
        std::vector<InstanceOutcome> result;result.reserve(data->ids.size());
        for(std::size_t i=0;i<data->ids.size();++i) {
            if(data->values[i])result.push_back({data->ids[i],std::move(*data->values[i])});
            else if(!completed[i%width])result.push_back({data->ids[i],std::unexpected(completed[i%width].error())});
            else result.push_back({data->ids[i],core::fail(core::ErrorCode::failed,"Instance operation did not produce a result")});
        }
        return result;
    });
}
Result<std::vector<InstanceOutcome>> Runtime::snapshot() {
    return guarded([&]() -> Result<std::vector<InstanceOutcome>> {
        auto ids=impl_->store.ids();if(!ids)return std::unexpected(ids.error());
        return apply(Operation::describe,std::move(*ids));
    });
}
ii::runtime::RuntimeStats Runtime::execution_stats() const {return impl_->workers.stats();}
} // namespace ii::virtualizer
