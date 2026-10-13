// Smoke test: the public lam/audio headers compile and AudioPlugin runs a few headless frames.
#include <lam/assets/asset_server.h>
#include <lam/audio/audio.h>
#include <lam/audio/audio_plugin.h>
#include <lam/audio/components.h>
#include <lam/audio/device.h>
#include <lam/test/support.h>

TEST_CASE("audio smoke: header-only helpers are usable") {
    CHECK_FALSE(static_cast<bool>(lam::VoiceHandle{}));
}

TEST_CASE("audio smoke: AudioPlugin builds headless and runs frames") {
    lam::test::HeadlessApp h;
    h.add<lam::AssetsPlugin>();
    h.add<lam::AudioPlugin>();
    CHECK(h.app().has_plugin("lam.assets"));
    CHECK(h.app().has_plugin("lam.audio"));
    h.frames(3);
}
