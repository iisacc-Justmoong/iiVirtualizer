#include "VmClient.h"
#include "GatewayWire.h"

namespace ii::virtualizer {
namespace {
VmResult<GatewayValue> item(GatewayResult<GatewayReply> reply) {
    if(!reply)return std::unexpected(VmError{reply.error()});
    if(reply->items.size()!=1)return std::unexpected(VmError{GatewayError{GatewayErrorCode::protocol,"A VM client requires one bound result"}});
    auto& row=reply->items.front().result;if(!row)return std::unexpected(VmError{row.error()});return std::move(*row);
}
VmResult<ProcessSnapshot> snapshot(VmResult<ProcessReply> reply) {
    if(!reply)return std::unexpected(reply.error());if(!reply->process)return std::unexpected(VmError{GatewayError{GatewayErrorCode::protocol,"Missing process snapshot"}});return std::move(*reply->process);
}
}
VmClient::VmClient(const Gateway& gateway,std::string runtime,std::string instance):gateway_(gateway),runtime_(std::move(runtime)),instance_(std::move(instance)) {
    InstanceSpec spec;spec.id=instance_;spec.kernel="kernel";spec.initramfs="initramfs";
    if(!gateway_detail::key(runtime_)||!validate(spec))throw std::invalid_argument("A VM client requires valid runtime and instance IDs");
}
VmResult<Instance> VmClient::describe()const{return control(Operation::describe);}
VmResult<Instance> VmClient::control(Operation operation,std::chrono::milliseconds timeout)const {
    GatewayRequest request;request.runtime=runtime_;request.instances={instance_};request.timeout=timeout;
    switch(operation){case Operation::describe:request.action=GatewayAction::describe;break;case Operation::start:request.action=GatewayAction::start;break;
        case Operation::pause:request.action=GatewayAction::pause;break;case Operation::resume:request.action=GatewayAction::resume;break;
        case Operation::stop:request.action=GatewayAction::stop;break;case Operation::force_stop:request.action=GatewayAction::force_stop;break;
        default:return std::unexpected(VmError{GatewayError{GatewayErrorCode::invalid_request,"Unknown VM control operation"}});}
    auto reply=item(gateway_.execute(std::move(request)));if(!reply)return std::unexpected(reply.error());return std::move(reply->instance);
}
VmResult<FileReply> VmClient::files(FileRequest request)const {auto reply=item(gateway_.files(runtime_,instance_,std::move(request)));if(!reply)return std::unexpected(reply.error());if(!reply->file)return std::unexpected(VmError{GatewayError{GatewayErrorCode::protocol,"Missing Files result"}});return std::move(*reply->file);}
VmResult<ProcessReply> VmClient::processes(ProcessRequest request)const {auto reply=item(gateway_.processes(runtime_,instance_,std::move(request)));if(!reply)return std::unexpected(reply.error());if(!reply->process)return std::unexpected(VmError{GatewayError{GatewayErrorCode::protocol,"Missing process result"}});return std::move(*reply->process);}
VmResult<ProcessSnapshot> VmClient::launch(ProcessLaunch launch)const {ProcessRequest request;request.action=ProcessAction::launch;request.launch=std::move(launch);return snapshot(processes(std::move(request)));}
VmResult<ProcessSnapshot> VmClient::one(ProcessAction action,std::string id)const{ProcessRequest request;request.action=action;request.id=std::move(id);return snapshot(processes(std::move(request)));}
VmResult<ProcessSnapshot> VmClient::process(std::string id)const{return one(ProcessAction::stat,std::move(id));}
VmResult<ProcessSnapshot> VmClient::stop(std::string id)const{return one(ProcessAction::stop,std::move(id));}
VmResult<ProcessSnapshot> VmClient::close_input(std::string id)const{return one(ProcessAction::close_input,std::move(id));}
VmResult<ProcessSnapshot> VmClient::write(std::string id,std::uint64_t offset,core::ByteView bytes)const {ProcessRequest request;request.action=ProcessAction::write;request.id=std::move(id);request.offset=offset;request.data.assign(bytes.begin(),bytes.end());return snapshot(processes(std::move(request)));}
VmResult<ProcessReply> VmClient::read(std::string id,ProcessStream stream,std::uint64_t offset,std::size_t length)const {ProcessRequest request;request.action=ProcessAction::read;request.id=std::move(id);request.stream=stream;request.offset=offset;request.length=length;return processes(std::move(request));}
VmResult<ProcessReply> VmClient::list(std::string cursor,std::size_t limit)const {ProcessRequest request;request.action=ProcessAction::list;request.cursor=std::move(cursor);request.limit=limit;return processes(std::move(request));}
VmResult<void> VmClient::release(std::string id)const {ProcessRequest request;request.action=ProcessAction::release;request.id=std::move(id);auto reply=processes(std::move(request));if(!reply)return std::unexpected(reply.error());return {};}
VmResult<VmMonitor> VmClient::monitor()const {ProcessRequest request;request.action=ProcessAction::monitor;auto reply=processes(std::move(request));if(!reply)return std::unexpected(reply.error());if(!reply->monitor)return std::unexpected(VmError{GatewayError{GatewayErrorCode::protocol,"Missing VM monitor snapshot"}});return std::move(*reply->monitor);}
}
