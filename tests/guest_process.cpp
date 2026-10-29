#include <iiSDKCore/Runtime/EventLoop.h>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static bool write_all(int fd,const void* bytes,std::size_t size) {auto data=static_cast<const char*>(bytes);while(size){auto n=::write(fd,data,size);if(n<0&&errno==EINTR)continue;if(n<=0)return false;data+=n;size-=static_cast<std::size_t>(n);}return true;}
static int child_loop(){if(::setsid()<0)return 91;for(unsigned count=0;;++count){std::ofstream out("tree-child-ticks");out<<count;out.flush();::usleep(10000);}return 0;}
int main(int argc,char** argv) {
    const std::string mode=argc>1?argv[1]:"echo";
    if(mode=="echo") {
        std::array<char,4096> data{};
        for(;;){auto n=::read(0,data.data(),data.size());if(n<0&&errno==EINTR)continue;if(n<0)return 2;if(!n)break;if(!write_all(1,data.data(),static_cast<std::size_t>(n))||!write_all(2,data.data(),static_cast<std::size_t>(n)))return 3;}
        return 7;
    }
    if(mode=="exit")return argc>2?std::atoi(argv[2]):0;
    if(mode=="wait"){for(;;)::pause();}
    if(mode=="flood"){std::array<char,4096> data{};for(std::size_t i=0;i<data.size();++i)data[i]=static_cast<char>(i%256);for(unsigned i=0;i<64;++i)if(!write_all(1,data.data(),data.size())||!write_all(2,data.data(),data.size()))return 3;return 9;}
    if(mode=="tree"||mode=="orphan") {auto child=::fork();if(child<0)return 90;if(!child)return child_loop();std::cout<<"child="<<child<<std::endl;if(mode=="orphan"){::usleep(80000);return 0;}for(;;)::pause();}
    if(mode=="install") {std::ifstream input("/app/service",std::ios::binary);std::ofstream output("installed-probe",std::ios::binary);output<<input.rdbuf();output.flush();return input.bad()||!output?4:0;}
    if(mode=="probe") {
        unsigned checks=0,failures=0;auto check=[&](bool ok){++checks;if(!ok)++failures;};
        check(::getuid()==1000&&::geteuid()==1000&&::getgid()==1000);check(::prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)==1&&::prctl(PR_GET_SECCOMP,0,0,0,0)==2);
        for(const char* path:{"/escaped","/etc/os-release","/data/Society.societycontainer/.ii-runtime/boot-count","/sys/fs/cgroup/iiv"}){int fd=::open(path,O_WRONLY|O_CREAT|O_CLOEXEC,0600);check(fd<0);if(fd>=0)::close(fd);}
        check(::setuid(0)<0);int disk=::open("/dev/vda",O_RDONLY);check(disk<0);if(disk>=0)::close(disk);
        check(::symlink("/etc/os-release","process-escape-link")==0||errno==EEXIST);int link=::open("process-escape-link",O_WRONLY);check(link<0);if(link>=0)::close(link);
        {std::ofstream file("process-inside");file<<"inside Files";file.flush();check(bool(file));}
        std::array<char,2048> cwd{};check(::getcwd(cwd.data(),cwd.size())!=nullptr);std::cout<<"{\"checks\":"<<checks<<",\"failed\":"<<failures<<",\"cwd\":\""<<cwd.data()<<"\",\"env\":\""<<(std::getenv("APP_VALUE")?std::getenv("APP_VALUE"):"")<<"\"}\n";return failures?5:0;
    }
    if(mode=="cpu") {ii::runtime::EventLoop loop(8);auto token=loop.every(std::chrono::milliseconds(1),[]{volatile unsigned value=0;for(unsigned i=0;i<100000;++i)value=value+i;(void)value;});if(!token)return 2;loop.run();}
    return 64;
}
