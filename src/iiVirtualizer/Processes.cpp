#include "Processes.h"
#include "Files.h"
#include <iiSDKCore/Core/Text/Utf8.h>
#include <algorithm>

namespace ii::virtualizer {
std::string_view name(ProcessAction action) {
    switch(action){case ProcessAction::launch:return "launch";case ProcessAction::list:return "list";case ProcessAction::stat:return "stat";
        case ProcessAction::read:return "read";case ProcessAction::write:return "write";case ProcessAction::close_input:return "close-input";
        case ProcessAction::stop:return "stop";case ProcessAction::release:return "release";case ProcessAction::monitor:return "monitor";}return "unknown";
}
std::string_view name(ProcessState state) {
    switch(state){case ProcessState::running:return "running";case ProcessState::exited:return "exited";case ProcessState::stopped:return "stopped";case ProcessState::timed_out:return "timed-out";}return "unknown";
}
core::Result<ProcessAction> process_action(std::string_view value) {
    for(auto action:{ProcessAction::launch,ProcessAction::list,ProcessAction::stat,ProcessAction::read,ProcessAction::write,ProcessAction::close_input,ProcessAction::stop,ProcessAction::release,ProcessAction::monitor})if(name(action)==value)return action;
    return core::fail(core::ErrorCode::invalid_argument,"Unknown process operation");
}
bool process_key(std::string_view key) {
    return !key.empty()&&key.size()<=64&&std::all_of(key.begin(),key.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_';});
}
core::Result<void> validate(const ProcessLaunch& launch) {
    auto fail=[](){return core::fail(core::ErrorCode::invalid_argument,"Invalid bounded process launch contract");};
    if(!process_key(launch.key)||launch.arguments.size()>64||launch.environment.size()>32||launch.memory_mib<16||launch.memory_mib>65536||!launch.pids||launch.pids>256||launch.lifetime_ms>86400000)return fail();
    if(launch.executable.starts_with('@')) {if(!process_key(std::string_view(launch.executable).substr(1)))return fail();}
    else if(!validate_file_path(launch.executable))return fail();
    if(!validate_file_path(launch.working_directory,true))return fail();
    std::size_t bytes=0;
    auto text=[&](std::string_view value){bytes+=value.size();return value.size()<=4096&&value.find('\0')==value.npos&&core::valid_utf8(core::byte_view(value));};
    for(const auto& arg:launch.arguments)if(!text(arg))return fail();
    for(const auto& [key,value]:launch.environment) {
        if(key.empty()||key.size()>64||!std::all_of(key.begin(),key.end(),[](unsigned char c){return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||c=='_'||(c>='0'&&c<='9');})||
            (key.front()>='0'&&key.front()<='9')||key=="HOME"||key=="TMPDIR"||key=="PATH"||key.starts_with("IIV_")||key.starts_with("LD_")||key.starts_with("DYLD_")||!text(value))return fail();
        bytes+=key.size();
    }
    return bytes<=8192?core::Result<void>{}:fail();
}
core::Result<void> validate(const ProcessRequest& request) {
    auto fail=[](){return core::fail(core::ErrorCode::invalid_argument,"Invalid process operation fields or bounded limits");};
    if(name(request.action)=="unknown"||request.data.size()>process_chunk_limit||!request.length||request.length>process_chunk_limit||!request.limit||request.limit>process_page_limit)return fail();
    if(request.action==ProcessAction::launch) {
        if(!request.launch||!request.id.empty())return fail();
        if(auto valid=validate(*request.launch);!valid)return valid;
    }else if(request.launch)return fail();
    if(request.action==ProcessAction::list||request.action==ProcessAction::monitor) {if(!request.id.empty())return fail();}
    else if(request.action!=ProcessAction::launch&&!process_key(request.id))return fail();
    if(!request.cursor.empty()&&(request.action!=ProcessAction::list||!process_key(request.cursor)))return fail();
    if(request.action!=ProcessAction::read&&request.action!=ProcessAction::write&&request.offset)return fail();
    if(request.action!=ProcessAction::write&&!request.data.empty())return fail();
    if(request.action==ProcessAction::write&&request.data.empty())return fail();
    if(request.stream!=ProcessStream::output&&request.stream!=ProcessStream::error)return fail();
    if(request.action!=ProcessAction::read&&request.stream!=ProcessStream::output)return fail();
    return {};
}
} // namespace ii::virtualizer
