// Checks every #include <lam/...> under src/ against the module graph below. src/core/engine/ is
// exempt, and backend files may also include <lam/rl/convert.h>.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

#ifndef LAM_REPO_ROOT
#define LAM_REPO_ROOT ""
#endif

// The repository root: the compile-time path, else LAM_TEST_SOURCE_DIR (tests/core) up two levels.
fs::path repo_root() {
    fs::path root = LAM_REPO_ROOT;
    if (!root.empty() && fs::exists(root / "src")) return root;
    if (const char *env = std::getenv("LAM_TEST_SOURCE_DIR"); env && *env) {
        root = fs::path(env).parent_path().parent_path();
        if (fs::exists(root / "src")) return root;
    }
    return {};
}

struct Node {
    std::string_view name;
    std::string_view root; // include root, relative to the repository
    std::vector<std::string_view> may_include;
};

// The allowed edges, row by row. "rl" is lam_rl_common (<lam/rl/convert.h>), backend files only.
const std::vector<Node> &nodes() {
    static const std::vector<Node> table = {
        {"math", "src/math/include", {}},
        {"core", "src/core/include", {"math"}},
        {"ecs", "src/ecs/include", {"math", "core"}},
        {"app", "src/core/app/include", {"math", "core", "ecs"}},
        {"platform", "src/core/backend/include", {"math", "core", "ecs", "app"}},
        {"assets", "src/assets/include", {"math", "core", "ecs", "app"}},
        {"render", "src/render/include", {"math", "core", "ecs", "app", "assets"}},
        {"physics", "src/physics/include", {"math", "core", "ecs", "app"}},
        {"input", "src/input/include", {"math", "core", "ecs", "app", "render"}},
        {"audio", "src/audio/include", {"math", "core", "ecs", "app", "assets"}},
        {"scene", "src/scene/include", {"math", "core", "ecs", "app", "assets", "render"}},
        {"tilemap",
         "src/tilemap/include",
         {"math", "core", "ecs", "app", "assets", "render", "physics"}},
        {"animation",
         "src/animation/include",
         {"math", "core", "ecs", "app", "assets", "render", "physics"}},
        {"particles", "src/particles/include", {"math", "core", "ecs", "app", "assets", "render"}},
        {"ui",
         "src/ui/include",
         {"math", "core", "ecs", "app", "assets", "render", "input", "scene", "audio"}},
        {"rl", "src/core/backend/rl", {"math", "core"}},
    };
    return table;
}

const Node *node_named(std::string_view name) {
    for (const Node &n : nodes())
        if (n.name == name) return &n;
    return nullptr;
}

// The node a source file belongs to, by folder; "" for exempt or unknown files.
std::string_view node_of_file(std::string_view rel) {
    if (rel.starts_with("src/core/engine/")) return "";
    if (rel.starts_with("src/core/app/")) return "app";
    if (rel.starts_with("src/core/backend/rl/")) return "rl";
    if (rel.starts_with("src/core/backend/")) return "platform";
    if (rel.starts_with("src/core/")) return "core";
    if (!rel.starts_with("src/")) return "";
    const std::string_view rest = rel.substr(4);
    const std::string_view module = rest.substr(0, rest.find('/'));
    return node_named(module) ? node_named(module)->name : std::string_view{};
}

// The node whose root holds `header` ("lam/render/camera.h"). Falls back to the folder name for
// modules whose headers are not written yet (lam/ecs/... before src/ecs lands); lam/core/... must
// exist, because four nodes share that prefix.
std::string_view node_of_header(const fs::path &repo, std::string_view header) {
    for (const Node &n : nodes())
        if (fs::exists(repo / n.root / header)) return n.name;
    if (!header.starts_with("lam/")) return "";
    const std::string_view rest = header.substr(4);
    const std::string_view module = rest.substr(0, rest.find('/'));
    if (module == "core" || module == "test") return "";
    return node_named(module) ? node_named(module)->name : std::string_view{};
}

// The layering rule for one edge.
bool edge_allowed(std::string_view from, std::string_view to, std::string_view header,
                  bool backend_file) {
    if (from == to) return true;
    if (to == "rl") return backend_file;
    const Node *n = node_named(from);
    if (!n) return false;
    if (std::find(n->may_include.begin(), n->may_include.end(), to) == n->may_include.end())
        return false;
    if (from == "input" && to == "render")
        return header == "lam/render/camera.h" || header == "lam/render/virtual_screen.h";
    return true;
}

// Every `#include <lam/...>` in a file.
std::vector<std::string> lam_includes(const fs::path &file) {
    std::vector<std::string> out;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        std::size_t i = line.find_first_not_of(" \t");
        if (i == std::string::npos || line[i] != '#') continue;
        i = line.find_first_not_of(" \t", i + 1);
        if (i == std::string::npos || line.compare(i, 7, "include") != 0) continue;
        i = line.find_first_not_of(" \t", i + 7);
        if (i == std::string::npos || line[i] != '<') continue;
        const std::size_t end = line.find('>', i);
        if (end == std::string::npos) continue;
        const std::string header = line.substr(i + 1, end - i - 1);
        if (header.starts_with("lam/")) out.push_back(header);
    }
    return out;
}

bool is_source(const fs::path &p) {
    const std::string ext = p.extension().string();
    return ext == ".h" || ext == ".hpp" || ext == ".cpp" || ext == ".inl";
}

} // namespace

TEST_CASE("include graph: the rule table allows and refuses the expected edges (canaries)") {
    CHECK(edge_allowed("core", "math", "lam/math/vec2.h", false));
    CHECK_FALSE(edge_allowed("math", "core", "lam/core/id.h", false));
    CHECK_FALSE(edge_allowed("core", "ecs", "lam/ecs/world.h", false));
    CHECK(edge_allowed("ui", "audio", "lam/audio/components.h", false));
    CHECK_FALSE(edge_allowed("physics", "render", "lam/render/sprite.h", false));
    CHECK_FALSE(edge_allowed("render", "physics", "lam/physics/components.h", false));
    CHECK(edge_allowed("input", "render", "lam/render/camera.h", false));
    CHECK(edge_allowed("input", "render", "lam/render/virtual_screen.h", false));
    CHECK_FALSE(edge_allowed("input", "render", "lam/render/sprite.h", false));
    CHECK(edge_allowed("render", "rl", "lam/rl/convert.h", true));
    CHECK_FALSE(edge_allowed("render", "rl", "lam/rl/convert.h", false));
    CHECK_FALSE(edge_allowed("render", "platform", "lam/core/raylib_platform.h", true));
    CHECK(edge_allowed("platform", "app", "lam/core/app.h", false));
    CHECK_FALSE(edge_allowed("app", "platform", "lam/core/raylib_platform.h", false));
    CHECK(node_of_file("src/core/engine/default_plugins.cpp").empty());
    CHECK(node_of_file("src/core/app/ctx.cpp") == "app");
    CHECK(node_of_file("src/core/backend/raylib_platform.cpp") == "platform");
    CHECK(node_of_file("src/core/data.cpp") == "core");
    CHECK(node_of_file("src/render/backend/batcher.cpp") == "render");
    CHECK(node_of_file("src/ui/include/lam/ui/ui.h") == "ui");
}

TEST_CASE("include graph: every #include <lam/...> under src/ follows the layering") {
    const fs::path repo = repo_root();
    REQUIRE_MESSAGE(!repo.empty(), "set LAM_REPO_ROOT or LAM_TEST_SOURCE_DIR");
    std::size_t files = 0;
    std::size_t edges = 0;
    std::vector<std::string> problems;
    for (const auto &entry : fs::recursive_directory_iterator(repo / "src")) {
        if (!entry.is_regular_file() || !is_source(entry.path())) continue;
        const std::string rel = fs::relative(entry.path(), repo).generic_string();
        const std::string_view from = node_of_file(rel);
        if (from.empty()) continue;
        ++files;
        const bool backend = rel.find("/backend/") != std::string::npos;
        for (const std::string &header : lam_includes(entry.path())) {
            ++edges;
            const std::string_view to = node_of_header(repo, header);
            if (to.empty()) {
                problems.push_back(rel + ": <" + header + "> does not resolve to any include root");
                continue;
            }
            if (!edge_allowed(from, to, header, backend))
                problems.push_back(rel + " (" + std::string(from) + ") includes <" + header +
                                   "> (" + std::string(to) +
                                   "): edge not allowed by the module layering");
        }
    }
    CHECK(files > 20);
    CHECK(edges > 50);
    for (const std::string &p : problems)
        FAIL_CHECK(p);
    CHECK(problems.empty());
}
