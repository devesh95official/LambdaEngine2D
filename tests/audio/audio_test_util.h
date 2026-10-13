#pragma once
// lam_audio test helpers: generated WAVs, a headless App and readers for the device call log.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <lam/assets/asset_server.h>
#include <lam/audio/audio.h>
#include <lam/audio/audio_plugin.h>
#include <lam/audio/device.h>
#include <lam/core/fs.h>
#include <lam/test/support.h>

namespace audio_test {

using Kind = lam::NullAudioDevice::Call::Kind;

// A PCM WAV (RIFF/WAVE) holding a sine tone: `seconds` long at `rate` Hz, `channels` channels,
// `bits` (8 or 16) per sample.
inline std::vector<std::byte> make_wav(float seconds, int rate = 8000, int channels = 1, int bits = 8,
                                       float freq = 440.0f) {
    const int frames = static_cast<int>(std::lround(static_cast<double>(seconds) * rate));
    const int block = channels * (bits / 8);
    const auto data_size = static_cast<std::uint32_t>(frames * block);
    std::vector<std::byte> out;
    auto put = [&](std::uint32_t v, int n) {
        for (int i = 0; i < n; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
    };
    auto tag = [&](const char *t) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>(t[i]));
    };
    tag("RIFF");
    put(36 + data_size, 4);
    tag("WAVE");
    tag("fmt ");
    put(16, 4);
    put(1, 2); // PCM
    put(static_cast<std::uint32_t>(channels), 2);
    put(static_cast<std::uint32_t>(rate), 4);
    put(static_cast<std::uint32_t>(rate * block), 4);
    put(static_cast<std::uint32_t>(block), 2);
    put(static_cast<std::uint32_t>(bits), 2);
    tag("data");
    put(data_size, 4);
    for (int f = 0; f < frames; ++f) {
        const double s = std::sin(2.0 * 3.14159265358979 * freq * f / rate) * 0.5;
        for (int c = 0; c < channels; ++c) {
            if (bits == 8) {
                put(static_cast<std::uint32_t>(128 + std::lround(s * 127)), 1);
            } else {
                put(static_cast<std::uint32_t>(static_cast<std::uint16_t>(std::lround(s * 32767))), 2);
            }
        }
    }
    return out;
}

inline void write_bytes(const std::string &path, std::span<const std::byte> bytes) {
    (void)lam::make_directories(lam::parent_path(path));
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

inline void write_text(const std::string &path, std::string_view text) {
    (void)lam::make_directories(lam::parent_path(path));
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// A headless App with AssetsPlugin (root = a fresh TempDir, synchronous) and AudioPlugin.
struct Fixture {
    lam::test::TempDir dir;
    lam::test::HeadlessApp h;

    explicit Fixture(lam::AppConfig config = {}) : h(std::move(config)) {
        lam::AssetsConfig c;
        c.root = dir.path();
        h.add<lam::AssetsPlugin>(c);
        h.add<lam::AudioPlugin>();
    }
    lam::App &app() { return h.app(); }
    lam::Audio &audio() { return h.app().res<lam::Audio>(); }
    lam::AssetServer &assets() { return h.app().res<lam::AssetServer>(); }
    lam::NullAudioDevice &device() { return dynamic_cast<lam::NullAudioDevice &>(audio().device()); }
    // Writes a WAV under the asset root and returns its root-relative name.
    std::string wav(std::string_view name, float seconds = 0.25f, int rate = 8000) {
        write_bytes(dir.file(name), make_wav(seconds, rate));
        return std::string(name);
    }
    void file(std::string_view name, std::string_view text) { write_text(dir.file(name), text); }
    lam::AssetId<lam::SoundClip> clip(std::string_view name, float seconds = 0.25f) {
        wav(name, seconds);
        auto r = assets().load_now<lam::SoundClip>(name);
        REQUIRE(r.ok());
        return *r;
    }
    lam::AssetId<lam::MusicTrack> track(std::string_view name, float seconds = 2.0f) {
        wav(name, seconds);
        auto r = assets().load_now<lam::MusicTrack>(name);
        REQUIRE(r.ok());
        return *r;
    }
    lam::AssetId<lam::SoundBank> bank(std::string_view name, std::string_view json) {
        file(name, json);
        auto r = assets().load_now<lam::SoundBank>(name);
        REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
        audio().add_bank(*r);
        return *r;
    }
};

// Calls of one kind, in order.
inline std::vector<lam::NullAudioDevice::Call> calls_of(const lam::NullAudioDevice &d, Kind k) {
    std::vector<lam::NullAudioDevice::Call> out;
    for (const auto &c : d.calls())
        if (c.kind == k) out.push_back(c);
    return out;
}

// The last call of one kind (for `slot` when slot >= 0); a default Call with id 0 if none.
inline lam::NullAudioDevice::Call last_of(const lam::NullAudioDevice &d, Kind k, int slot = -1) {
    lam::NullAudioDevice::Call found{};
    found.kind = k;
    for (const auto &c : d.calls())
        if (c.kind == k && (slot < 0 || c.slot == slot)) found = c;
    return found;
}

// Last volume sent to a stream (StreamVolume), -1 if none.
inline float stream_volume(const lam::NullAudioDevice &d, std::uint32_t stream) {
    float v = -1;
    for (const auto &c : d.calls())
        if (c.kind == Kind::StreamVolume && c.id == stream) v = c.volume;
    return v;
}

// Id of the last stream opened (OpenStream).
inline std::uint32_t last_stream(const lam::NullAudioDevice &d) { return last_of(d, Kind::OpenStream).id; }

} // namespace audio_test
