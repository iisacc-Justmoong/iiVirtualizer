#include <iiVirtualizer/Types.h>
#include <iiVirtualizer/Runtime.h>
#include <iiVirtualizer/Qemu.h>
#include <iiVirtualizer/GuestProtocol.h>
#include <iostream>

int main(int argc,char** argv) {
    if(argc!=2)return 3;
    const std::string uuid="00000000-0000-4000-8000-000000000001";
    auto encoded=ii::virtualizer::guest_message(uuid,ii::virtualizer::GuestCommand::ping);
    encoded.pop_back();
    auto request=ii::virtualizer::guest_request(encoded,uuid);
    if(!request||*request!=ii::virtualizer::GuestCommand::ping)return 1;
    ii::virtualizer::InstanceSpec spec;
    spec.id="consumer";spec.kernel="kernel";spec.initramfs="initramfs";
    if(!ii::virtualizer::validate(spec))return 2;
    ii::virtualizer::Runtime runtime(argv[1],ii::virtualizer::qemu_backend());
    auto inventory=runtime.snapshot();
    if(!inventory||!inventory->empty())return 4;
    auto future=runtime.submit(ii::virtualizer::Operation::describe,"missing");
    auto missing=future.get();if(missing||missing.error().code!=ii::core::ErrorCode::not_found)return 5;
    if(runtime.execution_stats().workers==0)return 6;
    std::cout<<ii::virtualizer::name(ii::virtualizer::host_architecture())<<" installed consumer passed\n";
}
