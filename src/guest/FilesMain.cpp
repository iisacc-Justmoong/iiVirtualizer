#include "FilesStore.h"
#include "WorkloadSandbox.h"
#include "../Files/FilesWire.h"
#include <cstdlib>
#include <iostream>
#include <sys/prctl.h>
#include <unistd.h>

int main(int argc,char**) {
    namespace v=ii::virtualizer;namespace g=v::guest;namespace wire=v::files_detail;
    std::optional<wire::Envelope> request;
    try {
        if(argc!=1||::getppid()!=1||::geteuid()!=0)throw std::runtime_error("Files worker requires the trusted boot supervisor");
        const std::string uuid=std::getenv("IIV_INSTANCE_UUID")?std::getenv("IIV_INSTANCE_UUID"):"";
        std::string input;char byte;
        while(std::cin.get(byte)){if(input.size()==wire::frame_limit)throw std::runtime_error("Files input limit");input+=byte;}
        auto decoded=wire::decode_request(input,uuid);if(!decoded)throw std::runtime_error("Invalid Files input");request=std::move(*decoded);
        g::confine_workload({.runtime_root="/run/iiv-files",.executable="/sbin/ii-files",.workspace=g::workspace_paths(),.require_home=false});
        if(::getuid()!=g::workload_uid||::geteuid()!=g::workload_uid||::getgid()!=g::workload_gid||
            ::prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)!=1||::prctl(PR_GET_SECCOMP,0,0,0,0)!=2)
            throw std::runtime_error("Files worker confinement was not retained");
        if(::clearenv()!=0)throw std::runtime_error("Cannot clear Files worker environment");
        // All content operations happen after root isolation, uid/capability
        // removal and syscall filtering, including direct POSIX file calls.
        g::FilesStore store(g::workspace_root);
        std::cout<<wire::encode_reply(uuid,request->id,request->request.action,store.execute(request->request));return 0;
    }catch(...) {
        if(request) {
            try{std::cout<<wire::encode_reply(request->uuid,request->id,request->request.action,ii::core::fail(ii::core::ErrorCode::failed,"Confined Files worker could not complete the operation; inspect state before retrying"));return 0;}catch(...){}
        }
        std::cerr<<"ii-files: request blocked\n";return 125;
    }
}
