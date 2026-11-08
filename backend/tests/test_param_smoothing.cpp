// Unit tests for param_smoothing.h (soft transitions of show parameters).
// Run before every push by .githooks/pre-push (via tests/run_tests.sh) - a
// failing test aborts the push.

#include "param_smoothing.h"

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

static int g_checks = 0, g_failures = 0;
static const char* g_current = "";
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; \
    std::printf("  FAIL [%s] %s:%d: %s\n", g_current, __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_NEAR(a, b, eps) do { ++g_checks; const double va_ = (a), vb_ = (b); \
    if (!(std::fabs(va_ - vb_) <= (eps))) { ++g_failures; \
    std::printf("  FAIL [%s] %s:%d: %s = %g, expected %g\n", g_current, __FILE__, __LINE__, #a, va_, vb_); } } while (0)
struct TestCase { const char* name; std::function<void()> fn; };
static std::vector<TestCase>& registry() { static std::vector<TestCase> r; return r; }
struct Register { Register(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); } };
#define TEST(name) static void name(); static Register reg_##name(#name, name); static void name()

// Same field names/types as LaserState in laser_daemon.cpp.
struct State {
    float radius = 0.7f, shape_scale = 0, dot_amount = 1, move_size = 0.5f;
    float move_speed = 0.3f, rotation_speed = 0, rainbow_amount = 0, rainbow_speed = 0;
    float move_phase_degrees = 0, rainbow_phase_degrees = 0, flicker_phase_degrees = 0;
    std::string move_mode = "none", move_mode_from = "none";
    float move_blend = 1.0f;
};

static void run(ShowParamSmoother<State>& s, const State& t, float seconds, float dt = 1.0f / 300.0f) {
    for (float e = 0; e < seconds; e += dt) s.update(t, dt);
}
static State rendered(const ShowParamSmoother<State>& s) { State out; s.apply(out); return out; }

// ── glide_toward ─────────────────────────────────────────────────────────────
TEST(glide_moves_part_of_the_way_without_jumping) {
    const float v = glide_toward(0.0f, 1.0f, 1.0f / 60.0f, kShapeGlideTau);
    CHECK(v > 0.0f && v < 0.2f);
}
TEST(glide_reaches_63_percent_after_tau) {
    float v = 0;
    for (int i = 0; i < 1000; ++i) v = glide_toward(v, 1.0f, kShapeGlideTau / 1000.0f, kShapeGlideTau);
    CHECK_NEAR(v, 1.0 - std::exp(-1.0), 1e-3);
}
TEST(glide_is_frame_rate_independent) {
    float a = 0, b = 0;
    for (int i = 0; i < 30; ++i) a = glide_toward(a, 2.0f, 1.0f / 30.0f, kSpeedGlideTau);
    for (int i = 0; i < 300; ++i) b = glide_toward(b, 2.0f, 1.0f / 300.0f, kSpeedGlideTau);
    CHECK_NEAR(a, b, 1e-3);
}
TEST(glide_never_overshoots_in_either_direction) {
    float up = 0, down = 5;
    for (int i = 0; i < 2000; ++i) {
        up = glide_toward(up, 1.0f, 0.01f, kShapeGlideTau);
        down = glide_toward(down, -3.0f, 0.01f, kShapeGlideTau);
        CHECK(up <= 1.0f && down >= -3.0f);
    }
}
TEST(glide_snaps_exactly_onto_target) {
    float v = 1.0f;
    for (int i = 0; i < 2000; ++i) v = glide_toward(v, 0.0f, 0.005f, kShapeGlideTau);
    CHECK(v == 0.0f); // exact - the renderer checks rainbow_amount > 0 / dot_amount < 1
}
TEST(glide_settle_time_does_not_depend_on_jump_size) {
    auto settle = [](float target) {
        float v = 0; int steps = 0;
        while (std::fabs(target - v) > 0.05f * std::fabs(target) && steps < 100000) { v = glide_toward(v, target, 0.001f, kShapeGlideTau, 0); ++steps; }
        return steps;
    };
    CHECK(std::abs(settle(0.1f) - settle(5.0f)) <= 1);
}
TEST(glide_survives_hitches_and_garbage) {
    CHECK(glide_toward(0.0f, 1.0f, 5.0f, kShapeGlideTau) < 0.9f); // a 5 s stall is treated as 0.25 s
    CHECK(glide_toward(0.0f, 1.0f, -1.0f, kShapeGlideTau) == 0.0f);
    CHECK(glide_toward(NAN, 0.5f, 0.01f, kShapeGlideTau) == 0.5f);
    CHECK(std::isfinite(glide_toward(0.3f, 0.5f, 0.01f, 0.0f)));
}

// ── ShowParamSmoother ────────────────────────────────────────────────────────
TEST(first_update_takes_state_without_gliding) {
    ShowParamSmoother<State> s; State t; t.shape_scale = 0.8f; t.move_mode = "circle";
    s.update(t, 0.01f);
    const State r = rendered(s);
    CHECK(r.shape_scale == 0.8f); CHECK(r.move_mode == "circle"); CHECK(r.move_blend == 1.0f);
}
TEST(scale_and_move_size_glide_instead_of_jumping) {
    ShowParamSmoother<State> s; State t; s.update(t, 0.01f);
    t.shape_scale = 1.0f; t.move_size = 2.0f; t.radius = 0.2f;
    s.update(t, 1.0f / 60.0f);
    State r = rendered(s);
    CHECK(r.shape_scale > 0.0f && r.shape_scale < 0.2f);
    CHECK(r.move_size > 0.5f && r.move_size < 0.8f);
    CHECK(r.radius < 0.7f && r.radius > 0.6f);
    run(s, t, 1.5f);
    r = rendered(s);
    CHECK(r.shape_scale == 1.0f); CHECK(r.move_size == 2.0f); CHECK(r.radius == 0.2f);
}
TEST(speeds_accelerate_softly) {
    ShowParamSmoother<State> s; State t; s.update(t, 0.01f);
    t.move_speed = 2.0f; t.rotation_speed = -1.0f; t.rainbow_speed = 0.5f;
    run(s, t, 0.1f);
    CHECK(s.move_speed > 0.3f && s.move_speed < 1.0f);
    CHECK(s.rotation_speed < 0.0f && s.rotation_speed > -0.5f);
    run(s, t, 3.0f);
    CHECK(s.move_speed == 2.0f); CHECK(s.rotation_speed == -1.0f); CHECK(s.rainbow_speed == 0.5f);
}
TEST(speed_snap_is_immediate) {
    ShowParamSmoother<State> s; State t; t.move_speed = 1.5f; t.rotation_speed = 2; t.rainbow_speed = 1;
    s.update(t, 0.01f);
    t.move_speed = 0; t.rotation_speed = 0; t.rainbow_speed = 0; // motion hold
    s.snap_speeds(t);
    CHECK(s.move_speed == 0 && s.rotation_speed == 0 && s.rainbow_speed == 0);
}
TEST(rotation_snap_only_touches_rotation) {
    ShowParamSmoother<State> s; State t; t.rotation_speed = 1.0f; t.move_speed = 0.3f; s.update(t, 0.01f);
    t.rotation_speed = 0.0f; t.move_speed = 2.0f; // recalled cue: no rotation, faster movement
    s.update(t, 0.01f);
    s.snap_rotation(t);
    CHECK(s.rotation_speed == 0.0f);                     // no leftover spin
    CHECK(s.move_speed > 0.3f && s.move_speed < 2.0f);   // movement still glides
}
TEST(snap_speeds_leaves_shape_gliding) {
    ShowParamSmoother<State> s; State t; s.update(t, 0.01f);
    t.shape_scale = 1.0f; s.snap_speeds(t);
    CHECK(s.shape_scale == 0.0f);
}
TEST(dot_amount_glides_slower_than_shape) {
    ShowParamSmoother<State> s; State t; t.dot_amount = 0.1f; t.shape_scale = 0; s.update(t, 0.01f);
    t.dot_amount = 0.9f; t.shape_scale = 0.8f; run(s, t, 0.3f);
    const float dotProgress = (s.dot_amount - 0.1f) / 0.8f, scaleProgress = s.shape_scale / 0.8f;
    CHECK(dotProgress < scaleProgress);
    CHECK(dotProgress > 0.4f && dotProgress < 0.7f); // ~1 - e^(-0.3/0.35)
}
TEST(rainbow_amount_reaches_exact_zero) {
    ShowParamSmoother<State> s; State t; t.rainbow_amount = 0.6f; s.update(t, 0.01f);
    t.rainbow_amount = 0; run(s, t, 2.0f);
    CHECK(rendered(s).rainbow_amount == 0.0f);
}
TEST(dot_amount_reaches_exact_one) {
    ShowParamSmoother<State> s; State t; t.dot_amount = 0.2f; s.update(t, 0.01f);
    t.dot_amount = 1.0f; run(s, t, 4.0f);
    CHECK(rendered(s).dot_amount == 1.0f);
}
TEST(move_mode_switch_crossfades) {
    ShowParamSmoother<State> s; State t; s.update(t, 0.01f);
    t.move_mode = "circle";
    s.update(t, 1.0f / 60.0f);
    State r = rendered(s);
    CHECK(r.move_mode == "circle"); CHECK(r.move_mode_from == "none");
    CHECK(r.move_blend > 0.0f && r.move_blend < 0.1f);
    run(s, t, kMoveModeBlendSec * 0.5f);
    r = rendered(s);
    CHECK(r.move_blend > 0.3f && r.move_blend < 0.7f);
    run(s, t, kMoveModeBlendSec);
    CHECK(rendered(s).move_blend == 1.0f);
}
TEST(move_blend_is_monotonic_and_eased) {
    ShowParamSmoother<State> s; State t; s.update(t, 0.01f);
    t.move_mode = "pan";
    float prev = -1, firstStep = -1;
    for (int i = 0; i < 100; ++i) {
        s.update(t, kMoveModeBlendSec / 100.0f);
        const float b = rendered(s).move_blend;
        CHECK(b >= prev);
        if (firstStep < 0) firstStep = b;
        prev = b;
    }
    CHECK(firstStep < 0.01f); // smoothstep: starts gently
}
TEST(second_mode_switch_mid_crossfade_starts_from_current_target) {
    ShowParamSmoother<State> s; State t; s.update(t, 0.01f);
    t.move_mode = "circle"; run(s, t, 0.1f);
    t.move_mode = "eight"; s.update(t, 0.01f);
    const State r = rendered(s);
    CHECK(r.move_mode_from == "circle"); CHECK(r.move_mode == "eight");
}
TEST(per_laser_phase_offsets_glide) {
    ShowParamSmoother<State> s; State t; s.update(t, 0.01f);
    t.move_phase_degrees = 90; t.rainbow_phase_degrees = 180; t.flicker_phase_degrees = 120;
    s.update(t, 1.0f / 60.0f);
    CHECK(s.move_phase_degrees > 0 && s.move_phase_degrees < 20);
    CHECK(s.flicker_phase_degrees > 0 && s.flicker_phase_degrees < 30);
    run(s, t, 2.0f);
    CHECK(s.move_phase_degrees == 90); CHECK(s.rainbow_phase_degrees == 180);
    CHECK(rendered(s).flicker_phase_degrees == 120);
}
TEST(apply_leaves_unsmoothed_fields_alone) {
    struct Big : State { float r = 0.25f; float intensity = 0.4f; };
    ShowParamSmoother<State> s; State t; t.shape_scale = 0.5f; s.update(t, 0.01f);
    Big b; b.shape_scale = -1;
    s.apply(b);
    CHECK(b.shape_scale == 0.5f); CHECK(b.r == 0.25f); CHECK(b.intensity == 0.4f);
}

int main() {
    std::printf("param_smoothing tests\n");
    for (const auto& t : registry()) {
        g_current = t.name;
        const int before = g_failures;
        t.fn();
        std::printf("  %s %s\n", g_failures == before ? "ok  " : "FAIL", t.name);
    }
    std::printf("%zu tests, %d checks, %d failed\n", registry().size(), g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
