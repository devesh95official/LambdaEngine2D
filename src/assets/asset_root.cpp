// Asset root resolution. See lam/assets/asset_root.h.
#include <cstdlib>
#include <string>
#include <string_view>

#include <lam/assets/asset_root.h>
#include <lam/core/fs.h>

namespace lam {
namespace {

std::string_view trim(std::string_view s) {
    const auto space = [](char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; };
    while (!s.empty() && space(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && space(s.back()))
        s.remove_suffix(1);
    return s;
}

} // namespace

std::string_view asset_root_source_name(AssetRootSource source) {
    switch (source) {
    case AssetRootSource::Config: return "config";
    case AssetRootSource::Environment: return "env";
    case AssetRootSource::Sidecar: return "sidecar";
    case AssetRootSource::BesideExecutable: return "exe";
    case AssetRootSource::Bundle: return "bundle";
    case AssetRootSource::Fallback: return "fallback";
    }
    return "fallback";
}

AssetRootResult resolve_asset_root(std::string_view config_root, std::string_view env_root,
                                   std::string_view exe_dir, std::string_view exe_name, bool dev) {
    if (!config_root.empty()) return {std::string(config_root), AssetRootSource::Config};
    if (!env_root.empty()) return {std::string(env_root), AssetRootSource::Environment};
    if (exe_dir.empty()) return {"assets", AssetRootSource::Fallback};
    if (dev && !exe_name.empty()) {
        const std::string sidecar = join_path(exe_dir, std::string(exe_name) + ".assetroot");
        if (Result<std::string> text = read_file(sidecar)) {
            const std::string_view dir = trim(*text);
            if (!dir.empty() && file_exists(std::string(dir)))
                return {std::string(dir), AssetRootSource::Sidecar};
        }
    }
    std::string beside = join_path(exe_dir, "assets");
    if (file_exists(beside)) return {std::move(beside), AssetRootSource::BesideExecutable};
    std::string bundle = join_path(exe_dir, "../Resources/assets");
    if (file_exists(bundle)) return {std::move(bundle), AssetRootSource::Bundle};
    return {std::move(beside), AssetRootSource::Fallback};
}

AssetRootResult resolve_asset_root(std::string_view config_root) {
    if (!config_root.empty()) return {std::string(config_root), AssetRootSource::Config};
    const char *env = std::getenv("LAM_ASSET_ROOT");
    const std::string_view env_root = env != nullptr ? std::string_view(env) : std::string_view{};
    const Result<std::string> exe_dir = executable_dir();
    const std::string dir = exe_dir ? *exe_dir : std::string{};
    const std::string name = dir.empty() ? std::string{} : executable_name();
    return resolve_asset_root(config_root, env_root, dir, name, LAM_DEV != 0);
}

} // namespace lam
