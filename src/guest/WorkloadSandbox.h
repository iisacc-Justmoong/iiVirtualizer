#pragma once
#include "Workspace.h"

namespace ii::virtualizer::guest {
struct WorkloadSandbox final {
    std::filesystem::path runtime_root="/run/iiv-workload";
    std::filesystem::path executable="/app/service";
    Workspace workspace;
    bool require_home=true;
};
// Linux only, irreversible, single-threaded launcher process. On any failure
// the caller must exit without executing application code.
void confine_workload(const WorkloadSandbox&);
} // namespace ii::virtualizer::guest
