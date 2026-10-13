// Every lam/...h path named in a comment under src/ must exist. Paths into a module that has no
// src/<module>/CMakeLists.txt yet only print a message.
#include <doctest/doctest.h>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

#ifndef LAM_REPO_ROOT
#define LAM_REPO_ROOT ""
#endif

fs::path repo_root() {
    fs::path root = LAM_REPO_ROOT;
    if (!root.empty() && fs::exists(root / "src")) return root;
    if (const char *env = std::getenv("LAM_TEST_SOURCE_DIR"); env && *env) {
        root = fs::path(env).parent_path().parent_path();
        if (fs::exists(root / "src")) return root;
    }
    return {};
}

// Every include root a `lam/...` path can live under.
std::vector<fs::path> include_roots(const fs::path &repo) {
    std::vector<fs::path> roots;
    for (const auto &entry : fs::directory_iterator(repo / "src")) {
        if (entry.is_directory() && fs::exists(entry.path() / "include"))
            roots.push_back(entry.path() / "include");
    }
    for (const char *extra :
         {"src/core/app/include", "src/core/backend/include", "src/core/engine/include",
          "src/core/backend/rl", "tests/support/include"})
        if (fs::exists(repo / extra)) roots.push_back(repo / extra);
    return roots;
}

// The comment text of a C++ source: // and /* */ comments, skipping string and character literals
// (including raw strings) so "http://..." in a literal is not a comment.
std::string comments_of(std::string_view src) {
    std::string out;
    std::size_t i = 0;
    const std::size_t n = src.size();
    while (i < n) {
        const char c = src[i];
        if (c == '/' && i + 1 < n && src[i + 1] == '/') {
            const std::size_t end = src.find('\n', i);
            const std::size_t stop = end == std::string_view::npos ? n : end;
            out.append(src.substr(i + 2, stop - i - 2));
            out += '\n';
            i = stop;
        } else if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            const std::size_t end = src.find("*/", i + 2);
            const std::size_t stop = end == std::string_view::npos ? n : end;
            out.append(src.substr(i + 2, stop - i - 2));
            out += '\n';
            i = stop == n ? n : stop + 2;
        } else if (c == 'R' && i + 1 < n && src[i + 1] == '"' &&
                   (i == 0 ||
                    !(std::isalnum(static_cast<unsigned char>(src[i - 1])) || src[i - 1] == '_'))) {
            const std::size_t open = src.find('(', i + 2);
            if (open == std::string_view::npos) break;
            const std::string close = ")" + std::string(src.substr(i + 2, open - i - 2)) + "\"";
            const std::size_t end = src.find(close, open + 1);
            i = end == std::string_view::npos ? n : end + close.size();
        } else if (c == '"' || (c == '\'' &&
                                !(i > 0 && std::isalnum(static_cast<unsigned char>(src[i - 1]))))) {
            // A string or character literal (a ' after a digit is a digit separator).
            ++i;
            while (i < n && src[i] != c && src[i] != '\n')
                i += src[i] == '\\' ? 2 : 1;
            ++i;
        } else {
            ++i;
        }
    }
    return out;
}

// Every "lam/...h" path in `text`.
std::vector<std::string> header_refs(std::string_view text) {
    std::vector<std::string> out;
    auto path_char = [](char ch) {
        return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '/' ||
               ch == '.' || ch == '-';
    };
    std::size_t at = 0;
    while ((at = text.find("lam/", at)) != std::string_view::npos) {
        const bool word_start =
            at == 0 ||
            !(std::isalnum(static_cast<unsigned char>(text[at - 1])) || text[at - 1] == '_');
        std::size_t end = at + 4;
        while (end < text.size() && path_char(text[end]))
            ++end;
        std::string_view ref = text.substr(at, end - at);
        while (!ref.empty() && (ref.back() == '.' || ref.back() == '/'))
            ref.remove_suffix(1); // sentence end
        if (word_start && ref.size() > 6 && ref.ends_with(".h")) out.emplace_back(ref);
        at = end;
    }
    return out;
}

bool is_source(const fs::path &p) {
    const std::string ext = p.extension().string();
    return ext == ".h" || ext == ".hpp" || ext == ".cpp" || ext == ".inl";
}

} // namespace

TEST_CASE("header refs: the comment scanner finds references and skips literals (canaries)") {
    const std::string src =
        "// see lam/core/id.h and lam/math/vec2.h.\n"
        "const char *s = \"lam/not/a/comment.h\"; /* lam/render/sprite.h */\n"
        "auto r = R\"x(// lam/raw/string.h)x\"; int k = 1'000; // xlam/no.h lam/ok/a.h\n";
    const std::vector<std::string> refs = header_refs(comments_of(src));
    REQUIRE(refs.size() == 4);
    CHECK(refs[0] == "lam/core/id.h");
    CHECK(refs[1] == "lam/math/vec2.h");
    CHECK(refs[2] == "lam/render/sprite.h");
    CHECK(refs[3] == "lam/ok/a.h");
}

TEST_CASE("header refs: every lam/...h mentioned in a comment under src/ exists") {
    const fs::path repo = repo_root();
    REQUIRE_MESSAGE(!repo.empty(), "set LAM_REPO_ROOT or LAM_TEST_SOURCE_DIR");
    const std::vector<fs::path> roots = include_roots(repo);
    std::size_t refs_seen = 0;
    std::vector<std::string> missing;
    for (const auto &entry : fs::recursive_directory_iterator(repo / "src")) {
        if (!entry.is_regular_file() || !is_source(entry.path())) continue;
        std::ifstream in(entry.path(), std::ios::binary);
        std::stringstream buf;
        buf << in.rdbuf();
        const std::string rel = fs::relative(entry.path(), repo).generic_string();
        for (const std::string &ref : header_refs(comments_of(buf.str()))) {
            ++refs_seen;
            bool found = false;
            for (const fs::path &r : roots)
                found = found || fs::exists(r / ref);
            if (found) continue;
            // lam/<module>/... of a module that is not built yet (no src/<module>/CMakeLists.txt).
            const std::string module = ref.substr(4, ref.find('/', 4) - 4);
            if (module != "core" && module != "rl" && module != "test" &&
                !fs::exists(repo / "src" / module / "CMakeLists.txt")) {
                MESSAGE(rel << " mentions " << ref << " (module " << module << " not written yet)");
                continue;
            }
            missing.push_back(rel + " mentions " + ref + ", which does not exist");
        }
    }
    CHECK(refs_seen > 0);
    for (const std::string &m : missing)
        FAIL_CHECK(m);
    CHECK(missing.empty());
}
