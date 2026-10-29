#include "GatewayAgent.h"
#include "GatewayWire.h"
#include <algorithm>
#include <set>

namespace ii::virtualizer {
namespace {
bool allows(const GatewayGrant& grant,std::string_view id) {
    return grant.all_instances||(!grant.instance_prefix.empty()&&id.starts_with(grant.instance_prefix))||
        std::find(grant.instances.begin(),grant.instances.end(),id)!=grant.instances.end();
}
bool valid_instance(std::string_view id) {InstanceSpec spec;spec.id=id;spec.kernel="kernel";spec.initramfs="initramfs";return bool(validate(spec));}
}
GatewayAgent::GatewayAgent(GatewayVerifier verifier,std::vector<GatewayRuntime> runtimes,std::vector<GatewayGrant> grants)
    :verify_(std::move(verifier)),runtimes_(std::move(runtimes)),grants_(std::move(grants)) {
    if(!verify_||runtimes_.empty())throw std::invalid_argument("Gateway agent requires account verification and registered runtimes");
    std::set<std::string> ids;
    for(const auto& runtime:runtimes_) {
        if(!gateway_detail::key(runtime.id)||!ids.insert(runtime.id).second)throw std::invalid_argument("Invalid or duplicate gateway runtime");
        std::set<std::string> images;for(const auto& image:runtime.images)
            if(!gateway_detail::key(image.id,64)||!images.insert(image.id).second)throw std::invalid_argument("Invalid gateway image catalog");
    }
    std::set<std::pair<std::string,std::string>> bindings;
    for(const auto& grant:grants_) {
        if(!gateway_detail::key(grant.subject)||!ids.contains(grant.runtime)||!bindings.emplace(grant.subject,grant.runtime).second||grant.actions.empty()||
            grant.max_cpus<1||grant.max_cpus>256||grant.max_memory_mib<64||grant.max_memory_mib>1048576||grant.max_disk_mib<16||grant.max_disk_mib>16777216)
            throw std::invalid_argument("Invalid account grant");
        if(!grant.instance_prefix.empty()&&(grant.instance_prefix.size()>32||!std::all_of(grant.instance_prefix.begin(),grant.instance_prefix.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-';})))throw std::invalid_argument("Invalid instance prefix");
        for(const auto& id:grant.instances)if(!valid_instance(id))throw std::invalid_argument("Invalid granted instance id");
        for(auto action:grant.actions)if(name(action)=="unknown")throw std::invalid_argument("Invalid granted action");
    }
}
std::string GatewayAgent::handle(std::string_view frame) const {
    std::string id,runtime_id;
    auto fail=[&](GatewayErrorCode code,std::string message){return gateway_detail::encode_error(id,runtime_id,{code,std::move(message)});};
    try {
        auto envelope=gateway_detail::decode_request(frame);
        if(!envelope)return fail(envelope.error().code,envelope.error().message);
        const auto& request=envelope->request;id=request.request_id;runtime_id=request.runtime;
        if(auto valid=gateway_detail::validate(envelope->credential);!valid)return fail(valid.error().code,valid.error().message);
        auto account=verify_(envelope->credential);
        if(!account)return fail(account.error().code,account.error().message);
        if(account->subject!=envelope->credential.subject()||!gateway_detail::key(account->subject))return fail(GatewayErrorCode::unauthenticated,"Account identity binding failed");
        auto grant=std::find_if(grants_.begin(),grants_.end(),[&](const auto& value){return value.subject==account->subject&&value.runtime==runtime_id;});
        if(grant==grants_.end()||std::find(grant->actions.begin(),grant->actions.end(),request.action)==grant->actions.end())return fail(GatewayErrorCode::forbidden,"Account is not permitted to perform this runtime operation");
        for(const auto& target:request.instances)if(!allows(*grant,target))return fail(GatewayErrorCode::forbidden,"Account is not permitted to control the requested instances");
        if(request.create) {
            const auto& c=*request.create;
            if(!allows(*grant,c.id)||c.cpus>grant->max_cpus||c.memory_mib>grant->max_memory_mib||c.disk_mib>grant->max_disk_mib||
                std::any_of(c.forwards.begin(),c.forwards.end(),[](auto p){return p.host<1024;}))
                return fail(GatewayErrorCode::forbidden,"Instance creation exceeds the account grant");
        }
        auto binding=std::find_if(runtimes_.begin(),runtimes_.end(),[&](const auto& value){return value.id==runtime_id;});
        if(binding==runtimes_.end())return fail(GatewayErrorCode::not_found,"Runtime is unavailable");
        auto& runtime=binding->runtime.get();GatewayReply reply{id,runtime_id,*account,{}};
        if(request.action==GatewayAction::create) {
            const auto& c=*request.create;
            auto image=std::find_if(binding->images.begin(),binding->images.end(),[&](const auto& value){return value.id==c.image;});
            if(image==binding->images.end())return fail(GatewayErrorCode::not_found,"Requested boot image is unavailable");
            auto spec=image->defaults;spec.id=c.id;spec.cpus=c.cpus;spec.memory_mib=c.memory_mib;spec.disk_mib=c.disk_mib;spec.network=c.network;spec.forwards=c.forwards;
            auto created=runtime.create(spec);
            if(created)reply.items.push_back({c.id,GatewayValue{*created,{},false}});else reply.items.push_back({c.id,std::unexpected(created.error())});
        }else if(request.action==GatewayAction::list) {
            auto inventory=runtime.snapshot();if(!inventory)return fail(GatewayErrorCode::unavailable,"Runtime inventory is unavailable");
            for(const auto& row:*inventory)if(allows(*grant,row.id)) {
                if(reply.items.size()>=gateway_detail::batch_limit)return fail(GatewayErrorCode::unavailable,"Inventory exceeds the gateway response limit");
                if(row.result)reply.items.push_back({row.id,GatewayValue{*row.result,{},false}});else reply.items.push_back({row.id,std::unexpected(row.result.error())});
            }
        }else if(request.process) {
            const auto& target=request.instances.front();auto before=runtime.describe(target);
            if(!before)reply.items.push_back({target,std::unexpected(before.error())});
            else {
                auto result=runtime.processes(target,*request.process,request.timeout);
                if(result)reply.items.push_back({target,GatewayValue{*before,{},false,std::nullopt,std::move(*result)}});
                else reply.items.push_back({target,std::unexpected(result.error())});
            }
        }else if(request.file) {
            const auto& target=request.instances.front();auto before=runtime.describe(target);
            if(!before)reply.items.push_back({target,std::unexpected(before.error())});
            else {
                auto result=runtime.files(target,*request.file,request.timeout);
                if(result)reply.items.push_back({target,GatewayValue{*before,{},false,std::move(*result)}});
                else reply.items.push_back({target,std::unexpected(result.error())});
            }
        }else if(request.action==GatewayAction::remove||request.action==GatewayAction::guest_info) {
            for(const auto& target:request.instances) {
                auto before=runtime.describe(target);if(!before) {reply.items.push_back({target,std::unexpected(before.error())});continue;}
                if(request.action==GatewayAction::remove) {
                    auto removed=runtime.remove(target);if(removed)reply.items.push_back({target,GatewayValue{*before,{},true}});else reply.items.push_back({target,std::unexpected(removed.error())});
                }else {
                    auto info=runtime.guest_info(target);if(info)reply.items.push_back({target,GatewayValue{*before,*info,false}});else reply.items.push_back({target,std::unexpected(info.error())});
                }
            }
        }else {
            Operation operation=Operation::describe;
            switch(request.action) {
                case GatewayAction::start:operation=Operation::start;break;case GatewayAction::pause:operation=Operation::pause;break;
                case GatewayAction::resume:operation=Operation::resume;break;case GatewayAction::stop:operation=Operation::stop;break;
                case GatewayAction::force_stop:operation=Operation::force_stop;break;default:break;
            }
            auto values=runtime.apply(operation,request.instances,request.timeout);
            if(!values)return fail(GatewayErrorCode::unavailable,"Runtime operation could not be submitted");
            for(auto& row:*values)if(row.result)reply.items.push_back({row.id,GatewayValue{std::move(*row.result),{},false}});else reply.items.push_back({row.id,std::unexpected(row.result.error())});
        }
        auto output=gateway_detail::encode_reply(reply);
        if(output.size()>gateway_detail::reply_limit)return fail(GatewayErrorCode::unavailable,"Gateway reply exceeds limit");return output;
    }catch(...) {return fail(GatewayErrorCode::unavailable,"Gateway operation could not be completed");}
}
} // namespace ii::virtualizer
