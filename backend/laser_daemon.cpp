// BeamCommander3 — C++ laser backend + HTTP/WebSocket server
//   Full feature parity with BeamCommander (Python edition):
//   position, scale, rotation, movement, wave, rainbow, blackout, flash
//
// Build: cd backend && bash build.sh
// Run:   ./laser_daemon [--port 8000]

#include "libera.h"
#include "libera/etherdream/EtherDreamControllerInfo.hpp"
#define CPPHTTPLIB_WEBSOCKET_SUPPORT
#include "httplib.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>

// Used only to locate the running executable's own directory (to seed a
// per-user data file from a same-named default shipped alongside the
// binary on first run) - see exe_dir() below.
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace std::chrono_literals;
using namespace libera;

// ── Persistent per-user data files (midi_map.json, cues.json, ...) ─────────
// These used to be read/written via plain relative paths, which only work
// if the daemon happens to be launched with its cwd set to backend/ - a
// real bug (start.sh once launched it from the repo root instead, which
// silently disabled MIDI mapping and cue persistence with no obvious
// error). Instead, always resolve them to a per-OS, per-user app-data
// directory, completely independent of cwd and of where the app/binary
// itself lives (so moving, reinstalling, or launching it differently never
// loses saved cues or the MIDI map again):
//   macOS:   ~/Library/Application Support/BeamCommander3/
//   Windows: %APPDATA%\BeamCommander3\
//   Linux:   $XDG_CONFIG_HOME/BeamCommander3/ (or ~/.config/BeamCommander3/)

// Directory containing the running executable itself (NOT cwd) - used only
// to find a same-named "seed" default (e.g. the midi_map.json shipped next
// to the binary in the repo/release package) to copy from on first run.
static std::string exe_dir() {
#if defined(__APPLE__)
    char buf[4096]; uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) return ".";
    std::string p(buf);
#elif defined(_WIN32)
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0) return ".";
    std::string p(buf, n);
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return ".";
    buf[n] = 0;
    std::string p(buf);
#endif
    auto pos = p.find_last_of("/\\");
    return pos == std::string::npos ? "." : p.substr(0, pos);
}

static std::string app_data_dir() {
    std::string dir;
#if defined(__APPLE__)
    const char* home = std::getenv("HOME");
    dir = std::string(home ? home : ".") + "/Library/Application Support/BeamCommander3";
#elif defined(_WIN32)
    const char* appdata = std::getenv("APPDATA");
    dir = std::string(appdata ? appdata : ".") + "/BeamCommander3";
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    dir = xdg ? (std::string(xdg) + "/BeamCommander3")
              : (std::string(home ? home : ".") + "/.config/BeamCommander3");
#endif
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// Resolves the persistent path for a data file: always
// <app_data_dir>/<filename>. If it doesn't exist there yet, seeds it by
// copying a same-named default file shipped alongside the app, if one
// exists - otherwise the caller just creates it fresh on first save (e.g.
// cues.json, which has no meaningful default). Checks two candidate seed
// locations: right next to the executable (dev builds, Windows release
// package - laser_daemon.exe and midi_map.json sit side by side), and a
// sibling Resources/ directory (the macOS .app bundle layout, where
// Contents/MacOS/laser_daemon and Contents/Resources/midi_map.json are
// NOT in the same directory).
static std::string resolve_data_file(const std::string& filename) {
    namespace fs = std::filesystem;
    std::string target = app_data_dir() + "/" + filename;
    if (!fs::exists(target)) {
        std::error_code ec;
        for (const std::string& seed : {exe_dir() + "/" + filename,
                                         exe_dir() + "/../Resources/" + filename}) {
            if (fs::exists(seed)) {
                fs::copy_file(seed, target, ec);
                if (!ec) { std::cout << "[data] seeded " << filename << " from " << seed << "\n"; break; }
            }
        }
    }
    return target;
}

// ── Laser state — full BeamCommander feature set ───────────────────────────────
struct LaserState {
    // Controller
    std::string target_ip;
    float rate_kpps       = 30.0f;
    // Upper bound (and initial default value, above) for rate_kpps - a
    // venue/hardware setting ("how fast can this scanner safely run"), not
    // a per-look show parameter, so - like target_ip/flash_release_ms - it's
    // deliberately excluded from cue_state_to_json/cue_state_from_json and
    // preserved across cue recalls (do_cue_recall()) and /api/reset. The
    // scan-rate fader/REST/MIDI input is clamped to this value everywhere
    // it's applied, so turning it down here also lowers the fader's max.
    float max_rate_kpps   = 30.0f;

    // Shape
    std::string shape     = "circle";
    float radius          = 0.7f;     // normalised 0..1
    int   points          = 300;
    float shape_scale     = 0.0f;     // -1..1 (0 = no extra scale)

    // Color (0..1)
    float r = 0.0f, g = 1.0f, b = 0.314f;
    // Master brightness. Like target_ip and flash_release_ms below, this is
    // a *global* daemon setting, not part of the per-cue show state - it's
    // deliberately excluded from cue_state_to_json/cue_state_from_json and
    // explicitly preserved across cue recalls in do_cue_recall(), so
    // switching cues (manually or via MIDI) never changes the fader's
    // current position - the operator's live brightness setting always
    // stays valid regardless of which cue/look is showing.
    float intensity       = 1.0f;     // master brightness
    // Footswitch brightness-gate release fade time (ms): 0 = instant cut
    // to dark when the gate closes, >0 = fade from 1.0 down to 0 over this
    // many ms instead (see g_gate_level). Defaults to 200ms. This is a
    // *global* daemon setting, not part of the per-cue show state - like
    // target_ip, it's deliberately excluded from cue_state_to_json/
    // cue_state_from_json and explicitly preserved across cue recalls in
    // do_cue_recall() and across /api/reset, so switching cues or
    // resetting the show never changes it.
    float flash_release_ms = 200.0f;    // 0..2000

    // Position -1..1
    float pos_x = 0.0f, pos_y = 0.0f;

    // Rotation
    float rotation_speed  = 0.0f;     // rotations/sec
    // Current rotation angle (radians), used *only* as cue save/recall
    // payload - see do_cue_save/do_cue_recall. Not part of the live
    // /api/state contract (deliberately excluded from state_to_json and
    // the /api/state bulk update), since the true live angle lives in the
    // G_rotation_phase global and changes every frame; this field just
    // carries that value into and out of a saved cue.
    double rotation_phase = 0.0;

    // Mirror: flips the final output x-axis around center when true.
    // Matches the original BeamCommander's invertX/holdInvertX (there just
    // one bool here since nothing else drives a separate persistent toggle
    // yet - a hold button sets this true on press, false on release).
    bool  mirror_x        = false;

    // Movement
    std::string move_mode = "none";   // none circle pan tilt eight random
    float move_speed      = 0.30f;    // cycles/sec
    float move_size       = 0.50f;    // 0..6

    // Wave shape params
    float wave_frequency  = 1.0f;     // cycles across width
    float wave_amplitude  = 0.45f;    // fraction of radius
    float wave_speed      = 0.0f;     // cycles/sec (animated wave)

    // Rainbow
    float rainbow_amount  = 0.0f;     // 0..1 blend
    float rainbow_speed   = 0.0f;     // hue cycles/sec

    // FX
    // Blackout is a global daemon safety setting, not part of the per-cue
    // show state - like intensity/master brightness, it's deliberately
    // excluded from cue_state_to_json/cue_state_from_json and preserved
    // across cue recalls (do_cue_recall()) and /api/reset, so recalling or
    // resetting a look can never silently un-blank the laser (or blank it)
    // behind the operator's back. Defaults to true (safety: a fresh daemon
    // start / first install always comes up blacked out, master brightness
    // at full 100% - see `intensity` above - so the very first thing an
    // operator does is a deliberate, visible "un-blackout" action rather
    // than the beam silently firing at startup).
    bool  blackout        = true;
    float dot_amount      = 1.0f;     // 0..1 fraction of points shown
    float flicker_hz      = 0.0f;     // strobe frequency

    // Zone: a master pan/zoom applied to the *entire* final output, on top
    // of everything else (shape/position/rotation/movement/mirror) - lets
    // the whole show be mapped onto a sub-region of this laser's full
    // physical range (e.g. to fit a wall/truss segment). Horizontal and
    // vertical scale are independent axes (zone_scale_x/zone_scale_y), not
    // a single uniform scale, so the zone can be stretched/squeezed on just
    // one axis - matches the frontend's drag-to-move/drag-edge-to-resize
    // zone box (see ZoningPanel.vue), which is the only way this is meant
    // to be set (no numeric fader). Hardware-only, exactly like
    // `blackout`/the footswitch gate above - it reshapes only the copy of
    // the frame actually sent to the real controller (laser_thread()'s
    // hardware-send step), never the browser preview (make_frame(), used
    // for the WS broadcast), so the preview always shows the true,
    // un-zoned show regardless of how the physical output is currently
    // mapped. Exactly one zone exists right now ("Zone 1", permanently
    // auto-assigned to "Laser 1") since only a single laser output is
    // supported - this becomes a real per-laser list once multi-laser
    // output exists. It's a physical venue
    // calibration, not a per-cue show look, so - like target_ip - it's
    // deliberately excluded from cue_state_to_json/cue_state_from_json and
    // preserved across cue recalls (do_cue_recall()) and /api/reset; it has
    // its own persistence (zones.json) and REST endpoint (/api/zone),
    // loaded once at startup independently of the rest of this struct.
    float zone_x          = 0.0f;     // -1..1, offset of the whole output
    float zone_y          = 0.0f;     // -1..1
    float zone_scale_x    = 1.0f;     // 0.1..2, independent horizontal multiplier
    float zone_scale_y    = 1.0f;     // 0.1..2, independent vertical multiplier

    // Preview-only
    int   persistence_ms  = 25;
};

static LaserState        G;
static std::mutex        G_mtx;
static std::atomic<bool> G_armed{false};
static std::atomic<bool> G_running{true};

// Runtime animation clock (seconds since start, never locked — only laser thread writes)
static double G_time = 0.0;

// Phase accumulators for animated parameters (move/rotation/wave/rainbow).
// These integrate `speed * dt` every tick instead of computing `speed *
// G_time` fresh each frame - the latter makes the phase jump discontinuously
// the instant a speed changes (since G_time has already accumulated a large
// elapsed value, multiplying it by a *new* speed lands on a completely
// different point in the cycle). Integrating incrementally means changing a
// speed only changes the rate of change from that moment on - the current
// position/rotation/wave/hue is preserved and the animation stays smooth.
// Never locked - only laser_thread writes and reads these (single-threaded),
// *except* G_rotation_phase: cue recall also needs to set it (so recalling a
// cue restores the exact rotation angle it was saved at, not just the speed/
// direction), so it's atomic to make that cross-thread write safe.
static double             G_move_phase     = 0.0; // radians
static std::atomic<double> G_rotation_phase{0.0}; // radians
static double             G_wave_phase     = 0.0; // radians
static double             G_rainbow_phase  = 0.0; // cycles (0..1, wraps via fmod)

// Exponential position smoothing (ported from the original BeamCommander's
// ofApp.cpp `smoothOne`): pos_x/pos_y are treated as a *target*, and the
// value actually used for rendering slews toward it with an adaptive time
// constant - finer/slower blending for tiny moves (hides MIDI's 7-bit/
// 128-step resolution as visible stepping) and a boost for big jumps so
// large moves still feel responsive instead of sluggish. Never locked -
// only laser_thread writes and reads these (single-threaded).
static float G_pos_x_smooth = 0.0f;
static float G_pos_y_smooth = 0.0f;

// Same treatment for the wave shape's three MIDI-driven knobs (frequency,
// amplitude, speed): each is a *target*, and the smoothed value below is
// what's actually used for rendering / phase integration, so a coarse
// 7-bit MIDI knob glides instead of visibly stepping - same as pos_x/pos_y.
static float G_wave_frequency_smooth = 1.0f;
static float G_wave_amplitude_smooth = 0.45f;
static float G_wave_speed_smooth     = 0.0f;

// Whether a momentary white flash (REST /flash, MIDI note/CC "flash") is
// currently held. Declared up here (ahead of its do_flash_press/release
// definitions further down) so make_frame() can suppress the rainbow
// effect while flashing - otherwise a flash would just get immediately
// overwritten by the rainbow's full-saturation color whenever
// rainbow_amount > 0. Atomic because make_frame() reads it from
// laser_thread without holding G_mtx, while do_flash_press/release write
// it from whichever thread (HTTP or MIDI) triggered the flash, under
// G_mtx.
static std::atomic<bool> g_flash_active{false};

// Master-brightness gate for a footswitch wired as a "hold to show light"
// pedal (CC64, the standard MIDI sustain-pedal number): while held/open,
// hardware output renders normally; while released/closed (including at
// startup, before the pedal has ever been touched), *hardware* output is
// forced dark. This is hardware-only, exactly like `blackout` below - the
// browser preview always renders at full s.intensity regardless of the
// gate (see laser_thread()'s hardware-send step, which applies g_gate_level
// only to the copy sent to the DAC), so recalling a cue or just opening the
// UI after a restart shows the correct preview immediately, without first
// having to touch the pedal or the brightness fader. Deliberately does NOT
// touch LaserState.intensity itself - the fader/knob/UI/REST setting it is
// LTP (last value counts) and keeps updating independently of the gate, so
// whatever the fader was last set to (even while the gate was closed)
// takes effect immediately the next time the pedal is pressed. LTP goes
// both ways: touching the intensity fader directly (MIDI CC, REST
// /api/state, /laser/brightness) also opens the gate immediately, so
// brightness shows right away even if the pedal was never pressed - the
// footswitch only needs to be used at all if you actually want it to be
// able to force things dark on release. Read from laser_thread without
// holding G_mtx (same reasoning as g_flash_active).
static std::atomic<bool> g_brightness_gate_open{false};

// Fade multiplier driven by the gate's *closing* transition, reusing
// flash_release_ms as a shared "how fast should brightness fall to 0"
// knob: opening the gate snaps this to 1.0 instantly (matching the
// original design - pressing the pedal shows light right away), but
// closing it starts a decay from 1.0 to 0.0 over flash_release_ms instead
// of an instant cut (0ms = instant, same as before this existed). Applied
// as a multiplier only to the hardware-bound frame copy in laser_thread()
// (never to the preview, and never to LaserState.intensity itself, so the
// fader's LTP value is untouched by how quickly the gate happens to be
// closing). g_prev_gate_open detects the open->closed edge (the atomic
// bool alone carries no timestamp). Not atomic themselves, but always
// touched under G_mtx - both by laser_thread's per-tick update and by
// do_flash_press() forcing the gate open (see its comment).
static bool                                  g_prev_gate_open = false;
static bool                                  g_gate_decaying = false;
static std::chrono::steady_clock::time_point g_gate_decay_start;
static float                                 g_gate_level = 0.0f;

static float smooth_toward(float cur, float target, float dt, float tau) {
    dt = std::clamp(dt, 0.0f, 0.25f); // clamp hitches, matches the original
    float alpha = 1.0f - std::exp(-dt / std::max(0.0001f, tau));
    float dist = std::fabs(target - cur);
    if (dist < 0.02f) {
        float t = std::clamp(dist / 0.02f, 0.0f, 1.0f);
        alpha *= 0.15f + t * 0.85f; // map(dist, 0, 0.02, 0.15, 1.0, clamped)
    }
    if (dist > 0.25f) {
        float dc = std::clamp(dist, 0.25f, 2.0f);
        float t = (dc - 0.25f) / (2.0f - 0.25f);
        float boost = 1.0f + t * 2.0f; // map(dist, 0.25, 2.0, 1.0, 3.0, clamped)
        alpha = 1.0f - std::pow(1.0f - alpha, boost);
    }
    return cur + (target - cur) * alpha;
}

// ── Shape generators ───────────────────────────────────────────────────────────
struct Pt { float x, y; };

static std::vector<Pt> gen_base_shape(const LaserState& s) {
    const float pi  = std::acos(-1.0f);
    const float tau = 2.0f * pi;
    const int   n   = std::max(8, s.points);
    const float rad = std::max(0.01f, s.radius);

    // Scale factor: map shape_scale [-1..1] -> geometric scale
    float sf = s.shape_scale >= 0 ? (1.0f + s.shape_scale * 2.0f) : (1.0f + s.shape_scale * 0.7f);
    const float r = rad * sf;

    std::vector<Pt> pts;
    pts.reserve(n);

    if (s.shape == "circle") {
        for (int i = 0; i < n; ++i) { float a = (float)i/n*tau; pts.push_back({r*std::cos(a), r*std::sin(a)}); }
    } else if (s.shape == "line") {
        for (int i = 0; i < n; ++i) { float t = (float)i/(n-1)*2.0f-1.0f; pts.push_back({t*r, 0.0f}); }
    } else if (s.shape == "triangle") {
        float vx[3]={0.0f,-r,r}, vy[3]={r,-r*0.577f,-r*0.577f};
        int third=n/3;
        for (int e=0;e<3;++e) { int nx=(e+1)%3;
            for (int i=0;i<third;++i) { float t=(float)i/third;
                pts.push_back({vx[e]+t*(vx[nx]-vx[e]),vy[e]+t*(vy[nx]-vy[e])}); } }
    } else if (s.shape == "square") {
        float cx[4]={-r,r,r,-r},cy[4]={-r,-r,r,r};
        int side=n/4;
        for (int e=0;e<4;++e) { int nx=(e+1)%4;
            for (int i=0;i<side;++i) { float t=(float)i/side;
                pts.push_back({cx[e]+t*(cx[nx]-cx[e]),cy[e]+t*(cy[nx]-cy[e])}); } }
    } else if (s.shape == "wave") {
        float phase = (float)G_wave_phase;
        for (int i=0;i<n;++i) {
            float t=(float)i/(n-1)*tau;
            pts.push_back({(t/tau*2.0f-1.0f)*r, r*G_wave_amplitude_smooth*std::sin(G_wave_frequency_smooth*t+phase)});
        }
    } else if (s.shape == "staticwave") {
        for (int i=0;i<n;++i) {
            float t=(float)i/(n-1)*tau;
            pts.push_back({(t/tau*2.0f-1.0f)*r, r*G_wave_amplitude_smooth*std::sin(G_wave_frequency_smooth*t)});
        }
    } else {
        for (int i=0;i<n;++i) pts.push_back({0.0f,0.0f});
    }
    return pts;
}

static void rotate_pts(std::vector<Pt>& pts, float angle) {
    float c=std::cos(angle), sv=std::sin(angle);
    for (auto& p : pts) { float nx=p.x*c-p.y*sv; p.y=p.x*sv+p.y*c; p.x=nx; }
}

static Pt calc_movement(const LaserState& s) {
    if (s.move_mode == "none") return {0.0f,0.0f};
    float phase = (float)G_move_phase;
    float amp   = s.move_size * 0.6f; // fraction of canvas
    if (s.move_mode=="circle")  return {amp*std::cos(phase), amp*std::sin(phase)};
    if (s.move_mode=="pan")     return {amp*std::sin(phase), 0.0f};
    if (s.move_mode=="tilt")    return {0.0f, amp*std::sin(phase)};
    if (s.move_mode=="eight")   return {amp*std::sin(phase), amp*std::sin(2*phase)};
    if (s.move_mode=="random")  return {amp*(std::sin(phase*1.3f)+std::sin(phase*2.7f))/2,
                                        amp*(std::sin(phase*1.7f)+std::sin(phase*3.1f))/2};
    return {0.0f,0.0f};
}

// Simple HSV->RGB (h 0..1)
static void hsv_to_rgb(float h, float s, float v, float& r, float& g, float& b) {
    if (s == 0) { r=g=b=v; return; }
    int i = (int)(h*6);
    float f=h*6-i; float p=v*(1-s),q=v*(1-f*s),t=v*(1-(1-f)*s);
    switch (i%6) {
        case 0:r=v;g=t;b=p;break; case 1:r=q;g=v;b=p;break;
        case 2:r=p;g=v;b=t;break; case 3:r=p;g=q;b=v;break;
        case 4:r=t;g=p;b=v;break; default:r=v;g=p;b=q;break;
    }
}

static core::Frame make_frame(const LaserState& s) {
    auto base = gen_base_shape(s);

    // Rotation - always applied at the current phase, even when
    // rotation_speed is currently 0 (e.g. motion_hold is pressed, or the
    // speed knob itself is at 0). Previously this was gated on
    // `rotation_speed != 0.0f`, so the instant speed hit 0 the shape
    // snapped back to its unrotated base orientation instead of holding
    // whatever angle it had reached - G_rotation_phase itself was already
    // frozen correctly (integrates by speed*dt, so 0 speed = no further
    // change), it just wasn't being drawn. Rotating by phase 0 is a no-op,
    // so applying it unconditionally is always safe.
    rotate_pts(base, (float)G_rotation_phase);

    // Position + movement. Uses the smoothed position (see G_pos_x_smooth
    // above), not the raw target s.pos_x/s.pos_y directly, so manual moves
    // - especially MIDI, whose 7-bit knobs would otherwise visibly step -
    // glide instead of jumping.
    Pt mv = calc_movement(s);
    float ox = G_pos_x_smooth + mv.x;
    float oy = G_pos_y_smooth + mv.y;

    // Mirror is applied last (matches the original app's transform order:
    // scale/rotate/translate, *then* mirror), so it flips the fully
    // transformed output around the canvas center (x=0), not just the base
    // shape before it's positioned.
    float mirror_sign = s.mirror_x ? -1.0f : 1.0f;

    // Dot amount
    int n0 = (int)base.size();
    // Number of dots to keep, spaced *evenly* around the full point set
    // (not by an integer stride - see numDots loop below). Rounding to the
    // nearest whole dot count means the actual kept fraction can differ
    // very slightly from dot_amount, which is inaudible/invisible in
    // practice and far preferable to the alternative (a fixed integer
    // stride leaves an uneven remainder in the last gap - see next
    // comment).
    int numDots = n0;
    if (s.dot_amount < 1.0f && s.dot_amount > 0.0f)
        numDots = std::clamp((int)std::lround(s.dot_amount * n0), 1, n0);
    bool dotted = numDots < n0;

    // A DAC (Ether Dream / Helios) plays a frame's points back-to-back at a
    // fixed point rate, moving the galvo in a straight line between
    // consecutive buffer entries - it does NOT teleport, and the mirror
    // itself has real inertia/settling time. Just thinning the point list
    // (the old fixed-integer-stride subsampling alone) kept every kept
    // point fully lit, so the beam travelled *lit* from one kept point
    // straight to the next, redrawing the original solid outline instead
    // of separate dots. A fixed integer stride also picked kept indices
    // as 0, step, 2*step, ... up to the largest multiple of step below n -
    // whenever step didn't evenly divide n (the overwhelmingly common
    // case), the final wrap-around gap (last kept dot back to index 0) was
    // shorter than every other gap, so on a circle the dots visibly
    // bunched up unevenly instead of sitting at equal angular spacing all
    // the way around. Picking `numDots` indices by evenly dividing the
    // full point count in floating point (below) spreads the rounding
    // error out over every gap instead of dumping it all into one, so
    // consecutive dots - including the wrap-around pair - end up the same
    // distance apart.
    // A *fixed* number of blanked settle points wasn't enough either: a
    // small jump (dense dots, e.g. neighbouring points on a tight circle)
    // settles almost instantly, but a large jump (sparse dots - worst case
    // the "line" shape at low dot_amount, where only a couple of widely
    // spaced points survive, sometimes clear across the whole canvas)
    // needs far more settle time than that - with a fixed small constant,
    // those big jumps still showed up as short streaks/dashes right where
    // the beam was still catching up when it lit back up. Fix: scale the
    // number of blanked settle points by how far this dot actually is from
    // the previous one (in normalised -1..1 space), so nearby dots settle
    // fast and far-apart dots get proportionally longer to actually stop
    // moving before the beam turns back on.
    const float kBlankPerUnitDistance = 24.0f; // blanked points per unit of jump distance
    const int   kBlankMin  = 4;    // floor, even for a zero-distance jump
    const int   kBlankMax  = 80;   // ceiling, so a full-canvas jump can't stall the frame
    const int   kDotDwell  = 4;    // lit points held once settled
    const int   kBlankTrail = 2;   // blanked points held before leaving

    // Flicker/strobe (still affects the generated frame directly, so both
    // preview and hardware strobe together - only `blackout` is treated
    // differently, see laser_thread()'s hardware-send step below, which
    // blanks a *copy* of the frame for hardware only, leaving this one -
    // used for the WS preview broadcast - showing the true colors).
    bool frame_blank = false;
    if (s.flicker_hz > 0.0f) {
        double period = 1.0 / s.flicker_hz;
        double phase  = std::fmod(G_time, period);
        frame_blank = (phase > period * 0.5);
    }

    // Rainbow phase
    float rainbow_phase = (float)std::fmod(G_rainbow_phase, 1.0);

    core::Frame frame;
    int n = n0;
    frame.points.reserve(dotted ? numDots * (kBlankMin + kDotDwell + kBlankTrail + 8) : n);

    // The DAC loops the frame continuously, so the "incoming" jump into the
    // very first kept dot is really the wrap-around travel from the *last*
    // kept dot back to it - seed prev{x,y} with that last dot's position
    // (rather than e.g. 0,0) so that wrap-around segment gets the correct
    // settle time too, not an arbitrary one based on a fake start point.
    float prevx = 0.0f, prevy = 0.0f;
    if (dotted) {
        int lastKept = (int)((double)(numDots - 1) * n / numDots + 0.5) % n;
        prevx = mirror_sign * (base[lastKept].x + ox);
        prevy = base[lastKept].y + oy;
    }

    for (int d = 0; d < numDots; ++d) {
        int i = dotted ? (int)((double)d * n / numDots + 0.5) % n : d;
        float fr = s.r, fg = s.g, fb = s.b;

        // Spatial rainbow, ported from the original BeamCommander's
        // colorForX(): once rainbow_amount > 0, points get a *full*
        // saturated HSV rainbow color (fully replacing the selected beam
        // color, not a washed-out lerp blend with it) - amount only
        // controls how many color bands repeat across the shape (0 =
        // dense rainbow stripes, 1 = the whole shape is a single solid
        // hue), while rainbow_speed animates that hue over time via
        // rainbow_phase. Skipped while a white flash is held so flash
        // always wins - matches the original app's whiteFlash check,
        // which short-circuits colorForX the same way.
        if (s.rainbow_amount > 0.0f && !g_flash_active) {
            float x_norm = n > 1 ? (float)i / (float)(n - 1) : 0.0f;
            float cycles_across = (1.0f - s.rainbow_amount) * 24.0f;
            float hue = std::fmod(rainbow_phase + x_norm * cycles_across, 1.0f);
            if (hue < 0.0f) hue += 1.0f;
            hsv_to_rgb(hue, 1.0f, 1.0f, fr, fg, fb);
        }

        // Master brightness. The footswitch gate (g_gate_level) is
        // deliberately *not* applied here - it's hardware-only, applied to
        // a separate copy of the frame in laser_thread()'s hardware-send
        // step (same treatment as `blackout`), so the WS preview always
        // shows the true, ungated brightness. Zone 1 (see zone_x/zone_y/
        // zone_scale_x/zone_scale_y's comment) gets the exact same
        // hardware-only treatment, for the exact same reason - applied in
        // laser_thread(), never here.
        float px = mirror_sign * (base[i].x + ox);
        float py = base[i].y + oy;
        // Screen-edge safety: whatever we send downstream, the DAC encoder
        // still clamps each axis independently to the hardware's physical
        // +-1 range at the final output boundary (see LaserPoint.hpp's
        // sanitizer comment) - we can't change that (it's in libera, not
        // here). Left alone, that clamp pins any out-of-range point to the
        // nearest edge and keeps it lit, so a shape that's scaled/moved
        // past the safe range gets flattened against the screen boundary
        // instead of clipped away - worst case, an oversized circle ends
        // up with almost its whole outline pinned to all four edges,
        // degenerating into a plain rectangle. Blank (not clamp) instead:
        // fully blank any point that would land outside +-1 on either
        // axis, so the figure simply fades out before reaching the edge
        // rather than being disturbed/squashed against it.
        bool offScreen = std::fabs(px) > 1.0f || std::fabs(py) > 1.0f;
        float bri = (frame_blank || offScreen) ? 0.0f : s.intensity;

        if (dotted) {
            // Arrive blanked and hold for a distance-scaled number of
            // settle points (see kBlankPerUnitDistance comment above), then
            // hold lit for kDotDwell, then blank again for kBlankTrail
            // before the beam is allowed to move off.
            float dx = px - prevx, dy = py - prevy;
            float dist = std::sqrt(dx * dx + dy * dy);
            int blankSettle = std::clamp((int)std::round(dist * kBlankPerUnitDistance) + kBlankMin,
                                          kBlankMin, kBlankMax);
            for (int d = 0; d < blankSettle; ++d)
                frame.points.emplace_back(core::LaserPoint{px, py, 0.0f, 0.0f, 0.0f, 1.0f});
            for (int d = 0; d < kDotDwell; ++d) {
                frame.points.emplace_back(core::LaserPoint{
                    px, py, fr * bri, fg * bri, fb * bri, 1.0f
                });
            }
            for (int d = 0; d < kBlankTrail; ++d)
                frame.points.emplace_back(core::LaserPoint{px, py, 0.0f, 0.0f, 0.0f, 1.0f});
            prevx = px; prevy = py;
        } else {
            frame.points.emplace_back(core::LaserPoint{
                px, py, fr * bri, fg * bri, fb * bri, 1.0f
            });
        }
    }
    return frame;
}

// ── JSON helpers ───────────────────────────────────────────────────────────────
static std::string frame_to_ws_json(const core::Frame& fr) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(4) << "{\"pts\":[";
    for (std::size_t i=0;i<fr.points.size();++i) {
        if (i) ss<<',';
        const auto& p=fr.points[i];
        ss<<'['<<p.x<<','<<p.y<<','<<p.r<<','<<p.g<<','<<p.b<<']';
    }
    ss << "]}"; return ss.str();
}

static std::string state_to_json() {
    std::lock_guard<std::mutex> lk(G_mtx);
    std::ostringstream ss; ss << std::fixed << std::setprecision(4);
    ss << "{"
       << "\"shape\":\""       << G.shape         << "\","
       << "\"radius\":"        << G.radius         << ","
       << "\"points\":"        << G.points         << ","
       << "\"rate_kpps\":"     << G.rate_kpps      << ","
       << "\"max_rate_kpps\":" << G.max_rate_kpps  << ","
       << "\"intensity\":"     << G.intensity      << ","
       << "\"flash_release_ms\":" << G.flash_release_ms << ","
       << "\"r\":"             << G.r              << ","
       << "\"g\":"             << G.g              << ","
       << "\"b\":"             << G.b              << ","
       << "\"shape_scale\":"   << G.shape_scale    << ","
       << "\"pos_x\":"         << G.pos_x          << ","
       << "\"pos_y\":"         << G.pos_y          << ","
       << "\"rotation_speed\":" << G.rotation_speed << ","
       << "\"mirror_x\":"      << (G.mirror_x?"true":"false") << ","
       << "\"move_mode\":\""   << G.move_mode      << "\","
       << "\"move_speed\":"    << G.move_speed     << ","
       << "\"move_size\":"     << G.move_size      << ","
       << "\"wave_frequency\":" << G.wave_frequency << ","
       << "\"wave_amplitude\":" << G.wave_amplitude << ","
       << "\"wave_speed\":"    << G.wave_speed     << ","
       << "\"rainbow_amount\":" << G.rainbow_amount << ","
       << "\"rainbow_speed\":" << G.rainbow_speed  << ","
       << "\"blackout\":"      << (G.blackout?"true":"false") << ","
       << "\"dot_amount\":"    << G.dot_amount     << ","
       << "\"flicker_hz\":"    << G.flicker_hz     << ","
       << "\"brightness_gate_open\":" << (g_brightness_gate_open.load()?"true":"false") << ","
       << "\"armed\":"         << (G_armed.load()?"true":"false") << ","
       << "\"ip\":\""          << G.target_ip      << "\""
       << "}";
    return ss.str();
}

// ── WebSocket broadcast ────────────────────────────────────────────────────────
static std::mutex                         G_ws_mtx;
static std::set<httplib::ws::WebSocket*>  G_ws_clients;
static void ws_broadcast(const std::string& msg) {
    std::lock_guard<std::mutex> lk(G_ws_mtx);
    for (auto* ws : G_ws_clients) ws->send(msg);
}

// ── Signal ─────────────────────────────────────────────────────────────────────
static void on_signal(int) { G_running = false; }

// ── Laser output thread ────────────────────────────────────────────────────────
// Preview (WebSocket) generation is fully decoupled from the hardware
// connection: the browser preview must keep responding to every parameter
// change whether or not a real controller is connected/armed.
static void laser_thread() {
    System sys;
    core::LaserController::setMaxFrameHoldTime(std::chrono::milliseconds(0));

    using Clock = std::chrono::steady_clock;
    auto last_preview = Clock::now() - 1s;
    auto last_tick    = Clock::now();
    // Paces hardware sendFrame() calls (see hardwareReady below) - without
    // this, the loop would call sendFrame() every time
    // ctrl->isReadyForNewFrame() allows (which can be much more often than
    // a frame's actual physical draw time of points/rate_kpps), enqueueing
    // new frames faster than the hardware can drain them. That builds an
    // ever-growing backlog/delay on the real laser output while the
    // preview (throttled separately to 33ms via last_preview/previewDue,
    // and not subject to any hardware queue) stays near-instant - exactly
    // the "preview has zero delay, real output has massive delay" bug this
    // fixes.
    auto last_hw_send = Clock::now() - 1s;

    std::shared_ptr<core::LaserController> ctrl;
    std::string connected_ip;
    float       connected_rate = 0;
    auto        next_connect_attempt = Clock::now();

    while (G_running) {
        // Advance animation clock
        auto now_tp = Clock::now();
        double dt = std::chrono::duration<double>(now_tp - last_tick).count();
        G_time += dt;
        last_tick = now_tp;

        std::string ip; float rate;
        {
            std::lock_guard<std::mutex> lk(G_mtx);
            ip   = G.target_ip;
            rate = G.rate_kpps;

            // Integrate each animated parameter's phase by its *current*
            // speed - see the G_move_phase/etc. comment above for why this
            // (rather than speed*G_time) is what keeps things smooth across
            // a speed change instead of jumping/resetting.
            const double tau = 2.0 * std::acos(-1.0);
            G_move_phase     = std::fmod(G_move_phase     + G.move_speed     * dt * tau, tau);
            // Negated here (not at every rotation_speed setter) so the
            // reversed direction applies no matter which API set the speed
            // (REST bulk update, /laser/rotation/speed/<v>, or MIDI).
            G_rotation_phase = std::fmod(G_rotation_phase - G.rotation_speed * dt * tau, tau);
            // Uses the *smoothed* wave speed (updated below) rather than
            // G.wave_speed directly, so a MIDI knob jump changes the wave's
            // animation rate gradually instead of snapping to a new speed
            // instantly - matching the same glide the value knobs get.
            G_wave_phase     = std::fmod(G_wave_phase     + G_wave_speed_smooth * dt * tau, tau);
            G_rainbow_phase  = std::fmod(G_rainbow_phase  + G.rainbow_speed  * dt, 1.0);

            // Slew the rendered position toward the latest pos_x/pos_y
            // target (see smooth_toward's comment above).
            G_pos_x_smooth = smooth_toward(G_pos_x_smooth, G.pos_x, (float)dt, 0.090f);
            G_pos_y_smooth = smooth_toward(G_pos_y_smooth, G.pos_y, (float)dt, 0.090f);

            // Same glide for the wave knobs (see G_wave_frequency_smooth's
            // comment above) - identical tau to pos_x/pos_y per the request
            // that these transition "as smooth as x/y changes".
            G_wave_frequency_smooth = smooth_toward(G_wave_frequency_smooth, G.wave_frequency, (float)dt, 0.090f);
            G_wave_amplitude_smooth = smooth_toward(G_wave_amplitude_smooth, G.wave_amplitude, (float)dt, 0.090f);
            G_wave_speed_smooth     = smooth_toward(G_wave_speed_smooth,     G.wave_speed,     (float)dt, 0.090f);

            // Brightness gate fade (see g_gate_level's comment): opening
            // the gate snaps g_gate_level to 1.0 instantly; closing it
            // starts a decay to 0.0 over flash_release_ms - the footswitch
            // is the *only* thing this knob affects; flash's own release
            // is always instant (see do_flash_release()).
            bool gate_open_now = g_brightness_gate_open.load();
            if (gate_open_now) {
                g_gate_level = 1.0f;
                g_gate_decaying = false;
            } else if (g_prev_gate_open) {
                // Just closed this tick.
                if (G.flash_release_ms > 0.0f) {
                    g_gate_decay_start = now_tp;
                    g_gate_decaying = true;
                } else {
                    g_gate_level = 0.0f;
                    g_gate_decaying = false;
                }
            }
            if (g_gate_decaying) {
                double elapsed_ms = std::chrono::duration<double, std::milli>(now_tp - g_gate_decay_start).count();
                double dur_ms = std::max(1.0f, G.flash_release_ms);
                if (elapsed_ms >= dur_ms) {
                    g_gate_level = 0.0f;
                    g_gate_decaying = false;
                } else {
                    g_gate_level = 1.0f - (float)(elapsed_ms / dur_ms);
                }
            }
            g_prev_gate_open = gate_open_now;
        }

        // Tear down the hardware connection if disarmed/disconnected, but
        // keep running the loop so the preview keeps animating.
        if (ip.empty() || !G_armed.load()) {
            if (ctrl) { ctrl->setArmed(false); ctrl.reset(); connected_ip.clear(); std::cout << "[laser] disarmed\n" << std::flush; }
        } else if ((!ctrl || connected_ip != ip || std::abs(rate-connected_rate) > 0.1f) && now_tp >= next_connect_attempt) {
            if (ctrl) { ctrl->setArmed(false); ctrl.reset(); }
            etherdream::EtherDreamControllerInfo info(ip,"Ether Dream",ip,7765,4095);
            ctrl = sys.connectController(info);
            if (!ctrl) {
                std::cout << "[laser] connect failed " << ip << "\n" << std::flush;
                next_connect_attempt = now_tp + 1s; // back off before retrying
            } else {
                ctrl->setPointRate(static_cast<std::uint32_t>(rate * 1000));
                ctrl->setArmed(true);
                connected_ip = ip; connected_rate = rate;
                std::cout << "[laser] streaming " << ip << " @ " << rate << " kpps\n" << std::flush;
            }
        } else if (ctrl && connected_ip == ip) {
            // Live rate update on an already-connected controller
            std::lock_guard<std::mutex> lk(G_mtx);
            if (std::abs(G.rate_kpps-connected_rate) > 0.1f) {
                ctrl->setPointRate(static_cast<std::uint32_t>(G.rate_kpps*1000));
                connected_rate = G.rate_kpps;
            }
        }


        bool hardwareReady = ctrl && ctrl->isReadyForNewFrame() && (now_tp - last_hw_send) >= 20ms;
        bool previewDue     = (now_tp - last_preview) >= 33ms;

        if (!hardwareReady && !previewDue) {
            std::this_thread::sleep_for(1ms);
            continue;
        }

        LaserState snap; { std::lock_guard<std::mutex> lk(G_mtx); snap = G; }
        auto frame = make_frame(snap);

        if (previewDue) {
            last_preview = now_tp;
            ws_broadcast(frame_to_ws_json(frame));
        }

        if (hardwareReady) {
            last_hw_send = now_tp;
            // Zone 1 (laser 1): a hardware-only master pan/zoom, never
            // applied to the browser preview (which already got the true,
            // unzoned `frame` above) - see zone_x/zone_y/zone_scale_x/
            // zone_scale_y's comment. Only a real controller output should
            // be reshaped to fit the venue's physical sub-region; the
            // preview always shows the full, un-zoned show. Horizontal and
            // vertical scale are independent, so the zone can be
            // stretched/squeezed on just one axis. Unlike the
            // blackout/gate step below (color-only), this rewrites each
            // point's actual x/y - and, same reasoning as make_frame()'s
            // own off-screen handling, blanks (not clamps) anything the
            // zone pushes outside the DAC's +-1 range instead of letting
            // it pin/smear against the edge.
            core::Frame hwFrame = frame;
            if (snap.zone_scale_x != 1.0f || snap.zone_scale_y != 1.0f || snap.zone_x != 0.0f || snap.zone_y != 0.0f) {
                for (auto& p : hwFrame.points) {
                    float zx = p.x * snap.zone_scale_x + snap.zone_x;
                    float zy = p.y * snap.zone_scale_y + snap.zone_y;
                    p.x = zx; p.y = zy;
                    if (std::fabs(zx) > 1.0f || std::fabs(zy) > 1.0f) { p.r = 0.0f; p.g = 0.0f; p.b = 0.0f; }
                }
            }
            // Blackout and the footswitch brightness gate only ever
            // suppress/dim the *real* laser output, never the browser
            // preview (which already got the true, full-brightness `frame`
            // above) - so a modified copy is what actually goes to
            // hardware, keeping the same point positions (safety-relevant
            // for some controllers) but scaling/zeroing every channel's
            // color. Blackout wins outright (full zero); otherwise the
            // gate's current fade level (1.0 = fully open, decaying to 0.0
            // while closed/closing - see g_gate_level's comment) is applied
            // as a multiplier.
            if (snap.blackout) {
                core::Frame blanked = hwFrame;
                for (auto& p : blanked.points) { p.r = 0.0f; p.g = 0.0f; p.b = 0.0f; }
                ctrl->sendFrame(std::move(blanked));
            } else if (g_gate_level < 1.0f) {
                core::Frame gated = hwFrame;
                for (auto& p : gated.points) {
                    p.r *= g_gate_level; p.g *= g_gate_level; p.b *= g_gate_level;
                }
                ctrl->sendFrame(std::move(gated));
            } else {
                ctrl->sendFrame(std::move(hwFrame));
            }
        }
    }
    if (ctrl) ctrl->setArmed(false);
    sys.shutdown();
}


// ── JSON parsers ───────────────────────────────────────────────────────────────
static std::string json_str(const std::string& b, const std::string& k) {
    auto p=b.find("\""+k+"\""); if(p==std::string::npos) return "";
    p=b.find(':',p); if(p==std::string::npos) return "";
    p=b.find('"',p+1); if(p==std::string::npos) return "";
    auto e=b.find('"',p+1); if(e==std::string::npos) return "";
    return b.substr(p+1,e-p-1);
}
static float json_float(const std::string& b,const std::string& k,float d) {
    auto p=b.find("\""+k+"\""); if(p==std::string::npos) return d;
    p=b.find(':',p); if(p==std::string::npos) return d;
    ++p; while(p<b.size()&&b[p]==' ')++p;
    try{return std::stof(b.substr(p));}catch(...){return d;}
}
static bool json_bool(const std::string& b,const std::string& k,bool d) {
    auto p=b.find("\""+k+"\""); if(p==std::string::npos) return d;
    p=b.find(':',p); if(p==std::string::npos) return d;
    ++p; while(p<b.size()&&b[p]==' ')++p;
    if(p<b.size()&&b.substr(p,4)=="true") return true;
    if(p<b.size()&&b.substr(p,5)=="false") return false;
    return d;
}
static int json_int(const std::string& b,const std::string& k,int d){ return (int)json_float(b,k,(float)d); }

// ── Zone persistence ─────────────────────────────────────────────────────────
// "Zone 1" (see LaserState::zone_x/zone_y/zone_scale_x/zone_scale_y) is a
// physical venue calibration, not a show look, so it lives in its own
// small file (zones.json) rather than the general show state - which
// currently isn't persisted at all - so a projector alignment survives a
// daemon restart.
static const std::string ZONES_FILE = resolve_data_file("zones.json");

static void save_zone_to_disk() {
    std::ostringstream ss; ss << std::fixed << std::setprecision(4);
    float zx, zy, zsx, zsy;
    { std::lock_guard<std::mutex> lk(G_mtx); zx = G.zone_x; zy = G.zone_y; zsx = G.zone_scale_x; zsy = G.zone_scale_y; }
    ss << "{\"laser_id\":1,\"x\":" << zx << ",\"y\":" << zy << ",\"scale_x\":" << zsx << ",\"scale_y\":" << zsy << "}";
    std::ofstream f(ZONES_FILE, std::ios::trunc);
    if (f) f << ss.str();
}

// Seeds zones.json with the compiled-in defaults on first run (no file
// yet), otherwise loads the persisted zone 1 values into G.
static void load_zone_from_disk() {
    std::ifstream f(ZONES_FILE);
    if (!f) { save_zone_to_disk(); return; }
    std::ostringstream buf; buf << f.rdbuf();
    std::string body = buf.str();
    std::lock_guard<std::mutex> lk(G_mtx);
    G.zone_x       = json_float(body, "x", G.zone_x);
    G.zone_y       = json_float(body, "y", G.zone_y);
    G.zone_scale_x = json_float(body, "scale_x", G.zone_scale_x);
    G.zone_scale_y = json_float(body, "scale_y", G.zone_scale_y);
    std::cout << "[laser_daemon] Loaded zone 1 from " << ZONES_FILE << "\n";
}

static std::string zone_to_json() {
    std::lock_guard<std::mutex> lk(G_mtx);
    std::ostringstream ss; ss << std::fixed << std::setprecision(4);
    ss << "{\"laser_id\":1,\"x\":" << G.zone_x << ",\"y\":" << G.zone_y
       << ",\"scale_x\":" << G.zone_scale_x << ",\"scale_y\":" << G.zone_scale_y << "}";
    return ss.str();
}

// ── Cue save/recall ──────────────────────────────────────────────────────────
// A cue is a saved snapshot of every show parameter (shape, color, transform,
// movement, wave, rainbow, FX) — NOT the controller connection (target_ip),
// so recalling a cue never drops/changes the current DAC link. Persisted to
// a small JSON file on disk so cues survive a daemon restart, mirroring the
// cue system from the original BeamCommander (Python edition).
static constexpr int MAX_CUES = 32;
static std::map<int, LaserState> G_cues;
static std::mutex                G_cues_mtx;
static const std::string         CUES_FILE = resolve_data_file("cues.json");

static std::string cue_state_to_json(const LaserState& s) {
    std::ostringstream ss; ss << std::fixed << std::setprecision(4);
    ss << "{"
       << "\"shape\":\""       << s.shape         << "\","
       << "\"radius\":"        << s.radius        << ","
       << "\"points\":"        << s.points        << ","
       << "\"rate_kpps\":"     << s.rate_kpps     << ","
       << "\"r\":"             << s.r             << ","
       << "\"g\":"             << s.g             << ","
       << "\"b\":"             << s.b             << ","
       << "\"shape_scale\":"   << s.shape_scale   << ","
       << "\"pos_x\":"         << s.pos_x         << ","
       << "\"pos_y\":"         << s.pos_y         << ","
       << "\"rotation_speed\":" << s.rotation_speed << ","
       << "\"rotation_phase\":" << s.rotation_phase << ","
       << "\"mirror_x\":"      << (s.mirror_x?"true":"false") << ","
       << "\"move_mode\":\""   << s.move_mode     << "\","
       << "\"move_speed\":"    << s.move_speed    << ","
       << "\"move_size\":"     << s.move_size     << ","
       << "\"wave_frequency\":" << s.wave_frequency << ","
       << "\"wave_amplitude\":" << s.wave_amplitude << ","
       << "\"wave_speed\":"    << s.wave_speed    << ","
       << "\"rainbow_amount\":" << s.rainbow_amount << ","
       << "\"rainbow_speed\":" << s.rainbow_speed << ","
       << "\"dot_amount\":"    << s.dot_amount    << ","
       << "\"flicker_hz\":"    << s.flicker_hz
       << "}";
    return ss.str();
}

static LaserState cue_state_from_json(const std::string& obj) {
    LaserState s{};
    auto str = json_str(obj,"shape");     if(!str.empty()) s.shape = str;
    s.radius          = json_float(obj,"radius",s.radius);
    s.points          = json_int(obj,"points",s.points);
    s.rate_kpps       = json_float(obj,"rate_kpps",s.rate_kpps);
    s.r               = json_float(obj,"r",s.r);
    s.g               = json_float(obj,"g",s.g);
    s.b               = json_float(obj,"b",s.b);
    s.shape_scale     = json_float(obj,"shape_scale",s.shape_scale);
    s.pos_x           = json_float(obj,"pos_x",s.pos_x);
    s.pos_y           = json_float(obj,"pos_y",s.pos_y);
    s.rotation_speed  = json_float(obj,"rotation_speed",s.rotation_speed);
    s.rotation_phase  = json_float(obj,"rotation_phase",(float)s.rotation_phase);
    s.mirror_x        = json_bool(obj,"mirror_x",s.mirror_x);
    auto mv = json_str(obj,"move_mode");  if(!mv.empty()) s.move_mode = mv;
    s.move_speed      = json_float(obj,"move_speed",s.move_speed);
    s.move_size       = json_float(obj,"move_size",s.move_size);
    s.wave_frequency  = json_float(obj,"wave_frequency",s.wave_frequency);
    s.wave_amplitude  = json_float(obj,"wave_amplitude",s.wave_amplitude);
    s.wave_speed      = json_float(obj,"wave_speed",s.wave_speed);
    s.rainbow_amount  = json_float(obj,"rainbow_amount",s.rainbow_amount);
    s.rainbow_speed   = json_float(obj,"rainbow_speed",s.rainbow_speed);
    s.dot_amount      = json_float(obj,"dot_amount",s.dot_amount);
    s.flicker_hz      = json_float(obj,"flicker_hz",s.flicker_hz);
    return s;
}

static void save_cues_to_disk() {
    std::lock_guard<std::mutex> lk(G_cues_mtx);
    std::ofstream f(CUES_FILE, std::ios::trunc);
    if (!f) return;
    f << "{";
    bool first = true;
    for (auto& [num, s] : G_cues) {
        if (!first) f << ",";
        first = false;
        f << "\"" << num << "\":" << cue_state_to_json(s);
    }
    f << "}";
}

// Minimal brace-matching split of a flat {"1":{...},"2":{...}} file - this is
// our own file format (produced only by save_cues_to_disk above), so a full
// JSON parser isn't needed, just enough to recover each numbered cue block.
static void load_cues_from_disk() {
    std::ifstream f(CUES_FILE);
    if (!f) return;
    std::ostringstream buf; buf << f.rdbuf();
    std::string body = buf.str();

    std::lock_guard<std::mutex> lk(G_cues_mtx);
    G_cues.clear();
    std::size_t i = 0;
    while (true) {
        auto q1 = body.find('"', i);          if (q1 == std::string::npos) break;
        auto q2 = body.find('"', q1 + 1);     if (q2 == std::string::npos) break;
        std::string key = body.substr(q1 + 1, q2 - q1 - 1);
        auto colon = body.find(':', q2);      if (colon == std::string::npos) break;
        auto brace = body.find('{', colon);   if (brace == std::string::npos) break;
        int depth = 0; std::size_t j = brace;
        for (; j < body.size(); ++j) {
            if (body[j] == '{') ++depth;
            else if (body[j] == '}') { --depth; if (depth == 0) { ++j; break; } }
        }
        if (depth != 0) break; // malformed, bail out
        try {
            int num = std::stoi(key);
            if (num >= 1 && num <= MAX_CUES)
                G_cues[num] = cue_state_from_json(body.substr(brace, j - brace));
        } catch (...) {}
        i = j;
    }
    std::cout << "[laser_daemon] Loaded " << G_cues.size() << " cue(s) from " << CUES_FILE << "\n";
}

// Shared cue operations - used by both the HTTP /api/cue/* routes and the
// MIDI dispatcher below, so there's exactly one place that knows the correct
// (deadlock-safe) lock order: G_mtx and G_cues_mtx are NEVER held at the same
// time by any of these.
static void do_cue_save(int n) {
    if (n < 1 || n > MAX_CUES) return;
    LaserState snap; { std::lock_guard<std::mutex> lk(G_mtx); snap = G; }
    // Rotation angle lives outside LaserState (see G_rotation_phase's
    // comment) - stash the live value into the snapshot's transport field
    // so recalling this cue restores the exact angle, not just the speed.
    snap.rotation_phase = G_rotation_phase.load();
    { std::lock_guard<std::mutex> lk(G_cues_mtx); G_cues[n] = snap; }
    save_cues_to_disk();
}
static bool do_cue_recall(int n) {
    if (n < 1 || n > MAX_CUES) return false;
    bool found = false; LaserState snap;
    { std::lock_guard<std::mutex> lk(G_cues_mtx);
      auto it = G_cues.find(n);
      if (it != G_cues.end()) { snap = it->second; found = true; } }
    if (!found) return false;
    {
        std::lock_guard<std::mutex> lk(G_mtx);
        std::string ip = G.target_ip;
        // flash_release_ms, intensity (master brightness), max_rate_kpps,
        // blackout and the zone 1 calibration are global daemon settings,
        // not part of the per-cue show state - preserve whatever they're
        // currently set to across the recall, same treatment as target_ip.
        float flash_release_ms = G.flash_release_ms;
        float intensity = G.intensity;
        float max_rate_kpps = G.max_rate_kpps;
        bool blackout = G.blackout;
        float zone_x = G.zone_x, zone_y = G.zone_y, zone_scale_x = G.zone_scale_x, zone_scale_y = G.zone_scale_y;
        G = snap;
        G.target_ip = ip;
        G.flash_release_ms = flash_release_ms;
        G.intensity = intensity;
        G.max_rate_kpps = max_rate_kpps;
        G.rate_kpps = std::clamp(G.rate_kpps, 0.0f, G.max_rate_kpps);
        G.blackout = blackout;
        G.zone_x = zone_x; G.zone_y = zone_y; G.zone_scale_x = zone_scale_x; G.zone_scale_y = zone_scale_y;
    }
    // Restore the rotation angle the cue was saved at (see do_cue_save).
    G_rotation_phase.store(snap.rotation_phase);
    return true;
}
static void do_cue_clear(int n) {
    if (n < 1 || n > MAX_CUES) return;
    { std::lock_guard<std::mutex> lk(G_cues_mtx); G_cues.erase(n); }
    save_cues_to_disk();
}
// Moves a saved cue from slot `from` to slot `to`, overwriting whatever was
// in `to` (if anything) and clearing `from`. No-op (returns false) if
// `from` has no saved cue, either number is out of range, or from == to.
static bool do_cue_move(int from, int to) {
    if (from < 1 || from > MAX_CUES || to < 1 || to > MAX_CUES || from == to) return false;
    bool found = false; LaserState snap;
    { std::lock_guard<std::mutex> lk(G_cues_mtx);
      auto it = G_cues.find(from);
      if (it != G_cues.end()) { snap = it->second; found = true; } }
    if (!found) return false;
    { std::lock_guard<std::mutex> lk(G_cues_mtx); G_cues[to] = snap; G_cues.erase(from); }
    save_cues_to_disk();
    return true;
}

// ── MIDI control (optional) ─────────────────────────────────────────────────
// Lets an external MIDI controller (e.g. an Akai APC40 mkII connected via
// USB, like the original BeamCommander) drive exactly the same state that
// the REST API drives - it's just another input source feeding the same
// `G` struct, never a separate code path. Entirely optional: if no MIDI
// hardware is connected, or backend/midi_map.json has no bindings, this
// subsystem simply does nothing.
//
// Because the exact note/CC numbers a given controller sends depend on its
// firmware mode (and can't be verified without the physical unit), bindings
// are loaded from a small JSON file rather than hard-coded: run the daemon,
// press a button/turn a knob, read the "[midi] unmapped ..." log line it
// prints, then add a matching entry to midi_map.json (see README.md for the
// full action-name catalog) and restart the daemon.
struct MidiBinding {
    std::string type;      // "note" or "cc"
    int channel = -1;      // 0-15, or -1 to match any channel
    int number  = -1;      // note number or CC number (0-127)
    std::string action;    // e.g. "r", "shape:circle", "move:pan", "cue:5"

    // Optional per-binding response curve for "cc" bindings (ignored for
    // "note" bindings) - mirrors the original BeamCommander's MIDI mapper
    // (MidiToOscMapper.h) so mappings ported from it behave the same way:
    bool  centered = false; // bipolar knob: raw MIDI 63/64 = exact center -> 0
    float deadzone = 0.0f;  // (centered only) snap to 0 within this distance of center, 0..1
    float gamma    = 1.0f;  // response curve exponent (1 = linear)
    bool  invert   = false;
    float scale    = 1.0f;  // applied before offset, non-centered mode only
    float offset   = 0.0f;  // applied after scale, non-centered mode only
    float outMin   = 0.0f;  // final mapped output range
    float outMax   = 1.0f;
    // Relative/endless encoder support (some APC40 knobs, e.g. the "Cue
    // Level" knob, send two's-complement deltas instead of an absolute
    // position - see midi_cc_value's relative branch for the exact decode).
    bool  relative = false;
    float step     = 0.01f; // normalized 0..1 accumulator step per raw delta unit
};
static std::vector<MidiBinding> G_midi_bindings;
static std::mutex               G_midi_mtx;

static void load_midi_map(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        std::cout << "[midi] no mapping file at " << path << " - MIDI control disabled\n";
        return;
    }
    std::ostringstream buf; buf << f.rdbuf();
    std::string body = buf.str();

    std::lock_guard<std::mutex> lk(G_midi_mtx);
    G_midi_bindings.clear();
    std::size_t i = body.find('{');
    while (i != std::string::npos) {
        int depth = 0; std::size_t j = i;
        for (; j < body.size(); ++j) {
            if (body[j] == '{') ++depth;
            else if (body[j] == '}') { --depth; if (depth == 0) { ++j; break; } }
        }
        if (depth != 0) break;
        std::string obj = body.substr(i, j - i);
        MidiBinding b;
        b.type     = json_str(obj, "type");
        b.channel  = json_int(obj, "channel", -1);
        b.number   = json_int(obj, "number", -1);
        b.action   = json_str(obj, "action");
        b.centered = json_bool(obj, "centered", false);
        b.deadzone = json_float(obj, "deadzone", 0.0f);
        b.gamma    = json_float(obj, "gamma", 1.0f);
        b.invert   = json_bool(obj, "invert", false);
        b.scale    = json_float(obj, "scale", 1.0f);
        b.offset   = json_float(obj, "offset", 0.0f);
        b.outMin   = json_float(obj, "outMin", 0.0f);
        b.outMax   = json_float(obj, "outMax", 1.0f);
        b.relative = json_bool(obj, "relative", false);
        b.step     = json_float(obj, "step", 0.01f);
        if ((b.type == "note" || b.type == "cc") && b.number >= 0 && !b.action.empty())
            G_midi_bindings.push_back(b);
        i = body.find('{', j);
    }
    std::cout << "[midi] loaded " << G_midi_bindings.size() << " binding(s) from " << path << "\n";
}

// Accumulator for relative/endless-encoder CC bindings, keyed by
// (channel<<8 | cc number) - each key holds a normalized 0..1 running
// position that ticks accumulate into (see midi_cc_value's relative
// branch). Only ever touched from midi_thread, so no locking needed.
static std::map<int, float> G_midi_relative_acc;

// Converts a raw 0..127 CC value into the binding's configured output range,
// replicating the original BeamCommander's MidiToOscMapper.h curve engine:
//  - "centered" bipolar knobs treat raw 63/64 as the exact zero point (not
//    just val/127*2-1), so the physical center detent reads as exact 0.
//  - "gamma" applies a response curve (symmetric around center when
//    centered) for a less twitchy/more natural feel on fine controls.
//  - "deadzone" (centered only) snaps small values near center to exactly 0.
//  - non-centered controls use the simpler invert/scale/offset/gamma chain.
//  - "relative" (endless encoders, e.g. the APC40's "Cue Level" knob):
//    raw isn't a position at all, it's a two's-complement tick delta (1..63
//    = turned up that many ticks, 65..127 = turned down (128-raw) ticks) -
//    each tick nudges a persistent normalized accumulator by `step` instead
//    of jumping to an absolute position, exactly like a real endless pot.
static float midi_cc_value(const MidiBinding& b, int raw, int channel) {
    raw = std::clamp(raw, 0, 127);
    if (b.relative) {
        int delta = (raw <= 63) ? raw : (raw - 128); // e.g. 127 -> -1, 126 -> -2
        float d = delta * b.step;
        if (b.invert) d = -d;
        int key = (channel << 8) | (b.number & 0xff);
        float acc = std::clamp(G_midi_relative_acc[key] + d, 0.0f, 1.0f);
        G_midi_relative_acc[key] = acc;
        float norm = acc;
        if (b.gamma > 0.0f && b.gamma != 1.0f) norm = std::pow(norm, b.gamma);
        return b.outMin + norm * (b.outMax - b.outMin);
    }
    if (b.centered) {
        float c = (raw >= 64) ? (float)(raw - 64) / 63.0f : (float)(raw - 63) / 63.0f;
        if (b.invert) c = -c;
        c = std::clamp(c, -1.0f, 1.0f);
        if (b.gamma > 0.0f && b.gamma != 1.0f) {
            float sgn = c >= 0.0f ? 1.0f : -1.0f;
            c = sgn * std::pow(std::fabs(c), b.gamma);
        }
        if (b.deadzone > 0.0f && std::fabs(c) < b.deadzone) c = 0.0f;
        return b.outMin + (c + 1.0f) * 0.5f * (b.outMax - b.outMin);
    }
    float norm = raw / 127.0f;
    if (b.invert) norm = 1.0f - norm;
    norm = std::clamp(norm * b.scale + b.offset, 0.0f, 1.0f);
    if (b.gamma > 0.0f && b.gamma != 1.0f) norm = std::pow(norm, b.gamma);
    return b.outMin + norm * (b.outMax - b.outMin);
}

// Applies one CC-triggered action. `out` is already mapped into the
// binding's configured output range (see midi_cc_value above), so each
// branch just clamps to the field's valid range and assigns - no further
// scaling here.
// (Forward-declared: a footswitch's momentary press/release can arrive as a
// CC rather than a note - e.g. the APC40's 1/4" footswitch jack sends CC64 -
// so "flash" needs to be reachable from CC too, not just notes.)
static void do_flash_press();
static void do_flash_release();

static void midi_apply_cc_action(const std::string& action, float out) {
    // Footswitch-style momentary controls sent as CC (e.g. a sustain-pedal
    // jack sending CC64): treat above/below the midpoint as press/release.
    // do_flash_press/release is idempotent, so it's safe to call on every
    // CC message without edge-detection.
    if (action=="flash") { if (out > 0.5f) do_flash_press(); else do_flash_release(); return; }
    // Footswitch master-brightness gate (CC64): idempotent, just tracks
    // whether the pedal is currently held above/below the midpoint - see
    // g_brightness_gate_open's comment for the full behavior.
    if (action=="brightness_gate") { g_brightness_gate_open.store(out > 0.5f); return; }
    if (action=="r")              { std::lock_guard<std::mutex> lk(G_mtx); G.r=std::clamp(out,0.0f,1.0f); return; }
    if (action=="g")              { std::lock_guard<std::mutex> lk(G_mtx); G.g=std::clamp(out,0.0f,1.0f); return; }
    if (action=="b")              { std::lock_guard<std::mutex> lk(G_mtx); G.b=std::clamp(out,0.0f,1.0f); return; }
    if (action=="intensity")      { std::lock_guard<std::mutex> lk(G_mtx); G.intensity=std::clamp(out,0.0f,1.0f); g_brightness_gate_open.store(true); return; }
    if (action=="flash_release_ms") { std::lock_guard<std::mutex> lk(G_mtx); G.flash_release_ms=std::clamp(out,0.0f,2000.0f); return; }
    if (action=="shape_scale")    { std::lock_guard<std::mutex> lk(G_mtx); G.shape_scale=std::clamp(out,-1.0f,1.0f); return; }
    if (action=="rotation_speed") { std::lock_guard<std::mutex> lk(G_mtx); G.rotation_speed=out; return; }
    if (action=="pos_x")          { std::lock_guard<std::mutex> lk(G_mtx); G.pos_x=std::clamp(out,-1.0f,1.0f); return; }
    if (action=="pos_y")          { std::lock_guard<std::mutex> lk(G_mtx); G.pos_y=std::clamp(out,-1.0f,1.0f); return; }
    if (action=="dot_amount")     { std::lock_guard<std::mutex> lk(G_mtx); G.dot_amount=std::clamp(out,0.0f,1.0f); return; }
    if (action=="flicker_hz")     { std::lock_guard<std::mutex> lk(G_mtx); G.flicker_hz=std::max(0.0f,out); return; }
    if (action=="wave_frequency") { std::lock_guard<std::mutex> lk(G_mtx); G.wave_frequency=std::max(0.1f,out); return; }
    if (action=="wave_amplitude") { std::lock_guard<std::mutex> lk(G_mtx); G.wave_amplitude=std::clamp(out,0.0f,1.0f); return; }
    if (action=="wave_speed")     { std::lock_guard<std::mutex> lk(G_mtx); G.wave_speed=out; return; }
    if (action=="rainbow_amount") { std::lock_guard<std::mutex> lk(G_mtx); G.rainbow_amount=std::clamp(out,0.0f,1.0f); return; }
    if (action=="rainbow_speed")  { std::lock_guard<std::mutex> lk(G_mtx); G.rainbow_speed=out; return; }
    if (action=="move_size")      { std::lock_guard<std::mutex> lk(G_mtx); G.move_size=std::clamp(out,0.0f,6.0f); return; }
    if (action=="move_speed")     { std::lock_guard<std::mutex> lk(G_mtx); G.move_speed=std::max(0.0f,out); return; }
    if (action=="rate_kpps")      { std::lock_guard<std::mutex> lk(G_mtx); G.rate_kpps=std::clamp(out,0.0f,G.max_rate_kpps); return; }
}

// State for the momentary (hold-to-preview) cue buttons and the momentary
// flash button - both need to remember what to restore on release.
static bool      g_midi_cue_save_armed = false;
static LaserState g_cue_momentary_snapshot;
static bool       g_cue_momentary_active = false;
static bool       g_motion_held = false;        // for "motion_hold" (freeze movement while held)
static float      g_pre_motion_speed = 0.0f;
static float      g_pre_motion_rotation_speed = 0.0f;
static float      g_pre_motion_rainbow_speed = 0.0f;
// g_flash_active is declared earlier (near G_pos_x_smooth) so make_frame()
// can see it too.
static float      g_pre_flash_r = 1.0f, g_pre_flash_g = 1.0f, g_pre_flash_b = 1.0f;
static float      g_pre_flash_intensity = 1.0f;

// Shared flash press/release - used by both the MIDI dispatcher and the
// /flash/<0|1> HTTP route, so a flash triggered from either input behaves
// identically: forces color to white *and* full brightness only while
// held, restores the exact prior color and brightness on release.
static void do_flash_press() {
    std::lock_guard<std::mutex> lk(G_mtx);
    if (g_flash_active) return;
    g_pre_flash_r = G.r; g_pre_flash_g = G.g; g_pre_flash_b = G.b;
    g_pre_flash_intensity = G.intensity;
    G.r = G.g = G.b = 1.0f;
    G.intensity = 1.0f;
    // Flash must always be visible regardless of the footswitch gate -
    // force it open (and instantly at full level, bypassing any close
    // decay in progress) so pressing flash works even if the pedal was
    // never touched, or is currently up.
    g_brightness_gate_open.store(true);
    g_gate_level = 1.0f;
    g_gate_decaying = false;
    g_flash_active = true;
}
static void do_flash_release() {
    std::lock_guard<std::mutex> lk(G_mtx);
    if (!g_flash_active) return;
    // Flash's own release is always instant - flash_release_ms only drives
    // the footswitch gate's close-fade (see g_gate_level), not this.
    G.r = g_pre_flash_r; G.g = g_pre_flash_g; G.b = g_pre_flash_b;
    G.intensity = g_pre_flash_intensity;
    g_flash_active = false;
}

// Shared motion_hold press/release - used by both the MIDI dispatcher and
// the /motion/hold/<0|1> HTTP route: stops the movement pattern, rotation,
// *and* the rainbow hue cycle in place while held, resumes all three on
// release.
static void do_motion_hold_press() {
    std::lock_guard<std::mutex> lk(G_mtx);
    if (g_motion_held) return;
    g_pre_motion_speed = G.move_speed; G.move_speed = 0.0f;
    g_pre_motion_rotation_speed = G.rotation_speed; G.rotation_speed = 0.0f;
    g_pre_motion_rainbow_speed = G.rainbow_speed; G.rainbow_speed = 0.0f;
    g_motion_held = true;
}
static void do_motion_hold_release() {
    std::lock_guard<std::mutex> lk(G_mtx);
    if (!g_motion_held) return;
    G.move_speed = g_pre_motion_speed;
    G.rotation_speed = g_pre_motion_rotation_speed;
    G.rainbow_speed = g_pre_motion_rainbow_speed;
    g_motion_held = false;
}

// One-shot rotation reset - used by both the MIDI "rotation_reset" note
// action and the /rotation/reset REST route: snaps the rotation angle back
// to 0 and stops it from spinning further (rotation_speed to 0), rather
// than the momentary hold/restore pattern the other do_* helpers use.
static void do_rotation_reset() {
    G_rotation_phase.store(0.0);
    std::lock_guard<std::mutex> lk(G_mtx);
    G.rotation_speed = 0.0f;
}

// Applies one note-triggered (button) action. `isPress`/`isRelease`
// describe note-on/note-off.
static void midi_apply_note_action(const std::string& action, bool isPress, bool isRelease) {
    if (!isPress && !isRelease) return;

    if (action.rfind("shape:",0)==0) {
        if (!isPress) return;
        static const std::set<std::string> SHAPES{"circle","line","triangle","square","wave","staticwave"};
        std::string s = action.substr(6);
        if (SHAPES.count(s)) { std::lock_guard<std::mutex> lk(G_mtx); G.shape=s; }
        return;
    }
    if (action.rfind("move:",0)==0) {
        if (!isPress) return;
        static const std::set<std::string> MOVES{"none","circle","pan","tilt","eight","random"};
        std::string m = action.substr(5);
        if (MOVES.count(m)) { std::lock_guard<std::mutex> lk(G_mtx); G.move_mode=m; }
        return;
    }
    if (action.rfind("color:",0)==0) {
        if (!isPress) return;
        std::string c = action.substr(6);
        float cr,cg,cb;
        if      (c=="red")     { cr=1.0f; cg=0.0f;  cb=0.0f; }
        else if (c=="orange")  { cr=1.0f; cg=0.4f;  cb=0.0f; }
        else if (c=="yellow")  { cr=1.0f; cg=1.0f;  cb=0.0f; }
        else if (c=="green")   { cr=0.0f; cg=1.0f;  cb=0.0f; }
        else if (c=="cyan")    { cr=0.0f; cg=1.0f;  cb=1.0f; }
        else if (c=="blue")    { cr=0.0f; cg=0.2f;  cb=1.0f; }
        else if (c=="magenta") { cr=1.0f; cg=0.0f;  cb=1.0f; }
        else if (c=="white")   { cr=1.0f; cg=1.0f;  cb=1.0f; }
        else return;
        std::lock_guard<std::mutex> lk(G_mtx);
        G.r=cr; G.g=cg; G.b=cb;
        // A plain color-select button is mutually exclusive with the
        // rainbow preset (matches the original mapping's "colors" exclusive
        // group, which included both) - picking a solid color should always
        // show that color immediately, not stay hidden behind a still-active
        // rainbow blend.
        G.rainbow_amount = 0.0f;
        return;
    }
    if (action=="blackout_toggle") {
        if (!isPress) return;
        std::lock_guard<std::mutex> lk(G_mtx); G.blackout = !G.blackout;
        return;
    }
    // One-shot: snap rotation back to angle 0 and stop it spinning. Shared
    // with the /rotation/reset REST route. Fires on *every* MIDI message
    // for this note (both isPress and isRelease), not just isPress - this
    // particular pad is apparently in a toggle-style LED mode that sends
    // alternating note-on/note-off across successive physical taps rather
    // than an on+off pair per tap, so gating on isPress only (like the
    // other one-shot actions above) made it fire on just every other press.
    // Since do_rotation_reset() is idempotent, reacting to both is safe and
    // makes it behave like a plain momentary touch button regardless of
    // the pad's LED/toggle firmware mode.
    if (action=="rotation_reset") {
        do_rotation_reset();
        return;
    }
    // Preset button from the original mapping (rainbow/preset/slowfull):
    // full rainbow blend at a slow animation speed, one press away.
    if (action=="rainbow_preset_slowfull") {
        if (!isPress) return;
        std::lock_guard<std::mutex> lk(G_mtx); G.rainbow_amount=1.0f; G.rainbow_speed=0.15f;
        return;
    }
    // Momentary movement freeze (motion/hold in the original mapping):
    // stops the movement pattern, rotation, *and* the rainbow hue cycle in
    // place while held, resumes all three on release. Shared with the
    // /motion/hold/<0|1> REST route.
    if (action=="motion_hold") {
        if (isPress) do_motion_hold_press();
        else if (isRelease) do_motion_hold_release();
        return;
    }
    // "Hold" blackout (matches the original BeamCommander's Blackout button):
    // dark while held, resumes on release - safer for a live "oh no" button
    // than a toggle, since letting go always restores the show.
    if (action=="blackout_hold") {
        std::lock_guard<std::mutex> lk(G_mtx);
        if (isPress) G.blackout = true;
        else if (isRelease) G.blackout = false;
        return;
    }
    // Momentary mirror (matches the original BeamCommander's invert/x/hold):
    // flips the output horizontally while held, un-flips on release.
    if (action=="mirror_hold") {
        std::lock_guard<std::mutex> lk(G_mtx);
        if (isPress) G.mirror_x = true;
        else if (isRelease) G.mirror_x = false;
        return;
    }
    if (action=="flash") {
        if (isPress) do_flash_press();
        else if (isRelease) do_flash_release();
        return;
    }
    if (action=="cue_save_arm") {
        if (isPress) { g_midi_cue_save_armed = true; std::cout << "[midi] cue save armed - next cue button saves\n"; }
        return;
    }
    if (action.rfind("cue:",0)==0) {
        if (!isPress) return;
        int n = 0;
        try { n = std::stoi(action.substr(4)); } catch (...) { return; }
        if (g_midi_cue_save_armed) {
            do_cue_save(n);
            g_midi_cue_save_armed = false;
            std::cout << "[midi] saved cue " << n << "\n";
        } else if (do_cue_recall(n)) {
            std::cout << "[midi] recalled cue " << n << "\n";
        } else {
            std::cout << "[midi] cue " << n << " is empty\n";
        }
        return;
    }
    // Momentary cue preview (matches the original BeamCommander's APC40
    // grid buttons): hold to preview the cue, release to snap back to
    // whatever was showing before - lets you audition a look without
    // committing to it.
    if (action.rfind("cue_momentary:",0)==0) {
        int n = 0;
        try { n = std::stoi(action.substr(14)); } catch (...) { return; }
        if (isPress) {
            { std::lock_guard<std::mutex> lk(G_mtx); g_cue_momentary_snapshot = G; }
            g_cue_momentary_active = true;
            if (do_cue_recall(n)) std::cout << "[midi] previewing cue " << n << "\n";
        } else if (isRelease && g_cue_momentary_active) {
            { std::lock_guard<std::mutex> lk(G_mtx);
              std::string ip = G.target_ip; G = g_cue_momentary_snapshot; G.target_ip = ip; }
            g_cue_momentary_active = false;
            std::cout << "[midi] cue " << n << " preview released\n";
        }
        return;
    }
}

#ifdef __APPLE__
#include <CoreMIDI/CoreMIDI.h>

static MIDIClientRef g_midi_client  = 0;
static MIDIPortRef   g_midi_in_port = 0;

static void midi_read_proc(const MIDIPacketList* pktlist, void*, void*) {
    const MIDIPacket* packet = &pktlist->packet[0];
    for (UInt32 i = 0; i < pktlist->numPackets; ++i) {
        if (packet->length >= 2) {
            unsigned char status  = packet->data[0];
            unsigned char type    = status & 0xF0;
            unsigned char channel = status & 0x0F;
            unsigned char d1      = packet->data[1];
            unsigned char d2      = packet->length >= 3 ? packet->data[2] : 0;

            bool isNote = (type == 0x90 || type == 0x80);
            bool isCC   = (type == 0xB0);
            if (isNote || isCC) {
                bool isPress   = isNote && type == 0x90 && d2 > 0;
                bool isRelease = isNote && (type == 0x80 || (type == 0x90 && d2 == 0));

                MidiBinding match; bool found = false;
                {
                    std::lock_guard<std::mutex> lk(G_midi_mtx);
                    for (auto& b : G_midi_bindings) {
                        bool typeOk = (isNote && b.type=="note") || (isCC && b.type=="cc");
                        if (!typeOk || b.number != d1) continue;
                        if (b.channel != -1 && b.channel != channel) continue;
                        match = b; found = true;
                        if (b.channel == channel) break; // exact-channel match wins over wildcard
                    }
                }
                if (found) {
                    if (isNote) {
                        midi_apply_note_action(match.action, isPress, isRelease);
                    } else {
                        midi_apply_cc_action(match.action, midi_cc_value(match, d2, channel));
                    }
                } else {
                    std::cout << "[midi] unmapped " << (isNote ? "note" : "cc")
                              << " ch=" << (int)channel << " num=" << (int)d1
                              << " val=" << (int)d2 << "\n" << std::flush;
                }
            }
        }
        packet = MIDIPacketNext(packet);
    }
}

// Runs on its own thread with an active CFRunLoop (required for CoreMIDI to
// deliver read-proc callbacks). Periodically rescans for newly-connected
// sources so plugging in the controller after the daemon starts still works.
static void midi_thread() {
    load_midi_map(resolve_data_file("midi_map.json"));

    OSStatus st = MIDIClientCreate(CFSTR("BeamCommander3"), nullptr, nullptr, &g_midi_client);
    if (st != noErr) { std::cout << "[midi] MIDIClientCreate failed (" << st << ")\n"; return; }
    st = MIDIInputPortCreate(g_midi_client, CFSTR("BeamCommander3 In"), midi_read_proc, nullptr, &g_midi_in_port);
    if (st != noErr) { std::cout << "[midi] MIDIInputPortCreate failed (" << st << ")\n"; return; }

    std::set<MIDIEndpointRef> connected;
    auto scan = [&]() {
        ItemCount n = MIDIGetNumberOfSources();
        for (ItemCount i = 0; i < n; ++i) {
            MIDIEndpointRef src = MIDIGetSource(i);
            if (connected.count(src)) continue;
            CFStringRef nameRef = nullptr;
            MIDIObjectGetStringProperty(src, kMIDIPropertyDisplayName, &nameRef);
            char name[256] = "?";
            if (nameRef) { CFStringGetCString(nameRef, name, sizeof(name), kCFStringEncodingUTF8); CFRelease(nameRef); }
            if (MIDIPortConnectSource(g_midi_in_port, src, nullptr) == noErr) {
                connected.insert(src);
                std::cout << "[midi] connected: " << name << "\n" << std::flush;
            }
        }
    };
    scan();
    std::cout << "[midi] " << connected.size() << " source(s) connected"
                 " (will keep scanning for hot-plugged devices)\n" << std::flush;

    CFRunLoopTimerRef timer = CFRunLoopTimerCreateWithHandler(
        kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + 3, 3, 0, 0,
        ^(CFRunLoopTimerRef) { scan(); });
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopDefaultMode);

    CFRunLoopRun(); // pumps MIDI + the rescan timer forever
}
#else
static void midi_thread() {
    std::cout << "[midi] CoreMIDI is only implemented on macOS - MIDI control disabled\n";
}
#endif

#define APPLY_F(key, field) { float _v=json_float(req.body,key,-9999); if(_v!=-9999) G.field=_v; }
#define APPLY_S(key, field) { auto _v=json_str(req.body,key); if(!_v.empty()) G.field=_v; }
#define APPLY_B(key, field) { if(req.body.find("\""+std::string(key)+"\"")!=std::string::npos) G.field=json_bool(req.body,key,G.field); }
#define APPLY_CLAMP(key, field, lo, hi) { float _v=json_float(req.body,key,-9999); if(_v!=-9999) G.field=std::clamp(_v,(float)(lo),(float)(hi)); }

// ── main ───────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    int http_port = 8000;
    for (int i=1;i<argc;++i) {
        if (!strcmp(argv[i],"--port")&&i+1<argc) http_port=std::stoi(argv[++i]);
        else if (!strcmp(argv[i],"-h")||!strcmp(argv[i],"--help")){ std::cout<<"Usage: laser_daemon [--port 8000]\n"; return 0; }
    }
    std::signal(SIGINT,on_signal); std::signal(SIGTERM,on_signal);

    // Settings/cues persist here regardless of cwd or where the app is
    // installed - print it so it's obvious (e.g. in start.sh's terminal
    // output) where midi_map.json/cues.json actually live.
    std::cout << "[laser_daemon] User data directory: " << app_data_dir() << "\n";

    load_cues_from_disk();
    load_zone_from_disk();

    std::thread laser_t(laser_thread);
    std::thread midi_t(midi_thread);
    midi_t.detach(); // pumps a CFRunLoop forever; torn down on process exit
    httplib::Server svr;

    svr.set_default_headers({{"Access-Control-Allow-Origin","*"},{"Access-Control-Allow-Methods","GET,POST,OPTIONS"},{"Access-Control-Allow-Headers","Content-Type"}});
    svr.Options(".*",[](const httplib::Request&,httplib::Response& r){r.status=204;});

    // ── Serve the built frontend (frontend/dist), if present ───────────────────
    // Lets a packaged release run standalone (just start laser_daemon and open
    // the browser) without needing `npm run dev` from source. In normal dev
    // usage the Vite dev server handles the frontend instead and this
    // directory won't exist, so it's a harmless no-op then. Override the
    // path via the FRONTEND_DIST env var (e.g. if packaged differently).
    {
        const char* envDir = std::getenv("FRONTEND_DIST");
        std::string wwwDir = envDir ? envDir : "./frontend_dist";
        if (std::filesystem::is_directory(wwwDir)) {
            svr.set_mount_point("/", wwwDir);
            std::cout << "[laser_daemon] Serving frontend from " << wwwDir << "\n";
        }
    }

    // ── GET /api/state ──────────────────────────────────────────────────────────
    svr.Get("/api/state",[](const httplib::Request&,httplib::Response& res){
        res.set_content(state_to_json(),"application/json");
    });

    // ── GET /api/zone — "Zone 1", auto-assigned to "Laser 1" ───────────────────
    svr.Get("/api/zone",[](const httplib::Request&,httplib::Response& res){
        res.set_content(zone_to_json(),"application/json");
    });

    // ── POST /api/zone — move/scale zone 1 (x/y move, independent scale_x/scale_y) ─
    svr.Post("/api/zone",[](const httplib::Request& req,httplib::Response& res){
        { std::lock_guard<std::mutex> lk(G_mtx);
          APPLY_F("x",zone_x) APPLY_CLAMP("x",zone_x,-1,1)
          APPLY_F("y",zone_y) APPLY_CLAMP("y",zone_y,-1,1)
          APPLY_F("scale_x",zone_scale_x) APPLY_CLAMP("scale_x",zone_scale_x,0.1,2)
          APPLY_F("scale_y",zone_scale_y) APPLY_CLAMP("scale_y",zone_scale_y,0.1,2) }
        save_zone_to_disk();
        res.set_content(zone_to_json(),"application/json");
    });

    // ── POST /api/state — bulk update ──────────────────────────────────────────
    svr.Post("/api/state",[](const httplib::Request& req,httplib::Response& res){
        { // scope the lock so it's released before state_to_json() re-locks G_mtx
        std::lock_guard<std::mutex> lk(G_mtx);
        APPLY_S("shape",shape)  APPLY_S("ip",target_ip)  APPLY_S("move_mode",move_mode)
        APPLY_F("radius",radius)  APPLY_CLAMP("radius",radius,0,1)
        APPLY_F("max_rate_kpps",max_rate_kpps)  APPLY_CLAMP("max_rate_kpps",max_rate_kpps,1,100)
        APPLY_F("rate_kpps",rate_kpps)  APPLY_CLAMP("rate_kpps",rate_kpps,0,G.max_rate_kpps)
        APPLY_F("intensity",intensity)  APPLY_CLAMP("intensity",intensity,0,1)
        // Directly setting the brightness fader (any source: UI, REST, MIDI)
        // implicitly opens the footswitch gate too, so it's visible right
        // away without needing the pedal held - see g_brightness_gate_open's
        // comment for the full footswitch/fader interaction.
        if (json_float(req.body,"intensity",-9999) != -9999) g_brightness_gate_open.store(true);
        APPLY_F("flash_release_ms",flash_release_ms) APPLY_CLAMP("flash_release_ms",flash_release_ms,0,2000)
        APPLY_F("r",r) APPLY_CLAMP("r",r,0,1)
        APPLY_F("g",g) APPLY_CLAMP("g",g,0,1)
        APPLY_F("b",b) APPLY_CLAMP("b",b,0,1)
        APPLY_F("shape_scale",shape_scale) APPLY_CLAMP("shape_scale",shape_scale,-1,1)
        APPLY_F("pos_x",pos_x) APPLY_CLAMP("pos_x",pos_x,-1,1)
        APPLY_F("pos_y",pos_y) APPLY_CLAMP("pos_y",pos_y,-1,1)
        APPLY_F("rotation_speed",rotation_speed)
        APPLY_B("mirror_x",mirror_x)
        APPLY_F("move_speed",move_speed)
        APPLY_F("move_size",move_size) APPLY_CLAMP("move_size",move_size,0,6)
        APPLY_F("wave_frequency",wave_frequency)
        APPLY_F("wave_amplitude",wave_amplitude) APPLY_CLAMP("wave_amplitude",wave_amplitude,0,1)
        APPLY_F("wave_speed",wave_speed)
        APPLY_F("rainbow_amount",rainbow_amount) APPLY_CLAMP("rainbow_amount",rainbow_amount,0,1)
        APPLY_F("rainbow_speed",rainbow_speed)
        APPLY_F("dot_amount",dot_amount) APPLY_CLAMP("dot_amount",dot_amount,0,1)
        APPLY_F("flicker_hz",flicker_hz)
        APPLY_B("blackout",blackout)
        APPLY_F("points",points)
        } // lock released here
        res.set_content(state_to_json(),"application/json");
    });

    // ── POST /laser/shape/<shape> ───────────────────────────────────────────────
    static const std::set<std::string> SHAPES{"circle","line","triangle","square","wave","staticwave"};
    svr.Post(R"(/laser/shape/([a-z]+))",[](const httplib::Request& req,httplib::Response& res){
        std::string s=req.matches[1];
        if(!SHAPES.count(s)){res.status=400;res.set_content("{\"error\":\"unknown shape\"}","application/json");return;}
        {std::lock_guard<std::mutex> lk(G_mtx);G.shape=s;}
        res.set_content("{\"shape\":\""+s+"\"}","application/json");
    });

    // ── POST /laser/brightness/<val> ───────────────────────────────────────────
    svr.Post(R"(/laser/brightness/([\d.]+))",[](const httplib::Request& req,httplib::Response& res){
        try{float v=std::stof(std::string(req.matches[1]));
            std::lock_guard<std::mutex> lk(G_mtx);
            G.intensity=std::clamp(v>1.0f?v/255.0f:v,0.0f,1.0f);
            g_brightness_gate_open.store(true); // touching the fader directly opens the gate too
            }catch(...){}
        res.set_content(state_to_json(),"application/json");
    });

    // ── POST /laser/color/<r>/<g>/<b> ──────────────────────────────────────────
    svr.Post(R"(/laser/color/([\d.]+)/([\d.]+)/([\d.]+))",[](const httplib::Request& req,httplib::Response& res){
        try{std::lock_guard<std::mutex> lk(G_mtx);
            float rv=std::stof(std::string(req.matches[1]));
            float gv=std::stof(std::string(req.matches[2]));
            float bv=std::stof(std::string(req.matches[3]));
            auto norm=[](float v){return v>1.0f?v/255.0f:v;};
            G.r=std::clamp(norm(rv),0.0f,1.0f);
            G.g=std::clamp(norm(gv),0.0f,1.0f);
            G.b=std::clamp(norm(bv),0.0f,1.0f);}catch(...){}
        res.set_content(state_to_json(),"application/json");
    });

    // ── POST /laser/position/<x>/<y> ───────────────────────────────────────────
    svr.Post(R"(/laser/position/(-?[\d.]+)/(-?[\d.]+))",[](const httplib::Request& req,httplib::Response& res){
        try{std::lock_guard<std::mutex> lk(G_mtx);
            G.pos_x=std::clamp(std::stof(std::string(req.matches[1])),-1.0f,1.0f);
            G.pos_y=std::clamp(std::stof(std::string(req.matches[2])),-1.0f,1.0f);}catch(...){}
        res.set_content(state_to_json(),"application/json");
    });

    // ── POST /laser/rotation/speed/<val> ───────────────────────────────────────
    svr.Post(R"(/laser/rotation/speed/(-?[\d.]+))",[](const httplib::Request& req,httplib::Response& res){
        try{std::lock_guard<std::mutex> lk(G_mtx);G.rotation_speed=std::stof(std::string(req.matches[1]));}catch(...){}
        res.set_content(state_to_json(),"application/json");
    });

    // ── POST /rotation/reset — snap angle back to 0 and stop spinning ──────────
    // Shares do_rotation_reset with the MIDI "rotation_reset" note action.
    svr.Post("/rotation/reset",[](const httplib::Request&,httplib::Response& res){
        do_rotation_reset();
        res.set_content(state_to_json(),"application/json");
    });

    // ── POST /move/mode/<mode> ─────────────────────────────────────────────────
    static const std::set<std::string> MOVES{"none","circle","pan","tilt","eight","random"};
    svr.Post(R"(/move/mode/([a-z]+))",[](const httplib::Request& req,httplib::Response& res){
        std::string m=req.matches[1];
        if(!MOVES.count(m)){res.status=400;res.set_content("{\"error\":\"unknown mode\"}","application/json");return;}
        {std::lock_guard<std::mutex> lk(G_mtx);G.move_mode=m;}
        res.set_content("{\"move_mode\":\""+m+"\"}","application/json");
    });

    // ── POST /move/speed/<val>  /move/size/<val> ───────────────────────────────
    svr.Post(R"(/move/speed/(-?[\d.]+))",[](const httplib::Request& req,httplib::Response& res){
        try{std::lock_guard<std::mutex> lk(G_mtx);G.move_speed=std::stof(std::string(req.matches[1]));}catch(...){}
        res.set_content(state_to_json(),"application/json");
    });
    svr.Post(R"(/move/size/([\d.]+))",[](const httplib::Request& req,httplib::Response& res){
        try{std::lock_guard<std::mutex> lk(G_mtx);G.move_size=std::clamp(std::stof(std::string(req.matches[1])),0.0f,6.0f);}catch(...){}
        res.set_content(state_to_json(),"application/json");
    });

    // ── POST /laser/rainbow/amount/<val>  /speed/<val> ─────────────────────────
    svr.Post(R"(/laser/rainbow/amount/([\d.]+))",[](const httplib::Request& req,httplib::Response& res){
        try{std::lock_guard<std::mutex> lk(G_mtx);G.rainbow_amount=std::clamp(std::stof(std::string(req.matches[1])),0.0f,1.0f);}catch(...){}
        res.set_content(state_to_json(),"application/json");
    });
    svr.Post(R"(/laser/rainbow/speed/(-?[\d.]+))",[](const httplib::Request& req,httplib::Response& res){
        try{std::lock_guard<std::mutex> lk(G_mtx);G.rainbow_speed=std::stof(std::string(req.matches[1]));}catch(...){}
        res.set_content(state_to_json(),"application/json");
    });

    // ── POST /blackout/<0|1> ───────────────────────────────────────────────────
    svr.Post(R"(/blackout/([01]))",[](const httplib::Request& req,httplib::Response& res){
        {std::lock_guard<std::mutex> lk(G_mtx);G.blackout=(req.matches[1]=="1");}
        res.set_content(state_to_json(),"application/json");
    });

    // ── POST /mirror/x/<0|1> — momentary (or toggled) horizontal flip ─────────
    // Same shared-state pattern as /blackout/<0|1>: 1 = mirrored, 0 = normal.
    // Used identically by the UI, REST, and the MIDI "mirror_hold" action
    // (a hold button flips while held, un-flips on release).
    svr.Post(R"(/mirror/x/([01]))",[](const httplib::Request& req,httplib::Response& res){
        {std::lock_guard<std::mutex> lk(G_mtx);G.mirror_x=(req.matches[1]=="1");}
        res.set_content(state_to_json(),"application/json");
    });

    // ── POST /motion/hold/<0|1> — momentary movement+rotation+rainbow freeze ──
    // Shares do_motion_hold_press/release with the MIDI "motion_hold" action
    // so triggering it from the UI/REST or a controller behaves identically.
    svr.Post(R"(/motion/hold/([01]))",[](const httplib::Request& req,httplib::Response& res){
        if (req.matches[1]=="1") do_motion_hold_press(); else do_motion_hold_release();
        res.set_content(state_to_json(),"application/json");
    });

    // ── POST /brightness/gate/<0|1> — footswitch master-brightness gate ───────
    // 1 = pedal held (hardware output renders normally), 0 = pedal up
    // (hardware output forced dark) - see g_brightness_gate_open's comment.
    // Shared state with the MIDI "brightness_gate" CC action.
    svr.Post(R"(/brightness/gate/([01]))",[](const httplib::Request& req,httplib::Response& res){
        g_brightness_gate_open.store(req.matches[1]=="1");
        res.set_content(state_to_json(),"application/json");
    });

    // ── POST /flash/<0|1> — momentary white + full brightness while held ─────
    // 1 = press (forces color to white and full brightness, remembering the
    // prior values), 0 = release (restores them). Shares do_flash_press/
    // release with the MIDI dispatcher so a flash triggered from the UI,
    // REST, or MIDI (including a footswitch sending note or CC64) all
    // behave identically.
    svr.Post(R"(/flash/([01]))",[](const httplib::Request& req,httplib::Response& res){
        if (req.matches[1]=="1") do_flash_press(); else do_flash_release();
        res.set_content(state_to_json(),"application/json");
    });

    // ── POST /flash/release_ms/<v> — flash release fade time (0..2000ms) ──────
    // 0 = instant restore (default), >0 = fade brightness to 0 over this
    // many ms on release instead of snapping back - ported from the
    // original BeamCommander's flashReleaseMs knob.
    svr.Post(R"(/flash/release_ms/([\d.]+))",[](const httplib::Request& req,httplib::Response& res){
        try{std::lock_guard<std::mutex> lk(G_mtx);G.flash_release_ms=std::clamp(std::stof(std::string(req.matches[1])),0.0f,2000.0f);}catch(...){}
        res.set_content(state_to_json(),"application/json");
    });

    // ── POST /api/reset — restore all show params to defaults ─────────────────
    // Preserves the current controller connection (target_ip / armed state),
    // the global flash_release_ms / max_rate_kpps settings, blackout, and
    // the zone 1 calibration (see their comments) so resetting the show
    // doesn't drop the laser link, change the footswitch's fade time,
    // change the venue's configured scan-rate ceiling, silently un-blank/
    // blank the laser, or undo the projector's physical zone alignment.
    svr.Post("/api/reset",[](const httplib::Request&,httplib::Response& res){
        {
            std::lock_guard<std::mutex> lk(G_mtx);
            std::string ip = G.target_ip;
            float flash_release_ms = G.flash_release_ms;
            float max_rate_kpps = G.max_rate_kpps;
            bool blackout = G.blackout;
            float zone_x = G.zone_x, zone_y = G.zone_y, zone_scale_x = G.zone_scale_x, zone_scale_y = G.zone_scale_y;
            G = LaserState{};
            G.target_ip = ip;
            G.flash_release_ms = flash_release_ms;
            G.max_rate_kpps = max_rate_kpps;
            G.rate_kpps = std::clamp(G.rate_kpps, 0.0f, G.max_rate_kpps);
            G.blackout = blackout;
            G.zone_x = zone_x; G.zone_y = zone_y; G.zone_scale_x = zone_scale_x; G.zone_scale_y = zone_scale_y;
        }
        res.set_content(state_to_json(),"application/json");
    });

    // ── POST /laser/connect/<ip>  /laser/disconnect ────────────────────────────
    svr.Post(R"(/laser/connect/(.+))",[](const httplib::Request& req,httplib::Response& res){
        std::string ip=req.matches[1];
        {std::lock_guard<std::mutex> lk(G_mtx);G.target_ip=ip;}
        G_armed=true;
        res.set_content("{\"ip\":\""+ip+"\",\"armed\":true}","application/json");
    });
    svr.Post("/laser/disconnect",[](const httplib::Request&,httplib::Response& res){
        G_armed=false;{std::lock_guard<std::mutex> lk(G_mtx);G.target_ip="";}
        res.set_content("{\"armed\":false}","application/json");
    });

    // ── GET /api/cues — list all populated cue slots ───────────────────────────
    svr.Get("/api/cues",[](const httplib::Request&,httplib::Response& res){
        std::ostringstream ss; ss<<"{";
        { std::lock_guard<std::mutex> lk(G_cues_mtx);
          bool first=true;
          for (auto& [num,s] : G_cues) {
              if(!first) ss<<","; first=false;
              ss<<"\""<<num<<"\":"<<cue_state_to_json(s);
          } }
        ss<<"}";
        res.set_content(ss.str(),"application/json");
    });

    // ── POST /api/cue/<n>/save — snapshot current show params into slot n ─────
    svr.Post(R"(/api/cue/(\d+)/save)",[](const httplib::Request& req,httplib::Response& res){
        int n=std::stoi(std::string(req.matches[1]));
        if(n<1||n>MAX_CUES){res.status=400;res.set_content("{\"error\":\"invalid cue number\"}","application/json");return;}
        do_cue_save(n);
        res.set_content("{\"saved\":"+std::to_string(n)+"}","application/json");
    });

    // ── POST /api/cue/<n>/recall — apply slot n's saved show params ────────────
    // Preserves the current controller connection (target_ip), just like
    // /api/reset does, so recalling a cue never drops the laser link.
    svr.Post(R"(/api/cue/(\d+)/recall)",[](const httplib::Request& req,httplib::Response& res){
        int n=std::stoi(std::string(req.matches[1]));
        if(n<1||n>MAX_CUES){res.status=400;res.set_content("{\"error\":\"invalid cue number\"}","application/json");return;}
        if(!do_cue_recall(n)){res.status=404;res.set_content("{\"error\":\"empty cue\"}","application/json");return;}
        res.set_content(state_to_json(),"application/json");
    });

    // ── POST /api/cue/<n>/clear — delete a saved cue slot ──────────────────────
    svr.Post(R"(/api/cue/(\d+)/clear)",[](const httplib::Request& req,httplib::Response& res){
        int n=std::stoi(std::string(req.matches[1]));
        if(n<1||n>MAX_CUES){res.status=400;res.set_content("{\"error\":\"invalid cue number\"}","application/json");return;}
        do_cue_clear(n);
        res.set_content("{\"cleared\":"+std::to_string(n)+"}","application/json");
    });

    // ── POST /api/cue/<from>/move/<to> — relocate a saved cue, overwriting <to> ─
    svr.Post(R"(/api/cue/(\d+)/move/(\d+))",[](const httplib::Request& req,httplib::Response& res){
        int from=std::stoi(std::string(req.matches[1]));
        int to  =std::stoi(std::string(req.matches[2]));
        if(from<1||from>MAX_CUES||to<1||to>MAX_CUES){res.status=400;res.set_content("{\"error\":\"invalid cue number\"}","application/json");return;}
        if(!do_cue_move(from,to)){res.status=404;res.set_content("{\"error\":\"empty source cue, or from == to\"}","application/json");return;}
        res.set_content("{\"moved\":"+std::to_string(from)+",\"to\":"+std::to_string(to)+"}","application/json");
    });

    // ── WebSocket /ws/points ───────────────────────────────────────────────────
    svr.WebSocket("/ws/points",[](const httplib::Request&,httplib::ws::WebSocket& ws){
        {std::lock_guard<std::mutex> lk(G_ws_mtx);G_ws_clients.insert(&ws);}
        std::string msg;
        while(ws.read(msg)!=httplib::ws::ReadResult::Fail){}
        {std::lock_guard<std::mutex> lk(G_ws_mtx);G_ws_clients.erase(&ws);}
    });

    std::cout << "[laser_daemon] Listening on :" << http_port << "\n"
              << "  GET/POST /api/state   POST /api/reset\n"
              << "  GET /api/cues   POST /api/cue/<1-" << MAX_CUES << ">/save|recall|clear|move/<n>\n"
              << "  POST /laser/shape/<circle|line|triangle|square|wave|staticwave>\n"
              << "  POST /laser/brightness/<0-1>  /laser/color/<r>/<g>/<b>\n"
              << "  POST /laser/position/<x>/<y>  /laser/rotation/speed/<v>\n"
              << "  POST /move/mode/<none|circle|pan|tilt|eight|random>\n"
              << "  POST /laser/rainbow/amount/<v>  /laser/rainbow/speed/<v>\n"
              << "  POST /blackout/<0|1>  /flash/<0|1>  /mirror/x/<0|1>  /motion/hold/<0|1>\n"
              << "  POST /brightness/gate/<0|1>  /rotation/reset\n"
              << "  POST /laser/connect/<ip>  /laser/disconnect\n"
              << "  WS   /ws/points\n"
              << "  MIDI: optional, see backend/midi_map.json (Akai APC40 mkII or any USB controller)\n";

    std::thread stop_t([&](){while(G_running)std::this_thread::sleep_for(100ms);svr.stop();});
    svr.listen("0.0.0.0",http_port);
    stop_t.join(); laser_t.join();
    return 0;
}
