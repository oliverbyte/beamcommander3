#pragma once
// Test patterns for setting up the projectors: the official ILDA Standard
// Test Pattern in its 12K and 30K versions, played from ILDA's own files
// (backend/test_patterns/*.ILD, embedded via ilda_test_pattern_data.h) -
// nothing redrawn or approximated. While one is active, every *armed* laser
// outputs it instead of the show (laser_thread() in laser_daemon.cpp); the
// selection is runtime-only and never saved, so a restart always comes back
// to the show.
//
// Per the ILDA Test Pattern tuning procedure (Rev. 002, October 1995) the
// pattern only works at exactly its point rate: "Your image source must
// output points at 12,000 or 30,000 points per second (+/- 1%)". The daemon
// therefore switches the DACs to that rate while the pattern runs and keeps
// a laser dark until its DAC actually runs at it (test_pattern_rate_ok()).
// The procedure also says to scan it small (30K: about 8 degrees optical or
// less, 12K: up to about 15), hence the size factor.

#include "ilda_file.h"
#include "ilda_test_pattern_data.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

struct TestPatternInfo {
    const char* id;          // API/UI id
    const char* label;
    float kpps;              // required point rate, kpps
    const unsigned char* data;
    unsigned int size;
};

inline const std::vector<TestPatternInfo>& test_pattern_catalog() {
    static const std::vector<TestPatternInfo> c = {
        {"ilda_12k", "ILDA 12K", 12.0f, TEST_12K_ILD, TEST_12K_ILD_len},
        {"ilda_30k", "ILDA 30K", 30.0f, TEST_30K_ILD, TEST_30K_ILD_len},
    };
    return c;
}

inline const TestPatternInfo* find_test_pattern(const std::string& id) {
    for (const auto& t : test_pattern_catalog()) if (id == t.id) return &t;
    return nullptr;
}

// The pattern's points exactly as stored in the file (first frame), parsed
// once. Empty for an unknown id (or, impossible for the embedded files, a
// parse error) - callers then output nothing.
inline const std::vector<libera::core::LaserPoint>& test_pattern_points(const std::string& id) {
    static const std::vector<std::vector<libera::core::LaserPoint>> parsed = [] {
        std::vector<std::vector<libera::core::LaserPoint>> out;
        for (const auto& t : test_pattern_catalog()) {
            std::vector<IldaFrame> frames;
            if (parse_ilda(t.data, t.size, frames) && !frames.empty()) out.push_back(frames.front().points);
            else out.emplace_back();
        }
        return out;
    }();
    static const std::vector<libera::core::LaserPoint> none;
    const auto& c = test_pattern_catalog();
    for (std::size_t i = 0; i < c.size(); ++i) if (id == c[i].id) return parsed[i];
    return none;
}

inline constexpr float kTestPatternMinSize = 0.05f;

// Signal size (scaled around the center - the timing of the points stays
// untouched) and master brightness. Blackout, footswitch, mirror, zone and
// safety masks follow in finalize_hardware_frame(), exactly as for the show.
inline void apply_test_pattern_output(std::vector<libera::core::LaserPoint>& pts, float size, float intensity) {
    const float s = std::clamp(size, kTestPatternMinSize, 1.0f);
    const float k = std::clamp(intensity, 0.0f, 1.0f);
    for (auto& p : pts) { p.x *= s; p.y *= s; p.r *= k; p.g *= k; p.b *= k; }
}

// ILDA: the rate must be within +-1% of the pattern's rate.
inline bool test_pattern_rate_ok(float actualKpps, float requiredKpps) {
    return requiredKpps > 0.0f && std::fabs(actualKpps - requiredKpps) <= requiredKpps * 0.01f;
}
