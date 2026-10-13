// SoundClip, MusicTrack and SoundBank loaders. Clips upload at finalize and drop their bytes;
// tracks keep theirs to stream from memory.
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <lam/assets/asset_server.h>
#include <lam/audio/audio.h>
#include <lam/core/data.h>
#include <lam/core/id.h>
#include <lam/core/result.h>

#include "detail/audio_impl.h"
#include "detail/loaders.h"

namespace lam::detail {

namespace {

// Lower-case extension of the file name in `path`, with the dot (".wav"); "" if none.
std::string extension_of(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos) return {};
    std::string ext(name.substr(dot));
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

class SoundClipLoader final : public AssetLoader<SoundClip> {
public:
    std::span<const std::string_view> extensions() const override {
        static constexpr std::string_view kExt[] = {".wav", ".ogg", ".mp3", ".qoa", ".flac"};
        return kExt;
    }
    Result<SoundClip> decode(std::span<const std::byte> bytes, LoadContext &ctx) override {
        if (bytes.empty()) return make_error(ErrorCode::Invalid, "empty sound file");
        SoundClip clip;
        clip.extension = extension_of(ctx.path());
        clip.seconds = encoded_sound_seconds(bytes, clip.extension);
        if (clip.extension == ".wav" && !(clip.seconds > 0.0f))
            return make_error(ErrorCode::Parse, "not a RIFF/WAVE file with fmt and data chunks");
        clip.encoded.assign(bytes.begin(), bytes.end());
        return clip;
    }
    Status finalize(SoundClip &clip, FinalizeContext &ctx) override {
        Audio *audio = ctx.app.try_res<Audio>();
        if (audio == nullptr) return make_error(ErrorCode::Invalid, "no Audio resource (add AudioPlugin)");
        return AudioAccess::upload_clip(*audio, clip);
    }
    void unload(SoundClip &clip, FinalizeContext &ctx) override {
        if (Audio *audio = ctx.app.try_res<Audio>()) AudioAccess::release_clip(*audio, clip);
    }
};

class MusicTrackLoader final : public AssetLoader<MusicTrack> {
public:
    std::span<const std::string_view> extensions() const override {
        static constexpr std::string_view kExt[] = {".ogg", ".mp3", ".wav", ".qoa", ".flac", ".xm", ".mod"};
        return kExt;
    }
    Result<MusicTrack> decode(std::span<const std::byte> bytes, LoadContext &ctx) override {
        if (bytes.empty()) return make_error(ErrorCode::Invalid, "empty music file");
        MusicTrack track;
        track.extension = extension_of(ctx.path());
        track.seconds = encoded_sound_seconds(bytes, track.extension);
        track.encoded.assign(bytes.begin(), bytes.end());
        return track;
    }
    Status finalize(MusicTrack &track, FinalizeContext &ctx) override {
        track.path = ctx.server.resolve_path(ctx.path);
        return {};
    }
    void unload(MusicTrack &track, FinalizeContext &ctx) override {
        if (Audio *audio = ctx.app.try_res<Audio>()) AudioAccess::release_track(*audio, track);
    }
};

// "<stem>_<n><ext>" for n in 1..count, or the file itself for count 1.
void add_variants(SoundBank::Group &group, std::string_view file, int count, LoadContext &ctx) {
    if (count <= 1) {
        group.variants.push_back(ctx.depend<SoundClip>(file));
        return;
    }
    const std::size_t slash = file.find_last_of("/\\");
    const std::size_t dot = file.rfind('.');
    const bool has_ext = dot != std::string_view::npos && (slash == std::string_view::npos || dot > slash);
    const std::string_view stem = has_ext ? file.substr(0, dot) : file;
    const std::string_view ext = has_ext ? file.substr(dot) : std::string_view();
    for (int n = 1; n <= count; ++n) {
        std::string name(stem);
        name += '_';
        name += std::to_string(n);
        name += ext;
        group.variants.push_back(ctx.depend<SoundClip>(name));
    }
}

class SoundBankLoader final : public AssetLoader<SoundBank> {
public:
    std::span<const std::string_view> extensions() const override {
        static constexpr std::string_view kExt[] = {".sounds.json"};
        return kExt;
    }
    Result<SoundBank> decode(std::span<const std::byte> bytes, LoadContext &ctx) override {
        const std::string_view text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        return parse_data<SoundBank>(text, ctx.path(), [&ctx](DataReader &root) {
            SoundBank bank;
            const std::string default_bus = root.text_or("bus", "sfx");
            DataReader groups = root.object("groups");
            if (!groups.valid()) return bank;
            for (std::string_view name : groups.keys()) {
                DataReader g = groups.object(name);
                if (!g.valid()) continue;
                SoundBank::Group group;
                if (name.empty()) {
                    g.fail("a group needs a non-empty name");
                    continue;
                }
                group.id = intern(name);
                const bool has_files = g.has("files");
                const bool has_file = g.has("file");
                if (has_files == has_file) {
                    g.fail("a group needs exactly one of \"files\" (a list) or \"file\" (+ \"variants\")");
                    continue;
                }
                if (has_files) {
                    DataArray files = g.array("files");
                    if (files.size() == 0) files.fail("needs at least one file");
                    for (std::size_t i = 0; i < files.size(); ++i) {
                        const std::string f = files.string(i);
                        if (!f.empty()) group.variants.push_back(ctx.depend<SoundClip>(f));
                    }
                } else {
                    const std::string f = g.string("file");
                    const int count = g.integer_or("variants", 1, 1, 64);
                    if (!f.empty()) add_variants(group, f, count, ctx);
                }
                group.volume = g.number_or("volume", 1.0f, 0.0f, 2.0f);
                group.pitch_jitter = g.number_or("pitch_jitter", 0.04f, 0.0f, 0.5f);
                group.bus = intern(g.text_or("bus", default_bus));
                group.max_voices = static_cast<std::uint8_t>(g.integer_or("max_voices", 3, 1, 64));
                group.min_interval_ms = static_cast<std::uint16_t>(g.integer_or("min_interval_ms", 30, 0, 60000));
                bank.groups.push_back(std::move(group));
            }
            return bank;
        });
    }
};

} // namespace

void register_audio_loaders(AssetServer &assets) {
    assets.register_loader<SoundClip>(std::make_unique<SoundClipLoader>());
    assets.register_loader<MusicTrack>(std::make_unique<MusicTrackLoader>());
    assets.register_loader<SoundBank>(std::make_unique<SoundBankLoader>());
}

} // namespace lam::detail
