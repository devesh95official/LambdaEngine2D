// Command-line parsing and the LaunchOptions queries.
#include <lam/core/app.h>

#include <algorithm>
#include <charconv>
#include <limits>

namespace lam {
namespace {

bool parse_int(std::string_view s, int &out) {
    int v = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || end != s.data() + s.size()) return false;
    out = v;
    return true;
}

bool parse_u64(std::string_view s, std::uint64_t &out) {
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s.remove_prefix(2);
        base = 16;
    }
    std::uint64_t v = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v, base);
    if (s.empty() || ec != std::errc{} || end != s.data() + s.size()) return false;
    out = v;
    return true;
}

} // namespace

bool LaunchOptions::has(std::string_view flag) const {
    if (flag == "--hidden") return hidden;
    if (flag == "--headless") return headless;
    if (flag == "--step") return step;
    if (flag == "--replay-any-build") return replay_any_build;
    if (flag == "--print-schedule") return print_schedule;
    if (flag == "--no-vsync") return no_vsync;
    return std::find(extra.begin(), extra.end(), flag) != extra.end();
}

std::optional<std::string_view> LaunchOptions::value(std::string_view flag) const {
    if (flag.empty()) return std::nullopt;
    for (std::size_t i = 0; i < extra.size(); ++i) {
        const std::string_view arg = extra[i];
        if (arg == flag) {
            if (i + 1 < extra.size()) return std::string_view(extra[i + 1]);
            return std::nullopt;
        }
        if (arg.size() > flag.size() && arg.starts_with(flag) && arg[flag.size()] == '=')
            return arg.substr(flag.size() + 1);
    }
    return std::nullopt;
}

int LaunchOptions::int_value(std::string_view flag, int fallback) const {
    const std::optional<std::string_view> v = value(flag);
    int out = 0;
    if (!v || !parse_int(*v, out)) return fallback;
    return out;
}

bool LaunchOptions::deterministic() const {
    return headless || step || !record_input.empty() || !replay.empty();
}

bool LaunchOptions::scripted() const {
    return deterministic() || hidden || frames > 0 || has("--bot");
}

LaunchOptions parse_launch_options(int argc, const char *const *argv) {
    LaunchOptions o;
    if (!argv) return o;
    for (int i = 1; i < argc; ++i) {
        if (!argv[i]) continue;
        const std::string_view arg = argv[i];
        const bool has_value = i + 1 < argc && argv[i + 1] != nullptr;
        const std::string_view value =
            has_value ? std::string_view(argv[i + 1]) : std::string_view();
        // Value flags: consume the value only when it parses; otherwise keep both in `extra`.
        auto int_flag = [&](int &field, int min) {
            int v = 0;
            if (has_value && parse_int(value, v)) {
                field = std::max(min, v);
                ++i;
                return;
            }
            o.extra.emplace_back(arg);
        };
        auto string_flag = [&](std::string &field) {
            if (has_value && !value.empty()) {
                field = std::string(value);
                ++i;
                return;
            }
            o.extra.emplace_back(arg);
        };
        if (arg == "--hidden") {
            o.hidden = true;
        } else if (arg == "--headless") {
            o.headless = true;
        } else if (arg == "--step") {
            o.step = true;
        } else if (arg == "--frames") {
            int_flag(o.frames, 0);
        } else if (arg == "--draw-every") {
            int_flag(o.draw_every, 1);
        } else if (arg == "--hash-every") {
            int_flag(o.hash_every, 0);
        } else if (arg == "--seed") {
            std::uint64_t v = 0;
            if (has_value && parse_u64(value, v)) {
                o.seed = v;
                ++i;
            } else {
                o.extra.emplace_back(arg);
            }
        } else if (arg == "--record-input") {
            string_flag(o.record_input);
        } else if (arg == "--replay") {
            string_flag(o.replay);
        } else if (arg == "--replay-any-build") {
            o.replay_any_build = true;
        } else if (arg == "--screenshot") {
            string_flag(o.screenshot);
        } else if (arg == "--print-schedule") {
            o.print_schedule = true;
        } else if (arg == "--no-vsync") {
            o.no_vsync = true;
        } else {
            o.extra.emplace_back(arg);
        }
    }
    return o;
}

} // namespace lam
