# lam_ui tests. All headless, using the built-in theme and 6x8 fallback metrics, or the
# lam_test_assets monospace font where glyphs are counted.
lam_add_test(MODULE ui
  SOURCES
    test_smoke.cpp
    test_tree.cpp
    test_layout.cpp
    test_navigation.cpp
    test_pointer.cpp
    test_draw.cpp
    test_theme.cpp
    test_toasts_log.cpp
    test_dialogue.cpp
    test_plugin.cpp
    test_menus.cpp
    test_regressions.cpp
  DEPS lam_ui lam_test_assets)
