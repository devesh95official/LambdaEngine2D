#pragma once
// Build and platform identity, recorded in replay headers.
#include <string>
#include <string_view>
namespace lam {
std::string_view engine_version();                              // "0.1.0"
std::string_view build_id();                                    // LAM_BUILD_ID (git short hash or "dev")
std::string os_description();                                   // "macOS 26.6 arm64"
}
