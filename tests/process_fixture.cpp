#include <array>
#include <cerrno>
#include <cstdlib>
#include <string>
#include <unistd.h>
static bool write_all(int fd,const void* bytes,std::size_t size){auto p=static_cast<const char*>(bytes);while(size){auto n=::write(fd,p,size);if(n<0&&errno==EINTR)continue;if(n<=0)return false;p+=n;size-=static_cast<std::size_t>(n);}return true;}
int main(int argc,char** argv) {
    const std::string mode=argc>1?argv[1]:"echo";
    if(mode=="echo"){std::array<char,4096> bytes{};for(;;){auto n=::read(0,bytes.data(),bytes.size());if(n<0&&errno==EINTR)continue;if(n<0)return 2;if(!n)return 7;if(!write_all(1,bytes.data(),static_cast<std::size_t>(n))||!write_all(2,bytes.data(),static_cast<std::size_t>(n)))return 3;}}
    if(mode=="flood"){std::array<char,4096> bytes{};for(std::size_t i=0;i<bytes.size();++i)bytes[i]=static_cast<char>(i%256);for(unsigned i=0;i<64;++i)if(!write_all(1,bytes.data(),bytes.size())||!write_all(2,bytes.data(),bytes.size()))return 3;return 9;}
    if(mode=="exit")return argc>2?std::atoi(argv[2]):0;
    if(mode=="wait")for(;;)::pause();
    return 64;
}
