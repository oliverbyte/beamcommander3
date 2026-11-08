#pragma once
// Hardware output safety pipeline: everything that happens to a show frame
// between make_frame() and a DAC's sendFrame() - blackout/arm gate,
// footswitch gate, per-laser mirror + zone pan/zoom, safety masks and the
// red mask setup outline. Lives in its own header (depending only on
// libera's header-only LaserPoint and on json_lite.h) so that
// backend/tests/test_output_pipeline.cpp can test exactly the code the
// daemon runs. Those tests run before every push (.githooks/pre-push).

#include "json_lite.h"
#include "libera/core/LaserPoint.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

using OutPoint = libera::core::LaserPoint;

// Safety masks (like Pangolin Beyond's / Liberation's beam masks): any
// number of rectangles per laser, in that laser's *physical output*
// coordinates (-1..1, y up - i.e. after its zone pan/zoom, so a mask stays
// on the same spot in the venue no matter how the show is zoned). Light
// inside a mask is reduced by `reduce` percent (100 = fully blanked); where
// masks overlap, the strongest reduction wins. `outline` is a setup aid
// only: while true, the mask's border is additionally drawn in red by the
// laser itself so the operator can see in the room what's masked. It is
// deliberately runtime-only (never written to lasers.json), so a restart
// can never leave red rectangles in a show. Set via
// POST /api/lasers/<id>/masks (ZoningPanel.vue).
struct LaserMask {
    float x0 = -0.25f, y0 = -0.25f, x1 = 0.25f, y1 = 0.25f; // x0<x1, y0<y1, all -1..1
    float reduce = 100.0f;  // 0..100 percent of output removed inside the mask
    bool  outline = false;  // draw the border in red on the laser (runtime-only)
};
inline constexpr std::size_t MAX_MASKS_PER_LASER = 64;

// Upper bound for a frame after mask edge splits. make_frame() already keeps
// the show itself under 3000 points; splits may add to that, but never past
// this, and outlines never past kOutlineFrameMaxPoints (the Ether Dream
// buffer is 4095 points - see make_frame()'s HARDWARE SAFETY CAP comment for
// why overrunning it matters).
inline constexpr std::size_t kMaskFrameMaxPoints    = 3800;
inline constexpr std::size_t kOutlineFrameMaxPoints = 4000;
inline constexpr float       kOutlineRed            = 0.6f; // fixed setup brightness, independent of the show

// Brightness multiplier at (x, y): 1 outside every mask, otherwise
// 1 - reduce% of the strongest mask containing the point (borders count as
// inside).
inline float mask_factor_at(const std::vector<LaserMask>& masks, float x, float y) {
    float f = 1.0f;
    for (const auto& m : masks)
        if (x >= m.x0 && x <= m.x1 && y >= m.y0 && y <= m.y1)
            f = std::min(f, 1.0f - std::clamp(m.reduce, 0.0f, 100.0f) / 100.0f);
    return f;
}

// Dims every point inside a mask. A lit segment that crosses a mask border
// is split exactly at the crossing (the point is emitted twice: once with
// the brightness before the border, once with the one after), so the cut
// lands on the border even when the shape only has a few widely spaced
// points (e.g. a square or a low-point line) - per-point dimming alone
// would let such a segment shine straight across a mask. The frame loops,
// so the last point's segment wraps back to the first. If splitting would
// exceed kMaskFrameMaxPoints, the remaining segments fall back to per-point
// dimming (every point is still dimmed - only the exact border cut is lost).
inline void apply_masks(std::vector<OutPoint>& pts, const std::vector<LaserMask>& masks) {
    if (masks.empty() || pts.empty()) return;
    const std::vector<OutPoint> in = std::move(pts);
    const std::size_t n = in.size();
    std::vector<OutPoint> out;
    out.reserve(std::min<std::size_t>(n * 2, kMaskFrameMaxPoints));
    std::vector<float> ts;

    for (std::size_t i = 0; i < n; ++i) {
        const auto& a = in[i];
        OutPoint pa = a;
        const float fa = mask_factor_at(masks, a.x, a.y);
        pa.r *= fa; pa.g *= fa; pa.b *= fa;
        out.push_back(pa);

        const auto& b = in[(i + 1) % n];
        const bool lit = (a.r + a.g + a.b) > 0.0f || (b.r + b.g + b.b) > 0.0f;
        if (!lit || n < 2) continue;

        // Every parameter t in (0,1) where a->b crosses a mask border.
        ts.clear();
        const float dx = b.x - a.x, dy = b.y - a.y;
        // No epsilon: a crossing right next to a lit point still matters
        // (otherwise a point just outside a mask would be drawn lit
        // straight into it). Duplicate points at near-equal positions are
        // harmless for the DAC.
        auto addCross = [&](float t) { if (t > 0.0f && t < 1.0f) ts.push_back(t); };
        for (const auto& m : masks) {
            for (float xb : {m.x0, m.x1}) {
                if ((a.x - xb) * (b.x - xb) < 0.0f) {
                    float t = (xb - a.x) / dx, y = a.y + dy * t;
                    if (y >= m.y0 && y <= m.y1) addCross(t);
                }
            }
            for (float yb : {m.y0, m.y1}) {
                if ((a.y - yb) * (b.y - yb) < 0.0f) {
                    float t = (yb - a.y) / dy, x = a.x + dx * t;
                    if (x >= m.x0 && x <= m.x1) addCross(t);
                }
            }
        }
        if (ts.empty()) continue;
        std::sort(ts.begin(), ts.end());

        float prevT = 0.0f;
        for (std::size_t k = 0; k < ts.size(); ++k) {
            const float t = ts[k];
            const float nextT = k + 1 < ts.size() ? ts[k + 1] : 1.0f;
            const float mb = (prevT + t) * 0.5f, ma = (t + nextT) * 0.5f;
            const float fBefore = mask_factor_at(masks, a.x + dx * mb, a.y + dy * mb);
            const float fAfter  = mask_factor_at(masks, a.x + dx * ma, a.y + dy * ma);
            prevT = t;
            if (fBefore == fAfter) continue;
            // Budget guard: always keep room for the remaining original points.
            if (out.size() + 2 + (n - i) > kMaskFrameMaxPoints) break;
            OutPoint q = a;
            q.x = a.x + dx * t; q.y = a.y + dy * t;
            const float cr = a.r + (b.r - a.r) * t, cg = a.g + (b.g - a.g) * t, cb = a.b + (b.b - a.b) * t;
            q.r = cr * fBefore; q.g = cg * fBefore; q.b = cb * fBefore; out.push_back(q);
            q.r = cr * fAfter;  q.g = cg * fAfter;  q.b = cb * fAfter;  out.push_back(q);
        }
    }
    pts = std::move(out);
}

// Appends the border of every mask with outline == true, drawn in red, to
// the end of the (already masked) frame - so the outline itself is never
// dimmed by its own mask. Travel between the show and each rectangle is
// blanked and interpolated (gentle on the galvos), with settle/dwell points
// at corners so the edges come out straight and the corners don't streak.
// Edge point density shrinks if the frame would otherwise exceed
// kOutlineFrameMaxPoints; if not even the minimum fits, no outline is drawn.
inline void append_mask_outlines(std::vector<OutPoint>& pts, const std::vector<LaserMask>& masks) {
    constexpr float kBlankPerUnit   = 24.0f; // blanked travel points per unit distance
    constexpr int   kMaxTravel = 48, kSettle = 4, kCornerDwell = 3, kTrail = 2;
    constexpr float kMaxEdgeDensity = 40.0f; // lit points per unit of edge length
    constexpr float kMinEdgeDensity = 4.0f;

    std::vector<const LaserMask*> outlined;
    float totalLen = 0.0f;
    for (const auto& m : masks)
        if (m.outline) { outlined.push_back(&m); totalLen += 2.0f * ((m.x1 - m.x0) + (m.y1 - m.y0)); }
    if (outlined.empty()) return;

    // Fixed cost per rectangle (travel + settle + corners + trail) plus the
    // travel back to the frame start; whatever's left goes to the edges -
    // each edge also has a floor of 2 points.
    const std::size_t fixedPerMask = 1 + kMaxTravel + kSettle + 5 * kCornerDwell + kTrail + 4 * 2;
    const std::size_t fixedTotal = fixedPerMask * outlined.size() + 1 + kMaxTravel + kSettle;
    const std::size_t used = pts.size() + fixedTotal;
    if (used >= kOutlineFrameMaxPoints) return; // no room at all - never overrun the DAC
    const float budget = (float)(kOutlineFrameMaxPoints - used);
    const float density = std::min(kMaxEdgeDensity, budget / std::max(0.01f, totalLen));
    if (density < kMinEdgeDensity) return;

    const bool hadShow = !pts.empty();
    const float startX = hadShow ? pts.front().x : 0.0f, startY = hadShow ? pts.front().y : 0.0f;
    float cx = hadShow ? pts.back().x : 0.0f, cy = hadShow ? pts.back().y : 0.0f;

    auto blank = [&](float x, float y) { pts.push_back(OutPoint{x, y, 0.0f, 0.0f, 0.0f, 1.0f}); };
    auto red   = [&](float x, float y) { pts.push_back(OutPoint{x, y, kOutlineRed, 0.0f, 0.0f, 1.0f}); };
    auto blankTravel = [&](float tx, float ty) {
        blank(cx, cy); // switch off before moving, never drag a lit point along
        const float dist = std::hypot(tx - cx, ty - cy);
        const int steps = std::clamp((int)std::round(dist * kBlankPerUnit), 0, kMaxTravel);
        for (int s = 1; s <= steps; ++s) {
            const float t = (float)s / (float)steps;
            blank(cx + (tx - cx) * t, cy + (ty - cy) * t);
        }
        for (int s = 0; s < kSettle; ++s) blank(tx, ty);
        cx = tx; cy = ty;
    };

    for (const LaserMask* m : outlined) {
        const float corners[5][2] = {{m->x0, m->y0}, {m->x1, m->y0}, {m->x1, m->y1}, {m->x0, m->y1}, {m->x0, m->y0}};
        blankTravel(corners[0][0], corners[0][1]);
        for (int c = 0; c < 4; ++c) {
            const float ax = corners[c][0], ay = corners[c][1];
            const float bx = corners[c + 1][0], by = corners[c + 1][1];
            for (int d = 0; d < kCornerDwell; ++d) red(ax, ay);
            const int steps = std::max(2, (int)std::round(std::hypot(bx - ax, by - ay) * density));
            for (int s = 1; s <= steps; ++s) {
                const float t = (float)s / (float)steps;
                red(ax + (bx - ax) * t, ay + (by - ay) * t);
            }
        }
        for (int d = 0; d < kCornerDwell; ++d) red(corners[4][0], corners[4][1]);
        for (int d = 0; d < kTrail; ++d) blank(corners[4][0], corners[4][1]);
        cx = corners[4][0]; cy = corners[4][1];
    }
    // The DAC loops the frame: travel back (blanked) to where the show starts.
    blankTravel(startX, startY);
}

// Per-laser hardware output settings for finalize_hardware_frame().
struct HardwareOutputParams {
    bool  blackout = true;         // global safety gate (LaserState::blackout)
    bool  outputAllowed = false;   // laser configured + armed + its output armed
    float gateLevel = 1.0f;        // footswitch fade multiplier (g_gate_level)
    bool  mirrorX = false;         // LaserConfig::mirror_x
    bool  mirrorY = false;         // LaserConfig::mirror_y
    float zoneX = 0.0f, zoneY = 0.0f, zoneScaleX = 1.0f, zoneScaleY = 1.0f;
    const std::vector<LaserMask>* masks = nullptr;
};

// The complete, ordered hardware-only processing of one laser's frame
// (used for the DAC copy *and* the WS preview - the preview calls it with
// blackout = false, everything else identical):
//  1. blackout or not armed -> every point dark; else footswitch gate fade
//  2. per-laser mirror (x and/or y), then zone pan/zoom; points pushed outside +-1 are
//     blanked (not clamped - see make_frame()'s off-screen comment)
//  3. safety masks, in physical output space (so after the zone)
//  4. red mask outlines, only if not blackout and armed - deliberately not
//     scaled by the show's intensity/footswitch, so they stay visible while
//     the operator lines masks up
inline void finalize_hardware_frame(std::vector<OutPoint>& pts, const HardwareOutputParams& p) {
    if (p.blackout || !p.outputAllowed) {
        for (auto& q : pts) { q.r = 0.0f; q.g = 0.0f; q.b = 0.0f; }
    } else if (p.gateLevel < 1.0f) {
        const float g = std::max(0.0f, p.gateLevel);
        for (auto& q : pts) { q.r *= g; q.g *= g; q.b *= g; }
    }
    if (p.mirrorX || p.mirrorY || p.zoneScaleX != 1.0f || p.zoneScaleY != 1.0f || p.zoneX != 0.0f || p.zoneY != 0.0f) {
        for (auto& q : pts) {
            const float x  = p.mirrorX ? -q.x : q.x;
            const float zx = x * p.zoneScaleX + p.zoneX;
            const float y  = p.mirrorY ? -q.y : q.y;
            const float zy = y * p.zoneScaleY + p.zoneY;
            q.x = zx; q.y = zy;
            if (std::fabs(zx) > 1.0f || std::fabs(zy) > 1.0f) { q.r = 0.0f; q.g = 0.0f; q.b = 0.0f; }
        }
    }
    if (p.masks && !p.masks->empty()) {
        apply_masks(pts, *p.masks);
        if (!p.blackout && p.outputAllowed) append_mask_outlines(pts, *p.masks);
    }
}

// Masks are serialized as a "masks":[{...},...] array nested in a laser
// object. withOutline=false is used for lasers.json, since `outline` is a
// runtime-only setup aid (see LaserMask).
inline std::string masks_to_json(const std::vector<LaserMask>& masks, bool withOutline) {
    std::ostringstream ss; ss << std::fixed << std::setprecision(4) << "[";
    for (std::size_t i = 0; i < masks.size(); ++i) {
        const auto& m = masks[i];
        if (i) ss << ",";
        ss << "{\"x0\":" << m.x0 << ",\"y0\":" << m.y0 << ",\"x1\":" << m.x1 << ",\"y1\":" << m.y1
           << ",\"reduce\":" << m.reduce;
        if (withOutline) ss << ",\"outline\":" << (m.outline ? "true" : "false");
        ss << "}";
    }
    ss << "]";
    return ss.str();
}

// Finds the "masks":[ ... ] array in `body`. Returns false if the key is
// absent, the array is malformed or any number in it is not finite (a NaN
// coordinate would silently disable a mask, so the whole list is refused
// rather than half-applied). Otherwise fills `masks` (clamped to -1..1 /
// 0..100, corners normalized so x0<=x1 and y0<=y1, capped at
// MAX_MASKS_PER_LASER) and, if given, the [begin,end) span of the whole
// "masks":[...] entry so callers can cut it out before reading the
// surrounding object's own keys.
inline bool parse_masks(const std::string& body, std::vector<LaserMask>& masks,
                        std::size_t* spanBegin = nullptr, std::size_t* spanEnd = nullptr) {
    auto key = body.find("\"masks\"");
    if (key == std::string::npos) return false;
    auto open = body.find('[', key);
    if (open == std::string::npos) return false;
    std::size_t close = std::string::npos;
    int depth = 0;
    for (std::size_t j = open; j < body.size(); ++j) {
        if (body[j] == '[') ++depth;
        else if (body[j] == ']' && --depth == 0) { close = j; break; }
    }
    if (close == std::string::npos) return false;
    std::vector<LaserMask> parsed;
    std::size_t i = body.find('{', open);
    while (i != std::string::npos && i < close && parsed.size() < MAX_MASKS_PER_LASER) {
        auto e = body.find('}', i);
        if (e == std::string::npos || e > close) return false;
        const std::string obj = body.substr(i, e - i + 1);
        LaserMask m;
        const float x0 = json_float(obj, "x0", m.x0), x1 = json_float(obj, "x1", m.x1);
        const float y0 = json_float(obj, "y0", m.y0), y1 = json_float(obj, "y1", m.y1);
        const float reduce = json_float(obj, "reduce", m.reduce);
        for (float v : {x0, x1, y0, y1, reduce}) if (!std::isfinite(v)) return false;
        m.x0 = std::clamp(std::min(x0, x1), -1.0f, 1.0f); m.x1 = std::clamp(std::max(x0, x1), -1.0f, 1.0f);
        m.y0 = std::clamp(std::min(y0, y1), -1.0f, 1.0f); m.y1 = std::clamp(std::max(y0, y1), -1.0f, 1.0f);
        m.reduce  = std::clamp(reduce, 0.0f, 100.0f);
        m.outline = json_bool(obj, "outline", false);
        parsed.push_back(m);
        i = body.find('{', e);
    }
    masks = std::move(parsed);
    if (spanBegin) *spanBegin = key;
    if (spanEnd) *spanEnd = close + 1;
    return true;
}
