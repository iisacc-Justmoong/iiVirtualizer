#include "../src/guest/Workspace.h"
#include <iiFileProviderStorage.h>
#include <iiSDKCore/Runtime/Runtime.h>
#include <iiSDKCore/Process/Process.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <linux/capability.h>
#include <sched.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace g=ii::virtualizer::guest;
namespace fs=std::filesystem;
static bool cannot_write(const char* path) {
    errno=0;int fd=::open(path,O_WRONLY|O_CREAT|O_CLOEXEC,0600);
    if(fd>=0){::close(fd);return false;}return errno==EACCES||errno==EROFS||errno==ENOENT;
}
int main(int argc,char**) {
    if(argc>1)return (::getuid()==g::workload_uid&&::prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)==1&&cannot_write("/escaped-child"))?0:1;
    unsigned checks=0;std::vector<std::string> failures;
    auto check=[&](bool passed,const char* label){++checks;if(!passed)failures.emplace_back(label);};
    check(::getuid()==g::workload_uid&&::geteuid()==g::workload_uid&&::getgid()==g::workload_gid,"unprivileged_identity");
    check(::getgroups(0,nullptr)==0,"supplementary_groups_cleared");
    check(::prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)==1,"no_new_privileges");
    check(::prctl(PR_GET_SECCOMP,0,0,0,0)==2,"seccomp_filter");
    __user_cap_header_struct header{_LINUX_CAPABILITY_VERSION_3,0};__user_cap_data_struct capabilities[2]{};
    check(::syscall(SYS_capget,&header,capabilities)==0&&!capabilities[0].effective&&!capabilities[0].permitted&&!capabilities[0].inheritable&&
        !capabilities[1].effective&&!capabilities[1].permitted&&!capabilities[1].inheritable,"capabilities_cleared");
    bool descriptors=true;for(int fd=3;fd<256;++fd)if(::fcntl(fd,F_GETFD)>=0)descriptors=false;
    check(descriptors,"inherited_descriptors_closed");
    std::array<char,4096> cwd{};check(::getcwd(cwd.data(),cwd.size())&&std::string(cwd.data())==g::workspace_root,"container_working_directory");
    struct statvfs root{},workspace{};
    check(::statvfs("/",&root)==0&&(root.f_flag&ST_RDONLY),"readonly_runtime_root");
    check(::statvfs(g::workspace_root,&workspace)==0&&!(workspace.f_flag&ST_RDONLY)&&(workspace.f_flag&ST_NOSUID)&&(workspace.f_flag&ST_NODEV),"writable_nosuid_nodev_workspace");
    const std::string boot=std::getenv("IIV_BOOT_COUNT")?std::getenv("IIV_BOOT_COUNT"):"0";
    const auto previous_path=fs::path(g::workspace_root)/"probe-persistence";
    std::string previous;{std::ifstream file(previous_path);file>>previous;}
    check((boot=="1"&&previous.empty())||(boot=="2"&&previous=="1"),"persistent_container_data");
    {iiFileProvider::native::OutputFile file(previous_path);file<<boot<<'\n';file.flush();check(bool(file),"provider_write_inside_container");}
    try {
        const auto directory=fs::path(g::workspace_root)/"probe-files";
        iiFileProvider::native::createDirectories(directory);
        {iiFileProvider::native::OutputFile file(directory/"one");file<<"content";}
        fs::rename(directory/"one",directory/"two");std::string content;std::ifstream(directory/"two")>>content;
        check(content=="content","create_read_rename_inside");
        fs::remove(directory/"alias");fs::create_hard_link(directory/"two",directory/"alias");
        check(fs::equivalent(directory/"two",directory/"alias"),"internal_hard_links");
        fs::remove(directory/"escape");fs::create_symlink("../../../../../../../etc/os-release",directory/"escape");
        check(cannot_write((directory/"escape").c_str()),"relative_symlink_escape_blocked");
        fs::remove(directory/"outside");fs::create_symlink("/init",directory/"outside");
        std::ifstream outside(directory/"outside");check(!outside,"original_guest_root_hidden");
        fs::remove(directory/"deleted");{iiFileProvider::native::OutputFile file(directory/"deleted");file<<"delete";}
        check(fs::remove(directory/"deleted"),"delete_inside");
        const auto temporary=std::getenv("TMPDIR");const auto home=std::getenv("HOME");
        check(temporary&&fs::path(temporary).parent_path()==g::workspace_root&&home&&fs::path(home).parent_path()==g::workspace_root,"home_and_temporary_inside");
        if(temporary){iiFileProvider::native::OutputFile file(fs::path(temporary)/"temporary");file<<"temporary";file.flush();check(bool(file),"temporary_write_inside");}
    }catch(const std::exception&){check(false,"workspace_operations");}
    for(const char* path:{"/escaped","/tmp/escaped","/etc/os-release","/etc/resolv.conf","../../../etc/os-release", "/data/boot-count", "/data/Society.societycontainer/.ii-runtime/boot-count"})
        check(cannot_write(path),path);
    int binary=::open("/app/service",O_WRONLY|O_CLOEXEC);check(binary<0,"executable_write_blocked");if(binary>=0)::close(binary);
    check(::chmod("/etc/os-release",0777)<0,"os_chmod_blocked");
    check(::unlink("/etc/os-release")<0,"os_delete_blocked");
    check(::rename("probe-persistence","/etc/escaped")<0,"outside_rename_blocked");
    check(::link("/etc/os-release","os-alias")<0,"outside_hard_link_blocked");
    for(const char* path:{"/dev/vda","/dev/mem","/proc/1/root/init","/proc/self/fd/3","/sys/kernel"}) {
        int fd=::open(path,O_RDONLY|O_CLOEXEC);check(fd<0,path);if(fd>=0)::close(fd);
    }
    errno=0;check(::mount("tmpfs",".","tmpfs",0,nullptr)<0&&errno==EPERM,"mount_blocked");
    errno=0;check(::umount2("/data/Society.societycontainer",MNT_DETACH)<0&&errno==EPERM,"unmount_blocked");
    errno=0;check(::chroot(".")<0&&errno==EPERM,"chroot_escape_blocked");
    errno=0;check(::unshare(CLONE_NEWUSER|CLONE_NEWNS)<0&&errno==EPERM,"user_namespace_escape_blocked");
    errno=0;check(::syscall(SYS_clone,CLONE_NEWUSER|SIGCHLD,nullptr,0,0,0)<0&&errno==EPERM,"clone_namespace_escape_blocked");
    errno=0;check(::syscall(SYS_clone3,nullptr,0)<0&&errno==ENOSYS,"clone3_fallback");
    errno=0;check(::mknodat(AT_FDCWD,"raw-device",S_IFBLK|0600,0)<0&&errno==EPERM,"device_creation_blocked");
    errno=0;check(::syscall(SYS_fsopen,"tmpfs",0)<0&&errno==EPERM,"new_mount_api_blocked");
    errno=0;check(::syscall(SYS_open_by_handle_at,-1,nullptr,0)<0&&errno==EPERM,"file_handle_escape_blocked");
    errno=0;check(::syscall(SYS_pidfd_getfd,-1,3,0)<0&&errno==EPERM,"foreign_descriptor_import_blocked");
    errno=0;check(::syscall(SYS_ptrace,0,1,0,0)<0&&errno==EPERM,"ptrace_blocked");
    errno=0;check(::ioctl(STDOUT_FILENO,0)<0&&errno==EPERM,"console_ioctl_blocked");
    errno=0;check(::setuid(0)<0&&errno==EPERM,"root_recovery_blocked");
    check(::prctl(PR_SET_NO_NEW_PRIVS,0,0,0,0)<0,"privilege_guard_irreversible");
    std::atomic<unsigned> completed=0;
    {ii::runtime::Runtime workers({.workers=2,.capacity=8});
        for(unsigned i=0;i<4;++i)check(bool(workers.post([&,i]{
            iiFileProvider::native::OutputFile file(fs::path(g::workspace_root)/("thread-"+std::to_string(i)));file<<i;file.flush();
            if(file&&cannot_write("/escaped-thread"))++completed;
        })),"worker_admission");
        check(bool(workers.wait_idle())&&completed==4,"iisdk_workers_inherit_boundary");}
    const auto child=::fork();
    if(child==0){char program[]="/app/service",mode[]="--child";char* arguments[]{program,mode,nullptr};::execv(program,arguments);::_exit(99);}
    int status=0;check(child>0&&::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fork_exec_inherits_boundary");
    ii::process::Launch launch;launch.arguments={"/app/service","--child"};launch.working_directory=g::workspace_root;launch.input=ii::process::Input::closed;
    auto process=ii::process::Process::start(launch);
    if(process){ii::process::CommunicationOptions options;options.deadline=ii::core::Deadline::after(std::chrono::seconds(5));auto result=process->communicate({},options);
        check(result&&result->exit.reason==ii::process::ExitReason::exited&&result->exit.code==0,"iisdk_process_inherits_boundary");}
    else check(false,"iisdk_process_launch");
    std::cout<<"IIV_SANDBOX_TEST {\"passed\":"<<(failures.empty()?"true":"false")<<",\"checks\":"<<checks<<",\"uid\":"<<::getuid()<<",\"boot\":\""<<boot<<"\",\"failed\":[";
    for(std::size_t i=0;i<failures.size();++i){if(i)std::cout<<',';std::cout<<'"'<<failures[i]<<'"';}
    std::cout<<"]}"<<std::endl;
    // This disposable fixture deliberately poisons a required directory after
    // the persistence test. The next boot must refuse to execute the workload.
    if(boot=="2"&&failures.empty()) {
        const auto home=fs::path(g::workspace_root)/".home";
        fs::rename(home,fs::path(g::workspace_root)/".home-before-tamper");fs::create_directory_symlink("/",home);
        std::cout<<"IIV_SANDBOX_TAMPER_TEST completed=1"<<std::endl;
    }
    return failures.empty()?0:1;
}
