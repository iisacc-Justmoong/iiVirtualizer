#include <iiVirtualizer/GatewayAgent.h>
#include "../src/guest/FilesStore.h"
#include "../src/Files/FilesWire.h"
#include "../src/Gateway/GatewayWire.h"
#include <iiFileProviderStorage.h>
#include <atomic>
#include <iostream>
#include <map>
#include <unistd.h>

namespace v=ii::virtualizer;
static void check(bool value,const char* label){if(!value)throw std::runtime_error(label);}
template<class T,class E>static T take(std::expected<T,E> value){if(!value)throw std::runtime_error(value.error().message);return std::move(*value);}
static ii::core::Bytes bytes(std::string_view value){auto view=ii::core::byte_view(value);return {view.begin(),view.end()};}
int main(int argc,char** argv) {
    try {
        check(argc==2,"test root required");const auto root=v::fs::absolute(argv[1])/std::to_string(::getpid());
        iiFileProvider::native::createDirectories(root);iiFileProvider::native::OutputFile(root/"kernel")<<"fixture";iiFileProvider::native::OutputFile(root/"initramfs")<<"fixture";
        std::map<std::string,v::State> states;unsigned file_calls=0;bool revoked=false;
        v::Backend backend{
            [&](const v::Instance& vm)->v::Result<void>{states[vm.uuid]=v::State::running;return {};},
            [&](const v::Instance& vm,v::Command command)->v::Result<v::State>{auto& state=states.try_emplace(vm.uuid,v::State::stopped).first->second;if(command==v::Command::pause)state=v::State::paused;if(command==v::Command::resume)state=v::State::running;return state;},
            [](const v::Instance&,std::string_view)->v::Result<std::string>{return "fixture";},
            [&](const v::Instance& vm,const v::FileRequest& request,std::chrono::milliseconds)->v::Result<v::FileReply>{++file_calls;v::guest::FilesStore store(root/"data"/vm.spec.id);return store.execute(request);}
        };
        v::Runtime runtime(root/"vms",backend);v::InstanceSpec spec;spec.kernel=root/"kernel";spec.initramfs=root/"initramfs";spec.memory_mib=64;spec.disk_mib=16;
        for(auto id:{"owner-a","other-a"}){spec.id=id;take(runtime.create(spec));take(runtime.start(id));iiFileProvider::native::createDirectories(root/"data"/id);}
        v::GatewayGrant owner;owner.subject="owner";owner.runtime="local";owner.instance_prefix="owner-";
        owner.actions={v::GatewayAction::file_list,v::GatewayAction::file_stat,v::GatewayAction::file_read,v::GatewayAction::file_create,v::GatewayAction::file_write,v::GatewayAction::file_mkdir,v::GatewayAction::file_move,v::GatewayAction::file_remove};
        v::GatewayGrant reader;reader.subject="reader";reader.runtime="local";reader.instances={"owner-a"};reader.actions={v::GatewayAction::file_list,v::GatewayAction::file_stat,v::GatewayAction::file_read};
        v::GatewayAgent agent([&](const v::GatewayCredential& proof)->v::GatewayResult<v::GatewayIdentity>{
            if(revoked)return std::unexpected(v::GatewayError{v::GatewayErrorCode::unauthenticated,"Revoked"});
            return v::GatewayIdentity{proof.cookie_header().find("owner-proof")!=std::string_view::npos?"owner":"reader","fixture@example.test","fixture"};
        },{{"local",runtime,{}}},{owner,reader});
        auto client=[&](std::string who){return v::Gateway([who]()->v::GatewayResult<v::GatewayCredential>{return v::GatewayCredential("https://iisacc.test",who,"iisacc_auth_id="+who+"-proof; iisacc_login_session=fixture");},[&](std::string_view frame)->v::GatewayResult<std::string>{return agent.handle(frame);});};
        auto writer=client("owner"),viewer=client("reader");
        auto run=[&](v::FileRequest request){auto reply=take(writer.files("local","owner-a",std::move(request)));check(reply.items.size()==1,"one VM Files reply");auto value=take(std::move(reply.items[0].result));check(bool(value.file),"typed Files result");return std::move(*value.file);};
        v::FileRequest request;request.action=v::FileAction::make_directory;request.path="docs";run(request);
        request.action=v::FileAction::create;request.path="docs/한글.bin";request.data=bytes(std::string_view("data\0bytes",10));run(request);
        const auto content=request.data;request.action=v::FileAction::read;request.data.clear();check(run(request).data==content,"Gateway binary roundtrip");
        auto read=take(viewer.files("local","owner-a",request));check(read.items[0].result&&read.items[0].result->file->data==content,"read-only grant read");
        auto before=file_calls;request.action=v::FileAction::write;request.data=bytes("edit");
        auto denied=viewer.files("local","owner-a",request);check(!denied&&denied.error().code==v::GatewayErrorCode::forbidden&&file_calls==before,"read grant cannot write");
        request.truncate=true;run(request);request.action=v::FileAction::move;request.data.clear();request.truncate=false;request.destination="docs/moved.bin";run(request);
        request.action=v::FileAction::read;request.path=request.destination;request.destination.clear();check(run(request).data==bytes("edit"),"Gateway write/move");
        before=file_calls;denied=writer.files("local","other-a",request);check(!denied&&denied.error().code==v::GatewayErrorCode::forbidden&&file_calls==before,"cross-VM Files access denied");
        request.path="../.ii-runtime/boot-count";denied=writer.files("local","owner-a",request);check(!denied&&file_calls==before,"Files traversal denied before backend");
        request.path="docs/moved.bin";revoked=true;denied=writer.files("local","owner-a",request);check(!denied&&denied.error().code==v::GatewayErrorCode::unauthenticated&&file_calls==before,"revocation applies to Files");revoked=false;
        take(runtime.pause("owner-a"));auto paused=take(writer.files("local","owner-a",request));check(!paused.items[0].result&&paused.items[0].result.error().code==ii::core::ErrorCode::busy&&file_calls==before,"paused VM Files blocked");take(runtime.resume("owner-a"));
        v::GatewayRequest confused;confused.runtime="local";confused.action=v::GatewayAction::file_read;confused.instances={"owner-a"};request.action=v::FileAction::write;request.data=bytes("bad");confused.file=request;
        check(!writer.execute(confused)&&file_calls==before,"write payload cannot use a read permission");
        request.action=v::FileAction::remove;request.data.clear();run(request);request.action=v::FileAction::list;request.path="docs";check(run(request).entries.empty(),"Gateway deletion");
        // Guest protocol binding and canonical binary transport are independent
        // of the SSH/account envelope and reject stale or forged responses.
        const std::string uuid="00000000-0000-4000-8000-000000000001";request.action=v::FileAction::create;request.path="binary";request.data.resize(v::file_chunk_limit);
        for(std::size_t i=0;i<request.data.size();++i)request.data[i]=static_cast<std::byte>(i%256);
        const auto frame=v::files_detail::encode_request(uuid,"request",request,std::chrono::seconds(5));
        auto decoded=take(v::files_detail::decode_request(frame,uuid));check(decoded.request.data==request.data,"full binary chunk wire roundtrip");
        check(!v::files_detail::decode_request(frame,"00000000-0000-4000-8000-000000000002"),"Files request UUID binding");
        auto object=v::files_detail::request_payload(request);auto tree=object.get<ii::json::Value::Object>()->get();tree.insert_or_assign("data_base64",ii::json::Value("AR=="));
        check(!v::files_detail::parse_request(ii::json::Value(tree),v::FileAction::create),"noncanonical base64 rejected");
        v::FileReply payload;payload.entry=v::FileEntry{.path="binary",.name="binary",.kind=v::FileKind::regular,.size=request.data.size()};
        const auto response=v::files_detail::encode_reply(uuid,"request",request.action,payload);
        check(bool(v::files_detail::decode_reply(response,uuid,"request",request)),"bound Files result");
        check(!v::files_detail::decode_reply(response,uuid,"different",request),"Files reply nonce binding");
        std::cout<<"Gateway Files, per-action grants, VM scope, revocation, typed binary payloads and guest protocol binding passed\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
