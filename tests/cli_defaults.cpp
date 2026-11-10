#include "../src/cli/Main.h"
#include <filesystem>
#include <iostream>
#include <string>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/un.h>
#endif

int main(int argc, char** argv) {
    const std::filesystem::path root = ii::virtualizer::cli::default_instance_root;
    if (argc != 2 || root.is_absolute() || root != std::filesystem::path("resources/vm")) {
        std::cerr << "The default instance root must be a project resource path\n";
        return 1;
    }
#if defined(__unix__) || defined(__APPLE__)
    const auto socket = std::filesystem::path(argv[1]) / root / std::string(32, 'a') / "agent.sock";
    if (socket.string().size() >= sizeof(sockaddr_un{}.sun_path)) {
        std::cerr << "The project path leaves insufficient room for an instance socket\n";
        return 1;
    }
#endif
}
