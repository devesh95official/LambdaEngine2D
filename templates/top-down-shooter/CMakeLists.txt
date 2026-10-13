# Top-down shooter starter template (see README.md). Targets:
#   top_down_shooter_sim         the rules (src/game/*.cpp), pure lam_* targets only (no raylib)
#   top_down_shooter             the executable (src/main.cpp), links lam_engine
#   lam_top_down_shooter_tests   ctest --test-dir <build> -L top_down_shooter
#
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug [-DLAMBDA_ENGINE_DIR=/path/to/engine]
#   cmake --build build
cmake_minimum_required(VERSION 3.28)
if(CMAKE_HOST_APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Android" AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3" CACHE STRING "Minimum macOS")   # before project(), as the engine does
endif()
project(top_down_shooter LANGUAGES C CXX)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)   # Debug/RelWithDebInfo: dev overlay (F3)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- The engine ------------------------------------------------------------------------------------
# The default works inside the engine repo; elsewhere pass -DLAMBDA_ENGINE_DIR.
set(LAMBDA_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "LambdaEngine2D source folder")
option(GAME_BUILD_TESTS "Build this game's rule tests" ON)
# LAM_BUILD_TESTS gives us the engine's test harness. EXCLUDE_FROM_ALL keeps the engine's own tests
# out of the build. Offline: -DLAM_DEPS_DIR=<archives>.
set(LAM_BUILD_TESTS ${GAME_BUILD_TESTS})
add_subdirectory(${LAMBDA_ENGINE_DIR} lambda_engine EXCLUDE_FROM_ALL)
# Or fetch it at a pinned commit instead of the add_subdirectory line:
#   include(FetchContent)
#   FetchContent_Declare(lambda_engine
#     GIT_REPOSITORY <LambdaEngine2D git url>
#     GIT_TAG        <commit hash>          # pin a commit: replays are only valid per build
#     EXCLUDE_FROM_ALL)
#   FetchContent_MakeAvailable(lambda_engine)

# --- The game --------------------------------------------------------------------------------------
# lam_add_game adds the engine's warning and determinism flags. Rules are included as <game/...>.
# TODO(build): rule files in SIM_SOURCES, view files in SOURCES, extra pure modules in SIM_DEPS.
# Never lam_engine or a *_rl backend in SIM_DEPS.
lam_add_game(NAME top_down_shooter
  SOURCES src/main.cpp
  ASSETS assets
  SIM_LIBRARY top_down_shooter_sim
  SIM_SOURCES src/game/rules.cpp src/game/bot.cpp
  SIM_DEPS lam_app lam_ecs lam_input lam_physics lam_particles lam_audio)

if(GAME_BUILD_TESTS AND TARGET lam_test_support)
  enable_testing()
  add_subdirectory(tests)
endif()
