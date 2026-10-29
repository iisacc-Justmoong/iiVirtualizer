#include "Qmp.h"
#include "FilesWire.h"
#include "ProcessWire.h"
#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>
#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace ii::virtualizer::detail {
#if defined(__unix__) || defined(__APPLE__)
class Socket final {
public:
    Socket(const fs::path& path, std::chrono::milliseconds timeout):deadline_(std::chrono::steady_clock::now()+timeout) {
        auto text=path.string();sockaddr_un address{};address.sun_family=AF_UNIX;
        if(text.size()>=sizeof(address.sun_path)) throw std::runtime_error("Control socket path exceeds the host AF_UNIX limit; use a shorter state root");
        std::memcpy(address.sun_path,text.c_str(),text.size()+1);
        fd_=::socket(AF_UNIX,SOCK_STREAM,0);
        if(fd_<0) native("create control socket");
        try {
            if(::fcntl(fd_,F_SETFD,FD_CLOEXEC)<0 || ::fcntl(fd_,F_SETFL,O_NONBLOCK)<0) native("configure control socket");
#if defined(SO_NOSIGPIPE)
            int yes=1;if(::setsockopt(fd_,SOL_SOCKET,SO_NOSIGPIPE,&yes,sizeof(yes))!=0) native("disable socket SIGPIPE");
#endif
            if(::connect(fd_,reinterpret_cast<sockaddr*>(&address),sizeof(address))!=0) {
                if(errno!=EINPROGRESS) native("connect control socket");
                ready(POLLOUT);int error=0;socklen_t length=sizeof(error);
                if(::getsockopt(fd_,SOL_SOCKET,SO_ERROR,&error,&length)!=0) native("connect status");
                if(error) throw std::system_error(error,std::generic_category(),"connect control socket");
            }
        } catch(...) {::close(fd_);fd_=-1;throw;}
    }
    ~Socket() { if(fd_>=0)::close(fd_); }
    Socket(const Socket&)=delete;
    Socket& operator=(const Socket&)=delete;
    void write(std::string_view text) {
        while(!text.empty()) {
            ready(POLLOUT);
#if defined(MSG_NOSIGNAL)
            auto n=::send(fd_,text.data(),text.size(),MSG_NOSIGNAL);
#else
            auto n=::send(fd_,text.data(),text.size(),0);
#endif
            if(n<0) {if(errno==EINTR||errno==EAGAIN)continue;native("send control command");}
            if(n==0)throw std::runtime_error("Control socket accepted zero bytes");
            text.remove_prefix(static_cast<std::size_t>(n));
        }
    }
    std::string line() {
        for(;;) {
            if(auto at=buffer_.find('\n');at!=std::string::npos) {
                if(at>65536)throw std::runtime_error("Control response exceeds 64 KiB");
                auto line=buffer_.substr(0,at);buffer_.erase(0,at+1);return line;
            }
            if(buffer_.size()>65536)throw std::runtime_error("Control response exceeds 64 KiB");
            ready(POLLIN);std::array<char,4096> bytes{};auto n=::recv(fd_,bytes.data(),bytes.size(),0);
            if(n<0) {if(errno==EINTR||errno==EAGAIN)continue;native("read control response");}
            if(n==0)throw std::system_error(ECONNRESET,std::generic_category(),"Control connection closed before a complete response");
            buffer_.append(bytes.data(),static_cast<std::size_t>(n));
        }
    }
private:
    int fd_=-1;
    std::chrono::steady_clock::time_point deadline_;
    std::string buffer_;
    [[noreturn]] static void native(const char* action) {throw std::system_error(errno,std::generic_category(),action);}
    void ready(short events) {
        for(;;) {
            auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline_-std::chrono::steady_clock::now()).count();
            if(left<=0)throw std::system_error(ETIMEDOUT,std::generic_category(),"control deadline");
            pollfd descriptor{fd_,events,0};int n=::poll(&descriptor,1,static_cast<int>(std::min<std::int64_t>(left,2147483647)));
            if(n<0) {if(errno==EINTR)continue;native("poll control socket");}
            if(n==0)throw std::system_error(ETIMEDOUT,std::generic_category(),"control deadline");
            if(descriptor.revents & (events|POLLHUP|POLLERR))return;
            throw std::runtime_error("Invalid control socket readiness");
        }
    }
};
static Json parse_line(Socket& socket) {
    auto parsed=ii::json::parse(socket.line(),{.input_bytes=65536,.output_bytes=65536,.string_bytes=32768,.items=2048,.depth=16});
    if(!parsed)throw std::runtime_error("Invalid QMP JSON: "+parsed.error().message);
    if(!parsed->get<Json::Object>())throw std::runtime_error("QMP response is not an object");
    return *parsed;
}
static Json execute(Socket& socket, std::string_view command, unsigned id) {
    socket.write(json_text(Json(Json::Object{{"execute",Json(command)},{"id",Json(id)}}))+"\r\n");
    for(;;) {
        Json response;
        try {response=parse_line(socket);}
        catch(const std::system_error& error) {
            // quit can terminate the main loop before its output queue drains.
            // The caller still waits for independently observed process exit.
            if(command=="quit"&&error.code().value()==ECONNRESET)return Json(Json::Object{});
            throw;
        }
        if(response.find("event"))continue;
        auto actual=integer_field(response,"id");
        if(actual!=id)throw std::runtime_error("QMP response transaction id mismatch");
        if(auto error=response.find("error"))throw std::runtime_error("QMP error: "+json_text(error->get()));
        return field(response,"return");
    }
}
static Result<Json> qmp_impl(const Instance& vm,std::string_view command,std::chrono::milliseconds timeout) {
    Socket socket(vm.paths.qmp,timeout);
    auto greeting=parse_line(socket);
    (void)field(greeting,"QMP");
    (void)execute(socket,"qmp_capabilities",1);
    auto identity=execute(socket,"query-uuid",2);
    if(text_field(identity,"UUID")!=vm.uuid) return core::fail(core::ErrorCode::invalid_argument,"QMP instance UUID mismatch; refusing control");
    return execute(socket,command,3);
}
template<class F> static auto transport(F&& fn) -> decltype(fn()) {
    try{return fn();}
    catch(const std::system_error& e) {
        auto code=(e.code().value()==ENOENT||e.code().value()==ECONNREFUSED)?core::ErrorCode::not_found:
            (e.code().value()==ECONNRESET||e.code().value()==EPIPE)?core::ErrorCode::busy:core::ErrorCode::failed;
        return core::fail(code,e.what());
    } catch(const std::exception& e) {return core::fail(core::ErrorCode::failed,e.what());}
}
#endif
Result<Json> qmp(const Instance& vm,std::string_view command,std::chrono::milliseconds timeout) {
#if defined(__unix__) || defined(__APPLE__)
    return transport([&]{return qmp_impl(vm,command,timeout);});
#else
    (void)vm;(void)command;(void)timeout;return core::fail(core::ErrorCode::unsupported,"QMP AF_UNIX adapter requires macOS or Linux");
#endif
}
Result<std::string> agent(const Instance& vm,std::string_view command,std::chrono::milliseconds timeout) {
#if defined(__unix__) || defined(__APPLE__)
    return transport([&]() -> Result<std::string> {
        if(command!="info"&&command!="ping"&&command!="shutdown")return core::fail(core::ErrorCode::invalid_argument,"Unknown guest command");
        Socket socket(vm.paths.agent,timeout);
        socket.write("IIV1 "+vm.uuid+" "+std::string(command)+"\n");
        auto response=socket.line();
        if(!response.starts_with("IIV1 "+vm.uuid+" "))return core::fail(core::ErrorCode::invalid_argument,"Guest protocol or UUID mismatch");
        return response;
    });
#else
    (void)vm;(void)command;(void)timeout;return core::fail(core::ErrorCode::unsupported,"Guest transport requires macOS or Linux");
#endif
}
Result<FileReply> files_agent(const Instance& vm,const FileRequest& request,std::chrono::milliseconds timeout) {
#if defined(__unix__) || defined(__APPLE__)
    return transport([&]() -> Result<FileReply> {
        Socket socket(vm.paths.agent,timeout);
        socket.write("IIV1 "+vm.uuid+" info\n");const auto capabilities=socket.line();
        if(!capabilities.starts_with("IIV1 "+vm.uuid+" INFO "))return core::fail(core::ErrorCode::invalid_argument,"Files guest UUID or protocol mismatch");
        if(capabilities.find(" files_api=1 ")==std::string::npos)return core::fail(core::ErrorCode::unsupported,"Guest image has no Files API; update its stopped boot image");
        const auto id=files_detail::nonce();socket.write(files_detail::encode_request(vm.uuid,id,request,timeout));
        return files_detail::decode_reply(socket.line(),vm.uuid,id,request);
    });
#else
    (void)vm;(void)request;(void)timeout;return core::fail(core::ErrorCode::unsupported,"Guest Files transport requires macOS or Linux");
#endif
}
Result<ProcessReply> processes_agent(const Instance& vm,const ProcessRequest& request,std::chrono::milliseconds timeout) {
#if defined(__unix__) || defined(__APPLE__)
    return transport([&]() -> Result<ProcessReply> {
        Socket socket(vm.paths.agent,timeout);socket.write("IIV1 "+vm.uuid+" info\n");const auto capabilities=socket.line();
        if(!capabilities.starts_with("IIV1 "+vm.uuid+" INFO "))return core::fail(core::ErrorCode::invalid_argument,"Process guest UUID or protocol mismatch");
        if(capabilities.find(" processes_api=1 ")==std::string::npos)return core::fail(core::ErrorCode::unsupported,"Guest image has no process API; update its stopped boot image");
        const auto id=files_detail::nonce();socket.write(process_detail::encode_request(vm.uuid,id,request));
        return process_detail::decode_reply(socket.line(),vm.uuid,id,request);
    });
#else
    (void)vm;(void)request;(void)timeout;return core::fail(core::ErrorCode::unsupported,"Guest process transport requires macOS or Linux");
#endif
}
} // namespace ii::virtualizer::detail
