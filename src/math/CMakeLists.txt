# lam_math: header-mostly maths, std only. The root of every dependency chain,
# so the build-wide switches (LAM_DEV, LAM_ENABLE_ASSERTS) are PUBLIC definitions here.
lam_add_module(NAME math
  SOURCES scalar.cpp vec2.cpp color.cpp transform.cpp geometry.cpp rng.cpp ease.cpp tween.cpp curve.cpp)
target_compile_definitions(lam_math PUBLIC
  LAM_DEV=$<IF:$<CONFIG:Release>,0,1>
  LAM_ENABLE_ASSERTS=$<IF:$<CONFIG:Release>,0,1>
  LAM_PROFILE=$<BOOL:${LAM_PROFILE}>)
