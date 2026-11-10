#include "HostProcessId.h"

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace ii::virtualizer::testing {
std::uint64_t host_process_id() noexcept {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::_getpid());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}
} // namespace ii::virtualizer::testing
