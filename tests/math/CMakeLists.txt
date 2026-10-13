# lam_math unit tests.
lam_add_test(MODULE math
  SOURCES test_scalar.cpp test_vec2.cpp test_rect.cpp test_color.cpp test_transform.cpp
          test_geometry.cpp test_rng.cpp test_ease.cpp test_grid.cpp test_tween.cpp test_curve.cpp
          test_dir4.cpp
  DEPS lam_math)
