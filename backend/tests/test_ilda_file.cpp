// Unit tests for the ILDA file reader (ilda_file.h) and the ILDA test
// patterns (test_patterns.h): all five ILDA formats, palette handling,
// blanking, malformed files, and that the embedded test pattern bytes are
// still exactly the official files in backend/test_patterns/. Run before
// every push by .githooks/pre-push (via tests/run_tests.sh).
//
// Run manually:  bash backend/tests/run_tests.sh

#include "test_patterns.h"

#include <cmath>
#include <cstdint>
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

// ── Helpers: build ILDA files byte by byte ──────────────────────────────────
using Bytes = std::vector<std::uint8_t>;
static void put16(Bytes& b, int v) { b.push_back((std::uint8_t)((v >> 8) & 0xff)); b.push_back((std::uint8_t)(v & 0xff)); }
static void header(Bytes& b, int format, int records, const char* name = "frame", int projector = 0) {
    for (char c : std::string("ILDA")) b.push_back((std::uint8_t)c);
    b.push_back(0); b.push_back(0); b.push_back(0); b.push_back((std::uint8_t)format);
    std::string n(name); n.resize(8, ' ');
    for (char c : n) b.push_back((std::uint8_t)c);
    for (char c : std::string("company ")) b.push_back((std::uint8_t)c);
    put16(b, records); put16(b, 0); put16(b, 1);
    b.push_back((std::uint8_t)projector); b.push_back(0);
}
static void eof(Bytes& b) { header(b, 0, 0, ""); }
static bool parse(const Bytes& b, std::vector<IldaFrame>& f) { return parse_ilda(b.data(), b.size(), f); }
static bool isBlank(const libera::core::LaserPoint& p) { return p.r == 0.0f && p.g == 0.0f && p.b == 0.0f; }

static Bytes readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
// backend/ from this file's own path (run_tests.sh compiles it by absolute path).
static std::string backendDir() {
    std::string f = __FILE__;
    for (int up = 0; up < 2; ++up) { auto s = f.find_last_of("/\\"); f = s == std::string::npos ? "." : f.substr(0, s); }
    return f;
}

// ── Format handling ─────────────────────────────────────────────────────────
TEST(format0_indexed_3d_uses_default_palette) {
    Bytes b; header(b, 0, 3, "abc");
    // x, y, z, status, color index
    put16(b, 32767); put16(b, -32768); put16(b, 0); b.push_back(0x00); b.push_back(0);   // red
    put16(b, 0);     put16(b, 16384);  put16(b, 0); b.push_back(0x00); b.push_back(56);  // white
    put16(b, -100);  put16(b, 100);    put16(b, 0); b.push_back(0x80); b.push_back(40);  // blue, last point
    eof(b);
    std::vector<IldaFrame> f;
    CHECK(parse(b, f));
    CHECK(f.size() == 1);
    CHECK(f[0].name == "abc");
    CHECK(f[0].company == "company");
    CHECK(f[0].points.size() == 3);
    CHECK_NEAR(f[0].points[0].x, 1.0, 1e-6);
    CHECK_NEAR(f[0].points[0].y, -1.0, 1e-6);   // -32768 clamps to -1
    CHECK_NEAR(f[0].points[0].r, 1.0, 1e-6); CHECK_NEAR(f[0].points[0].g, 0.0, 1e-6);
    CHECK_NEAR(f[0].points[1].y, 16384.0 / 32767.0, 1e-6);
    CHECK_NEAR(f[0].points[1].r, 1.0, 1e-6); CHECK_NEAR(f[0].points[1].g, 1.0, 1e-6); CHECK_NEAR(f[0].points[1].b, 1.0, 1e-6);
    CHECK_NEAR(f[0].points[2].b, 1.0, 1e-6); CHECK_NEAR(f[0].points[2].r, 0.0, 1e-6);
}

TEST(format1_indexed_2d) {
    Bytes b; header(b, 1, 2);
    put16(b, 100); put16(b, -100); b.push_back(0x00); b.push_back(24); // green
    put16(b, 200); put16(b, -200); b.push_back(0x80); b.push_back(16); // yellow
    eof(b);
    std::vector<IldaFrame> f;
    CHECK(parse(b, f));
    CHECK(f.size() == 1 && f[0].points.size() == 2);
    CHECK_NEAR(f[0].points[0].x, 100.0 / 32767.0, 1e-6);
    CHECK_NEAR(f[0].points[0].g, 1.0, 1e-6); CHECK_NEAR(f[0].points[0].r, 0.0, 1e-6);
    CHECK_NEAR(f[0].points[1].r, 1.0, 1e-6); CHECK_NEAR(f[0].points[1].g, 1.0, 1e-6);
}

TEST(format4_and_5_true_color_order_is_blue_green_red) {
    Bytes b; header(b, 4, 1);
    put16(b, 0); put16(b, 0); put16(b, 0); b.push_back(0x80); b.push_back(10); b.push_back(20); b.push_back(255);
    header(b, 5, 1);
    put16(b, 0); put16(b, 0); b.push_back(0x80); b.push_back(255); b.push_back(51); b.push_back(0);
    eof(b);
    std::vector<IldaFrame> f;
    CHECK(parse(b, f));
    CHECK(f.size() == 2);
    CHECK_NEAR(f[0].points[0].r, 1.0, 1e-6);
    CHECK_NEAR(f[0].points[0].g, 20.0 / 255.0, 1e-6);
    CHECK_NEAR(f[0].points[0].b, 10.0 / 255.0, 1e-6);
    CHECK_NEAR(f[1].points[0].b, 1.0, 1e-6);
    CHECK_NEAR(f[1].points[0].g, 0.2, 1e-6);
    CHECK_NEAR(f[1].points[0].r, 0.0, 1e-6);
}

TEST(blanking_bit_wins_over_any_color) {
    Bytes b; header(b, 1, 1);
    put16(b, 0); put16(b, 0); b.push_back(0x40 | 0x80); b.push_back(56); // white, but blanked
    header(b, 5, 1);
    put16(b, 0); put16(b, 0); b.push_back(0x40 | 0x80); b.push_back(255); b.push_back(255); b.push_back(255);
    eof(b);
    std::vector<IldaFrame> f;
    CHECK(parse(b, f));
    CHECK(f.size() == 2);
    CHECK(isBlank(f[0].points[0]));
    CHECK(isBlank(f[1].points[0]));
}

TEST(format2_palette_replaces_default_for_its_projector_only) {
    Bytes b;
    header(b, 2, 2, "pal", 1);
    b.push_back(10); b.push_back(20); b.push_back(30);
    b.push_back(40); b.push_back(50); b.push_back(60);
    header(b, 1, 2, "p1", 1);
    put16(b, 0); put16(b, 0); b.push_back(0x00); b.push_back(1);
    put16(b, 0); put16(b, 0); b.push_back(0x80); b.push_back(5);  // outside the 2-color palette
    header(b, 1, 1, "p0", 0);
    put16(b, 0); put16(b, 0); b.push_back(0x80); b.push_back(1);  // default palette, index 1
    eof(b);
    std::vector<IldaFrame> f;
    CHECK(parse(b, f));
    CHECK(f.size() == 2); // the palette itself is not a frame
    CHECK(f[0].projector == 1);
    CHECK_NEAR(f[0].points[0].r, 40.0 / 255.0, 1e-6);
    CHECK_NEAR(f[0].points[0].b, 60.0 / 255.0, 1e-6);
    CHECK(isBlank(f[0].points[1]));          // unknown index -> dark, never a guessed color
    CHECK_NEAR(f[1].points[0].r, 1.0, 1e-6); // default palette 1 = 255,16,0
    CHECK_NEAR(f[1].points[0].g, 16.0 / 255.0, 1e-6);
}

TEST(malformed_files_are_rejected) {
    std::vector<IldaFrame> f;
    Bytes good; header(good, 1, 1); put16(good, 0); put16(good, 0); good.push_back(0x80); good.push_back(0); eof(good);
    CHECK(parse(good, f) && f.size() == 1);

    Bytes noEof(good.begin(), good.end() - 32);
    CHECK(!parse(noEof, f)); CHECK(f.empty());

    Bytes truncated(good.begin(), good.begin() + 36); // header says 1 record, only 4 of 6 bytes there
    CHECK(!parse(truncated, f));

    Bytes badMagic = good; badMagic[0] = 'X';
    CHECK(!parse(badMagic, f));

    Bytes badFormat = good; badFormat[7] = 3; // format 3 was never approved
    CHECK(!parse(badFormat, f));

    Bytes emptyPalette; header(emptyPalette, 2, 0); eof(emptyPalette);
    CHECK(!parse(emptyPalette, f));

    CHECK(!parse(Bytes{}, f));
}

// ── The official ILDA test patterns ─────────────────────────────────────────
TEST(embedded_patterns_are_the_official_files_byte_for_byte) {
    const Bytes f12 = readFile(backendDir() + "/test_patterns/TEST_12K.ILD");
    const Bytes f30 = readFile(backendDir() + "/test_patterns/TEST_30K.ILD");
    CHECK(f12.size() == TEST_12K_ILD_len);
    CHECK(f30.size() == TEST_30K_ILD_len);
    CHECK(f12 == Bytes(TEST_12K_ILD, TEST_12K_ILD + TEST_12K_ILD_len));
    CHECK(f30 == Bytes(TEST_30K_ILD, TEST_30K_ILD + TEST_30K_ILD_len));
}

TEST(official_patterns_parse_completely) {
    struct Expect { const char* id; const char* name; std::size_t points, blanked; float kpps; };
    for (const Expect& e : {Expect{"ilda_12k", "ILDA 12K", 1191, 467, 12.0f},
                            Expect{"ilda_30k", "ILDA 30K", 1415, 500, 30.0f}}) {
        const TestPatternInfo* info = find_test_pattern(e.id);
        CHECK(info != nullptr);
        if (!info) continue;
        CHECK_NEAR(info->kpps, e.kpps, 0.0);
        std::vector<IldaFrame> frames;
        CHECK(parse_ilda(info->data, info->size, frames));
        CHECK(frames.size() == 1);
        CHECK(frames[0].name == e.name);
        const auto& pts = test_pattern_points(e.id);
        CHECK(pts.size() == e.points);
        std::size_t blanked = 0;
        float minX = 2, maxX = -2, minY = 2, maxY = -2;
        for (const auto& p : pts) {
            blanked += isBlank(p);
            minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
            minY = std::min(minY, p.y); maxY = std::max(maxY, p.y);
        }
        CHECK(blanked == e.blanked);
        CHECK(minX >= -1.0f && maxX <= 1.0f && minY >= -1.0f && maxY <= 1.0f);
        CHECK(maxX > 0.99f && minX < -0.99f); // both use the full scan area
    }
    CHECK(test_pattern_points("nope").empty());
    CHECK(find_test_pattern("off") == nullptr);
}

TEST(output_scales_size_and_brightness_without_touching_timing) {
    std::vector<libera::core::LaserPoint> pts = test_pattern_points("ilda_30k");
    const auto orig = pts;
    apply_test_pattern_output(pts, 0.5f, 0.25f);
    CHECK(pts.size() == orig.size());
    for (std::size_t i = 0; i < pts.size(); ++i) {
        CHECK_NEAR(pts[i].x, orig[i].x * 0.5, 1e-6);
        CHECK_NEAR(pts[i].y, orig[i].y * 0.5, 1e-6);
        CHECK_NEAR(pts[i].g, orig[i].g * 0.25, 1e-6);
    }
    pts = orig;
    apply_test_pattern_output(pts, 0.0f, 2.0f); // clamped: min size, full brightness
    CHECK_NEAR(pts[0].x, orig[0].x * kTestPatternMinSize, 1e-6);
    CHECK_NEAR(pts[0].r, orig[0].r, 1e-6);
}

TEST(rate_must_be_within_one_percent) {
    CHECK(test_pattern_rate_ok(30.0f, 30.0f));
    CHECK(test_pattern_rate_ok(30.29f, 30.0f));
    CHECK(!test_pattern_rate_ok(30.4f, 30.0f));
    CHECK(!test_pattern_rate_ok(29.0f, 30.0f));
    CHECK(test_pattern_rate_ok(12.1f, 12.0f));
    CHECK(!test_pattern_rate_ok(30.0f, 12.0f));
    CHECK(!test_pattern_rate_ok(0.0f, 0.0f));
}

int main() {
    std::printf("ilda_file tests\n");
    for (const auto& t : registry()) {
        g_current = t.name;
        const int before = g_failures;
        t.fn();
        std::printf("  %s %s\n", g_failures == before ? "ok  " : "FAIL", t.name);
    }
    std::printf("%zu tests, %d checks, %d failed\n", registry().size(), g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
