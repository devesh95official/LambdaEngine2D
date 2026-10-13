# platformer: a LambdaEngine2D starter game, built as its own CMake project (see README.md).
#   cmake -S . -B build -G Ninja [-DLAMBDA_ENGINE_DIR=/path/to/LambdaEngine2D] [-DLAM_DEPS_DIR=...]
#   cmake --build build && ctest --test-dir build && build/platformer
# Targets: platformer_sim (rules + bot, pure engine targets only), platformer (the executable),
# lam_platformer_tests (the rule test, ctest label "platformer").
cmake_minimum_required(VERSION 3.28)
if(CMAKE_HOST_APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Android" AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3" CACHE STRING "Minimum macOS")   # std::format; before project()
endif()
project(platformer LANGUAGES C CXX)                             # TODO(rename): your game's name
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)   # Debug: F3 overlay, asset hot reload
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---- The engine -------------------------------------------------------------------------------
# The default works inside the engine repo. Elsewhere, pass -DLAMBDA_ENGINE_DIR (a checkout or a
# submodule); offline, also -DLAM_DEPS_DIR. As a subproject the engine builds only its modules.
set(LAMBDA_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "LambdaEngine2D source folder")
if(NOT EXISTS "${LAMBDA_ENGINE_DIR}/cmake/LamHelpers.cmake")
  message(FATAL_ERROR "LAMBDA_ENGINE_DIR (${LAMBDA_ENGINE_DIR}) is not a LambdaEngine2D checkout")
endif()
add_subdirectory(${LAMBDA_ENGINE_DIR} lambda_engine)            # engine targets + lam_add_game/test
# FetchContent alternative (no local checkout): replace the five lines above with
#   include(FetchContent)
#   FetchContent_Declare(lambda_engine
#     GIT_REPOSITORY <LambdaEngine2D git url>
#     GIT_TAG        <commit hash>)                  # pin a commit: replays are only valid per build
#   FetchContent_MakeAvailable(lambda_engine)
#   set(LAMBDA_ENGINE_DIR ${lambda_engine_SOURCE_DIR})            # the test block below reads it

# ---- The game ---------------------------------------------------------------------------------
# platformer_sim (rules + bot) links only raylib-free engine targets so it runs headless and in
# tests; the executable adds main.cpp and lam_engine. Include root is src/. Debug reads assets/ in
# place for hot reload.
# TODO(build): list new rule files in SIM_SOURCES and new pure modules (lam_animation, lam_scene,
# lam_ui, ...) in SIM_DEPS. Never put lam_engine or a *_rl backend in SIM_DEPS.
lam_add_game(NAME platformer
             SOURCES main.cpp
             ASSETS assets
             SIM_LIBRARY platformer_sim
             SIM_SOURCES src/platformer/rules.cpp src/platformer/bot.cpp
             SIM_DEPS lam_app lam_assets lam_physics lam_tilemap lam_input lam_audio lam_particles)

# ---- Tests ------------------------------------------------------------------------------------
option(GAME_BUILD_TESTS "Build the game's rule tests" ON)
if(GAME_BUILD_TESTS)
  enable_testing()
  if(NOT TARGET lam_test_support)
    # The engine only adds its test harness as the top-level project. Pull in doctest and
    # lam_test_support alone, so ctest runs just this game's tests.
    set(LAM_BUILD_TESTS ON)                                     # this scope only: turns on doctest
    include(${LAMBDA_ENGINE_DIR}/cmake/LamDependencies.cmake)   # raylib and json exist: skipped
    add_subdirectory(${LAMBDA_ENGINE_DIR}/tests/support lam_test_support)
  endif()
  add_subdirectory(tests)
endif()
