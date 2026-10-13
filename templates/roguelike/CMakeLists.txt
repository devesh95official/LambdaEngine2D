# Roguelike starter template (see README.md). Targets:
#   roguelike_sim          the rules (src/game/*.cpp), pure lam_* targets only (no raylib)
#   roguelike              the executable (src/main.cpp), links lam_engine
#   lam_roguelike_tests    the rule test: ctest --test-dir <build>
#
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug [-DLAMBDA_ENGINE_DIR=/path/to/engine]
#   cmake --build build && ./build/roguelike
cmake_minimum_required(VERSION 3.28)
if(CMAKE_HOST_APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Android" AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3" CACHE STRING "Minimum macOS")   # before project(), as the engine does
endif()
project(roguelike LANGUAGES C CXX)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)   # Debug: dev overlay (F3), hot reload
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- The engine ----------------------------------------------------------------------------------
# The default works inside the engine repo. Elsewhere, pass -DLAMBDA_ENGINE_DIR (a checkout or a
# submodule); offline, also -DLAM_DEPS_DIR.
set(LAMBDA_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "LambdaEngine2D source folder")
add_subdirectory(${LAMBDA_ENGINE_DIR} lambda_engine)   # its own tests and examples stay off here
# Or fetch it at a pinned commit instead of the add_subdirectory line, and point
# LAMBDA_ENGINE_DIR at it for the test harness below:
#   include(FetchContent)
#   FetchContent_Declare(lambda_engine
#     GIT_REPOSITORY <LambdaEngine2D git url>
#     GIT_TAG        <commit hash>)          # pin a commit: replays and hashes are per build
#   FetchContent_MakeAvailable(lambda_engine)
#   set(LAMBDA_ENGINE_DIR ${lambda_engine_SOURCE_DIR})

# --- The game ------------------------------------------------------------------------------------
# lam_add_game adds the engine's warning and determinism flags. Rule headers are included as
# <game/...> (the sim include root is src/).
# TODO(build): rule files in SIM_SOURCES, presentation files in SOURCES, extra pure modules
# (lam_animation, lam_scene, lam_ui, ...) in SIM_DEPS. Never lam_engine or a *_rl backend there.
lam_add_game(NAME roguelike
  SOURCES src/main.cpp
  ASSETS assets
  SIM_LIBRARY roguelike_sim
  SIM_SOURCES src/game/rules.cpp src/game/bot.cpp
  SIM_DEPS lam_app lam_ecs lam_input lam_physics lam_tilemap lam_audio)

# --- Tests ---------------------------------------------------------------------------------------
option(GAME_BUILD_TESTS "Build this game's rule tests" ON)
if(GAME_BUILD_TESTS)
  if(NOT TARGET lam_test_support)
    # As a subproject the engine skips its test harness. Add just the harness; re-reading the
    # dependency file with LAM_BUILD_TESTS on fetches doctest and skips what is already there.
    set(LAM_BUILD_TESTS ON)
    include(${LAMBDA_ENGINE_DIR}/cmake/LamDependencies.cmake)
    add_subdirectory(${LAMBDA_ENGINE_DIR}/tests/support lam_test_support EXCLUDE_FROM_ALL)
  endif()
  enable_testing()
  add_subdirectory(tests)
endif()
