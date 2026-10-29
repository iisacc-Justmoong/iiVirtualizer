#include "GatewayWire.h"
#include "FilesWire.h"
#include "ProcessWire.h"
#include <openssl/crypto.h>
#include <algorithm>
#include <set>
#include <stdexcept>

namespace ii::virtualizer {
std::string_view name(GatewayAction action) {
    switch(action) {
        case GatewayAction::list:return "list";case GatewayAction::create:return "create";
        case GatewayAction::describe:return "describe";case GatewayAction::start:return "start";
        case GatewayAction::pause:return "pause";case GatewayAction::resume:return "resume";
        case GatewayAction::stop:return "stop";case GatewayAction::force_stop:return "force-stop";
        case GatewayAction::remove:return "remove";case GatewayAction::guest_info:return "guest-info";
        case GatewayAction::file_list:return "file-list";case GatewayAction::file_stat:return "file-stat";case GatewayAction::file_read:return "file-read";
        case GatewayAction::file_create:return "file-create";case GatewayAction::file_write:return "file-write";case GatewayAction::file_mkdir:return "file-mkdir";
        case GatewayAction::file_move:return "file-move";case GatewayAction::file_remove:return "file-remove";
        case GatewayAction::process_launch:return "process-launch";case GatewayAction::process_list:return "process-list";case GatewayAction::process_stat:return "process-stat";
        case GatewayAction::process_read:return "process-read";case GatewayAction::process_write:return "process-write";case GatewayAction::process_close_input:return "process-close-input";
        case GatewayAction::process_stop:return "process-stop";case GatewayAction::process_release:return "process-release";case GatewayAction::monitor:return "vm-monitor";
    }
    return "unknown";
}
GatewayResult<GatewayAction> gateway_action(std::string_view value) {
    for(auto action:{GatewayAction::list,GatewayAction::create,GatewayAction::describe,GatewayAction::start,
        GatewayAction::pause,GatewayAction::resume,GatewayAction::stop,GatewayAction::force_stop,GatewayAction::remove,GatewayAction::guest_info,
        GatewayAction::file_list,GatewayAction::file_stat,GatewayAction::file_read,GatewayAction::file_create,GatewayAction::file_write,GatewayAction::file_mkdir,GatewayAction::file_move,GatewayAction::file_remove,GatewayAction::process_launch,GatewayAction::process_list,GatewayAction::process_stat,GatewayAction::process_read,GatewayAction::process_write,GatewayAction::process_close_input,GatewayAction::process_stop,GatewayAction::process_release,GatewayAction::monitor})
        if(name(action)==value)return action;
    return std::unexpected(GatewayError{GatewayErrorCode::invalid_request,"Unknown gateway action"});
}
std::optional<ProcessAction> gateway_process_action(GatewayAction action) {
    switch(action){case GatewayAction::process_launch:return ProcessAction::launch;case GatewayAction::process_list:return ProcessAction::list;
        case GatewayAction::process_stat:return ProcessAction::stat;case GatewayAction::process_read:return ProcessAction::read;case GatewayAction::process_write:return ProcessAction::write;
        case GatewayAction::process_close_input:return ProcessAction::close_input;case GatewayAction::process_stop:return ProcessAction::stop;case GatewayAction::process_release:return ProcessAction::release;case GatewayAction::monitor:return ProcessAction::monitor;default:return std::nullopt;}
}
std::optional<FileAction> gateway_file_action(GatewayAction action) {
    switch(action){case GatewayAction::file_list:return FileAction::list;case GatewayAction::file_stat:return FileAction::stat;
        case GatewayAction::file_read:return FileAction::read;case GatewayAction::file_create:return FileAction::create;
        case GatewayAction::file_write:return FileAction::write;case GatewayAction::file_mkdir:return FileAction::make_directory;
        case GatewayAction::file_move:return FileAction::move;case GatewayAction::file_remove:return FileAction::remove;default:return std::nullopt;}
}
GatewayCredential::GatewayCredential(std::string issuer,std::string subject,std::string cookie)
    :issuer_(std::move(issuer)),subject_(std::move(subject)),cookie_(cookie.begin(),cookie.end()) {
    if(!cookie.empty())OPENSSL_cleanse(cookie.data(),cookie.size());
}
GatewayCredential::~GatewayCredential() {if(!cookie_.empty())OPENSSL_cleanse(cookie_.data(),cookie_.size());}
GatewayCredential::GatewayCredential(GatewayCredential&&) noexcept=default;
GatewayCredential& GatewayCredential::operator=(GatewayCredential&& other) noexcept {
    if(this!=&other) {
        if(!cookie_.empty())OPENSSL_cleanse(cookie_.data(),cookie_.size());
        issuer_=std::move(other.issuer_);subject_=std::move(other.subject_);cookie_=std::move(other.cookie_);
    }
    return *this;
}
std::string_view GatewayCredential::cookie_header() const {return {reinterpret_cast<const char*>(cookie_.data()),cookie_.size()};}
} // namespace ii::virtualizer

namespace ii::virtualizer::gateway_detail {
using detail::Json;
SecretText::~SecretText() {if(!value.empty())OPENSSL_cleanse(value.data(),value.size());}
bool key(std::string_view value,std::size_t maximum) {
    if(value.empty()||value.size()>maximum)return false;
    return std::all_of(value.begin(),value.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_';});
}
static void fields(const Json& value,std::initializer_list<std::string_view> allowed) {
    auto object=value.get<Json::Object>();if(!object)throw std::runtime_error("Expected object");
    for(const auto& [name,item]:object->get()) { (void)item;
        if(std::find(allowed.begin(),allowed.end(),name)==allowed.end())throw std::runtime_error("Unknown protocol field");
    }
}
static bool boolean(const Json& value,std::string_view name) {
    auto result=detail::field(value,name).get<bool>();if(!result)throw std::runtime_error("Expected boolean");return result->get();
}
static unsigned bounded(const Json& value,std::string_view name,std::uint64_t maximum) {
    auto result=detail::integer_field(value,name);if(result>maximum)throw std::runtime_error("Integer exceeds limit");return static_cast<unsigned>(result);
}
static std::vector<std::string> strings(const Json& value,std::string_view name) {
    auto result=detail::field(value,name).get<Json::Array>();if(!result||result->get().size()>batch_limit)throw std::runtime_error("Invalid instance list");
    std::vector<std::string> output;for(const auto& item:result->get()) {auto text=item.get<std::string>();if(!text)throw std::runtime_error("Expected instance id");output.push_back(text->get());}return output;
}
static Json ports(const std::vector<PortForward>& forwards) {
    Json::Array values;for(auto p:forwards)values.emplace_back(Json::Object{{"host",Json(p.host)},{"guest",Json(p.guest)}});return Json(std::move(values));
}
static std::vector<PortForward> ports(const Json& value) {
    auto result=value.get<Json::Array>();if(!result||result->get().size()>32)throw std::runtime_error("Invalid port forwards");
    std::vector<PortForward> output;
    for(const auto& port:result->get()) {fields(port,{"host","guest"});output.push_back({bounded(port,"host",65535),bounded(port,"guest",65535)});}
    return output;
}
GatewayResult<void> validate(const GatewayCredential& proof) {
    auto issuer=std::string_view(proof.issuer());
    if(!issuer.starts_with("https://")||issuer.size()>2048||issuer.size()<=8||issuer.substr(8).find_first_of("/@?#\r\n\t ")!=std::string_view::npos||!key(proof.subject()))
        return std::unexpected(GatewayError{GatewayErrorCode::unauthenticated,"Invalid account session binding"});
    auto cookie=proof.cookie_header();
    if(cookie.empty()||cookie.size()>16384)return std::unexpected(GatewayError{GatewayErrorCode::unauthenticated,"Invalid account session"});
    std::set<std::string> names;
    while(!cookie.empty()) {
        const auto end=cookie.find(';');auto item=cookie.substr(0,end);
        while(!item.empty()&&item.front()==' ')item.remove_prefix(1);
        while(!item.empty()&&item.back()==' ')item.remove_suffix(1);
        auto equal=item.find('=');if(equal==std::string_view::npos||equal+1==item.size())break;
        auto name=item.substr(0,equal),value=item.substr(equal+1);
        if((name!="iisacc_auth_id"&&name!="iisacc_auth_linked"&&name!="iisacc_login_session")||!names.insert(std::string(name)).second||value.size()>8192)break;
        if(!std::all_of(value.begin(),value.end(),[](unsigned char c){return c>=0x21&&c<=0x7e&&c!=';'&&c!=','&&c!='"'&&c!='\\';}))break;
        if(end==std::string_view::npos) {
            if(names.size()==2&&names.contains("iisacc_login_session")&&(names.contains("iisacc_auth_id")||names.contains("iisacc_auth_linked")))return {};
            break;
        }
        cookie.remove_prefix(end+1);
    }
    return std::unexpected(GatewayError{GatewayErrorCode::unauthenticated,"A current identity proof and login session are required; refresh credentials are not forwarded"});
}
GatewayResult<void> validate(const GatewayRequest& request) {
    auto fail=[](){return std::unexpected(GatewayError{GatewayErrorCode::invalid_request,"Invalid gateway request"});};
    if(!key(request.runtime)||(!request.request_id.empty()&&!key(request.request_id))||request.instances.size()>batch_limit||request.timeout.count()<0||request.timeout.count()>60000||name(request.action)=="unknown")return fail();
    std::set<std::string> unique;
    for(const auto& id:request.instances) {
        InstanceSpec spec;spec.id=id;spec.kernel="kernel";spec.initramfs="initramfs";
        if(!ii::virtualizer::validate(spec)||!unique.insert(id).second)return fail();
    }
    if(auto action=gateway_process_action(request.action)) {
        if(!request.process||request.file||request.create||request.instances.size()!=1||request.process->action!=*action||request.timeout.count()<1||!ii::virtualizer::validate(*request.process))return fail();
        return {};
    }else if(request.process)return fail();
    if(auto action=gateway_file_action(request.action)) {
        if(!request.file||request.create||request.instances.size()!=1||request.file->action!=*action||request.timeout.count()<1)return fail();
        if(!ii::virtualizer::validate(*request.file))return fail();
    }else if(request.file)return fail();
    if(request.action==GatewayAction::create) {
        if(!request.create||!request.instances.empty()||!key(request.create->image,64))return fail();
        const auto& create=*request.create;
        InstanceSpec spec;spec.id=create.id;spec.cpus=create.cpus;spec.memory_mib=create.memory_mib;spec.disk_mib=create.disk_mib;
        spec.kernel="kernel";spec.initramfs="initramfs";spec.network=create.network;spec.forwards=create.forwards;
        if(!ii::virtualizer::validate(spec)||create.forwards.size()>32)return fail();
    }else if(request.create)return fail();
    else if(request.action==GatewayAction::list) {if(!request.instances.empty())return fail();}
    else if(request.instances.empty())return fail();
    return {};
}
static Json request_json(const GatewayRequest& request) {
    Json::Array ids;for(const auto& id:request.instances)ids.emplace_back(id);
    Json::Object object{{"version",Json(1)},{"request_id",Json(request.request_id)},{"runtime",Json(request.runtime)},
        {"action",Json(name(request.action))},{"instances",Json(std::move(ids))},{"timeout_ms",Json(request.timeout.count())}};
    if(request.create) {
        const auto& c=*request.create;
        object.emplace("create",Json(Json::Object{{"id",Json(c.id)},{"image",Json(c.image)},{"cpus",Json(c.cpus)},
            {"memory_mib",Json(c.memory_mib)},{"disk_mib",Json(c.disk_mib)},{"network",Json(c.network)},{"forwards",ports(c.forwards)}}));
    }
    if(request.file)object.emplace("file",files_detail::request_payload(*request.file));
    if(request.process)object.emplace("process",process_detail::request_payload(*request.process));
    return Json(std::move(object));
}
std::string encode_request(const GatewayRequest& request,const GatewayCredential& proof) {
    auto tree=request_json(request);auto value=tree.get<Json::Object>()->get();
    value.emplace("credential",Json(Json::Object{{"issuer",Json(proof.issuer())},{"subject",Json(proof.subject())},{"cookie",Json(proof.cookie_header())}}));
    return detail::json_text(Json(std::move(value)))+"\n";
}
GatewayResult<GatewayRequest> public_request(const Json& json) {
    try {
        fields(json,{"version","request_id","runtime","action","instances","timeout_ms","create","credential","file","process"});
        if(auto v=json.find("version");v&&detail::integer_field(json,"version")!=1)throw std::runtime_error("Version mismatch");
        GatewayRequest request;
        if(json.find("request_id"))request.request_id=detail::text_field(json,"request_id");
        request.runtime=detail::text_field(json,"runtime");
        auto action=gateway_action(detail::text_field(json,"action"));if(!action)return std::unexpected(action.error());request.action=*action;
        if(json.find("instances"))request.instances=strings(json,"instances");
        if(json.find("timeout_ms"))request.timeout=std::chrono::milliseconds(bounded(json,"timeout_ms",60000));
        if(auto file=json.find("file")) {
            auto operation=gateway_file_action(request.action);if(!operation)throw std::runtime_error("Files payload requires a Files action");
            auto decoded=files_detail::parse_request(file->get(),*operation);if(!decoded)throw std::runtime_error("Invalid Files payload");request.file=std::move(*decoded);
        }
        if(auto process=json.find("process")) {
            auto action=gateway_process_action(request.action);if(!action)throw std::runtime_error("Process payload requires a process action");
            auto decoded=process_detail::parse_request(process->get(),*action);if(!decoded)throw std::runtime_error("Invalid process payload");request.process=std::move(*decoded);
        }else if(auto action=gateway_process_action(request.action);action&&(*action==ProcessAction::monitor||*action==ProcessAction::list))request.process=ProcessRequest{.action=*action};
        if(auto create=json.find("create")) {
            const auto& c=create->get();fields(c,{"id","image","cpus","memory_mib","disk_mib","network","forwards"});
            GatewayCreate value;value.id=detail::text_field(c,"id");value.image=detail::text_field(c,"image");
            if(c.find("cpus"))value.cpus=bounded(c,"cpus",256);
            if(c.find("memory_mib"))value.memory_mib=bounded(c,"memory_mib",1048576);
            if(c.find("disk_mib"))value.disk_mib=detail::integer_field(c,"disk_mib");
            if(c.find("network"))value.network=boolean(c,"network");
            if(c.find("forwards"))value.forwards=ports(detail::field(c,"forwards"));
            request.create=std::move(value);
        }
        if(auto valid=validate(request);!valid)return std::unexpected(valid.error());
        return request;
    }catch(...) {return std::unexpected(GatewayError{GatewayErrorCode::invalid_request,"Malformed gateway request"});}
}
GatewayResult<Envelope> decode_request(std::string_view frame) {
    if(frame.size()>request_limit)return std::unexpected(GatewayError{GatewayErrorCode::invalid_request,"Gateway request exceeds limit"});
    try {
        auto json=ii::json::parse(frame,{.input_bytes=request_limit,.output_bytes=reply_limit,.string_bytes=32768,.items=4096,.depth=16});
        if(!json)throw std::runtime_error("Invalid JSON");
        if(detail::integer_field(*json,"version")!=1)throw std::runtime_error("Version mismatch");
        auto request=public_request(*json);if(!request)return std::unexpected(request.error());
        if(!key(request->request_id))throw std::runtime_error("Missing request id");
        const auto& proof=detail::field(*json,"credential");fields(proof,{"issuer","subject","cookie"});
        return Envelope{std::move(*request),GatewayCredential(detail::text_field(proof,"issuer"),detail::text_field(proof,"subject"),detail::text_field(proof,"cookie"))};
    }catch(...) {return std::unexpected(GatewayError{GatewayErrorCode::invalid_request,"Malformed gateway frame"});}
}
std::string encode_error(std::string_view id,std::string_view runtime,GatewayError error) {
    return detail::json_text(Json(Json::Object{{"version",Json(1)},{"request_id",Json(id)},{"runtime",Json(runtime)},{"ok",Json(false)},
        {"error",Json(Json::Object{{"code",Json(static_cast<int>(error.code))},{"message",Json(error.message)}})}}))+"\n";
}
std::string encode_reply(const GatewayReply& reply) {
    Json::Array rows;
    for(const auto& item:reply.items) {
        Json::Object row{{"id",Json(item.id)},{"ok",Json(bool(item.result))}};
        if(item.result) {
            row.emplace("instance",detail::instance_json(item.result->instance));row.emplace("guest",Json(item.result->guest));row.emplace("removed",Json(item.result->removed));
            if(item.result->file)row.emplace("file",files_detail::reply_payload(*item.result->file));
            if(item.result->process)row.emplace("process",process_detail::reply_payload(*item.result->process));
        }else row.emplace("error",Json(Json::Object{{"code",Json(static_cast<int>(item.result.error().code))},{"message",Json(item.result.error().message)}}));
        rows.emplace_back(std::move(row));
    }
    return detail::json_text(Json(Json::Object{{"version",Json(1)},{"request_id",Json(reply.request_id)},{"runtime",Json(reply.runtime)},{"ok",Json(true)},
        {"account",Json(Json::Object{{"subject",Json(reply.account.subject)},{"email",Json(reply.account.email)},{"user_id",Json(reply.account.user_id)}})},
        {"items",Json(std::move(rows))}}))+"\n";
}
static Instance instance(const Json& value) {
    Instance vm;vm.spec.id=detail::text_field(value,"id");vm.uuid=detail::text_field(value,"uuid");
    vm.spec.cpus=bounded(value,"cpus",256);vm.spec.memory_mib=bounded(value,"memory_mib",1048576);vm.spec.disk_mib=detail::integer_field(value,"disk_mib");
    if(value.find("cpu_model"))vm.spec.cpu_model=detail::text_field(value,"cpu_model");
    vm.spec.kernel=detail::text_field(value,"kernel");vm.spec.initramfs=detail::text_field(value,"initramfs");
    vm.spec.network=boolean(value,"network");vm.spec.initialize_disk=boolean(value,"initialize_disk");vm.spec.forwards=ports(detail::field(value,"forwards"));
    if(value.find("boot_mode")) {
        auto boot=detail::text_field(value,"boot_mode");if(boot!="kernel"&&boot!="disk")throw std::runtime_error("Invalid boot mode");
        vm.spec.boot_mode=boot=="disk"?BootMode::disk:BootMode::kernel;
    }
    if(value.find("disk_bus")) {
        auto bus=detail::text_field(value,"disk_bus");if(bus!="virtio"&&bus!="sata"&&bus!="nvme")throw std::runtime_error("Invalid disk bus");
        vm.spec.disk_bus=bus=="sata"?DiskBus::sata:bus=="nvme"?DiskBus::nvme:DiskBus::virtio;
    }
    if(value.find("guest_channel")) {
        auto channel=detail::text_field(value,"guest_channel");if(channel!="virtio"&&channel!="serial")throw std::runtime_error("Invalid guest channel");
        vm.spec.guest_channel=channel=="serial"?GuestChannel::serial:GuestChannel::virtio;
    }
    if(value.find("allow_reboot"))vm.spec.allow_reboot=boolean(value,"allow_reboot");
    if(value.find("boot_disk_first"))vm.spec.boot_disk_first=boolean(value,"boot_disk_first");
    if(value.find("external_tpm"))vm.spec.external_tpm=boolean(value,"external_tpm");
    if(value.find("boot_iso"))vm.spec.boot_iso=detail::text_field(value,"boot_iso");
    if(value.find("payload_iso"))vm.spec.payload_iso=detail::text_field(value,"payload_iso");
    if(value.find("firmware"))vm.spec.firmware=detail::text_field(value,"firmware");
    if(value.find("firmware_vars"))vm.spec.firmware_vars=detail::text_field(value,"firmware_vars");
    if(value.find("kernel_command_line"))vm.spec.kernel_command_line=detail::text_field(value,"kernel_command_line");
    if(value.find("graphics")) {
        auto graphics=detail::text_field(value,"graphics");
        if(graphics!="none"&&graphics!="virtio-gpu"&&graphics!="vga"&&graphics!="ramfb")throw std::runtime_error("Invalid graphics device");
        vm.spec.graphics=graphics=="virtio-gpu"?Graphics::virtio_gpu:graphics=="vga"?Graphics::vga:graphics=="ramfb"?Graphics::ramfb:Graphics::none;
    }
    auto architecture=detail::text_field(value,"architecture"),accelerator=detail::text_field(value,"accelerator"),state=detail::text_field(value,"state");
    if(architecture=="aarch64")vm.spec.architecture=Architecture::aarch64;else if(architecture=="x86_64")vm.spec.architecture=Architecture::x86_64;else throw std::runtime_error("Invalid architecture");
    if(accelerator=="auto")vm.spec.accelerator=Accelerator::automatic;else if(accelerator=="hvf")vm.spec.accelerator=Accelerator::hvf;else if(accelerator=="kvm")vm.spec.accelerator=Accelerator::kvm;else if(accelerator=="tcg")vm.spec.accelerator=Accelerator::tcg;else throw std::runtime_error("Invalid accelerator");
    if(state=="stopped")vm.state=State::stopped;else if(state=="running")vm.state=State::running;else if(state=="paused")vm.state=State::paused;else if(state=="unavailable")vm.state=State::unavailable;else throw std::runtime_error("Invalid VM state");
    const fs::path directory=detail::text_field(value,"directory");vm.paths=paths_for(directory.parent_path(),vm.spec.id);
    if(!ii::virtualizer::validate(vm.spec)||!detail::valid_uuid(vm.uuid))throw std::runtime_error("Invalid instance");return vm;
}
GatewayResult<GatewayReply> decode_reply(std::string_view frame,const GatewayRequest& request) {
    if(frame.size()>reply_limit)return std::unexpected(GatewayError{GatewayErrorCode::protocol,"Gateway reply exceeds limit"});
    try {
        auto json=ii::json::parse(frame,{.input_bytes=reply_limit,.output_bytes=reply_limit,.string_bytes=65536,.items=32768,.depth=24});
        if(!json||detail::integer_field(*json,"version")!=1||detail::text_field(*json,"request_id")!=request.request_id||detail::text_field(*json,"runtime")!=request.runtime)throw std::runtime_error("Reply binding mismatch");
        if(!boolean(*json,"ok")) {
            const auto& error=detail::field(*json,"error");auto code=bounded(error,"code",static_cast<unsigned>(GatewayErrorCode::unavailable));
            return std::unexpected(GatewayError{static_cast<GatewayErrorCode>(code),detail::text_field(error,"message")});
        }
        GatewayReply reply;reply.request_id=request.request_id;reply.runtime=request.runtime;
        const auto& account=detail::field(*json,"account");reply.account={detail::text_field(account,"subject"),detail::text_field(account,"email"),detail::text_field(account,"user_id")};
        auto rows=detail::field(*json,"items").get<Json::Array>();if(!rows||rows->get().size()>batch_limit)throw std::runtime_error("Invalid item count");
        for(const auto& row:rows->get()) {
            auto id=detail::text_field(row,"id");
            if(boolean(row,"ok")) {
                auto vm=instance(detail::field(row,"instance"));if(vm.spec.id!=id)throw std::runtime_error("Instance id mismatch");
                GatewayValue value{std::move(vm),detail::text_field(row,"guest"),boolean(row,"removed")};
                if(request.file) {
                    auto file=files_detail::parse_reply(detail::field(row,"file"),*request.file);if(!file)throw std::runtime_error("Invalid Files result");value.file=std::move(*file);
                }else if(row.find("file"))throw std::runtime_error("Unexpected Files result");
                if(request.process) {
                    auto result=process_detail::parse_reply(detail::field(row,"process"),*request.process);if(!result)throw std::runtime_error("Invalid process result");value.process=std::move(*result);
                }else if(row.find("process"))throw std::runtime_error("Unexpected process result");
                reply.items.push_back({id,std::move(value)});
            }else {
                const auto& error=detail::field(row,"error");auto code=bounded(error,"code",static_cast<unsigned>(core::ErrorCode::wrong_executor));
                reply.items.push_back({id,core::fail(static_cast<core::ErrorCode>(code),detail::text_field(error,"message"))});
            }
        }
        if(request.action!=GatewayAction::list) {
            auto ids=request.action==GatewayAction::create?std::vector<std::string>{request.create->id}:request.instances;
            if(ids.size()!=reply.items.size())throw std::runtime_error("Item count mismatch");
            for(std::size_t i=0;i<ids.size();++i)if(ids[i]!=reply.items[i].id)throw std::runtime_error("Item order mismatch");
        }
        return reply;
    }catch(...) {return std::unexpected(GatewayError{GatewayErrorCode::protocol,"Malformed or mismatched gateway reply"});}
}
} // namespace ii::virtualizer::gateway_detail
