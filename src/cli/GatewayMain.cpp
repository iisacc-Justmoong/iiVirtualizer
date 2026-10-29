#include <iiVirtualizer/GatewayAgent.h>
#include <iiVirtualizer/Qemu.h>
#include "../iiVirtualizer/GatewayWire.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <random>
#include <set>

namespace v=ii::virtualizer;
using Json=v::detail::Json;
template<class T,class E> static T take(std::expected<T,E> value) {if(!value)throw std::runtime_error(value.error().message);return std::move(*value);}
static void allowed(const Json& value,std::initializer_list<std::string_view> keys) {
    auto object=take(value.get<Json::Object>());
    for(const auto& [key,item]:object.get()) {(void)item;if(std::find(keys.begin(),keys.end(),key)==keys.end())throw std::invalid_argument("Unknown gateway configuration field");}
}
static std::string text(const Json& value,std::string_view key,std::string fallback={}) {
    if(!value.find(key))return fallback;return v::detail::text_field(value,key);
}
static unsigned integer(const Json& value,std::string_view key,unsigned fallback,unsigned maximum) {
    if(!value.find(key))return fallback;auto number=v::detail::integer_field(value,key);
    if(number>maximum)throw std::invalid_argument("Gateway configuration integer exceeds limit");return static_cast<unsigned>(number);
}
static bool boolean(const Json& value,std::string_view key,bool fallback=false) {
    if(!value.find(key))return fallback;auto flag=take(v::detail::field(value,key).get<bool>());return flag.get();
}
static std::vector<std::string> strings(const Json& value,std::string_view key) {
    std::vector<std::string> result;if(!value.find(key))return result;
    auto items=take(v::detail::field(value,key).get<Json::Array>());
    for(const auto& item:items.get())result.push_back(take(item.get<std::string>()).get());return result;
}
static Json read_json(const v::fs::path& path) {
    if(v::fs::file_size(path)>262144)throw std::invalid_argument("Gateway configuration exceeds limit");
    std::ifstream stream(path,std::ios::binary);if(!stream)throw std::runtime_error("Cannot read gateway configuration");
    std::string bytes((std::istreambuf_iterator<char>(stream)),{});auto result=ii::json::parse(bytes);
    if(!result)throw std::invalid_argument("Malformed gateway configuration");return *result;
}
static v::fs::path path(const v::fs::path& base,const std::string& value) {
    if(value.empty())return {};v::fs::path result(value);return result.is_absolute()?result:(base/result).lexically_normal();
}
static v::Accelerator accelerator(std::string_view value) {
    if(value=="auto")return v::Accelerator::automatic;if(value=="hvf")return v::Accelerator::hvf;
    if(value=="kvm")return v::Accelerator::kvm;if(value=="tcg")return v::Accelerator::tcg;throw std::invalid_argument("Unknown configured accelerator");
}
static std::string bounded_input(std::size_t maximum) {
    std::string result;char byte;
    while(std::cin.get(byte)) {if(result.size()==maximum)throw std::invalid_argument("Gateway input exceeds limit");result+=byte;}
    return result;
}
static int agent(const v::fs::path& filename) {
    auto config=read_json(filename);allowed(config,{"authority","runtimes","grants"});const auto base=filename.parent_path();
    v::IisaccAuthority authority;
    if(auto item=config.find("authority")) {
        const auto& value=item->get();allowed(value,{"endpoint","ca_file","timeout_ms"});
        authority.endpoint=text(value,"endpoint",authority.endpoint);authority.ca_file=path(base,text(value,"ca_file"));
        authority.timeout=std::chrono::milliseconds(integer(value,"timeout_ms",10000,60000));
    }
    auto verifier=v::iisacc_account_verifier(authority);
    std::vector<std::unique_ptr<v::Runtime>> owners;std::vector<v::GatewayRuntime> runtimes;
    auto definitions=take(v::detail::field(config,"runtimes").get<Json::Array>());
    for(const auto& value:definitions.get()) {
        allowed(value,{"id","root","qemu_aarch64","qemu_x86_64","images","workers","capacity"});
        v::QemuOptions engine;engine.aarch64_executable=text(value,"qemu_aarch64",engine.aarch64_executable.string());engine.x86_64_executable=text(value,"qemu_x86_64",engine.x86_64_executable.string());
        v::RuntimeOptions options;options.workers=integer(value,"workers",0,256);options.capacity=integer(value,"capacity",128,65536);
        auto root=path(base,text(value,"root"));if(root.empty())throw std::invalid_argument("Runtime root is required");
        owners.push_back(std::make_unique<v::Runtime>(root,v::qemu_backend(engine),options));
        v::GatewayRuntime runtime{text(value,"id"),*owners.back(),{}};
        if(auto images=value.find("images")) {
            auto list=take(images->get().get<Json::Array>());
            for(const auto& image:list.get()) {
                allowed(image,{"id","kernel","initramfs","architecture","accelerator","cpu_model"});
                v::InstanceSpec spec;spec.kernel=path(base,text(image,"kernel"));spec.initramfs=path(base,text(image,"initramfs"));
                spec.cpu_model=text(image,"cpu_model");
                const auto architecture=text(image,"architecture","aarch64");
                if(architecture=="aarch64")spec.architecture=v::Architecture::aarch64;else if(architecture=="x86_64")spec.architecture=v::Architecture::x86_64;else throw std::invalid_argument("Unknown image architecture");
                spec.accelerator=accelerator(text(image,"accelerator","auto"));runtime.images.push_back({text(image,"id"),std::move(spec)});
            }
        }
        runtimes.push_back(std::move(runtime));
    }
    std::vector<v::GatewayGrant> grants;auto rules=take(v::detail::field(config,"grants").get<Json::Array>());
    for(const auto& value:rules.get()) {
        allowed(value,{"subject","runtime","actions","instances","instance_prefix","all_instances","max_cpus","max_memory_mib","max_disk_mib"});
        v::GatewayGrant grant;grant.subject=text(value,"subject");grant.runtime=text(value,"runtime");grant.instances=strings(value,"instances");
        grant.instance_prefix=text(value,"instance_prefix");grant.all_instances=boolean(value,"all_instances");
        for(const auto& action:strings(value,"actions"))grant.actions.push_back(take(v::gateway_action(action)));
        grant.max_cpus=integer(value,"max_cpus",8,256);grant.max_memory_mib=integer(value,"max_memory_mib",8192,1048576);grant.max_disk_mib=integer(value,"max_disk_mib",65536,16777216);
        grants.push_back(std::move(grant));
    }
    v::GatewayAgent gateway(std::move(verifier),std::move(runtimes),std::move(grants));
    v::gateway_detail::SecretText frame(bounded_input(v::gateway_detail::request_limit));
    std::cout<<gateway.handle(frame.value);return 0;
}
static int call(const v::fs::path& filename,const v::fs::path& request_file,bool session_stdin) {
    auto config=read_json(filename);allowed(config,{"host","user","port","known_hosts","identity_file","ssh_executable","subsystem","timeout_ms","connect_timeout_seconds","runtime","issuer","subject","credential_service","credential_key"});
    const auto base=filename.parent_path();v::SshGatewayEndpoint endpoint;
    endpoint.host=text(config,"host");endpoint.user=text(config,"user");endpoint.port=integer(config,"port",22,65535);
    endpoint.known_hosts=path(base,text(config,"known_hosts"));endpoint.identity_file=path(base,text(config,"identity_file"));
    endpoint.ssh_executable=text(config,"ssh_executable","ssh");endpoint.subsystem=text(config,"subsystem","iivirtualizer");
    endpoint.timeout=std::chrono::milliseconds(integer(config,"timeout_ms",120000,3600000));endpoint.connect_timeout_seconds=integer(config,"connect_timeout_seconds",10,120);
    auto query=read_json(request_file);
    if(query.find("credential"))throw std::invalid_argument("Credentials must not be stored in request files");
    auto object=take(query.get<Json::Object>()).get();
    if(!object.contains("runtime"))object.emplace("runtime",Json(text(config,"runtime")));
    auto request=take(v::gateway_detail::public_request(Json(std::move(object))));
    if(request.request_id.empty()) {
        std::random_device random;request.request_id="cli-";constexpr char hex[]="0123456789abcdef";
        for(unsigned i=0;i<16;++i){auto b=random()&255;request.request_id+=hex[b>>4];request.request_id+=hex[b&15];}
    }
    v::gateway_detail::SecretText input(session_stdin?bounded_input(16384):std::string{});
    while(!input.value.empty()&&(input.value.back()=='\n'||input.value.back()=='\r'))input.value.pop_back();
    v::GatewayCredentials source;
    if(session_stdin)source=[&]()->v::GatewayResult<v::GatewayCredential>{return v::GatewayCredential(text(config,"issuer","https://iisacc.com"),text(config,"subject"),input.value);};
    else source=v::secure_account_session(text(config,"credential_service"),text(config,"credential_key"));
    v::Gateway gateway(std::move(source),v::ssh_gateway_transport(std::move(endpoint)));
    auto reply=gateway.execute(request);
    if(!reply) {std::cout<<v::gateway_detail::encode_error(request.request_id,request.runtime,reply.error());return 1;}
    std::cout<<v::gateway_detail::encode_reply(*reply);
    return std::any_of(reply->items.begin(),reply->items.end(),[](const auto& item){return !item.result;})?1:0;
}
int main(int argc,char** argv) {
    try {
        if(argc==2&&std::string_view(argv[1])=="--version") {std::cout<<IIVIRTUALIZER_VERSION<<'\n';return 0;}
        if(argc==3&&std::string_view(argv[1])=="agent")return agent(v::fs::absolute(argv[2]));
        if((argc==4||argc==5)&&std::string_view(argv[1])=="call") {
            if(argc==5&&std::string_view(argv[4])!="--session-stdin")throw std::invalid_argument("Unknown gateway option");
            return call(v::fs::absolute(argv[2]),v::fs::absolute(argv[3]),argc==5);
        }
        std::cerr<<"Usage: iivirtualizer-gateway agent SERVER_CONFIG\n       iivirtualizer-gateway call CLIENT_CONFIG REQUEST_JSON [--session-stdin]\n";return 2;
    }catch(const std::exception& error) {std::cerr<<"iivirtualizer-gateway: "<<error.what()<<'\n';return 2;}
}
