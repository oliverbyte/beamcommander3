#pragma once
// Laser text: single-stroke Hershey fonts (backend/fonts/hershey/, embedded
// via hershey_font_data.h - see LICENSE-HERSHEY.txt there: The Hershey Fonts
// were originally created by Dr. A. V. Hershey while working at the U. S.
// National Bureau of Standards; the font data format by James Hurt,
// Cognition, Inc.). Laser text needs single strokes, not outline fonts.
//
//  parse_jhf()       one .jhf file -> 96 glyphs (ASCII 32..127, file order)
//  text_font_catalog the selectable fonts and which styles each has
//  layout_text()     UTF-8 text -> strokes in output units (y up, centered)
//  text_points()     strokes -> laser points: blanked, interpolated travel
//                    between strokes, settle/dwell at stroke ends, fitted
//                    into the point budget (the DAC buffer)
//
// Header-only and libera-light (only LaserPoint), so backend/tests/
// test_laser_text.cpp tests exactly the code the daemon runs.

#include "hershey_font_data.h"
#include "libera/core/LaserPoint.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

struct HersheyGlyph {
    int left = 0, right = 0;                                // horizontal bounds (advance = right - left)
    std::vector<std::vector<std::pair<int, int>>> strokes;  // Hershey units, y grows downwards
};
struct HersheyFont { std::vector<HersheyGlyph> glyphs; };   // [0] = ASCII 32

// .jhf: per glyph a 5-char number, a 3-char vertex count, then 2 chars per
// vertex (value = char - 'R'; the first pair is the left/right bound, " R"
// lifts the pen). Long records continue on the next line.
inline bool parse_jhf(const char* text, HersheyFont& font) {
    font.glyphs.clear();
    const char* p = text;
    while (true) {
        while (*p == '\n' || *p == '\r') ++p;
        if (!*p) break;
        for (int i = 0; i < 8; ++i) if (!p[i] || p[i] == '\n') return false;
        const int n = std::atoi(std::string(p + 5, 3).c_str());
        p += 8;
        if (n < 1) return false;
        std::string data;
        while (data.size() < (std::size_t)(2 * n) && *p) {
            if (*p != '\n' && *p != '\r') data.push_back(*p);
            ++p;
        }
        if (data.size() < (std::size_t)(2 * n)) return false;
        HersheyGlyph g;
        g.left = data[0] - 'R';
        g.right = data[1] - 'R';
        g.strokes.emplace_back();
        for (int i = 1; i < n; ++i) {
            const char a = data[2 * i], b = data[2 * i + 1];
            if (a == ' ' && b == 'R') { g.strokes.emplace_back(); continue; }
            g.strokes.back().emplace_back(a - 'R', b - 'R');
        }
        g.strokes.erase(std::remove_if(g.strokes.begin(), g.strokes.end(),
                                       [](const auto& s) { return s.empty(); }), g.strokes.end());
        font.glyphs.push_back(std::move(g));
    }
    return font.glyphs.size() >= 95;
}

// ── Fonts ─────────────────────────────────────────────────────────────────────
// Each font: its regular face and - where Hershey has one - real bold
// (duplex/medium strokes) and italic faces. `slant`: italic may be made by
// slanting a face that has no italic of its own (not for script fonts, which
// are slanted already).
struct TextFontInfo {
    const char* id;
    const char* label;
    const char* regular;
    const char* bold;        // nullptr = no bold
    const char* italic;      // nullptr = slanted (if `slant`) or none
    const char* boldItalic;
    bool slant;
};
inline const std::vector<TextFontInfo>& text_font_catalog() {
    static const std::vector<TextFontInfo> c = {
        {"sans",           "Sans",           "futural",   "futuram", nullptr,  nullptr,   true},
        {"roman",          "Roman",          "rowmans",   "rowmand", nullptr,  nullptr,   true},
        {"times",          "Times",          "timesr",    "timesrb", "timesi", "timesib", false},
        {"script",         "Script",         "scripts",   "scriptc", nullptr,  nullptr,   false},
        {"gothic_english", "Gothic English", "gothiceng", nullptr,   nullptr,  nullptr,   true},
        {"gothic_german",  "Gothic German",  "gothicger", nullptr,   nullptr,  nullptr,   true},
        {"cursive",        "Cursive",        "cursive",   nullptr,   nullptr,  nullptr,   false},
    };
    return c;
}
inline const TextFontInfo* find_text_font(const std::string& id) {
    for (const auto& f : text_font_catalog()) if (id == f.id) return &f;
    return nullptr;
}
inline bool text_font_has_bold(const TextFontInfo& f) { return f.bold != nullptr; }
inline bool text_font_has_italic(const TextFontInfo& f) { return f.italic != nullptr || f.slant; }

// Every embedded face, parsed once.
inline const HersheyFont* hershey_face(const std::string& name) {
    static const std::map<std::string, HersheyFont> faces = [] {
        const std::pair<const char*, const char*> files[] = {
            {"cursive", HERSHEY_cursive}, {"futural", HERSHEY_futural}, {"futuram", HERSHEY_futuram},
            {"gothiceng", HERSHEY_gothiceng}, {"gothicger", HERSHEY_gothicger}, {"rowmand", HERSHEY_rowmand},
            {"rowmans", HERSHEY_rowmans}, {"scriptc", HERSHEY_scriptc}, {"scripts", HERSHEY_scripts},
            {"timesi", HERSHEY_timesi}, {"timesib", HERSHEY_timesib}, {"timesr", HERSHEY_timesr},
            {"timesrb", HERSHEY_timesrb},
        };
        std::map<std::string, HersheyFont> m;
        for (const auto& [name, text] : files) {
            HersheyFont f;
            if (parse_jhf(text, f)) m.emplace(name, std::move(f));
        }
        return m;
    }();
    auto it = faces.find(name);
    return it != faces.end() ? &it->second : nullptr;
}

// ── Layout ────────────────────────────────────────────────────────────────────
struct TextStyle {
    std::string font = "sans";
    bool  bold = false, italic = false, underline = false;
    float size = 0.3f;     // cap height in output units (the full output is 2 wide)
    float spacing = 0.0f;  // extra letter spacing, in cap heights
};
struct TextPt { float x, y; };
using TextStroke = std::vector<TextPt>;

inline constexpr float kHersheyCapHeight = 21.0f; // 'A' spans y -12..9 in Hershey units
inline constexpr float kItalicSlant      = 0.22f; // slanted italic: x += y * this
inline constexpr std::size_t kMaxTextChars = 120;

// Decodes UTF-8 into the characters we can draw: printable ASCII as is,
// German umlauts as their base letter plus two dots, ß as "ss", anything else
// as '?'. Control characters (incl. line breaks) are dropped.
struct TextChar { char base; bool dots; };
inline std::vector<TextChar> text_chars(const std::string& utf8) {
    std::vector<TextChar> out;
    for (std::size_t i = 0; i < utf8.size() && out.size() < kMaxTextChars;) {
        const unsigned char c = (unsigned char)utf8[i];
        if (c < 0x80) {
            if (c == '\t') out.push_back({' ', false});
            else if (c >= 32 && c < 127) out.push_back({(char)c, false});
            ++i;
            continue;
        }
        int len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
        unsigned cp = 0;
        if (len == 2 && i + 1 < utf8.size()) cp = ((c & 0x1F) << 6) | ((unsigned char)utf8[i + 1] & 0x3F);
        switch (cp) {
            case 0xE4: out.push_back({'a', true}); break;
            case 0xF6: out.push_back({'o', true}); break;
            case 0xFC: out.push_back({'u', true}); break;
            case 0xC4: out.push_back({'A', true}); break;
            case 0xD6: out.push_back({'O', true}); break;
            case 0xDC: out.push_back({'U', true}); break;
            case 0xDF: out.push_back({'s', false}); out.push_back({'s', false}); break;
            default:   out.push_back({'?', false}); break;
        }
        i += len;
    }
    return out;
}

// Text -> strokes in output units, centered on (0,0), y up. Unknown font =
// Sans; bold/italic use the font's own faces where it has them.
inline std::vector<TextStroke> layout_text(const std::string& utf8, const TextStyle& st) {
    const TextFontInfo* info = find_text_font(st.font);
    if (!info) info = &text_font_catalog().front();
    const char* faceName = info->regular;
    bool trueItalic = false;
    if (st.bold && st.italic && info->boldItalic) { faceName = info->boldItalic; trueItalic = true; }
    else if (st.italic && info->italic && !(st.bold && info->bold && !info->boldItalic)) { faceName = info->italic; trueItalic = true; }
    else if (st.bold && info->bold) faceName = info->bold;
    const HersheyFont* face = hershey_face(faceName);
    if (!face) return {};
    const float slant = (st.italic && !trueItalic && info->slant) ? kItalicSlant : 0.0f;

    const float k = std::max(0.001f, st.size) / kHersheyCapHeight;
    std::vector<TextStroke> strokes;
    float pen = 0.0f;
    auto emit = [&](TextStroke s) {
        for (auto& p : s) p.x += p.y * slant;
        strokes.push_back(std::move(s));
    };
    for (const TextChar& ch : text_chars(utf8)) {
        const std::size_t idx = (std::size_t)((unsigned char)ch.base - 32);
        if (idx >= face->glyphs.size()) continue;
        const HersheyGlyph& g = face->glyphs[idx];
        const float ox = pen - g.left * k;
        for (const auto& hs : g.strokes) {
            TextStroke s;
            for (const auto& [hx, hy] : hs) s.push_back({ox + hx * k, -hy * k});
            emit(std::move(s));
        }
        if (ch.dots) {
            // Two short dashes above the letter (above the cap height for capitals).
            const float cx = ox + (g.left + g.right) * 0.5f * k;
            const float y = (ch.base >= 'A' && ch.base <= 'Z') ? 15.5f * k : 12.5f * k;
            const float d = 1.0f * k;
            for (float dx : {-3.5f * k, 3.5f * k}) emit({{cx + dx - d, y}, {cx + dx + d, y}});
        }
        pen += (g.right - g.left) * k + st.spacing * st.size;
    }
    if (st.underline && pen > 0.0f) emit({{0.0f, -14.0f * k}, {pen - st.spacing * st.size, -14.0f * k}});
    if (strokes.empty()) return strokes;

    float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
    for (const auto& s : strokes)
        for (const auto& p : s) { minX = std::min(minX, p.x); maxX = std::max(maxX, p.x); minY = std::min(minY, p.y); maxY = std::max(maxY, p.y); }
    const float cx = (minX + maxX) * 0.5f, cy = (minY + maxY) * 0.5f;
    for (auto& s : strokes) for (auto& p : s) { p.x -= cx; p.y -= cy; }
    return strokes;
}

// ── Laser points ──────────────────────────────────────────────────────────────
// Lit points are white (the caller colors them); travel between strokes is
// blanked and interpolated (gentle on the galvos), with settle points before
// a stroke lights up and dwell points at its ends so ends don't fade. The
// lit point spacing grows until everything fits `maxPoints`; if not even the
// coarsest spacing fits, the remaining strokes are left out - the frame
// never exceeds the budget. The frame loops, so it ends travelling back to
// its first point.
inline std::vector<libera::core::LaserPoint> text_points(const std::vector<TextStroke>& strokes, std::size_t maxPoints) {
    using LP = libera::core::LaserPoint;
    std::vector<LP> out;
    if (strokes.empty() || maxPoints < 16) { out.push_back(LP{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f}); return out; }

    constexpr float kTravelPerUnit = 14.0f; // blanked travel points per output unit
    constexpr int   kMaxTravel = 18, kSettle = 3, kDwell = 2, kTrail = 1;
    constexpr float kMinSpacing = 0.008f, kMaxSpacing = 0.25f;

    auto dist = [](const TextPt& a, const TextPt& b) { return std::hypot(b.x - a.x, b.y - a.y); };
    auto travelSteps = [&](float d) { return std::clamp((int)std::round(d * kTravelPerUnit), 0, kMaxTravel); };
    float litLen = 0.0f;
    std::size_t fixed = 0;
    TextPt prev = strokes.back().back();
    for (const auto& s : strokes) {
        for (std::size_t i = 1; i < s.size(); ++i) litLen += dist(s[i - 1], s[i]);
        fixed += 1 + travelSteps(dist(prev, s.front())) + kSettle + 2 * kDwell + kTrail + s.size();
        prev = s.back();
    }
    const float room = (float)maxPoints - (float)fixed;
    const float spacing = std::clamp(room > 1.0f ? litLen / room : kMaxSpacing, kMinSpacing, kMaxSpacing);

    auto push = [&](const TextPt& p, bool lit) {
        if (out.size() < maxPoints) out.push_back(lit ? LP{p.x, p.y, 1.0f, 1.0f, 1.0f, 1.0f} : LP{p.x, p.y, 0.0f, 0.0f, 0.0f, 1.0f});
    };
    TextPt cur = strokes.back().back();
    for (const auto& s : strokes) {
        // What this stroke needs at most - skip it (and the rest) if it can't fit.
        std::size_t need = 1 + travelSteps(dist(cur, s.front())) + kSettle + 2 * kDwell + kTrail;
        for (std::size_t i = 1; i < s.size(); ++i) need += 1 + (std::size_t)(dist(s[i - 1], s[i]) / spacing);
        if (out.size() + need + kMaxTravel + 1 > maxPoints) break;

        push(cur, false); // switch off before moving
        const int steps = travelSteps(dist(cur, s.front()));
        for (int i = 1; i <= steps; ++i) {
            const float t = (float)i / (float)steps;
            push({cur.x + (s.front().x - cur.x) * t, cur.y + (s.front().y - cur.y) * t}, false);
        }
        for (int i = 0; i < kSettle; ++i) push(s.front(), false);
        for (int i = 0; i < kDwell; ++i) push(s.front(), true);
        for (std::size_t i = 1; i < s.size(); ++i) {
            const TextPt a = s[i - 1], b = s[i];
            const int n = std::max(1, (int)std::ceil(dist(a, b) / spacing));
            for (int j = 1; j <= n; ++j) {
                const float t = (float)j / (float)n;
                push({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}, true);
            }
        }
        for (int i = 0; i < kDwell; ++i) push(s.back(), true);
        for (int i = 0; i < kTrail; ++i) push(s.back(), false);
        cur = s.back();
    }
    // Back to where the frame starts, blanked.
    if (!out.empty()) {
        const TextPt start{out.front().x, out.front().y};
        const int steps = travelSteps(dist(cur, start));
        for (int i = 1; i <= steps; ++i) {
            const float t = (float)i / (float)steps;
            push({cur.x + (start.x - cur.x) * t, cur.y + (start.y - cur.y) * t}, false);
        }
    }
    if (out.empty()) out.push_back(LP{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f});
    return out;
}
