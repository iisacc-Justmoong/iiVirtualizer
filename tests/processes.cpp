#include "../src/Processes/ProcessWire.h"
#include "../src/guest/ProcessSupervisor.h"
#include "../src/guest/ProcessBootstrap.h"
#include <iostream>
#include <stdexcept>
#include <thread>
#include <set>

namespace v=ii::virtualizer;
static void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class T>static T take(ii::core::Result<T> result){if(!result)throw std::runtime_error(result.error().message);return std::move(*result);}
int main(int argc,char** argv) {
    try {
        if(argc!=2)return 2;
        v::ProcessLaunch launch{.key="launch-1",.executable="@echo"};
        check(bool(v::validate(launch)),"registered program contract");
        auto configured=launch;configured.environment={{"APP_VALUE","value"},{"GLIBC_TUNABLES","glibc.malloc.perturb=165"}};
        const auto bootstrap=take(v::guest::process_bootstrap_launch(configured));
        check(!bootstrap.inherit_environment&&bootstrap.environment.size()==1&&!bootstrap.environment.contains("APP_VALUE")&&!bootstrap.environment.contains("GLIBC_TUNABLES"),"application environment cannot affect privileged helper startup");
        check(take(v::guest::process_bootstrap_environment(*bootstrap.environment.at(v::guest::process_environment_key)))==configured.environment,"application environment decoded for confined exec");
        check(!v::guest::process_bootstrap_environment("{\"HOME\":\"/\"}")&&!v::guest::process_bootstrap_environment("{\"APP_VALUE\":1}"),"bootstrap environment validates reserved keys and value types");
        auto bad=launch;bad.executable="../outside";check(!v::validate(bad),"executable traversal");
        bad=launch;bad.environment["HOME"]="/";check(!v::validate(bad),"reserved environment");
        bad=launch;bad.arguments.push_back(std::string("a\0b",3));check(!v::validate(bad),"NUL argv");
        unsigned prepared=0,killed=0,retired=0;
        v::guest::ProcessSupervisor failed([](const v::ProcessLaunch&)->ii::core::Result<ii::process::Launch>{ii::process::Launch command;command.arguments={"/iiv-fixture-that-does-not-exist"};command.inherit_environment=false;return command;},
            {.prepare=[&](std::string_view,const v::ProcessLaunch&)->ii::core::Result<void>{++prepared;return {};},.kill=[&](std::string_view)->ii::core::Result<void>{++killed;return {};},.sample={},.retire=[&](std::string_view){++retired;return true;}});
        v::ProcessRequest failure;failure.action=v::ProcessAction::launch;failure.launch=launch;
        check(!failed.execute(failure)&&failed.retained()==0&&prepared==1&&killed==2&&retired==1,"failed spawn rolls back owned cgroup and registry");
        v::guest::ProcessSupervisor supervisor([&](const v::ProcessLaunch& input)->ii::core::Result<ii::process::Launch>{
            ii::process::Launch command;command.arguments={argv[1],input.executable.substr(1)};command.arguments.insert(command.arguments.end(),input.arguments.begin(),input.arguments.end());command.inherit_environment=false;command.environment["PATH"]="/usr/bin:/bin";return command;
        });
        v::ProcessRequest request;request.action=v::ProcessAction::launch;request.launch=launch;
        auto started=take(supervisor.execute(request));check(started.process.has_value(),"owned process admission");const auto id=started.process->id;
        check(take(supervisor.execute(request)).process->id==id&&supervisor.running()==1,"launch key idempotency");
        request.launch->arguments={"changed"};check(!supervisor.execute(request),"conflicting launch key rejected");request.launch.reset();
        auto run=[&](v::ProcessAction action,std::string token){v::ProcessRequest operation;operation.action=action;operation.id=std::move(token);return take(supervisor.execute(operation));};
        auto until=[&](const std::string& token){auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);for(;;){supervisor.tick();auto state=run(v::ProcessAction::stat,token);if(state.process->exit&&state.process->output_eof&&state.process->error_eof)return *state.process;if(std::chrono::steady_clock::now()>deadline)throw std::runtime_error("process pipe/exit deadline");std::this_thread::sleep_for(std::chrono::milliseconds(2));}};
        check(!supervisor.execute({.action=v::ProcessAction::release,.id=id}),"running release denied");
        request={};request.action=v::ProcessAction::write;request.id=id;request.data.resize(v::process_chunk_limit);for(std::size_t i=0;i<request.data.size();++i)request.data[i]=static_cast<std::byte>(i%256);
        const auto data=request.data;auto accepted=take(supervisor.execute(request));check(accepted.process->input_accepted==data.size(),"binary stdin accepted");check(!supervisor.execute(request),"duplicate stdin offset rejected");
        run(v::ProcessAction::close_input,id);const auto ended=until(id);check(ended.exit->code==7&&ended.exit->signal==0&&ended.input_written==data.size()&&!ended.input_discarded,"exit code and input delivery");
        for(auto stream:{v::ProcessStream::output,v::ProcessStream::error}) {
            request={};request.action=v::ProcessAction::read;request.id=id;request.stream=stream;
            auto output=take(supervisor.execute(request));check(output.data==data&&output.eof&&!output.truncated,"independent binary stdout/stderr");
            const auto uuid="00000000-0000-4000-8000-000000000001";const auto frame=v::process_detail::encode_request(uuid,"req",request);check(take(v::process_detail::decode_request(frame,uuid)).request.id==id,"process request frame");
            const auto response=v::process_detail::encode_reply(uuid,"req",request.action,output);check(take(v::process_detail::decode_reply(response,uuid,"req",request)).data==data,"process reply binary frame");
            check(!v::process_detail::decode_reply(response,uuid,"other",request),"process reply request identity");check(!v::process_detail::decode_request(frame,"00000000-0000-4000-8000-000000000002"),"process request UUID");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));check(run(v::ProcessAction::stat,id).process->elapsed_ms==ended.elapsed_ms,"exit elapsed time is frozen");
        check(run(v::ProcessAction::release,id).released,"completed release");check(!supervisor.execute({.action=v::ProcessAction::stat,.id=id}),"released handle absent");
        request={};request.action=v::ProcessAction::launch;request.launch=v::ProcessLaunch{.key="flood",.executable="@flood"};const auto flood=take(supervisor.execute(request)).process->id;const auto flooded=until(flood);
        check(flooded.output_end==262144&&flooded.error_end==262144&&flooded.output_begin==196608&&flooded.exit->code==9,"bounded independent stream retention");
        request={};request.action=v::ProcessAction::read;request.id=flood;auto tail=take(supervisor.execute(request));check(tail.truncated&&tail.offset==196608&&tail.data.size()==16384,"explicit output gap");
        for(std::size_t i=0;i<tail.data.size();++i)check(tail.data[i]==static_cast<std::byte>(i%256),"retained stream bytes");
        request.offset=262145;check(!supervisor.execute(request),"future output offset rejected");run(v::ProcessAction::release,flood);
        request={};request.action=v::ProcessAction::launch;request.launch=v::ProcessLaunch{.key="deadline",.executable="@wait",.lifetime_ms=40};const auto timed=take(supervisor.execute(request)).process->id;
        check(until(timed).state==v::ProcessState::timed_out,"owned lifetime deadline");run(v::ProcessAction::release,timed);
        std::vector<std::string> live;
        for(unsigned i=0;i<v::process_running_limit;++i){request={};request.action=v::ProcessAction::launch;request.launch=v::ProcessLaunch{.key="live-"+std::to_string(i),.executable="@wait"};live.push_back(take(supervisor.execute(request)).process->id);}
        request.launch->key="over-capacity";check(!supervisor.execute(request)&&supervisor.running()==16,"running admission bound");
        for(const auto& token:live){check(run(v::ProcessAction::stop,token).process->state==v::ProcessState::stopped,"owned cancellation");until(token);run(v::ProcessAction::release,token);}
        check(!supervisor.execute({.action=v::ProcessAction::stop,.id="1"}),"numeric system PID cannot be controlled");
        std::set<std::string> recorded;
        for(unsigned i=0;i<v::process_record_limit;++i){request={};request.action=v::ProcessAction::launch;request.launch=v::ProcessLaunch{.key="record-"+std::to_string(i),.executable="@exit"};const auto token=take(supervisor.execute(request)).process->id;until(token);recorded.insert(token);}
        request.launch->key="record-overflow";check(!supervisor.execute(request)&&supervisor.retained()==32,"retained record bound");
        std::set<std::string> listed;request={};request.action=v::ProcessAction::list;request.limit=3;
        do{auto page=take(supervisor.execute(request));check(page.processes.size()<=3,"bounded process page");for(const auto& p:page.processes)listed.insert(p.id);request.cursor=page.next_cursor;}while(!request.cursor.empty());
        check(listed==recorded,"complete sorted process pagination");
        std::cout<<"process ownership, idempotency, binary I/O, EOF, ring gaps, deadlines, capacity, pagination, release and wire binding passed\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
