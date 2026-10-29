#pragma once
#include "Gateway.h"
#include <variant>

namespace ii::virtualizer {
// Keeps transport/account errors distinct from errors returned by the VM.
using VmError=std::variant<GatewayError,core::Error>;
template<class T>using VmResult=std::expected<T,VmError>;
// Lightweight borrowed view: the Gateway outlives this object. Calls perform
// bounded synchronous RPC; the application owns executor and polling cadence.
class VmClient final {
public:
    VmClient(const Gateway&,std::string runtime,std::string instance);
    [[nodiscard]] VmResult<Instance> describe()const;
    [[nodiscard]] VmResult<Instance> control(Operation,std::chrono::milliseconds timeout=std::chrono::seconds(10))const;
    [[nodiscard]] VmResult<FileReply> files(FileRequest)const;
    [[nodiscard]] VmResult<ProcessReply> processes(ProcessRequest)const;
    [[nodiscard]] VmResult<ProcessSnapshot> launch(ProcessLaunch)const;
    [[nodiscard]] VmResult<ProcessSnapshot> process(std::string id)const;
    [[nodiscard]] VmResult<ProcessSnapshot> stop(std::string id)const;
    [[nodiscard]] VmResult<ProcessSnapshot> write(std::string id,std::uint64_t offset,core::ByteView)const;
    [[nodiscard]] VmResult<ProcessSnapshot> close_input(std::string id)const;
    [[nodiscard]] VmResult<ProcessReply> read(std::string id,ProcessStream,std::uint64_t offset=0,std::size_t length=process_chunk_limit)const;
    [[nodiscard]] VmResult<ProcessReply> list(std::string cursor={},std::size_t limit=process_page_limit)const;
    [[nodiscard]] VmResult<void> release(std::string id)const;
    [[nodiscard]] VmResult<VmMonitor> monitor()const;
private:
    [[nodiscard]] VmResult<ProcessSnapshot> one(ProcessAction,std::string id)const;
    const Gateway& gateway_;std::string runtime_,instance_;
};
}
