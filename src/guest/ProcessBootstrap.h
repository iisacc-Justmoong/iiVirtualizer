#pragma once
#include "../iiVirtualizer/Processes.h"
#include <iiSDKCore/Process/Process.h>

namespace ii::virtualizer::guest {
// Untrusted application environment keys are serialized under one inert key.
// The privileged helper's libc must never receive them as its environment.
inline constexpr char process_environment_key[]="IIV_PROCESS_ENV";
[[nodiscard]] core::Result<ii::process::Launch> process_bootstrap_launch(const ProcessLaunch&);
[[nodiscard]] core::Result<ii::process::Environment> process_bootstrap_environment(std::string_view);
}
