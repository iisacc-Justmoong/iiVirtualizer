#include <iiSDKCore/Process/Process.h>
#include <iiJson/Json.h>
#include <array>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/utsname.h>
#endif

namespace fs=std::filesystem;
using Json=ii::json::Value;
const Json& field(const Json& v,std::string_view k) {auto p=v.find(k);if(!p)throw std::runtime_error("Missing job field: "+std::string(k));return p->get();}
std::string text(const Json& v,std::string_view k) {auto p=field(v,k).get<std::string>();if(!p)throw std::runtime_error("Expected job text");return p->get();}
std::uint64_t number(const Json& v,std::string_view k) {const auto& f=field(v,k);if(auto n=f.get<std::uint64_t>())return n->get();if(auto n=f.get<std::int64_t>();n&&n->get()>=0)return static_cast<std::uint64_t>(n->get());throw std::runtime_error("Expected nonnegative job integer");}
std::string b64(const void* raw,std::size_t size) {
    static constexpr char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto* p=static_cast<const unsigned char*>(raw);std::string out;out.reserve((size+2)/3*4);
    for(std::size_t i=0;i<size;i+=3) {auto n=std::min<std::size_t>(3,size-i);unsigned v=unsigned(p[i])<<16;if(n>1)v|=unsigned(p[i+1])<<8;if(n>2)v|=p[i+2];out+=alphabet[v>>18];out+=alphabet[(v>>12)&63];out+=n>1?alphabet[(v>>6)&63]:'=';out+=n>2?alphabet[v&63]:'=';}
    return out;
}
class Transport final {
public:
    explicit Transport(const fs::path& endpoint) {
        if(endpoint=="stdio")return;
#if defined(_WIN32)
        handle_=CreateFileW(endpoint.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
        if(handle_==INVALID_HANDLE_VALUE)throw std::runtime_error("Open build UART failed");
        DCB dcb{};dcb.DCBlength=sizeof(dcb);if(!GetCommState(handle_,&dcb))throw std::runtime_error("Query UART failed");
        dcb.BaudRate=CBR_115200;dcb.ByteSize=8;dcb.Parity=NOPARITY;dcb.StopBits=ONESTOPBIT;dcb.fBinary=TRUE;
        dcb.fOutxCtsFlow=FALSE;dcb.fOutxDsrFlow=FALSE;dcb.fOutX=FALSE;dcb.fInX=FALSE;
        if(!SetCommState(handle_,&dcb))throw std::runtime_error("Configure UART failed");
#else
        descriptor_=::open(endpoint.c_str(),O_WRONLY|O_CLOEXEC);
        if(descriptor_<0)throw std::runtime_error("Open build channel failed");
#endif
    }
    ~Transport() {
#if defined(_WIN32)
        if(handle_!=INVALID_HANDLE_VALUE)CloseHandle(handle_);
#else
        if(descriptor_>=0)::close(descriptor_);
#endif
    }
    void send(const Json& value) {
        auto encoded=ii::json::stringify(value);if(!encoded)throw std::runtime_error(encoded.error().message);
        std::string line="IIVB1 "+*encoded+"\n";
        if(line.size()>32768)throw std::runtime_error("Build event exceeds bound");
        std::string_view left=line;
        while(!left.empty()) {
#if defined(_WIN32)
            if(handle_==INVALID_HANDLE_VALUE) {std::cout<<left<<std::flush;break;}
            DWORD done=0;if(!WriteFile(handle_,left.data(),static_cast<DWORD>(left.size()),&done,nullptr)||!done)throw std::runtime_error("Write build UART failed");
#else
            if(descriptor_<0) {std::cout<<left<<std::flush;break;}
            auto done=::write(descriptor_,left.data(),left.size());if(done<0&&errno==EINTR)continue;if(done<=0)throw std::runtime_error("Write build channel failed");
#endif
            left.remove_prefix(static_cast<std::size_t>(done));
        }
    }
private:
#if defined(_WIN32)
    HANDLE handle_=INVALID_HANDLE_VALUE;
#else
    int descriptor_=-1;
#endif
};
fs::path relative_file(const fs::path& root,const std::string& input) {
    fs::path path(input);if(path.empty()||path.is_absolute()||path.has_root_name())throw std::runtime_error("Artifact must be relative");
    for(const auto& part:path)if(part=="..")throw std::runtime_error("Artifact escapes build workspace");
    auto full=fs::weakly_canonical(root/path);auto base=fs::canonical(root);
    auto [a,b]=std::mismatch(base.begin(),base.end(),full.begin(),full.end());
    if(a!=base.end())throw std::runtime_error("Artifact link escapes build workspace");
    return full;
}
int main(int argc,char** argv) {
    std::unique_ptr<Transport> output;std::string identity;
    try {
        if(argc!=3)throw std::runtime_error("Usage: iiv-build-agent JOB.json CHANNEL|stdio");
        std::ifstream input(argv[1],std::ios::binary);std::string data((std::istreambuf_iterator<char>(input)),{});
        if(!input||data.size()>65536)throw std::runtime_error("Invalid or oversized build job");
        auto parsed=ii::json::parse(data);if(!parsed)throw std::runtime_error(parsed.error().message);
        const auto job=text(*parsed,"job");if(job.empty()||job.size()>64)throw std::runtime_error("Invalid job identity");identity=job;
        const fs::path workspace=text(*parsed,"workspace");fs::create_directories(workspace);
        output=std::make_unique<Transport>(argv[2]);auto& transport=*output;
        auto send=[&](std::string kind,Json::Object values={}) {values.emplace("job",Json(job));values.emplace("kind",Json(std::move(kind)));transport.send(Json(std::move(values)));};
#if defined(_WIN32)
        SYSTEM_INFO system{};GetNativeSystemInfo(&system);
        std::string os="windows",arch=system.wProcessorArchitecture==PROCESSOR_ARCHITECTURE_AMD64?"x86_64":system.wProcessorArchitecture==PROCESSOR_ARCHITECTURE_ARM64?"aarch64":"other";
#else
        utsname system{};if(::uname(&system)!=0)throw std::runtime_error("Query guest OS failed");
        std::string os=system.sysname,arch=system.machine;
#endif
        send("started",{{"os",Json(os)},{"architecture",Json(arch)},{"workspace",Json(workspace.string())}});
        const auto steps=field(*parsed,"steps").get<Json::Array>();if(!steps||steps->get().size()>32)throw std::runtime_error("Invalid step count");
        unsigned index=0;
        for(const auto& step:steps->get()) {
            ii::process::Launch launch;launch.working_directory=workspace;
            if(step.find("cwd"))launch.working_directory=relative_file(workspace,text(step,"cwd"));
            auto args=field(step,"argv").get<Json::Array>();if(!args||args->get().empty()||args->get().size()>256)throw std::runtime_error("Invalid build argv");
            for(const auto& arg:args->get()) {auto value=arg.get<std::string>();if(!value)throw std::runtime_error("Invalid build argument");launch.arguments.push_back(value->get());}
            if(step.find("path"))launch.environment["PATH"]=text(step,"path");
            launch.input=ii::process::Input::closed;launch.error=ii::process::ErrorOutput::merge_output;
            const auto deadline=number(step,"timeout_ms");if(!deadline||deadline>3600000)throw std::runtime_error("Invalid step deadline");
            send("step",{{"index",Json(index)},{"executable",Json(launch.arguments.front())}});
            auto child=ii::process::Process::start(launch);
            if(!child)throw std::runtime_error(child.error().message);
            ii::process::CommunicationOptions options;options.deadline=ii::core::Deadline::after(std::chrono::milliseconds(deadline));
            options.output_limit=65536;options.error_limit=65536;
            options.receive=[&](ii::process::Stream,ii::core::ByteView bytes) {
                while(!bytes.empty()) {auto count=std::min<std::size_t>(8192,bytes.size());send("log",{{"index",Json(index)},{"data",Json(b64(bytes.data(),count))}});bytes=bytes.subspan(count);}
            };
            auto result=child->communicate({},options);
            if(!result)throw std::runtime_error(result.error().message);
            send("exit",{{"index",Json(index)},{"code",Json(result->exit.code)},{"reason",Json(static_cast<int>(result->exit.reason))}});
            if(result->exit.reason!=ii::process::ExitReason::exited||result->exit.code!=0) {send("failed",{{"step",Json(index)}});return 1;}
            ++index;
        }
        auto artifacts=field(*parsed,"artifacts").get<Json::Array>();if(!artifacts||artifacts->get().size()>64)throw std::runtime_error("Invalid artifact count");
        for(const auto& artifact:artifacts->get()) {
            auto name=artifact.get<std::string>();if(!name)throw std::runtime_error("Invalid artifact name");
            auto path=relative_file(workspace,name->get());if(!fs::is_regular_file(path)||fs::file_size(path)>32*1024*1024)throw std::runtime_error("Invalid artifact file");
            std::ifstream file(path,std::ios::binary);std::array<char,8192> bytes{};std::uint64_t offset=0;
            send("artifact",{{"path",Json(name->get())},{"size",Json(fs::file_size(path))}});
            while(file) {file.read(bytes.data(),bytes.size());auto count=static_cast<std::size_t>(file.gcount());if(!count)break;send("chunk",{{"path",Json(name->get())},{"offset",Json(offset)},{"data",Json(b64(bytes.data(),count))}});offset+=count;}
            send("artifact-end",{{"path",Json(name->get())},{"size",Json(offset)}});
        }
        send("complete",{{"steps",Json(index)}});
        // Keep the OS alive for independent QMP lifecycle verification. The
        // controller owns VM termination; no host filesystem is mounted here.
        if(argc==3&&std::string_view(argv[2])!="stdio")for(;;)std::this_thread::sleep_for(std::chrono::seconds(1));
        return 0;
    }catch(const std::exception& error) {
        if(output)try {output->send(Json(Json::Object{{"job",Json(identity)},{"kind",Json("error")},{"message",Json(error.what())}}));}catch(...) {}
        std::cerr<<"iiv-build-agent: "<<error.what()<<'\n';return 2;
    }
}
