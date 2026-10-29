#include "../src/iiVirtualizer/VmClient.h"
#include "../src/iiVirtualizer/GatewayAgent.h"
#include "../src/iiVirtualizer/GatewayWire.h"
#include "../src/guest/ProcessSupervisor.h"
#include <fstream>
#include <iostream>
#include <thread>

namespace v=ii::virtualizer;namespace fs=std::filesystem;
static void check(bool ok,const char* what){if(!ok)throw std::runtime_error(what);}
template<class T,class E>static T take(std::expected<T,E> result){if(!result)throw std::runtime_error("Expected successful gateway process result");return std::move(*result);}
int main(int argc,char** argv) {
    try {
        if(argc!=3)return 2;const auto root=fs::absolute(argv[1]);fs::remove_all(root);fs::create_directories(root);std::ofstream(root/"kernel")<<"k";std::ofstream(root/"initramfs")<<"i";
        std::size_t launches=0,process_calls=0;
        auto launcher=[&](const v::ProcessLaunch& launch)->ii::core::Result<ii::process::Launch>{++launches;ii::process::Launch command;command.arguments={argv[2],launch.executable.substr(1)};command.inherit_environment=false;command.environment["PATH"]="/usr/bin:/bin";return command;};
        v::guest::ProcessSupervisor a(launcher),b(launcher);std::map<std::string,v::State> states;
        v::Backend backend;
        backend.start=[&](const v::Instance& vm)->v::Result<void>{states[vm.spec.id]=v::State::running;return {};};
        backend.control=[&](const v::Instance& vm,v::Command command)->v::Result<v::State>{auto& state=states[vm.spec.id];if(command==v::Command::pause)state=v::State::paused;else if(command==v::Command::resume)state=v::State::running;else if(command==v::Command::terminate||command==v::Command::power_button)state=v::State::stopped;return state;};
        backend.guest=[](const v::Instance&,std::string_view)->v::Result<std::string>{return "fixture";};
        backend.processes=[&](const v::Instance& vm,const v::ProcessRequest& request,std::chrono::milliseconds)->v::Result<v::ProcessReply>{++process_calls;auto& supervisor=vm.spec.id=="owner-a"?a:b;if(request.action==v::ProcessAction::monitor){v::ProcessReply reply;reply.monitor=v::VmMonitor{.boot_id=supervisor.boot_id(),.filesystem_profile="society-v1",.cpus=1,.memory_total=134217728,.memory_available=67108864,.cpu_total_ticks=100,.cpu_idle_ticks=20,.clock_ticks=100,.running=supervisor.running(),.retained=supervisor.retained(),.workers=2,.processes_available=true,.files_available=true};return reply;}return supervisor.execute(request);};
        backend.files=[](const v::Instance&,const v::FileRequest& request,std::chrono::milliseconds)->v::Result<v::FileReply>{v::FileReply reply;reply.entry=v::FileEntry{.path=request.path,.name=request.path,.kind=v::FileKind::regular};return reply;};
        v::Runtime runtime(root/"state",std::move(backend));
        for(const auto& id:{"owner-a","owner-b"}){v::InstanceSpec spec;spec.id=id;spec.kernel=root/"kernel";spec.initramfs=root/"initramfs";spec.memory_mib=256;spec.disk_mib=16;take(runtime.create(spec));take(runtime.start(id));}
        v::GatewayGrant owner;owner.subject="owner";owner.runtime="local";owner.instance_prefix="owner-";
        owner.actions={v::GatewayAction::describe,v::GatewayAction::pause,v::GatewayAction::resume,v::GatewayAction::file_stat,v::GatewayAction::monitor,v::GatewayAction::process_launch,v::GatewayAction::process_list,v::GatewayAction::process_stat,v::GatewayAction::process_read,v::GatewayAction::process_write,v::GatewayAction::process_close_input,v::GatewayAction::process_stop,v::GatewayAction::process_release};
        v::GatewayGrant reader;reader.subject="reader";reader.runtime="local";reader.instances={"owner-a"};reader.actions={v::GatewayAction::monitor,v::GatewayAction::process_list,v::GatewayAction::process_stat,v::GatewayAction::process_read};bool revoked=false;
        v::GatewayAgent agent([&](const v::GatewayCredential& proof)->v::GatewayResult<v::GatewayIdentity>{if(revoked)return std::unexpected(v::GatewayError{v::GatewayErrorCode::unauthenticated,"Revoked fixture"});return v::GatewayIdentity{proof.subject(),"fixture@example.test","fixture"};},{{"local",runtime,{}}},{owner,reader});
        auto gateway=[&](std::string subject){return v::Gateway([subject]()->v::GatewayResult<v::GatewayCredential>{return v::GatewayCredential("https://iisacc.test",subject,"iisacc_auth_id=fixture; iisacc_login_session=fixture");},[&](std::string_view input)->v::GatewayResult<std::string>{return agent.handle(input);});};
        auto writer=gateway("owner"),viewer=gateway("reader");v::VmClient vm(writer,"local","owner-a"),other(writer,"local","owner-b"),read(viewer,"local","owner-a"),forbidden(viewer,"local","owner-b");
        auto launched=take(vm.launch({.key="echo",.executable="@echo"}));check(take(vm.launch({.key="echo",.executable="@echo"})).id==launched.id&&launches==1,"gateway idempotent launch");
        auto isolated=take(other.launch({.key="echo",.executable="@echo"}));check(isolated.id!=launched.id,"VM boot handle isolation");
        const std::string content("gateway\0binary",14);take(vm.write(launched.id,0,ii::core::byte_view(content)));take(vm.close_input(launched.id));
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while(!take(vm.process(launched.id)).output_eof){a.tick();if(std::chrono::steady_clock::now()>deadline)throw std::runtime_error("gateway pipe deadline");std::this_thread::sleep_for(std::chrono::milliseconds(2));}
        auto output=take(read.read(launched.id,v::ProcessStream::output));check(std::string(reinterpret_cast<const char*>(output.data.data()),output.data.size())==content,"facade binary output");
        check(take(vm.process(launched.id)).exit->code==7,"facade exit status");const auto before=process_calls;
        check(!read.stop(launched.id)&&process_calls==before,"read grant cannot stop");check(!forbidden.monitor()&&process_calls==before,"monitor grant VM scope");
        auto missing=other.process(launched.id);check(!missing&&std::holds_alternative<ii::core::Error>(missing.error()),"stale handle retains VM error type");
        check(take(vm.monitor()).boot_id==a.boot_id()&&take(other.monitor()).running==1,"structured VM metrics");
        take(vm.control(v::Operation::pause));auto paused=vm.monitor();check(!paused&&std::get<ii::core::Error>(paused.error()).code==ii::core::ErrorCode::busy,"paused guest monitoring error");check(take(vm.describe()).state==v::State::paused&&take(other.monitor()).running==1,"host state and other VM remain available");take(vm.control(v::Operation::resume));
        revoked=true;auto denied=read.process(launched.id);check(!denied&&std::get<v::GatewayError>(denied.error()).code==v::GatewayErrorCode::unauthenticated,"revocation before process access");revoked=false;
        v::GatewayRequest confusion;confusion.runtime="local";confusion.action=v::GatewayAction::process_read;confusion.instances={"owner-a"};confusion.process=v::ProcessRequest{.action=v::ProcessAction::stop,.id=launched.id};check(!writer.execute(confusion),"process action confusion rejected");
        confusion.action=v::GatewayAction::process_stop;confusion.file=v::FileRequest{};check(!writer.execute(confusion),"mixed Files and process payload rejected");
        take(other.stop(isolated.id));for(unsigned i=0;i<20&&!take(other.process(isolated.id)).output_eof;++i){b.tick();std::this_thread::sleep_for(std::chrono::milliseconds(2));}
        auto released=vm.release(launched.id);check(bool(released),"facade release");
        v::FileRequest file;file.action=v::FileAction::stat;file.path="inside";check(take(vm.files(file)).entry->path=="inside","same VM facade Files API");
        std::cout<<"VM client, account/VM/action grants, lifecycle monitoring, binary pipes, revocation and error layers passed\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
