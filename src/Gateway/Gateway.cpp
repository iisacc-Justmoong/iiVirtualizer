#include "GatewayWire.h"
#include <iiSDKCore/Process/Process.h>
#include <openssl/rand.h>
#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>

namespace ii::virtualizer {
namespace {
bool path_allowed(const fs::path& path) {
    auto value=path.string();
    return path.is_absolute()&&value.size()<=4096&&value.find_first_of("\r\n\0%$",0,5)==std::string::npos;
}
std::string quoted_path(const fs::path& path) {
    std::string result="\"";
    for(char c:path.string()) {if(c=='"'||c=='\\')result+='\\';result+=c;}
    return result+'"';
}
}
GatewayResult<std::vector<std::string>> ssh_gateway_arguments(const SshGatewayEndpoint& endpoint) {
    auto host=std::string_view(endpoint.host);
    const bool host_ok=!host.empty()&&host.size()<=253&&host.front()!='-'&&std::all_of(host.begin(),host.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='-'||c==':';});
    if(!host_ok||!gateway_detail::key(endpoint.user,64)||!gateway_detail::key(endpoint.subsystem,64)||!endpoint.port||endpoint.port>65535||!path_allowed(endpoint.known_hosts)||
        (!endpoint.identity_file.empty()&&!path_allowed(endpoint.identity_file))||endpoint.timeout.count()<=0||!endpoint.connect_timeout_seconds||endpoint.connect_timeout_seconds>120||endpoint.ssh_executable.empty())
        return std::unexpected(GatewayError{GatewayErrorCode::invalid_request,"Invalid SSH gateway endpoint or trust paths"});
    std::vector<std::string> args{endpoint.ssh_executable.string(),"-F","none","-T","-e","none","-p",std::to_string(endpoint.port),"-l",endpoint.user,
        "-o","BatchMode=yes","-o","StrictHostKeyChecking=yes","-o","UserKnownHostsFile="+quoted_path(endpoint.known_hosts),
        "-o","GlobalKnownHostsFile=none","-o","UpdateHostKeys=no","-o","PasswordAuthentication=no","-o","KbdInteractiveAuthentication=no",
        "-o","PreferredAuthentications=publickey","-o","IdentitiesOnly=yes","-o","ForwardAgent=no","-o","ClearAllForwardings=yes",
        "-o","PermitLocalCommand=no","-o","RequestTTY=no","-o","ControlMaster=no","-o","ControlPath=none","-o","ConnectionAttempts=1",
        "-o","ConnectTimeout="+std::to_string(endpoint.connect_timeout_seconds),"-o","LogLevel=ERROR"};
    if(!endpoint.identity_file.empty())args.insert(args.end(),{"-i",endpoint.identity_file.string()});
    args.insert(args.end(),{"-s",endpoint.host,endpoint.subsystem});return args;
}
GatewayTransport ssh_gateway_transport(SshGatewayEndpoint endpoint) {
    auto arguments=ssh_gateway_arguments(endpoint);if(!arguments)throw std::invalid_argument(arguments.error().message);
    return [endpoint=std::move(endpoint),arguments=std::move(*arguments)](std::string_view frame) -> GatewayResult<std::string> {
        if(frame.size()>gateway_detail::request_limit)return std::unexpected(GatewayError{GatewayErrorCode::invalid_request,"Gateway request exceeds limit"});
        try {
            ii::process::Launch launch;launch.arguments=arguments;launch.environment["LC_ALL"]="C";
            auto process=ii::process::Process::start(launch);
            if(!process)return std::unexpected(GatewayError{GatewayErrorCode::transport,"SSH gateway process could not be started"});
            ii::process::CommunicationOptions options;options.deadline=ii::core::Deadline::after(endpoint.timeout);
            options.output_limit=gateway_detail::reply_limit;options.error_limit=16384;
            auto result=process->communicate(std::as_bytes(std::span(frame.data(),frame.size())),options);
            if(!result)return std::unexpected(GatewayError{GatewayErrorCode::transport,"SSH gateway communication failed"});
            if(result->exit.reason==ii::process::ExitReason::timed_out)
                return std::unexpected(GatewayError{GatewayErrorCode::timed_out,"SSH gateway deadline exceeded; inspect VM state before retrying"});
            if(result->exit.reason!=ii::process::ExitReason::exited||result->exit.code!=0)
                return std::unexpected(GatewayError{GatewayErrorCode::transport,"SSH gateway failed; check host trust, SSH identity and subsystem configuration (exit="+std::to_string(result->exit.code)+")"});
            if(result->output_truncated||result->error_truncated)return std::unexpected(GatewayError{GatewayErrorCode::protocol,"SSH gateway output exceeded limits"});
            return std::string(reinterpret_cast<const char*>(result->output.data()),result->output.size());
        }catch(...) {return std::unexpected(GatewayError{GatewayErrorCode::transport,"SSH gateway communication could not be completed"});}
    };
}
Gateway::Gateway(GatewayCredentials source,GatewayTransport transport):credentials_(std::move(source)),transport_(std::move(transport)) {
    if(!credentials_||!transport_)throw std::invalid_argument("Gateway requires credentials and transport instances");
}
GatewayResult<GatewayReply> Gateway::execute(GatewayRequest request) const {
    try {
        if(auto valid=gateway_detail::validate(request);!valid)return std::unexpected(valid.error());
        if(request.request_id.empty()) {
            std::array<unsigned char,16> bytes{};if(RAND_bytes(bytes.data(),bytes.size())!=1)throw std::runtime_error("Request id generation failed");
            std::ostringstream id;for(auto b:bytes)id<<std::hex<<std::setfill('0')<<std::setw(2)<<static_cast<unsigned>(b);request.request_id=id.str();
        }
        auto proof=credentials_();if(!proof)return std::unexpected(proof.error());
        if(auto valid=gateway_detail::validate(*proof);!valid)return std::unexpected(valid.error());
        gateway_detail::SecretText frame(gateway_detail::encode_request(request,*proof));
        if(frame.value.size()>gateway_detail::request_limit)return std::unexpected(GatewayError{GatewayErrorCode::invalid_request,"Gateway request exceeds limit"});
        auto response=transport_(frame.value);if(!response)return std::unexpected(response.error());
        auto reply=gateway_detail::decode_reply(*response,request);if(!reply)return reply;
        if(reply->account.subject!=proof->subject())return std::unexpected(GatewayError{GatewayErrorCode::protocol,"Gateway reply identifies another account"});
        return reply;
    }catch(...) {return std::unexpected(GatewayError{GatewayErrorCode::protocol,"Gateway request could not be completed"});}
}
GatewayResult<GatewayReply> Gateway::files(std::string runtime,std::string instance,FileRequest file,std::chrono::milliseconds timeout) const {
    GatewayRequest request;request.runtime=std::move(runtime);request.instances.push_back(std::move(instance));request.timeout=timeout;
    switch(file.action) {
        case FileAction::list:request.action=GatewayAction::file_list;break;case FileAction::stat:request.action=GatewayAction::file_stat;break;
        case FileAction::read:request.action=GatewayAction::file_read;break;case FileAction::create:request.action=GatewayAction::file_create;break;
        case FileAction::write:request.action=GatewayAction::file_write;break;case FileAction::make_directory:request.action=GatewayAction::file_mkdir;break;
        case FileAction::move:request.action=GatewayAction::file_move;break;case FileAction::remove:request.action=GatewayAction::file_remove;break;
        default:return std::unexpected(GatewayError{GatewayErrorCode::invalid_request,"Unknown Files operation"});
    }
    request.file=std::move(file);return execute(std::move(request));
}
GatewayResult<GatewayReply> Gateway::processes(std::string runtime,std::string instance,ProcessRequest process,std::chrono::milliseconds timeout) const {
    GatewayRequest request;request.runtime=std::move(runtime);request.instances.push_back(std::move(instance));request.timeout=timeout;
    bool found=false;
    for(auto action:{GatewayAction::process_launch,GatewayAction::process_list,GatewayAction::process_stat,GatewayAction::process_read,GatewayAction::process_write,GatewayAction::process_close_input,GatewayAction::process_stop,GatewayAction::process_release,GatewayAction::monitor})
        if(gateway_process_action(action)==process.action){request.action=action;found=true;break;}
    if(!found)return std::unexpected(GatewayError{GatewayErrorCode::invalid_request,"Unknown process action"});
    request.process=std::move(process);return execute(std::move(request));
}
} // namespace ii::virtualizer
