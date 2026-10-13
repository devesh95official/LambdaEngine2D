# Third-party libraries, fetched as hash-checked archives. Each block is skipped if the target
# already exists, so a game can bring its own copy.
include(FetchContent)
# Sources may be shared between build trees (FETCHCONTENT_BASE_DIR); build output never is.

set(LAM_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")

# Offline builds: point LAM_DEPS_DIR at a folder with the same archives (same file names as the
# URLs below). Hashes are still checked.
set(LAM_DEPS_DIR "" CACHE PATH "Folder with the dependency archives, for offline builds")
function(lam_dep_url out_var url)
  if(LAM_DEPS_DIR)
    get_filename_component(name "${url}" NAME)
    set(${out_var} "file://${LAM_DEPS_DIR}/${name}" PARENT_SCOPE)
  else()
    set(${out_var} "${url}" PARENT_SCOPE)
  endif()
endfunction()

# --- raylib 6.0 (commit dbc56a87) + 3-hunk patch ----------------------------------------------------
if(NOT TARGET raylib)
  if(ANDROID)
    set(PLATFORM "Android" CACHE STRING "raylib platform" FORCE)
    set(OPENGL_VERSION "ES 3.0" CACHE STRING "raylib graphics API" FORCE)
  endif()
  # The source directory is named after the patch's hash, so a changed patch is applied to a fresh
  # copy instead of an already patched one.
  set(LAM_RAYLIB_PATCH "${LAM_CMAKE_DIR}/patches/raylib-6.0.patch")
  file(SHA256 "${LAM_RAYLIB_PATCH}" LAM_RAYLIB_PATCH_HASH)
  set(LAM_RAYLIB_PATCH_SHA256 "22d2b241fba4de77c05b83e01479dd27ea1b76caa242e3c13992cece77459409")
  if(NOT LAM_RAYLIB_PATCH_HASH STREQUAL LAM_RAYLIB_PATCH_SHA256)
    message(FATAL_ERROR "cmake/patches/raylib-6.0.patch changed (SHA-256 ${LAM_RAYLIB_PATCH_HASH}). "
                        "Review it, then update LAM_RAYLIB_PATCH_SHA256.")
  endif()
  string(SUBSTRING "${LAM_RAYLIB_PATCH_HASH}" 0 12 LAM_RAYLIB_PATCH_ID)
  lam_dep_url(raylib_url https://github.com/raysan5/raylib/archive/dbc56a87da87d973a9c5baa4e7438a9d20121d28.tar.gz)
  FetchContent_Declare(raylib
    URL      ${raylib_url}
    URL_HASH SHA256=81b06ce7c19cf3b634b0271c23c361ba6ad8bf45fb8b036abbfeb4260ec1e126
    SOURCE_DIR "${FETCHCONTENT_BASE_DIR}/raylib-src-${LAM_RAYLIB_PATCH_ID}"
    BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/raylib-build"
    PATCH_COMMAND patch -p1 --forward --batch -i "${LAM_RAYLIB_PATCH}"
    DOWNLOAD_NO_PROGRESS TRUE
    SYSTEM
    EXCLUDE_FROM_ALL)
  # Never CUSTOMIZE_BUILD=ON on 6.0: it switches on SUPPORT_CUSTOM_FRAME_CONTROL, which stops
  # EndDrawing() from swapping buffers.
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(raylib)
  # A 2D engine needs no 3D models or camera system.
  target_compile_definitions(raylib PRIVATE
    SUPPORT_MODULE_RMODELS=0
    SUPPORT_CAMERA_SYSTEM=0
    SUPPORT_SCREEN_CAPTURE=0
    SUPPORT_AUTOMATION_EVENTS=0)
endif()

# --- nlohmann/json 3.12.0 (private to lam_core's data.cpp) ----------------------------------
if(NOT TARGET nlohmann_json::nlohmann_json)
  set(JSON_ImplicitConversions OFF)
  set(JSON_Diagnostics ON)
  lam_dep_url(json_url https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz)
  FetchContent_Declare(nlohmann_json
    URL      ${json_url}
    BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/nlohmann_json-build"
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    DOWNLOAD_NO_PROGRESS TRUE
    SYSTEM
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(nlohmann_json)
endif()

# --- doctest 2.5.3 (tests only) ----------------------------------------------------------------------
if(LAM_BUILD_TESTS AND NOT TARGET doctest::doctest)
  lam_dep_url(doctest_url https://github.com/doctest/doctest/releases/download/v2.5.3/doctest-v2.5.3.tar.gz)
  FetchContent_Declare(doctest
    URL      ${doctest_url}
    BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/doctest-build"
    URL_HASH SHA256=eb4a39729d091e43d4d593888bf4d145ee9a0f0e13f7404484b6f2fb60d5d50f
    DOWNLOAD_NO_PROGRESS TRUE
    SYSTEM
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(doctest)
endif()
