#pragma once
// Asset root resolution (no chdir). The explicit-input overload lets tests check the order
// without touching the environment.
#include <cstdint>
#include <string>
#include <string_view>
namespace lam {
// Which step of the resolution order chose the root.
enum class AssetRootSource : std::uint8_t {
    Config,            // 1. AssetsConfig::root
    Environment,       // 2. LAM_ASSET_ROOT
    Sidecar,           // 3. <exe_dir>/<exe_name>.assetroot (LAM_DEV builds only)
    BesideExecutable,  // 4. <exe_dir>/assets
    Bundle,            // 5. <exe_dir>/../Resources/assets (macOS bundle)
    Fallback,          // nothing exists: <exe_dir>/assets, or "assets" if exe_dir is unknown
};
// "config", "env", "sidecar", "exe", "bundle", "fallback". Static storage.
std::string_view asset_root_source_name(AssetRootSource source);
struct AssetRootResult {
    std::string path;                                           // as found; may be relative
    AssetRootSource source = AssetRootSource::Fallback;
};
// Resolution with explicit inputs ("" = unset or unknown); `dev` enables the sidecar step.
// Empty sidecar files and missing folders are skipped.
AssetRootResult resolve_asset_root(std::string_view config_root, std::string_view env_root,
                                   std::string_view exe_dir, std::string_view exe_name, bool dev);
// The resolution order for this process: LAM_ASSET_ROOT from the environment, executable_dir() and
// executable_name() from lam/core/fs.h, and dev = LAM_DEV.
AssetRootResult resolve_asset_root(std::string_view config_root);
}
