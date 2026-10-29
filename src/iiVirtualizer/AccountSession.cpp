#include "GatewayWire.h"
#include <iiAccountManager/NativeAccount.h>
#include <iiNetwork/Http.h>
#include <iiNetwork/Wire.h>
#include <iiSDKCore/Runtime/EventLoop.h>

namespace ii::virtualizer {
GatewayVerifier iisacc_account_verifier(IisaccAuthority authority) {
    const std::string suffix="/Account/GraphQL";
    if(!authority.endpoint.starts_with("https://")||!authority.endpoint.ends_with(suffix)||authority.timeout.count()<=0||authority.timeout>std::chrono::seconds(60))
        throw std::invalid_argument("The account authority must use HTTPS /Account/GraphQL with a bounded timeout");
    auto origin=authority.endpoint.substr(0,authority.endpoint.size()-suffix.size());
    GatewayCredential check(origin,"validation","iisacc_auth_id=proof; iisacc_login_session=registry");
    if(!gateway_detail::validate(check))throw std::invalid_argument("Invalid account authority origin");
    return [authority=std::move(authority),origin=std::move(origin)](const GatewayCredential& proof) -> GatewayResult<GatewayIdentity> {
        if(auto valid=gateway_detail::validate(proof);!valid)return std::unexpected(valid.error());
        if(proof.issuer()!=origin)return std::unexpected(GatewayError{GatewayErrorCode::unauthenticated,"Account issuer does not match the configured authority"});
        try {
            ii::runtime::EventLoop loop(32);ii::network::Http http(loop);
            ii::network::HttpRequest request;request.url=authority.endpoint;request.method="POST";
            request.headers={{"Content-Type","application/json"},{"Origin",origin}};
            request.cookie=proof.cookie_header();request.timeout=authority.timeout;request.maximum_bytes=262144;
            request.follow_redirects=false;request.tls.ca_file=authority.ca_file.string();
            request.body=iisacc::accounts::native::graphql_request(
                "query VirtualizerGatewayAccount { accountSession { account { sub email userId } } }",{});
            std::optional<ii::network::HttpResponse> response;
            auto submitted=http.request(std::move(request),[&](ii::network::HttpResponse value){response=std::move(value);loop.request_stop();});
            if(!submitted)return std::unexpected(GatewayError{GatewayErrorCode::unavailable,"Account authentication service could not be reached"});
            loop.run();
            if(!response||!response->error.empty()||response->status>=500||response->status==0)
                return std::unexpected(GatewayError{GatewayErrorCode::unavailable,"Account authentication service is unavailable"});
            auto result=iisacc::accounts::native::graphql_result(response->body,"accountSession",response->status);
            if(!result)return std::unexpected(GatewayError{GatewayErrorCode::unauthenticated,"Account session was not verified"});
            namespace wire=ii::network::wire;
            const auto& account=wire::field(*result,"account");
            GatewayIdentity identity{wire::text(wire::field(account,"sub")),wire::text(wire::field(account,"email")),wire::text(wire::field(account,"userId"))};
            if(identity.subject!=proof.subject()||!gateway_detail::key(identity.subject)||!iisacc::accounts::native::valid_email(identity.email))
                return std::unexpected(GatewayError{GatewayErrorCode::unauthenticated,"Account session is inactive or identifies another account"});
            identity.email=iisacc::accounts::native::normalize_email(identity.email);return identity;
        }catch(...) {return std::unexpected(GatewayError{GatewayErrorCode::unavailable,"Account authentication could not be completed"});}
    };
}
GatewayCredentials secure_account_session(std::string service,std::string key) {
    if(service.empty()||key.empty())throw std::invalid_argument("A secure account entry must be selected explicitly");
    return [service=std::move(service),key=std::move(key)]() -> GatewayResult<GatewayCredential> {
        try {
            iisacc::accounts::native::SecureStore store(service,key);
            if(!store.supported())return std::unexpected(GatewayError{GatewayErrorCode::unavailable,"Native account credential storage is unavailable"});
            auto record=store.read();
            if(record.status!=iisacc::accounts::native::SecretResult::Status::found)
                return std::unexpected(GatewayError{GatewayErrorCode::unauthenticated,"No current account session is available"});
            gateway_detail::SecretText bytes(std::move(record.bytes));
            if(bytes.value.size()>32768)throw std::runtime_error("Invalid account entry");
            auto json=ii::json::parse(bytes.value);if(!json||detail::integer_field(*json,"version")!=1)throw std::runtime_error("Invalid account entry");
            GatewayCredential proof(detail::text_field(*json,"issuer"),detail::text_field(*json,"subject"),detail::text_field(*json,"cookie"));
            if(auto valid=gateway_detail::validate(proof);!valid)return std::unexpected(valid.error());return proof;
        }catch(...) {return std::unexpected(GatewayError{GatewayErrorCode::unauthenticated,"The stored account session is invalid"});}
    };
}
} // namespace ii::virtualizer
