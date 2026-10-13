#include <lam/core/fs.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace lam {
namespace {

Error io_error(const std::string &path, std::string_view what) {
    return make_error(ErrorCode::Io, path + ": " + std::string(what) + ": " + std::strerror(errno));
}

// fsync, plus F_FULLFSYNC on macOS where plain fsync does not reach the platter.
bool full_sync(int fd) {
#if defined(__APPLE__)
    if (fcntl(fd, F_FULLFSYNC) == 0) return true;
#endif
    return fsync(fd) == 0;
}

} // namespace

Result<std::string> read_file(const std::string &path) {
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (errno == ENOENT) return make_error(ErrorCode::NotFound, path + ": file not found");
        return io_error(path, "cannot open");
    }
    std::string data;
    char buf[65536];
    for (;;) {
        const std::size_t n = std::fread(buf, 1, sizeof buf, f);
        data.append(buf, n);
        if (n < sizeof buf) break;
    }
    const bool bad = std::ferror(f) != 0;
    std::fclose(f);
    if (bad) return io_error(path, "read failed");
    return data;
}

Result<std::vector<std::byte>> read_file_bytes(const std::string &path) {
    auto text = read_file(path);
    if (!text) return text.error();
    std::vector<std::byte> bytes(text->size());
    if (!bytes.empty()) std::memcpy(bytes.data(), text->data(), text->size());
    return bytes;
}

Status write_file_atomic(const std::string &path, std::string_view data) {
    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return io_error(tmp, "cannot create");
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            const Error e = io_error(tmp, "write failed");
            ::close(fd);
            ::unlink(tmp.c_str());
            return e;
        }
        done += static_cast<std::size_t>(n);
    }
    if (!full_sync(fd)) {
        const Error e = io_error(tmp, "fsync failed");
        ::close(fd);
        ::unlink(tmp.c_str());
        return e;
    }
    ::close(fd);
    // rename() replaces the old file atomically; the old file is never deleted first.
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        const Error e = io_error(path, "rename failed");
        ::unlink(tmp.c_str());
        return e;
    }
    const std::string dir = parent_path(path);
    const int dfd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY);
    if (dfd >= 0) {
        full_sync(dfd);
        ::close(dfd);
    }
    return {};
}

bool file_exists(const std::string &path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0;
}

Status remove_file(const std::string &path) {
    if (::unlink(path.c_str()) == 0 || errno == ENOENT) return {};
    return io_error(path, "cannot remove");
}

Status make_directories(const std::string &path) {
    if (path.empty()) return {};
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) return make_error(ErrorCode::Io, path + ": cannot create directory: " + ec.message());
    return {};
}

Result<std::int64_t> file_mtime(const std::string &path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) {
        if (errno == ENOENT) return make_error(ErrorCode::NotFound, path + ": file not found");
        return io_error(path, "cannot stat");
    }
#if defined(__APPLE__)
    return static_cast<std::int64_t>(st.st_mtimespec.tv_sec) * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
    return static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
#endif
}

Result<std::string> user_data_dir(std::string_view app_name) {
    std::string dir;
    if (const char *env = std::getenv("LAM_DATA_DIR"); env && *env) {
        dir = env;
    } else {
        const char *home = std::getenv("HOME");
        if (!home || !*home) return make_error(ErrorCode::NotFound, "user_data_dir: HOME is not set");
#if defined(__APPLE__)
        dir = join_path(join_path(home, "Library/Application Support"), app_name);
#else
        const char *xdg = std::getenv("XDG_DATA_HOME");
        dir = join_path(xdg && *xdg ? std::string(xdg) : join_path(home, ".local/share"), app_name);
#endif
    }
    LAM_TRY(make_directories(dir));
    return dir;
}

Result<std::string> executable_dir() {
#if defined(__APPLE__)
    char buf[4096];
    std::uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) != 0) return make_error(ErrorCode::Io, "executable path too long");
    std::error_code ec;
    const auto p = std::filesystem::weakly_canonical(buf, ec);
    return ec ? parent_path(buf) : p.parent_path().string();
#else
    std::error_code ec;
    const auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) return make_error(ErrorCode::Io, "cannot read /proc/self/exe");
    return p.parent_path().string();
#endif
}

std::string executable_name() {
#if defined(__APPLE__)
    char buf[4096];
    std::uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) != 0) return {};
    return std::filesystem::path(buf).stem().string();
#else
    std::error_code ec;
    const auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::string{} : p.stem().string();
#endif
}

std::string join_path(std::string_view a, std::string_view b) {
    if (a.empty()) return std::string(b);
    if (b.empty()) return std::string(a);
    if (b.front() == '/') return std::string(b);
    std::string out(a);
    if (out.back() != '/') out += '/';
    out += b;
    return out;
}

std::string parent_path(std::string_view p) {
    while (p.size() > 1 && p.back() == '/') p.remove_suffix(1);
    const auto slash = p.rfind('/');
    if (slash == std::string_view::npos) return {};
    if (slash == 0) return "/";
    return std::string(p.substr(0, slash));
}

std::string_view extension_of(std::string_view path) {
    const auto slash = path.rfind('/');
    std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const auto dot = name.find('.', name.empty() || name.front() != '.' ? 0 : 1);
    if (dot == std::string_view::npos || dot == 0) return {};
    return name.substr(dot);
}

} // namespace lam
