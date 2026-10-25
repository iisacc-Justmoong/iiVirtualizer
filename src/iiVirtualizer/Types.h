#pragma once
#include <iiSDKCore/Core/Result/Result.h>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace ii::virtualizer {
using core::Result;
namespace fs = std::filesystem;
enum class Architecture { aarch64, x86_64 };
enum class Accelerator { automatic, hvf, kvm, tcg };
enum class State { stopped, running, paused, unavailable };
enum class Command { inspect, pause, resume, power_button, terminate };
struct PortForward final { unsigned host = 0, guest = 0; };
struct InstanceSpec final {
    std::string id;
    Architecture architecture = Architecture::aarch64;
    Accelerator accelerator = Accelerator::automatic;
    unsigned cpus = 2, memory_mib = 512;
    std::uint64_t disk_mib = 128;
    fs::path kernel, initramfs, base_disk;
    bool initialize_disk = true;
    bool network = false;
    std::vector<PortForward> forwards;
};
struct InstancePaths final {
    fs::path directory, manifest, disk, qmp, agent, console, pid;
};
struct Instance final {
    InstanceSpec spec;
    std::string uuid;
    InstancePaths paths;
    State state = State::stopped;
};
// A backend is composed from instance-owned callables. No global registry or
// host OS types enter the public contract. An error is never a stopped state.
struct Backend final {
    std::function<Result<void>(const Instance&)> start;
    std::function<Result<State>(const Instance&, Command)> control;
    std::function<Result<std::string>(const Instance&, std::string_view)> guest;
};
[[nodiscard]] Result<void> validate(const InstanceSpec&);
[[nodiscard]] InstancePaths paths_for(const fs::path& root, std::string_view id);
[[nodiscard]] std::string_view name(Architecture);
[[nodiscard]] std::string_view name(Accelerator);
[[nodiscard]] std::string_view name(State);
[[nodiscard]] Architecture host_architecture();
[[nodiscard]] Accelerator native_accelerator();
} // namespace ii::virtualizer
