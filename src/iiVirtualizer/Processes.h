#pragma once
#include <iiSDKCore/Core/Bytes/Bytes.h>
#include <iiSDKCore/Core/Result/Result.h>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ii::virtualizer {
inline constexpr std::size_t process_chunk_limit=16384,process_page_limit=16;
inline constexpr std::size_t process_stream_capacity=65536,process_input_capacity=65536;
inline constexpr std::size_t process_running_limit=16,process_record_limit=32;
enum class ProcessAction { launch,list,stat,read,write,close_input,stop,release,monitor };
enum class ProcessState { running,exited,stopped,timed_out };
enum class ProcessStream { output,error };
struct ProcessLaunch final {
    // @service or @<program> selects a trusted boot-image program. Otherwise
    // the executable is a Files-relative static ELF, loaded after confinement.
    std::string key,executable,working_directory;
    std::vector<std::string> arguments;
    std::map<std::string,std::string,std::less<>> environment;
    unsigned memory_mib=64,pids=64;
    std::uint64_t lifetime_ms=0;
    bool operator==(const ProcessLaunch&) const = default;
};
struct ProcessRequest final {
    ProcessAction action=ProcessAction::list;
    std::optional<ProcessLaunch> launch;
    std::string id,cursor;
    ProcessStream stream=ProcessStream::output;
    std::uint64_t offset=0;
    std::size_t length=process_chunk_limit,limit=process_page_limit;
    core::Bytes data;
};
struct ProcessExit final { std::uint32_t code=0;int signal=0; };
struct ProcessSnapshot final {
    std::string id,key,executable;
    ProcessState state=ProcessState::running;
    std::uint64_t pid=0,started_ms=0,elapsed_ms=0;
    std::uint64_t input_accepted=0,input_written=0,input_discarded=0;
    std::uint64_t output_begin=0,output_end=0,error_begin=0,error_end=0;
    std::uint64_t cpu_usec=0,memory_bytes=0,peak_memory_bytes=0,tasks=0;
    bool input_closing=false,input_closed=false,output_eof=false,error_eof=false;
    std::optional<ProcessExit> exit;
};
struct VmMonitor final {
    std::string boot_id,filesystem_profile;
    std::uint64_t uptime_ms=0,cpus=0,memory_total=0,memory_available=0;
    std::uint64_t cpu_total_ticks=0,cpu_idle_ticks=0,clock_ticks=0;
    std::uint64_t sequence=0,running=0,retained=0,workers=0,worker_pending=0;
    bool processes_available=false,files_available=false;
};
struct ProcessReply final {
    std::optional<ProcessSnapshot> process;
    std::vector<ProcessSnapshot> processes;
    std::optional<VmMonitor> monitor;
    core::Bytes data;
    std::string next_cursor;
    std::uint64_t offset=0,next_offset=0;
    bool truncated=false,eof=false,released=false;
};
[[nodiscard]] std::string_view name(ProcessAction);
[[nodiscard]] std::string_view name(ProcessState);
[[nodiscard]] core::Result<ProcessAction> process_action(std::string_view);
[[nodiscard]] bool process_key(std::string_view);
[[nodiscard]] core::Result<void> validate(const ProcessLaunch&);
[[nodiscard]] core::Result<void> validate(const ProcessRequest&);
} // namespace ii::virtualizer
