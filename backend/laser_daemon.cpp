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
#include "json_lite.h"
#include "output_pipeline.h" // LaserMask + hardware output safety pipeline (unit-tested, see backend/tests/)
#include "param_smoothing.h" // soft transitions of show parameters (unit-tested, see backend/tests/)
#include "show_fx.h"         // flicker rate + on time (unit-tested, see backend/tests/)

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>
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
    float rate_kpps       = 30.0f;
    // Upper bound (and initial default value, above) for rate_kpps - a
    // venue/hardware setting ("how fast can this scanner safely run"), not
    // a per-look show parameter, so - like flash_release_ms - it's
    // deliberately excluded from cue_state_to_json/cue_state_from_json and
    // preserved across cue recalls (do_cue_recall()) and /api/reset. The
    // scan-rate fader/REST/MIDI input is clamped to this value everywhere
    // it's applied, so turning it down here also lowers the fader's max.
    float max_rate_kpps   = 30.0f;
    // How far ahead frames are queued for the DACs (libera's target latency,
    // ms): every change - cue switch, fader, flash - reaches the laser this
    // much later. libera's own default is 100 ms; lower = snappier, but too
    // low can underrun a DAC on a slow/jittery network (brief blank gaps).
    // Global daemon setting like max_rate_kpps: not part of cues, kept
    // across cue recall and /api/reset.
    float output_latency_ms = 40.0f;

    // Shape
    std::string shape     = "circle";
    float radius          = 0.7f;     // normalised 0..1
    int   points          = 300;
    float shape_scale     = 0.0f;     // -1..1 (0 = no extra scale)

    // Color (0..1)
    float r = 0.0f, g = 1.0f, b = 0.314f;
    // Master brightness. Like flash_release_ms below, this is
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
    // *global* daemon setting, not part of the per-cue show state - it's
    // deliberately excluded from cue_state_to_json/
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
    std::string move_mode = "none";   // none circle pan tilt tiltup tiltdown eight random
    float move_speed      = 0.30f;    // cycles/sec
    float move_size       = 0.50f;    // 0..6
    float move_phase_degrees = 0.0f;  // additional phase per configured laser, degrees
    // Render-only (never set by clients, not part of cues/API): crossfade
    // from the previous movement pattern, filled in by ShowParamSmoother
    // (param_smoothing.h) on the snapshot passed to make_frame().
    std::string move_mode_from = "none";
    float move_blend      = 1.0f;     // 0 = previous pattern, 1 = move_mode

    // Wave shape params
    float wave_frequency  = 1.0f;     // cycles across width
    float wave_amplitude  = 0.45f;    // fraction of radius
    float wave_speed      = 0.0f;     // cycles/sec (animated wave)

    // Rainbow
    float rainbow_amount  = 0.0f;     // 0..1 blend
    float rainbow_speed   = 0.0f;     // hue cycles/sec
    float rainbow_phase_degrees = 0.0f; // hue offset per configured laser, degrees (360 = one full hue cycle)

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
    float flicker_hz      = 0.0f;     // strobe frequency (flicker rate)
    float flicker_on_ms   = 0.0f;     // lit time per flicker period in ms, 0 = auto (half the period)
    float flicker_phase_degrees = 0.0f; // flicker delay per configured laser, degrees (360 = one period)

    // Preview-only
    int   persistence_ms  = 25;
};

static LaserState        G;

// `next` (a cue, a momentary snapshot, the defaults) with the *global* daemon
// settings taken over from `current`: footswitch fade time, scan-rate
// ceiling, output latency, blackout and - for cue recall - master brightness.
// None of these are part of a look, so switching looks never changes them.
static LaserState with_global_settings(LaserState next, const LaserState& current, bool keepIntensity) {
    next.flash_release_ms  = current.flash_release_ms;
    next.max_rate_kpps     = current.max_rate_kpps;
    next.output_latency_ms = current.output_latency_ms;
    next.blackout          = current.blackout;
    if (keepIntensity) next.intensity = current.intensity;
    next.rate_kpps = std::clamp(next.rate_kpps, 0.0f, next.max_rate_kpps);
    return next;
}
static std::mutex        G_mtx;
static std::atomic<bool> G_running{true};

// ── Laser (DAC) list — multiple physical EtherDream controllers ────────────
// Unlike LaserState (the single show/look, applied identically everywhere),
// each entry here is one physical DAC connection with its own venue
// calibration ("zone"). `armed` is the master enable for that specific
// laser - false means configured but idle (no network connection ever
// attempted, e.g. while wiring it up or between songs); true means
// laser_thread() should connect to it and stream the show, transformed by
// *this laser's own* zone_x/zone_y/zone_scale_x/zone_scale_y (a master pan/
// zoom mapping the whole show onto a sub-region of this laser's physical
// range - e.g. a wall/truss segment - since two projectors in the same rig
// often need different calibrations even when showing the exact same
// content). Set via the Zoning panel's drag-to-move/drag-edge-to-resize
// box (ZoningPanel.vue's laser selector) or numerically through
// POST /api/lasers/<id>. Any number of lasers can be armed at once, in
// which case laser_thread() streams to all of them in parallel (see its
// hardware-send step) - never blocking on the network write, so one slow/
// unreachable DAC never lags another's output. Persisted to its own file
// (lasers.json) and REST surface (GET/POST/DELETE /api/lasers). Each laser
// also has its own list of safety masks (LaserMask, output_pipeline.h).
struct LaserConfig {
    int         id;
    std::string name;
    std::string ip;
    bool        armed = false; // master enable - connect+stream only when true
    float       zone_x = 0.0f, zone_y = 0.0f;             // -1..1, offset of the whole output
    float       zone_scale_x = 1.0f, zone_scale_y = 1.0f; // 0.1..2, independent per-axis multiplier
    bool        mirror_x = false; // this laser's own horizontal flip (hardware-only, applied before its zone offset/scale) - independent of the global LaserState::mirror_x
    bool        mirror_y = false; // same, vertical flip (e.g. a projector mounted upside down)
    float       preview_x = 0.5f, preview_y = 0.5f; // normalized position in the browser preview
    std::vector<LaserMask> masks; // safety masks in this laser's output space (see LaserMask)
};
static std::vector<LaserConfig> G_lasers;
static std::mutex               G_lasers_mtx;
static int                      G_next_laser_id = 1;

// Runtime connection status per laser id, written only by laser_thread and
// read by GET /api/lasers - separate from G_lasers/G_lasers_mtx above
// (which holds the persisted *configuration*) since "connected" is a live
// fact about the actual hardware link, never set directly by a client.
static std::map<int, bool> G_laser_connected;
static std::mutex          G_laser_connected_mtx;
static void set_laser_connected(int id, bool connected) {
    std::lock_guard<std::mutex> lk(G_laser_connected_mtx);
    G_laser_connected[id] = connected;
}
static bool get_laser_connected(int id) {
    std::lock_guard<std::mutex> lk(G_laser_connected_mtx);
    auto it = G_laser_connected.find(id);
    return it != G_laser_connected.end() && it->second;
}


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
// *except* G_rotation_phase, which cue save reads from other threads (atomic).
// Cue recall / rotation reset never write it directly: they call
// request_rotation_angle(), and laser_thread applies the angle (together with
// the new rotation speed, no glide) at its next tick - so a recalled cue
// always starts exactly at its saved angle.
static double             G_move_phase     = 0.0; // radians
static std::atomic<double> G_rotation_phase{0.0}; // radians
static double             G_wave_phase     = 0.0; // radians
static double             G_rainbow_phase  = 0.0; // cycles, *unwrapped* (0 = rainbow just switched on) - see laser_thread()
// Bumped by motion hold / rotation reset: laser_thread() then applies the
// new speeds immediately instead of gliding (see ShowParamSmoother) - a
// freeze must be a freeze, not a slow-down.
static std::atomic<unsigned> G_speed_snap_seq{0};
// Rotation start angle requests (cue recall, rotation reset, momentary cue
// release) - see request_rotation_angle().
static std::atomic<double>   G_pending_rotation_angle{0.0};
static std::atomic<unsigned> G_rotation_set_seq{0};
static std::atomic<unsigned> G_rotation_applied_seq{0}; // last request laser_thread applied
// The current rotation angle - including a requested one laser_thread
// hasn't applied yet (e.g. a cue saved right after a rotation reset).
static double current_rotation_angle() {
    if (G_rotation_set_seq.load() != G_rotation_applied_seq.load()) return G_pending_rotation_angle.load();
    return G_rotation_phase.load();
}
// Cue recall / momentary cue: the whole new look applies at once - no glide
// of any parameter (scale, speeds, position, wave, movement crossfade, ...),
// see laser_thread(). Live fader/knob changes keep gliding.
static std::atomic<unsigned> G_show_snap_seq{0};

// Call while holding G_mtx right after setting G.rotation_speed, so
// laser_thread sees the new speed and the angle in the same tick.
static void request_rotation_angle(double radians) {
    G_pending_rotation_angle.store(radians);
    G_rotation_set_seq.fetch_add(1);
}

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
// forced dark. Applied after make_frame() in finalize_hardware_frame()
// (output_pipeline.h) - to every laser's DAC copy *and* to the browser
// preview, which shows the real output (blackout being the only exception).
// Deliberately does NOT
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
//
// Defaults OPEN (true): a footswitch is optional gear, not every rig has
// one wired up, and requiring it just to get any output at all (on top of
// the `blackout` safety toggle, which already defaults to closed/dark) was
// a trap - operators with no pedal connected had no way to ever see light.
// `blackout` remains the one deliberate, always-on-by-default safety gate;
// this one now only matters to rigs that actually use a pedal.
static std::atomic<bool> g_brightness_gate_open{true};

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
static bool                                  g_prev_gate_open = true;
static bool                                  g_gate_decaying = false;
static std::chrono::steady_clock::time_point g_gate_decay_start;
static float                                 g_gate_level = 1.0f;

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

static Pt calc_movement(const std::string& mode, float move_size, double phase_value) {
    if (mode == "none") return {0.0f,0.0f};
    const float tau = 2.0f * std::acos(-1.0f);
    float phase = std::fmod((float)phase_value, tau);
    if (phase < 0.0f) phase += tau;
    float amp   = move_size * 0.6f; // fraction of canvas
    if (mode=="circle")  return {amp*std::cos(phase), amp*std::sin(phase)};
    if (mode=="pan")     return {amp*std::sin(phase), 0.0f};
    if (mode=="tilt")    return {0.0f, amp*std::sin(phase)};
    if (mode=="tiltup") {
        // Unidirectional/continuous vertical sweep: rises linearly from the
        // bottom (-amp) to the top (+amp) as phase runs 0..tau, then wraps
        // instantly back to the bottom - i.e. it never comes back down, it
        // "re-enters" from the bottom once it exits the top of the screen.
        // G_move_phase is already kept in [0,tau) via fmod (see the phase
        // update loop below), so a plain linear map of that range is enough.
        return {0.0f, amp*(2.0f*(phase/tau) - 1.0f)};
    }
    if (mode=="tiltdown") {
        // Mirror of tiltup: falls linearly from the top (+amp) to the
        // bottom (-amp) as phase runs 0..tau, then wraps instantly back to
        // the top - re-enters from the top once it exits the bottom.
        return {0.0f, amp*(1.0f - 2.0f*(phase/tau))};
    }
    if (mode=="eight")   return {amp*std::sin(phase), amp*std::sin(2*phase)};
    if (mode=="random")  return {amp*(std::sin(phase*1.3f)+std::sin(phase*2.7f))/2,
                                        amp*(std::sin(phase*1.7f)+std::sin(phase*3.1f))/2};
    return {0.0f,0.0f};
}

// Simple RGB->HSV (all 0..1; h = 0 for grey/white/black)
static void rgb_to_hsv(float r, float g, float b, float& h, float& s, float& v) {
    float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    v = mx;
    s = mx > 0.0f ? d / mx : 0.0f;
    if (d <= 0.0f) { h = 0.0f; return; }
    if (mx == r)      h = (g - b) / d;
    else if (mx == g) h = 2.0f + (b - r) / d;
    else              h = 4.0f + (r - g) / d;
    h /= 6.0f;
    if (h < 0.0f) h += 1.0f;
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

static core::Frame make_frame(const LaserState& s, double movement_phase, double rainbow_cycles) {
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
    Pt mv = calc_movement(s.move_mode, s.move_size, movement_phase);
    // Switching the movement pattern crossfades from the old one (blend
    // driven by ShowParamSmoother) instead of jumping to a new position.
    if (s.move_blend < 1.0f && s.move_mode_from != s.move_mode) {
        const Pt from = calc_movement(s.move_mode_from, s.move_size, movement_phase);
        const float w = std::clamp(s.move_blend, 0.0f, 1.0f);
        mv.x = from.x + (mv.x - from.x) * w;
        mv.y = from.y + (mv.y - from.y) * w;
    }
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

    // HARDWARE SAFETY CAP: each dot can cost up to kBlankMax+kDotDwell+
    // kBlankTrail points (~86), so a shape with many base points (`points`,
    // up to 1000 via the UI slider) at a dot_amount just under 1.0 - the
    // overwhelmingly common case while slowly dragging the fader down -
    // could keep numDots close to n0 while every one of those dots still
    // pays close to the full per-dot cost, producing frames of tens of
    // thousands of points. That's far beyond what a real DAC's ring buffer
    // can hold in one frame (EtherDreamControllerInfo above is constructed
    // with a 4095-point buffer) and was traced back as the root cause of
    // the DAC locking up/needing a power cycle after slowly reducing dot
    // amount. Fix in two parts, both driven by the same fixed budget so
    // the *total* frame size sent to hardware always stays under a safe
    // cap no matter the combination of `points` and dot_amount:
    //  1. Hard-cap the number of lit dots itself (kMaxDots) - otherwise
    //     even the cheapest possible per-dot cost (kMinDotCost, all at the
    //     kBlankMin floor) could still add up to far more than the budget
    //     when `points` is large.
    //  2. Within that dot count, shrink the per-dot blank-settle ceiling
    //     as numDots grows, so large jumps still get proportionally more
    //     settle time relative to each other, just capped lower overall
    //     when there are many dots.
    const int kMaxFramePoints = 3000; // safely under the DAC's buffer capacity
    const int kMinDotCost = kBlankMin + kDotDwell + kBlankTrail;
    const int kMaxDots = kMaxFramePoints / kMinDotCost;
    if (dotted && numDots > kMaxDots) numDots = kMaxDots;
    int perDotBlankMax = kBlankMax;
    if (dotted && numDots > 0) {
        int budgetPerDot = kMaxFramePoints / numDots - kDotDwell - kBlankTrail;
        perDotBlankMax = std::clamp(budgetPerDot, kBlankMin, kBlankMax);
    }

    // Flicker/strobe is NOT applied here: it's a time-domain effect applied
    // per point by its playback time - in laser_thread()'s hardware-send
    // step (apply_flicker_timeline, show_fx.h) and per simulated point in the
    // browser preview - so both show the exact same rate and on time.

    // Rainbow: anchored to the current beam color so turning rainbow_amount
    // up from 0 starts *at* that color instead of jumping to red. The hue
    // starts at the beam color's hue (rainbow_cycles is this laser's
    // animated hue position, 0 when the rainbow was just switched on - see
    // the per-laser delay in laser_thread()), and over the first
    // kRainbowFadeIn of the amount range saturation/brightness glide from
    // the beam color's own S/V to the full rainbow S=V=1. So a pure beam
    // color, white or a dimmed color all blend in without a step.
    constexpr float kRainbowFadeIn = 0.1f;
    float rb_fade = std::clamp(s.rainbow_amount / kRainbowFadeIn, 0.0f, 1.0f);
    rb_fade = rb_fade * rb_fade * (3.0f - 2.0f * rb_fade); // smoothstep
    float beam_h, beam_s, beam_v;
    rgb_to_hsv(std::clamp(s.r, 0.0f, 1.0f), std::clamp(s.g, 0.0f, 1.0f), std::clamp(s.b, 0.0f, 1.0f),
               beam_h, beam_s, beam_v);
    const float rb_sat = beam_s + (1.0f - beam_s) * rb_fade;
    const float rb_val = beam_v + (1.0f - beam_v) * rb_fade;
    float rainbow_phase = (float)std::fmod(beam_h + rainbow_cycles, 1.0);
    if (rainbow_phase < 0.0f) rainbow_phase += 1.0f;

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
        // colorForX(): once rainbow_amount > 0 (past the short fade-in
        // from the beam color, see rb_fade above), points get a *full*
        // saturated HSV rainbow color (fully replacing the selected beam
        // color, not a washed-out lerp blend with it) - amount
        // controls how many color bands repeat across the shape (0 = no
        // rainbow at all, just above 0 = one wide hue band, growing
        // towards 1 = ever smaller bands, up to 24 rainbow cycles across
        // the shape), while rainbow_speed animates that hue over time via
        // rainbow_phase. Skipped while a white flash is held so flash
        // always wins - matches the original app's whiteFlash check,
        // which short-circuits colorForX the same way.
        if (s.rainbow_amount > 0.0f && !g_flash_active) {
            float x_norm = n > 1 ? (float)i / (float)(n - 1) : 0.0f;
            float cycles_across = s.rainbow_amount * 24.0f;
            // Minus, not plus: as rainbow_phase advances, each hue band then
            // moves towards higher x_norm, so new colors enter at the start
            // of the shape (left for lines/waves) and run to the other end -
            // the same left-to-right direction the per-laser rainbow delay
            // chain (laser_thread()) passes them from laser to laser.
            float hue = std::fmod(rainbow_phase - x_norm * cycles_across, 1.0f);
            if (hue < 0.0f) hue += 1.0f;
            hsv_to_rgb(hue, rb_sat, rb_val, fr, fg, fb);
        }

        // Master brightness. The footswitch gate (g_gate_level), blackout
        // and each laser's mirror/zone/masks are applied per laser
        // afterwards in finalize_hardware_frame() - to the DAC output and
        // the preview alike (the preview skips blackout) - never here.
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
        float bri = offScreen ? 0.0f : s.intensity;

        if (dotted) {
            // Arrive blanked and hold for a distance-scaled number of
            // settle points (see kBlankPerUnitDistance comment above), then
            // hold lit for kDotDwell, then blank again for kBlankTrail
            // before the beam is allowed to move off.
            float dx = px - prevx, dy = py - prevy;
            float dist = std::sqrt(dx * dx + dy * dy);
            int blankSettle = std::clamp((int)std::round(dist * kBlankPerUnitDistance) + kBlankMin,
                                          kBlankMin, perDotBlankMax);
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
static std::string frame_to_ws_json(const core::Frame& fr,
                                    const std::vector<std::pair<int, core::Frame>>& laserFrames,
                                    double showTime, float flickerHz, float flickerOnMs,
                                    const std::vector<std::pair<int, double>>& flickerOffsets) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(4);
    auto writePoints = [&](const core::Frame& frame) {
        ss << "[";
        for (std::size_t i=0;i<frame.points.size();++i) {
            if (i) ss<<',';
            const auto& p=frame.points[i];
            ss<<'['<<p.x<<','<<p.y<<','<<p.r<<','<<p.g<<','<<p.b<<']';
        }
        ss << "]";
    };
    // t = show clock (G_time) and fl = [flicker_hz, flicker_on_ms]: the
    // preview applies flicker per simulated point itself, in step with the
    // laser (frames here are sent unflickered - see make_frame()).
    ss << "{\"t\":" << std::setprecision(6) << showTime << std::setprecision(4)
       << ",\"fl\":[" << flickerHz << ',' << flickerOnMs << "]";
    // flo = per-laser flicker time offset in seconds (flicker phase).
    ss << ",\"flo\":{";
    for (std::size_t i = 0; i < flickerOffsets.size(); ++i)
        ss << (i ? "," : "") << '\"' << flickerOffsets[i].first << "\":" << std::setprecision(6) << flickerOffsets[i].second << std::setprecision(4);
    ss << "}";
    ss << ",\"pts\":";
    writePoints(fr);
    if (!laserFrames.empty()) {
        ss << ",\"laser_pts\":{";
        for (std::size_t i=0;i<laserFrames.size();++i) {
            if (i) ss << ',';
            ss << '\"' << laserFrames[i].first << "\":";
            writePoints(laserFrames[i].second);
        }
        ss << "}";
    }
    ss << "}";
    return ss.str();
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
       << "\"output_latency_ms\":" << G.output_latency_ms << ","
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
    << "\"move_phase_degrees\":" << G.move_phase_degrees << ","
       << "\"wave_frequency\":" << G.wave_frequency << ","
       << "\"wave_amplitude\":" << G.wave_amplitude << ","
       << "\"wave_speed\":"    << G.wave_speed     << ","
       << "\"rainbow_amount\":" << G.rainbow_amount << ","
       << "\"rainbow_speed\":" << G.rainbow_speed  << ","
       << "\"rainbow_phase_degrees\":" << G.rainbow_phase_degrees << ","
       << "\"blackout\":"      << (G.blackout?"true":"false") << ","
       << "\"dot_amount\":"    << G.dot_amount     << ","
       << "\"flicker_hz\":"    << G.flicker_hz     << ","
       << "\"flicker_on_ms\":" << G.flicker_on_ms  << ","
       << "\"flicker_phase_degrees\":" << G.flicker_phase_degrees << ","
       << "\"brightness_gate_open\":" << (g_brightness_gate_open.load()?"true":"false")
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

// Per-laser settings for finalize_hardware_frame() - shared by the real DAC
// output and the browser preview, so both always go through the exact same
// processing. `outputArmed` is whether this laser may emit light at all.
static HardwareOutputParams hardware_params(const LaserConfig* cfg, const LaserState& snap, bool outputArmed) {
    HardwareOutputParams out;
    out.blackout      = snap.blackout;
    out.outputAllowed = cfg && outputArmed;
    out.gateLevel     = g_gate_level;
    if (cfg) {
        out.mirrorX = cfg->mirror_x;
        out.mirrorY = cfg->mirror_y;
        out.zoneX = cfg->zone_x; out.zoneY = cfg->zone_y;
        out.zoneScaleX = cfg->zone_scale_x; out.zoneScaleY = cfg->zone_scale_y;
        out.masks = &cfg->masks;
    }
    return out;
}

// ── Laser output thread ────────────────────────────────────────────────────────
// Preview (WebSocket) generation is fully decoupled from the hardware
// connection: the browser preview must keep responding to every parameter
// change whether or not any real controller is connected/armed.
//
// Multi-DAC output: every laser in G_lasers with armed == true gets its
// own core::LaserController and its own connection/pacing state (see
// LaserRuntime below), all owned locally by this thread (never shared with
// the HTTP thread - only G_laser_connected's bool summary is, via
// set_laser_connected()). Each tick, every armed+connected controller is
// offered the show frame - transformed by *that laser's own* zone
// calibration - if *that* controller's own isReadyForNewFrame()/20ms pacing
// says it's due - since core::LaserController queues frames internally
// rather than blocking on the network write, looping over N controllers
// here adds no meaningful per-controller latency, so all armed lasers
// stream in parallel with no cross-talk lag between them.
static void laser_thread() {
    System sys;
    core::LaserController::setMaxFrameHoldTime(std::chrono::milliseconds(0));

    using Clock = std::chrono::steady_clock;
    auto last_preview = Clock::now() - 1s;
    auto last_tick    = Clock::now();

    struct LaserRuntime {
        std::shared_ptr<core::LaserController> ctrl;
        std::string connected_ip;
        float       connected_rate = 0;
        // Last reported link state (ctrl->hasConnection()), only used to log changes.
        bool        link_up = false;
        // DAC protection (see the connection loop below): output armed on this controller,
        // last live rate update, and the current reconnect back-off.
        bool        output_armed = false;
        std::chrono::steady_clock::time_point last_rate_update = std::chrono::steady_clock::now() - 1s;
        std::chrono::milliseconds connect_backoff{1000};
        // Paces this controller's own sendFrame() calls - without this,
        // the loop would call sendFrame() every time isReadyForNewFrame()
        // allows (which can be much more often than a frame's actual
        // physical draw time of points/rate_kpps), enqueueing new frames
        // faster than the hardware can drain them. Each laser gets its own
        // independent 20ms floor so one DAC's pacing can never starve or
        // be starved by another's.
        std::chrono::steady_clock::time_point last_hw_send        = std::chrono::steady_clock::now() - 1s;
        std::chrono::steady_clock::time_point next_connect_attempt = std::chrono::steady_clock::now();
        // Continuous point clock for exact per-point flicker (show_fx.h).
        FlickerTimeline flicker;
    };
    std::map<int, LaserRuntime> runtimes; // keyed by LaserConfig::id - this thread's own state, never shared

    // Rendered (smoothed) show parameters - see param_smoothing.h. Owned by
    // this thread only; G keeps the targets set by REST/MIDI/cues.
    ShowParamSmoother<LaserState> smooth;
    unsigned lastSpeedSnap = G_speed_snap_seq.load();
    unsigned lastRotationSet = G_rotation_set_seq.load();
    unsigned lastShowSnap = G_show_snap_seq.load();
    float appliedLatencyMs = -1.0f;

    while (G_running) {
        // Advance animation clock
        auto now_tp = Clock::now();
        double dt = std::chrono::duration<double>(now_tp - last_tick).count();
        G_time += dt;
        last_tick = now_tp;

        float rate;
        {
            std::lock_guard<std::mutex> lk(G_mtx);
            rate = G.rate_kpps;

            // Glide the rendered show parameters towards their targets
            // (snapping the speeds when motion hold/rotation reset asked
            // for it), *before* integrating the phases below, so speed
            // changes accelerate/decelerate smoothly too.
            smooth.update(G, (float)dt);
            // Cue switch: the new look appears at once - every glide (incl.
            // position and wave knobs below) jumps straight to the target.
            if (const unsigned showSeq = G_show_snap_seq.load(); showSeq != lastShowSnap) {
                lastShowSnap = showSeq;
                smooth.snap_all(G);
                G_pos_x_smooth = G.pos_x; G_pos_y_smooth = G.pos_y;
                G_wave_frequency_smooth = G.wave_frequency;
                G_wave_amplitude_smooth = G.wave_amplitude;
                G_wave_speed_smooth     = G.wave_speed;
            }
            // Output latency (how far ahead libera queues frames).
            if (G.output_latency_ms != appliedLatencyMs) {
                appliedLatencyMs = G.output_latency_ms;
                core::LaserController::setTargetLatency(std::chrono::milliseconds(
                    (long long)std::lround(std::clamp(appliedLatencyMs, 10.0f, 200.0f))));
            }
            if (const unsigned snapSeq = G_speed_snap_seq.load(); snapSeq != lastSpeedSnap) {
                lastSpeedSnap = snapSeq;
                smooth.snap_speeds(G);
            }
            // Cue recall / rotation reset: jump to the requested start angle
            // and take the new rotation speed as is (no leftover spin).
            if (const unsigned rotSeq = G_rotation_set_seq.load(); rotSeq != lastRotationSet) {
                lastRotationSet = rotSeq;
                G_rotation_phase.store(G_pending_rotation_angle.load());
                smooth.snap_rotation(G);
                G_rotation_applied_seq.store(rotSeq);
            }

            // Integrate each animated parameter's phase by its *current*
            // speed - see the G_move_phase/etc. comment above for why this
            // (rather than speed*G_time) is what keeps things smooth across
            // a speed change instead of jumping/resetting.
            const double tau = 2.0 * std::acos(-1.0);
            G_move_phase     = std::fmod(G_move_phase     + smooth.move_speed * dt * tau, tau);
            // Negated here (not at every rotation_speed setter) so the
            // reversed direction applies no matter which API set the speed
            // (REST bulk update, /laser/rotation/speed/<v>, or MIDI).
            G_rotation_phase = std::fmod(G_rotation_phase - smooth.rotation_speed * dt * tau, tau);
            // Uses the *smoothed* wave speed (updated below) rather than
            // G.wave_speed directly, so a MIDI knob jump changes the wave's
            // animation rate gradually instead of snapping to a new speed
            // instantly - matching the same glide the value knobs get.
            G_wave_phase     = std::fmod(G_wave_phase     + G_wave_speed_smooth * dt * tau, tau);
            // Held at 0 while the rainbow is off, so turning it back on
            // always starts at the current beam color (see make_frame()).
            // Deliberately not wrapped: the per-laser rainbow delay below
            // needs to know how far the hue has travelled since switch-on.
            G_rainbow_phase  = smooth.rainbow_amount > 0.0f
                ? G_rainbow_phase + smooth.rainbow_speed * dt : 0.0;

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

        // Snapshot the configured laser list (id/ip/armed/zone calibration)
        // - kept brief so no HTTP request (add/remove/(un)arm/re-calibrate)
        // is ever blocked on this thread's connect/send work below.
        std::vector<LaserConfig> lasers_snapshot;
        { std::lock_guard<std::mutex> lk(G_lasers_mtx); lasers_snapshot = G_lasers; }

        // Drop runtime state for any laser no longer configured at all
        // (deleted via DELETE /api/lasers/<id>).
        for (auto it = runtimes.begin(); it != runtimes.end(); ) {
            bool stillExists = std::any_of(lasers_snapshot.begin(), lasers_snapshot.end(),
                                            [&](const LaserConfig& l){ return l.id == it->first; });
            if (!stillExists) {
                if (it->second.ctrl) it->second.ctrl->setArmed(false);
                set_laser_connected(it->first, false);
                it = runtimes.erase(it);
            } else ++it;
        }

        // Connect/disconnect/retry each configured laser independently.
        //
        // DAC protection: EtherDream firmware can lock up hard (off the network until
        // power-cycled) when it sees rapid TCP connect/disconnect cycles. This loop used to
        // tear down and reopen the connection on *every* scan-rate change - up to ~1000x/s
        // while a rate fader/MIDI knob moved, or on each cue recall with another rate - and on
        // every disarm/arm toggle. Now:
        //  - rate changes are passed live (setPointRate, libera restarts the stream on the same
        //    connection), throttled to DAC_RATE_UPDATE_MIN and clamped to what the DAC supports;
        //  - arm/disarm act immediately (disarm releases the connection, the DAC stops at once);
        //  - new connection attempts per DAC are spaced >= DAC_MIN_RECONNECT (short enough to
        //    feel instant, long enough that even a stuck MIDI button or script can't cause a
        //    reconnect storm), backing off to 30s while connecting fails.
        constexpr auto  DAC_RATE_UPDATE_MIN      = 500ms;
        constexpr auto  DAC_MIN_RECONNECT        = 250ms;
        constexpr auto  DAC_MAX_BACKOFF          = std::chrono::seconds(30);
        constexpr float DAC_MIN_KPPS = 1.0f, DAC_MAX_KPPS = 100.0f;
        const float hwRate = std::clamp(rate, DAC_MIN_KPPS, DAC_MAX_KPPS);

        for (const auto& l : lasers_snapshot) {
            auto& rt = runtimes[l.id]; // default-constructed on first sight
            bool wantsOutput = l.armed && !l.ip.empty();

            // IP changed (or cleared): the old connection points at the wrong device.
            if (rt.ctrl && rt.connected_ip != l.ip) {
                rt.ctrl->setArmed(false); rt.ctrl.reset(); rt.connected_ip.clear();
                rt.output_armed = false;
                std::cout << "[laser] " << l.name << " IP changed - connection released\n" << std::flush;
            }

            if (!wantsOutput) {
                if (rt.ctrl) {
                    rt.ctrl->setArmed(false); rt.ctrl.reset(); rt.connected_ip.clear();
                    rt.output_armed = false;
                    std::cout << "[laser] " << l.name << " disarmed - connection released\n" << std::flush;
                }
            } else if (!rt.ctrl) {
                if (now_tp >= rt.next_connect_attempt) {
                    etherdream::EtherDreamControllerInfo info(l.ip, "Ether Dream", l.ip, 7765, 4095);
                    info.setDriverId("libera.builtin.etherdream");
                    rt.ctrl = sys.connectController(info);
                    if (!rt.ctrl) {
                        std::cout << "[laser] connect failed " << l.name << " (" << l.ip << "), retry in "
                                  << rt.connect_backoff.count() << "ms\n" << std::flush;
                        rt.next_connect_attempt = now_tp + rt.connect_backoff;
                        rt.connect_backoff = std::min<std::chrono::milliseconds>(rt.connect_backoff * 2, DAC_MAX_BACKOFF);
                    } else {
                        rt.ctrl->setPointRate(static_cast<std::uint32_t>(hwRate * 1000));
                        rt.ctrl->setArmed(true);
                        rt.output_armed = true;
                        rt.connected_ip = l.ip; rt.connected_rate = hwRate;
                        rt.last_rate_update = now_tp;
                        rt.next_connect_attempt = now_tp + DAC_MIN_RECONNECT; // never reopen faster than this
                        rt.connect_backoff = std::chrono::milliseconds(1000);
                        std::cout << "[laser] " << l.name << " armed, connecting to " << l.ip << " @ " << hwRate << " kpps\n" << std::flush;
                    }
                }
            } else {
                if (std::abs(hwRate - rt.connected_rate) > 0.1f && now_tp - rt.last_rate_update >= DAC_RATE_UPDATE_MIN) {
                    rt.ctrl->setPointRate(static_cast<std::uint32_t>(hwRate * 1000));
                    rt.connected_rate = hwRate;
                    rt.last_rate_update = now_tp;
                }
            }

            // "connected" reports the real link to the DAC, not just that a controller object
            // exists - an armed DAC that's unplugged/off/unreachable must show as disconnected
            // (connectController() succeeds even when the TCP connect then fails).
            bool link = rt.ctrl && rt.ctrl->hasConnection();
            if (link != rt.link_up) {
                rt.link_up = link;
                std::cout << "[laser] " << l.name << " (" << l.ip << ") "
                          << (link ? "connected" : "NOT reachable - check power/network") << "\n" << std::flush;
            }
            set_laser_connected(l.id, link);
        }

        bool anyHardwareReady = false;
        for (auto& [id, rt] : runtimes) {
            if (rt.ctrl && rt.ctrl->isReadyForNewFrame() && (now_tp - rt.last_hw_send) >= 20ms) {
                anyHardwareReady = true; break;
            }
        }
        bool previewDue = (now_tp - last_preview) >= 33ms;

        if (!anyHardwareReady && !previewDue) {
            std::this_thread::sleep_for(1ms);
            continue;
        }

        LaserState snap; { std::lock_guard<std::mutex> lk(G_mtx); snap = G; }
        smooth.apply(snap); // render the glided values, not the raw targets
        auto frame = make_frame(snap, G_move_phase, G_rainbow_phase);
        std::vector<std::pair<int, core::Frame>> laserFrames;
        // Per-laser offsets: laser i (Lasers-list order, laser 1 = 0) gets
        // i * move_phase_degrees added to its movement phase, so the
        // movement runs "one after another" across the rig instead of in
        // unison. The rainbow works as a delay chain instead: laser i runs
        // the exact same hue animation as laser 1, just i *
        // rainbow_phase_degrees (360 = one full hue cycle) behind it. Right
        // after the rainbow is switched on every laser therefore still
        // shows the beam color, laser 1 starts cycling first and the others
        // follow one after another - no laser jumps to an offset hue - and
        // once the chain has caught up it is a steady hue spread.
        const double phaseStep = snap.move_phase_degrees * (std::acos(-1.0) / 180.0);
        const double rainbowStep = snap.rainbow_phase_degrees / 360.0; // hue cycles
        const bool movePhased = snap.move_mode != "none" && std::abs(phaseStep) > 1e-9;
        const bool rainbowPhased = snap.rainbow_amount > 0.0f && std::abs(rainbowStep) > 1e-9;
        if ((movePhased || rainbowPhased) && !lasers_snapshot.empty()) {
            laserFrames.reserve(lasers_snapshot.size());
            laserFrames.emplace_back(lasers_snapshot[0].id, frame);
            for (std::size_t i = 1; i < lasers_snapshot.size(); ++i) {
                const double k = static_cast<double>(i);
                const double laserPhase = G_move_phase + (movePhased ? k * phaseStep : 0.0);
                double laserHue = G_rainbow_phase;
                if (rainbowPhased) {
                    // Lag behind laser 1 along the direction of travel,
                    // but never past the switch-on point (hue position 0).
                    laserHue = G_rainbow_phase >= 0.0
                        ? std::max(0.0, G_rainbow_phase - k * rainbowStep)
                        : std::min(0.0, G_rainbow_phase + k * rainbowStep);
                }
                laserFrames.emplace_back(lasers_snapshot[i].id, make_frame(snap, laserPhase, laserHue));
            }
        }

        if (previewDue) {
            last_preview = now_tp;
            // The preview shows exactly what each laser outputs: every
            // configured laser's frame goes through the same
            // finalize_hardware_frame() as its DAC copy below (master
            // brightness gate/flash, mirror, zone, safety masks, mask
            // outlines) - with one deliberate exception: blackout. The
            // preview keeps showing the look while the laser is blacked out,
            // so the operator can prepare/check it in the dark. "Armed" is
            // the configured state, so an armed laser that isn't connected
            // yet still previews what it will show (unarmed lasers are
            // hidden in the 3D preview anyway). Without any configured laser
            // the single virtual preview laser still honours the gate.
            std::vector<std::pair<int, core::Frame>> previewFrames;
            std::vector<std::pair<int, double>> flickerOffsets;
            previewFrames.reserve(lasers_snapshot.size());
            for (std::size_t li = 0; li < lasers_snapshot.size(); ++li) {
                const auto& l = lasers_snapshot[li];
                flickerOffsets.emplace_back(l.id, flicker_phase_offset(li, snap.flicker_hz, snap.flicker_phase_degrees));
                const core::Frame* base = &frame;
                for (const auto& [lid, lf] : laserFrames) if (lid == l.id) { base = &lf; break; }
                core::Frame pf = *base;
                HardwareOutputParams pp = hardware_params(&l, snap, l.armed);
                pp.blackout = false; // preview ignores blackout (see above)
                finalize_hardware_frame(pf.points, pp);
                previewFrames.emplace_back(l.id, std::move(pf));
            }
            core::Frame mainPreview;
            if (lasers_snapshot.empty()) {
                mainPreview = frame;
                HardwareOutputParams virt; virt.blackout = false; virt.outputAllowed = true; virt.gateLevel = g_gate_level;
                finalize_hardware_frame(mainPreview.points, virt);
            }
            ws_broadcast(frame_to_ws_json(mainPreview, previewFrames, G_time, snap.flicker_hz, snap.flicker_on_ms, flickerOffsets));
        }

        if (anyHardwareReady) {
            // Each controller gets the frame for its configured list index;
            // global blackout/gate and per-laser zone transforms are then
            // applied only to that hardware-bound copy.
            for (auto& [id, rt] : runtimes) {
                if (!rt.ctrl) continue;
                if (!(rt.ctrl->isReadyForNewFrame() && (now_tp - rt.last_hw_send) >= 20ms)) continue;
                rt.last_hw_send = now_tp;

                const LaserConfig* cfg = nullptr;
                std::size_t laserIndex = 0; // Lasers-list order, laser 1 = 0 (flicker phase)
                for (std::size_t i = 0; i < lasers_snapshot.size(); ++i) {
                    if (lasers_snapshot[i].id == id) { cfg = &lasers_snapshot[i]; laserIndex = i; break; }
                }

                const core::Frame* phaseFrame = &frame;
                if (!laserFrames.empty()) {
                    const int laserId = id;
                    auto it = std::find_if(laserFrames.begin(), laserFrames.end(),
                                           [laserId](const auto& item) { return item.first == laserId; });
                    if (it != laserFrames.end()) phaseFrame = &it->second;
                }
                core::Frame toSend = *phaseFrame;
                // Flicker, per point by playback time: pad the frame so the
                // DAC plays it once instead of looping it until the next one
                // arrives, then blank the points that fall into an "off"
                // phase of the continuous timeline (see show_fx.h).
                const double pointRateHz = std::max(1.0f, rt.connected_rate) * 1000.0;
                const bool flickering = snap.flicker_hz > 0.0f;
                if (flickering) {
                    pad_frame_to_duration(toSend.points, pointRateHz, kFlickerMinFrameSeconds, kFlickerMaxFramePoints);
                    // Flicker phase: this laser runs its flicker that much later.
                    const double phaseOffset = flicker_phase_offset(laserIndex, snap.flicker_hz, snap.flicker_phase_degrees);
                    apply_flicker_timeline(toSend.points, rt.flicker.begin(G_time) + phaseOffset, pointRateHz,
                                           snap.flicker_hz, snap.flicker_on_ms);
                } else {
                    rt.flicker.reset();
                }
                // Blackout/arm gate, footswitch, mirror + zone, safety masks
                // and mask outlines - all in output_pipeline.h, where the
                // order is documented and covered by backend/tests/.
                // Only an armed laser may emit light - checked here too,
                // independent of the connection handling above and of
                // libera's own blanking.
                finalize_hardware_frame(toSend.points,
                                        hardware_params(cfg, snap, cfg && cfg->armed && rt.output_armed));
                if (flickering) rt.flicker.advance(toSend.points.size(), pointRateHz);
                rt.ctrl->sendFrame(std::move(toSend));
            }
        }
    }
    for (auto& [id, rt] : runtimes) if (rt.ctrl) rt.ctrl->setArmed(false);
    sys.shutdown();
}


// ── Laser (DAC) list persistence ────────────────────────────────────────────
// See struct LaserConfig's comment (near G_lasers) for the data model. Same
// brace-matching array parsing technique as load_midi_map() below - this is
// our own file format (produced only by save_lasers_to_disk), so a full
// JSON parser isn't needed.
static const std::string LASERS_FILE = resolve_data_file("lasers.json");

static std::string laser_config_to_json(const LaserConfig& l) {
    std::ostringstream ss; ss << std::fixed << std::setprecision(4);
    ss << "{\"id\":" << l.id
       << ",\"name\":\"" << l.name << "\""
       << ",\"ip\":\"" << l.ip << "\""
       << ",\"armed\":" << (l.armed ? "true" : "false")
       << ",\"zone_x\":" << l.zone_x
       << ",\"zone_y\":" << l.zone_y
       << ",\"zone_scale_x\":" << l.zone_scale_x
       << ",\"zone_scale_y\":" << l.zone_scale_y
       << ",\"mirror_x\":" << (l.mirror_x ? "true" : "false")
       << ",\"mirror_y\":" << (l.mirror_y ? "true" : "false")
         << ",\"preview_x\":" << l.preview_x
         << ",\"preview_y\":" << l.preview_y
       << ",\"masks\":" << masks_to_json(l.masks, true)
       << ",\"connected\":" << (get_laser_connected(l.id) ? "true" : "false")
       << "}";
    return ss.str();
}

static void save_lasers_to_disk() {
    std::lock_guard<std::mutex> lk(G_lasers_mtx);
    std::ostringstream ss; ss << std::fixed << std::setprecision(4) << "[";
    for (std::size_t i = 0; i < G_lasers.size(); ++i) {
        if (i) ss << ",";
        const auto& l = G_lasers[i];
        ss << "{\"id\":" << l.id << ",\"name\":\"" << l.name << "\",\"ip\":\"" << l.ip
           << "\",\"armed\":" << (l.armed ? "true" : "false")
           << ",\"zone_x\":" << l.zone_x << ",\"zone_y\":" << l.zone_y
           << ",\"zone_scale_x\":" << l.zone_scale_x << ",\"zone_scale_y\":" << l.zone_scale_y
           << ",\"mirror_x\":" << (l.mirror_x ? "true" : "false")
           << ",\"mirror_y\":" << (l.mirror_y ? "true" : "false")
           << ",\"preview_x\":" << l.preview_x << ",\"preview_y\":" << l.preview_y
           << ",\"masks\":" << masks_to_json(l.masks, false)
           << "}";
    }
    ss << "]";
    std::ofstream f(LASERS_FILE, std::ios::trunc);
    if (f) f << ss.str();
}

static void load_lasers_from_disk() {
    std::ifstream f(LASERS_FILE);
    if (!f) { save_lasers_to_disk(); return; } // no file yet - empty list is a fine default
    std::ostringstream buf; buf << f.rdbuf();
    std::string body = buf.str();

    bool migrated = false;
    {
        std::lock_guard<std::mutex> lk(G_lasers_mtx);
        G_lasers.clear();
        std::size_t i = body.find('{');
        while (i != std::string::npos) {
            int depth = 0; std::size_t j = i;
            for (; j < body.size(); ++j) {
                if (body[j] == '{') ++depth;
                else if (body[j] == '}') { --depth; if (depth == 0) { ++j; break; } }
            }
            if (depth != 0) break;
            std::string obj = body.substr(i, j - i);
            LaserConfig l;
            // Masks first, then cut their array out so the laser's own keys
            // below can never be matched inside a nested mask object.
            std::size_t mb = 0, me = 0;
            if (parse_masks(obj, l.masks, &mb, &me)) obj.erase(mb, me - mb);
            else if (obj.find("\"masks\"") != std::string::npos)
                std::cerr << "[laser_daemon] WARNING: unreadable safety masks for laser entry in "
                          << LASERS_FILE << " - masks NOT loaded, check the Zoning panel!\n";
            l.id            = json_int(obj, "id", 0);
            l.name          = json_str(obj, "name");
            l.ip            = json_str(obj, "ip");
            l.armed         = json_bool(obj, "armed", false);
            l.zone_x        = json_float(obj, "zone_x", 0.0f);
            l.zone_y        = json_float(obj, "zone_y", 0.0f);
            l.zone_scale_x  = json_float(obj, "zone_scale_x", 1.0f);
            l.zone_scale_y  = json_float(obj, "zone_scale_y", 1.0f);
            l.mirror_x      = json_bool(obj, "mirror_x", false);
            l.mirror_y      = json_bool(obj, "mirror_y", false);
            const bool hasPreviewPosition = obj.find("\"preview_x\"") != std::string::npos
                && obj.find("\"preview_y\"") != std::string::npos;
            l.preview_x     = json_float(obj, "preview_x", -1.0f);
            l.preview_y     = json_float(obj, "preview_y", -1.0f);
            if (l.id > 0) {
                G_lasers.push_back(l);
                G_next_laser_id = std::max(G_next_laser_id, l.id + 1);
                migrated = migrated || !hasPreviewPosition;
            }
            i = body.find('{', j);
        }

        const float spacing = 1.0f / (G_lasers.size() + 1);
        for (std::size_t index = 0; index < G_lasers.size(); ++index) {
            auto& laser = G_lasers[index];
            if (laser.preview_x < 0.0f || laser.preview_y < 0.0f) {
                laser.preview_x = spacing * (index + 1);
                laser.preview_y = 0.5f;
                migrated = true;
            }
        }
    }
    if (migrated) save_lasers_to_disk();
    std::cout << "[laser_daemon] Loaded " << G_lasers.size() << " laser(s) from " << LASERS_FILE << "\n";
}

// ── Cue save/recall ──────────────────────────────────────────────────────────
// A cue is a saved snapshot of every show parameter (shape, color, transform,
// movement, wave, rainbow, FX) — NOT the controller connections (see the
// G_lasers list above) or their zone calibrations, so recalling a cue never
// drops/changes the current DAC links or their venue alignment. Persisted to
// a small JSON file on disk so cues survive a daemon restart, mirroring the
// cue system from the original BeamCommander (Python edition).
// Must match the frontend's CUE_COUNT (CuePanel.vue, 16x8 grid) - anything
// higher there would be silently rejected by do_cue_save/recall/clear/move
// below (they all range-check against MAX_CUES).
static constexpr int MAX_CUES = 128;
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
    << "\"move_phase_degrees\":" << s.move_phase_degrees << ","
       << "\"wave_frequency\":" << s.wave_frequency << ","
       << "\"wave_amplitude\":" << s.wave_amplitude << ","
       << "\"wave_speed\":"    << s.wave_speed    << ","
       << "\"rainbow_amount\":" << s.rainbow_amount << ","
       << "\"rainbow_speed\":" << s.rainbow_speed << ","
       << "\"rainbow_phase_degrees\":" << s.rainbow_phase_degrees << ","
       << "\"dot_amount\":"    << s.dot_amount    << ","
       << "\"flicker_hz\":"    << s.flicker_hz    << ","
       << "\"flicker_on_ms\":" << s.flicker_on_ms << ","
       << "\"flicker_phase_degrees\":" << s.flicker_phase_degrees
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
    s.move_phase_degrees = json_float(obj,"move_phase_degrees",s.move_phase_degrees);
    s.wave_frequency  = json_float(obj,"wave_frequency",s.wave_frequency);
    s.wave_amplitude  = json_float(obj,"wave_amplitude",s.wave_amplitude);
    s.wave_speed      = json_float(obj,"wave_speed",s.wave_speed);
    s.rainbow_amount  = json_float(obj,"rainbow_amount",s.rainbow_amount);
    s.rainbow_speed   = json_float(obj,"rainbow_speed",s.rainbow_speed);
    s.rainbow_phase_degrees = json_float(obj,"rainbow_phase_degrees",s.rainbow_phase_degrees);
    s.dot_amount      = json_float(obj,"dot_amount",s.dot_amount);
    s.flicker_hz      = json_float(obj,"flicker_hz",s.flicker_hz);
    // Cues saved before the on time existed have no such key: keep the
    // default 0 (= auto, half the period), i.e. exactly how they looked.
    s.flicker_on_ms   = std::clamp(json_float(obj,"flicker_on_ms",0.0f),0.0f,kFlickerOnMsMax);
    s.flicker_phase_degrees = std::clamp(json_float(obj,"flicker_phase_degrees",0.0f),0.0f,360.0f);
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
    snap.rotation_phase = current_rotation_angle();
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
        // Global daemon settings aren't part of the per-cue show state -
        // keep whatever they're currently set to across the recall.
        G = with_global_settings(snap, G, /*keepIntensity=*/true);
        G_show_snap_seq.fetch_add(1); // switch instantly, no morphing
        // Start at the rotation angle the cue was saved at (see do_cue_save),
        // with the cue's own rotation speed - not turned further by the
        // previous look's rotation.
        request_rotation_angle(snap.rotation_phase);
    }
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
    if (action=="flicker_on_ms")  { std::lock_guard<std::mutex> lk(G_mtx); G.flicker_on_ms=std::clamp(out,0.0f,kFlickerOnMsMax); return; }
    if (action=="flicker_phase_degrees") { std::lock_guard<std::mutex> lk(G_mtx); G.flicker_phase_degrees=std::clamp(out,0.0f,360.0f); return; }
    if (action=="wave_frequency") { std::lock_guard<std::mutex> lk(G_mtx); G.wave_frequency=std::max(0.1f,out); return; }
    if (action=="wave_amplitude") { std::lock_guard<std::mutex> lk(G_mtx); G.wave_amplitude=std::clamp(out,0.0f,1.0f); return; }
    if (action=="wave_speed")     { std::lock_guard<std::mutex> lk(G_mtx); G.wave_speed=out; return; }
    if (action=="rainbow_amount") { std::lock_guard<std::mutex> lk(G_mtx); G.rainbow_amount=std::clamp(out,0.0f,1.0f); return; }
    if (action=="rainbow_speed")  { std::lock_guard<std::mutex> lk(G_mtx); G.rainbow_speed=out; return; }
    if (action=="rainbow_phase_degrees") { std::lock_guard<std::mutex> lk(G_mtx); G.rainbow_phase_degrees=std::clamp(out,0.0f,360.0f); return; }
    if (action=="move_size")      { std::lock_guard<std::mutex> lk(G_mtx); G.move_size=std::clamp(out,0.0f,6.0f); return; }
    if (action=="move_speed")     { std::lock_guard<std::mutex> lk(G_mtx); G.move_speed=std::max(0.0f,out); return; }
    if (action=="move_phase_degrees") { std::lock_guard<std::mutex> lk(G_mtx); G.move_phase_degrees=std::clamp(out,0.0f,360.0f); return; }
    if (action=="rate_kpps")      { std::lock_guard<std::mutex> lk(G_mtx); G.rate_kpps=std::clamp(out,0.0f,G.max_rate_kpps); return; }
}

// State for the momentary (hold-to-preview) cue buttons and the momentary
// flash button - both need to remember what to restore on release.
static bool      g_midi_cue_save_armed = false;
static LaserState g_cue_momentary_snapshot;
static double     g_cue_momentary_angle = 0.0;
static bool       g_cue_momentary_active = false;
// Guards the three momentary fields above (MIDI thread + HTTP threads).
// Lock order: always taken *before* G_mtx / G_cues_mtx, never inside them.
static std::mutex g_cue_momentary_mtx;

// Momentary cue (active only while held) - shared by the MIDI
// "cue_momentary:<n>" action and POST /api/cue/<n>/momentary/<0|1>: press
// remembers the current look *and rotation angle* and recalls cue n,
// release puts both back. Global settings (master brightness, blackout,
// release time, max rate) aren't part of a recall and stay untouched.
static bool do_cue_momentary_press(int n) {
    std::lock_guard<std::mutex> mlk(g_cue_momentary_mtx);
    if (!g_cue_momentary_active) {
        { std::lock_guard<std::mutex> lk(G_mtx); g_cue_momentary_snapshot = G; }
        g_cue_momentary_angle = current_rotation_angle();
    }
    if (!do_cue_recall(n)) return false;
    g_cue_momentary_active = true;
    return true;
}
static void do_cue_momentary_release() {
    std::lock_guard<std::mutex> mlk(g_cue_momentary_mtx);
    if (!g_cue_momentary_active) return;
    std::lock_guard<std::mutex> lk(G_mtx);
    G = with_global_settings(g_cue_momentary_snapshot, G, /*keepIntensity=*/true);
    G_show_snap_seq.fetch_add(1); // back instantly, no morphing
    request_rotation_angle(g_cue_momentary_angle);
    g_cue_momentary_active = false;
}
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
    G_speed_snap_seq.fetch_add(1); // freeze instantly, don't glide to a stop
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
    std::lock_guard<std::mutex> lk(G_mtx);
    G.rotation_speed = 0.0f;
    request_rotation_angle(0.0); // angle 0, stop instantly (no glide)
}

// Applies one note-triggered (button) action. `isPress`/`isRelease`
// Shared by the "color:<name>" note action and sync_group_leds()'s color
// LED sync (see its comment) - single source of truth for the named color
// palette so both always agree on what RGB each name means.
static bool named_color_rgb(const std::string& name, float& r, float& g, float& b) {
    if      (name=="red")     { r=1.0f; g=0.0f;  b=0.0f; }
    else if (name=="orange")  { r=1.0f; g=0.4f;  b=0.0f; }
    else if (name=="yellow")  { r=1.0f; g=1.0f;  b=0.0f; }
    else if (name=="green")   { r=0.0f; g=1.0f;  b=0.0f; }
    else if (name=="cyan")    { r=0.0f; g=1.0f;  b=1.0f; }
    else if (name=="blue")    { r=0.0f; g=0.2f;  b=1.0f; }
    else if (name=="magenta") { r=1.0f; g=0.0f;  b=1.0f; }
    else if (name=="white")   { r=1.0f; g=1.0f;  b=1.0f; }
    else return false;
    return true;
}

// describe note-on/note-off.
static void midi_apply_note_action(const std::string& action, bool isPress, bool isRelease) {
    if (!isPress && !isRelease) return;

    if (action.rfind("shape:",0)==0) {
        // Fires on every message for this note (both isPress and
        // isRelease), not just isPress - like laser_mirror_toggle/
        // rotation_reset, these grid pads alternate note-on/note-off
        // across successive physical taps rather than an on+off pair per
        // tap. Setting the same shape twice is idempotent, so reacting to
        // both is safe.
        static const std::set<std::string> SHAPES{"circle","line","triangle","square","wave","staticwave"};
        std::string s = action.substr(6);
        if (SHAPES.count(s)) { std::lock_guard<std::mutex> lk(G_mtx); G.shape=s; }
        return;
    }
    if (action.rfind("move:",0)==0) {
        // See shape:'s comment above - same alternating-LED pad behavior.
        static const std::set<std::string> MOVES{"none","circle","pan","tilt","tiltup","tiltdown","eight","random"};
        std::string m = action.substr(5);
        if (MOVES.count(m)) { std::lock_guard<std::mutex> lk(G_mtx); G.move_mode=m; }
        return;
    }
    if (action.rfind("color:",0)==0) {
        // See shape:'s comment above - same alternating-LED pad behavior.
        std::string c = action.substr(6);
        float cr,cg,cb;
        if (!named_color_rgb(c, cr, cg, cb)) return;
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
    // Per-laser hardware-output on/off toggle (e.g. APC40 mkII Activator
    // buttons, one per channel strip - note 52, channel = 0-based track
    // index). "laser_toggle:<n>" toggles G_lasers[n-1].armed (1-based, in
    // on-screen laser order) - this only gates real hardware output
    // (laser_thread()'s connect/stream loop); the preview hides a
    // disarmed laser's beam, matching its dark real output.
    if (action.rfind("laser_toggle:",0)==0) {
        if (!isPress) return; // toggle once per press, ignore release
        int n = 0; try { n = std::stoi(action.substr(13)); } catch (...) { return; }
        {
            std::lock_guard<std::mutex> lk(G_lasers_mtx);
            int idx = n - 1;
            if (idx < 0 || idx >= (int)G_lasers.size()) return;
            G_lasers[idx].armed = !G_lasers[idx].armed;
            std::cout << "[midi] laser " << n << " (" << G_lasers[idx].name << ") "
                      << (G_lasers[idx].armed ? "armed" : "disarmed") << "\n" << std::flush;
        }
        save_lasers_to_disk();
        return;
    }
    // Per-laser hardware-output X-mirror toggle (e.g. another APC40 mkII
    // button row, one per channel strip). "laser_mirror_toggle:<n>" flips
    // G_lasers[n-1].mirror_x (1-based, in on-screen laser order) - like
    // laser_toggle: above, this only affects that laser's real hardware
    // output (see laser_thread()'s per-laser zone-transform step), never
    // the browser preview. Fires on *every* MIDI message for this note
    // (both isPress and isRelease), not just isPress - matches
    // rotation_reset's comment below: this pad alternates note-on/note-off
    // across successive physical taps rather than sending an on+off pair
    // per tap, so gating on isPress only made it toggle on just every
    // other press.
    // "laser_mirror_y_toggle:<n>" does the same for the vertical axis (mirror_y).
    const bool mirrorXToggle = action.rfind("laser_mirror_toggle:",0)==0;
    const bool mirrorYToggle = action.rfind("laser_mirror_y_toggle:",0)==0;
    if (mirrorXToggle || mirrorYToggle) {
        int n = 0; try { n = std::stoi(action.substr(action.find(':') + 1)); } catch (...) { return; }
        {
            std::lock_guard<std::mutex> lk(G_lasers_mtx);
            int idx = n - 1;
            if (idx < 0 || idx >= (int)G_lasers.size()) return;
            bool& flag = mirrorXToggle ? G_lasers[idx].mirror_x : G_lasers[idx].mirror_y;
            flag = !flag;
            std::cout << "[midi] laser " << n << " (" << G_lasers[idx].name << ") "
                      << (mirrorXToggle ? "mirror_x " : "mirror_y ") << (flag ? "on" : "off") << "\n" << std::flush;
        }
        save_lasers_to_disk();
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
            if (do_cue_momentary_press(n)) std::cout << "[midi] previewing cue " << n << "\n";
        } else if (isRelease) {
            do_cue_momentary_release();
            std::cout << "[midi] cue " << n << " preview released\n";
        }
        return;
    }
}

#ifdef __APPLE__
#include <CoreMIDI/CoreMIDI.h>

static MIDIClientRef   g_midi_client   = 0;
static MIDIPortRef     g_midi_in_port  = 0;
static MIDIPortRef     g_midi_out_port = 0;
static MIDIEndpointRef g_midi_out_dest = 0; // 0 = no output destination found yet
static std::string     g_midi_out_dest_name;
static bool            g_midi_led_wait_logged = false;

// Sends a Note On (velocity>0) or Note Off (velocity 0) for (channel, note)
// back to the controller, if an output destination has been found (see
// scan() in midi_thread()). Used to override the APC40 mkII's own firmware
// LED behavior for buttons we've repurposed (e.g. the "Solo" row, which the
// controller lights up blue on press regardless of what the host does with
// the incoming note) - the controller listens to its own MIDI *output* port
// for note messages to set that same button's LED state, so echoing a note
// back lets us force the LED to reflect our own toggle state instead
// (lit while active, off otherwise) rather than the controller's default
// light-on-press-only behavior.
static bool send_note(unsigned char channel, unsigned char note, unsigned char velocity) {
    if (!g_midi_out_port || !g_midi_out_dest) {
        if (!g_midi_led_wait_logged) {
            std::cout << "[midi-led] send skipped: output port/destination unavailable (port="
                      << (g_midi_out_port ? "yes" : "no") << ", destination="
                      << (g_midi_out_dest ? "yes" : "no") << ")\n" << std::flush;
            g_midi_led_wait_logged = true;
        }
        return false;
    }
    Byte msg[3] = { (Byte)((velocity > 0 ? 0x90 : 0x80) | (channel & 0x0F)), note, velocity };
    Byte buffer[64];
    MIDIPacketList* pktlist = reinterpret_cast<MIDIPacketList*>(buffer);
    MIDIPacket* packet = MIDIPacketListInit(pktlist);
    packet = MIDIPacketListAdd(pktlist, sizeof(buffer), packet, 0, 3, msg);
    if (!packet) {
        std::cout << "[midi-led] packet allocation failed for ch=" << (int)channel
                  << " note=" << (int)note << " velocity=" << (int)velocity << "\n" << std::flush;
        return false;
    }
    const OSStatus status = MIDISend(g_midi_out_port, g_midi_out_dest, pktlist);
    std::cout << "[midi-led] send ch=" << (int)channel << " note=" << (int)note
              << " velocity=" << (int)velocity << " destination='" << g_midi_out_dest_name
              << "' status=" << status << "\n" << std::flush;
    return status == noErr;
}

// Forward-declared (defined further down, just before midi_thread()) so
// midi_read_proc() below can call it immediately after a shape:/move:/
// color:/rainbow_preset_slowfull message instead of waiting for its own
// periodic timer tick (up to 200ms later) - these grid pads apparently
// self-illuminate briefly the instant they're physically pressed
// (independent of anything we send), so asserting the real color as fast
// as possible after that minimizes the visible "wrong color" flash/blink
// window instead of leaving it lit with stale state until the next tick.
// forceChannel/forceNumber (both -1 by default) name one specific pad that
// must always be resent even if its computed velocity hasn't changed since
// the last send - needed because a physical tap on that exact pad appears
// to reset/re-derive its own LED state locally on this hardware, so the
// normal "only send on change" dedup (see sync_group_leds()) would
// otherwise leave it dark right after the very tap that selected it,
// while the same selection made via REST/UI (which always sees a 0->127
// change the first time) stays lit correctly.
static void sync_shape_color_move_leds(int forceChannel = -1, int forceNumber = -1);

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
                        std::cout << "[midi] note ch=" << (int)channel << " num=" << (int)d1
                                  << " " << (isPress ? "press" : "release")
                                  << " -> " << match.action << "\n" << std::flush;
                        midi_apply_note_action(match.action, isPress, isRelease);
                        // Assert the correct LED color immediately for
                        // these groups too (see sync_shape_color_move_leds()'s
                        // forward-declaration comment above) instead of
                        // waiting for the next periodic timer tick. Force
                        // the exact pad that was just tapped (channel, d1)
                        // to be resent even if its velocity didn't change,
                        // since a physical tap on this hardware can locally
                        // reset that pad's own LED regardless of anything
                        // we'd already sent it before.
                        if (match.action.rfind("shape:",0)==0 || match.action.rfind("move:",0)==0 ||
                            match.action.rfind("color:",0)==0 || match.action=="rainbow_preset_slowfull")
                            sync_shape_color_move_leds(channel, d1);
                        // These buttons default to a "Solo"-style blue LED
                        // on the controller itself (lit only while held) -
                        // not meaningful for this app's own use of them, so
                        // override it right after each message to instead
                        // reflect our own toggle state: lit while that
                        // laser's x-axis is inverted, off otherwise (see
                        // send_note()'s comment). Fires on every message
                        // (not just isPress), matching
                        // midi_apply_note_action's laser_mirror_toggle
                        // handling above - this pad alternates note-on/
                        // note-off per tap rather than a press+release
                        // pair per tap.
                        if (match.action.rfind("laser_mirror_toggle:",0)==0) {
                            int n = 0; try { n = std::stoi(match.action.substr(20)); } catch (...) { n = 0; }
                            bool mirrored = false;
                            if (n > 0) {
                                std::lock_guard<std::mutex> lk(G_lasers_mtx);
                                int idx = n - 1;
                                if (idx >= 0 && idx < (int)G_lasers.size()) mirrored = G_lasers[idx].mirror_x;
                            }
                            send_note(channel, d1, mirrored ? 127 : 0);
                        }
                        if (match.action.rfind("laser_toggle:",0)==0) {
                            int n = 0; try { n = std::stoi(match.action.substr(13)); } catch (...) { n = 0; }
                            bool armed = false;
                            if (n > 0) {
                                std::lock_guard<std::mutex> lk(G_lasers_mtx);
                                const int idx = n - 1;
                                if (idx >= 0 && idx < static_cast<int>(G_lasers.size())) armed = G_lasers[idx].armed;
                            }
                            send_note(channel, d1, armed ? 127 : 0);
                        }
                    } else {
                        midi_apply_cc_action(match.action, midi_cc_value(match, d2, channel));
                    }
                } else {
                    std::cout << "[midi] unmapped " << (isNote ? "note" : "cc")
                              << " ch=" << (int)channel << " num=" << (int)d1
                              << " val=" << (int)d2 << "\n" << std::flush;
                }
            } else {
                // Any other MIDI message type (e.g. program change, sysex,
                // pitch bend) - log it raw so unexpected controller modes
                // (e.g. APC40 not in the expected mode) are visible instead
                // of silently doing nothing.
                std::cout << "[midi] other status=0x" << std::hex << (int)status
                          << std::dec << " d1=" << (int)d1 << " d2=" << (int)d2
                          << "\n" << std::flush;
            }
        }
        packet = MIDIPacketNext(packet);
    }
}

// Runs on its own thread with an active CFRunLoop (required for CoreMIDI to
// deliver read-proc callbacks). Periodically rescans for newly-connected
// sources so plugging in the controller after the daemon starts still works.

// Keeps each laser's "S" button LED in sync with its mirror_x state even
// when that state changes via the REST API/web UI rather than the MIDI
// button itself - those changes never arrive as an incoming MIDI message,
// so midi_read_proc()'s LED echo (which only fires when it sees a
// laser_mirror_toggle message come in) can't react to them. Instead this
// polls G_lasers periodically (see the dedicated CFRunLoopTimer in
// midi_thread() below) and sends a Note On/Off for any laser whose
// mirror_x has changed since the last check. Only sends on a detected
// change (not unconditionally every tick) - repeatedly sending the same
// Note On/Off re-triggers the pad's own attack/flash animation on this
// controller even when the color doesn't change, which looked like
// constant blinking.
static std::map<int, bool> g_last_mirror_led; // keyed by laser's 1-based position (matches laser_mirror_toggle:<n>)
static std::map<int, bool> g_last_armed_led;
static void sync_mirror_leds() {
    std::vector<std::pair<int, bool>> current; // (1-based position, mirror_x)
    {
        std::lock_guard<std::mutex> lk(G_lasers_mtx);
        for (std::size_t i = 0; i < G_lasers.size(); ++i)
            current.emplace_back((int)i + 1, G_lasers[i].mirror_x);
    }
    for (auto& [n, mirrored] : current) {
        auto it = g_last_mirror_led.find(n);
        if (it != g_last_mirror_led.end() && it->second == mirrored) continue;
        std::string action = "laser_mirror_toggle:" + std::to_string(n);
        std::lock_guard<std::mutex> lk(G_midi_mtx);
        for (auto& b : G_midi_bindings) {
            if (b.type == "note" && b.action == action && b.channel >= 0 &&
                send_note((unsigned char)b.channel, (unsigned char)b.number, mirrored ? 127 : 0))
                g_last_mirror_led[n] = mirrored;
        }
    }
}

static void sync_armed_leds() {
    std::vector<std::pair<int, bool>> current;
    {
        std::lock_guard<std::mutex> lk(G_lasers_mtx);
        for (std::size_t i = 0; i < G_lasers.size(); ++i)
            current.emplace_back(static_cast<int>(i) + 1, G_lasers[i].armed);
    }
    std::vector<MidiBinding> bindings;
    { std::lock_guard<std::mutex> lk(G_midi_mtx); bindings = G_midi_bindings; }
    for (const auto& [n, armed] : current) {
        auto it = g_last_armed_led.find(n);
        if (it != g_last_armed_led.end() && it->second == armed) continue;
        const std::string action = "laser_toggle:" + std::to_string(n);
        bool sent = false;
        for (const auto& b : bindings) {
            if (b.type == "note" && b.action == action && b.channel >= 0)
                sent = send_note(static_cast<unsigned char>(b.channel), static_cast<unsigned char>(b.number), armed ? 127 : 0) || sent;
        }
        if (sent) g_last_armed_led[n] = armed;
    }
}

// Generalized version of the same idea as sync_mirror_leds(), for a group
// of mutually-exclusive "pick one" note buttons (shape:<x>, move:<x>,
// color:<x>) - keeps each button's LED reflecting whether its choice is
// the currently active one, regardless of what changed the active choice
// (this same MIDI button, a different one in the group, the REST API,
// etc.). `prefix` selects which bindings to consider (e.g. "shape:") and
// `velocityFor` computes the exact velocity/color-palette-index (0-127) to
// send for a given binding's full action string (e.g. "shape:circle") -
// these buttons sit on the APC40 mkII's 5x8 RGB clip-launch grid (notes
// 0-39), which supports a full 128-entry color palette via the Note On
// velocity byte (reverse-engineered from doc/APC40 colour codes.webp by
// sampling its swatch pixels), not just plain on/off. Only sends on a
// detected per-pad change, same reasoning as sync_mirror_leds() above -
// resending an unchanged velocity every tick made the pad visibly blink.
// forceChannel/forceNumber (both -1 = no force) name one exact (channel,
// note) pad that must be resent unconditionally, bypassing the dedup - see
// sync_shape_color_move_leds()'s forward-declaration comment for why.
static std::map<std::pair<int,int>, int> g_last_group_led; // keyed by (channel, note) -> last sent velocity
static void sync_group_leds(const std::string& prefix, const std::function<int(const std::string&)>& velocityFor,
                            int forceChannel = -1, int forceNumber = -1) {
    std::vector<MidiBinding> bindings;
    { std::lock_guard<std::mutex> lk(G_midi_mtx); bindings = G_midi_bindings; }
    for (auto& b : bindings) {
        if (b.type != "note" || b.channel < 0 || b.action.rfind(prefix, 0) != 0) continue;
        int velocity = velocityFor(b.action);
        auto key = std::make_pair(b.channel, b.number);
        bool forced = (b.channel == forceChannel && b.number == forceNumber);
        auto it = g_last_group_led.find(key);
        if (!forced && it != g_last_group_led.end() && it->second == velocity) continue;
        if (send_note((unsigned char)b.channel, (unsigned char)b.number, (unsigned char)velocity))
            g_last_group_led[key] = velocity;
    }
}

// Concrete shape/move/color instances of sync_group_leds() - called
// together from the same periodic LED-sync timer as sync_mirror_leds()
// (see midi_thread() below). Simple on/off (127/0), same as the mirror/
// laser-toggle buttons - a fancier per-hue/dim-when-inactive palette
// scheme was tried and reverted (see git history) since it caused visible
// blinking on this hardware.
static void sync_shape_color_move_leds(int forceChannel, int forceNumber) {
    LaserState snap; { std::lock_guard<std::mutex> lk(G_mtx); snap = G; }

    sync_group_leds("shape:", [&](const std::string& action) {
        return (action.substr(6) == snap.shape) ? 127 : 0;
    }, forceChannel, forceNumber);
    sync_group_leds("move:", [&](const std::string& action) {
        return (action.substr(5) == snap.move_mode) ? 127 : 0;
    }, forceChannel, forceNumber);
    bool rainbowActive = snap.rainbow_amount > 0.0f;
    sync_group_leds("rainbow_preset_slowfull", [&](const std::string&) {
        return rainbowActive ? 127 : 0;
    }, forceChannel, forceNumber);
    sync_group_leds("color:", [&](const std::string& action) {
        float cr, cg, cb;
        if (!named_color_rgb(action.substr(6), cr, cg, cb)) return 0;
        const float eps = 0.02f;
        bool active = std::fabs(snap.r - cr) < eps && std::fabs(snap.g - cg) < eps && std::fabs(snap.b - cb) < eps;
        return active ? 127 : 0;
    }, forceChannel, forceNumber);
}

static void find_midi_output_destination() {
    if (!g_midi_out_port) return;
    const ItemCount destinationCount = MIDIGetNumberOfDestinations();
    const ItemCount sourceCount = MIDIGetNumberOfSources();
    static bool waitingLogged = false;
    MIDIEndpointRef bestDestination = 0;
    std::string bestName;
    int bestScore = -1;
    for (ItemCount i = 0; i < sourceCount; ++i) {
        MIDIEndpointRef source = MIDIGetSource(i);
        CFStringRef sourceNameRef = nullptr;
        MIDIObjectGetStringProperty(source, kMIDIPropertyDisplayName, &sourceNameRef);
        char sourceName[256] = "?";
        if (sourceNameRef) {
            CFStringGetCString(sourceNameRef, sourceName, sizeof(sourceName), kCFStringEncodingUTF8);
            CFRelease(sourceNameRef);
        }
        const std::string inputName(sourceName);
        if (inputName.find("IAC") != std::string::npos) continue;
        for (ItemCount j = 0; j < destinationCount; ++j) {
            MIDIEndpointRef destination = MIDIGetDestination(j);
            CFStringRef destinationNameRef = nullptr;
            MIDIObjectGetStringProperty(destination, kMIDIPropertyDisplayName, &destinationNameRef);
            char destinationName[256] = "?";
            if (destinationNameRef) {
                CFStringGetCString(destinationNameRef, destinationName, sizeof(destinationName), kCFStringEncodingUTF8);
                CFRelease(destinationNameRef);
            }
            const std::string outputName(destinationName);
            if (outputName != inputName || outputName.find("IAC") != std::string::npos) continue;
            const int score = outputName.find("APC40") != std::string::npos ? 100 : 10;
            if (score > bestScore) {
                bestScore = score;
                bestDestination = destination;
                bestName = outputName;
            }
        }
    }
    if (bestDestination) {
        const bool changed = g_midi_out_dest != bestDestination;
        g_midi_out_dest = bestDestination;
        g_midi_out_dest_name = bestName;
        g_midi_led_wait_logged = false;
        if (changed) {
            g_last_mirror_led.clear();
            g_last_armed_led.clear();
            g_last_group_led.clear();
            std::cout << "[midi] output destination selected: " << bestName
                      << (bestScore == 100 ? " (APC40 preferred)" : " (hardware fallback)")
                      << "\n" << std::flush;
        }
    } else {
        g_midi_out_dest = 0;
        g_midi_out_dest_name.clear();
        g_last_mirror_led.clear();
        g_last_armed_led.clear();
        g_last_group_led.clear();
    }
    if (!bestDestination && !waitingLogged) {
        std::cout << "[midi-led] matching output destination not found; APC LED feedback waiting ("
                  << sourceCount << " MIDI source(s), " << destinationCount << " destination(s))\n" << std::flush;
        waitingLogged = true;
    }
}

static void midi_thread() {
    load_midi_map(resolve_data_file("midi_map.json"));

    OSStatus st = MIDIClientCreate(CFSTR("BeamCommander3"), nullptr, nullptr, &g_midi_client);
    if (st != noErr) { std::cout << "[midi] MIDIClientCreate failed (" << st << ")\n"; return; }
    st = MIDIInputPortCreate(g_midi_client, CFSTR("BeamCommander3 In"), midi_read_proc, nullptr, &g_midi_in_port);
    if (st != noErr) { std::cout << "[midi] MIDIInputPortCreate failed (" << st << ")\n"; return; }
    st = MIDIOutputPortCreate(g_midi_client, CFSTR("BeamCommander3 Out"), &g_midi_out_port);
    if (st != noErr) std::cout << "[midi] MIDIOutputPortCreate failed (" << st << ") - LED feedback disabled\n";

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
        find_midi_output_destination();
    };
    scan();
    std::cout << "[midi] " << connected.size() << " source(s) connected"
                 " (will keep scanning for hot-plugged devices)\n" << std::flush;

    CFRunLoopTimerRef timer = CFRunLoopTimerCreateWithHandler(
        kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + 3, 3, 0, 0,
        ^(CFRunLoopTimerRef) { scan(); });
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopDefaultMode);

    // Separate, much faster timer for sync_mirror_leds()/
    // sync_shape_color_move_leds() (see their comments) so an API/UI/other-
    // button-driven state change reaches the physical LEDs promptly,
    // without waiting for (or being tied to) the 3s device-rescan cadence.
    CFRunLoopTimerRef ledTimer = CFRunLoopTimerCreateWithHandler(
        kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + 0.2, 0.2, 0, 0,
        ^(CFRunLoopTimerRef) { sync_mirror_leds(); sync_armed_leds(); sync_shape_color_move_leds(); });
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), ledTimer, kCFRunLoopDefaultMode);

    CFRunLoopRun(); // pumps MIDI + the rescan timer forever
}
#else
static void midi_thread() {
    std::cout << "[midi] CoreMIDI is only implemented on macOS - MIDI control disabled\n";
}
#endif

// ── Magic Touch layout (shared by all web clients) ─────────────────────────────
// The Magic Touch builder's pages/widgets live here instead of each
// browser's localStorage, so every connected client shows the same control
// surface. The backend treats the layout as an opaque JSON object owned by
// the frontend - it only checks it looks like an object and persists it
// verbatim. G_magic_rev changes on every save; clients poll
// GET /api/magic-touch?rev=<their rev> and only receive the full layout when
// it differs. It's seeded from the clock so a daemon restart can never
// reuse a revision number a client still holds.
static std::mutex        G_magic_mtx;
static std::string       G_magic_layout; // empty = nothing saved yet
static long long         G_magic_rev = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch()).count();
static const std::string MAGIC_FILE = resolve_data_file("magic_touch.json");
static constexpr std::size_t MAGIC_MAX_BYTES = 4 * 1024 * 1024;

static std::string trim_ws(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static void load_magic_from_disk() {
    std::ifstream f(MAGIC_FILE);
    if (!f) return;
    std::ostringstream buf; buf << f.rdbuf();
    std::string body = trim_ws(buf.str());
    if (body.size() >= 2 && body.front() == '{' && body.back() == '}') {
        std::lock_guard<std::mutex> lk(G_magic_mtx);
        G_magic_layout = body;
    }
}

// Writes via a temp file + rename so a crash mid-write can't corrupt the layout.
static void save_magic_to_disk(const std::string& body) {
    std::string tmp = MAGIC_FILE + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) return;
        f << body;
        if (!f) return;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, MAGIC_FILE, ec);
    if (ec) std::cerr << "[magic-touch] could not save " << MAGIC_FILE << ": " << ec.message() << "\n";
}

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
    load_lasers_from_disk();
    load_magic_from_disk();

    std::thread laser_t(laser_thread);
    std::thread midi_t(midi_thread);
    midi_t.detach(); // pumps a CFRunLoop forever; torn down on process exit
    httplib::Server svr;

    svr.set_default_headers({{"Access-Control-Allow-Origin","*"},{"Access-Control-Allow-Methods","GET,POST,DELETE,OPTIONS"},{"Access-Control-Allow-Headers","Content-Type"}});
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

    // ── POST /api/state — bulk update ──────────────────────────────────────────
    svr.Post("/api/state",[](const httplib::Request& req,httplib::Response& res){
        { // scope the lock so it's released before state_to_json() re-locks G_mtx
        std::lock_guard<std::mutex> lk(G_mtx);
        APPLY_S("shape",shape)  APPLY_S("move_mode",move_mode)
        APPLY_F("radius",radius)  APPLY_CLAMP("radius",radius,0,1)
        APPLY_F("max_rate_kpps",max_rate_kpps)  APPLY_CLAMP("max_rate_kpps",max_rate_kpps,1,100)
        APPLY_F("output_latency_ms",output_latency_ms) APPLY_CLAMP("output_latency_ms",output_latency_ms,10,200)
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
        APPLY_F("move_phase_degrees",move_phase_degrees) APPLY_CLAMP("move_phase_degrees",move_phase_degrees,0,360)
        APPLY_F("wave_frequency",wave_frequency)
        APPLY_F("wave_amplitude",wave_amplitude) APPLY_CLAMP("wave_amplitude",wave_amplitude,0,1)
        APPLY_F("wave_speed",wave_speed)
        APPLY_F("rainbow_amount",rainbow_amount) APPLY_CLAMP("rainbow_amount",rainbow_amount,0,1)
        APPLY_F("rainbow_speed",rainbow_speed)
        APPLY_F("rainbow_phase_degrees",rainbow_phase_degrees) APPLY_CLAMP("rainbow_phase_degrees",rainbow_phase_degrees,0,360)
        APPLY_F("dot_amount",dot_amount) APPLY_CLAMP("dot_amount",dot_amount,0,1)
        APPLY_F("flicker_hz",flicker_hz)
        APPLY_F("flicker_on_ms",flicker_on_ms) APPLY_CLAMP("flicker_on_ms",flicker_on_ms,0,kFlickerOnMsMax)
        APPLY_F("flicker_phase_degrees",flicker_phase_degrees) APPLY_CLAMP("flicker_phase_degrees",flicker_phase_degrees,0,360)
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
    static const std::set<std::string> MOVES{"none","circle","pan","tilt","tiltup","tiltdown","eight","random"};
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
    // ── POST /laser/rainbow/phase/<deg> ─ hue offset per laser, 0..360 ────────
    svr.Post(R"(/laser/rainbow/phase/([\d.]+))",[](const httplib::Request& req,httplib::Response& res){
        try{std::lock_guard<std::mutex> lk(G_mtx);G.rainbow_phase_degrees=std::clamp(std::stof(std::string(req.matches[1])),0.0f,360.0f);}catch(...){}
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
    // Preserves the global flash_release_ms / max_rate_kpps / output_latency_ms settings and
    // blackout (see their comments) so resetting the show doesn't change
    // the footswitch's fade time, change the venue's configured scan-rate
    // ceiling, or silently un-blank/blank the laser. The configured laser
    // list and each laser's arm state/zone calibration (G_lasers/
    // lasers.json) are entirely separate from LaserState and untouched by
    // a show reset.
    svr.Post("/api/reset",[](const httplib::Request&,httplib::Response& res){
        {
            std::lock_guard<std::mutex> lk(G_mtx);
            G = with_global_settings(LaserState{}, G, /*keepIntensity=*/false);
        }
        res.set_content(state_to_json(),"application/json");
    });

    // ── GET /api/lasers — list every configured laser (DAC) ─────────────────────
    svr.Get("/api/lasers",[](const httplib::Request&,httplib::Response& res){
        std::vector<LaserConfig> snapshot;
        { std::lock_guard<std::mutex> lk(G_lasers_mtx); snapshot = G_lasers; }
        std::ostringstream ss; ss << "[";
        for (std::size_t i=0;i<snapshot.size();++i) { if(i) ss<<","; ss<<laser_config_to_json(snapshot[i]); }
        ss << "]";
        res.set_content(ss.str(),"application/json");
    });

    // ── POST /api/lasers/order — persist installation order ───────────────────
    // Body: {"ids":[3,1,2]} - must contain every configured laser exactly once.
    svr.Post("/api/lasers/order",[](const httplib::Request& req,httplib::Response& res){
        std::vector<int> ids;
        if (!json_int_array(req.body, "ids", ids)) {
            res.status=400; res.set_content("{\"error\":\"invalid laser order\"}","application/json"); return;
        }
        bool valid = false;
        {
            std::lock_guard<std::mutex> lk(G_lasers_mtx);
            if (ids.size() == G_lasers.size()) {
                std::set<int> seen;
                std::vector<LaserConfig> reordered;
                reordered.reserve(ids.size());
                valid = true;
                for (int id : ids) {
                    auto it = std::find_if(G_lasers.begin(), G_lasers.end(),
                                           [&](const LaserConfig& l) { return l.id == id; });
                    if (it == G_lasers.end() || !seen.insert(id).second) { valid = false; break; }
                    reordered.push_back(*it);
                }
                if (valid) G_lasers = std::move(reordered);
            }
        }
        if (!valid) { res.status=400; res.set_content("{\"error\":\"order must contain every laser once\"}","application/json"); return; }
        save_lasers_to_disk();
        res.set_content("{\"ok\":true}","application/json");
    });

    // ── POST /api/lasers — add a new laser (DAC) ────────────────────────────────
    // Body: {"name":"...", "ip":"..."} - starts unarmed (configured but
    // idle) until explicitly armed via POST /api/lasers/<id>.
    svr.Post("/api/lasers",[](const httplib::Request& req,httplib::Response& res){
        LaserConfig l;
        l.ip = json_str(req.body,"ip");
        { std::lock_guard<std::mutex> lk(G_lasers_mtx);
          l.id = G_next_laser_id++;
          l.name = json_str(req.body,"name"); if (l.name.empty()) l.name = "Laser " + std::to_string(l.id);
                    l.preview_x = static_cast<float>(G_lasers.size() + 1) / static_cast<float>(G_lasers.size() + 2);
                    l.preview_y = 0.5f;
          G_lasers.push_back(l);
        }
        save_lasers_to_disk();
        res.set_content(laser_config_to_json(l),"application/json");
    });

    // ── POST /api/lasers/<id> — update name/ip/armed/zone calibration ──────────
    // Body: any of {"name", "ip", "armed":true|false, "mirror_x", "mirror_y", "zone_x", "zone_y",
    // "zone_scale_x", "zone_scale_y", "preview_x", "preview_y"} - only fields present in the body are
    // changed. Setting "armed":true is what actually makes laser_thread()
    // connect to and stream this laser (see laser_thread()'s connect loop);
    // false disconnects/idles it. zone_x/zone_y/zone_scale_x/zone_scale_y
    // are this laser's own venue pan/zoom calibration (see ZoningPanel.vue's
    // laser selector and LaserConfig's comment).
    svr.Post(R"(/api/lasers/(\d+))",[](const httplib::Request& req,httplib::Response& res){
        int id = std::stoi(std::string(req.matches[1]));
        bool found = false; LaserConfig updated;
        { std::lock_guard<std::mutex> lk(G_lasers_mtx);
          for (auto& l : G_lasers) {
              if (l.id != id) continue;
              auto nm = json_str(req.body,"name"); if (!nm.empty()) l.name = nm;
              if (req.body.find("\"ip\"") != std::string::npos) l.ip = json_str(req.body,"ip");
              if (req.body.find("\"armed\"") != std::string::npos) l.armed = json_bool(req.body,"armed",l.armed);
              if (req.body.find("\"mirror_x\"") != std::string::npos) l.mirror_x = json_bool(req.body,"mirror_x",l.mirror_x);
              if (req.body.find("\"mirror_y\"") != std::string::npos) l.mirror_y = json_bool(req.body,"mirror_y",l.mirror_y);
              l.zone_x       = json_float(req.body,"zone_x",l.zone_x);
              l.zone_y       = json_float(req.body,"zone_y",l.zone_y);
              l.zone_scale_x = std::clamp(json_float(req.body,"zone_scale_x",l.zone_scale_x),0.1f,2.0f);
              l.zone_scale_y = std::clamp(json_float(req.body,"zone_scale_y",l.zone_scale_y),0.1f,2.0f);
              l.preview_x    = std::clamp(json_float(req.body,"preview_x",l.preview_x),0.0f,1.0f);
              l.preview_y    = std::clamp(json_float(req.body,"preview_y",l.preview_y),0.0f,1.0f);
              updated = l; found = true; break;
          } }
        if (!found) { res.status=404; res.set_content("{\"error\":\"unknown laser id\"}","application/json"); return; }
        save_lasers_to_disk();
        res.set_content(laser_config_to_json(updated),"application/json");
    });

    // ── POST /api/lasers/<id>/masks — replace this laser's safety masks ───────
    // Body: {"masks":[{"x0","y0","x1","y1","reduce","outline"}, ...]} - the
    // complete list (add/edit/delete are all just a new list). Coordinates
    // are this laser's output space (-1..1, y up), reduce is 0..100 percent.
    // `outline` is runtime-only - see LaserMask.
    svr.Post(R"(/api/lasers/(\d+)/masks)",[](const httplib::Request& req,httplib::Response& res){
        int id = std::stoi(std::string(req.matches[1]));
        std::vector<LaserMask> masks;
        if (!parse_masks(req.body, masks)) {
            res.status=400; res.set_content("{\"error\":\"expected {\\\"masks\\\":[...]}\"}","application/json"); return;
        }
        bool found = false; LaserConfig updated;
        { std::lock_guard<std::mutex> lk(G_lasers_mtx);
          for (auto& l : G_lasers) {
              if (l.id != id) continue;
              l.masks = masks; updated = l; found = true; break;
          } }
        if (!found) { res.status=404; res.set_content("{\"error\":\"unknown laser id\"}","application/json"); return; }
        save_lasers_to_disk();
        res.set_content(laser_config_to_json(updated),"application/json");
    });

    // ── DELETE /api/lasers/<id> — remove a laser ────────────────────────────────
    svr.Delete(R"(/api/lasers/(\d+))",[](const httplib::Request& req,httplib::Response& res){
        int id = std::stoi(std::string(req.matches[1]));
        bool found = false;
        { std::lock_guard<std::mutex> lk(G_lasers_mtx);
          auto it = std::find_if(G_lasers.begin(), G_lasers.end(), [&](const LaserConfig& l){ return l.id==id; });
          if (it != G_lasers.end()) { G_lasers.erase(it); found = true; } }
        if (!found) { res.status=404; res.set_content("{\"error\":\"unknown laser id\"}","application/json"); return; }
        save_lasers_to_disk();
        res.set_content("{\"deleted\":"+std::to_string(id)+"}","application/json");
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
    svr.Post(R"(/api/cue/(\d+)/recall)",[](const httplib::Request& req,httplib::Response& res){
        int n=std::stoi(std::string(req.matches[1]));
        if(n<1||n>MAX_CUES){res.status=400;res.set_content("{\"error\":\"invalid cue number\"}","application/json");return;}
        if(!do_cue_recall(n)){res.status=404;res.set_content("{\"error\":\"empty cue\"}","application/json");return;}
        res.set_content(state_to_json(),"application/json");
    });

    // ── POST /api/cue/<n>/momentary/<1|0> — hold-to-preview a cue ──────────────
    // 1 = press (remember current look + rotation angle, recall cue n),
    // 0 = release (restore both). Shares do_cue_momentary_press/release with
    // the MIDI "cue_momentary:<n>" action.
    svr.Post(R"(/api/cue/(\d+)/momentary/([01]))",[](const httplib::Request& req,httplib::Response& res){
        int n=std::stoi(std::string(req.matches[1]));
        if(n<1||n>MAX_CUES){res.status=400;res.set_content("{\"error\":\"invalid cue number\"}","application/json");return;}
        if(req.matches[2]=="1"){
            if(!do_cue_momentary_press(n)){res.status=404;res.set_content("{\"error\":\"empty cue\"}","application/json");return;}
        } else do_cue_momentary_release();
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

    // ── GET /api/magic-touch[?rev=<n>] — shared Magic Touch layout ─────────────
    // Returns {"rev":<n>,"layout":<object|null>}. If ?rev matches the current
    // revision, "layout" is omitted (nothing changed) - keeps polling cheap.
    svr.Get("/api/magic-touch",[](const httplib::Request& req,httplib::Response& res){
        std::lock_guard<std::mutex> lk(G_magic_mtx);
        std::string rev = std::to_string(G_magic_rev);
        if (req.has_param("rev") && req.get_param_value("rev") == rev) {
            res.set_content("{\"rev\":"+rev+"}","application/json");
            return;
        }
        res.set_content("{\"rev\":"+rev+",\"layout\":"+(G_magic_layout.empty()?std::string("null"):G_magic_layout)+"}","application/json");
    });

    // ── POST /api/magic-touch — replace the shared layout (body: JSON object) ──
    svr.Post("/api/magic-touch",[](const httplib::Request& req,httplib::Response& res){
        std::string body = trim_ws(req.body);
        if (body.size() < 2 || body.front() != '{' || body.back() != '}' || body.size() > MAGIC_MAX_BYTES) {
            res.status=400; res.set_content("{\"error\":\"layout must be a JSON object\"}","application/json"); return;
        }
        long long rev;
        {
            std::lock_guard<std::mutex> lk(G_magic_mtx);
            G_magic_layout = body;
            rev = ++G_magic_rev;
            save_magic_to_disk(body);
        }
        res.set_content("{\"rev\":"+std::to_string(rev)+"}","application/json");
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
              << "  POST /move/mode/<none|circle|pan|tilt|tiltup|tiltdown|eight|random>\n"
              << "  POST /laser/rainbow/amount/<v>  /laser/rainbow/speed/<v>\n"
              << "  POST /blackout/<0|1>  /flash/<0|1>  /mirror/x/<0|1>  /motion/hold/<0|1>\n"
              << "  POST /brightness/gate/<0|1>  /rotation/reset\n"
              << "  GET /api/lasers   POST /api/lasers   POST /api/lasers/<id>   POST /api/lasers/<id>/masks   DELETE /api/lasers/<id>\n"
              << "  GET/POST /api/magic-touch   (shared Magic Touch layout)\n"
              << "  WS   /ws/points\n"
              << "  MIDI: optional, see backend/midi_map.json (Akai APC40 mkII or any USB controller)\n";

    std::thread stop_t([&](){while(G_running)std::this_thread::sleep_for(100ms);svr.stop();});
    svr.listen("0.0.0.0",http_port);
    stop_t.join(); laser_t.join();
    return 0;
}
