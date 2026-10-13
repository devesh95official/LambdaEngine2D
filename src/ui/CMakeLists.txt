# lam_ui: retained UiTree, themes, toasts, message log, dialogue and menu scenes. No backend.
lam_add_module(NAME ui
  SOURCES
    ui_tree.cpp
    ui_layout.cpp
    ui_input.cpp
    ui_draw.cpp
    ui_theme.cpp
    toasts.cpp
    message_log.cpp
    dialogue.cpp
    menu_scenes.cpp
    ui_plugin.cpp
  DEPS lam_render lam_input lam_scene lam_audio)
target_include_directories(lam_ui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
