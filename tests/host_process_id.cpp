#include "HostProcessId.h"

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

int main() {
#if defined(_WIN32)
    const auto expected = ::_getpid();
#else
    const auto expected = ::getpid();
#endif
    return ii::virtualizer::testing::host_process_id() == static_cast<std::uint64_t>(expected) ? 0 : 1;
}
