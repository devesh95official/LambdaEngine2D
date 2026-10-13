// Asset path helpers (detail/asset_internal.h): normalisation of cache keys and sibling paths.
// Cold path only (load time); every function allocates.
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "detail/asset_internal.h"

namespace lam::detail {

std::string asset_to_lower(std::string_view s) {
    std::string out(s);
    for (char &c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::string_view asset_strip_fragment(std::string_view path) {
    const std::size_t hash = path.find('#');
    return hash == std::string_view::npos ? path : path.substr(0, hash);
}

bool asset_is_absolute(std::string_view p) {
    return (!p.empty() && (p.front() == '/' || p.front() == '\\')) ||
           (p.size() >= 2 && p[1] == ':');
}

std::string normalize_asset_path(std::string_view path) {
    const std::size_t hash = path.find('#');
    const std::string_view fragment =
        hash == std::string_view::npos ? std::string_view{} : path.substr(hash);
    std::string p(asset_strip_fragment(path));
    std::replace(p.begin(), p.end(), '\\', '/');
    const bool absolute = !p.empty() && p.front() == '/';
    std::vector<std::string_view> parts;
    std::string_view rest = p;
    while (!rest.empty()) {
        const std::size_t slash = rest.find('/');
        const std::string_view part = rest.substr(0, slash);
        rest = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash + 1);
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            if (!parts.empty() && parts.back() != "..") {
                parts.pop_back();
            } else if (!absolute) {
                parts.push_back(part);
            }
            continue;
        }
        parts.push_back(part);
    }
    std::string out = absolute ? "/" : "";
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out += '/';
        out += parts[i];
    }
    out += fragment;
    return out;
}

std::string sibling_asset_path(std::string_view parent, std::string_view relative) {
    if (asset_is_absolute(relative)) return normalize_asset_path(relative);
    const std::string_view file = asset_strip_fragment(parent);
    const std::size_t slash = file.find_last_of("/\\");
    if (slash == std::string_view::npos) return normalize_asset_path(relative);
    std::string joined(file.substr(0, slash + 1));
    joined += relative;
    return normalize_asset_path(joined);
}

} // namespace lam::detail
