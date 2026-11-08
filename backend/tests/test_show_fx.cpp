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

// ── Several looks in one frame ───────────────────────────────────────────────
static std::vector<Pt> square(float cx, float cy, float h, int perEdge, float bri = 1) {
    std::vector<Pt> v; const float c[4][2] = {{cx - h, cy - h}, {cx + h, cy - h}, {cx + h, cy + h}, {cx - h, cy + h}};
    for (int e = 0; e < 4; ++e) for (int i = 0; i < perEdge; ++i) {
        const float t = (float)i / perEdge; const auto& a = c[e]; const auto& b = c[(e + 1) % 4];
        v.push_back({a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, bri, bri, bri});
    }
    return v;
}
static bool litP(const Pt& p) { return p.r > 0 || p.g > 0 || p.b > 0; }

TEST(single_figure_is_unchanged) {
    auto a = square(0, 0, 0.3f, 10);
    auto out = compose_figures<Pt>({a});
    CHECK(out.size() == a.size());
    auto out2 = compose_figures<Pt>({a, {}}); // empty layers are ignored
    CHECK(out2.size() == a.size());
    CHECK(compose_figures<Pt>({}).empty());
}
TEST(two_figures_contain_both_shapes) {
    auto a = square(-0.5f, 0, 0.2f, 10), b = square(0.5f, 0, 0.2f, 10);
    auto out = compose_figures<Pt>({a, b});
    int litA = 0, litB = 0;
    for (auto& p : out) if (litP(p)) (p.x < 0 ? litA : litB)++;
    CHECK(litA == 41 && litB == 41); // 40 points + the closing point each
}
TEST(no_lit_jump_between_figures) {
    // Every lit segment (both ends lit) stays within one figure: no streak across the room.
    auto a = square(-0.5f, 0, 0.2f, 10), b = square(0.5f, 0.3f, 0.2f, 10), c = square(0, -0.6f, 0.1f, 6);
    auto out = compose_figures<Pt>({a, b, c});
    bool ok = true;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const auto& p = out[i]; const auto& q = out[(i + 1) % out.size()];
        if (litP(p) && litP(q)) ok = ok && std::hypot(q.x - p.x, q.y - p.y) < 0.25f; // edge steps are 0.04
    }
    CHECK(ok);
    // and the frame ends blanked, back at the first figure's start
    CHECK(!litP(out.back()));
    CHECK(out.back().x == a.front().x && out.back().y == a.front().y);
}
TEST(figures_are_closed) {
    auto a = square(-0.5f, 0, 0.2f, 10), b = square(0.5f, 0, 0.2f, 10);
    auto out = compose_figures<Pt>({a, b});
    // The closing segment of figure a (last edge point -> first point) is drawn lit.
    bool closedA = false;
    for (std::size_t i = 0; i + 1 < out.size(); ++i)
        if (litP(out[i]) && litP(out[i + 1]) && out[i].x == a.back().x && out[i].y == a.back().y &&
            out[i + 1].x == a.front().x && out[i + 1].y == a.front().y) closedA = true;
    CHECK(closedA);
}
TEST(blank_travel_is_gentle_and_settles) {
    auto a = square(-0.9f, -0.9f, 0.05f, 4), b = square(0.9f, 0.9f, 0.05f, 4);
    auto out = compose_figures<Pt>({a, b});
    bool gentle = true;
    for (std::size_t i = 1; i < out.size(); ++i)
        if (!litP(out[i]) && !litP(out[i - 1])) gentle = gentle && std::hypot(out[i].x - out[i - 1].x, out[i].y - out[i - 1].y) < 0.1f;
    CHECK(gentle);
    // settle: the point before b lights up is a blank point exactly at b's start, repeated
    std::size_t firstB = 0;
    for (std::size_t i = 0; i < out.size(); ++i) if (litP(out[i]) && out[i].x > 0) { firstB = i; break; }
    CHECK(firstB >= (std::size_t)kFigureSettle);
    for (int k = 1; k <= kFigureSettle; ++k) {
        CHECK(!litP(out[firstB - k]));
        CHECK(out[firstB - k].x == b.front().x && out[firstB - k].y == b.front().y);
    }
}
TEST(dotted_figures_are_not_closed_with_a_lit_segment) {
    // A figure that starts/ends blank (dotted mode) gets no closing point.
    auto a = square(-0.5f, 0, 0.2f, 10); a.front().r = a.front().g = a.front().b = 0;
    auto b = square(0.5f, 0, 0.2f, 10);
    auto out = compose_figures<Pt>({a, b});
    int litA = 0; for (auto& p : out) if (litP(p) && p.x < 0) ++litA;
    CHECK(litA == 39);
}

// ── Dot placement ────────────────────────────────────────────────────────────
TEST(whole_dot_count_is_evenly_spaced_and_fully_lit) {
    auto d = place_dots(5.0, 300);
    CHECK(d.size() == 5);
    for (int i = 0; i < 5; ++i) { CHECK_NEAR(d[i].pos, i * 60.0, 1e-9); CHECK(d[i].level == 1.0f); }
}
TEST(fractional_count_fades_in_the_newest_dot) {
    auto d = place_dots(5.25, 300);
    CHECK(d.size() == 6);
    for (int i = 0; i < 5; ++i) CHECK(d[i].level == 1.0f);
    CHECK_NEAR(d[5].level, 0.25, 1e-6);
}
TEST(dots_never_jump_while_the_count_glides) {
    // Sweep the count finely: every dot that exists in two consecutive steps
    // moves only a tiny bit, and the fading dot's level changes smoothly.
    const int n = 300;
    auto prev = place_dots(3.0, n);
    double maxMove = 0, maxLevelStep = 0;
    for (double c = 3.0; c <= 40.0; c += 0.001) {
        auto cur = place_dots(c, n);
        for (std::size_t i = 0; i < std::min(prev.size(), cur.size()); ++i) {
            maxMove = std::max(maxMove, std::fabs(cur[i].pos - prev[i].pos));
            maxLevelStep = std::max(maxLevelStep, (double)std::fabs(cur[i].level - prev[i].level));
        }
        prev = cur;
    }
    CHECK(maxMove < 0.5);       // well under one base point per 0.001 step
    CHECK(maxLevelStep < 0.01); // fade, no pop
}
TEST(new_dot_appears_dark_and_reaches_full_brightness) {
    auto justAfter = place_dots(7.0005, 300), justBefore = place_dots(7.9995, 300);
    CHECK(justAfter.size() == 8 && justAfter[7].level < 0.01f);
    CHECK(justBefore.size() == 8 && justBefore[7].level > 0.99f);
}
TEST(dot_count_limits) {
    CHECK(place_dots(0.2, 300).size() == 1);     // at least one dot
    CHECK(place_dots(300.0, 300).size() == 300); // every base point
    CHECK(place_dots(299.5, 300).size() == 300);
    for (auto& p : place_dots(299.5, 300)) CHECK(p.pos >= 0 && p.pos < 300);
    CHECK(place_dots(5.0, 0).empty());
    CHECK(place_dots(std::numeric_limits<double>::quiet_NaN(), 300).empty());
}

// ── Dot timing ───────────────────────────────────────────────────────────────
TEST(dot_timing_converts_microseconds_at_the_scan_rate) {
    const DotTiming t = dot_timing(30000.0, 400.0f, 300.0f); // 30 kpps: 1 point = 33.3 us
    CHECK(t.dwell == 12); CHECK(t.trail == 9); CHECK(t.settleMin == 9);
    CHECK_NEAR(t.settlePerUnit, 60.0, 1e-3); // 2 ms per unit
    CHECK(t.settleMax == 9 + 120);           // + 4 ms
}
TEST(dot_timing_is_the_same_time_at_any_rate) {
    const DotTiming slow = dot_timing(15000.0, 400.0f, 300.0f), fast = dot_timing(60000.0, 400.0f, 300.0f);
    CHECK_NEAR(slow.dwell / 15000.0, fast.dwell / 60000.0, 1e-4);
    CHECK_NEAR(slow.trail / 15000.0, fast.trail / 60000.0, 1e-4);
    CHECK_NEAR(slow.settlePerUnit / 15000.0, fast.settlePerUnit / 60000.0, 1e-9);
}
TEST(dot_timing_much_longer_than_before) {
    // The old fixed point counts (dwell 4, trail 2, settle >= 4) were 133/67/133 us at 30 kpps.
    const DotTiming t = dot_timing(30000.0, kDotDwellUsDefault, kDotBlankUsDefault);
    CHECK(t.dwell >= 3 * 4); CHECK(t.trail >= 4 * 2); CHECK(t.settleMin >= 2 * 4);
}
TEST(dot_timing_never_zero_and_handles_garbage) {
    const DotTiming t = dot_timing(1000.0, 0.0f, 0.0f);
    CHECK(t.dwell >= 1 && t.trail >= 1 && t.settleMin >= 2 && t.settleMax >= t.settleMin);
    const DotTiming g = dot_timing(std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN(), -5.0f);
    CHECK(g.dwell == 12 && g.settleMin >= 2 && g.trail >= 1);
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
