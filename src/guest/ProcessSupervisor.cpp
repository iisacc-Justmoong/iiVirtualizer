#include "ProcessSupervisor.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <map>
#include <random>
#include <sstream>

namespace ii::virtualizer::guest {
namespace {
using Clock=std::chrono::steady_clock;
std::string nonce(){std::random_device random;std::ostringstream out;for(unsigned i=0;i<16;++i)out<<std::hex<<std::setfill('0')<<std::setw(2)<<(random()&255);return out.str();}
struct Ring final {
    std::array<std::byte,process_stream_capacity> bytes{};std::uint64_t end=0;bool eof=false;
    std::uint64_t begin()const{return end>bytes.size()?end-bytes.size():0;}
    void append(core::ByteView data){for(auto byte:data){bytes[end%bytes.size()]=byte;++end;}}
    void read(ProcessReply& reply,std::uint64_t offset,std::size_t length)const {
        reply.offset=std::max(begin(),offset);reply.truncated=offset<begin();
        const auto count=std::min<std::uint64_t>(length,end-reply.offset);reply.data.reserve(static_cast<std::size_t>(count));
        for(std::uint64_t i=0;i<count;++i)reply.data.push_back(bytes[(reply.offset+i)%bytes.size()]);
        reply.next_offset=reply.offset+count;reply.eof=eof&&reply.next_offset==end;
    }
};
struct Record final {
    ProcessLaunch launch;std::optional<ii::process::Process> owner;ProcessSnapshot value;
    Ring output,error;core::Bytes pending;std::size_t pending_offset=0;
    Clock::time_point started=Clock::now();bool tree_retired=false;
};
}
struct ProcessSupervisor::Impl final {
    Launcher launch;Hooks hooks;std::string boot=nonce();Clock::time_point started=Clock::now();
    std::map<std::string,std::unique_ptr<Record>,std::less<>> records;std::uint64_t serial=0;std::size_t round=0;bool stopping=false;
    Impl(Launcher value,Hooks callbacks):launch(std::move(value)),hooks(std::move(callbacks)){if(!launch)throw std::invalid_argument("A process launcher is required");}
    void sample(Record& record) {
        auto& p=record.value;if(!p.exit)p.elapsed_ms=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-record.started).count());
        p.output_begin=record.output.begin();p.output_end=record.output.end;p.error_begin=record.error.begin();p.error_end=record.error.end;
        p.output_eof=record.output.eof;p.error_eof=record.error.eof;
        if(hooks.sample&&!record.tree_retired)hooks.sample(p.id,p);
    }
    core::Result<void> terminate(Record& record,ProcessState state) {
        sample(record);auto& p=record.value;if(p.exit)return {};
        auto killed=hooks.kill?hooks.kill(p.id):core::Result<void>{};
        auto result=record.owner->cancel();if(!result)return std::unexpected(result.error());
        p.state=state;p.exit=ProcessExit{result->code,result->signal};p.input_closing=true;p.input_closed=true;
        (void)record.owner->input().close();p.input_discarded=p.input_accepted-p.input_written;record.pending.clear();record.pending_offset=0;
        if(hooks.kill){auto again=hooks.kill(p.id);if(!again)return again;}return killed;
    }
    void drain(ii::io::Channel channel,Ring& ring,std::size_t& budget) {
        if(ring.eof||!budget)return;std::array<std::byte,4096> bytes{};auto result=channel.read(std::span(bytes).first(std::min(bytes.size(),budget)));
        if(result){ring.append(std::span(bytes).first(result->bytes));budget-=result->bytes;ring.eof=result->eof;}
        else if(result.error().code!=ii::io::ErrorCode::would_block&&result.error().code!=ii::io::ErrorCode::busy)ring.eof=true;
    }
};
ProcessSupervisor::ProcessSupervisor(Launcher launch,Hooks hooks):impl_(std::make_unique<Impl>(std::move(launch),std::move(hooks))){}
ProcessSupervisor::~ProcessSupervisor(){shutdown();}
void ProcessSupervisor::tick() {
    auto& p=*impl_;if(p.records.empty())return;
    std::size_t budget=65536;auto start=p.round++%p.records.size();auto it=p.records.begin();std::advance(it,static_cast<std::ptrdiff_t>(start));
    for(std::size_t count=0;count<p.records.size();++count) {
        auto& record=*it->second;auto& value=record.value;
        if(record.owner) {
            p.drain(record.owner->output(),record.output,budget);p.drain(record.owner->error(),record.error,budget);
            if(!value.exit) {
                if(record.pending_offset<record.pending.size()) {
                    auto result=record.owner->input().write(core::ByteView(record.pending).subspan(record.pending_offset));
                    if(result){record.pending_offset+=*result;value.input_written+=*result;}
                    else if(result.error().code!=ii::io::ErrorCode::would_block&&result.error().code!=ii::io::ErrorCode::busy){value.input_closing=true;value.input_closed=true;value.input_discarded=value.input_accepted-value.input_written;(void)record.owner->input().close();record.pending.clear();record.pending_offset=0;}
                    if(record.pending_offset==record.pending.size()){record.pending.clear();record.pending_offset=0;}
                }
                if(value.input_closing&&record.pending.empty()&&!value.input_closed){(void)record.owner->input().close();value.input_closed=true;}
                if(record.launch.lifetime_ms&&Clock::now()-record.started>=std::chrono::milliseconds(record.launch.lifetime_ms))(void)p.terminate(record,ProcessState::timed_out);
                else {
                    auto exit=record.owner->poll();
                    if(exit&&*exit){value.elapsed_ms=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-record.started).count());value.state=ProcessState::exited;value.exit=ProcessExit{(**exit).code,(**exit).signal};value.input_closing=true;value.input_closed=true;value.input_discarded=value.input_accepted-value.input_written;(void)record.owner->input().close();record.pending.clear();record.pending_offset=0;if(p.hooks.kill)(void)p.hooks.kill(value.id);}
                }
            }
            if(value.exit) {
                p.sample(record);if(!record.tree_retired)record.tree_retired=p.hooks.retire?p.hooks.retire(value.id):true;
                if(record.output.eof&&record.error.eof&&record.tree_retired)record.owner.reset();
            }
        }
        value.output_begin=record.output.begin();value.output_end=record.output.end;value.error_begin=record.error.begin();value.error_end=record.error.end;value.output_eof=record.output.eof;value.error_eof=record.error.eof;
        if(++it==p.records.end())it=p.records.begin();
    }
}
core::Result<ProcessReply> ProcessSupervisor::execute(const ProcessRequest& request) {
    if(auto valid=validate(request);!valid)return std::unexpected(valid.error());
    try {
        tick();auto& p=*impl_;ProcessReply reply;
        if(request.action==ProcessAction::monitor)return core::fail(core::ErrorCode::unsupported,"VM metrics are supplied by the guest supervisor adapter");
        if(request.action==ProcessAction::launch) {
            if(p.stopping)return core::fail(core::ErrorCode::closed,"The process supervisor is stopping");
            for(auto& [id,record]:p.records)if(record->launch.key==request.launch->key){(void)id;if(record->launch!=*request.launch)return core::fail(core::ErrorCode::already_exists,"Launch key is bound to different process options");p.sample(*record);reply.process=record->value;return reply;}
            if(p.records.size()>=process_record_limit||running()>=process_running_limit)return core::fail(core::ErrorCode::busy,"Release completed records or stop running processes before launching more");
            auto command=p.launch(*request.launch);if(!command)return std::unexpected(command.error());
            std::ostringstream suffix;suffix<<std::hex<<std::setfill('0')<<std::setw(16)<<++p.serial;const auto id=p.boot+"-"+suffix.str();
            // Allocate the record, metadata and registry node before spawning.
            // After spawn, even a failed reply allocation must retain ownership
            // and the launch key so the caller can recover the same handle.
            auto record=std::make_unique<Record>();record->launch=*request.launch;
            record->value.id=id;record->value.key=record->launch.key;record->value.executable=record->launch.executable;
            const auto position=p.records.emplace(id,std::move(record)).first;
            struct Admission final {
                Impl& impl;decltype(p.records)::iterator position;bool prepared=false,committed=false;
                ~Admission(){if(committed)return;auto& record=*position->second;try{if(prepared&&impl.hooks.kill)(void)impl.hooks.kill(record.value.id);record.owner.reset();if(prepared&&impl.hooks.kill)(void)impl.hooks.kill(record.value.id);if(prepared&&impl.hooks.retire)(void)impl.hooks.retire(record.value.id);}catch(...){}impl.records.erase(position);}
            } admission{p,position};
            if(p.hooks.prepare){admission.prepared=true;auto prepared=p.hooks.prepare(id,*request.launch);if(!prepared)return std::unexpected(prepared.error());}
            command->environment["IIV_PROCESS_ID"]=id;command->input=ii::process::Input::pipe;command->output=ii::process::Output::pipe;command->error=ii::process::ErrorOutput::pipe;
            auto owner=ii::process::Process::start(*command);if(!owner)return std::unexpected(owner.error());
            auto& accepted=*position->second;accepted.owner.emplace(std::move(*owner));accepted.started=Clock::now();auto& value=accepted.value;
            value.pid=accepted.owner->id();value.started_ms=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(accepted.started-p.started).count());admission.committed=true;reply.process=value;return reply;
        }
        if(request.action==ProcessAction::list) {
            auto it=request.cursor.empty()?p.records.begin():p.records.upper_bound(request.cursor);
            while(it!=p.records.end()&&reply.processes.size()<request.limit){p.sample(*it->second);reply.processes.push_back(it->second->value);++it;}
            if(it!=p.records.end()&&!reply.processes.empty())reply.next_cursor=reply.processes.back().id;return reply;
        }
        const auto found=p.records.find(request.id);if(found==p.records.end())return core::fail(core::ErrorCode::not_found,"No process handle belongs to this VM boot");
        auto& record=*found->second;auto& value=record.value;
        if(request.action==ProcessAction::release) {if(record.owner||!record.tree_retired)return core::fail(core::ErrorCode::busy,"Only fully retired process records can be released");p.records.erase(found);reply.released=true;return reply;}
        if(request.action==ProcessAction::stop){if(auto result=p.terminate(record,ProcessState::stopped);!result)return std::unexpected(result.error());}
        if(request.action==ProcessAction::write) {
            if(value.exit||value.input_closing||value.input_closed)return core::fail(core::ErrorCode::closed,"Process stdin is closed");
            if(request.offset!=value.input_accepted)return core::fail(core::ErrorCode::out_of_range,"Stdin offset differs from accepted byte count; inspect before resuming");
            const auto pending=record.pending.size()-record.pending_offset;
            if(pending+request.data.size()>process_input_capacity)return core::fail(core::ErrorCode::busy,"Process stdin queue is full");
            if(record.pending_offset){record.pending.erase(record.pending.begin(),record.pending.begin()+static_cast<std::ptrdiff_t>(record.pending_offset));record.pending_offset=0;}
            record.pending.insert(record.pending.end(),request.data.begin(),request.data.end());value.input_accepted+=request.data.size();
        }
        if(request.action==ProcessAction::close_input)value.input_closing=true;
        if(request.action==ProcessAction::read){const auto& ring=request.stream==ProcessStream::output?record.output:record.error;if(request.offset>ring.end)return core::fail(core::ErrorCode::out_of_range,"Output cursor is beyond the produced stream");ring.read(reply,request.offset,request.length);}
        p.sample(record);reply.process=value;return reply;
    }catch(const std::exception& error){return core::fail(core::ErrorCode::failed,error.what());}
}
void ProcessSupervisor::shutdown(){if(!impl_||impl_->stopping)return;impl_->stopping=true;for(auto& [id,record]:impl_->records){(void)id;try{if(!record->value.exit)(void)impl_->terminate(*record,ProcessState::stopped);if(impl_->hooks.kill)(void)impl_->hooks.kill(record->value.id);}catch(...){}}}
bool ProcessSupervisor::owns(std::uint64_t pid)const{for(const auto& [id,r]:impl_->records){(void)id;if(r->owner&&!r->value.exit&&r->owner->id()==pid)return true;}return false;}
std::size_t ProcessSupervisor::running()const{std::size_t count=0;for(const auto& [id,r]:impl_->records){(void)id;if(!r->value.exit)++count;}return count;}
std::size_t ProcessSupervisor::retained()const{return impl_->records.size();}
const std::string& ProcessSupervisor::boot_id()const{return impl_->boot;}
}
