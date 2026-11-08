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

// ── Several looks in one frame (cue layers) ──────────────────────────────────
// Joins figures (the main look plus every active layer, each already
// rendered) into one looping frame: each figure is closed (its lit last ->
// first segment is drawn explicitly, since it no longer loops on its own),
// and between figures - including the wrap from the last one back to the
// first - the beam is switched off, travels blanked (interpolated, gentle on
// the galvos) and settles before the next figure lights up.
inline constexpr float kFigureBlankPerUnit = 24.0f;
inline constexpr int   kFigureMaxTravel = 48, kFigureSettle = 4;

template <class Point>
bool figure_point_lit(const Point& p) { return p.r > 0.0f || p.g > 0.0f || p.b > 0.0f; }

// `from`/`to` by value on purpose: callers pass out.back(), which the
// push_backs below may reallocate.
template <class Point>
void append_blank_travel(std::vector<Point>& out, const Point from, const Point to) {
    Point q = from; q.r = 0; q.g = 0; q.b = 0;
    out.push_back(q); // switch off before moving
    const float dx = to.x - from.x, dy = to.y - from.y;
    const int steps = std::clamp((int)std::round(std::hypot(dx, dy) * kFigureBlankPerUnit), 0, kFigureMaxTravel);
    for (int s = 1; s <= steps; ++s) {
        const float t = (float)s / (float)steps;
        q.x = from.x + dx * t; q.y = from.y + dy * t;
        out.push_back(q);
    }
    q.x = to.x; q.y = to.y;
    for (int s = 0; s < kFigureSettle; ++s) out.push_back(q);
}

template <class Point>
std::vector<Point> compose_figures(const std::vector<std::vector<Point>>& figures) {
    std::vector<const std::vector<Point>*> parts;
    for (const auto& f : figures) if (!f.empty()) parts.push_back(&f);
    if (parts.empty()) return {};
    if (parts.size() == 1) return *parts[0]; // a single look keeps looping on its own
    std::vector<Point> out;
    for (const auto* f : parts) {
        if (!out.empty()) append_blank_travel(out, out.back(), f->front());
        out.insert(out.end(), f->begin(), f->end());
        if (f->size() > 1 && figure_point_lit(f->front()) && figure_point_lit(f->back()))
            out.push_back(f->front()); // close the figure
    }
    append_blank_travel(out, out.back(), out.front()); // loop back to the first figure, blanked
    return out;
}

// ── Dot amount with smooth fades ─────────────────────────────────────────────
// Where to put the dots for a continuous dot count (dot_amount * n base
// points): dots sit evenly spaced at n / count base-point steps, the first
// one always at the shape's start. With a fractional count, the extra
// (newest) dot is drawn at `level` = the fraction - so while the dot amount
// glides, the dots slide apart/together continuously and the newest one fades
// in (or out) instead of every dot jumping to a new position whenever the
// whole-number count changes. `pos` is a fractional base-point index in
// [0, n); between base points the renderer interpolates.
struct DotPlacement { double pos; float level; };
inline std::vector<DotPlacement> place_dots(double count, int n) {
    std::vector<DotPlacement> dots;
    if (n <= 0 || !std::isfinite(count)) return dots;
    count = std::clamp(count, 1.0, (double)n);
    const int whole = (int)std::floor(count + 1e-9);
    const double frac = count - whole;
    const bool fading = frac > 1e-4 && whole < n;
    const int k = whole + (fading ? 1 : 0);
    const double spacing = (double)n / count;
    dots.reserve(k);
    for (int d = 0; d < k; ++d) {
        const float level = (fading && d == whole) ? (float)frac : 1.0f;
        dots.push_back({std::min((double)n - 1e-6, d * spacing), level});
    }
    return dots;
}

// ── Dot timing (scanner tuning) ──────────────────────────────────────────────
// Dots only look like dots if the beam is dark while the galvos travel and
// settle, lit only once they stand still, and dark again (allowing for the
// laser's own switch-off delay) before they move on - otherwise every dot is
// smeared into a short streak along the travel direction. Times are in
// microseconds and converted to points at the current scan rate, so the
// look doesn't change with the rate. dwellUs / blankUs are the user's
// global "Dot dwell" / "Dot blanking" settings; the travel time per unit of
// jump distance (-1..1 output space, full range = 2 units) is fixed.
inline constexpr float kDotTravelUsPerUnit = 2000.0f; // galvo travel + settle per unit of distance
inline constexpr float kDotTravelUsMax     = 4000.0f; // longest wait for a far jump
inline constexpr float kDotDwellUsDefault  = 400.0f;
inline constexpr float kDotBlankUsDefault  = 300.0f;

struct DotTiming {
    int   settleMin;     // blank points before a dot, even for a tiny jump
    int   settleMax;     // ... and at most (far jumps)
    float settlePerUnit; // extra blank points per unit of jump distance
    int   dwell;         // lit points per dot
    int   trail;         // blank points after a dot, before moving on
};
inline DotTiming dot_timing(double rateHz, float dwellUs, float blankUs) {
    if (!(rateHz > 0.0) || !std::isfinite(rateHz)) rateHz = 30000.0;
    if (!std::isfinite(dwellUs)) dwellUs = kDotDwellUsDefault;
    if (!std::isfinite(blankUs)) blankUs = kDotBlankUsDefault;
    auto pts = [&](double us) { return (int)std::lround(std::max(0.0, us) * 1e-6 * rateHz); };
    DotTiming t;
    t.dwell = std::max(1, pts(dwellUs));
    t.trail = std::max(1, pts(blankUs));
    t.settleMin = std::max(2, pts(blankUs));
    t.settlePerUnit = (float)(kDotTravelUsPerUnit * 1e-6 * rateHz);
    t.settleMax = std::max(t.settleMin, t.settleMin + pts(kDotTravelUsMax));
    return t;
}
