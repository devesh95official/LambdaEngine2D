#pragma once
// Banked fixed-step clock. advance() never returns more than max_ticks_per_frame; excess
// banked time is dropped so a hitch is followed by at most that many ticks.
//   int n = clock.advance(frame_seconds);  for (int i = 0; i < n; ++i) tick();  draw(clock.alpha());
namespace lam {
class FixedClock {
public:
    explicit FixedClock(int ticks_per_second = 60, double max_frame_seconds = 0.25,
                        int max_ticks_per_frame = 4);
    // Banks frame_seconds (capped at max_frame_seconds) and returns the ticks to run now.
    // Non-finite or negative -> 0.
    int advance(double frame_seconds);
    // [0, 1): the share of a tick banked but not yet run.
    float alpha() const;
    // Forgets banked time.
    void reset();
    int ticks_per_second() const;
    float tick_seconds() const;
private:
    int ticks_per_second_;
    int max_ticks_per_frame_;
    double tick_seconds_;
    double max_frame_seconds_;
    double banked_ = 0.0;
};
}
