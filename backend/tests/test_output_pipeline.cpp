// Unit tests for the hardware output safety pipeline (output_pipeline.h):
// safety masks, mask outlines, blackout/arm gating, zone transform and the
// mask JSON (de)serialization. Run before every push by .githooks/pre-push
// (via tests/run_tests.sh) - a failing test aborts the push. Self-contained (no test framework, no libera linking: LaserPoint is
// header-only), so it runs anywhere the daemon compiles.
//
// Run manually:  bash backend/tests/run_tests.sh

#include "output_pipeline.h"

#include <cstdio>
#include <functional>
#include <limits>
#include <random>
#include <string>
#include <vector>

// ── Minimal test harness ─────────────────────────────────────────────────────
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

// ── Helpers ─────────────────────────────────────────────────────────────────
using Pts = std::vector<OutPoint>;
static OutPoint P(float x, float y, float r = 1, float g = 1, float b = 1) { return OutPoint{x, y, r, g, b, 1.0f}; }
static LaserMask M(float x0, float y0, float x1, float y1, float reduce = 100, bool outline = false) {
    LaserMask m; m.x0 = x0; m.y0 = y0; m.x1 = x1; m.y1 = y1; m.reduce = reduce; m.outline = outline; return m;
}
static float lum(const OutPoint& p) { return std::max({p.r, p.g, p.b}); }
static bool isBlank(const OutPoint& p) { return p.r == 0.0f && p.g == 0.0f && p.b == 0.0f; }
static bool strictlyInside(const LaserMask& m, float x, float y, float e = 1e-4f) {
    return x > m.x0 + e && x < m.x1 - e && y > m.y0 + e && y < m.y1 - e;
}
static bool onBorder(const LaserMask& m, float x, float y, float e = 1e-4f) {
    const bool inX = x >= m.x0 - e && x <= m.x1 + e, inY = y >= m.y0 - e && y <= m.y1 + e;
    return (inX && (std::fabs(y - m.y0) < e || std::fabs(y - m.y1) < e)) ||
           (inY && (std::fabs(x - m.x0) < e || std::fabs(x - m.x1) < e));
}
// A closed polyline (the DAC loops the frame) with `n` points per edge.
static Pts polygon(const std::vector<std::pair<float, float>>& corners, int perEdge, float bri = 1.0f) {
    Pts out;
    for (std::size_t c = 0; c < corners.size(); ++c) {
        auto [ax, ay] = corners[c];
        auto [bx, by] = corners[(c + 1) % corners.size()];
        for (int i = 0; i < perEdge; ++i) {
            const float t = (float)i / (float)perEdge;
            out.push_back(P(ax + (bx - ax) * t, ay + (by - ay) * t, bri, bri, bri));
        }
    }
    return out;
}

// Safety invariant used by the geometric and randomized tests: model the
// beam as travelling in straight lines between consecutive points (with
// wrap-around), each segment lit at most as bright as its brighter endpoint.
// Then no light may land inside a mask brighter than that mask allows:
// for every segment, sample its interior; wherever a sample is strictly
// inside a mask, both endpoints' brightness must be <= maxBri * factor.
static int countMaskViolations(const Pts& pts, const std::vector<LaserMask>& masks, float maxBri) {
    int violations = 0;
    const std::size_t n = pts.size();
    for (std::size_t i = 0; i < n; ++i) {
        const auto& a = pts[i];
        const auto& b = pts[(i + 1) % n];
        const float segBri = std::max(lum(a), lum(b));
        if (segBri <= 0.0f) continue;
        for (int s = 0; s <= 20; ++s) {
            const float t = (float)s / 20.0f;
            const float x = a.x + (b.x - a.x) * t, y = a.y + (b.y - a.y) * t;
            float allowed = 1.0f;
            for (const auto& m : masks)
                if (strictlyInside(m, x, y)) allowed = std::min(allowed, 1.0f - m.reduce / 100.0f);
            if (segBri > maxBri * allowed + 1e-4f) { ++violations; break; }
        }
    }
    return violations;
}

// ════════════════════════════════════════════════════════════════════════════
// apply_masks - per point
// ════════════════════════════════════════════════════════════════════════════
TEST(point_inside_full_mask_is_blanked) {
    Pts f{P(0, 0, 1, 0.5f, 0.25f)};
    apply_masks(f, {M(-0.5f, -0.5f, 0.5f, 0.5f)});
    CHECK(f.size() == 1); CHECK(isBlank(f[0]));
    CHECK(f[0].x == 0.0f && f[0].y == 0.0f);
}
TEST(point_outside_mask_is_untouched) {
    Pts f{P(0.9f, 0.9f, 1, 0.5f, 0.25f)};
    apply_masks(f, {M(-0.5f, -0.5f, 0.5f, 0.5f)});
    CHECK(f.size() == 1);
    CHECK(f[0].r == 1.0f && f[0].g == 0.5f && f[0].b == 0.25f);
}
TEST(partial_reduction_scales_all_color_channels) {
    Pts f{P(0, 0, 1, 0.5f, 0.2f)};
    apply_masks(f, {M(-0.5f, -0.5f, 0.5f, 0.5f, 50)});
    CHECK_NEAR(f[0].r, 0.5, 1e-6); CHECK_NEAR(f[0].g, 0.25, 1e-6); CHECK_NEAR(f[0].b, 0.1, 1e-6);
    CHECK(f[0].i == 1.0f); // legacy intensity channel untouched (colors carry brightness)
}
TEST(reduction_levels_are_exact) {
    for (float reduce : {0.0f, 10.0f, 25.0f, 33.3f, 75.0f, 99.0f, 100.0f}) {
        Pts f{P(0, 0)};
        apply_masks(f, {M(-1, -1, 1, 1, reduce)});
        CHECK_NEAR(f[0].r, 1.0 - reduce / 100.0, 1e-5);
    }
}
TEST(zero_percent_mask_changes_nothing) {
    Pts f = polygon({{-0.8f, -0.8f}, {0.8f, -0.8f}, {0.8f, 0.8f}, {-0.8f, 0.8f}}, 10);
    const Pts orig = f;
    apply_masks(f, {M(-0.3f, -0.9f, 0.3f, 0.9f, 0)});
    CHECK(f.size() == orig.size());
    for (std::size_t i = 0; i < f.size() && i < orig.size(); ++i)
        CHECK(f[i].x == orig[i].x && f[i].y == orig[i].y && f[i].r == orig[i].r);
}
TEST(point_exactly_on_border_counts_as_inside) {
    const LaserMask m = M(-0.5f, -0.5f, 0.5f, 0.5f);
    for (auto [x, y] : std::vector<std::pair<float, float>>{{-0.5f, 0}, {0.5f, 0}, {0, -0.5f}, {0, 0.5f}, {0.5f, 0.5f}}) {
        Pts f{P(x, y)};
        apply_masks(f, {m});
        CHECK(isBlank(f[0]));
    }
}
TEST(overlapping_masks_strongest_reduction_wins_in_any_order) {
    const LaserMask weak = M(-1, -1, 1, 1, 30), strong = M(-0.2f, -0.2f, 0.2f, 0.2f, 80);
    Pts a{P(0, 0)}, b{P(0, 0)}, c{P(0.5f, 0.5f)};
    apply_masks(a, {weak, strong});
    apply_masks(b, {strong, weak});
    apply_masks(c, {weak, strong});
    CHECK_NEAR(a[0].r, 0.2, 1e-6); CHECK_NEAR(b[0].r, 0.2, 1e-6);
    CHECK_NEAR(c[0].r, 0.7, 1e-6); // only the weak one covers (0.5,0.5)
}
TEST(out_of_range_reduce_is_clamped_defensively) {
    Pts a{P(0, 0)}, b{P(0, 0)};
    apply_masks(a, {M(-1, -1, 1, 1, 250)}); // never brighter/negative
    apply_masks(b, {M(-1, -1, 1, 1, -50)}); // never amplifies
    CHECK(isBlank(a[0])); CHECK(b[0].r == 1.0f);
}
TEST(empty_inputs_are_noops) {
    Pts empty;
    apply_masks(empty, {M(-1, -1, 1, 1)});
    CHECK(empty.empty());
    Pts f{P(0, 0)};
    apply_masks(f, {});
    CHECK(f.size() == 1 && f[0].r == 1.0f);
}
TEST(single_point_frame_is_masked_without_splitting) {
    Pts f{P(0.1f, 0.1f)};
    apply_masks(f, {M(0, 0, 0.5f, 0.5f)});
    CHECK(f.size() == 1); CHECK(isBlank(f[0]));
}

// ════════════════════════════════════════════════════════════════════════════
// apply_masks - segment splitting at mask borders
// ════════════════════════════════════════════════════════════════════════════
TEST(horizontal_line_is_cut_exactly_at_both_borders) {
    Pts f{P(-1, 0), P(1, 0)}; // the DAC loops: -1 -> 1 -> back to -1
    apply_masks(f, {M(-0.2f, -0.5f, 0.2f, 0.5f)});
    // -1 lit | -0.2 lit, -0.2 dark | 0.2 dark, 0.2 lit | 1 lit | 0.2 lit, 0.2 dark | -0.2 dark, -0.2 lit
    CHECK(f.size() == 10);
    if (f.size() == 10) {
        CHECK_NEAR(f[1].x, -0.2, 1e-5); CHECK(lum(f[1]) == 1.0f); CHECK(isBlank(f[2]));
        CHECK_NEAR(f[3].x,  0.2, 1e-5); CHECK(isBlank(f[3]));     CHECK(lum(f[4]) == 1.0f);
        CHECK_NEAR(f[6].x,  0.2, 1e-5); CHECK(lum(f[6]) == 1.0f); CHECK(isBlank(f[7]));
        CHECK_NEAR(f[8].x, -0.2, 1e-5); CHECK(isBlank(f[8]));     CHECK(lum(f[9]) == 1.0f);
        CHECK(f[1].x == f[2].x && f[1].y == f[2].y); // split points share the exact border position
    }
    CHECK(countMaskViolations(f, {M(-0.2f, -0.5f, 0.2f, 0.5f)}, 1.0f) == 0);
}
TEST(vertical_line_is_cut_at_horizontal_borders) {
    Pts f{P(0, -1), P(0, 1)};
    const LaserMask m = M(-0.5f, 0.1f, 0.5f, 0.3f);
    apply_masks(f, {m});
    int splits = 0;
    for (const auto& p : f) if (std::fabs(p.y - 0.1f) < 1e-5f || std::fabs(p.y - 0.3f) < 1e-5f) ++splits;
    CHECK(splits == 8); // 2 borders x 2 copies x 2 directions
    CHECK(countMaskViolations(f, {m}, 1.0f) == 0);
}
TEST(sparse_square_crossing_a_mask_does_not_shine_through) {
    // Only the 4 corners: without splitting, the edges would be drawn straight through the mask.
    Pts f{P(-0.8f, -0.8f), P(0.8f, -0.8f), P(0.8f, 0.8f), P(-0.8f, 0.8f)};
    const LaserMask m = M(-0.1f, -1, 0.1f, 1);
    apply_masks(f, {m});
    CHECK(countMaskViolations(f, {m}, 1.0f) == 0);
    // ...and the parts outside the mask are still lit.
    int lit = 0; for (const auto& p : f) if (lum(p) > 0) ++lit;
    CHECK(lit >= 4);
}
TEST(diagonal_segment_through_a_mask) {
    Pts f{P(-0.9f, -0.9f), P(0.9f, 0.9f)};
    const LaserMask m = M(-0.3f, -0.1f, 0.2f, 0.4f);
    apply_masks(f, {m});
    CHECK(countMaskViolations(f, {m}, 1.0f) == 0);
    CHECK(f.size() > 2);
}
TEST(segment_passing_beside_a_mask_is_not_split) {
    // Crosses the mask's x-range but stays above it.
    Pts f{P(-1, 0.8f), P(1, 0.8f)};
    apply_masks(f, {M(-0.2f, -0.5f, 0.2f, 0.5f)});
    CHECK(f.size() == 2);
    CHECK(lum(f[0]) == 1.0f && lum(f[1]) == 1.0f);
}
TEST(segment_fully_inside_a_mask_is_dark_and_not_split) {
    Pts f{P(-0.1f, 0), P(0.1f, 0)};
    apply_masks(f, {M(-0.5f, -0.5f, 0.5f, 0.5f)});
    CHECK(f.size() == 2); CHECK(isBlank(f[0]) && isBlank(f[1]));
}
TEST(blank_moves_are_never_split) {
    Pts f{P(-1, 0, 0, 0, 0), P(1, 0, 0, 0, 0)};
    apply_masks(f, {M(-0.2f, -0.5f, 0.2f, 0.5f)});
    CHECK(f.size() == 2);
}
TEST(partial_mask_split_uses_interpolated_color) {
    // Red -> blue segment crossing a 50% mask at its midpoint x = 0.
    Pts f{P(-1, 0, 1, 0, 0), P(1, 0, 0, 0, 1)};
    apply_masks(f, {M(0, -0.5f, 1, 0.5f, 50)});
    bool found = false;
    for (std::size_t i = 0; i + 1 < f.size(); ++i) {
        if (std::fabs(f[i].x) < 1e-5f && std::fabs(f[i + 1].x) < 1e-5f && f[i].y == 0) {
            found = true;
            CHECK_NEAR(f[i].r, 0.5, 1e-5);      CHECK_NEAR(f[i].b, 0.5, 1e-5);      // before: full
            CHECK_NEAR(f[i + 1].r, 0.25, 1e-5); CHECK_NEAR(f[i + 1].b, 0.25, 1e-5); // after: 50%
            break;
        }
    }
    CHECK(found);
}
TEST(adjacent_masks_with_shared_border) {
    const std::vector<LaserMask> ms{M(-0.6f, -0.5f, 0, 0.5f, 100), M(0, -0.5f, 0.6f, 0.5f, 50)};
    Pts f{P(-1, 0), P(1, 0)};
    apply_masks(f, ms);
    CHECK(countMaskViolations(f, ms, 1.0f) == 0);
}
TEST(nested_masks) {
    const std::vector<LaserMask> ms{M(-0.8f, -0.8f, 0.8f, 0.8f, 40), M(-0.2f, -0.2f, 0.2f, 0.2f, 100)};
    Pts f{P(-1, 0), P(1, 0), P(0, 1), P(0, -1)};
    apply_masks(f, ms);
    CHECK(countMaskViolations(f, ms, 1.0f) == 0);
}
TEST(full_coverage_mask_blanks_entire_frame) {
    Pts f = polygon({{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}, 25);
    apply_masks(f, {M(-1, -1, 1, 1)});
    for (const auto& p : f) CHECK(isBlank(p));
}
TEST(no_crossings_keeps_point_count_and_order) {
    Pts f = polygon({{0.5f, 0.5f}, {0.9f, 0.5f}, {0.9f, 0.9f}}, 10);
    const Pts orig = f;
    apply_masks(f, {M(-0.9f, -0.9f, 0, 0)});
    CHECK(f.size() == orig.size());
    bool same = true;
    for (std::size_t i = 0; i < f.size(); ++i) same = same && f[i].x == orig[i].x && f[i].y == orig[i].y;
    CHECK(same);
}
TEST(split_budget_never_exceeded_and_every_point_still_dimmed) {
    // A dense zig-zag with many thin masks forces far more crossings than the budget allows.
    Pts f;
    for (int i = 0; i < 3000; ++i) f.push_back(P(std::sin(i * 0.37f) * 0.95f, -0.95f + 1.9f * i / 3000.0f));
    std::vector<LaserMask> ms;
    for (int k = 0; k < 64; ++k) ms.push_back(M(-1 + k * 0.031f, -1, -1 + k * 0.031f + 0.015f, 1));
    apply_masks(f, ms);
    CHECK(f.size() <= kMaskFrameMaxPoints);
    for (const auto& p : f) // per-point dimming always applies, even past the split budget
        for (const auto& m : ms)
            if (strictlyInside(m, p.x, p.y)) CHECK(isBlank(p));
}
TEST(randomized_frames_never_light_inside_masks) {
    std::mt19937 rng(1234567);
    std::uniform_real_distribution<float> U(-1.0f, 1.0f), R(0.0f, 100.0f), B(0.0f, 1.0f);
    int total = 0;
    for (int iter = 0; iter < 20000; ++iter) {
        std::vector<LaserMask> ms;
        const int nm = 1 + (int)(rng() % 5);
        for (int k = 0; k < nm; ++k) {
            float a = U(rng), b = U(rng), c = U(rng), d = U(rng);
            ms.push_back(M(std::min(a, b), std::min(c, d), std::max(a, b), std::max(c, d),
                           (rng() % 3 == 0) ? R(rng) : 100.0f));
        }
        Pts f;
        const int np = 2 + (int)(rng() % 12);
        const float bri = 0.2f + 0.8f * B(rng);
        for (int k = 0; k < np; ++k) f.push_back(P(U(rng), U(rng), bri, bri * B(rng), bri * B(rng)));
        apply_masks(f, ms);
        total += countMaskViolations(f, ms, bri);
    }
    CHECK(total == 0);
}

// ════════════════════════════════════════════════════════════════════════════
// append_mask_outlines
// ════════════════════════════════════════════════════════════════════════════
TEST(outline_not_drawn_unless_requested) {
    Pts f{P(0.9f, 0.9f)};
    append_mask_outlines(f, {M(-0.5f, -0.5f, 0.5f, 0.5f)});
    CHECK(f.size() == 1);
}
TEST(outline_lit_points_are_red_and_on_the_border) {
    const LaserMask m = M(-0.4f, -0.3f, 0.2f, 0.5f, 100, true);
    Pts f{P(0.9f, 0.9f, 0, 1, 0)};
    append_mask_outlines(f, {m});
    int lit = 0;
    for (std::size_t i = 1; i < f.size(); ++i) {
        if (isBlank(f[i])) continue;
        ++lit;
        CHECK(onBorder(m, f[i].x, f[i].y));
        CHECK(f[i].r == kOutlineRed && f[i].g == 0.0f && f[i].b == 0.0f);
    }
    CHECK(lit > 20);
}
TEST(outline_covers_all_four_edges_and_corners) {
    const LaserMask m = M(-0.4f, -0.3f, 0.2f, 0.5f, 100, true);
    Pts f;
    append_mask_outlines(f, {m});
    int bottom = 0, top = 0, left = 0, right = 0, corners = 0;
    for (const auto& p : f) {
        if (isBlank(p)) continue;
        const bool L = std::fabs(p.x - m.x0) < 1e-5f, R = std::fabs(p.x - m.x1) < 1e-5f;
        const bool Bo = std::fabs(p.y - m.y0) < 1e-5f, T = std::fabs(p.y - m.y1) < 1e-5f;
        bottom += Bo; top += T; left += L; right += R; corners += (L || R) && (Bo || T);
    }
    CHECK(bottom > 3 && top > 3 && left > 3 && right > 3);
    CHECK(corners >= 4 * 3); // every corner dwells
}
TEST(outline_lit_segments_never_leave_the_border) {
    // No visible streak across the room: every lit segment's midpoint is on the border.
    const LaserMask m = M(-0.6f, -0.6f, -0.1f, -0.2f, 100, true);
    Pts f{P(0.8f, 0.8f), P(0.7f, 0.8f)};
    append_mask_outlines(f, {m});
    for (std::size_t i = 2; i < f.size(); ++i) {
        const auto& a = f[i - 1]; const auto& b = f[i];
        if (isBlank(b)) continue;
        CHECK(onBorder(m, (a.x + b.x) / 2, (a.y + b.y) / 2));
    }
}
TEST(outline_travel_is_blank_and_returns_to_show_start) {
    Pts f{P(0.7f, 0.7f, 1, 1, 1), P(0.8f, 0.7f, 1, 1, 1)};
    append_mask_outlines(f, {M(-0.5f, -0.5f, -0.2f, -0.2f, 100, true)});
    CHECK(isBlank(f[2]));                  // leaving the show: blanked
    CHECK(isBlank(f.back()));              // returning: blanked
    CHECK(f.back().x == 0.7f && f.back().y == 0.7f); // ends where the looped frame restarts
    // Blank travel is interpolated - no single jump longer than ~0.1 while blanked.
    for (std::size_t i = 2; i < f.size(); ++i)
        if (isBlank(f[i])) CHECK(std::hypot(f[i].x - f[i - 1].x, f[i].y - f[i - 1].y) < 0.1f);
}
TEST(outline_on_empty_frame_starts_and_ends_at_origin) {
    Pts f;
    append_mask_outlines(f, {M(0.1f, 0.1f, 0.3f, 0.3f, 100, true)});
    CHECK(!f.empty());
    CHECK(isBlank(f.back()) && f.back().x == 0.0f && f.back().y == 0.0f);
}
TEST(multiple_outlines_each_drawn) {
    const std::vector<LaserMask> ms{M(-0.9f, -0.9f, -0.5f, -0.5f, 100, true), M(0.5f, 0.5f, 0.9f, 0.9f, 100, true),
                                    M(-0.1f, -0.1f, 0.1f, 0.1f, 100, false)};
    Pts f;
    append_mask_outlines(f, ms);
    int onA = 0, onB = 0, onC = 0;
    for (const auto& p : f) {
        if (isBlank(p)) continue;
        onA += onBorder(ms[0], p.x, p.y); onB += onBorder(ms[1], p.x, p.y); onC += onBorder(ms[2], p.x, p.y);
    }
    CHECK(onA > 10 && onB > 10); CHECK(onC == 0);
}
TEST(outline_respects_dac_point_budget) {
    Pts f(3000, P(0, 0));
    std::vector<LaserMask> ms;
    for (int k = 0; k < 8; ++k) ms.push_back(M(-1 + k * 0.2f, -1, -1 + k * 0.2f + 0.15f, 1, 100, true));
    append_mask_outlines(f, ms);
    CHECK(f.size() > 3000);                    // still drawn, with lower edge density
    CHECK(f.size() <= kOutlineFrameMaxPoints);
}
TEST(outline_skipped_when_no_room) {
    Pts f(3900, P(0, 0));
    append_mask_outlines(f, {M(-1, -1, 1, 1, 100, true)});
    CHECK(f.size() == 3900);
    std::vector<LaserMask> many(MAX_MASKS_PER_LASER, M(-0.5f, -0.5f, 0.5f, 0.5f, 100, true));
    Pts g(1000, P(0, 0));
    append_mask_outlines(g, many);
    CHECK(g.size() <= kOutlineFrameMaxPoints);
}
TEST(degenerate_zero_size_mask_outline_is_safe) {
    Pts f{P(0.5f, 0.5f)};
    append_mask_outlines(f, {M(0.2f, 0.2f, 0.2f, 0.2f, 100, true)});
    for (const auto& p : f) CHECK(std::isfinite(p.x) && std::isfinite(p.y));
    CHECK(f.size() <= kOutlineFrameMaxPoints);
}

// ════════════════════════════════════════════════════════════════════════════
// finalize_hardware_frame - complete ordered pipeline
// ════════════════════════════════════════════════════════════════════════════
static HardwareOutputParams live(const std::vector<LaserMask>* masks = nullptr) {
    HardwareOutputParams p; p.blackout = false; p.outputAllowed = true; p.masks = masks; return p;
}
TEST(defaults_are_safe_dark) {
    Pts f{P(0, 0), P(0.5f, 0.5f)};
    HardwareOutputParams p; // default-constructed: blackout on, not allowed
    finalize_hardware_frame(f, p);
    for (const auto& q : f) CHECK(isBlank(q));
}
TEST(blackout_blanks_everything_including_outlines) {
    const std::vector<LaserMask> ms{M(-0.5f, -0.5f, 0.5f, 0.5f, 0, true)};
    Pts f{P(0.8f, 0.8f), P(-0.8f, 0.8f)};
    auto p = live(&ms); p.blackout = true;
    finalize_hardware_frame(f, p);
    for (const auto& q : f) CHECK(isBlank(q));
    CHECK(f.size() == 2); // no outline appended
}
TEST(unarmed_laser_blanks_everything_including_outlines) {
    const std::vector<LaserMask> ms{M(-0.5f, -0.5f, 0.5f, 0.5f, 0, true)};
    Pts f{P(0.8f, 0.8f)};
    auto p = live(&ms); p.outputAllowed = false;
    finalize_hardware_frame(f, p);
    for (const auto& q : f) CHECK(isBlank(q));
}
TEST(gate_level_dims_show_but_not_outline) {
    const std::vector<LaserMask> ms{M(-0.5f, -0.5f, -0.2f, -0.2f, 100, true)};
    Pts f{P(0.8f, 0.8f)};
    auto p = live(&ms); p.gateLevel = 0.0f;
    finalize_hardware_frame(f, p);
    CHECK(isBlank(f[0]));
    int red = 0; for (const auto& q : f) red += q.r == kOutlineRed;
    CHECK(red > 10);
}
TEST(gate_level_partial_fade) {
    Pts f{P(0.8f, 0.8f)};
    auto p = live(); p.gateLevel = 0.25f;
    finalize_hardware_frame(f, p);
    CHECK_NEAR(f[0].r, 0.25, 1e-6);
}
TEST(masks_apply_in_output_space_after_zone) {
    // Content at (0,0) shifted by zone_x=+0.5 lands at (0.5,0) on the laser.
    const std::vector<LaserMask> atVenue{M(0.4f, -0.1f, 0.6f, 0.1f)};
    const std::vector<LaserMask> atContent{M(-0.1f, -0.1f, 0.1f, 0.1f)};
    Pts a{P(0, 0)}, b{P(0, 0)};
    auto pa = live(&atVenue); pa.zoneX = 0.5f;
    auto pb = live(&atContent); pb.zoneX = 0.5f;
    finalize_hardware_frame(a, pa);
    finalize_hardware_frame(b, pb);
    CHECK_NEAR(a[0].x, 0.5, 1e-6); CHECK(isBlank(a[0]));
    CHECK_NEAR(b[0].x, 0.5, 1e-6); CHECK(lum(b[0]) == 1.0f);
}
TEST(masks_apply_after_zone_scale_and_mirror) {
    const std::vector<LaserMask> ms{M(-0.55f, -0.05f, -0.45f, 0.05f)};
    Pts f{P(0.5f, 0)}; // mirrored -> -0.5, scale 1 -> -0.5: inside the mask
    auto p = live(&ms); p.mirrorX = true;
    finalize_hardware_frame(f, p);
    CHECK_NEAR(f[0].x, -0.5, 1e-6); CHECK(isBlank(f[0]));
    Pts g{P(0.5f, 0)}; // scale 0.5 -> 0.25: outside the mask
    auto q = live(&ms); q.zoneScaleX = 0.5f;
    finalize_hardware_frame(g, q);
    CHECK_NEAR(g[0].x, 0.25, 1e-6); CHECK(lum(g[0]) == 1.0f);
}
TEST(mirror_y_flips_vertically_before_zone) {
    Pts f{P(0.2f, 0.4f)};
    auto p = live(); p.mirrorY = true; p.zoneY = 0.1f; p.zoneScaleY = 0.5f;
    finalize_hardware_frame(f, p);
    CHECK_NEAR(f[0].x, 0.2, 1e-6);            // x untouched
    CHECK_NEAR(f[0].y, -0.4 * 0.5 + 0.1, 1e-6); // flipped, then scaled + offset
    CHECK(lum(f[0]) == 1.0f);
}
TEST(mirror_x_and_y_together_rotate_180_degrees) {
    Pts f{P(0.3f, -0.6f), P(-0.5f, 0.2f)};
    auto p = live(); p.mirrorX = true; p.mirrorY = true;
    finalize_hardware_frame(f, p);
    CHECK_NEAR(f[0].x, -0.3, 1e-6); CHECK_NEAR(f[0].y, 0.6, 1e-6);
    CHECK_NEAR(f[1].x, 0.5, 1e-6);  CHECK_NEAR(f[1].y, -0.2, 1e-6);
}
TEST(masks_stay_in_room_coordinates_when_mirrored_y) {
    // Audience mask at the bottom of the real output: content drawn at the
    // top lands at the bottom once the laser is flipped, and must be masked.
    const std::vector<LaserMask> audience{M(-1, -1, 1, -0.5f)};
    Pts top{P(0, 0.8f)}, bottom{P(0, -0.8f)};
    auto p = live(&audience); p.mirrorY = true;
    finalize_hardware_frame(top, p);
    finalize_hardware_frame(bottom, p);
    CHECK_NEAR(top[0].y, -0.8, 1e-6); CHECK(isBlank(top[0]));
    CHECK_NEAR(bottom[0].y, 0.8, 1e-6); CHECK(lum(bottom[0]) == 1.0f);
}
TEST(mirror_y_alone_triggers_transform) {
    // mirrorY with an otherwise identity zone must still be applied.
    Pts f{P(0, 0.5f)};
    auto p = live(); p.mirrorY = true;
    finalize_hardware_frame(f, p);
    CHECK_NEAR(f[0].y, -0.5, 1e-6);
}
TEST(zone_blanks_points_pushed_off_screen) {
    Pts f{P(0.8f, 0), P(-0.8f, 0)};
    auto p = live(); p.zoneX = 0.5f;
    finalize_hardware_frame(f, p);
    CHECK(isBlank(f[0])); CHECK(lum(f[1]) == 1.0f);
}
TEST(outline_is_not_dimmed_by_its_own_mask) {
    const std::vector<LaserMask> ms{M(-0.5f, -0.5f, 0.5f, 0.5f, 100, true)};
    Pts f{P(0.9f, 0.9f)};
    finalize_hardware_frame(f, live(&ms));
    int red = 0; for (const auto& q : f) red += q.r == kOutlineRed && onBorder(ms[0], q.x, q.y);
    CHECK(red > 20);
}
TEST(show_masked_while_outline_visible) {
    const std::vector<LaserMask> ms{M(-0.3f, -1, 0.3f, 1, 100, true)};
    Pts f{P(-0.9f, 0, 0, 1, 0), P(0.9f, 0, 0, 1, 0)}; // green line through the mask
    finalize_hardware_frame(f, live(&ms));
    // Green (show) never inside the mask, red (outline) present.
    for (const auto& q : f) if (q.g > 0) CHECK(!strictlyInside(ms[0], q.x, q.y));
    CHECK(countMaskViolations(f, ms, 1.0f) == 0); // whole frame incl. outline travel
}
TEST(no_masks_pipeline_matches_plain_zone_transform) {
    Pts f{P(0.2f, 0.3f, 0.5f, 0.6f, 0.7f)};
    auto p = live(); p.zoneX = 0.1f; p.zoneY = -0.2f; p.zoneScaleX = 2; p.zoneScaleY = 0.5f;
    finalize_hardware_frame(f, p);
    CHECK(f.size() == 1);
    CHECK_NEAR(f[0].x, 0.5, 1e-6); CHECK_NEAR(f[0].y, -0.05, 1e-6);
    CHECK(f[0].r == 0.5f && f[0].g == 0.6f && f[0].b == 0.7f);
}

// ════════════════════════════════════════════════════════════════════════════
// JSON
// ════════════════════════════════════════════════════════════════════════════
TEST(parse_valid_list) {
    std::vector<LaserMask> ms;
    CHECK(parse_masks(R"({"masks":[{"x0":-0.3,"y0":-0.2,"x1":0.5,"y1":0.4,"reduce":70,"outline":true},{"x0":0,"y0":0,"x1":1,"y1":1,"reduce":100}]})", ms));
    CHECK(ms.size() == 2);
    if (ms.size() == 2) {
        CHECK_NEAR(ms[0].x0, -0.3, 1e-6); CHECK_NEAR(ms[0].y1, 0.4, 1e-6); CHECK_NEAR(ms[0].reduce, 70, 1e-6);
        CHECK(ms[0].outline); CHECK(!ms[1].outline);
    }
}
TEST(parse_normalizes_swapped_corners) {
    std::vector<LaserMask> ms;
    CHECK(parse_masks(R"({"masks":[{"x0":0.5,"y0":0.4,"x1":-0.3,"y1":-0.2}]})", ms));
    CHECK(ms.size() == 1 && ms[0].x0 < ms[0].x1 && ms[0].y0 < ms[0].y1);
    CHECK_NEAR(ms[0].x0, -0.3, 1e-6); CHECK_NEAR(ms[0].y0, -0.2, 1e-6);
}
TEST(parse_clamps_ranges) {
    std::vector<LaserMask> ms;
    CHECK(parse_masks(R"({"masks":[{"x0":-5,"y0":-2,"x1":3,"y1":1.5,"reduce":400},{"reduce":-20}]})", ms));
    CHECK(ms.size() == 2);
    if (ms.size() == 2) {
        CHECK(ms[0].x0 == -1 && ms[0].y0 == -1 && ms[0].x1 == 1 && ms[0].y1 == 1 && ms[0].reduce == 100);
        CHECK(ms[1].reduce == 0);
    }
}
TEST(parse_defaults_missing_fields_to_full_reduction) {
    std::vector<LaserMask> ms;
    CHECK(parse_masks(R"({"masks":[{"x0":0.1,"y0":0.1,"x1":0.2,"y1":0.2}]})", ms));
    CHECK(ms.size() == 1 && ms[0].reduce == 100.0f && !ms[0].outline);
}
TEST(parse_rejects_non_finite_numbers) {
    std::vector<LaserMask> ms{M(0, 0, 1, 1)};
    CHECK(!parse_masks(R"({"masks":[{"x0":nan,"y0":0,"x1":1,"y1":1}]})", ms));
    CHECK(!parse_masks(R"({"masks":[{"x0":0,"y0":0,"x1":inf,"y1":1}]})", ms));
    CHECK(!parse_masks(R"({"masks":[{"x0":0,"y0":0,"x1":1,"y1":1,"reduce":nan}]})", ms));
    CHECK(ms.size() == 1 && ms[0].x1 == 1); // a refused list leaves the existing masks untouched
}
TEST(parse_rejects_missing_or_malformed) {
    std::vector<LaserMask> ms{M(0, 0, 1, 1)};
    CHECK(!parse_masks(R"({"nope":[]})", ms));
    CHECK(!parse_masks(R"({"masks":[{"x0":0)", ms));
    CHECK(!parse_masks(R"({"masks":)", ms));
    CHECK(ms.size() == 1);
}
TEST(parse_empty_list_clears) {
    std::vector<LaserMask> ms{M(0, 0, 1, 1)};
    CHECK(parse_masks(R"({"masks":[]})", ms));
    CHECK(ms.empty());
    CHECK(parse_masks(R"({ "masks" : [ ] })", ms));
}
TEST(parse_caps_mask_count) {
    std::string body = R"({"masks":[)";
    for (int i = 0; i < 100; ++i) body += std::string(i ? "," : "") + R"({"x0":0,"y0":0,"x1":0.1,"y1":0.1})";
    body += "]}";
    std::vector<LaserMask> ms;
    CHECK(parse_masks(body, ms));
    CHECK(ms.size() == MAX_MASKS_PER_LASER);
}
TEST(parse_reports_span_so_laser_keys_are_not_shadowed) {
    // lasers.json entry: the laser's own fields must be readable after cutting masks out.
    std::string obj = R"({"id":3,"masks":[{"x0":-0.5,"y0":-0.5,"x1":0.5,"y1":0.5,"reduce":40}],"zone_x":0.25})";
    std::vector<LaserMask> ms; std::size_t b = 0, e = 0;
    CHECK(parse_masks(obj, ms, &b, &e));
    obj.erase(b, e - b);
    CHECK(obj.find("x0") == std::string::npos);
    CHECK(json_int(obj, "id", 0) == 3);
    CHECK_NEAR(json_float(obj, "zone_x", 0), 0.25, 1e-6);
}
TEST(json_round_trip_with_and_without_outline) {
    const std::vector<LaserMask> in{M(-0.75f, -0.5f, 0.25f, 0.125f, 62.5f, true), M(0, 0, 1, 1, 100, false)};
    const std::string withOutline = masks_to_json(in, true), noOutline = masks_to_json(in, false);
    CHECK(noOutline.find("outline") == std::string::npos); // never persisted to lasers.json
    std::vector<LaserMask> a, b;
    CHECK(parse_masks("{\"masks\":" + withOutline + "}", a));
    CHECK(parse_masks("{\"masks\":" + noOutline + "}", b));
    CHECK(a.size() == 2 && b.size() == 2);
    if (a.size() == 2 && b.size() == 2) {
        for (int i = 0; i < 2; ++i) {
            CHECK_NEAR(a[i].x0, in[i].x0, 1e-4); CHECK_NEAR(a[i].y0, in[i].y0, 1e-4);
            CHECK_NEAR(a[i].x1, in[i].x1, 1e-4); CHECK_NEAR(a[i].y1, in[i].y1, 1e-4);
            CHECK_NEAR(a[i].reduce, in[i].reduce, 1e-4);
            CHECK(a[i].outline == in[i].outline);
            CHECK(!b[i].outline); // reloaded from disk: setup outline always off
        }
    }
}
TEST(json_empty_list_serializes) {
    CHECK(masks_to_json({}, true) == "[]");
}

// ════════════════════════════════════════════════════════════════════════════
int main() {
    std::printf("output_pipeline tests\n");
    for (const auto& t : registry()) {
        g_current = t.name;
        const int before = g_failures;
        t.fn();
        std::printf("  %s %s\n", g_failures == before ? "ok  " : "FAIL", t.name);
    }
    std::printf("%zu tests, %d checks, %d failed\n", registry().size(), g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
