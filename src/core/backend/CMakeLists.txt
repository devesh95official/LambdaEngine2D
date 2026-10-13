# lam_rl_common and lam_platform. raylib appears only in
# backend .cpp files: lam_platform links it PRIVATE, and lam_rl_common carries it to the other
# backends together with <lam/rl/convert.h>.
add_library(lam_rl_common INTERFACE)
target_include_directories(lam_rl_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/rl)
target_link_libraries(lam_rl_common INTERFACE lam_math raylib)

# TARGET override, so the name is lam_platform, not lam_core_rl. The public header lives in
# include/ here (lam_add_backend only looks for backend/include under the calling folder).
lam_add_backend(NAME core TARGET lam_platform SOURCES raylib_platform.cpp log_redirect.cpp
  DEPS lam_app)
target_include_directories(lam_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
