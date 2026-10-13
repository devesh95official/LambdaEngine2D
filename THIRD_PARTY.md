# Third-party code

No third-party source is stored in this repository. CMake downloads each library at configure time
and checks it against a pinned SHA-256 (`cmake/LamDependencies.cmake`); a changed upload or a moved
tag fails the configure.

| Library | Version | Licence | Used for | Linked into |
|---|---|---|---|---|
| [raylib](https://www.raylib.com) | 6.0 (commit `dbc56a87`), with `cmake/patches/raylib-6.0.patch` | zlib/libpng | window, input devices, OpenGL drawing, audio device | the backend targets only (`lam_platform`, `lam_render_rl`, `lam_input_rl`, `lam_audio_rl`), PRIVATE |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 | MIT | reading and writing JSON | `lam_core` only, PRIVATE (`src/core/data.cpp`) |
| [doctest](https://github.com/doctest/doctest) | 2.5.3 | MIT | unit tests | test executables only (`LAM_BUILD_TESTS`) |

raylib compiles in its own bundled libraries (GLFW, glad, stb, miniaudio, dr_libs, qoi/qoa and
others); their licences are in raylib's `src/external/` folder. The engine switches off raylib's
`SUPPORT_MODULE_RMODELS`, `SUPPORT_CAMERA_SYSTEM`, `SUPPORT_SCREEN_CAPTURE` and
`SUPPORT_AUTOMATION_EVENTS`.

## The raylib patch

`cmake/patches/raylib-6.0.patch` carries three fixes, each described at the top of the file:

1. Upstream fix 7ee0b08 (raylib PR #6040): an Android activity destroyed while unfocused no longer
   leaves the game thread blocked.
2. An OpenGL ES 3 build asks EGL for an ES 3 context (raylib 6.0 always asks for 2).
3. Upstream fix 1893226 (raylib PR #6039): `CloseAudioDevice()` no longer destroys the mixing mutex
   while the device's callback thread can still lock it.

The patch is hash-checked too: update `LAM_RAYLIB_PATCH_SHA256` after editing it.

## Offline builds

Pass `-DLAM_DEPS_DIR=<folder>` holding the archives under the file names used in
`cmake/LamDependencies.cmake` (`dbc56a87da87d973a9c5baa4e7438a9d20121d28.tar.gz`, `json.tar.xz`,
`doctest-v2.5.3.tar.gz`). Hashes are still checked.

## Example assets

The example games, demos and templates generate their art, maps and sounds with small Python
scripts (standard library only), in `tools/` or next to a demo's `main.cpp`. The sprites, levels,
sounds and music of Pathar, Lantern Run and Apple Trail were made by the engine's author. No
third-party asset packs or fonts are used; text uses the engine's built-in 6x8 pixel font.
