#pragma once
#include "ProcessSupervisor.h"
#include <iiSDKCore/Runtime/Runtime.h>

namespace ii::virtualizer::guest {
// Linux adapter. The cgroup namespace and privileged paths never enter the
// application-facing process contract or the confined application's root.
class ProcessPlatform final {
public:
    ProcessPlatform();
    [[nodiscard]] bool available()const{return available_;}
    [[nodiscard]] ProcessSupervisor::Launcher launcher()const;
    [[nodiscard]] ProcessSupervisor::Hooks hooks()const;
    [[nodiscard]] VmMonitor monitor(const ProcessSupervisor&,std::uint64_t sequence,const ii::runtime::RuntimeStats&,bool files)const;
private:
    bool available_=false;
};
}
