#pragma once
// Small, pure show effects used by make_frame() in laser_daemon.cpp. Kept in
// a header so backend/tests/test_show_fx.cpp can test them (run before
// every push by .githooks/pre-push).

#include <algorithm>
#include <cmath>
#include <cstddef>

inline constexpr float kFlickerOnMsMax = 1000.0f;

// Flicker/strobe: whether the output is lit at time `t` (seconds).
//  - rateHz <= 0: flicker off, always lit.
//  - onMs  == 0: "auto" - lit for the first half of every period (the
//    original 50 % behaviour, so cues saved before the on time existed look
//    exactly the same).
//  - onMs  >  0: lit for that many milliseconds at the start of every
//    period ("Anzeigedauer"); an on time >= the period means always lit.
// Evaluated per point by its playback time (apply_flicker_timeline below for
// the laser, showFx.js for the preview), so it is exact to the point.
inline bool flicker_is_lit(double t, float rateHz, float onMs) {
    if (!(rateHz > 0.0f) || !std::isfinite(rateHz) || !std::isfinite(t)) return true;
    const double period = 1.0 / rateHz;
    const double on = onMs > 0.0f ? std::min<double>(onMs, kFlickerOnMsMax) / 1000.0 : period * 0.5;
    if (on >= period) return true;
    double phase = std::fmod(t, period);
    if (phase < 0.0) phase += period;
    return phase < on;
}

#include <vector>

// Flicker phase per laser: laser `laserIndex` (Lasers-list order, laser 1 = 0)
// flashes `laserIndex * phaseDegrees / 360` of a period *after* laser 1 - so
// 0° = all lasers flash together, 90° with four lasers = a chase across the
// rig. Returned as a time offset in seconds to add to that laser's flicker
// time (always <= 0, i.e. a delay; whole periods wrap naturally).
inline double flicker_phase_offset(std::size_t laserIndex, float rateHz, float phaseDegrees) {
    if (!(rateHz > 0.0f) || !std::isfinite(rateHz) || !std::isfinite(phaseDegrees)) return 0.0;
    return -(double)laserIndex * ((double)phaseDegrees / 360.0) / (double)rateHz;
}

// ── Exact flicker on the laser ───────────────────────────────────────────────
// Flicker used to blank whole frames, decided once per frame: with the DAC
// looping each ~10 ms frame until a new one arrives every >= 20 ms, and the
// preview sampling every 33 ms, both quantised the rhythm differently and
// drifted apart. Instead, flicker is applied per *point* by the time that
// point is played: hardware frames are padded so they don't loop and carry a
// continuous point timeline (FlickerTimeline), and the preview applies the
// same flicker_is_lit() per simulated point (LaserScene.vue / showFx.js).

inline constexpr double kFlickerMinFrameSeconds = 0.030; // frames shorter than this would loop on the DAC
inline constexpr std::size_t kFlickerMaxFramePoints = 3000; // leaves room for mask splits/outlines (see output_pipeline.h)

// Repeats a closed (looping) frame until it lasts at least `minSeconds` at
// `rateHz` points/s, never beyond `maxPoints` and only whole repeats, so the
// drawn shape is unchanged - it is simply traced several times per frame.
template <class Point>
void pad_frame_to_duration(std::vector<Point>& pts, double rateHz, double minSeconds, std::size_t maxPoints) {
    const std::size_t n = pts.size();
    if (n == 0 || !(rateHz > 0.0) || !(minSeconds > 0.0)) return;
    const double wanted = std::ceil(minSeconds * rateHz);
    std::size_t repeats = (std::size_t)std::ceil(wanted / (double)n);
    repeats = std::min(repeats, std::max<std::size_t>(1, maxPoints / n));
    if (repeats <= 1) return;
    pts.reserve(n * repeats);
    for (std::size_t r = 1; r < repeats; ++r)
        for (std::size_t i = 0; i < n; ++i) pts.push_back(pts[i]);
}

// Blanks every point that falls into a flicker "off" phase, point i being
// played at startT + i / rateHz.
template <class Point>
void apply_flicker_timeline(std::vector<Point>& pts, double startT, double rateHz, float flickerHz, float onMs) {
    if (!(flickerHz > 0.0f) || !(rateHz > 0.0)) return;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        if (!flicker_is_lit(startT + (double)i / rateHz, flickerHz, onMs)) { pts[i].r = 0; pts[i].g = 0; pts[i].b = 0; }
    }
}

// Continuous playback clock for one DAC: each frame starts where the previous
// one ended (frames are played back to back), anchored to the show clock and
// re-anchored if the two drift apart (DAC stalled, frame repeated, flicker
// just switched on) so all lasers stay in step with each other and the preview.
struct FlickerTimeline {
    bool   active = false;
    double next = 0.0;
    double begin(double now, double resyncAfter = 0.25) {
        if (!active || !std::isfinite(next) || std::fabs(next - now) > resyncAfter) next = now;
        active = true;
        return next;
    }
    void advance(std::size_t points, double rateHz) { if (rateHz > 0.0) next += (double)points / rateHz; }
    void reset() { active = false; }
};
