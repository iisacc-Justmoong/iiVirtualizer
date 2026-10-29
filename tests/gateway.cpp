#include <iiVirtualizer/Gateway.h>
#include <iiVirtualizer/GatewayAgent.h>
#include "../src/Gateway/GatewayWire.h"
#include <algorithm>
#include <atomic>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>

namespace v=ii::virtualizer;
void check(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
template<class T,class E> T take(std::expected<T,E> value) {if(!value)throw std::runtime_error(value.error().message);return std::move(*value);}
int main(int argc,char** argv) {
    try {
        check(argc==2,"build directory required");
        auto root=v::fs::absolute(argv[1])/("g"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()%1000000));
        v::fs::create_directories(root);auto kernel=root/"kernel",initrd=root/"initramfs";
        std::ofstream(kernel)<<"fixture";std::ofstream(initrd)<<"fixture";
        std::mutex mutex;std::map<std::string,v::State> states;std::atomic<unsigned> executions=0,checks=0;bool revoked=false;
        v::Backend backend{
            [&](const v::Instance& vm)->v::Result<void>{std::lock_guard lock(mutex);++executions;states[vm.uuid]=v::State::running;return {};},
            [&](const v::Instance& vm,v::Command command)->v::Result<v::State>{std::lock_guard lock(mutex);++executions;auto& state=states.try_emplace(vm.uuid,v::State::stopped).first->second;if(command==v::Command::pause)state=v::State::paused;if(command==v::Command::resume)state=v::State::running;if(command==v::Command::terminate)state=v::State::stopped;return state;},
            [&](const v::Instance& vm,std::string_view command)->v::Result<std::string>{std::lock_guard lock(mutex);++executions;if(command=="shutdown")states[vm.uuid]=v::State::stopped;return "IIV1 "+vm.uuid+" INFO pid=1";}
        };
        v::Runtime local(root/"local",backend),other(root/"other",backend);
        v::InstanceSpec image;image.kernel=kernel;image.initramfs=initrd;
        image.kernel_command_line="console=ttyAMA0 root=/dev/vda1 rw";image.graphics=v::Graphics::virtio_gpu;
        v::GatewayGrant alice;alice.subject="alice";alice.runtime="local";alice.instance_prefix="alice-";alice.max_cpus=2;
        for(auto action:{v::GatewayAction::list,v::GatewayAction::create,v::GatewayAction::describe,v::GatewayAction::start,v::GatewayAction::pause,v::GatewayAction::resume,v::GatewayAction::stop,v::GatewayAction::force_stop,v::GatewayAction::remove,v::GatewayAction::guest_info})alice.actions.push_back(action);
        v::GatewayGrant bob;bob.subject="bob";bob.runtime="local";bob.instance_prefix="bob-";bob.actions={v::GatewayAction::list,v::GatewayAction::describe};
        v::GatewayVerifier verifier=[&](const v::GatewayCredential& proof)->v::GatewayResult<v::GatewayIdentity>{
            ++checks;if(revoked)return std::unexpected(v::GatewayError{v::GatewayErrorCode::unauthenticated,"Session revoked"});
            if(proof.cookie_header().find("alice-proof")!=std::string_view::npos)return v::GatewayIdentity{"alice","alice@example.test","alice-public"};
            if(proof.cookie_header().find("bob-proof")!=std::string_view::npos)return v::GatewayIdentity{"bob","bob@example.test","bob-public"};
            return std::unexpected(v::GatewayError{v::GatewayErrorCode::unauthenticated,"Session inactive"});
        };
        v::GatewayAgent agent(verifier,{{"local",local,{{"iios",image}}},{"other",other,{}}},{alice,bob});
        auto credentials=[]()->v::GatewayResult<v::GatewayCredential>{return v::GatewayCredential("https://iisacc.test","alice","iisacc_auth_id=alice-proof; iisacc_login_session=fixture-session");};
        v::Gateway client(credentials,[&](std::string_view frame)->v::GatewayResult<std::string>{return agent.handle(frame);});
        auto create=[&](std::string id,unsigned cpus=1){v::GatewayRequest request;request.runtime="local";request.action=v::GatewayAction::create;request.create=v::GatewayCreate{.id=std::move(id),.image="iios",.cpus=cpus,.memory_mib=64,.disk_mib=16};return client.execute(std::move(request));};
        check(bool(take(create("alice-a")).items[0].result),"gateway create failed");
        check(bool(take(create("alice-b")).items[0].result),"gateway second create failed");
        auto denied=create("alice-too-large",3);check(!denied&&denied.error().code==v::GatewayErrorCode::forbidden,"resource grant ignored");
        image.id="bob-a";take(local.create(image));
        v::GatewayRequest request;request.runtime="local";request.action=v::GatewayAction::list;
        auto listed=take(client.execute(request));check(listed.items.size()==2,"list exposed another account's VM");
        check(listed.items[0].result->instance.spec.kernel_command_line==image.kernel_command_line&&
            listed.items[0].result->instance.spec.graphics==image.graphics,"gateway lost guest boot settings");
        v::InstanceSpec arm;arm.id="alice-arm";arm.architecture=v::Architecture::aarch64;
        arm.boot_mode=v::BootMode::disk;arm.firmware=kernel;arm.firmware_vars=initrd;arm.boot_iso=kernel;arm.boot_disk_first=true;
        arm.disk_bus=v::DiskBus::nvme;arm.graphics=v::Graphics::ramfb;arm.external_tpm=true;
        arm.cpu_model="max,pauth-impdef=on";
        take(local.create(arm));request.action=v::GatewayAction::describe;request.instances={"alice-arm"};
        auto described=take(client.execute(request)).items[0].result->instance.spec;
        check(described.disk_bus==v::DiskBus::nvme&&described.graphics==v::Graphics::ramfb&&described.external_tpm&&
              !described.firmware_vars.empty()&&described.cpu_model==arm.cpu_model&&described.boot_disk_first,"gateway lost ARM disk boot settings");
        request.action=v::GatewayAction::start;request.instances={"alice-a","alice-b"};
        auto started=take(client.execute(request));check(started.items[0].result&&started.items[1].result,"remote multi-VM start failed");
        auto before=executions.load();request.instances={"alice-a","bob-a"};
        denied=client.execute(request);check(!denied&&denied.error().code==v::GatewayErrorCode::forbidden&&executions==before,"mixed unauthorized batch had side effects");
        request.runtime="other";request.instances={"alice-a"};denied=client.execute(request);
        check(!denied&&denied.error().code==v::GatewayErrorCode::forbidden&&executions==before,"cross-runtime grant ignored");
        request.runtime="local";request.action=v::GatewayAction::guest_info;
        check(take(client.execute(request)).items[0].result->guest.find("pid=1")!=std::string::npos,"guest-info response lost");
        request.action=v::GatewayAction::pause;check(take(client.execute(request)).items[0].result->instance.state==v::State::paused,"remote pause");
        request.action=v::GatewayAction::resume;check(take(client.execute(request)).items[0].result->instance.state==v::State::running,"remote resume");
        revoked=true;before=executions;request.action=v::GatewayAction::stop;
        denied=client.execute(request);check(!denied&&denied.error().code==v::GatewayErrorCode::unauthenticated&&executions==before,"revoked account retained authority");revoked=false;
        v::Gateway impostor([]()->v::GatewayResult<v::GatewayCredential>{return v::GatewayCredential("https://iisacc.test","bob","iisacc_auth_id=alice-proof; iisacc_login_session=fixture-session");},[&](std::string_view frame)->v::GatewayResult<std::string>{return agent.handle(frame);});
        denied=impostor.execute(request);check(!denied&&denied.error().code==v::GatewayErrorCode::unauthenticated&&executions==before,"client account claim was trusted");
        v::Gateway mismatched(credentials,[&](std::string_view frame)->v::GatewayResult<std::string>{auto output=agent.handle(frame);auto json=take(ii::json::parse(output));auto object=json.get<v::detail::Json::Object>()->get();object.insert_or_assign("request_id",v::detail::Json("wrong"));return v::detail::json_text(v::detail::Json(std::move(object)));});
        request.action=v::GatewayAction::describe;check(!mismatched.execute(request),"mismatched request reply accepted");
        auto malformed=agent.handle("{\"command\":\"rm\"}");check(malformed.find("alice-proof")==std::string::npos&&checks>0,"secret leaked in rejection");
        v::GatewayCredential refresh("https://iisacc.test","alice","iisacc_auth_refresh=long-lived; iisacc_login_session=fixture-session");
        check(!v::gateway_detail::validate(refresh),"refresh token forwarding allowed");
        v::SshGatewayEndpoint endpoint;endpoint.host="127.0.0.1";endpoint.user="user";endpoint.known_hosts=root/"known_hosts";
        auto args=take(v::ssh_gateway_arguments(endpoint));
        check(args.back()=="iivirtualizer"&&std::find(args.begin(),args.end(),"StrictHostKeyChecking=yes")!=args.end()&&std::find(args.begin(),args.end(),"none")!=args.end(),"SSH trust configuration missing");
        endpoint.host="-oProxyCommand=evil";check(!v::ssh_gateway_arguments(endpoint),"SSH host option injection accepted");
        endpoint.host="localhost";endpoint.subsystem="sh -c evil";check(!v::ssh_gateway_arguments(endpoint),"arbitrary SSH command accepted");
        bool insecure=false;try{(void)v::iisacc_account_verifier({.endpoint="http://iisacc.test/Account/GraphQL"});}catch(const std::invalid_argument&){insecure=true;}check(insecure,"cleartext authority accepted");
        request.action=v::GatewayAction::stop;request.instances={"alice-a","alice-b"};auto stopped=take(client.execute(request));check(stopped.items[0].result&&stopped.items[1].result,"remote shutdown failed");
        request.action=v::GatewayAction::remove;auto removed=take(client.execute(request));check(removed.items[0].result->removed&&removed.items[1].result->removed,"remote removal failed");
        std::cout<<"Account binding, grants, catalog provisioning, SSH arguments, request correlation and remote lifecycle passed\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
