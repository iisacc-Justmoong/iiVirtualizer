#include <iiSDKCore/Runtime/EventLoop.h>
#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

// A static Linux workload for the image recipe. It runs from /data and proves
// that the VM can host an externally reachable service with persistent data.
int main() {
    int server=::socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
    if(server<0)return 1;
    int yes=1;(void)::setsockopt(server,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_ANY);address.sin_port=htons(8080);
    if(::bind(server,reinterpret_cast<sockaddr*>(&address),sizeof(address))!=0||::listen(server,16)!=0)return 2;
    std::uint64_t boot=0;std::ifstream("/data/boot-count")>>boot;
    const std::string body="iiOS workload via iiSDKCore; boot="+std::to_string(boot)+"\n";
    const std::string response="HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
    ii::runtime::EventLoop loop(16);
    auto task=loop.every(std::chrono::milliseconds(10),[&] {
        for(unsigned count=0;count<8;++count) {
            int client=::accept4(server,nullptr,nullptr,SOCK_CLOEXEC|SOCK_NONBLOCK);
            if(client<0)break;
            // This demonstration serves a fixed small response; production
            // workloads should use iiNetwork/iiWebServer's connection handling.
            std::array<char,4096> request{};(void)::recv(client,request.data(),request.size(),0);
            (void)::send(client,response.data(),response.size(),MSG_NOSIGNAL);::close(client);
        }
    });
    if(!task)return 3;
    std::cout<<"ii-guest-service listening on :8080, boot="<<boot<<std::endl;
    loop.run();::close(server);
}
