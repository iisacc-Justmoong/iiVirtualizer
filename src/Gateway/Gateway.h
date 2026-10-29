#pragma once
#include <iiVirtualizer/Runtime.h>
#include <iiSDKCore/Core/Value/Value.h>
#include <functional>
#include <span>

namespace ii::virtualizer {
enum class GatewayErrorCode { invalid_request, unauthenticated, forbidden, not_found,
    transport, timed_out, protocol, unavailable };
struct GatewayError final { GatewayErrorCode code; std::string message; };
template<class T> using GatewayResult=std::expected<T,GatewayError>;
enum class GatewayAction { list, create, describe, start, pause, resume, stop, force_stop, remove, guest_info,
    file_list,file_stat,file_read,file_create,file_write,file_mkdir,file_move,file_remove,process_launch,process_list,process_stat,process_read,process_write,process_close_input,process_stop,process_release,monitor };
[[nodiscard]] std::string_view name(GatewayAction);
[[nodiscard]] GatewayResult<GatewayAction> gateway_action(std::string_view);
[[nodiscard]] std::optional<FileAction> gateway_file_action(GatewayAction);
[[nodiscard]] std::optional<ProcessAction> gateway_process_action(GatewayAction);
struct GatewayIdentity final { std::string subject, email, user_id; };

// An account session is proof input, never an authenticated display snapshot.
// Each request is verified by the receiving gateway's configured authority.
class GatewayCredential final {
public:
    GatewayCredential(std::string issuer,std::string subject,std::string cookie_header);
    ~GatewayCredential();
    GatewayCredential(GatewayCredential&&) noexcept;
    GatewayCredential& operator=(GatewayCredential&&) noexcept;
    GatewayCredential(const GatewayCredential&)=delete;
    GatewayCredential& operator=(const GatewayCredential&)=delete;
    [[nodiscard]] const std::string& issuer() const {return issuer_;}
    [[nodiscard]] const std::string& subject() const {return subject_;}
    [[nodiscard]] std::string_view cookie_header() const;
private:
    std::string issuer_,subject_;
    std::vector<unsigned char> cookie_;
};
using GatewayCredentials=std::function<GatewayResult<GatewayCredential>()>;
using GatewayVerifier=std::function<GatewayResult<GatewayIdentity>(const GatewayCredential&)>;
struct IisaccAuthority final {
    std::string endpoint="https://iisacc.com/Account/GraphQL";
    fs::path ca_file;
    std::chrono::milliseconds timeout=std::chrono::seconds(10);
};
[[nodiscard]] GatewayVerifier iisacc_account_verifier(IisaccAuthority = {});
// Reads an explicitly selected iiAccountManager Native SecureStore entry. This
// does not scrape browsers, read unrelated accounts or refresh credentials.
[[nodiscard]] GatewayCredentials secure_account_session(std::string service,std::string key);

struct GatewayCreate final {
    std::string id,image;
    unsigned cpus=1,memory_mib=512;
    std::uint64_t disk_mib=128;
    bool network=false;
    std::vector<PortForward> forwards;
};
struct GatewayRequest final {
    std::string request_id,runtime;
    GatewayAction action=GatewayAction::list;
    std::vector<std::string> instances;
    std::optional<GatewayCreate> create;
    std::chrono::milliseconds timeout=std::chrono::seconds(10);
    std::optional<FileRequest> file=std::nullopt;
    std::optional<ProcessRequest> process=std::nullopt;
};
struct GatewayValue final { Instance instance; std::string guest; bool removed=false;std::optional<FileReply> file=std::nullopt;std::optional<ProcessReply> process=std::nullopt; };
struct GatewayItem final { std::string id; Result<GatewayValue> result; };
struct GatewayReply final {
    std::string request_id,runtime;
    GatewayIdentity account;
    std::vector<GatewayItem> items;
};
// Transport receives a bounded secret frame over a borrowed view. Implementors
// must not log/persist it. The built-in transport sends it only on SSH stdin.
using GatewayTransport=std::function<GatewayResult<std::string>(std::string_view)>;
struct SshGatewayEndpoint final {
    std::string host,user;
    unsigned port=22;
    fs::path known_hosts,identity_file;
    std::string subsystem="iivirtualizer";
    fs::path ssh_executable="ssh";
    std::chrono::milliseconds timeout=std::chrono::seconds(120);
    unsigned connect_timeout_seconds=10;
};
[[nodiscard]] GatewayResult<std::vector<std::string>> ssh_gateway_arguments(const SshGatewayEndpoint&);
[[nodiscard]] GatewayTransport ssh_gateway_transport(SshGatewayEndpoint);

class Gateway final {
public:
    Gateway(GatewayCredentials,GatewayTransport);
    [[nodiscard]] GatewayResult<GatewayReply> execute(GatewayRequest) const;
    [[nodiscard]] GatewayResult<GatewayReply> files(std::string runtime,std::string instance,FileRequest,
        std::chrono::milliseconds timeout=std::chrono::seconds(10)) const;
    [[nodiscard]] GatewayResult<GatewayReply> processes(std::string runtime,std::string instance,ProcessRequest,
        std::chrono::milliseconds timeout=std::chrono::seconds(10)) const;
private:
    GatewayCredentials credentials_;
    GatewayTransport transport_;
};
} // namespace ii::virtualizer
