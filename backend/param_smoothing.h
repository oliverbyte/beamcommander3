#pragma once
// Soft transitions for show parameters. Every REST/MIDI/Magic Touch/cue
// change only sets a *target* in LaserState; laser_thread() glides the
// values actually rendered towards it, so turning a knob, dragging a fader
// or recalling a cue never makes the shape/movement jump. Header-only and
// templated on the state type so backend/tests/test_param_smoothing.cpp
// can test it without the daemon (run before every push, .githooks/pre-push).
//
// Deliberately NOT smoothed: color and master intensity (flashes, blackout
// and on-beat color hits must stay instant), shape/point count (structural),
// flicker and scan rate. pos_x/pos_y and the wave knobs have their own
// adaptive glide in laser_daemon.cpp (smooth_toward).

#include <algorithm>
#include <cmath>
#include <string>

// Exponential glide: after `tau` seconds 63% of the way, after 3*tau 95% -
// independent of how big the jump is, and frame-rate independent. Snaps
// onto the target once within `snapEps`, so values like rainbow_amount or
// dot_amount really reach their exact end value (0 / 1), which the
// renderer checks for.
inline float glide_toward(float cur, float target, float dt, float tau, float snapEps = 1e-4f) {
    if (!std::isfinite(cur) || !std::isfinite(target)) return target;
    dt = std::clamp(dt, 0.0f, 0.25f); // a hitch must not turn into a jump
    const float a = 1.0f - std::exp(-dt / std::max(1e-4f, tau));
    float next = cur + (target - cur) * a;
    if (std::fabs(target - next) <= snapEps) next = target;
    return next;
}

inline constexpr float kShapeGlideTau = 0.15f; // size/scale/density: ~0.45 s to 95 %
inline constexpr float kSpeedGlideTau = 0.25f; // speeds: ~0.75 s, gentle acceleration
inline constexpr float kDotGlideTau   = 0.05f; // dot amount: ~0.15 s - feels instant, only smooths knob steps
inline constexpr float kMoveModeBlendSec = 0.45f; // crossfade between movement patterns

// State needs: radius, shape_scale, dot_amount, move_size, move_speed,
// rotation_speed, rainbow_amount, rainbow_speed, move_phase_degrees,
// rainbow_phase_degrees, flicker_phase_degrees (float), move_mode, move_mode_from (std::string)
// and move_blend (float, 0 = fully the old pattern, 1 = fully the new one).
template <class State>
struct ShowParamSmoother {
    bool  initialized = false;
    float radius = 0, shape_scale = 0, dot_amount = 1, move_size = 0;
    float rainbow_amount = 0, move_phase_degrees = 0, rainbow_phase_degrees = 0, flicker_phase_degrees = 0;
    float move_speed = 0, rotation_speed = 0, rainbow_speed = 0;
    std::string move_mode, move_mode_from;
    float move_blend = 1.0f;

    void update(const State& t, float dt) {
        if (!initialized) { snap_all(t); return; }
        const float d = std::clamp(dt, 0.0f, 0.25f);
        radius                = glide_toward(radius,                t.radius,                d, kShapeGlideTau);
        shape_scale           = glide_toward(shape_scale,           t.shape_scale,           d, kShapeGlideTau);
        // Snap from 1 % away: dot_amount == 1 means "no dots", and anything
        // below it is still drawn dotted - a long exponential tail made
        // "turn the dots fully up" take seconds to actually go solid.
        dot_amount            = glide_toward(dot_amount,            t.dot_amount,            d, kDotGlideTau, 0.01f);
        move_size             = glide_toward(move_size,             t.move_size,             d, kShapeGlideTau);
        rainbow_amount        = glide_toward(rainbow_amount,        t.rainbow_amount,        d, kShapeGlideTau);
        move_phase_degrees    = glide_toward(move_phase_degrees,    t.move_phase_degrees,    d, kShapeGlideTau, 1e-3f);
        rainbow_phase_degrees = glide_toward(rainbow_phase_degrees, t.rainbow_phase_degrees, d, kShapeGlideTau, 1e-3f);
        flicker_phase_degrees = glide_toward(flicker_phase_degrees, t.flicker_phase_degrees, d, kShapeGlideTau, 1e-3f);
        move_speed            = glide_toward(move_speed,            t.move_speed,            d, kSpeedGlideTau);
        rotation_speed        = glide_toward(rotation_speed,        t.rotation_speed,        d, kSpeedGlideTau);
        rainbow_speed         = glide_toward(rainbow_speed,         t.rainbow_speed,         d, kSpeedGlideTau);

        if (t.move_mode != move_mode) {
            // A new switch during a running crossfade starts from the
            // pattern that was the target so far.
            move_mode_from = move_mode;
            move_mode = t.move_mode;
            move_blend = 0.0f;
        }
        move_blend = std::min(1.0f, move_blend + d / kMoveModeBlendSec);
    }

    // Cue recall / rotation reset: the new rotation speed applies at once,
    // so the restored start angle isn't turned further by the previous
    // look's still-gliding speed.
    void snap_rotation(const State& t) { rotation_speed = t.rotation_speed; }

    // Motion hold: speeds take effect immediately.
    void snap_speeds(const State& t) {
        move_speed = t.move_speed; rotation_speed = t.rotation_speed; rainbow_speed = t.rainbow_speed;
    }

    void snap_all(const State& t) {
        radius = t.radius; shape_scale = t.shape_scale; dot_amount = t.dot_amount; move_size = t.move_size;
        rainbow_amount = t.rainbow_amount; move_phase_degrees = t.move_phase_degrees;
        rainbow_phase_degrees = t.rainbow_phase_degrees;
        flicker_phase_degrees = t.flicker_phase_degrees;
        snap_speeds(t);
        move_mode = move_mode_from = t.move_mode;
        move_blend = 1.0f;
        initialized = true;
    }

    // Writes the rendered (smoothed) values into a snapshot used for
    // make_frame(); everything else in the snapshot stays the live target.
    void apply(State& s) const {
        s.radius = radius; s.shape_scale = shape_scale; s.dot_amount = dot_amount; s.move_size = move_size;
        s.rainbow_amount = rainbow_amount; s.move_phase_degrees = move_phase_degrees;
        s.rainbow_phase_degrees = rainbow_phase_degrees;
        s.flicker_phase_degrees = flicker_phase_degrees;
        s.move_speed = move_speed; s.rotation_speed = rotation_speed; s.rainbow_speed = rainbow_speed;
        s.move_mode = move_mode; s.move_mode_from = move_mode_from;
        // smoothstep, so the crossfade eases in and out
        s.move_blend = move_blend * move_blend * (3.0f - 2.0f * move_blend);
    }
};
