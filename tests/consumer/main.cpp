#include <iiVirtualizer/Types.h>
#include <iiVirtualizer/Runtime.h>
#include <iiVirtualizer/Qemu.h>
#include <iiVirtualizer/GuestProtocol.h>
#include <iiVirtualizer/Files.h>
#include <iiVirtualizer/Processes.h>
#if IIVIRTUALIZER_HAS_GATEWAY
#include <iiVirtualizer/Gateway.h>
#include <iiVirtualizer/GatewayAgent.h>
#include <iiVirtualizer/VmClient.h>
#endif
#include <iostream>
#include <algorithm>

int main(int argc,char** argv) {
    if(argc!=2)return 3;
    const std::string uuid="00000000-0000-4000-8000-000000000001";
    auto encoded=ii::virtualizer::guest_message(uuid,ii::virtualizer::GuestCommand::ping);
    encoded.pop_back();
    auto request=ii::virtualizer::guest_request(encoded,uuid);
    if(!request||*request!=ii::virtualizer::GuestCommand::ping)return 1;
    ii::virtualizer::InstanceSpec spec;
    spec.id="consumer";spec.kernel="kernel";spec.initramfs="initramfs";
    spec.architecture=ii::virtualizer::host_architecture();
    spec.kernel_command_line="console=ttyAMA0 root=/dev/vda rw";
    spec.graphics=ii::virtualizer::Graphics::virtio_gpu;
    if(!ii::virtualizer::validate(spec))return 2;
    ii::virtualizer::Instance probe{spec,uuid,ii::virtualizer::paths_for(std::filesystem::absolute(argv[1]),spec.id),ii::virtualizer::State::stopped};
    auto plan=ii::virtualizer::qemu_arguments(probe);
    if(!plan||std::find(plan->begin(),plan->end(),spec.kernel_command_line)==plan->end()||
       std::find(plan->begin(),plan->end(),"virtio-gpu-pci")==plan->end())return 13;
    auto disk=probe;disk.spec.architecture=ii::virtualizer::Architecture::x86_64;
    disk.spec.boot_mode=ii::virtualizer::BootMode::disk;disk.spec.kernel.clear();disk.spec.initramfs.clear();disk.spec.kernel_command_line.clear();
    disk.spec.disk_bus=ii::virtualizer::DiskBus::sata;disk.spec.guest_channel=ii::virtualizer::GuestChannel::serial;
    disk.spec.graphics=ii::virtualizer::Graphics::vga;disk.spec.boot_iso="installer.iso";disk.spec.allow_reboot=true;
    auto disk_plan=ii::virtualizer::qemu_arguments(disk);
    if(!disk_plan||std::find(disk_plan->begin(),disk_plan->end(),"-kernel")!=disk_plan->end()||
       std::find(disk_plan->begin(),disk_plan->end(),"ide-hd,drive=data,bus=ide.0")==disk_plan->end())return 14;
    ii::virtualizer::Runtime runtime(argv[1],ii::virtualizer::qemu_backend());
    auto inventory=runtime.snapshot();
    if(!inventory||!inventory->empty())return 4;
    auto future=runtime.submit(ii::virtualizer::Operation::describe,"missing");
    auto missing=future.get();if(missing||missing.error().code!=ii::core::ErrorCode::not_found)return 5;
    if(runtime.execution_stats().workers==0)return 6;
    ii::virtualizer::FileRequest file;file.action=ii::virtualizer::FileAction::read;file.path="notes.txt";
    auto missing_file=runtime.files("missing",file);if(missing_file||missing_file.error().code!=ii::core::ErrorCode::not_found)return 9;
    ii::virtualizer::ProcessRequest process_request;auto missing_process=runtime.processes("missing",process_request);if(missing_process||missing_process.error().code!=ii::core::ErrorCode::not_found)return 11;
#if IIVIRTUALIZER_HAS_GATEWAY
    ii::virtualizer::SshGatewayEndpoint endpoint;endpoint.host="localhost";endpoint.user="consumer";endpoint.known_hosts=std::filesystem::absolute(argv[1])/"known_hosts";
    auto arguments=ii::virtualizer::ssh_gateway_arguments(endpoint);if(!arguments)return 7;
    auto verifier=ii::virtualizer::iisacc_account_verifier();
    ii::virtualizer::GatewayAgent agent(std::move(verifier),{{"consumer",runtime,{}}},{});
    if(agent.handle("{}").empty())return 8;
    ii::virtualizer::Gateway files_client([]()->ii::virtualizer::GatewayResult<ii::virtualizer::GatewayCredential>{return ii::virtualizer::GatewayCredential("https://iisacc.test","consumer","iisacc_auth_id=proof; iisacc_login_session=fixture");},
        [](std::string_view)->ii::virtualizer::GatewayResult<std::string>{return std::unexpected(ii::virtualizer::GatewayError{ii::virtualizer::GatewayErrorCode::transport,"installed transport fixture"});});
    ii::virtualizer::VmClient vm(files_client,"consumer","missing");auto monitored=vm.monitor();if(monitored||!std::holds_alternative<ii::virtualizer::GatewayError>(monitored.error())||std::get<ii::virtualizer::GatewayError>(monitored.error()).code!=ii::virtualizer::GatewayErrorCode::transport)return 12;
    auto remote=files_client.files("consumer","missing",file);if(remote||remote.error().code!=ii::virtualizer::GatewayErrorCode::transport)return 10;
#endif
    std::cout<<ii::virtualizer::name(ii::virtualizer::host_architecture())<<" installed consumer passed\n";
}
