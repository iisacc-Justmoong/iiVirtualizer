#pragma once
#include <iiVirtualizer/Types.h>
#include <iiJson/Json.h>

namespace ii::virtualizer::detail {
using Json = ii::json::Value;
[[nodiscard]] std::string json_text(const Json&);
[[nodiscard]] const Json& field(const Json&, std::string_view);
[[nodiscard]] std::string text_field(const Json&, std::string_view);
[[nodiscard]] std::uint64_t integer_field(const Json&, std::string_view);
[[nodiscard]] Json instance_json(const Instance&);
[[nodiscard]] bool valid_uuid(std::string_view);

class Store final {
public:
    explicit Store(fs::path root);
    class Lock final {
    public:
        Lock(Store&, std::string_view id);
        ~Lock();
        Lock(const Lock&)=delete;
        Lock& operator=(const Lock&)=delete;
    private:
        std::intptr_t handle_=-1;
    };
    fs::path root;
    [[nodiscard]] Result<std::vector<std::string>> ids() const;
    [[nodiscard]] Result<Instance> load(std::string_view id) const;
    [[nodiscard]] Result<Instance> create(const InstanceSpec&);
    [[nodiscard]] Result<void> erase(const Instance&);
};
} // namespace ii::virtualizer::detail
