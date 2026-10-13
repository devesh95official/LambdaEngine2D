# lam_audio

The `Audio` service: sound clips, streamed music, sound banks with non-repeating variants and pitch
jitter, a bus tree whose volumes live in `SettingsStore`, voice handles with per-group and global voice
limits, crossfaded music, stingers that duck the music, ambience layers, spatial mixing from an
`AudioListener`, and the `PlaySound` World event for simulation code. `NullAudioDevice` records calls in
headless runs and tests; the raylib backend `lam_audio_rl` plays them.

```cpp
app.add_plugin<AssetsPlugin>();
app.add_plugin<AudioPlugin>();                        // + RaylibAudioPlugin when windowed (DefaultPlugins does both)
// startup: assets and banks
Audio &audio = ctx.res<Audio>();
audio.add_bank(ctx.res<AssetServer>().load<SoundBank>("sounds/sfx.sounds.json"));
audio.play_music(ctx.res<AssetServer>().load<MusicTrack>("music/theme.ogg"));      // crossfades, loops
// tick systems never call Audio: they emit an event, played once in the same frame (Update -900)
ctx.emit(PlaySound{.group = "coin"_id, .position = pos, .spatial = true});
// frame code (UI, menus) may call Audio directly; handles go stale safely
VoiceHandle v = audio.play("click"_id, {.bus = audio.bus("ui")});
audio.set_bus_volume(audio.bus("music"), 0.5f);        // saved as "audio.music.volume"
```

## Components, resources, events, systems

| Kind | Name | Scope / flags | Stage, scope, order | Notes |
|---|---|---|---|---|
| Component | `AudioListener` | S N | read in Update -900 | first entity with `AudioListener` + `Transform` per world; the focused world's becomes `Audio::set_listener` |
| Component | `AudioEmitter` | S N | read in Update -900 | needs `Transform`; `loop` emitters play while in range (stop out of range, restart when back); one-shots play once per `AudioVoice` |
| Component | `AudioVoice` | runtime (unreflected) | written in Update -900 | the emitter's voice handle; pruned on restore and re-acquired; removed when the `AudioEmitter` goes |
| Resource | `Audio` | App | Update | inserted by `AudioPlugin::build`; safe without a device |
| Event | `PlaySound` | World, capacity 64 | emitted in any tick stage; read Update -900 | spatial events are mixed against that world's listener; every event plays exactly once |
| Event | `LifecycleChanged` | App (lam_app) | read Update +1000 | `Background` suspends, `Foreground` resumes; `FocusLost` mutes if `audio.mute_unfocused` |
| System | `audio_world` | | Update, PerWorld, -900 | listener, `PlaySound`, emitters, orphan sweep |
| System | `audio_update` | | Update, Once, +1000 | lifecycle, then `Audio::update(frame_dt)`: settings sync, bank refresh, fades, voices, music, streams |

`RaylibAudioPlugin` (target `lam_audio_rl`) adds no systems: `build()` installs `RaylibAudioDevice` (silent
when `IsAudioDeviceReady()` is false or `AppConfig::audio` is off), `shutdown()` puts a `NullAudioDevice`
back, which unloads every raylib sound, alias and stream before `PlatformPlugin` closes the device.

## Buses and settings
- Default tree: `master` (0) with `sfx` (1), `ui` (2), `music` (3) and `ambience` (4) under it.
  `add_bus(name, parent)` adds more (at most `Audio::kMaxBuses` = 32). A bus's gain is its own volume
  times every ancestor's (`bus_gain`).
- Every bus volume is the setting `"audio.<name>.volume"` (float [0, 1], default 1) in the App's
  `SettingsStore`: `set_bus_volume` writes it, and changes made elsewhere (a settings menu) are applied by
  the next `update()`. `"audio.mute_unfocused"` (bool, default false) mutes while the window is unfocused.
- Music streams use the `music` bus, ambience the `ambience` bus; clips default to `sfx`, groups to their
  bank's bus.

## Sound banks (`*.sounds.json`)
```json
{
  "bus": "sfx",
  "groups": {
    "blip": {"file": "blip.wav", "variants": 3, "pitch_jitter": 0.06, "max_voices": 4},
    "coin": {"files": ["coin.wav"], "bus": "ui", "pitch_jitter": 0, "min_interval_ms": 60},
    "step": {"files": ["step_a.ogg", "step_b.ogg"], "volume": 0.7}
  }
}
```
- `"bus"` (optional, default `"sfx"`): the default bus of every group.
- Each group has exactly one of `"files"` (a list) or `"file"` + optional `"variants": n`, which expands to
  `<stem>_1<ext>` .. `<stem>_n<ext>`. Paths are relative to the bank file. Every variant is a dependency,
  so the bank is Ready only when all of its clips are, and fails as a whole if one fails.
- Optional: `"volume"` [0, 2] (1), `"pitch_jitter"` [0, 0.5] (0.04: pitch × 1 ± j), `"bus"`,
  `"max_voices"` [1, 64] (3), `"min_interval_ms"` [0, 60000] (30). Unknown keys are errors (strict
  `DataReader`); errors name the file and the key.
- A play never repeats the previous variant (uniform among the others). Variants and jitter use a
  presentation RNG seeded from the App seed: deterministic per seed, never hashed.

## Voices
- `play()` returns a `VoiceHandle` (index + generation; null = not played). Stale handles are ignored by
  every call, so code may keep handles forever. A slot whose generation reaches `UINT32_MAX` is retired.
- Limits: a group's `max_voices` steals that group's oldest voice (fading-out ones first). Beyond the
  global `voice_limit()` (default `Audio::kMaxVoices` = 64) the oldest fading-out voice is stolen, else the
  oldest one-shot; if every voice loops the play is dropped (not counted).
- Throttles run on real time advanced by `update(frame_dt)` every frame: a group's
  `min_interval_ms`, and `set_clip_min_interval` (default 0.03 s) for direct clip plays.
- Spatial plays (`PlayParams::spatial`) use `spatial_mix(listener, source, range)`: full gain within
  range/4, then a quadratic falloff to 0 at range; pan = dx / range clamped to ±0.8. A spatial one-shot
  out of range is not played.
- `stop(v, fade)` fades linearly. Loops restart when the device reports the slot idle.

## Music, stingers, ambience
- `play_music(track, crossfade)`: linear crossfade; the old track closes once silent; the same track again
  does nothing; a null id fades out. `music()` is the requested track.
- `play_stinger(track, duck)`: plays once over the music, which ducks to `duck` in 0.25 s and comes back
  in 1 s after the stinger ends (device end, or `MusicTrack::seconds` when the device cannot tell).
- `set_music_duck(gain, fade)`: pause-menu style duck, multiplied with the stinger duck (`music_duck()`).
- `set_ambience(loop, gain, fade)`: independent looping layers; gain 0 fades out and closes.
- At most `Audio::kMaxStreams` = 16 streams together. Streams open once their `MusicTrack` is Ready and
  decode from the bytes the asset server read (APK-safe); `MusicTrack::path` is the fallback.

## Lifecycle, devices, headless
- `set_suspended(true)` (on `LifecycleChanged::Background`) pauses the device, silences and freezes voices
  and streams and drops plays until resumed. `set_muted` mutes without pausing.
- `set_device` replaces the device: voices stop, streams reopen on the new device, clips uploaded to the
  old one stay silent until reloaded (install the backend before loading sounds; `DefaultPlugins` does).
- A clip exists only after the device accepted it; without audio hardware clips are Ready and
  silent, and plays still count (`plays()`).
- `NullAudioDevice` (headless, tests) records every call (up to `kMaxRecordedCalls`), simulates slot and
  stream state (`finish`, `finish_stream`) and reserves its memory once.

## Determinism and budgets
- Audio is presentation only. Nothing in it is hashed or snapshotted, and tick systems only emit
  `PlaySound`; `Audio` itself is called from frame stages (see GUIDE.md).
- No heap allocations in steady state: `play`, the voice setters, `update`, `audio_world` and raylib
  playback (aliases are created once per slot, then reused) use fixed arrays. `add_bus`, `add_bank`,
  `attach_settings`, loads and the first use of a bank allocate. The tests check this with
  `CHECK_NO_ALLOC` over 600 busy ticks.

## Files
| File | Contents |
|---|---|
| `include/lam/audio/audio.h` | `Audio`, `SoundClip`, `MusicTrack`, `SoundBank`, `PlayParams`, `spatial_mix`, `encoded_sound_seconds` |
| `include/lam/audio/device.h` | `AudioDevice`, `NullAudioDevice` |
| `include/lam/audio/components.h` | `VoiceHandle`, `AudioListener`, `AudioEmitter`, `AudioVoice`, `PlaySound` |
| `include/lam/audio/audio_plugin.h` | `AudioPlugin`, `RaylibAudioPlugin` |
| `audio.cpp` | buses, settings sync, voices, limits, throttles, banks and groups |
| `audio_streams.cpp` | music, stingers, ambience |
| `audio_plugin.cpp` | `AudioPlugin`, `audio_world`, `audio_update` |
| `loaders.cpp` | `SoundClip`, `MusicTrack`, `SoundBank` loaders |
| `null_device.cpp`, `sound_math.cpp` | `NullAudioDevice`; `spatial_mix`, header-only sound lengths (WAV, Ogg, QOA, FLAC) |
| `backend/raylib_audio.cpp` | `RaylibAudioDevice`, `RaylibAudioPlugin` (`lam_audio_rl`) |

Example: `examples/basics/12_audio` (windowed, `--hidden --frames N`, `--headless`). Tests:
`tests/audio`.
