#pragma once
#include "../iiVirtualizer/Processes.h"
#include <iiSDKCore/Process/Process.h>
#include <functional>
#include <memory>

namespace ii::virtualizer::guest {
// One event-loop owner, nonblocking pipe pump, no thread per process.
class ProcessSupervisor final {
public:
    using Launcher=std::function<core::Result<ii::process::Launch>(const ProcessLaunch&)>;
    struct Hooks final {
        std::function<core::Result<void>(std::string_view,const ProcessLaunch&)> prepare;
        std::function<core::Result<void>(std::string_view)> kill;
        std::function<void(std::string_view,ProcessSnapshot&)> sample;
        std::function<bool(std::string_view)> retire;
    };
    explicit ProcessSupervisor(Launcher,Hooks = {});
    ~ProcessSupervisor();
    ProcessSupervisor(const ProcessSupervisor&)=delete;
    ProcessSupervisor& operator=(const ProcessSupervisor&)=delete;
    [[nodiscard]] core::Result<ProcessReply> execute(const ProcessRequest&);
    void tick();
    void shutdown();
    [[nodiscard]] bool owns(std::uint64_t pid) const;
    [[nodiscard]] std::size_t running() const;
    [[nodiscard]] std::size_t retained() const;
    [[nodiscard]] const std::string& boot_id() const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
