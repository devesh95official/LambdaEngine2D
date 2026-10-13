# lam_app: App, stage loop, plugins, Ctx, worlds, events and the lam_app engine systems
# Headers live under include/lam/core/ (same prefix as lam_core).
lam_add_module(NAME app TARGET lam_app
  SOURCES app.cpp app_frame.cpp app_systems.cpp app_worlds.cpp ctx.cpp launch_options.cpp
          null_platform.cpp
  DEPS lam_ecs)
