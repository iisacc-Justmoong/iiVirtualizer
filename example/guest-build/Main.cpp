#include <iiSDKCore/Runtime/Concurrent.h>
#include <iiSDKCore/Runtime/Runtime.h>
#include <iostream>
int main(int argc,char** argv) {
    if(argc!=2)return 2;
    ii::runtime::Runtime workers({.workers=2,.capacity=16});
    ii::runtime::Concurrent jobs(workers);
    auto result=jobs.run([] {int total=0;for(int n=1;n<=100;++n)total+=n;return total;}).get();
    if(!result||*result!=5050)return 3;
    std::cout<<"IIV_GUEST_APP_OK os="<<IIV_CMAKE_HOST_OS<<" arch="<<IIV_CMAKE_HOST_ARCH
             <<" result="<<*result<<" nonce="<<argv[1]<<'\n';
    return 0;
}
