#include "WorkloadSandbox.h"
#include <cstdlib>
#include <iostream>
#include <unistd.h>

int main(int argc,char**) {
    try {
        if(argc!=1||::getppid()!=1)throw std::runtime_error("ii-workload is a PID 1 boot helper");
        namespace g=ii::virtualizer::guest;
        const auto workspace=g::workspace_paths();
        const std::string boot=std::getenv("IIV_BOOT_COUNT")?std::getenv("IIV_BOOT_COUNT"):"0";
        g::confine_workload({.workspace=workspace});
        if(::clearenv()!=0||::setenv("PATH","/app",1)!=0||::setenv("HOME",workspace.home.c_str(),1)!=0||
            ::setenv("TMPDIR",workspace.temporary.c_str(),1)!=0||::setenv("IIV_CONTAINER_ROOT",g::container_root,1)!=0||
            ::setenv("IIV_FILESYSTEM_PROFILE",g::filesystem_profile,1)!=0||::setenv("IIV_BOOT_COUNT",boot.c_str(),1)!=0)
            throw std::runtime_error("Cannot prepare the workload environment");
        std::cout<<"IIVIRTUALIZER_SANDBOX_READY profile="<<g::filesystem_profile<<" uid="<<::getuid()<<" container="<<g::container_root<<" root_readonly=1 seccomp=1"<<std::endl;
        char executable[]="/app/service";char* arguments[]{executable,nullptr};
        ::execv(executable,arguments);throw std::runtime_error("Cannot execute the confined workload");
    }catch(const std::exception& error){std::cerr<<"ii-workload: blocked: "<<error.what()<<std::endl;return 125;}
}
