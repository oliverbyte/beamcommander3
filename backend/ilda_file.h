#pragma once
// Reader for ILDA Image Data Transfer Format files (.ild), following the
// official ILDA specification "ILDA Image Data Transfer Format",
// Revision 011 (November 2014, https://www.ilda.com/technical.htm):
//  - every section = 32-byte header ("ILDA", format code, names, record
//    count, ...) + records, big endian; a frame header with 0 records ends
//    the file
//  - formats 0/1 (3D/2D, indexed color), 2 (color palette), 4/5 (3D/2D,
//    true color) - the spec requires a reader to support all five
//  - indexed frames without a preceding format 2 palette use the spec's
//    suggested default palette (Appendix A) - "used by most ILDA files that
//    do not contain a color palette, including the ILDA test pattern"
//  - the blanking bit takes precedence over any color
// Coordinates are mapped 1:1 to the daemon's -1..1 output space (y up, like
// ILDA). Points are never resampled or reordered: ILDA data are samples that
// go straight to the scanners (spec 2.4), and the ILDA test pattern depends
// on exactly that. Header-only and libera-light (only LaserPoint), so
// backend/tests/test_ilda_file.cpp tests exactly the code the daemon runs.

#include "libera/core/LaserPoint.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct IldaFrame {
    std::string name;     // frame name from the header (trailing spaces/NULs trimmed)
    std::string company;  // company name from the header
    int projector = 0;
    std::vector<libera::core::LaserPoint> points;
};

using IldaPalette = std::vector<std::array<std::uint8_t, 3>>;

// Appendix A, "Suggested Default Color Palette" (64 colors, by LFI and Aura
// Technologies).
inline const IldaPalette& ilda_default_palette() {
    static const IldaPalette pal = {
        {255,0,0},{255,16,0},{255,32,0},{255,48,0},{255,64,0},{255,80,0},{255,96,0},{255,112,0},
        {255,128,0},{255,144,0},{255,160,0},{255,176,0},{255,192,0},{255,208,0},{255,224,0},{255,240,0},
        {255,255,0},{224,255,0},{192,255,0},{160,255,0},{128,255,0},{96,255,0},{64,255,0},{32,255,0},
        {0,255,0},{0,255,36},{0,255,73},{0,255,109},{0,255,146},{0,255,182},{0,255,219},{0,255,255},
        {0,227,255},{0,198,255},{0,170,255},{0,142,255},{0,113,255},{0,85,255},{0,56,255},{0,28,255},
        {0,0,255},{32,0,255},{64,0,255},{96,0,255},{128,0,255},{160,0,255},{192,0,255},{224,0,255},
        {255,0,255},{255,32,255},{255,64,255},{255,96,255},{255,128,255},{255,160,255},{255,192,255},{255,224,255},
        {255,255,255},{255,224,224},{255,192,192},{255,160,160},{255,128,128},{255,96,96},{255,64,64},{255,32,32},
    };
    return pal;
}

namespace ilda_detail {
inline std::uint16_t u16(const std::uint8_t* p) { return (std::uint16_t)((p[0] << 8) | p[1]); }
inline std::int16_t  s16(const std::uint8_t* p) { return (std::int16_t)u16(p); }
// ILDA's -32768..32767 to -1..1 (the asymmetric end clamped to -1).
inline float coord(const std::uint8_t* p) {
    const float v = (float)s16(p) / 32767.0f;
    return v < -1.0f ? -1.0f : v;
}
inline std::string text8(const std::uint8_t* p) {
    std::string s;
    for (int i = 0; i < 8 && p[i] != 0; ++i) s.push_back((char)p[i]);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}
} // namespace ilda_detail

// Parses a complete ILDA file. Returns false (frames cleared) on anything
// malformed: wrong magic, unknown format code, a section running past the
// end of the data, or no end-of-file header. Palettes are tracked per
// projector number (spec 3.4). An indexed color outside the active palette
// is drawn blanked rather than in a guessed color.
inline bool parse_ilda(const std::uint8_t* data, std::size_t size, std::vector<IldaFrame>& frames) {
    using namespace ilda_detail;
    frames.clear();
    std::vector<IldaPalette> palettes(256, ilda_default_palette());
    std::size_t off = 0;
    while (true) {
        if (off + 32 > size) { frames.clear(); return false; }
        const std::uint8_t* h = data + off;
        if (h[0] != 'I' || h[1] != 'L' || h[2] != 'D' || h[3] != 'A') { frames.clear(); return false; }
        const int format = h[7];
        const std::size_t records = u16(h + 24);
        const int projector = h[30];
        std::size_t recSize = 0;
        switch (format) {
            case 0: recSize = 8; break;
            case 1: recSize = 6; break;
            case 2: recSize = 3; break;
            case 4: recSize = 10; break;
            case 5: recSize = 8; break;
            default: frames.clear(); return false;
        }
        off += 32;
        if (records == 0) {
            if (format == 2) { frames.clear(); return false; } // palettes need 2..256 colors
            return true; // end-of-file header
        }
        if (off + records * recSize > size) { frames.clear(); return false; }
        const std::uint8_t* r = data + off;
        if (format == 2) {
            IldaPalette pal(records);
            for (std::size_t i = 0; i < records; ++i) pal[i] = {r[i * 3], r[i * 3 + 1], r[i * 3 + 2]};
            palettes[projector] = std::move(pal);
        } else {
            IldaFrame f;
            f.name = text8(h + 8);
            f.company = text8(h + 16);
            f.projector = projector;
            f.points.reserve(records);
            const IldaPalette& pal = palettes[projector];
            for (std::size_t i = 0; i < records; ++i) {
                const std::uint8_t* p = r + i * recSize;
                const bool is3d = format == 0 || format == 4;
                const std::uint8_t* st = p + (is3d ? 6 : 4); // status code byte
                libera::core::LaserPoint q{};
                q.x = coord(p);
                q.y = coord(p + 2);
                std::uint8_t cr = 0, cg = 0, cb = 0;
                if (format == 0 || format == 1) {
                    const std::size_t idx = st[1];
                    if (idx < pal.size()) { cr = pal[idx][0]; cg = pal[idx][1]; cb = pal[idx][2]; }
                } else { // 4/5: blue, green, red after the status byte
                    cb = st[1]; cg = st[2]; cr = st[3];
                }
                if (st[0] & 0x40) cr = cg = cb = 0; // blanking bit wins (spec 5.2.4)
                q.r = cr / 255.0f; q.g = cg / 255.0f; q.b = cb / 255.0f;
                q.i = 1.0f;
                f.points.push_back(q);
            }
            frames.push_back(std::move(f));
        }
        off += records * recSize;
    }
}
