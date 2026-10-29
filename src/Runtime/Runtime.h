#pragma once
#include <iiVirtualizer/Types.h>
#include <iiSDKCore/Runtime/Future.h>
#include <memory>

namespace ii::virtualizer {
enum class Operation { describe, start, pause, resume, stop, force_stop };
struct RuntimeOptions final { std::size_t workers=0, capacity=128; };
struct InstanceOutcome final {
    std::string id;
    Result<Instance> result;
};

// One controller owns a bounded iiSDK executor for any number of persistent VMs.
// Different IDs can execute concurrently. Overlapping operations on one ID
// return busy across threads and processes. Destruction retires controller work
// without terminating running VMs; callers must not concurrently destroy it.
class Runtime final {
public:
    Runtime(fs::path root, Backend backend);
    Runtime(fs::path root, Backend backend, RuntimeOptions options);
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    [[nodiscard]] Result<Instance> create(const InstanceSpec&);
    [[nodiscard]] Result<Instance> describe(std::string_view id);
    [[nodiscard]] Result<std::vector<Instance>> list();
    [[nodiscard]] Result<Instance> start(std::string_view id);
    [[nodiscard]] Result<Instance> pause(std::string_view id);
    [[nodiscard]] Result<Instance> resume(std::string_view id);
    [[nodiscard]] Result<Instance> stop(std::string_view id, bool force = false,
        std::chrono::milliseconds timeout = std::chrono::seconds(10));
    [[nodiscard]] Result<void> remove(std::string_view id);
    [[nodiscard]] Result<std::string> guest_info(std::string_view id);
    [[nodiscard]] Result<FileReply> files(std::string_view id,const FileRequest&,
        std::chrono::milliseconds timeout=std::chrono::seconds(10));
    [[nodiscard]] Result<ProcessReply> processes(std::string_view id,const ProcessRequest&,
        std::chrono::milliseconds timeout=std::chrono::seconds(10));
    [[nodiscard]] ii::runtime::Future<Instance> submit(Operation, std::string id,
        std::chrono::milliseconds timeout = std::chrono::seconds(10));
    // Results retain input order and individual failures. A batch is not an
    // atomic transaction; successful VMs keep their resulting state.
    [[nodiscard]] Result<std::vector<InstanceOutcome>> apply(Operation, std::vector<std::string> ids,
        std::chrono::milliseconds timeout = std::chrono::seconds(10));
    [[nodiscard]] Result<std::vector<InstanceOutcome>> snapshot();
    [[nodiscard]] ii::runtime::RuntimeStats execution_stats() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ii::virtualizer
