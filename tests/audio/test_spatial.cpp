// spatial_mix and sound lengths read from file headers (WAV, Ogg Vorbis/Opus, QOA, FLAC).
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include <lam/audio/audio.h>
#include <lam/test/support.h>

#include "audio_test_util.h"

using lam::SpatialMix;
using lam::spatial_mix;
using lam::Vec2;

TEST_CASE("spatial_mix: full gain near the listener, quadratic falloff to zero at range") {
    const SpatialMix at = spatial_mix({0, 0}, {0, 0}, 200);
    CHECK(at.gain == doctest::Approx(1.0f));
    CHECK(at.pan == doctest::Approx(0.0f));
    CHECK(spatial_mix({0, 0}, {50, 0}, 200).gain == doctest::Approx(1.0f)); // within range / 4
    // d = 125: 75 px into the 150 px falloff band -> (1 - 0.5)^2.
    CHECK(spatial_mix({0, 0}, {0, 125}, 200).gain == doctest::Approx(0.25f));
    CHECK(spatial_mix({10, 10}, {10, 135}, 200).gain == doctest::Approx(0.25f)); // translation invariant
    CHECK(spatial_mix({0, 0}, {200, 0}, 200).gain == 0.0f);                     // at range: silent
    CHECK(spatial_mix({0, 0}, {0, 500}, 200).gain == 0.0f);
}

TEST_CASE("spatial_mix: gain never increases with distance") {
    float previous = 2.0f;
    for (int d = 0; d <= 260; d += 5) {
        const float g = spatial_mix({0, 0}, {static_cast<float>(d), 0}, 240).gain;
        CHECK(g <= previous);
        CHECK(g >= 0.0f);
        CHECK(g <= 1.0f);
        previous = g;
    }
}

TEST_CASE("spatial_mix: pan follows x, is symmetric and clamps to +-0.8") {
    CHECK(spatial_mix({0, 0}, {100, 0}, 200).pan == doctest::Approx(0.5f));
    CHECK(spatial_mix({0, 0}, {-100, 0}, 200).pan == doctest::Approx(-0.5f));
    CHECK(spatial_mix({0, 0}, {0, 100}, 200).pan == doctest::Approx(0.0f)); // straight below
    CHECK(spatial_mix({0, 0}, {900, 0}, 1000).pan == doctest::Approx(0.8f));
    CHECK(spatial_mix({0, 0}, {-900, 0}, 1000).pan == doctest::Approx(-0.8f));
}

TEST_CASE("spatial_mix: bad ranges and non-finite input are silent, never NaN") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (const SpatialMix m : {spatial_mix({0, 0}, {1, 0}, 0), spatial_mix({0, 0}, {1, 0}, -5),
                               spatial_mix({0, 0}, {1, 0}, nan), spatial_mix({0, 0}, {1, 0}, inf),
                               spatial_mix({0, 0}, {nan, 0}, 100), spatial_mix({inf, 0}, {0, 0}, 100)}) {
        CHECK(m.gain == 0.0f);
        CHECK(m.pan == 0.0f);
    }
}

namespace {
std::vector<std::byte> bytes_of(std::initializer_list<int> v) {
    std::vector<std::byte> out;
    for (int b : v) out.push_back(static_cast<std::byte>(b));
    return out;
}
// Byte i of v (0 beyond the 8 bytes of a uint64_t: callers write wider zero fields, and a shift
// by 64 or more is undefined).
std::byte byte_of(std::uint64_t v, int i) {
    return i < 8 ? static_cast<std::byte>((v >> (8 * i)) & 0xFF) : std::byte{0};
}
void put_le(std::vector<std::byte> &out, std::uint64_t v, int n) {
    for (int i = 0; i < n; ++i) out.push_back(byte_of(v, i));
}
void put_be(std::vector<std::byte> &out, std::uint64_t v, int n) {
    for (int i = n - 1; i >= 0; --i) out.push_back(byte_of(v, i));
}
void put_tag(std::vector<std::byte> &out, const char *t) {
    for (; *t != 0; ++t) out.push_back(static_cast<std::byte>(*t));
}
// One Ogg page holding `packet` as its only segment(s) (packet < 255 bytes).
void ogg_page(std::vector<std::byte> &out, std::uint64_t granule, const std::vector<std::byte> &packet) {
    put_tag(out, "OggS");
    out.push_back(std::byte{0});    // version
    out.push_back(std::byte{0});    // header type
    put_le(out, granule, 8);
    put_le(out, 1, 4);              // serial
    put_le(out, 0, 4);              // sequence
    put_le(out, 0, 4);              // crc (not checked)
    out.push_back(std::byte{1});    // one segment
    out.push_back(static_cast<std::byte>(packet.size()));
    out.insert(out.end(), packet.begin(), packet.end());
}
} // namespace

TEST_CASE("encoded_sound_seconds: WAV lengths from the fmt and data chunks") {
    using audio_test::make_wav;
    CHECK(lam::encoded_sound_seconds(make_wav(0.5f, 8000), ".wav") == doctest::Approx(0.5f));
    CHECK(lam::encoded_sound_seconds(make_wav(1.25f, 22050, 2, 16), ".wav") == doctest::Approx(1.25f));
    // A LIST chunk of odd size (padded) before fmt is skipped.
    std::vector<std::byte> w = make_wav(0.5f, 8000);
    std::vector<std::byte> list;
    put_tag(list, "LIST");
    put_le(list, 3, 4);
    list.insert(list.end(), {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{0}});
    w.insert(w.begin() + 12, list.begin(), list.end());
    CHECK(lam::encoded_sound_seconds(w, ".wav") == doctest::Approx(0.5f));
    // A data size larger than the file (streaming writers) is clamped to what is there.
    std::vector<std::byte> t = make_wav(1.0f, 8000);
    t.resize(t.size() - 4000);
    CHECK(lam::encoded_sound_seconds(t, ".wav") == doctest::Approx(0.5f));
    CHECK(lam::encoded_sound_seconds(bytes_of({'R', 'I', 'F', 'F'}), ".wav") == 0.0f);
    CHECK(lam::encoded_sound_seconds({}, ".wav") == 0.0f);
}

TEST_CASE("encoded_sound_seconds: Ogg Vorbis and Opus from the last granule position") {
    std::vector<std::byte> id;
    id.push_back(std::byte{1});
    put_tag(id, "vorbis");
    put_le(id, 0, 4);      // version
    id.push_back(std::byte{2});
    put_le(id, 44100, 4);  // sample rate
    put_le(id, 0, 12);
    std::vector<std::byte> ogg;
    ogg_page(ogg, 0, id);
    ogg_page(ogg, 44100, bytes_of({1, 2, 3}));
    ogg_page(ogg, 88200, bytes_of({4, 5, 6}));
    CHECK(lam::encoded_sound_seconds(ogg, ".ogg") == doctest::Approx(2.0f));

    std::vector<std::byte> head;
    put_tag(head, "OpusHead");
    head.push_back(std::byte{1});  // version
    head.push_back(std::byte{2});  // channels
    put_le(head, 312, 2);          // pre-skip
    put_le(head, 48000, 4);        // input rate (informational)
    put_le(head, 0, 3);
    std::vector<std::byte> opus;
    ogg_page(opus, 0, head);
    ogg_page(opus, 48000 + 312, bytes_of({9}));
    CHECK(lam::encoded_sound_seconds(opus, ".ogg") == doctest::Approx(1.0f));
    CHECK(lam::encoded_sound_seconds(bytes_of({'O', 'g', 'g', 'S'}), ".ogg") == 0.0f);
}

TEST_CASE("encoded_sound_seconds: QOA and FLAC headers; unknown formats are 0") {
    std::vector<std::byte> qoa;
    put_tag(qoa, "qoaf");
    put_be(qoa, 33075, 4);         // samples per channel
    qoa.push_back(std::byte{1});   // channels
    put_be(qoa, 22050, 3);         // sample rate
    put_be(qoa, 5120, 2);
    put_be(qoa, 0, 2);
    CHECK(lam::encoded_sound_seconds(qoa, ".qoa") == doctest::Approx(1.5f));

    std::vector<std::byte> flac;
    put_tag(flac, "fLaC");
    flac.push_back(std::byte{0x80});   // last block, STREAMINFO
    put_be(flac, 34, 3);
    put_be(flac, 4096, 2);
    put_be(flac, 4096, 2);
    put_be(flac, 0, 3);
    put_be(flac, 0, 3);
    // 20 bits rate | 3 bits channels-1 | 5 bits bps-1 | 36 bits total samples
    const std::uint64_t packed = (std::uint64_t{44100} << 44) | (std::uint64_t{1} << 41) |
                                 (std::uint64_t{15} << 36) | std::uint64_t{44100 * 3};
    put_be(flac, packed, 8);
    put_be(flac, 0, 16);               // md5
    CHECK(lam::encoded_sound_seconds(flac, ".flac") == doctest::Approx(3.0f));

    CHECK(lam::encoded_sound_seconds(audio_test::make_wav(1.0f), ".mp3") == 0.0f);
    CHECK(lam::encoded_sound_seconds(audio_test::make_wav(1.0f), ".xm") == 0.0f);
    CHECK(lam::encoded_sound_seconds(bytes_of({'q', 'o', 'a', 'f'}), ".qoa") == 0.0f);
    CHECK(lam::encoded_sound_seconds(bytes_of({'f', 'L', 'a', 'C', 0}), ".flac") == 0.0f);
}
