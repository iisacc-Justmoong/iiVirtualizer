#include "Store.h"
#include <array>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <system_error>
#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ii::virtualizer::detail {
std::string json_text(const Json& value) {
    auto result=ii::json::stringify(value);
    if (!result) throw std::runtime_error(result.error().message);
    return *result;
}
const Json& field(const Json& value, std::string_view key) {
    auto result=value.find(key);
    if (!result) throw std::runtime_error("Invalid JSON field: "+std::string(key));
    return result->get();
}
std::string text_field(const Json& value, std::string_view key) {
    auto result=field(value,key).get<std::string>();
    if (!result) throw std::runtime_error("Expected text: "+std::string(key));
    return result->get();
}
std::uint64_t integer_field(const Json& value, std::string_view key) {
    const auto& item=field(value,key);
    if (auto p=item.get<std::uint64_t>()) return p->get();
    if (auto p=item.get<std::int64_t>(); p && p->get()>=0) return static_cast<std::uint64_t>(p->get());
    throw std::runtime_error("Expected nonnegative integer: "+std::string(key));
}
Json instance_json(const Instance& vm) {
    Json::Array forwards;
    for (auto p:vm.spec.forwards) forwards.emplace_back(Json::Object{{"host",Json(p.host)},{"guest",Json(p.guest)}});
    return Json(Json::Object{{"schema",Json(1)}, {"id",Json(vm.spec.id)}, {"uuid",Json(vm.uuid)},
        {"architecture",Json(name(vm.spec.architecture))},{"accelerator",Json(name(vm.spec.accelerator))},
        {"cpus",Json(vm.spec.cpus)},{"memory_mib",Json(vm.spec.memory_mib)},{"disk_mib",Json(vm.spec.disk_mib)},
        {"kernel",Json(vm.spec.kernel.string())},{"initramfs",Json(vm.spec.initramfs.string())},
        {"initialize_disk",Json(vm.spec.initialize_disk)},
        {"network",Json(vm.spec.network)},{"forwards",Json(std::move(forwards))},
        {"state",Json(name(vm.state))},{"directory",Json(vm.paths.directory.string())},
        {"console",Json(vm.paths.console.string())}});
}
bool valid_uuid(std::string_view uuid) {
    if (uuid.size()!=36) return false;
    for (std::size_t i=0;i<uuid.size();++i) {
        if (i==8||i==13||i==18||i==23) { if(uuid[i]!='-') return false; }
        else if (!((uuid[i]>='0'&&uuid[i]<='9')||(uuid[i]>='a'&&uuid[i]<='f'))) return false;
    }
    return true;
}
static std::string new_uuid() {
    std::random_device random;
    std::array<unsigned char,16> bytes{};
    for(auto& b:bytes) b=static_cast<unsigned char>(random());
    bytes[6]=(bytes[6]&15)|64; bytes[8]=(bytes[8]&63)|128;
    std::ostringstream out;
    for(std::size_t i=0;i<bytes.size();++i) {
        if (i==4||i==6||i==8||i==10) out<<'-';
        out<<std::hex<<std::setw(2)<<std::setfill('0')<<static_cast<unsigned>(bytes[i]);
    }
    return out.str();
}
Store::Store(fs::path path) {
    const bool created=fs::create_directories(path);
    root=fs::canonical(path);
    if(created)fs::permissions(root,fs::perms::owner_all,fs::perm_options::replace);
}
Store::Lock::Lock(Store& store,std::string_view id) {
    InstanceSpec key;key.id=id;key.kernel="kernel";key.initramfs="initramfs";
    if(auto checked=validate(key);!checked)throw std::invalid_argument(checked.error().message);
#if defined(__unix__) || defined(__APPLE__)
    // A stable lock file outside the VM directory survives remove/recreate.
    // Never wait here: repeated requests for one busy VM must not occupy all
    // executor workers and starve otherwise unrelated instances.
    int fd=::open((store.root/(".vm-"+std::string(id)+".lock")).c_str(),O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0) throw std::system_error(errno,std::generic_category(),"open store lock");
    handle_=fd;
    if(::flock(fd,LOCK_EX|LOCK_NB)!=0) { int e=errno;::close(fd);handle_=-1;throw std::system_error(e,std::generic_category(),"instance operation is busy"); }
#else
    throw std::runtime_error("Persistent host control currently requires macOS or Linux");
#endif
}
Result<std::vector<std::string>> Store::ids() const {
    std::vector<std::string> result;
    for(const auto& entry:fs::directory_iterator(root)) {
        auto id=entry.path().filename().string();
        if(id.empty()||id.front()=='.')continue;
        std::error_code error;
        if(entry.is_directory(error))result.push_back(std::move(id));
        else if(error&&error!=std::errc::no_such_file_or_directory)
            return core::fail(core::ErrorCode::failed,"Cannot enumerate instance: "+error.message());
    }
    std::sort(result.begin(),result.end());return result;
}
Store::Lock::~Lock() {
#if defined(__unix__) || defined(__APPLE__)
    if(handle_>=0) { ::flock(static_cast<int>(handle_),LOCK_UN);::close(static_cast<int>(handle_)); }
#endif
}
Result<Instance> Store::load(std::string_view id) const {
    InstanceSpec check; check.id=id;check.kernel="kernel";check.initramfs="initramfs";
    if (auto v=validate(check);!v) return std::unexpected(v.error());
    const auto paths=paths_for(root,id);
    if(!fs::exists(paths.directory)) return core::fail(core::ErrorCode::not_found,"Instance not found: "+std::string(id));
    if(fs::is_symlink(paths.directory)||fs::is_symlink(paths.manifest)) return core::fail(core::ErrorCode::invalid_argument,"Instance paths must not be symlinks");
    if(fs::file_size(paths.manifest)>65536) return core::fail(core::ErrorCode::invalid_argument,"Manifest exceeds 64 KiB");
    std::ifstream stream(paths.manifest,std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(stream)),{});
    auto json=ii::json::parse(content);
    if(!json) return core::fail(core::ErrorCode::invalid_argument,"Invalid instance manifest: "+json.error().message);
    if(integer_field(*json,"schema")!=1 || text_field(*json,"id")!=id) return core::fail(core::ErrorCode::invalid_argument,"Manifest schema or identity mismatch");
    Instance vm;vm.paths=paths;vm.spec.id=id;vm.uuid=text_field(*json,"uuid");
    if(!valid_uuid(vm.uuid)) return core::fail(core::ErrorCode::invalid_argument,"Invalid instance UUID");
    auto arch=text_field(*json,"architecture"),accel=text_field(*json,"accelerator");
    if(arch!="aarch64"&&arch!="x86_64") return core::fail(core::ErrorCode::invalid_argument,"Invalid architecture");
    vm.spec.architecture=arch=="aarch64"?Architecture::aarch64:Architecture::x86_64;
    if(accel=="auto") vm.spec.accelerator=Accelerator::automatic;
    else if(accel=="hvf") vm.spec.accelerator=Accelerator::hvf;
    else if(accel=="kvm") vm.spec.accelerator=Accelerator::kvm;
    else if(accel=="tcg") vm.spec.accelerator=Accelerator::tcg;
    else return core::fail(core::ErrorCode::invalid_argument,"Invalid accelerator");
    const auto cpus=integer_field(*json,"cpus"), memory=integer_field(*json,"memory_mib");
    if(cpus>256 || memory>1048576) return core::fail(core::ErrorCode::invalid_argument,"Invalid resource size");
    vm.spec.cpus=static_cast<unsigned>(cpus);vm.spec.memory_mib=static_cast<unsigned>(memory);
    vm.spec.disk_mib=integer_field(*json,"disk_mib");
    // Boot and disk paths are derived from owned storage, never trusted from JSON.
    vm.spec.kernel=paths.directory/"kernel";vm.spec.initramfs=paths.directory/"initramfs";
    auto network=field(*json,"network").get<bool>();
    if(!network) return core::fail(core::ErrorCode::invalid_argument,"Invalid network policy");
    vm.spec.network=network->get();
    auto initialize=field(*json,"initialize_disk").get<bool>();
    if(!initialize)return core::fail(core::ErrorCode::invalid_argument,"Invalid disk initialization policy");
    vm.spec.initialize_disk=initialize->get();
    auto ports=field(*json,"forwards").get<Json::Array>();
    if(!ports) return core::fail(core::ErrorCode::invalid_argument,"Invalid forwarding policy");
    for(const auto& port:ports->get()) {
        auto host=integer_field(port,"host"),guest=integer_field(port,"guest");
        if(host>65535||guest>65535) return core::fail(core::ErrorCode::invalid_argument,"Invalid forwarding port");
        vm.spec.forwards.push_back({static_cast<unsigned>(host),static_cast<unsigned>(guest)});
    }
    if(auto v=validate(vm.spec);!v) return std::unexpected(v.error());
    return vm;
}
Result<Instance> Store::create(const InstanceSpec& input) {
    if(auto v=validate(input);!v) return std::unexpected(v.error());
    if(!fs::is_regular_file(input.kernel)||!fs::is_regular_file(input.initramfs)) return core::fail(core::ErrorCode::not_found,"Kernel or initramfs is not a regular file");
    if(!input.base_disk.empty() && (!fs::is_regular_file(input.base_disk)||fs::file_size(input.base_disk)>input.disk_mib*1024*1024))
        return core::fail(core::ErrorCode::invalid_argument,"Base disk must be a raw file no larger than the requested volume");
    auto paths=paths_for(root,input.id);
    if(fs::exists(paths.directory)||fs::is_symlink(paths.directory)) return core::fail(core::ErrorCode::already_exists,"Instance already exists");
    // Build a complete directory before publishing its name. A failed creation
    // never exposes a partial manifest as a usable instance.
    auto staging=root/(".creating-"+new_uuid());
    fs::create_directory(staging);
    fs::permissions(staging,fs::perms::owner_all,fs::perm_options::replace);
    try {
        Instance vm{input,new_uuid(),paths,State::stopped};
        vm.spec.kernel=paths.directory/"kernel";vm.spec.initramfs=paths.directory/"initramfs";
        vm.spec.base_disk.clear();
        vm.spec.initialize_disk=input.base_disk.empty()&&input.initialize_disk;
        fs::copy_file(input.kernel,staging/"kernel");fs::copy_file(input.initramfs,staging/"initramfs");
        if(input.base_disk.empty()) { std::ofstream file(staging/"data.raw",std::ios::binary);if(!file) throw std::runtime_error("Cannot create volume"); }
        else fs::copy_file(input.base_disk,staging/"data.raw");
        fs::resize_file(staging/"data.raw",input.disk_mib*1024*1024);
        auto manifest=json_text(instance_json(vm))+"\n";
        { std::ofstream out(staging/"instance.json",std::ios::binary);out<<manifest;out.flush();if(!out) throw std::runtime_error("Cannot write manifest"); }
#if defined(__unix__) || defined(__APPLE__)
        int fd=::open((staging/"instance.json").c_str(),O_RDONLY|O_CLOEXEC);
        if(fd<0) throw std::system_error(errno,std::generic_category(),"open manifest for flush");
        int sync=::fsync(fd);int error=errno;::close(fd);
        if(sync!=0) throw std::system_error(error,std::generic_category(),"flush manifest");
#endif
        fs::rename(staging,paths.directory);
        return vm;
    } catch(...) { std::error_code ignored;fs::remove_all(staging,ignored);throw; }
}
Result<void> Store::erase(const Instance& vm) {
    auto current=load(vm.spec.id);
    if(!current) return std::unexpected(current.error());
    if(current->uuid!=vm.uuid) return core::fail(core::ErrorCode::invalid_argument,"Instance identity changed");
    fs::remove_all(current->paths.directory);
    return {};
}
} // namespace ii::virtualizer::detail
