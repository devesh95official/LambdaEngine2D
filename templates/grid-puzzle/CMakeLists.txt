# Grid Puzzle: a LambdaEngine2D starter template (Sokoban-style grid puzzle). See README.md.
#
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug [-DLAMBDA_ENGINE_DIR=/path/to/LambdaEngine2D]
#   cmake --build build -j
#   build/grid_puzzle                     # play
#   build/grid_puzzle_tests               # the rules test (or: ctest --test-dir build -L grid_puzzle)
cmake_minimum_required(VERSION 3.28)
if(CMAKE_HOST_APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3" CACHE STRING "Minimum macOS")   # as the engine: before project()
endif()
project(grid_puzzle LANGUAGES C CXX)                    # TODO(rename): your game's name
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)   # Debug/RelWithDebInfo: LAM_DEV tools on
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- The engine ------------------------------------------------------------------------------------
# A local checkout. The default works while this folder sits in the engine repo (templates/<name>);
# a copy elsewhere passes -DLAMBDA_ENGINE_DIR=/path/to/LambdaEngine2D.
set(LAMBDA_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "LambdaEngine2D checkout")
# Or fetch it instead (pin a commit, never a branch). Replace the add_subdirectory() below with:
#   include(FetchContent)
#   FetchContent_Declare(lambda_engine
#     GIT_REPOSITORY <the engine's git URL>
#     GIT_TAG        <commit sha>
#     EXCLUDE_FROM_ALL)
#   FetchContent_MakeAvailable(lambda_engine)
# Offline builds of either kind: -DLAM_DEPS_DIR=<folder with the raylib/json/doctest archives>.
option(GRID_PUZZLE_TESTS "Build the rules test (needs the engine's doctest harness)" ON)
set(LAM_BUILD_TESTS ${GRID_PUZZLE_TESTS})               # the engine's lam_test_support (+ doctest)
set(LAM_BUILD_EXAMPLES OFF)
if(NOT EXISTS "${LAMBDA_ENGINE_DIR}/cmake/LamHelpers.cmake")
  message(FATAL_ERROR "No LambdaEngine2D checkout at '${LAMBDA_ENGINE_DIR}'. "
                      "Configure with -DLAMBDA_ENGINE_DIR=/path/to/LambdaEngine2D (see README.md).")
endif()
# EXCLUDE_FROM_ALL: only the engine targets this game links are built (not its tests or demos).
add_subdirectory(${LAMBDA_ENGINE_DIR} lambda_engine EXCLUDE_FROM_ALL)

# --- The game --------------------------------------------------------------------------------------
# SIM_LIBRARY: src/ builds into grid_puzzle_sim, which may link only pure engine targets (no raylib),
# so the rules run in tests and --headless. The executable links lam_engine (everything) + the sim.
lam_add_game(NAME grid_puzzle
  SOURCES main.cpp
  ASSETS assets                                         # copied next to the exe; Debug reads the source
  SIM_LIBRARY grid_puzzle_sim                           # include root: src/
  SIM_SOURCES src/grid_puzzle/rules.cpp src/grid_puzzle/bot.cpp
  SIM_DEPS lam_app lam_physics lam_tilemap lam_input lam_audio lam_particles)

if(GRID_PUZZLE_TESTS AND TARGET lam_test_support)
  enable_testing()
  # Executable grid_puzzle_tests, ctest label "grid_puzzle" (the engine's tests are registered but
  # not built here, so run ctest with -L grid_puzzle).
  lam_add_test(MODULE grid_puzzle NAME grid_puzzle_tests SOURCES tests/test_rules.cpp DEPS grid_puzzle_sim)
  target_compile_definitions(grid_puzzle_tests PRIVATE "GRID_PUZZLE_ASSETS=\"${CMAKE_CURRENT_SOURCE_DIR}/assets\"")
endif()
