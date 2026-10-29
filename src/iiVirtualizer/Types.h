#pragma once
#include "Files.h"
#include "Processes.h"
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
enum class Graphics { none, virtio_gpu, vga, ramfb };
enum class BootMode { kernel, disk };
enum class DiskBus { virtio, sata, nvme };
enum class GuestChannel { virtio, serial };
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
    // Empty preserves the iiOS boot recipe. A nonempty value replaces the
    // complete kernel command line, including the guest's console/root/init.
    std::string kernel_command_line;
    Graphics graphics = Graphics::none;
    BootMode boot_mode = BootMode::kernel;
    DiskBus disk_bus = DiskBus::virtio;
    GuestChannel guest_channel = GuestChannel::virtio;
    fs::path boot_iso, payload_iso, firmware, firmware_vars;
    bool allow_reboot = false;
    // The caller owns a TPM emulator listening on the instance's tpm.sock.
    bool external_tpm = false;
    // Creation-only hint. A clone is copy-on-write and falls back to copying.
    bool clone_base_disk = false;
    // Empty selects the existing architecture/accelerator default.
    std::string cpu_model;
    // Prefer an installed disk, with installer media as a fallback.
    bool boot_disk_first = false;
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
    std::function<Result<FileReply>(const Instance&, const FileRequest&, std::chrono::milliseconds)> files = {};
    std::function<Result<ProcessReply>(const Instance&, const ProcessRequest&, std::chrono::milliseconds)> processes = {};
};
[[nodiscard]] Result<void> validate(const InstanceSpec&);
[[nodiscard]] InstancePaths paths_for(const fs::path& root, std::string_view id);
[[nodiscard]] std::string_view name(Architecture);
[[nodiscard]] std::string_view name(Accelerator);
[[nodiscard]] std::string_view name(State);
[[nodiscard]] std::string_view name(Graphics);
[[nodiscard]] std::string_view name(BootMode);
[[nodiscard]] std::string_view name(DiskBus);
[[nodiscard]] std::string_view name(GuestChannel);
// Automatic selection transparently uses CPU emulation for a foreign ISA.
[[nodiscard]] Accelerator select_accelerator(Architecture, Accelerator);
[[nodiscard]] Architecture host_architecture();
[[nodiscard]] Accelerator native_accelerator();
} // namespace ii::virtualizer
