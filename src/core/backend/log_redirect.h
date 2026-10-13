#pragma once
// Routes raylib's TraceLog into the engine LogSink. Private to lam_platform.
namespace lam::detail {
// Call before InitWindow so start-up lines are routed too. Idempotent. Lines over 1 KiB are cut.
void install_raylib_log_redirect();
} // namespace lam::detail
