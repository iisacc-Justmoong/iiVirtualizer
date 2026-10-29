#pragma once
#include <iiVirtualizer/Gateway.h>

namespace ii::virtualizer {
struct GatewayImage final { std::string id; InstanceSpec defaults; };
struct GatewayRuntime final {
    std::string id;
    std::reference_wrapper<Runtime> runtime;
    std::vector<GatewayImage> images;
};
struct GatewayGrant final {
    std::string subject,runtime;
    std::vector<GatewayAction> actions;
    std::vector<std::string> instances;
    std::string instance_prefix;
    bool all_instances=false;
    unsigned max_cpus=8,max_memory_mib=8192;
    std::uint64_t max_disk_mib=65536;
};
// Receives a single bounded protocol frame. Authentication and whole-request
// authorization run before any VM operation. Runtimes must outlive this agent.
class GatewayAgent final {
public:
    GatewayAgent(GatewayVerifier,std::vector<GatewayRuntime>,std::vector<GatewayGrant>);
    [[nodiscard]] std::string handle(std::string_view frame) const;
private:
    GatewayVerifier verify_;
    std::vector<GatewayRuntime> runtimes_;
    std::vector<GatewayGrant> grants_;
};
} // namespace ii::virtualizer
