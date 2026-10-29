#include "ProcessWire.h"
#include "BinaryWire.h"
#include <algorithm>
#include <stdexcept>

namespace ii::virtualizer::process_detail {
using Json=ii::json::Value;
namespace {
const Json& field(const Json& value,std::string_view key){auto found=value.find(key);if(!found)throw std::invalid_argument("Missing process field");return found->get();}
std::string text(const Json& value,std::string_view key){auto found=field(value,key).get<std::string>();if(!found)throw std::invalid_argument("Invalid process text");return found->get();}
std::uint64_t number(const Json& value,std::string_view key){const auto& item=field(value,key);if(auto n=item.get<std::uint64_t>())return n->get();if(auto n=item.get<std::int64_t>();n&&n->get()>=0)return static_cast<std::uint64_t>(n->get());throw std::invalid_argument("Invalid process integer");}
bool flag(const Json& value,std::string_view key){auto found=field(value,key).get<bool>();if(!found)throw std::invalid_argument("Invalid process flag");return found->get();}
void fields(const Json& value,std::initializer_list<std::string_view> names){auto object=value.get<Json::Object>();if(!object)throw std::invalid_argument("Expected process object");for(const auto& [key,item]:object->get()){(void)item;if(std::find(names.begin(),names.end(),key)==names.end())throw std::invalid_argument("Unexpected process field");}}
Json snapshot(const ProcessSnapshot& p) {
    Json::Object value{{"id",Json(p.id)},{"key",Json(p.key)},{"executable",Json(p.executable)},{"state",Json(name(p.state))},
        {"pid",Json(p.pid)},{"started_ms",Json(p.started_ms)},{"elapsed_ms",Json(p.elapsed_ms)},
        {"input_accepted",Json(p.input_accepted)},{"input_written",Json(p.input_written)},{"input_discarded",Json(p.input_discarded)},
        {"output_begin",Json(p.output_begin)},{"output_end",Json(p.output_end)},{"error_begin",Json(p.error_begin)},{"error_end",Json(p.error_end)},
        {"cpu_usec",Json(p.cpu_usec)},{"memory_bytes",Json(p.memory_bytes)},{"peak_memory_bytes",Json(p.peak_memory_bytes)},{"tasks",Json(p.tasks)},
        {"input_closing",Json(p.input_closing)},{"input_closed",Json(p.input_closed)},{"output_eof",Json(p.output_eof)},{"error_eof",Json(p.error_eof)}};
    if(p.exit)value.emplace("exit",Json(Json::Object{{"code",Json(p.exit->code)},{"signal",Json(p.exit->signal)}}));
    return Json(std::move(value));
}
ProcessSnapshot parse_snapshot(const Json& value) {
    fields(value,{"id","key","executable","state","pid","started_ms","elapsed_ms","input_accepted","input_written","input_discarded","output_begin","output_end","error_begin","error_end","cpu_usec","memory_bytes","peak_memory_bytes","tasks","input_closing","input_closed","output_eof","error_eof","exit"});
    ProcessSnapshot p;p.id=text(value,"id");p.key=text(value,"key");p.executable=text(value,"executable");
    const auto state=text(value,"state");bool known=false;for(auto s:{ProcessState::running,ProcessState::exited,ProcessState::stopped,ProcessState::timed_out})if(state==name(s)){p.state=s;known=true;}
    if(!known||!process_key(p.id)||!process_key(p.key))throw std::invalid_argument("Process identity or state");
    ProcessLaunch launch;launch.key=p.key;launch.executable=p.executable;
    if(!validate(launch))throw std::invalid_argument("Process executable");
    p.pid=number(value,"pid");p.started_ms=number(value,"started_ms");p.elapsed_ms=number(value,"elapsed_ms");
    p.input_accepted=number(value,"input_accepted");p.input_written=number(value,"input_written");p.input_discarded=number(value,"input_discarded");
    p.output_begin=number(value,"output_begin");p.output_end=number(value,"output_end");p.error_begin=number(value,"error_begin");p.error_end=number(value,"error_end");
    p.cpu_usec=number(value,"cpu_usec");p.memory_bytes=number(value,"memory_bytes");p.peak_memory_bytes=number(value,"peak_memory_bytes");p.tasks=number(value,"tasks");
    p.input_closing=flag(value,"input_closing");p.input_closed=flag(value,"input_closed");p.output_eof=flag(value,"output_eof");p.error_eof=flag(value,"error_eof");
    if(value.find("exit")){const auto& e=field(value,"exit");fields(e,{"code","signal"});const auto code=number(e,"code"),signal=number(e,"signal");if(code>UINT32_MAX||signal>127)throw std::invalid_argument("Process exit");p.exit=ProcessExit{static_cast<std::uint32_t>(code),static_cast<int>(signal)};}
    if((p.state==ProcessState::running)==p.exit.has_value()||p.output_begin>p.output_end||p.error_begin>p.error_end||p.output_end-p.output_begin>process_stream_capacity||p.error_end-p.error_begin>process_stream_capacity||p.input_written>p.input_accepted||p.input_discarded>p.input_accepted-p.input_written)throw std::invalid_argument("Process counters");
    return p;
}
Json monitor_json(const VmMonitor& m) {return Json(Json::Object{{"boot_id",Json(m.boot_id)},{"filesystem_profile",Json(m.filesystem_profile)},{"uptime_ms",Json(m.uptime_ms)},
    {"cpus",Json(m.cpus)},{"memory_total",Json(m.memory_total)},{"memory_available",Json(m.memory_available)},
    {"cpu_total_ticks",Json(m.cpu_total_ticks)},{"cpu_idle_ticks",Json(m.cpu_idle_ticks)},{"clock_ticks",Json(m.clock_ticks)},
    {"sequence",Json(m.sequence)},{"running",Json(m.running)},{"retained",Json(m.retained)},{"workers",Json(m.workers)},{"worker_pending",Json(m.worker_pending)},
    {"processes_available",Json(m.processes_available)},{"files_available",Json(m.files_available)}});}
VmMonitor parse_monitor(const Json& value) {
    fields(value,{"boot_id","filesystem_profile","uptime_ms","cpus","memory_total","memory_available","cpu_total_ticks","cpu_idle_ticks","clock_ticks","sequence","running","retained","workers","worker_pending","processes_available","files_available"});
    VmMonitor m;m.boot_id=text(value,"boot_id");m.filesystem_profile=text(value,"filesystem_profile");m.uptime_ms=number(value,"uptime_ms");m.cpus=number(value,"cpus");
    m.memory_total=number(value,"memory_total");m.memory_available=number(value,"memory_available");m.cpu_total_ticks=number(value,"cpu_total_ticks");m.cpu_idle_ticks=number(value,"cpu_idle_ticks");m.clock_ticks=number(value,"clock_ticks");m.sequence=number(value,"sequence");
    m.running=number(value,"running");m.retained=number(value,"retained");m.workers=number(value,"workers");m.worker_pending=number(value,"worker_pending");m.processes_available=flag(value,"processes_available");m.files_available=flag(value,"files_available");
    if(!process_key(m.boot_id)||m.filesystem_profile!="society-v1"||!m.cpus||m.memory_available>m.memory_total||m.cpu_idle_ticks>m.cpu_total_ticks||m.running>process_running_limit||m.retained>process_record_limit||m.running>m.retained)throw std::invalid_argument("VM monitor bounds");
    return m;
}
bool uuid_valid(std::string_view value){if(value.size()!=36)return false;for(std::size_t i=0;i<value.size();++i)if((i==8||i==13||i==18||i==23)?value[i]!='-':!((value[i]>='0'&&value[i]<='9')||(value[i]>='a'&&value[i]<='f')))return false;return true;}
Json frame(std::string_view input,std::string_view uuid,std::string_view id) {
    const auto prefix="IIVP1 "+std::string(uuid)+" "+std::string(id)+" ";
    if(input.size()>frame_limit||!uuid_valid(uuid)||!process_key(id)||!input.starts_with(prefix))throw std::invalid_argument("Process frame binding");
    auto value=ii::json::parse(input.substr(prefix.size()),{.input_bytes=frame_limit,.output_bytes=frame_limit,.string_bytes=32768,.items=4096,.depth=12});if(!value)throw std::invalid_argument("Process frame JSON");return *value;
}
std::string encode(std::string_view uuid,std::string_view id,Json value) {
    if(!uuid_valid(uuid)||!process_key(id))throw std::invalid_argument("Process frame identity");
    auto body=ii::json::stringify(value,{.input_bytes=frame_limit,.output_bytes=frame_limit,.string_bytes=32768,.items=4096,.depth=12});if(!body)throw std::invalid_argument("Process frame encoding");
    auto output="IIVP1 "+std::string(uuid)+" "+std::string(id)+" "+*body+"\n";if(output.size()>frame_limit)throw std::invalid_argument("Process frame limit");return output;
}
}
Json request_payload(const ProcessRequest& request) {
    Json::Object value;
    if(request.action==ProcessAction::launch) {
        const auto& p=*request.launch;Json::Array args;for(const auto& arg:p.arguments)args.emplace_back(arg);Json::Object env;for(const auto& [key,v]:p.environment)env.emplace(key,Json(v));
        value={{"key",Json(p.key)},{"executable",Json(p.executable)},{"working_directory",Json(p.working_directory)},
            {"arguments",Json(std::move(args))},{"environment",Json(std::move(env))},{"memory_mib",Json(p.memory_mib)},{"pids",Json(p.pids)},{"lifetime_ms",Json(p.lifetime_ms)}};
    }else if(request.action==ProcessAction::list)value={{"cursor",Json(request.cursor)},{"limit",Json(request.limit)}};
    else if(request.action!=ProcessAction::monitor)value.emplace("id",Json(request.id));
    if(request.action==ProcessAction::read){value.emplace("stream",Json(request.stream==ProcessStream::output?"stdout":"stderr"));value.emplace("offset",Json(request.offset));value.emplace("length",Json(request.length));}
    if(request.action==ProcessAction::write){value.emplace("offset",Json(request.offset));value.emplace("data_base64",Json(wire_detail::encode_binary(request.data)));}
    return Json(std::move(value));
}
core::Result<ProcessRequest> parse_request(const Json& value,ProcessAction action) {
    try {
        ProcessRequest request;request.action=action;
        if(action==ProcessAction::launch) {
            fields(value,{"key","executable","working_directory","arguments","environment","memory_mib","pids","lifetime_ms"});ProcessLaunch p;p.key=text(value,"key");p.executable=text(value,"executable");
            if(value.find("working_directory"))p.working_directory=text(value,"working_directory");
            if(value.find("arguments")){auto args=field(value,"arguments").get<Json::Array>();if(!args||args->get().size()>64)throw std::invalid_argument("argv bounds");for(const auto& arg:args->get()){auto t=arg.get<std::string>();if(!t)throw std::invalid_argument("argv type");p.arguments.push_back(t->get());}}
            if(value.find("environment")){auto env=field(value,"environment").get<Json::Object>();if(!env||env->get().size()>32)throw std::invalid_argument("env bounds");for(const auto& [key,item]:env->get()){auto t=item.get<std::string>();if(!t)throw std::invalid_argument("env type");p.environment.emplace(key,t->get());}}
            if(value.find("memory_mib")){const auto n=number(value,"memory_mib");if(n>65536)throw std::invalid_argument("memory bound");p.memory_mib=static_cast<unsigned>(n);}
            if(value.find("pids")){const auto n=number(value,"pids");if(n>256)throw std::invalid_argument("pids bound");p.pids=static_cast<unsigned>(n);}
            if(value.find("lifetime_ms"))p.lifetime_ms=number(value,"lifetime_ms");
            request.launch=std::move(p);
        }else if(action==ProcessAction::list){fields(value,{"cursor","limit"});if(value.find("cursor"))request.cursor=text(value,"cursor");if(value.find("limit")){auto n=number(value,"limit");if(n>process_page_limit)throw std::invalid_argument("page bound");request.limit=static_cast<std::size_t>(n);}}
        else if(action==ProcessAction::monitor)fields(value,{});
        else {
            if(action==ProcessAction::read)fields(value,{"id","stream","offset","length"});else if(action==ProcessAction::write)fields(value,{"id","offset","data_base64"});else fields(value,{"id"});
            request.id=text(value,"id");if(value.find("offset"))request.offset=number(value,"offset");
            if(value.find("length")){const auto n=number(value,"length");if(n>process_chunk_limit)throw std::invalid_argument("read bound");request.length=static_cast<std::size_t>(n);}
            if(value.find("stream")){const auto stream=text(value,"stream");if(stream=="stderr")request.stream=ProcessStream::error;else if(stream!="stdout")throw std::invalid_argument("process stream");}
            if(value.find("data_base64"))request.data=wire_detail::decode_binary(text(value,"data_base64"),process_chunk_limit);
        }
        if(auto valid=validate(request);!valid)return std::unexpected(valid.error());
        return request;
    }catch(...){return core::fail(core::ErrorCode::invalid_argument,"Malformed process request");}
}
Json reply_payload(const ProcessReply& reply) {
    Json::Array records;for(const auto& p:reply.processes)records.push_back(snapshot(p));
    Json::Object value{{"processes",Json(std::move(records))},{"data_base64",Json(wire_detail::encode_binary(reply.data))},
        {"next_cursor",Json(reply.next_cursor)},{"offset",Json(reply.offset)},{"next_offset",Json(reply.next_offset)},
        {"truncated",Json(reply.truncated)},{"eof",Json(reply.eof)},{"released",Json(reply.released)}};
    if(reply.process)value.emplace("process",snapshot(*reply.process));
    if(reply.monitor)value.emplace("monitor",monitor_json(*reply.monitor));
    return Json(std::move(value));
}
core::Result<ProcessReply> parse_reply(const Json& value,const ProcessRequest& request) {
    try {
        fields(value,{"processes","data_base64","next_cursor","offset","next_offset","truncated","eof","released","process","monitor"});ProcessReply reply;
        if(value.find("process"))reply.process=parse_snapshot(field(value,"process"));
        if(value.find("monitor"))reply.monitor=parse_monitor(field(value,"monitor"));
        reply.data=wire_detail::decode_binary(text(value,"data_base64"),process_chunk_limit);reply.next_cursor=text(value,"next_cursor");reply.offset=number(value,"offset");reply.next_offset=number(value,"next_offset");reply.truncated=flag(value,"truncated");reply.eof=flag(value,"eof");reply.released=flag(value,"released");
        auto records=field(value,"processes").get<Json::Array>();if(!records||records->get().size()>request.limit)throw std::invalid_argument("Process page bound");
        std::string previous=request.cursor;for(const auto& item:records->get()){auto p=parse_snapshot(item);if(p.id<=previous)throw std::invalid_argument("Process page order");previous=p.id;reply.processes.push_back(std::move(p));}
        if(request.action==ProcessAction::monitor) {if(!reply.monitor||reply.process||!reply.processes.empty())throw std::invalid_argument("Monitor result shape");}
        else if(request.action==ProcessAction::list) {if(reply.process||reply.monitor||(!reply.next_cursor.empty()&&(reply.processes.empty()||reply.next_cursor!=reply.processes.back().id)))throw std::invalid_argument("Process list shape");}
        else if(request.action==ProcessAction::release) {if(!reply.released||reply.process||reply.monitor||!reply.processes.empty())throw std::invalid_argument("Process release shape");}
        else {
            if(!reply.process||reply.monitor||!reply.processes.empty()||!reply.next_cursor.empty()||reply.released)throw std::invalid_argument("Process result shape");
            if(request.action==ProcessAction::launch) {if(reply.process->key!=request.launch->key||reply.process->executable!=request.launch->executable)throw std::invalid_argument("Process launch binding");}
            else if(reply.process->id!=request.id)throw std::invalid_argument("Process result binding");
        }
        if(request.action==ProcessAction::read) {
            const auto& p=*reply.process;const auto begin=request.stream==ProcessStream::output?p.output_begin:p.error_begin,end=request.stream==ProcessStream::output?p.output_end:p.error_end;
            if(reply.offset!=std::max(begin,request.offset)||reply.data.size()>request.length||reply.offset>end||reply.data.size()>end-reply.offset||reply.next_offset!=reply.offset+reply.data.size()||reply.truncated!=(reply.offset>request.offset))throw std::invalid_argument("Process stream offsets");
        }else if(!reply.data.empty()||reply.offset||reply.next_offset||reply.truncated||reply.eof)throw std::invalid_argument("Unexpected process output");
        if(request.action!=ProcessAction::list&&!reply.next_cursor.empty())throw std::invalid_argument("Unexpected process cursor");
        if(request.action!=ProcessAction::release&&reply.released)throw std::invalid_argument("Unexpected process release");
        return reply;
    }catch(...){return core::fail(core::ErrorCode::failed,"Malformed or mismatched process result");}
}
std::string encode_request(std::string_view uuid,std::string_view id,const ProcessRequest& request) {if(!validate(request))throw std::invalid_argument("Process request contract");return encode(uuid,id,Json(Json::Object{{"operation",Json(name(request.action))},{"request",request_payload(request)}}));}
core::Result<Envelope> decode_request(std::string_view input,std::string_view uuid) {
    try {const std::string prefix="IIVP1 "+std::string(uuid)+" ";if(!input.starts_with(prefix))throw std::invalid_argument("Process UUID");const auto end=input.find(' ',prefix.size());if(end==input.npos)throw std::invalid_argument("Process ID");const auto id=input.substr(prefix.size(),end-prefix.size());const auto value=frame(input,uuid,id);fields(value,{"operation","request"});auto action=process_action(text(value,"operation"));if(!action)return std::unexpected(action.error());auto request=parse_request(field(value,"request"),*action);if(!request)return std::unexpected(request.error());return Envelope{std::string(uuid),std::string(id),std::move(*request)};
    }catch(...){return core::fail(core::ErrorCode::invalid_argument,"Malformed or mismatched process frame");}
}
std::string encode_reply(std::string_view uuid,std::string_view id,ProcessAction action,const core::Result<ProcessReply>& result) {
    Json::Object value{{"operation",Json(name(action))},{"ok",Json(bool(result))}};if(result)value.emplace("result",reply_payload(*result));else value.emplace("error",Json(Json::Object{{"code",Json(static_cast<int>(result.error().code))},{"message",Json(result.error().message)}}));return encode(uuid,id,Json(std::move(value)));
}
core::Result<ProcessReply> decode_reply(std::string_view input,std::string_view uuid,std::string_view id,const ProcessRequest& request) {
    try {const auto value=frame(input,uuid,id);fields(value,{"operation","ok","result","error"});if(text(value,"operation")!=name(request.action))throw std::invalid_argument("Process operation");if(!flag(value,"ok")){const auto& e=field(value,"error");fields(e,{"code","message"});const auto code=number(e,"code");if(code>static_cast<unsigned>(core::ErrorCode::wrong_executor))throw std::invalid_argument("Process error code");return core::fail(static_cast<core::ErrorCode>(code),text(e,"message"));}return parse_reply(field(value,"result"),request);
    }catch(...){return core::fail(core::ErrorCode::failed,"Malformed or mismatched process response");}
}
}
