#include "WorkloadSandbox.h"
#include <DirectoryStorage.h>
#include <iiFileProviderStorage.h>
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <grp.h>
#include <linux/audit.h>
#include <linux/capability.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/securebits.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace ii::virtualizer::guest {
namespace fs=std::filesystem;
namespace {
void require(int result,const char* operation) {if(result<0)throw std::system_error(errno,std::generic_category(),operation);}
void direct(const fs::path& path) {
    if(!path.is_absolute()||!fs::is_directory(fs::symlink_status(path))||fs::canonical(path)!=path)
        throw std::runtime_error("Sandbox directories must be canonical and direct");
}
void readonly_file(const fs::path& source,const fs::path& target) {
    if(!fs::is_regular_file(fs::symlink_status(source))||fs::canonical(source)!=source)
        throw std::runtime_error("Sandbox executable/runtime files must be direct regular files");
    {iiFileProvider::native::OutputFile output(target,std::ios::binary);if(!output)throw std::runtime_error("Cannot prepare a runtime file");}
    require(::mount(source.c_str(),target.c_str(),nullptr,MS_BIND,nullptr),"bind runtime file");
    require(::mount(nullptr,target.c_str(),nullptr,MS_BIND|MS_REMOUNT|MS_RDONLY|MS_NOSUID|MS_NODEV,nullptr),"seal runtime file");
}
void safe_null(const fs::path& target) {
    struct stat info{};
    require(::lstat("/dev/null",&info),"inspect null device");
    if(!S_ISCHR(info.st_mode)||info.st_rdev!=::makedev(1,3))throw std::runtime_error("Invalid null device");
    {iiFileProvider::native::OutputFile output(target,std::ios::binary);if(!output)throw std::runtime_error("Cannot prepare null endpoint");}
    require(::mount("/dev/null",target.c_str(),nullptr,MS_BIND,nullptr),"bind null endpoint");
    require(::mount(nullptr,target.c_str(),nullptr,MS_BIND|MS_REMOUNT|MS_NOSUID|MS_NOEXEC,nullptr),"restrict null endpoint");
}
void drop_privileges() {
    require(::prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0),"no_new_privs");
    require(::prctl(PR_CAP_AMBIENT,PR_CAP_AMBIENT_CLEAR_ALL,0,0,0),"clear ambient capabilities");
    for(unsigned capability=0;capability<64;++capability) {
        if(::prctl(PR_CAPBSET_DROP,capability,0,0,0)<0&&errno!=EINVAL)require(-1,"drop capability bounding set");
    }
    require(::prctl(PR_SET_SECUREBITS,SECBIT_NOROOT|SECBIT_NOROOT_LOCKED|SECBIT_NO_SETUID_FIXUP|SECBIT_NO_SETUID_FIXUP_LOCKED,0,0,0),"lock privilege inheritance");
    require(::setgroups(0,nullptr),"clear supplementary groups");
    require(::setresgid(workload_gid,workload_gid,workload_gid),"set workload group");
    require(::setresuid(workload_uid,workload_uid,workload_uid),"set workload user");
    __user_cap_header_struct header{_LINUX_CAPABILITY_VERSION_3,0};__user_cap_data_struct data[2]{};
    require(static_cast<int>(::syscall(SYS_capset,&header,data)),"clear process capabilities");
    require(::prctl(PR_SET_DUMPABLE,0,0,0,0),"disable process dumps");
    const rlimit core{0,0};require(::setrlimit(RLIMIT_CORE,&core),"disable core files");
}
void syscall_filter() {
#if defined(__aarch64__)
    constexpr unsigned architecture=AUDIT_ARCH_AARCH64;
#elif defined(__x86_64__)
    constexpr unsigned architecture=AUDIT_ARCH_X86_64;
#else
#error The workload syscall policy needs an explicit architecture audit.
#endif
    std::vector<sock_filter> filter{
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(seccomp_data,arch)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,architecture,1,0),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(seccomp_data,nr))};
#if defined(__x86_64__)
    filter.push_back(BPF_JUMP(BPF_JMP|BPF_JSET|BPF_K,0x40000000,0,1));
    filter.push_back(BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS));
#endif
    auto deny=[&](unsigned syscall,int error=EPERM) {
        filter.push_back(BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,syscall,0,1));
        filter.push_back(BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|static_cast<unsigned>(error)));
    };
    // clone3 has an indirect flags structure; ENOSYS allows libc to fall back
    // to clone, whose namespace flags are checked without dereferencing memory.
    deny(SYS_clone3,ENOSYS);
    constexpr unsigned namespaces=CLONE_NEWNS|CLONE_NEWCGROUP|CLONE_NEWUTS|CLONE_NEWIPC|CLONE_NEWUSER|CLONE_NEWPID|CLONE_NEWNET|0x80;
    filter.push_back(BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_clone,0,3));
    filter.push_back(BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(seccomp_data,args[0])));
    filter.push_back(BPF_JUMP(BPF_JMP|BPF_JSET|BPF_K,namespaces,0,1));
    filter.push_back(BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM));
    filter.push_back(BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(seccomp_data,nr)));
    for(unsigned call:{SYS_mount,SYS_umount2,SYS_pivot_root,SYS_chroot,SYS_unshare,SYS_setns,
        SYS_open_tree,SYS_move_mount,SYS_fsopen,SYS_fsconfig,SYS_fsmount,SYS_fspick,SYS_mount_setattr,
        SYS_open_by_handle_at,SYS_name_to_handle_at,SYS_mknodat,SYS_ptrace,SYS_process_vm_readv,SYS_process_vm_writev,SYS_pidfd_getfd,
        SYS_init_module,SYS_finit_module,SYS_delete_module,SYS_reboot,SYS_kexec_load,SYS_kexec_file_load,
        SYS_swapon,SYS_swapoff,SYS_syslog,SYS_acct,SYS_bpf,SYS_perf_event_open,SYS_io_uring_setup,SYS_ioctl})deny(call);
#ifdef SYS_mknod
    deny(SYS_mknod);
#endif
#ifdef SYS_iopl
    deny(SYS_iopl);
#endif
#ifdef SYS_ioperm
    deny(SYS_ioperm);
#endif
    filter.push_back(BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW));
    sock_fprog program{static_cast<unsigned short>(filter.size()),filter.data()};
    require(::prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&program),"install workload syscall policy");
}
}
void confine_workload(const WorkloadSandbox& policy) {
    if(::geteuid()!=0)throw std::runtime_error("The sandbox launcher requires trusted boot privileges");
    const auto& workspace=policy.workspace;
    if(workspace.container!=container_root||workspace.files!=workspace_root||
        !iiSocietyContainer::DirectoryStorage::isPackageDirectory(workspace.container))
        throw std::runtime_error("The workload must use the prepared Society package");
    direct(workspace.files);direct(workspace.state);
    if(policy.require_home){direct(workspace.home);direct(workspace.temporary);}
    require(::unshare(CLONE_NEWNS),"create workload mount namespace");
    require(::mount(nullptr,"/",nullptr,MS_REC|MS_PRIVATE,nullptr),"make mount propagation private");
    if(fs::symlink_status(policy.runtime_root).type()==fs::file_type::not_found)
        iiFileProvider::native::createDirectory(policy.runtime_root);
    direct(policy.runtime_root);
    require(::mount("tmpfs",policy.runtime_root.c_str(),"tmpfs",MS_NOSUID|MS_NODEV,"mode=755,size=4m"),"create private runtime root");
    const auto& root=policy.runtime_root;
    iiFileProvider::native::createDirectories(root/"app");iiFileProvider::native::createDirectories(root/"etc");
    iiFileProvider::native::createDirectories(root/"dev");safe_null(root/"dev/null");
    iiFileProvider::native::createDirectories(root/"data/Society.societycontainer/Files");
    readonly_file(policy.executable,root/"app/service");
    readonly_file("/etc/os-release",root/"etc/os-release");readonly_file("/etc/resolv.conf",root/"etc/resolv.conf");
    const auto target=root/"data/Society.societycontainer/Files";
    require(::mount(workspace.files.c_str(),target.c_str(),nullptr,MS_BIND,nullptr),"bind Society Files workspace");
    require(::mount(nullptr,target.c_str(),nullptr,MS_BIND|MS_REMOUNT|MS_NOSUID|MS_NODEV|MS_NOATIME,nullptr),"restrict workspace mount");
    require(::mount(nullptr,root.c_str(),nullptr,MS_REMOUNT|MS_RDONLY|MS_NOSUID|MS_NODEV,nullptr),"seal workload runtime root");
    require(::chdir(root.c_str()),"enter runtime root");require(::chroot("."),"change workload root");
    require(::chdir(workspace_root),"enter Society Files");
    drop_privileges();
    // No directory, block-device, agent or privileged process descriptor may
    // survive into application code. Only closed stdin and console streams do.
    require(static_cast<int>(::syscall(SYS_close_range,3,~0U,0)),"close inherited descriptors");
    syscall_filter();
}
} // namespace ii::virtualizer::guest
