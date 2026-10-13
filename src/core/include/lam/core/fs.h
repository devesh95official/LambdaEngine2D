#pragma once
// File IO without exceptions. write_file_atomic: tmp + fsync (F_FULLFSYNC on macOS) + rename
// + directory fsync; never deletes the old file first.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <lam/core/result.h>
namespace lam {
Result<std::string> read_file(const std::string &path);
Result<std::vector<std::byte>> read_file_bytes(const std::string &path);
Status write_file_atomic(const std::string &path, std::string_view data);
bool file_exists(const std::string &path);
Status remove_file(const std::string &path);                    // missing file -> ok
Status make_directories(const std::string &path);
Result<std::int64_t> file_mtime(const std::string &path);       // nanoseconds since epoch
// Per-user writable folder for `app_name` (created). LAM_DATA_DIR env overrides (used as is).
Result<std::string> user_data_dir(std::string_view app_name);
Result<std::string> executable_dir();
std::string executable_name();                                  // file name without extension
std::string join_path(std::string_view a, std::string_view b);
std::string parent_path(std::string_view p);
// ".sounds.json" style: from the first dot of the file name; "" if none.
std::string_view extension_of(std::string_view path);
}
