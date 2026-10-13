# lam_assets

The typed asset server: `AssetId<T>` loads cached by path and retained by scopes, loaders per
(type, extension), dependencies declared from `decode`, one IO/decode worker thread, main-thread
finalize in load order within a per-frame budget, `AssetEvent<T>`, garbage collection, mtime
hot reload in `LAM_DEV` builds, and the asset root resolved at runtime (no chdir).

```cpp
struct Palette { std::vector<lam::Color> colors; lam::AssetId<Palette> base; };
struct PaletteLoader final : lam::AssetLoader<Palette> {           // decode: worker thread, CPU only
    std::span<const std::string_view> extensions() const override { static constexpr std::string_view e[] = {".palette.json"}; return e; }
    lam::Result<Palette> decode(std::span<const std::byte> bytes, lam::LoadContext &ctx) override;  // ctx.depend<T>, ctx.read
    lam::Status finalize(Palette &p, lam::FinalizeContext &ctx) override;   // main thread; deps Ready
};
app.add_plugin<lam::AssetsPlugin>();                                // or DefaultPlugins
app.res<lam::AssetServer>().register_loader<Palette>(std::make_unique<PaletteLoader>());
// in a system: store the id, never the pointer
lam::AssetId<Palette> id = ctx.res<lam::AssetServer>().load<Palette>("palettes/day.palette.json");
for (const auto &e : ctx.read<lam::AssetEvent<Palette>>()) if (e.kind == lam::AssetEvent<Palette>::Kind::Modified) { /* hot reload */ }
if (const Palette *p = ctx.res<lam::AssetServer>().get(id)) use(*p);   // null until Ready
```

Public headers:
- `lam/assets/asset_server.h`: `AssetServer`, `AssetLoader<T>`, `LoadContext`,
  `FinalizeContext`, `AssetEvent<T>`, `AssetsConfig`, `Handle<T>`, `TextAsset`, `AssetsPlugin`.
- `lam/assets/asset_root.h`: `resolve_asset_root` (the root resolution order, with an
  explicit-input overload for tools and tests), `AssetRootSource`, `AssetRootResult`.

## Components, resources, events, systems

| Kind | Name | Scope | Stage / order | Notes |
|---|---|---|---|---|
| Resource | `AssetServer` | App | written FrameBegin -3000 | inserted by `AssetsPlugin`; also the App's `AssetResolver` |
| Event | `AssetEvent<T>` (`Loaded`, `Failed`, `Modified`, `Removed`) | App | emitted FrameBegin -3000 (`add`, synchronous loads, `reload`, `collect_garbage` emit at the call) | channel registered on first use of `T` |
| System | `asset_update` (`AssetServer::update`) | Once | FrameBegin -3000 | drain the worker, finalize in load order within `finalize_budget_ms`, hot-reload poll, GC of released assets |
| Loader | `TextAsset` (`.txt`) | — | — | built in |
| Components | none | | | components store plain `AssetId<T>` |

Startup tick hold: lam_app reads `AssetResolver::pending()` after the FrameBegin Once pass;
with `AppConfig::wait_for_startup_assets` (default) ticks wait until the startup loads finalized.

## Behaviour

- Loads are synchronous when `options().deterministic()` (`--headless`, `--step`, record,
  replay) or `AssetsConfig::synchronous` is set: decode, dependencies and finalize all run inside
  `load()` and no thread is started. Otherwise `load()` returns a Queued id right away, the worker
  reads and decodes, and `update()` finalizes on the main thread.
- Finalize order is the same in both modes: top-level loads and reloads in call order, each after
  its dependencies in declaration order. Async mode never finalizes an asset before every earlier
  one is done, so events and atlas packing don't depend on IO timing. A failed dependency fails
  its parent immediately.
- Top-level ids follow `load()` call order in both modes. Dependency ids come from the decode that
  declares them, so they're only deterministic in sync mode.
- `update()` finalizes within `finalize_budget_ms`, but always does at least one.
- `load_now`, `reload` and `wait_all` block the main thread. In async mode they move the decodes
  they need to the front of the worker queue and wait; decoding still only happens on the worker.
- Assets are kept alive by scopes (`begin_scope`/`end_scope`/`set_current_scope`), by parents
  that depend on them, and by `Handle<T>`. `collect_garbage()` (or the next `update()` after a
  release) unloads whatever nothing holds: `unload` callback, `Removed` event, then the index is
  reused with a higher generation so stale ids return null. A load whose scope ended while it was
  in flight is finalized, then collected.
- A reload decodes and finalizes a new value and swaps it in (`Modified`, or `Loaded` if the asset
  had failed). On failure the old value stays. Dependants reload too, and a reload that would
  close a dependency cycle fails.
- Hot reload (LAM_DEV, async mode, `hot_reload`) polls the mtime of every file a decode read,
  a round-robin slice per frame so the whole list is visited once per `watch_interval_seconds`.
  Changes during a reload coalesce, and missing files load when they appear.
- The root comes from `AssetsConfig::root`, `LAM_ASSET_ROOT`, `<exe>.assetroot` (LAM_DEV),
  `<exe_dir>/assets` or `<exe_dir>/../Resources/assets`, in that order, and is logged once.
- `AssetsPlugin::shutdown` ends the root scope: the worker is joined after its current decode,
  pending loads are cancelled (Failed, `Busy`) and everything is unloaded with callbacks. Loads
  after that run inline.
- Everything is main-thread only except `decode` and `LoadContext`.
- Steady-state `update()` allocates nothing, hot-reload polling included (tested with
  `CHECK_NO_ALLOC`). Loads, finalize, GC and reloads allocate.

## Tests and example

`tests/assets/` (`lam_assets_tests`): synchronous core, scopes and GC, the asynchronous worker,
hot reload, root resolution, edge cases across both modes, sync/async equivalence, budgets.
`examples/basics/05_assets`: a palette type with a `base` dependency, events, a scene scope and hot
reload in a window (`--hidden --frames N`), or in the console with `--headless`.
