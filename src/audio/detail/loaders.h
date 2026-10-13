#pragma once
// Registration of the lam_audio asset loaders (loaders.cpp), called by AudioPlugin::build.
#include <lam/assets/asset_server.h>
namespace lam::detail {
// SoundClip (.wav .ogg .mp3 .qoa .flac), MusicTrack (+ .xm .mod), SoundBank (.sounds.json).
void register_audio_loaders(AssetServer &assets);
}
