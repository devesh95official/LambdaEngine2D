#include <lam/core/build_info.h>

#include <sys/utsname.h>

#ifndef LAM_BUILD_ID
#define LAM_BUILD_ID "dev"
#endif

namespace lam {

std::string_view engine_version() { return "0.1.0"; }

std::string_view build_id() { return LAM_BUILD_ID; }

std::string os_description() {
    utsname u{};
    if (uname(&u) != 0) return "unknown";
    std::string sys = u.sysname;
    if (sys == "Darwin") sys = "macOS(Darwin)";
    return sys + " " + u.release + " " + u.machine;
}

} // namespace lam
