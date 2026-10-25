#include "Types.h"
#include <set>

namespace ii::virtualizer {
Result<void> validate(const InstanceSpec& s) {
    if (s.id.empty() || s.id.size()>32 || s.id.front()=='-' || s.id.back()=='-')
        return core::fail(core::ErrorCode::invalid_argument,"Instance id must contain 1..32 lowercase letters, digits or hyphens");
    for (char c : s.id) if (!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'))
        return core::fail(core::ErrorCode::invalid_argument,"Unsafe instance id");
    if (s.cpus<1 || s.cpus>256 || s.memory_mib<64 || s.memory_mib>1048576 || s.disk_mib<16 || s.disk_mib>16777216)
        return core::fail(core::ErrorCode::invalid_argument,"Resources outside limits: CPUs 1..256, memory 64..1048576 MiB, disk 16..16777216 MiB");
    if (s.kernel.empty() || s.initramfs.empty())
        return core::fail(core::ErrorCode::invalid_argument,"Kernel and initramfs are required");
    if (!s.network && !s.forwards.empty())
        return core::fail(core::ErrorCode::invalid_argument,"Port forwarding requires network=true");
    std::set<unsigned> hosts;
    for (const auto& p : s.forwards) if (!p.host || p.host>65535 || !p.guest || p.guest>65535 || !hosts.insert(p.host).second)
        return core::fail(core::ErrorCode::invalid_argument,"Invalid or duplicate TCP forwarding port");
    return {};
}
InstancePaths paths_for(const fs::path& root, std::string_view id) {
    auto d=root / id;
    return {d,d/"instance.json",d/"data.raw",d/"qmp.sock",d/"agent.sock",d/"console.log",d/"qemu.pid"};
}
std::string_view name(Architecture a) { return a==Architecture::aarch64?"aarch64":"x86_64"; }
std::string_view name(Accelerator a) {
    switch(a) { case Accelerator::automatic:return "auto"; case Accelerator::hvf:return "hvf";
        case Accelerator::kvm:return "kvm"; case Accelerator::tcg:return "tcg"; }
    return "unknown";
}
std::string_view name(State s) {
    switch(s) { case State::stopped:return "stopped"; case State::running:return "running";
        case State::paused:return "paused"; case State::unavailable:return "unavailable"; }
    return "unknown";
}
Architecture host_architecture() {
#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
    return Architecture::aarch64;
#else
    return Architecture::x86_64;
#endif
}
Accelerator native_accelerator() {
#if defined(__APPLE__)
    return Accelerator::hvf;
#elif defined(__linux__)
    return Accelerator::kvm;
#else
    return Accelerator::tcg;
#endif
}
} // namespace ii::virtualizer
