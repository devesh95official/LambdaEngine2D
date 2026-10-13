// Decode step: read the file, run the loader's decode, record the dependencies it declares.
// Runs on the worker (async) or inline (sync). Touches only the job, and the registry under mutex.
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <lam/core/assert.h>
#include <lam/core/fs.h>

#include "detail/asset_internal.h"

namespace lam {

using detail::Job;

namespace {

// The decode running on this thread, so LoadContext can find its job without changing its public
// layout. Decodes don't nest, but restore the previous frame anyway.
struct DecodeFrame {
    const AssetServer *server = nullptr;
    void *impl = nullptr;                   // AssetServer::Impl (private type)
    std::uint64_t (*depend)(void *impl, Job &job, TypeId type, const std::string &path) = nullptr;
    Job *job = nullptr;
};
thread_local DecodeFrame *t_decode = nullptr;

} // namespace

std::int64_t detail::file_mtime_ns(const std::filesystem::path &file) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(file, ec);
    if (ec) return -1;
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count());
}

void AssetServer::Impl::decode_job(Job &job) {
    const std::string absolute = self.resolve_path(job.path);
    if (job.watch) { // mtime before reading: a write during the read is seen as a later change
        job.files.push_back({absolute, detail::file_mtime_ns(std::filesystem::path(absolute))});
    }
    Result<std::vector<std::byte>> bytes = read_file_bytes(absolute);
    if (!bytes) {
        job.error = make_error(bytes.error().code, job.path + ": " + bytes.error().message);
        return;
    }
    DecodeFrame frame{&self, this, &Impl::depend_thunk, &job};
    DecodeFrame *const saved = t_decode;
    t_decode = &frame;
    LoadContext ctx(self, job.path, detail::asset_bits(job.ref.index, job.generation), job.ref.type);
    Result<void *> value = make_error(ErrorCode::Unknown, "decode did not run");
    try {
        value = job.loader->decode_erased(*bytes, ctx);
    } catch (...) { // never let a loader's exception kill the worker thread
        value = make_error(ErrorCode::Unknown, "decode threw an exception");
    }
    t_decode = saved;
    if (!value) {
        Error e = value.error();
        e.message = detail::name_file(job.path, e.message);
        job.error = std::move(e);
        return;
    }
    if (job.dep_error) {
        job.loader->destroy_erased(value.value());
        job.error = make_error(job.dep_error->code, job.path + ": " + job.dep_error->message);
        return;
    }
    job.value = value.value();
}

std::uint64_t AssetServer::Impl::depend_thunk(void *impl, Job &job, TypeId type,
                                              const std::string &path) {
    return static_cast<Impl *>(impl)->depend(job, type, path);
}

// LoadContext::depend: finds or allocates the registry entry of `path` and pins it for this job.
std::uint64_t AssetServer::Impl::depend(Job &job, TypeId type, const std::string &path) {
    for (const detail::DepRecord &rec : job.dep_records) {
        if (rec.ref.type == type && rec.path == path)
            return detail::asset_bits(rec.ref.index, rec.generation);
    }
    detail::Lookup lk;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        lk = lookup_locked(type, path, true, true);
    }
    if (!lk.ok) {
        if (!job.dep_error) {
            job.dep_error = make_error(ErrorCode::Unsupported,
                                       "no loader for dependency '" + path + "' of this type");
        }
        return 0;
    }
    job.dep_records.push_back({lk.ref, lk.generation, path, lk.loader});
    return detail::asset_bits(lk.ref.index, lk.generation);
}

// ---- LoadContext -------------------------------------------------------------------------------

LoadContext::LoadContext(AssetServer &server, std::string_view path, std::uint64_t parent_bits,
                         TypeId parent_type)
    : server_(&server), path_(path), parent_bits_(parent_bits), parent_type_(parent_type) {}

std::string_view LoadContext::path() const {
    return path_;
}

Result<std::vector<std::byte>> LoadContext::read(std::string_view relative_path) const {
    const std::string full = detail::sibling_asset_path(path_, relative_path);
    const std::string absolute = server_->resolve_path(full);
    DecodeFrame *frame = t_decode;
    const bool watch = frame != nullptr && frame->server == server_ && frame->job->watch;
    const std::int64_t mtime = watch ? detail::file_mtime_ns(std::filesystem::path(absolute)) : -1;
    Result<std::vector<std::byte>> bytes = read_file_bytes(absolute);
    if (watch) frame->job->files.push_back({absolute, mtime}); // also when missing: creating it reloads
    if (!bytes) return make_error(bytes.error().code, full + ": " + bytes.error().message);
    return bytes;
}

std::uint64_t LoadContext::depend_erased(TypeId type, std::string_view relative_path) {
    const std::string full = detail::sibling_asset_path(path_, relative_path);
    DecodeFrame *frame = t_decode;
    if (frame != nullptr && frame->server == server_)
        return frame->depend(frame->impl, *frame->job, type, full);
    // Outside a server-driven decode (a loader called directly, e.g. from a test): a plain load
    // retained by the current scope. Main thread only (AssetServer::load asserts it).
    (void)parent_bits_;
    (void)parent_type_;
    return server_->load_erased(type, full, server_->current_scope());
}

} // namespace lam
