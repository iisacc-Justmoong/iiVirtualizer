#include "Types.h"
#include <set>

namespace ii::virtualizer {
Result<void> validate(const InstanceSpec& s) {
    if(s.clone_base_disk && s.base_disk.empty())
        return core::fail(core::ErrorCode::invalid_argument,"Cloning requires a base disk");
    if (s.id.empty() || s.id.size()>32 || s.id.front()=='-' || s.id.back()=='-')
        return core::fail(core::ErrorCode::invalid_argument,"Instance id must contain 1..32 lowercase letters, digits or hyphens");
    for (char c : s.id) if (!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'))
        return core::fail(core::ErrorCode::invalid_argument,"Unsafe instance id");
    if (s.cpus<1 || s.cpus>256 || s.memory_mib<64 || s.memory_mib>1048576 || s.disk_mib<16 || s.disk_mib>16777216)
        return core::fail(core::ErrorCode::invalid_argument,"Resources outside limits: CPUs 1..256, memory 64..1048576 MiB, disk 16..16777216 MiB");
    if(s.boot_mode!=BootMode::kernel && s.boot_mode!=BootMode::disk)
        return core::fail(core::ErrorCode::invalid_argument,"Invalid boot mode");
    if (s.boot_mode==BootMode::kernel && (s.kernel.empty() || s.initramfs.empty()))
        return core::fail(core::ErrorCode::invalid_argument,"Kernel and initramfs are required");
    if(s.boot_mode==BootMode::disk && (!s.kernel.empty() || !s.initramfs.empty() || !s.kernel_command_line.empty()))
        return core::fail(core::ErrorCode::invalid_argument,"Disk boot does not accept kernel boot arguments");
    if(s.boot_disk_first && (s.boot_mode!=BootMode::disk || s.boot_iso.empty()))
        return core::fail(core::ErrorCode::invalid_argument,"Disk-first boot requires disk mode and installer media");
    if(s.disk_bus!=DiskBus::virtio && s.disk_bus!=DiskBus::sata && s.disk_bus!=DiskBus::nvme)
        return core::fail(core::ErrorCode::invalid_argument,"Invalid disk bus");
    if(s.guest_channel!=GuestChannel::virtio && s.guest_channel!=GuestChannel::serial)
        return core::fail(core::ErrorCode::invalid_argument,"Invalid guest channel");
    if(s.architecture==Architecture::aarch64 && (s.disk_bus==DiskBus::sata || s.graphics==Graphics::vga))
        return core::fail(core::ErrorCode::unsupported,"SATA and standard VGA require the x86 q35 board");
    if(s.architecture==Architecture::aarch64 && s.boot_mode==BootMode::disk && s.firmware.empty())
        return core::fail(core::ErrorCode::invalid_argument,"ARM disk boot requires compatible firmware");
    if(!s.firmware_vars.empty() && s.firmware.empty())
        return core::fail(core::ErrorCode::invalid_argument,"Firmware variables require firmware code");
    if(s.boot_mode==BootMode::kernel && (s.allow_reboot || !s.boot_iso.empty() || !s.payload_iso.empty() || !s.firmware.empty() || !s.firmware_vars.empty()))
        return core::fail(core::ErrorCode::invalid_argument,"Installer media and firmware require disk boot");
    if (s.kernel_command_line.size()>4096)
        return core::fail(core::ErrorCode::invalid_argument,"Kernel command line exceeds 4096 bytes");
    for (unsigned char c : s.kernel_command_line) if(c<32 || c>126)
        return core::fail(core::ErrorCode::invalid_argument,"Kernel command line must contain printable ASCII");
    auto cpu_char=[](unsigned char c) {return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9');};
    if(s.cpu_model.size()>256 || (!s.cpu_model.empty()&&!cpu_char(s.cpu_model.front())))
        return core::fail(core::ErrorCode::invalid_argument,"Invalid CPU model");
    for(unsigned char c:s.cpu_model) if(!cpu_char(c)&&c!='-'&&c!='_'&&c!='.'&&c!=','&&c!='='&&c!='+')
        return core::fail(core::ErrorCode::invalid_argument,"Invalid CPU model");
    if (s.graphics!=Graphics::none && s.graphics!=Graphics::virtio_gpu && s.graphics!=Graphics::vga && s.graphics!=Graphics::ramfb)
        return core::fail(core::ErrorCode::invalid_argument,"Invalid graphics device");
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
std::string_view name(Graphics g) { return g==Graphics::virtio_gpu?"virtio-gpu":g==Graphics::vga?"vga":g==Graphics::ramfb?"ramfb":"none"; }
std::string_view name(BootMode b) { return b==BootMode::disk?"disk":"kernel"; }
std::string_view name(DiskBus b) { return b==DiskBus::sata?"sata":b==DiskBus::nvme?"nvme":"virtio"; }
std::string_view name(GuestChannel c) { return c==GuestChannel::serial?"serial":"virtio"; }
Accelerator select_accelerator(Architecture architecture,Accelerator requested) {
    if(requested!=Accelerator::automatic)return requested;
    return architecture==host_architecture()?native_accelerator():Accelerator::tcg;
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
