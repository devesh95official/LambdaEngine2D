# lam_ecs: entities, components, reflection, snapshots, prefabs and turns.
lam_add_module(NAME ecs
  SOURCES component.cpp world.cpp commands.cpp transform.cpp snapshot.cpp prefab.cpp turns.cpp
  DEPS lam_core)
