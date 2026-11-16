// Unit tests for laser text (laser_text.h): Hershey font parsing, the font
// catalog, text layout (styles, umlauts, centering) and the laser points
// (budget, blanked travel), plus the JSON string helpers used for text.
// Run before every push by .githooks/pre-push (via tests/run_tests.sh).
//
// Run manually:  bash backend/tests/run_tests.sh

#include "json_lite.h"
#include "laser_text.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
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

using LP = libera::core::LaserPoint;
static bool lit(const LP& p) { return p.r > 0.0f || p.g > 0.0f || p.b > 0.0f; }
static std::string backendDir() {
    std::string f = __FILE__;
    for (int up = 0; up < 2; ++up) { auto s = f.find_last_of("/\\"); f = s == std::string::npos ? "." : f.substr(0, s); }
    return f;
}
struct Box { float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f; };
static Box box(const std::vector<TextStroke>& strokes) {
    Box b;
    for (const auto& s : strokes) for (const auto& p : s) {
        b.minX = std::min(b.minX, p.x); b.maxX = std::max(b.maxX, p.x);
        b.minY = std::min(b.minY, p.y); b.maxY = std::max(b.maxY, p.y);
    }
    return b;
}

// ── Fonts ─────────────────────────────────────────────────────────────────────
TEST(embedded_fonts_are_the_files_byte_for_byte) {
    const std::pair<const char*, const char*> files[] = {
        {"cursive", HERSHEY_cursive}, {"futural", HERSHEY_futural}, {"futuram", HERSHEY_futuram},
        {"gothiceng", HERSHEY_gothiceng}, {"gothicger", HERSHEY_gothicger}, {"rowmand", HERSHEY_rowmand},
        {"rowmans", HERSHEY_rowmans}, {"scriptc", HERSHEY_scriptc}, {"scripts", HERSHEY_scripts},
        {"timesi", HERSHEY_timesi}, {"timesib", HERSHEY_timesib}, {"timesr", HERSHEY_timesr},
        {"timesrb", HERSHEY_timesrb},
    };
    for (const auto& [name, text] : files) {
        std::ifstream in(backendDir() + "/fonts/hershey/" + name + ".jhf", std::ios::binary);
        const std::string file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(!file.empty());
        CHECK(file == std::string(text));
    }
}

TEST(every_catalog_face_parses_with_all_96_ascii_glyphs) {
    CHECK(text_font_catalog().size() >= 5);
    for (const auto& f : text_font_catalog()) {
        for (const char* face : {f.regular, f.bold, f.italic, f.boldItalic}) {
            if (!face) continue;
            const HersheyFont* hf = hershey_face(face);
            CHECK(hf != nullptr);
            if (!hf) continue;
            CHECK(hf->glyphs.size() == 96);
            CHECK(hf->glyphs[0].strokes.empty());                 // space
            CHECK(!hf->glyphs['A' - 32].strokes.empty());
            CHECK(hf->glyphs['A' - 32].right > hf->glyphs['A' - 32].left);
        }
    }
    CHECK(find_text_font("times") && text_font_has_bold(*find_text_font("times")) && text_font_has_italic(*find_text_font("times")));
    CHECK(!text_font_has_bold(*find_text_font("cursive")));
    CHECK(!text_font_has_italic(*find_text_font("script")));
    CHECK(find_text_font("nope") == nullptr);
}

TEST(parse_jhf_reads_continuation_lines_and_pen_up) {
    // Glyph with 5 vertex pairs split over two lines: bounds, (0,0)-(1,1), pen up, (2,2).
    HersheyFont f;
    std::string one = "12345  5JZRRSS\n RTT\n";
    std::string text;
    for (int i = 0; i < 96; ++i) text += one;
    CHECK(parse_jhf(text.c_str(), f));
    CHECK(f.glyphs.size() == 96);
    CHECK(f.glyphs[0].left == -8 && f.glyphs[0].right == 8);
    if (f.glyphs.empty()) return;
    CHECK(f.glyphs[0].strokes.size() == 2);
    if (f.glyphs[0].strokes.size() != 2) return;
    CHECK(f.glyphs[0].strokes[0].size() == 2 && f.glyphs[0].strokes[1].size() == 1);
    CHECK(f.glyphs[0].strokes[1].front() == std::make_pair(2, 2));
    HersheyFont bad;
    CHECK(!parse_jhf("12345  9JZRR\n", bad)); // record shorter than its count
    CHECK(!parse_jhf("", bad));
}

// ── Layout ────────────────────────────────────────────────────────────────────
TEST(layout_is_centered_and_sized_by_cap_height) {
    TextStyle st; st.size = 0.4f;
    const auto s = layout_text("HELLO", st);
    CHECK(!s.empty());
    const Box b = box(s);
    CHECK_NEAR((b.minX + b.maxX) * 0.5, 0.0, 1e-4);
    CHECK_NEAR((b.minY + b.maxY) * 0.5, 0.0, 1e-4);
    CHECK_NEAR(b.maxY - b.minY, 0.4, 0.02); // capitals: height = cap height
    st.size = 0.2f;
    const Box half = box(layout_text("HELLO", st));
    CHECK_NEAR((half.maxX - half.minX) * 2.0, b.maxX - b.minX, 1e-3);
}

TEST(spacing_widens_text) {
    TextStyle st;
    const Box tight = box(layout_text("ABC", st));
    st.spacing = 0.5f;
    const Box wide = box(layout_text("ABC", st));
    CHECK_NEAR((wide.maxX - wide.minX) - (tight.maxX - tight.minX), 2 * 0.5 * st.size, 1e-3);
}

TEST(bold_italic_underline) {
    TextStyle st; st.font = "sans";
    const auto regular = layout_text("A", st);
    st.bold = true;
    const auto bold = layout_text("A", st);
    CHECK(bold.size() > regular.size()); // the medium face draws double strokes

    st.bold = false; st.italic = true;
    const auto slanted = layout_text("I", st);
    TextStyle up; up.font = "sans";
    const auto upright = layout_text("I", up);
    const Box bs = box(slanted), bu = box(upright);
    CHECK((bs.maxX - bs.minX) > (bu.maxX - bu.minX) + 0.01f); // the vertical stroke leans

    TextStyle ul; ul.underline = true;
    const auto underlined = layout_text("AB", ul);
    TextStyle plain;
    CHECK(underlined.size() == layout_text("AB", plain).size() + 1);
    CHECK(box(underlined).minY < box(layout_text("AB", plain)).minY);

    TextStyle times; times.font = "times"; times.italic = true;
    const auto ti = layout_text("A", times);
    times.italic = false;
    CHECK(ti.size() != layout_text("A", times).size() || box(ti).minX != box(layout_text("A", times)).minX); // a different face
}

TEST(umlauts_are_letter_plus_two_dots_and_sharp_s_is_ss) {
    TextStyle st;
    const auto a = layout_text("a", st), ae = layout_text("\xC3\xA4", st); // ä
    CHECK(ae.size() == a.size() + 2);
    const auto U = layout_text("U", st), Ue = layout_text("\xC3\x9C", st); // Ü
    CHECK(Ue.size() == U.size() + 2);
    CHECK(box(Ue).maxY - box(Ue).minY > box(U).maxY - box(U).minY); // dots above the capital
    const auto ss = layout_text("ss", st), sz = layout_text("\xC3\x9F", st); // ß
    CHECK(ss.size() == sz.size());
    const auto chars = text_chars("x\n\x01\xE2\x82\xAC"); // newline/control dropped, € -> ?
    CHECK(chars.size() == 2 && chars[0].base == 'x' && chars[1].base == '?');
    CHECK(text_chars(std::string(500, 'A')).size() == kMaxTextChars);
    CHECK(layout_text("", st).empty());
    CHECK(layout_text("   ", st).empty());
}

// ── Laser points ──────────────────────────────────────────────────────────────
TEST(points_fit_the_budget_and_blank_every_jump) {
    TextStyle st; st.size = 0.3f;
    const auto strokes = layout_text("BEAMCOMMANDER 3", st);
    for (std::size_t budget : {3000u, 1500u, 600u, 200u}) {
        const auto pts = text_points(strokes, budget);
        CHECK(!pts.empty());
        CHECK(pts.size() <= budget);
        // A lit segment never spans a big jump (those are blanked travel).
        float maxLitStep = 0.0f;
        for (std::size_t i = 1; i < pts.size(); ++i)
            if (lit(pts[i]) && lit(pts[i - 1]))
                maxLitStep = std::max(maxLitStep, std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y));
        CHECK(maxLitStep <= 0.26f);
    }
    // Plenty of budget: fine spacing, the whole text drawn.
    const auto fine = text_points(strokes, 3000);
    std::size_t litCount = 0;
    for (const auto& p : fine) litCount += lit(p);
    CHECK(litCount > 200);
    // Frame loops: ends near its first point.
    CHECK(std::hypot(fine.back().x - fine.front().x, fine.back().y - fine.front().y) < 0.1f);
    // Nothing to draw: a single dark point.
    const auto none = text_points({}, 3000);
    CHECK(none.size() == 1 && !lit(none[0]));
}

TEST(lit_points_follow_the_strokes) {
    // One straight stroke: every lit point lies on it.
    std::vector<TextStroke> s = {{{-0.5f, 0.0f}, {0.5f, 0.0f}}};
    const auto pts = text_points(s, 500);
    for (const auto& p : pts) if (lit(p)) { CHECK_NEAR(p.y, 0.0, 1e-6); CHECK(p.x >= -0.5f - 1e-6f && p.x <= 0.5f + 1e-6f); }
}

// ── JSON text ─────────────────────────────────────────────────────────────────
TEST(json_string_round_trips_quotes_backslashes_and_unicode) {
    const std::string text = "Say \"Hi\" \\ \xC3\xA4";
    const std::string body = "{\"text\":" + json_escape(text) + ",\"r\":0.5}";
    std::string out;
    CHECK(json_string(body, "text", out));
    CHECK(out == text);
    CHECK_NEAR(json_float(body, "r", -1), 0.5, 1e-6);
    CHECK(json_string("{\"text\":\"\\u00e4\\n\"}", "text", out) && out == "\xC3\xA4\n");
    CHECK(!json_string("{\"text\":5}", "text", out));
    CHECK(!json_string("{\"other\":\"x\"}", "text", out));
    // An escaped quote inside a value never looks like a key.
    const std::string tricky = "{\"text\":" + json_escape("\"shape\":\"line\"") + ",\"shape\":\"circle\"}";
    CHECK(json_str(tricky, "shape") == "circle");
    CHECK(json_escape("a\nb") == "\"ab\"");
    // A value equal to a key name (shape "text") is not the key "text".
    const std::string look = "{\"shape\":\"text\",\"x\":1,\"text\":\"Hi\",\"font\":\"sans\"}";
    CHECK(json_string(look, "text", out) && out == "Hi");
    CHECK(json_str(look, "shape") == "text");
    CHECK(json_has(look, "font") && !json_has("{\"a\":\"font\"}", "font"));
    CHECK_NEAR(json_float("{\"name\":\"r\",\"r\":0.25}", "r", -1), 0.25, 1e-6);
}

int main() {
    std::printf("laser_text tests\n");
    for (const auto& t : registry()) {
        g_current = t.name;
        const int before = g_failures;
        t.fn();
        std::printf("  %s %s\n", g_failures == before ? "ok  " : "FAIL", t.name);
    }
    std::printf("%zu tests, %d checks, %d failed\n", registry().size(), g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
