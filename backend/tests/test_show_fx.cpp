// Unit tests for show_fx.h (flicker rate + flicker on time). Run before
// every push by .githooks/pre-push (via tests/run_tests.sh) - a failing test
// aborts the push.

#include "show_fx.h"

#include <cstdio>
#include <functional>
#include <limits>
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

// Fraction of time lit over `seconds`, sampled finely.
static double litFraction(float hz, float onMs, double seconds = 10.0, double start = 0.0) {
    const int n = 200000; int lit = 0;
    for (int i = 0; i < n; ++i) lit += flicker_is_lit(start + seconds * i / n, hz, onMs);
    return (double)lit / n;
}

TEST(flicker_off_is_always_lit) {
    for (double t : {0.0, 0.3, 12.7, 1e6}) {
        CHECK(flicker_is_lit(t, 0.0f, 0.0f));
        CHECK(flicker_is_lit(t, 0.0f, 50.0f));
        CHECK(flicker_is_lit(t, -3.0f, 50.0f));
    }
}
TEST(auto_on_time_is_half_the_period_like_before) {
    CHECK_NEAR(litFraction(10.0f, 0.0f), 0.5, 0.002);
    CHECK_NEAR(litFraction(2.5f, 0.0f), 0.5, 0.002);
    // same phase as the old code: lit at the start of every period
    CHECK(flicker_is_lit(0.0, 10.0f, 0.0f));
    CHECK(flicker_is_lit(0.049, 10.0f, 0.0f));
    CHECK(!flicker_is_lit(0.051, 10.0f, 0.0f));
    CHECK(flicker_is_lit(0.101, 10.0f, 0.0f));
}
TEST(on_time_sets_the_lit_duration) {
    CHECK_NEAR(litFraction(10.0f, 20.0f), 0.2, 0.002);  // 20 ms of 100 ms
    CHECK_NEAR(litFraction(4.0f, 50.0f), 0.2, 0.002);   // 50 ms of 250 ms
    CHECK_NEAR(litFraction(1.0f, 900.0f), 0.9, 0.002);
    CHECK(flicker_is_lit(0.019, 10.0f, 20.0f));
    CHECK(!flicker_is_lit(0.021, 10.0f, 20.0f));
    CHECK(flicker_is_lit(1.005, 10.0f, 20.0f));
}
TEST(rate_and_on_time_are_independent) {
    // Same on time at different rates: each flash is equally long.
    for (float hz : {1.0f, 5.0f, 12.5f}) CHECK_NEAR(litFraction(hz, 30.0f) / hz, 0.030, 0.0005);
    // Same rate with different on times: the number of flashes stays the same.
    auto flashes = [](float hz, float onMs) {
        int count = 0; bool prev = false;
        for (int i = 0; i < 100000; ++i) { bool l = flicker_is_lit(2.0 * i / 100000 + 1e-7, hz, onMs); count += l && !prev; prev = l; }
        return count;
    };
    CHECK(flashes(5.0f, 10.0f) == flashes(5.0f, 150.0f));
    CHECK(flashes(5.0f, 10.0f) == 10);
}
TEST(on_time_longer_than_period_means_always_lit) {
    CHECK_NEAR(litFraction(10.0f, 100.0f), 1.0, 1e-9);
    CHECK_NEAR(litFraction(10.0f, 500.0f), 1.0, 1e-9);
}
TEST(on_time_is_capped) {
    // 0.5 Hz = 2 s period: anything above the 1000 ms cap behaves like 1000 ms.
    CHECK_NEAR(litFraction(0.5f, 5000.0f, 20.0), 0.5, 0.002);
}
TEST(garbage_never_blanks_permanently) {
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    CHECK(flicker_is_lit(1.0, nan, 20.0f));
    CHECK(flicker_is_lit(1.0, inf, 20.0f));
    CHECK(flicker_is_lit(std::numeric_limits<double>::quiet_NaN(), 10.0f, 20.0f));
    CHECK_NEAR(litFraction(10.0f, nan), 0.5, 0.002);   // NaN on time -> auto
    CHECK_NEAR(litFraction(10.0f, -40.0f), 0.5, 0.002); // negative -> auto
}
TEST(negative_time_is_handled) {
    CHECK(flicker_is_lit(-0.099, 10.0f, 20.0f)); // phase 0.001 into the period
    CHECK(!flicker_is_lit(-0.05, 10.0f, 20.0f));
}

// ── Flicker phase per laser ──────────────────────────────────────────────────
TEST(phase_zero_means_all_lasers_together) {
    for (std::size_t i = 0; i < 6; ++i) CHECK(flicker_phase_offset(i, 5.0f, 0.0f) == 0.0);
}
TEST(phase_delays_each_laser_by_a_fraction_of_the_period) {
    // 4 Hz = 250 ms period, 90° = a quarter period per laser.
    CHECK(flicker_phase_offset(0, 4.0f, 90.0f) == 0.0);
    CHECK_NEAR(flicker_phase_offset(1, 4.0f, 90.0f), -0.0625, 1e-12);
    CHECK_NEAR(flicker_phase_offset(3, 4.0f, 90.0f), -0.1875, 1e-12);
    // 360° = one full period: in step again
    CHECK_NEAR(flicker_phase_offset(1, 4.0f, 360.0f), -0.25, 1e-12);
}
TEST(phase_chase_flashes_lasers_one_after_another) {
    // 2 Hz, 50 ms on, 90°: laser 2 lights exactly while laser 1 is 125..175 ms in.
    const float hz = 2.0f, on = 50.0f, ph = 90.0f;
    auto lit = [&](std::size_t laser, double t) { return flicker_is_lit(t + flicker_phase_offset(laser, hz, ph), hz, on); };
    CHECK(lit(0, 0.010) && !lit(1, 0.010));
    CHECK(!lit(0, 0.130) && lit(1, 0.130));
    CHECK(lit(2, 0.260) && !lit(1, 0.260) && !lit(0, 0.260));
    // each laser still flashes at the same rate and for the same time
    for (std::size_t laser = 0; laser < 4; ++laser) {
        int litN = 0; const int n = 100000;
        for (int i = 0; i < n; ++i) litN += lit(laser, 4.0 * i / n);
        CHECK_NEAR(litN / (double)n, 0.1, 0.002); // 50 ms of 500 ms
    }
}
TEST(phase_offset_ignores_garbage) {
    CHECK(flicker_phase_offset(3, 0.0f, 90.0f) == 0.0);
    CHECK(flicker_phase_offset(3, -1.0f, 90.0f) == 0.0);
    CHECK(flicker_phase_offset(3, 5.0f, std::numeric_limits<float>::quiet_NaN()) == 0.0);
}

// ── Per-point flicker timeline ───────────────────────────────────────────────
struct Pt { float x, y, r, g, b; };
static std::vector<Pt> shape(std::size_t n) {
    std::vector<Pt> v;
    for (std::size_t i = 0; i < n; ++i) v.push_back({(float)i, -(float)i, 1, 0.5f, 0.25f});
    return v;
}

TEST(padding_repeats_whole_shape_until_long_enough) {
    auto f = shape(300); // 10 ms at 30 kpps
    pad_frame_to_duration(f, 30000.0, kFlickerMinFrameSeconds, kFlickerMaxFramePoints);
    CHECK(f.size() == 900); // 3 x 10 ms = 30 ms
    bool same = true;
    for (std::size_t i = 0; i < f.size(); ++i) same = same && f[i].x == (float)(i % 300) && f[i].r == 1;
    CHECK(same);
}
TEST(padding_leaves_long_frames_alone) {
    auto f = shape(1200); // 40 ms at 30 kpps
    pad_frame_to_duration(f, 30000.0, kFlickerMinFrameSeconds, kFlickerMaxFramePoints);
    CHECK(f.size() == 1200);
}
TEST(padding_respects_point_cap) {
    auto f = shape(1100); // 11 ms at 100 kpps -> would need 3 repeats (3300 > 3000)
    pad_frame_to_duration(f, 100000.0, kFlickerMinFrameSeconds, kFlickerMaxFramePoints);
    CHECK(f.size() == 2200);
    CHECK(f.size() <= kFlickerMaxFramePoints);
    auto g = shape(2500);
    pad_frame_to_duration(g, 100000.0, kFlickerMinFrameSeconds, kFlickerMaxFramePoints);
    CHECK(g.size() == 2500);
}
TEST(padding_ignores_garbage) {
    auto f = shape(10);
    pad_frame_to_duration(f, 0.0, 0.03, 3000);   CHECK(f.size() == 10);
    pad_frame_to_duration(f, -5.0, 0.03, 3000);  CHECK(f.size() == 10);
    std::vector<Pt> empty; pad_frame_to_duration(empty, 30000.0, 0.03, 3000); CHECK(empty.empty());
}
TEST(timeline_blanks_exactly_the_off_points) {
    // 10 Hz, 20 ms on, 10 kpps: points 0..199 lit, 200..999 dark, 1000.. lit again.
    auto f = shape(1500);
    apply_flicker_timeline(f, 0.0, 10000.0, 10.0f, 20.0f);
    CHECK(f[0].r == 1 && f[199].r == 1);
    CHECK(f[200].r == 0 && f[200].g == 0 && f[200].b == 0 && f[999].r == 0);
    CHECK(f[1001].r == 1 && f[1198].r == 1 && f[1202].r == 0); // (exact edges are float-rounding territory)
    CHECK(f[1000].x == 1000.0f); // geometry untouched
}
TEST(timeline_off_when_flicker_off) {
    auto f = shape(100);
    apply_flicker_timeline(f, 0.0, 10000.0, 0.0f, 20.0f);
    bool lit = true; for (auto& p : f) lit = lit && p.r == 1;
    CHECK(lit);
}
TEST(back_to_back_frames_form_one_continuous_timeline) {
    // Same result whether the points are played as one long frame or as
    // several frames chained by FlickerTimeline.
    const double rate = 30000.0;
    auto whole = shape(9000);
    apply_flicker_timeline(whole, 5.0, rate, 7.0f, 35.0f);
    FlickerTimeline tl;
    std::vector<Pt> chained;
    for (int k = 0; k < 10; ++k) {
        auto f = shape(900);
        for (auto& p : f) p.x += 900.0f * k;
        const double t0 = tl.begin(5.0 + k * 0.03); // show clock roughly in step
        apply_flicker_timeline(f, t0, rate, 7.0f, 35.0f);
        tl.advance(f.size(), rate);
        chained.insert(chained.end(), f.begin(), f.end());
    }
    bool same = chained.size() == whole.size();
    for (std::size_t i = 0; same && i < whole.size(); ++i) same = whole[i].r == chained[i].r;
    CHECK(same);
}
TEST(timeline_flash_count_and_length_match_settings) {
    // 4 s at 30 kpps in 30 ms frames, 5 Hz, 40 ms on time.
    const double rate = 30000.0;
    FlickerTimeline tl;
    int flashes = 0, litPts = 0; bool prev = false;
    for (int k = 0; k < 4000 / 30; ++k) {
        auto f = shape(900);
        apply_flicker_timeline(f, tl.begin(k * 0.03), rate, 5.0f, 40.0f);
        tl.advance(f.size(), rate);
        for (auto& p : f) { const bool l = p.r > 0; flashes += l && !prev; litPts += l; prev = l; }
    }
    CHECK(flashes == 20);                        // 5 per second
    CHECK_NEAR(litPts / rate / flashes, 0.040, 0.0005); // each flash 40 ms
}
TEST(timeline_resyncs_when_drifting) {
    FlickerTimeline tl;
    CHECK(tl.begin(10.0) == 10.0);
    tl.advance(3000, 30000.0); // +0.1 s
    CHECK_NEAR(tl.begin(10.12), 10.1, 1e-9); // within tolerance: continue
    CHECK(tl.begin(11.0) == 11.0);            // far off: re-anchor
    tl.reset();
    CHECK(tl.begin(20.0) == 20.0);            // restart after flicker was off
    tl.next = std::numeric_limits<double>::quiet_NaN();
    CHECK(tl.begin(21.0) == 21.0);
}

int main() {
    std::printf("show_fx tests\n");
    for (const auto& t : registry()) {
        g_current = t.name;
        const int before = g_failures;
        t.fn();
        std::printf("  %s %s\n", g_failures == before ? "ok  " : "FAIL", t.name);
    }
    std::printf("%zu tests, %d checks, %d failed\n", registry().size(), g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
