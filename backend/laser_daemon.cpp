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
#include "test_patterns.h"   // official ILDA test patterns + ILDA file reader (unit-tested, see backend/tests/)
#include "laser_text.h"      // text in single-stroke Hershey fonts (unit-tested, see backend/tests/)

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
#include <optional>
#include <memory>
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

// ── ILDA files on cues ─────────────────────────────────────────────────────────
// Files dropped onto a cue slot (POST /api/cue/<n>/ilda) are stored in the
// user data dir's ilda/ folder under a content-hash name (ilda_file.h), and
// cues refer to them by that name. Shipped like the other user data: the
// release copies backend/ilda/ next to the binary (or into Resources/), and
// seed_ilda_files() copies any missing file into the data dir at startup.
// Parsed files are cached (shared, read-only) - the laser thread only ever
// reads from disk the first time a file is drawn.
static std::string ilda_dir() {
    std::string dir = app_data_dir() + "/ilda";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}
using IldaClip = std::shared_ptr<const std::vector<IldaFrame>>;
static std::mutex                     G_ilda_mtx;
static std::map<std::string, IldaClip> G_ilda_cache; // storage name -> frames (nullptr = unreadable)

static void ilda_cache_put(const std::string& name, std::vector<IldaFrame> frames) {
    auto clip = std::make_shared<const std::vector<IldaFrame>>(std::move(frames));
    std::lock_guard<std::mutex> lk(G_ilda_mtx);
    G_ilda_cache[name] = std::move(clip);
}
static IldaClip ilda_clip(const std::string& name) {
    if (!ilda_valid_storage_name(name)) return nullptr;
    {
        std::lock_guard<std::mutex> lk(G_ilda_mtx);
        auto it = G_ilda_cache.find(name);
        if (it != G_ilda_cache.end()) return it->second;
    }
    std::ifstream f(ilda_dir() + "/" + name, std::ios::binary);
    const std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<IldaFrame> frames;
    if (data.empty() || !parse_ilda(data.data(), data.size(), frames) || frames.empty()) {
        std::cout << "[ilda] cannot read " << name << " - cue shows nothing\n" << std::flush;
        std::lock_guard<std::mutex> lk(G_ilda_mtx);
        G_ilda_cache[name] = nullptr; // don't retry every frame
        return nullptr;
    }
    ilda_cache_put(name, std::move(frames));
    std::lock_guard<std::mutex> lk(G_ilda_mtx);
    return G_ilda_cache[name];
}
// First-run/new-release seeds: shipped ILDA files that aren't in the data
// dir yet (names are content hashes, so an existing file is never changed).
static void seed_ilda_files() {
    namespace fs = std::filesystem;
    const std::string dest = ilda_dir();
    for (const std::string& src : {exe_dir() + "/ilda", exe_dir() + "/../Resources/ilda"}) {
        std::error_code ec;
        if (!fs::is_directory(src, ec)) continue;
        for (const auto& e : fs::directory_iterator(src, ec)) {
            const std::string name = e.path().filename().string();
            if (!ilda_valid_storage_name(name) || fs::exists(dest + "/" + name)) continue;
            std::error_code cec;
            fs::copy_file(e.path(), dest + "/" + name, cec);
            if (!cec) std::cout << "[data] seeded ilda/" << name << "\n";
        }
    }
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
    // Scanner tuning for dots (see dot_timing() in show_fx.h): how long each
    // dot is lit, and how long the beam stays dark before (on top of the
    // travel time) and after it. Global like output_latency_ms.
    float dot_dwell_us = kDotDwellUsDefault;
    float dot_blank_us = kDotBlankUsDefault;

    // Shape
    std::string shape     = "circle";
    // ILDA file look (shape == "ilda", see make_ilda_frame()): the file in
    // the user data dir's ilda/ folder (content-hash storage name, see
    // ilda_storage_name()), its original name for display, and the
    // animation speed (ILDA files carry no timing). Part of cues.
    std::string ilda_file;
    std::string ilda_name;
    float ilda_fps        = 30.0f;
    // Text look (shape == "text", see make_text_frame() / laser_text.h):
    // the text (UTF-8), its Hershey font (text_font_catalog() id), cap
    // height in output units, extra letter spacing (in cap heights) and
    // styles. Part of cues.
    std::string text      = "TEXT";
    std::string font      = "sans";
    float text_size       = 0.3f;
    float text_spacing    = 0.0f;
    bool  text_bold       = false;
    bool  text_italic     = false;
    bool  text_underline  = false;
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
    // look's animation state (LookSlot::rotation_phase) and changes every frame; this field just
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

    // Stars shape: how many stars (dots at random, fixed spots - star_layout()).
    int   stars_amount    = 40;

    // Rainbow
    float rainbow_amount  = 0.0f;     // 0..1 blend
    float rainbow_speed   = 0.0f;     // hue cycles/sec
    float rainbow_phase_degrees = 0.0f; // hue offset per configured laser, degrees (360 = one full hue cycle)

    // Sparkle (glitter, see apply_sparkle() in show_fx.h): share of the
    // output flashing at any moment (0 = off), how far it goes to the
    // sparkle color, and that color (white by default). Works on every shape.
    float sparkle_amount    = 0.0f;   // 0..1
    float sparkle_intensity = 1.0f;   // 0..1
    float sparkle_speed     = kSparkleDefaultHz; // changes per second, 0 = frozen pattern
    float sparkle_r = 1.0f, sparkle_g = 1.0f, sparkle_b = 1.0f;

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

// ── Cue layers ───────────────────────────────────────────────────────────────
// Besides a laser's look (which a normal "Solo" cue recall replaces), up to
// MAX_SHOW_LAYERS cues can play *on top of it* ("Layer" playback, like
// Pangolin Beyond / lighting consoles): each layer is a copy of a cue's look
// with its own running movement/rotation/wave/rainbow, drawn into the same
// frame as the look (compose_figures(), show_fx.h). Master brightness,
// flash, blackout, gate, zone, masks and flicker apply to the whole output.
// Each laser has its own layers (LookSlot::layers) - a layer plays on the
// lasers it was started on.
// Turned on/off via POST /api/cue/<n>/layer/<on|off|toggle> (Magic Touch
// "Layer" cue buttons, MIDI cue_layer / cue_layer_flash), all off via
// POST /api/layers/clear or /api/reset. Solo cue recalls leave layers
// running. Runtime only (not saved). Guarded by G_mtx.
struct ShowLayer {
    int        cue = 0;
    LaserState s;
    double     move_phase = 0.0, rotation_phase = 0.0, wave_phase = 0.0, rainbow_phase = 0.0;
    double     ilda_time = 0.0; // seconds since the layer started (ILDA animation)
    double     sparkle_phase = 0.0; // integrated sparkle_speed (see apply_sparkle())
};
static constexpr std::size_t MAX_SHOW_LAYERS = 8;

// ── Test pattern ─────────────────────────────────────────────────────────────
// "off" or a test_patterns.h catalog id (official ILDA 12K/30K test pattern).
// While set, every *armed* laser outputs the pattern instead of the show
// (see laser_thread()); unarmed lasers stay dark as always. Switching a
// pattern on sets the real scan rate (G.rate_kpps) to the pattern's required
// rate, so the Beam page always shows what the DACs run at; switching it off
// restores the rate from before (G_rate_before_test_pattern). Meanwhile
// settings.json keeps that earlier rate, so a crash or quit during a test
// can't leave the scan rate at the pattern's. Blackout, master brightness, footswitch, mirror, zone and
// safety masks still apply; flicker, layers and the look don't. Size scales
// the signal (ILDA: scan the pattern small while tuning). Set from the
// Lasers panel via POST /api/test-pattern. Runtime only - never saved, so a
// restart is always back to the show. Guarded by G_mtx.
static std::string G_test_pattern      = "off";
static float       G_test_pattern_size = 1.0f;
static float       G_rate_before_test_pattern = -1.0f; // < 0: no pattern active

// The former "staticwave" shape is a wave with speed 0 - looks from older
// cues, scripts or MIDI maps that still name it become exactly that.
static void apply_legacy_shape(LaserState& L) {
    if (L.shape == "staticwave") { L.shape = "wave"; L.wave_speed = 0.0f; }
}

// `next` (a cue, a momentary snapshot, the defaults) with the *global* daemon
// settings taken over from `current`: footswitch fade time, blackout, - for
// cue recall - master brightness, and the Beam page's projector settings
// (scan rate and its ceiling, output latency, dot dwell/blanking). None of
// these are part of a look, so switching looks or resetting the show never
// changes them; the projector settings are also persisted centrally in
// settings.json (see save_settings_if_changed()). Dot density and flicker
// ARE part of the look (Shapes page) and come with the cue.
static LaserState with_global_settings(LaserState next, const LaserState& current, bool keepIntensity) {
    next.flash_release_ms  = current.flash_release_ms;
    next.max_rate_kpps     = current.max_rate_kpps;
    next.rate_kpps         = current.rate_kpps;
    next.output_latency_ms = current.output_latency_ms;
    next.dot_dwell_us      = current.dot_dwell_us;
    next.dot_blank_us      = current.dot_blank_us;
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
    // Preview only: this laser projects onto a screen/wall (text, graphics)
    // instead of beaming at the audience - the 3D preview then draws its
    // output on a virtual wall behind it, readable for the viewer. The
    // hardware output is the same either way.
    bool        canvas = false;
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

// ── Per-laser looks ──────────────────────────────────────────────────────────
// Every configured laser has its own complete look (shape, color, movement,
// rainbow, dots, flicker, ILDA file, ...), its own cue layers and its own
// animation state. Look 0 is the preview's virtual laser, used while no laser
// is configured. G keeps only the *global* settings - blackout, master
// brightness, flash release, the Beam/projector settings (see
// with_global_settings()); its look fields are unused.
//
// A change goes to a set of target lasers - the browser's laser selection,
// sent as `lasers` (see request_lasers()). Without targets it goes to every
// look, so anything that knows nothing about selections (MIDI, scripts, an
// older UI) drives all lasers alike, as before per-laser looks existed.
//
// Animation state lives here too and is integrated by laser_thread() (under
// G_mtx) every tick. Phase accumulators integrate `speed * dt` instead of
// computing `speed * G_time` fresh each frame - the latter jumps the instant
// a speed changes (G_time has accumulated a large value, so a *new* speed
// lands on a completely different point in the cycle). Integrating means a
// speed change only changes the rate from that moment on. Other threads
// never write the animation state directly: they set the request flags
// below (cue recall: snap_all + a start angle; motion hold: snap_speeds;
// rotation reset: a start angle) and laser_thread() applies them at its next
// tick - so a recalled cue always starts exactly at its saved angle.
struct LookSlot {
    LaserState look;               // look fields only - global settings come from G
    std::vector<ShowLayer> layers; // cue layers playing on this laser
    ShowParamSmoother<LaserState> smooth; // rendered (glided) values, see param_smoothing.h
    double move_phase = 0.0, rotation_phase = 0.0, wave_phase = 0.0; // radians
    double rainbow_phase = 0.0;    // cycles, *unwrapped* (0 = rainbow just switched on) - see animate_look()
    double ilda_time = 0.0;        // seconds since the look started (ILDA animation)
    double sparkle_phase = 0.0;    // integrated sparkle_speed - a speed change only changes the rate (see apply_sparkle())
    // Exponential position smoothing (ported from the original
    // BeamCommander's ofApp.cpp `smoothOne`, see smooth_toward()): pos_x/
    // pos_y are a *target*, the rendered value slews toward it - hides
    // MIDI's 7-bit stepping, while big jumps still feel responsive. Same
    // for the wave shape's three knobs.
    float  pos_x_smooth = 0.0f, pos_y_smooth = 0.0f;
    float  wave_frequency_smooth = 1.0f, wave_amplitude_smooth = 0.45f, wave_speed_smooth = 0.0f;
    // Requests for laser_thread()'s next tick.
    bool   snap_all = true;        // every glide jumps to the target (cue recall, new look)
    bool   snap_speeds = false;    // speeds jump - a freeze must be a freeze (motion hold)
    bool   rotation_pending = false;
    double pending_angle = 0.0;    // start angle (radians), applied with the look's own rotation speed
    // A cue's saved animation state (movement/wave/rainbow phase, ILDA
    // time), applied after snap_all so the cue continues exactly where it was.
    bool   anim_pending = false;
    double pending_move_phase = 0.0, pending_wave_phase = 0.0, pending_rainbow_phase = 0.0, pending_ilda_time = 0.0;
    // Points in this laser's last preview frame (the UI's frame rate hint).
    std::size_t frame_points = 0;
    // Speeds to restore when motion hold is released.
    float  pre_motion_move_speed = 0.0f, pre_motion_rotation_speed = 0.0f, pre_motion_rainbow_speed = 0.0f;

    // The current rotation angle - including a requested one laser_thread()
    // hasn't applied yet (e.g. a cue saved right after a rotation reset).
    double current_angle() const { return rotation_pending ? pending_angle : rotation_phase; }
    // Call while holding G_mtx right after setting look.rotation_speed, so
    // laser_thread() sees the new speed and the angle in the same tick.
    void request_angle(double radians) { pending_angle = radians; rotation_pending = true; }
};
static constexpr int kVirtualLook = 0;
static std::map<int, LookSlot> G_looks{{kVirtualLook, LookSlot{}}}; // laser id -> look. Guarded by G_mtx.
// Motion hold also freezes cue layers (see do_motion_hold_press()).
static std::atomic<bool> G_motion_frozen{false};

// Configured laser ids in Lasers-list order. Takes G_lasers_mtx - call it
// *before* locking G_mtx (the two are never held at the same time).
static std::vector<int> laser_order() {
    std::lock_guard<std::mutex> lk(G_lasers_mtx);
    std::vector<int> ids;
    for (const auto& l : G_lasers) ids.push_back(l.id);
    return ids;
}

// The following helpers expect G_mtx to be held.
// The first laser's look (or the virtual one without lasers): what clients
// that show a single look - the top level of GET /api/state, MIDI LEDs - see.
static int primary_look_id(const std::vector<int>& order) {
    for (int id : order) if (G_looks.count(id)) return id;
    return kVirtualLook;
}
// Every configured laser has a look; a new laser starts as a copy of the
// first laser's look, running animation and layers included, so it joins
// the show in sync. Looks of removed lasers are dropped; the virtual look
// always stays.
static void sync_look_slots(const std::vector<int>& order) {
    for (auto it = G_looks.begin(); it != G_looks.end(); ) {
        if (it->first != kVirtualLook && std::find(order.begin(), order.end(), it->first) == order.end()) it = G_looks.erase(it);
        else ++it;
    }
    for (int id : order) {
        if (G_looks.count(id)) continue;
        const LookSlot copy = G_looks.at(primary_look_id(order));
        G_looks[id] = copy;
    }
}
// The looks a change applies to, in Lasers-list order: the requested
// lasers (ids that aren't configured are ignored), or - when nothing was
// requested - every look: all lasers, then the virtual one.
static std::vector<int> look_targets(const std::optional<std::vector<int>>& requested, const std::vector<int>& order) {
    sync_look_slots(order);
    std::vector<int> out;
    if (!requested) {
        out = order;
        out.push_back(kVirtualLook);
        return out;
    }
    auto wanted = [&](int id) { return std::find(requested->begin(), requested->end(), id) != requested->end(); };
    for (int id : order) if (wanted(id)) out.push_back(id);
    if (order.empty() && wanted(kVirtualLook)) out.push_back(kVirtualLook);
    return out;
}
// Runs fn on the look of every target (locks G_mtx itself).
static void for_target_looks(const std::optional<std::vector<int>>& requested, const std::function<void(LookSlot&)>& fn) {
    const std::vector<int> order = laser_order();
    std::lock_guard<std::mutex> lk(G_mtx);
    for (int id : look_targets(requested, order)) fn(G_looks.at(id));
}
static void for_all_looks(const std::function<void(LookSlot&)>& fn) { for_target_looks(std::nullopt, fn); }

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

// The animated state a look is drawn with - its own for every laser's look
// (LookSlot, see look_render_input()) and for every layer (ShowLayer).
// maxPoints is the look's share of the frame's point budget (see
// kFramePointBudget): with layers active, each look gets fewer points so the
// combined frame still fits the DAC buffer.
struct RenderPose {
    double rotation = 0.0;           // radians
    float  pos_x = 0.0f, pos_y = 0.0f;
    float  wave_frequency = 1.0f, wave_amplitude = 0.45f;
    double wave_phase = 0.0;         // radians
    double ilda_time = 0.0;          // seconds since the look started (ILDA animation)
    double sparkle_phase = 0.0;      // the look's sparkle phase (integrated sparkle_speed)
    int    maxPoints = 3000;
};
static constexpr int kFramePointBudget = 3000; // safely under the DAC's 4095-point buffer

static std::vector<Pt> gen_base_shape(const LaserState& s, const RenderPose& pose) {
    const float pi  = std::acos(-1.0f);
    const float tau = 2.0f * pi;
    const int   n   = std::max(8, std::min(s.points, pose.maxPoints));
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
        float phase = (float)pose.wave_phase;
        for (int i=0;i<n;++i) {
            float t=(float)i/(n-1)*tau;
            pts.push_back({(t/tau*2.0f-1.0f)*r, r*pose.wave_amplitude*std::sin(pose.wave_frequency*t+phase)});
        }
    } else if (s.shape == "stars") {
        // Random but fixed spots in the disk of radius r (see star_layout());
        // make_frame() draws every one as a dot.
        for (const StarPos& p : star_layout(s.stars_amount)) pts.push_back({p.x * r, p.y * r});
        if (pts.empty()) pts.push_back({0.0f, 0.0f});
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
        // The movement phase is already kept in [0,tau) via fmod (see
        // animate_look()), so a plain linear map of that range is enough.
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

// Where a look sits: its (smoothed) position plus the movement offset.
// Uses the smoothed position (see LookSlot::pos_x_smooth), not the raw target
// s.pos_x/s.pos_y directly, so manual moves - especially MIDI, whose 7-bit
// knobs would otherwise visibly step - glide instead of jumping. Switching
// the movement pattern crossfades from the old one (blend driven by
// ShowParamSmoother) instead of jumping to a new position.
static Pt look_offset(const LaserState& s, double movement_phase, const RenderPose& pose) {
    Pt mv = calc_movement(s.move_mode, s.move_size, movement_phase);
    if (s.move_blend < 1.0f && s.move_mode_from != s.move_mode) {
        const Pt from = calc_movement(s.move_mode_from, s.move_size, movement_phase);
        const float w = std::clamp(s.move_blend, 0.0f, 1.0f);
        mv.x = from.x + (mv.x - from.x) * w;
        mv.y = from.y + (mv.y - from.y) * w;
    }
    return {pose.pos_x + mv.x, pose.pos_y + mv.y};
}

// An ILDA file look: the file's frame for this moment (animations loop at
// ilda_fps), its points and colors exactly as in the file, then the look's
// scale (shape_scale, 0 = the file's own size), rotation, position +
// movement, mirror, master brightness and a held white flash - like a
// shape. Not applied: the shape's color, radius, point count, dots, rainbow
// and wave. Flicker, blackout, zone and masks follow later, as for shapes.
// Only a frame bigger than the look's point budget is thinned
// (ilda_fit_points()); a missing/unreadable file draws a single dark point.
static core::Frame make_ilda_frame(const LaserState& s, double movement_phase, const RenderPose& pose) {
    core::Frame frame;
    const IldaClip clip = ilda_clip(s.ilda_file);
    if (clip && !clip->empty()) {
        const auto& src = (*clip)[ilda_frame_index(pose.ilda_time, s.ilda_fps, clip->size())].points;
        frame.points = ilda_fit_points(src, (std::size_t)std::clamp(pose.maxPoints, 100, kFramePointBudget));
    }
    if (frame.points.empty()) {
        frame.points.push_back(core::LaserPoint{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f});
        return frame;
    }
    const float sf = s.shape_scale >= 0 ? (1.0f + s.shape_scale * 2.0f) : (1.0f + s.shape_scale * 0.7f);
    const Pt off = look_offset(s, movement_phase, pose);
    const float mirror_sign = s.mirror_x ? -1.0f : 1.0f;
    const float c = std::cos((float)pose.rotation), sn = std::sin((float)pose.rotation);
    const float bri = std::clamp(s.intensity, 0.0f, 1.0f);
    for (auto& p : frame.points) {
        const float x = p.x * sf, y = p.y * sf;
        const float px = mirror_sign * (x * c - y * sn + off.x);
        const float py = x * sn + y * c + off.y;
        p.x = px; p.y = py;
        const bool lit = p.r > 0.0f || p.g > 0.0f || p.b > 0.0f;
        // Same screen-edge rule as shapes (see make_frame()): blank, never clamp.
        if (!lit || std::fabs(px) > 1.0f || std::fabs(py) > 1.0f) { p.r = p.g = p.b = 0.0f; continue; }
        if (g_flash_active) { p.r = 1.0f; p.g = 1.0f; p.b = 1.0f; }
        p.r *= bri; p.g *= bri; p.b *= bri;
    }
    return frame;
}

// A text look: the text in its Hershey font and styles (laser_text.h), fitted
// into the look's point budget, then the look's scale, rotation, position +
// movement and mirror like a shape. Colored with the beam color - or, with
// rainbow_amount > 0, a rainbow wave running across the text from left to
// right (same hue math as make_frame(): starts at the beam color, more
// amount = more bands, rainbow_speed animates it). Master brightness and a
// held white flash apply; blackout, zone and masks follow later.
static core::Frame make_text_frame(const LaserState& s, double movement_phase, double rainbow_cycles, const RenderPose& pose) {
    core::Frame frame;
    TextStyle st;
    st.font = s.font;
    st.bold = s.text_bold; st.italic = s.text_italic; st.underline = s.text_underline;
    const float sf = s.shape_scale >= 0 ? (1.0f + s.shape_scale * 2.0f) : (1.0f + s.shape_scale * 0.7f);
    st.size = std::max(0.01f, s.text_size * sf);
    st.spacing = s.text_spacing;
    frame.points = text_points(layout_text(s.text, st), (std::size_t)std::clamp(pose.maxPoints, 100, kFramePointBudget));

    float minX = 1e9f, maxX = -1e9f;
    for (const auto& p : frame.points) if (p.r > 0.0f) { minX = std::min(minX, p.x); maxX = std::max(maxX, p.x); }
    const float width = std::max(1e-4f, maxX - minX);

    constexpr float kRainbowFadeIn = 0.1f;
    float rb_fade = std::clamp(s.rainbow_amount / kRainbowFadeIn, 0.0f, 1.0f);
    rb_fade = rb_fade * rb_fade * (3.0f - 2.0f * rb_fade);
    float beam_h, beam_s, beam_v;
    rgb_to_hsv(std::clamp(s.r, 0.0f, 1.0f), std::clamp(s.g, 0.0f, 1.0f), std::clamp(s.b, 0.0f, 1.0f), beam_h, beam_s, beam_v);
    const float rb_sat = beam_s + (1.0f - beam_s) * rb_fade;
    const float rb_val = beam_v + (1.0f - beam_v) * rb_fade;
    float rainbow_phase = (float)std::fmod(beam_h + rainbow_cycles, 1.0);
    if (rainbow_phase < 0.0f) rainbow_phase += 1.0f;
    const bool rainbow = s.rainbow_amount > 0.0f && !g_flash_active;

    const Pt off = look_offset(s, movement_phase, pose);
    const float mirror_sign = s.mirror_x ? -1.0f : 1.0f;
    const float c = std::cos((float)pose.rotation), sn = std::sin((float)pose.rotation);
    const float bri = std::clamp(s.intensity, 0.0f, 1.0f);
    for (auto& p : frame.points) {
        const bool lit = p.r > 0.0f;
        const float xNorm = (p.x - minX) / width;
        const float px = mirror_sign * (p.x * c - p.y * sn + off.x);
        const float py = p.x * sn + p.y * c + off.y;
        p.x = px; p.y = py;
        if (!lit || std::fabs(px) > 1.0f || std::fabs(py) > 1.0f) { p.r = p.g = p.b = 0.0f; continue; }
        float fr = s.r, fg = s.g, fb = s.b;
        if (rainbow) {
            float hue = std::fmod(rainbow_phase - xNorm * s.rainbow_amount * 24.0f, 1.0f);
            if (hue < 0.0f) hue += 1.0f;
            hsv_to_rgb(hue, rb_sat, rb_val, fr, fg, fb);
        }
        p.r = fr * bri; p.g = fg * bri; p.b = fb * bri;
    }
    return frame;
}

static core::Frame make_frame(const LaserState& s, double movement_phase, double rainbow_cycles, const RenderPose& pose) {
    if (s.shape == "ilda") return make_ilda_frame(s, movement_phase, pose);
    if (s.shape == "text") return make_text_frame(s, movement_phase, rainbow_cycles, pose);
    auto base = gen_base_shape(s, pose);

    // Rotation - always applied at the current phase, even when
    // rotation_speed is currently 0 (e.g. motion_hold is pressed, or the
    // speed knob itself is at 0). Previously this was gated on
    // `rotation_speed != 0.0f`, so the instant speed hit 0 the shape
    // snapped back to its unrotated base orientation instead of holding
    // whatever angle it had reached - the rotation phase itself was already
    // frozen correctly (integrates by speed*dt, so 0 speed = no further
    // change), it just wasn't being drawn. Rotating by phase 0 is a no-op,
    // so applying it unconditionally is always safe.
    rotate_pts(base, (float)pose.rotation);

    // Position + movement (see look_offset()).
    const Pt off = look_offset(s, movement_phase, pose);
    float ox = off.x;
    float oy = off.y;

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
    // Continuous dot count (see place_dots() in show_fx.h): while the
    // (glided) dot amount changes, dots slide apart/together and the newest
    // one fades in or out - nothing jumps when the whole-number count changes.
    double dotCount = n0;
    if (s.dot_amount < 1.0f && s.dot_amount > 0.0f)
        dotCount = std::clamp((double)s.dot_amount * n0, 1.0, (double)n0);
    bool dotted = dotCount < n0 - 1e-6;
    // Stars are always dots - one per star (dot density doesn't apply).
    if (s.shape == "stars") { dotCount = n0; dotted = true; }
    int numDots = dotted ? (int)std::ceil(dotCount - 1e-9) : n0;

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
    //
    // These times used to be fixed *point counts* (dwell 4, trail 2, settle
    // >= 4: ~0.13/0.07/0.13 ms at 30 kpps) - far shorter than real galvos
    // and laser modulation need, so on a real projector every dot showed up
    // as a short streak along the travel direction. They are now times
    // (dot_timing(), show_fx.h: "Dot dwell" / "Dot blanking" global
    // settings plus a travel time per unit of jump distance), converted to
    // points at the actual scan rate.
    DotTiming T = dot_timing(std::clamp(s.rate_kpps, 1.0f, 100.0f) * 1000.0, s.dot_dwell_us, s.dot_blank_us);

    // HARDWARE SAFETY CAP: each dot can cost up to T.settleMax+T.dwell+
    // T.trail points (well over 100 at high scan rates), so a shape with many base points (`points`,
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
    //     even the cheapest possible per-dot cost (kFloorDotCost) could
    //     still add up to far more than the budget
    //     when `points` is large.
    //  2. Within that dot count, shrink the per-dot blank-settle ceiling
    //     as numDots grows, so large jumps still get proportionally more
    //     settle time relative to each other, just capped lower overall
    //     when there are many dots.
    const int kMaxFramePoints = std::clamp(pose.maxPoints, 100, kFramePointBudget); // this look's share of the budget
    // Few dots get the full times (crisp dots); with many dots the times
    // are compressed just enough to fit the budget, never below a floor of
    // 2 settle + 2 lit + 1 trail points.
    const int kFloorDotCost = 2 + 2 + 1;
    const int kMaxDots = kMaxFramePoints / kFloorDotCost;
    if (dotted && dotCount > kMaxDots) { dotCount = kMaxDots; numDots = kMaxDots; }
    int perDotBlankMax = T.settleMax;
    if (dotted && numDots > 0) {
        const int budgetPerDot = kMaxFramePoints / numDots;
        const int cost = T.settleMin + T.dwell + T.trail;
        if (cost > budgetPerDot) {
            const double f = (double)budgetPerDot / cost;
            T.settleMin = std::max(2, (int)(T.settleMin * f));
            T.dwell     = std::max(2, (int)(T.dwell * f));
            T.trail     = std::max(1, (int)(T.trail * f));
        }
        perDotBlankMax = std::clamp(budgetPerDot - T.dwell - T.trail, T.settleMin, std::max(T.settleMin, T.settleMax));
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
    frame.points.reserve(dotted ? numDots * (T.settleMin + T.dwell + T.trail + 8) : n);

    // The DAC loops the frame continuously, so the "incoming" jump into the
    // very first kept dot is really the wrap-around travel from the *last*
    // kept dot back to it - seed prev{x,y} with that last dot's position
    // (rather than e.g. 0,0) so that wrap-around segment gets the correct
    // settle time too, not an arbitrary one based on a fake start point.
    // Dots sit at fractional base-point positions; between two base points
    // the position is interpolated. Closed shapes wrap from the last point
    // back to the first, open ones (line, waves) stop at their end.
    const bool closedShape = s.shape == "circle" || s.shape == "triangle" || s.shape == "square";
    const std::vector<DotPlacement> dots = dotted ? place_dots(dotCount, n) : std::vector<DotPlacement>{};
    auto basePointAt = [&](double pos) {
        const int i0 = std::clamp((int)std::floor(pos), 0, n - 1);
        const float t = (float)(pos - i0);
        int i1 = i0 + 1;
        if (i1 >= n) i1 = closedShape ? 0 : n - 1;
        return Pt{base[i0].x + (base[i1].x - base[i0].x) * t, base[i0].y + (base[i1].y - base[i0].y) * t};
    };

    float prevx = 0.0f, prevy = 0.0f;
    if (dotted && !dots.empty()) {
        const Pt last = basePointAt(dots.back().pos);
        prevx = mirror_sign * (last.x + ox);
        prevy = last.y + oy;
    }

    const int drawCount = dotted ? (int)dots.size() : n;
    for (int d = 0; d < drawCount; ++d) {
        const double pos = dotted ? dots[d].pos : (double)d;
        const float dotLevel = dotted ? dots[d].level : 1.0f; // < 1 only for the dot fading in/out
        const Pt bp = dotted ? basePointAt(pos) : base[d];
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
            float x_norm = n > 1 ? (float)(pos / (double)(n - 1)) : 0.0f;
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
        float px = mirror_sign * (bp.x + ox);
        float py = bp.y + oy;
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
        float bri = offScreen ? 0.0f : s.intensity * dotLevel;

        if (dotted) {
            // Arrive blanked and hold for a distance-scaled number of
            // settle points (see the dot timing comment above), then
            // hold lit for T.dwell, then blank again for T.trail
            // before the beam is allowed to move off.
            float dx = px - prevx, dy = py - prevy;
            float dist = std::sqrt(dx * dx + dy * dy);
            int blankSettle = std::clamp((int)std::round(dist * T.settlePerUnit) + T.settleMin,
                                          T.settleMin, perDotBlankMax);
            for (int d = 0; d < blankSettle; ++d)
                frame.points.emplace_back(core::LaserPoint{px, py, 0.0f, 0.0f, 0.0f, 1.0f});
            for (int d = 0; d < T.dwell; ++d) {
                frame.points.emplace_back(core::LaserPoint{
                    px, py, fr * bri, fg * bri, fb * bri, 1.0f
                });
            }
            for (int d = 0; d < T.trail; ++d)
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
// A laser's flicker for the preview (its look's rate and on time, plus its
// flicker phase as a time offset in seconds).
struct PreviewFlicker { int id; float hz, onMs; double offset; };
static std::string frame_to_ws_json(const core::Frame& fr,
                                    const std::vector<std::pair<int, core::Frame>>& laserFrames,
                                    double showTime, float flickerHz, float flickerOnMs,
                                    const std::vector<PreviewFlicker>& flickers,
                                    const std::vector<int>& testPatternLasers = {}, float testPatternKpps = 0.0f) {
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
    // t = show clock (G_time) and fl = [flicker_hz, flicker_on_ms] of the
    // virtual preview laser: the preview applies flicker per simulated point
    // itself, in step with the laser (frames here are sent unflickered - see
    // make_frame()).
    ss << "{\"t\":" << std::setprecision(6) << showTime << std::setprecision(4)
       << ",\"fl\":[" << flickerHz << ',' << flickerOnMs << "]";
    // fll = per laser [flicker_hz, flicker_on_ms, time offset in s (flicker phase)] -
    // every laser has its own look, so its own flicker.
    ss << ",\"fll\":{";
    for (std::size_t i = 0; i < flickers.size(); ++i)
        ss << (i ? "," : "") << '\"' << flickers[i].id << "\":[" << flickers[i].hz << ',' << flickers[i].onMs << ','
           << std::setprecision(6) << flickers[i].offset << std::setprecision(4) << ']';
    ss << "}";
    // tp = lasers showing a test pattern -> its point rate (kpps): the
    // preview replays those at that rate and without flicker, like the DAC.
    if (!testPatternLasers.empty()) {
        ss << ",\"tp\":{";
        for (std::size_t i = 0; i < testPatternLasers.size(); ++i)
            ss << (i ? "," : "") << '\"' << testPatternLasers[i] << "\":" << testPatternKpps;
        ss << "}";
    }
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

// A look's fields as JSON members (no braces), incl. its active cue layers.
static void look_fields_json(std::ostringstream& ss, const LookSlot& sl) {
    const LaserState& L = sl.look;
    ss << "\"shape\":\""       << L.shape         << "\","
       << "\"ilda_file\":\""   << L.ilda_file     << "\","
       << "\"ilda_name\":\""   << L.ilda_name     << "\","
       << "\"ilda_fps\":"      << L.ilda_fps       << ","
       << "\"text\":"          << json_escape(L.text) << ","
       << "\"font\":\""        << L.font           << "\","
       << "\"text_size\":"     << L.text_size      << ","
       << "\"text_spacing\":"  << L.text_spacing   << ","
       << "\"text_bold\":"     << (L.text_bold?"true":"false") << ","
       << "\"text_italic\":"   << (L.text_italic?"true":"false") << ","
       << "\"text_underline\":" << (L.text_underline?"true":"false") << ","
       << "\"frame_points\":"  << sl.frame_points   << ","
       << "\"radius\":"        << L.radius         << ","
       << "\"points\":"        << L.points         << ","
       << "\"r\":"             << L.r              << ","
       << "\"g\":"             << L.g              << ","
       << "\"b\":"             << L.b              << ","
       << "\"shape_scale\":"   << L.shape_scale    << ","
       << "\"pos_x\":"         << L.pos_x          << ","
       << "\"pos_y\":"         << L.pos_y          << ","
       << "\"rotation_speed\":" << L.rotation_speed << ","
       << "\"mirror_x\":"      << (L.mirror_x?"true":"false") << ","
       << "\"move_mode\":\""   << L.move_mode      << "\","
       << "\"move_speed\":"    << L.move_speed     << ","
       << "\"move_size\":"     << L.move_size      << ","
       << "\"move_phase_degrees\":" << L.move_phase_degrees << ","
       << "\"wave_frequency\":" << L.wave_frequency << ","
       << "\"wave_amplitude\":" << L.wave_amplitude << ","
       << "\"wave_speed\":"    << L.wave_speed     << ","
       << "\"stars_amount\":"  << L.stars_amount   << ","
       << "\"rainbow_amount\":" << L.rainbow_amount << ","
       << "\"rainbow_speed\":" << L.rainbow_speed  << ","
       << "\"rainbow_phase_degrees\":" << L.rainbow_phase_degrees << ","
       << "\"sparkle_amount\":" << L.sparkle_amount << ","
       << "\"sparkle_intensity\":" << L.sparkle_intensity << ","
       << "\"sparkle_speed\":" << L.sparkle_speed << ","
       << "\"sparkle_r\":" << L.sparkle_r << ",\"sparkle_g\":" << L.sparkle_g << ",\"sparkle_b\":" << L.sparkle_b << ","
       << "\"dot_amount\":"    << L.dot_amount     << ","
       << "\"flicker_hz\":"    << L.flicker_hz     << ","
       << "\"flicker_on_ms\":" << L.flicker_on_ms  << ","
       << "\"flicker_phase_degrees\":" << L.flicker_phase_degrees << ","
       << "\"layers\":[";
    for (std::size_t i = 0; i < sl.layers.size(); ++i) ss << (i ? "," : "") << sl.layers[i].cue;
    ss << "]";
}

// GET /api/state: the global settings, the first laser's look at the top
// level (clients that only know one look keep working), and every laser's
// own look under "looks" (laser id -> look; "0" = the virtual preview laser).
static std::string state_to_json() {
    const std::vector<int> order = laser_order();
    std::lock_guard<std::mutex> lk(G_mtx);
    sync_look_slots(order);
    std::ostringstream ss; ss << std::fixed << std::setprecision(4);
    ss << "{"
       << "\"rate_kpps\":"     << G.rate_kpps      << ","
       << "\"max_rate_kpps\":" << G.max_rate_kpps  << ","
       << "\"output_latency_ms\":" << G.output_latency_ms << ","
       << "\"dot_dwell_us\":" << G.dot_dwell_us << ","
       << "\"dot_blank_us\":" << G.dot_blank_us << ","
       << "\"intensity\":"     << G.intensity      << ","
       << "\"flash_release_ms\":" << G.flash_release_ms << ","
       << "\"blackout\":"      << (G.blackout?"true":"false") << ","
       << "\"brightness_gate_open\":" << (g_brightness_gate_open.load()?"true":"false") << ","
       << "\"test_pattern\":\"" << G_test_pattern << "\","
       << "\"test_pattern_size\":" << G_test_pattern_size << ",";
    look_fields_json(ss, G_looks.at(primary_look_id(order)));
    ss << ",\"looks\":{";
    bool first = true;
    for (const auto& [id, sl] : G_looks) {
        ss << (first ? "" : ",") << '\"' << id << "\":{";
        look_fields_json(ss, sl);
        ss << "}";
        first = false;
    }
    ss << "}}";
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

// ── Rendering looks (main look + cue layers) ─────────────────────────────────
// The main look is drawn with the smoothed globals; each layer with its own
// phases and its cue's fixed values.
static RenderPose layer_pose(const ShowLayer& L, int maxPoints) {
    RenderPose p;
    p.rotation = L.rotation_phase;
    p.pos_x = L.s.pos_x; p.pos_y = L.s.pos_y;
    p.wave_frequency = L.s.wave_frequency; p.wave_amplitude = L.s.wave_amplitude;
    p.wave_phase = L.wave_phase;
    p.ilda_time = L.ilda_time;
    p.sparkle_phase = L.sparkle_phase;
    p.maxPoints = maxPoints;
    return p;
}
// A layer's cue look, with the live global settings of the main look:
// master brightness and a held white flash apply to layers too.
static LaserState layer_render_state(const ShowLayer& L, const LaserState& mainLook) {
    LaserState s = L.s;
    s.intensity = mainLook.intensity;
    s.move_mode_from = s.move_mode; s.move_blend = 1.0f;
    // Projector settings are global, not the layer cue's (its own dot
    // density stays - dots are part of the look)
    s.rate_kpps = mainLook.rate_kpps;
    s.dot_dwell_us = mainLook.dot_dwell_us; s.dot_blank_us = mainLook.dot_blank_us;
    if (g_flash_active) { s.r = 1.0f; s.g = 1.0f; s.b = 1.0f; }
    return s;
}
// One look as seen by laser k (Lasers-list order, laser 1 = 0): laser k gets
// k * move_phase_degrees added to its movement phase, and runs the rainbow
// k * rainbow_phase_degrees behind laser 1 (a delay chain that never goes
// past the switch-on point, hue position 0).
static core::Frame render_look_for_laser(const LaserState& s, double movePhase, double huePhase,
                                         const RenderPose& pose, std::size_t k) {
    const double kk = (double)k;
    double laserMove = movePhase, laserHue = huePhase;
    if (k > 0) {
        const double phaseStep = s.move_phase_degrees * (std::acos(-1.0) / 180.0);
        const double rainbowStep = s.rainbow_phase_degrees / 360.0; // hue cycles
        if (s.move_mode != "none" && std::abs(phaseStep) > 1e-9) laserMove += kk * phaseStep;
        if (s.rainbow_amount > 0.0f && std::abs(rainbowStep) > 1e-9)
            laserHue = huePhase >= 0.0 ? std::max(0.0, huePhase - kk * rainbowStep)
                                       : std::min(0.0, huePhase + kk * rainbowStep);
    }
    core::Frame f = make_frame(s, laserMove, laserHue, pose);
    // Sparkle on top of whatever the look drew - every shape, stars, text and
    // ILDA alike. Its color goes through the master brightness like the rest.
    if (s.sparkle_amount > 0.0f) {
        const float bri = std::clamp(s.intensity, 0.0f, 1.0f);
        apply_sparkle(f.points, s.sparkle_amount, s.sparkle_intensity,
                      std::clamp(s.sparkle_r, 0.0f, 1.0f) * bri, std::clamp(s.sparkle_g, 0.0f, 1.0f) * bri,
                      std::clamp(s.sparkle_b, 0.0f, 1.0f) * bri, pose.sparkle_phase, (unsigned)k * 7919U + 1U);
    }
    return f;
}

// Everything needed to draw one laser's look, copied under G_mtx so the
// drawing itself runs unlocked: the look with the global settings (master
// brightness, ...) and its glided values, a held white flash (full white at
// full brightness, any look), its animation pose and its cue layers.
struct LookRender {
    LaserState s;
    RenderPose pose;
    double move_phase = 0.0, rainbow_phase = 0.0;
    std::vector<ShowLayer> layers;
};
static LookRender look_render_input(const LookSlot& sl, const LaserState& globals) {
    LookRender r;
    r.s = with_global_settings(sl.look, globals, /*keepIntensity=*/true);
    sl.smooth.apply(r.s); // render the glided values, not the raw targets
    if (g_flash_active) { r.s.r = r.s.g = r.s.b = 1.0f; r.s.intensity = 1.0f; }
    r.pose.rotation = sl.rotation_phase;
    r.pose.pos_x = sl.pos_x_smooth; r.pose.pos_y = sl.pos_y_smooth;
    r.pose.wave_frequency = sl.wave_frequency_smooth; r.pose.wave_amplitude = sl.wave_amplitude_smooth;
    r.pose.wave_phase = sl.wave_phase;
    r.pose.ilda_time = sl.ilda_time;
    r.pose.sparkle_phase = sl.sparkle_phase;
    r.move_phase = sl.move_phase;
    r.rainbow_phase = sl.rainbow_phase;
    r.layers = sl.layers;
    return r;
}
// The frame laser k (Lasers-list position, laser 1 = 0) shows: its look plus
// its cue layers, each with its share of the frame's point budget, joined
// into one frame (compose_figures).
static core::Frame render_look_frame(const LookRender& r, std::size_t k) {
    const int budget = kFramePointBudget / (int)(1 + r.layers.size());
    RenderPose pose = r.pose;
    pose.maxPoints = budget;
    core::Frame f = render_look_for_laser(r.s, r.move_phase, r.rainbow_phase, pose, k);
    if (r.layers.empty()) return f;
    std::vector<std::vector<core::LaserPoint>> figures;
    figures.push_back(std::move(f.points));
    for (const auto& L : r.layers)
        figures.push_back(render_look_for_laser(layer_render_state(L, r.s), L.move_phase, L.rainbow_phase,
                                                layer_pose(L, budget), k).points);
    f.points = compose_figures(figures);
    return f;
}

// One laser_thread() tick of a look's animation (G_mtx held).
static void animate_look(LookSlot& sl, double dt, bool frozen) {
    const LaserState& L = sl.look;
    // Glide the rendered values towards their targets *before* integrating
    // the phases below, so speed changes accelerate/decelerate smoothly too.
    sl.smooth.update(L, (float)dt);
    // Cue switch: the new look appears at once - every glide (incl.
    // position and wave knobs) jumps straight to the target.
    if (sl.snap_all) {
        sl.snap_all = false;
        sl.smooth.snap_all(L);
        sl.pos_x_smooth = L.pos_x; sl.pos_y_smooth = L.pos_y;
        sl.wave_frequency_smooth = L.wave_frequency;
        sl.wave_amplitude_smooth = L.wave_amplitude;
        sl.wave_speed_smooth     = L.wave_speed;
        sl.ilda_time = 0.0; // an ILDA look starts at its first frame
    }
    if (sl.anim_pending) {
        sl.anim_pending = false;
        sl.move_phase = sl.pending_move_phase;
        sl.wave_phase = sl.pending_wave_phase;
        sl.rainbow_phase = sl.pending_rainbow_phase;
        sl.ilda_time = sl.pending_ilda_time;
    }
    if (sl.snap_speeds) { sl.snap_speeds = false; sl.smooth.snap_speeds(L); }
    // Cue recall / rotation reset: jump to the requested start angle and
    // take the new rotation speed as is (no leftover spin).
    if (sl.rotation_pending) {
        sl.rotation_pending = false;
        sl.rotation_phase = sl.pending_angle;
        sl.smooth.snap_rotation(L);
    }

    const double tau = 2.0 * std::acos(-1.0);
    sl.move_phase     = std::fmod(sl.move_phase + sl.smooth.move_speed * dt * tau, tau);
    // Negated here (not at every rotation_speed setter) so the reversed
    // direction applies no matter which API set the speed.
    sl.rotation_phase = std::fmod(sl.rotation_phase - sl.smooth.rotation_speed * dt * tau, tau);
    // The *smoothed* wave speed, so a MIDI knob jump changes the wave's
    // animation rate gradually, like the value knobs.
    sl.wave_phase     = std::fmod(sl.wave_phase + sl.wave_speed_smooth * dt * tau, tau);
    // Held at 0 while the rainbow is off, so turning it back on always starts
    // at the current beam color (see make_frame()). Deliberately not wrapped:
    // the per-laser rainbow delay (render_look_for_laser()) needs to know how
    // far the hue has travelled since switch-on.
    sl.rainbow_phase  = sl.smooth.rainbow_amount > 0.0f ? sl.rainbow_phase + sl.smooth.rainbow_speed * dt : 0.0;
    // ILDA animations play on regardless of motion hold.
    sl.ilda_time += dt;
    for (auto& Ly : sl.layers) Ly.ilda_time += dt;
    // Sparkle keeps glittering regardless of motion hold.
    sl.sparkle_phase += std::clamp(L.sparkle_speed, 0.0f, kSparkleMaxHz) * dt;
    for (auto& Ly : sl.layers) Ly.sparkle_phase += std::clamp(Ly.s.sparkle_speed, 0.0f, kSparkleMaxHz) * dt;
    // Cue layers run their own animation (frozen by motion hold).
    if (!frozen) {
        for (auto& Ly : sl.layers) {
            Ly.move_phase     = std::fmod(Ly.move_phase     + Ly.s.move_speed     * dt * tau, tau);
            Ly.rotation_phase = std::fmod(Ly.rotation_phase - Ly.s.rotation_speed * dt * tau, tau);
            Ly.wave_phase     = std::fmod(Ly.wave_phase     + Ly.s.wave_speed     * dt * tau, tau);
            Ly.rainbow_phase  = Ly.s.rainbow_amount > 0.0f ? Ly.rainbow_phase + Ly.s.rainbow_speed * dt : 0.0;
        }
    }
    // Slew the rendered position and wave knobs toward their targets.
    sl.pos_x_smooth = smooth_toward(sl.pos_x_smooth, L.pos_x, (float)dt, 0.090f);
    sl.pos_y_smooth = smooth_toward(sl.pos_y_smooth, L.pos_y, (float)dt, 0.090f);
    sl.wave_frequency_smooth = smooth_toward(sl.wave_frequency_smooth, L.wave_frequency, (float)dt, 0.090f);
    sl.wave_amplitude_smooth = smooth_toward(sl.wave_amplitude_smooth, L.wave_amplitude, (float)dt, 0.090f);
    sl.wave_speed_smooth     = smooth_toward(sl.wave_speed_smooth,     L.wave_speed,     (float)dt, 0.090f);
}

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

    float appliedLatencyMs = -1.0f;

    while (G_running) {
        // Advance animation clock
        auto now_tp = Clock::now();
        double dt = std::chrono::duration<double>(now_tp - last_tick).count();
        G_time += dt;
        last_tick = now_tp;

        // Snapshot the configured laser list (id/ip/armed/zone calibration)
        // - kept brief so no HTTP request (add/remove/(un)arm/re-calibrate)
        // is ever blocked on this thread's connect/send work below.
        std::vector<LaserConfig> lasers_snapshot;
        { std::lock_guard<std::mutex> lk(G_lasers_mtx); lasers_snapshot = G_lasers; }
        std::vector<int> order;
        for (const auto& l : lasers_snapshot) order.push_back(l.id);

        float rate, maxRate, testPatternSize;
        std::string testPatternId;
        {
            std::lock_guard<std::mutex> lk(G_mtx);
            rate = G.rate_kpps;
            maxRate = G.max_rate_kpps;
            testPatternId = G_test_pattern;
            testPatternSize = G_test_pattern_size;

            // Output latency (how far ahead libera queues frames).
            if (G.output_latency_ms != appliedLatencyMs) {
                appliedLatencyMs = G.output_latency_ms;
                core::LaserController::setTargetLatency(std::chrono::milliseconds(
                    (long long)std::lround(std::clamp(appliedLatencyMs, 10.0f, 200.0f))));
            }

            // Every laser's own look animates on its own (see LookSlot).
            sync_look_slots(order);
            const bool frozen = G_motion_frozen.load();
            for (auto& [id, sl] : G_looks) animate_look(sl, dt, frozen);

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
        // Test pattern: POST /api/test-pattern set the scan rate to the
        // pattern's rate. Armed lasers only show it while their DAC really runs
        // at it (ILDA: +-1%, checked per laser below) and stay dark otherwise -
        // e.g. if the scan rate or max rate is changed while the pattern runs.
        const TestPatternInfo* testPattern = find_test_pattern(testPatternId);
        const bool testPatternRunnable = testPattern && testPattern->kpps <= maxRate;
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

        // Each laser's look, copied under the lock and drawn unlocked below,
        // only for the lasers that need a frame this tick (cached).
        LaserState snap;
        std::map<int, LookRender> inputs; // laser id (kVirtualLook without lasers) -> its look
        {
            std::lock_guard<std::mutex> lk(G_mtx);
            snap = G; // global settings (blackout, master brightness, ...)
            for (int id : order) if (G_looks.count(id)) inputs[id] = look_render_input(G_looks.at(id), G);
            if (order.empty()) inputs[kVirtualLook] = look_render_input(G_looks.at(kVirtualLook), G);
        }
        std::map<int, core::Frame> frames;
        auto frameFor = [&](int id, std::size_t k) -> const core::Frame& {
            auto it = frames.find(id);
            if (it != frames.end()) return it->second;
            core::Frame f;
            auto in = inputs.find(id);
            if (in != inputs.end()) f = render_look_frame(in->second, k);
            else f.points.push_back(core::LaserPoint{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f}); // look not there yet: dark
            return frames.emplace(id, std::move(f)).first->second;
        };
        // A laser's flicker settings come with its look.
        auto flickerOf = [&](int id) -> const LaserState& {
            auto in = inputs.find(id);
            return in != inputs.end() ? in->second.s : snap;
        };

        // The test pattern exactly as in the ILDA file, sized and dimmed
        // (master brightness) - shared by the preview and every armed laser.
        std::vector<core::LaserPoint> testPoints;
        if (testPattern) {
            testPoints = test_pattern_points(testPattern->id);
            apply_test_pattern_output(testPoints, testPatternSize, snap.intensity);
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
            std::vector<PreviewFlicker> flickers;
            std::vector<int> testPatternLasers;
            std::vector<std::pair<int, std::size_t>> previewPoints; // laser id -> points in its look's frame
            previewFrames.reserve(lasers_snapshot.size());
            for (std::size_t li = 0; li < lasers_snapshot.size(); ++li) {
                const auto& l = lasers_snapshot[li];
                const LaserState& fl = flickerOf(l.id);
                flickers.push_back({l.id, fl.flicker_hz, fl.flicker_on_ms,
                                    flicker_phase_offset(li, fl.flicker_hz, fl.flicker_phase_degrees)});
                core::Frame pf = frameFor(l.id, li);
                bool previewAllowed = l.armed;
                if (testPattern && l.armed) {
                    pf.points = testPoints; previewAllowed = testPatternRunnable;
                    testPatternLasers.push_back(l.id);
                }
                previewPoints.emplace_back(l.id, frameFor(l.id, li).points.size());
                HardwareOutputParams pp = hardware_params(&l, snap, previewAllowed);
                pp.blackout = false; // preview ignores blackout (see above)
                finalize_hardware_frame(pf.points, pp);
                previewFrames.emplace_back(l.id, std::move(pf));
            }
            core::Frame mainPreview;
            const LaserState& mainFlicker = order.empty() ? flickerOf(kVirtualLook) : snap;
            if (lasers_snapshot.empty()) {
                mainPreview = frameFor(kVirtualLook, 0);
                HardwareOutputParams virt; virt.blackout = false; virt.outputAllowed = true; virt.gateLevel = g_gate_level;
                finalize_hardware_frame(mainPreview.points, virt);
            }
            if (order.empty()) previewPoints.emplace_back(kVirtualLook, frameFor(kVirtualLook, 0).points.size());
            {
                std::lock_guard<std::mutex> lk(G_mtx);
                for (const auto& [id, n] : previewPoints) { auto it = G_looks.find(id); if (it != G_looks.end()) it->second.frame_points = n; }
            }
            ws_broadcast(frame_to_ws_json(mainPreview, previewFrames, G_time,
                                          order.empty() ? mainFlicker.flicker_hz : 0.0f,
                                          order.empty() ? mainFlicker.flicker_on_ms : 0.0f, flickers,
                                          testPatternLasers, testPattern ? testPattern->kpps : 0.0f));
        }

        if (anyHardwareReady) {
            // Each controller gets its own laser's frame; global
            // blackout/gate and that laser's zone transform are then applied
            // only to that hardware-bound copy.
            for (auto& [id, rt] : runtimes) {
                if (!rt.ctrl) continue;
                if (!(rt.ctrl->isReadyForNewFrame() && (now_tp - rt.last_hw_send) >= 20ms)) continue;
                rt.last_hw_send = now_tp;

                const LaserConfig* cfg = nullptr;
                std::size_t laserIndex = 0; // Lasers-list order, laser 1 = 0 (phase offsets)
                for (std::size_t i = 0; i < lasers_snapshot.size(); ++i) {
                    if (lasers_snapshot[i].id == id) { cfg = &lasers_snapshot[i]; laserIndex = i; break; }
                }

                core::Frame toSend = frameFor(id, laserIndex);
                const LaserState& fl = flickerOf(id);
                // Test pattern instead of the show: only once this DAC really
                // runs at the pattern's rate (dark until then), never flickered.
                const bool showsTestPattern = testPattern && cfg && cfg->armed;
                bool outputAllowed = cfg && cfg->armed && rt.output_armed;
                if (showsTestPattern) {
                    toSend.points = testPoints;
                    outputAllowed = outputAllowed && testPatternRunnable &&
                                    test_pattern_rate_ok(rt.connected_rate, testPattern->kpps);
                }
                // Flicker, per point by playback time: pad the frame so the
                // DAC plays it once instead of looping it until the next one
                // arrives, then blank the points that fall into an "off"
                // phase of the continuous timeline (see show_fx.h).
                const double pointRateHz = std::max(1.0f, rt.connected_rate) * 1000.0;
                const bool flickering = fl.flicker_hz > 0.0f && !showsTestPattern;
                if (flickering) {
                    pad_frame_to_duration(toSend.points, pointRateHz, kFlickerMinFrameSeconds, kFlickerMaxFramePoints);
                    // Flicker phase: this laser runs its flicker that much later.
                    const double phaseOffset = flicker_phase_offset(laserIndex, fl.flicker_hz, fl.flicker_phase_degrees);
                    apply_flicker_timeline(toSend.points, rt.flicker.begin(G_time) + phaseOffset, pointRateHz,
                                           fl.flicker_hz, fl.flicker_on_ms);
                } else {
                    rt.flicker.reset();
                }
                // Blackout/arm gate, footswitch, mirror + zone, safety masks
                // and mask outlines - all in output_pipeline.h, where the
                // order is documented and covered by backend/tests/.
                // Only an armed laser may emit light - checked here too,
                // independent of the connection handling above and of
                // libera's own blanking.
                finalize_hardware_frame(toSend.points, hardware_params(cfg, snap, outputAllowed));
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
       << ",\"canvas\":" << (l.canvas ? "true" : "false")
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
           << ",\"canvas\":" << (l.canvas ? "true" : "false")
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
            l.canvas        = json_bool(obj, "canvas", false);
            const bool hasPreviewPosition = json_has(obj, "preview_x")
                && json_has(obj, "preview_y");
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
// One laser's complete state in a cue: its look (look.rotation_phase = its
// rotation angle), its animation state when it was saved (movement, wave and
// rainbow phase, ILDA time) and the cue layers playing on it (copies, with
// their own looks and animation) - so recalling brings back exactly that.
// Cues saved before this existed have no animation state / layers (has_*
// false): recalling them keeps the running animation and layers.
struct CueLaser {
    LaserState look;
    bool   has_anim = false;
    double move_phase = 0.0, wave_phase = 0.0, rainbow_phase = 0.0, ilda_time = 0.0;
    bool   has_layers = false;
    std::vector<ShowLayer> layers;
};
// A cue: the state of every laser it was saved on (laser id -> state) plus
// its main state - the first of them, or the only look of a cue saved before
// per-laser looks existed (or without any laser configured). Recalling it
// onto a laser gives that laser its own saved state, or the main state if
// the cue has none for it - so any cue can be put onto any laser.
struct CueData {
    CueLaser main;
    std::map<int, CueLaser> lasers;
    const CueLaser& for_laser(int id) const {
        auto it = lasers.find(id);
        return it != lasers.end() ? it->second : main;
    }
    // Every look in the cue, layers included.
    template <class F> void for_each_look(F fn) const {
        auto one = [&](const CueLaser& c) { fn(c.look); for (const auto& L : c.layers) fn(L.s); };
        one(main);
        for (const auto& [id, c] : lasers) one(c);
    }
};
static std::map<int, CueData> G_cues;
static std::mutex                G_cues_mtx;
static const std::string         CUES_FILE = resolve_data_file("cues.json");

// ── Central Beam settings (settings.json) ────────────────────────────────────
// The Beam page's projector settings are global (see with_global_settings()) and kept
// across daemon restarts in their own file in the app data directory - not in
// cues. Loaded once at startup; saved by a background thread within ~1 s of
// any change, no matter which input changed them (REST, MIDI, Magic Touch).
static const std::string SETTINGS_FILE = resolve_data_file("settings.json");
static std::string beam_settings_json(const LaserState& s) {
    std::ostringstream ss; ss << std::fixed << std::setprecision(4);
    ss << "{\"rate_kpps\":" << s.rate_kpps
       << ",\"max_rate_kpps\":" << s.max_rate_kpps
       << ",\"output_latency_ms\":" << s.output_latency_ms
       << ",\"dot_dwell_us\":" << s.dot_dwell_us
       << ",\"dot_blank_us\":" << s.dot_blank_us
       << "}";
    return ss.str();
}
static std::string G_settings_saved;
static void load_settings_from_disk() {
    std::ifstream f(SETTINGS_FILE);
    if (!f) return; // first run: defaults
    std::ostringstream buf; buf << f.rdbuf();
    const std::string b = buf.str();
    auto num = [&](const char* key, float def, float lo, float hi) {
        const float v = json_float(b, key, def);
        return std::isfinite(v) ? std::clamp(v, lo, hi) : def;
    };
    std::lock_guard<std::mutex> lk(G_mtx);
    G.max_rate_kpps     = num("max_rate_kpps", G.max_rate_kpps, 1, 100);
    G.rate_kpps         = num("rate_kpps", G.rate_kpps, 0, G.max_rate_kpps);
    G.output_latency_ms = num("output_latency_ms", G.output_latency_ms, 10, 200);
    G.dot_dwell_us      = num("dot_dwell_us", G.dot_dwell_us, 30, 3000);
    G.dot_blank_us      = num("dot_blank_us", G.dot_blank_us, 0, 3000);
    G_settings_saved = beam_settings_json(G);
    std::cout << "[laser_daemon] Loaded Beam settings from " << SETTINGS_FILE << "\n";
}
static void save_settings_if_changed() {
    std::string now;
    {
        std::lock_guard<std::mutex> lk(G_mtx);
        LaserState s = G;
        // A test pattern only borrows the scan rate (see G_test_pattern).
        if (G_rate_before_test_pattern >= 0.0f) s.rate_kpps = G_rate_before_test_pattern;
        now = beam_settings_json(s);
    }
    if (now == G_settings_saved) return;
    std::ofstream f(SETTINGS_FILE, std::ios::trunc);
    if (f) { f << now; G_settings_saved = now; }
}

static std::string cue_state_to_json(const LaserState& s) {
    std::ostringstream ss; ss << std::fixed << std::setprecision(4);
    ss << "{"
       << "\"shape\":\""       << s.shape         << "\","
       << "\"ilda_file\":\""   << s.ilda_file     << "\","
       << "\"ilda_name\":\""   << s.ilda_name     << "\","
       << "\"ilda_fps\":"      << s.ilda_fps      << ","
       << "\"text\":"          << json_escape(s.text) << ","
       << "\"font\":\""        << s.font          << "\","
       << "\"text_size\":"     << s.text_size     << ","
       << "\"text_spacing\":"  << s.text_spacing  << ","
       << "\"text_bold\":"     << (s.text_bold?"true":"false") << ","
       << "\"text_italic\":"   << (s.text_italic?"true":"false") << ","
       << "\"text_underline\":" << (s.text_underline?"true":"false") << ","
       << "\"radius\":"        << s.radius        << ","
       << "\"points\":"        << s.points        << ","
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
       << "\"stars_amount\":"  << s.stars_amount  << ","
       << "\"rainbow_amount\":" << s.rainbow_amount << ","
       << "\"rainbow_speed\":" << s.rainbow_speed << ","
       << "\"rainbow_phase_degrees\":" << s.rainbow_phase_degrees << ","
       << "\"sparkle_amount\":" << s.sparkle_amount << ","
       << "\"sparkle_intensity\":" << s.sparkle_intensity << ","
       << "\"sparkle_speed\":" << s.sparkle_speed << ","
       << "\"sparkle_r\":" << s.sparkle_r << ",\"sparkle_g\":" << s.sparkle_g << ",\"sparkle_b\":" << s.sparkle_b << ","
       << "\"dot_amount\":"    << s.dot_amount    << ","
       << "\"flicker_hz\":"    << s.flicker_hz    << ","
       << "\"flicker_on_ms\":" << s.flicker_on_ms << ","
       << "\"flicker_phase_degrees\":" << s.flicker_phase_degrees
       // The Beam page's projector settings (scan rate, max rate, output
       // latency, dot dwell/blanking) are global and never part of a cue -
       // see with_global_settings().
       << "}";
    return ss.str();
}

static LaserState cue_state_from_json(const std::string& obj) {
    LaserState s{};
    auto str = json_str(obj,"shape");     if(!str.empty()) s.shape = str;
    s.ilda_file       = json_str(obj,"ilda_file");
    if (!ilda_valid_storage_name(s.ilda_file)) s.ilda_file.clear();
    if (auto nm = json_str(obj,"ilda_name"); !nm.empty()) s.ilda_name = ilda_display_name(nm);
    if (std::string t; json_string(obj,"text",t)) s.text = t.substr(0, 4 * kMaxTextChars);
    if (auto f = json_str(obj,"font"); find_text_font(f)) s.font = f;
    s.text_size       = std::clamp(json_float(obj,"text_size",s.text_size),0.02f,2.0f);
    s.text_spacing    = std::clamp(json_float(obj,"text_spacing",s.text_spacing),-0.3f,2.0f);
    s.text_bold       = json_bool(obj,"text_bold",s.text_bold);
    s.text_italic     = json_bool(obj,"text_italic",s.text_italic);
    s.text_underline  = json_bool(obj,"text_underline",s.text_underline);
    s.ilda_fps        = std::clamp(json_float(obj,"ilda_fps",s.ilda_fps),1.0f,120.0f);
    s.radius          = json_float(obj,"radius",s.radius);
    s.points          = json_int(obj,"points",s.points);
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
    s.stars_amount    = std::clamp(json_int(obj,"stars_amount",s.stars_amount),1,kMaxStars);
    s.rainbow_amount  = json_float(obj,"rainbow_amount",s.rainbow_amount);
    s.rainbow_speed   = json_float(obj,"rainbow_speed",s.rainbow_speed);
    s.rainbow_phase_degrees = json_float(obj,"rainbow_phase_degrees",s.rainbow_phase_degrees);
    s.sparkle_amount    = std::clamp(json_float(obj,"sparkle_amount",s.sparkle_amount),0.0f,1.0f);
    s.sparkle_intensity = std::clamp(json_float(obj,"sparkle_intensity",s.sparkle_intensity),0.0f,1.0f);
    s.sparkle_speed     = std::clamp(json_float(obj,"sparkle_speed",s.sparkle_speed),0.0f,kSparkleMaxHz);
    s.sparkle_r = std::clamp(json_float(obj,"sparkle_r",s.sparkle_r),0.0f,1.0f);
    s.sparkle_g = std::clamp(json_float(obj,"sparkle_g",s.sparkle_g),0.0f,1.0f);
    s.sparkle_b = std::clamp(json_float(obj,"sparkle_b",s.sparkle_b),0.0f,1.0f);
    s.dot_amount      = std::clamp(json_float(obj,"dot_amount",s.dot_amount),0.0f,1.0f);
    s.flicker_hz      = std::max(0.0f, json_float(obj,"flicker_hz",s.flicker_hz));
    // Cues saved before the on time existed have no such key: keep the
    // default 0 (= auto, half the period), i.e. exactly how they looked.
    s.flicker_on_ms   = std::clamp(json_float(obj,"flicker_on_ms",0.0f),0.0f,kFlickerOnMsMax);
    s.flicker_phase_degrees = std::clamp(json_float(obj,"flicker_phase_degrees",0.0f),0.0f,360.0f);
    // rate_kpps stored by older versions is ignored on purpose: the scan
    // rate is a global projector setting.
    apply_legacy_shape(s);
    return s;
}

// {...main look..., "lasers":{"<id>":{...look...}, ...}} - older files have
// no "lasers" (one look for all lasers).
// {...look..., "move_phase", "wave_phase", "rainbow_phase", "ilda_time",
//  "layers":[{...layer look..., "cue":n, phases}, ...]}
static std::string cue_laser_to_json(const CueLaser& c) {
    auto num = [](double v) { std::ostringstream ss; ss << std::fixed << std::setprecision(6) << v; return ss.str(); };
    std::string out = cue_state_to_json(c.look);
    out.pop_back(); // '}'
    if (c.has_anim)
        out += ",\"move_phase\":" + num(c.move_phase) + ",\"wave_phase\":" + num(c.wave_phase) +
               ",\"rainbow_phase\":" + num(c.rainbow_phase) + ",\"ilda_time\":" + num(c.ilda_time);
    if (c.has_layers) {
        out += ",\"layers\":[";
        for (std::size_t i = 0; i < c.layers.size(); ++i) {
            const ShowLayer& L = c.layers[i];
            LaserState look = L.s;
            look.rotation_phase = L.rotation_phase; // the layer's live angle
            std::string l = cue_state_to_json(look);
            l.pop_back();
            out += (i ? "," : "") + l + ",\"cue\":" + std::to_string(L.cue) + ",\"move_phase\":" + num(L.move_phase) +
                   ",\"wave_phase\":" + num(L.wave_phase) + ",\"rainbow_phase\":" + num(L.rainbow_phase) +
                   ",\"ilda_time\":" + num(L.ilda_time) + "}";
        }
        out += "]";
    }
    return out + "}";
}
static CueLaser cue_laser_from_json(const std::string& obj) {
    CueLaser c;
    std::string own = obj;
    std::size_t b = 0, e = 0, open = 0;
    if (json_value_span(obj, "layers", b, e, open) && obj[open] == '[') {
        c.has_layers = true;
        const std::size_t close = e - 1;
        for (std::size_t i = obj.find('{', open); i != std::string::npos && i < close; i = obj.find('{', i)) {
            const std::size_t end = json_match(obj, i);
            if (end == std::string::npos || end > close) break;
            const std::string lo = obj.substr(i, end - i + 1);
            ShowLayer L;
            L.s = cue_state_from_json(lo);
            L.cue = json_int(lo, "cue", 0);
            L.rotation_phase = L.s.rotation_phase;
            L.move_phase = json_float(lo, "move_phase", 0.0f);
            L.wave_phase = json_float(lo, "wave_phase", 0.0f);
            L.rainbow_phase = json_float(lo, "rainbow_phase", 0.0f);
            L.ilda_time = json_float(lo, "ilda_time", 0.0f);
            if (c.layers.size() < MAX_SHOW_LAYERS) c.layers.push_back(L);
            i = end + 1;
        }
        own = obj.substr(0, b) + obj.substr(e); // the laser's own keys only
    }
    c.look = cue_state_from_json(own);
    c.has_anim = json_has(own, "move_phase");
    if (c.has_anim) {
        c.move_phase = json_float(own, "move_phase", 0.0f);
        c.wave_phase = json_float(own, "wave_phase", 0.0f);
        c.rainbow_phase = json_float(own, "rainbow_phase", 0.0f);
        c.ilda_time = json_float(own, "ilda_time", 0.0f);
    }
    return c;
}
// {...main laser state..., "lasers":{"<id>":{...laser state...}, ...}} -
// older files have no "lasers" (one look for all lasers).
static std::string cue_to_json(const CueData& c) {
    std::string out = cue_laser_to_json(c.main);
    out.pop_back(); // '}'
    out += ",\"lasers\":{";
    bool first = true;
    for (const auto& [id, l] : c.lasers) {
        out += (first ? "\"" : ",\"") + std::to_string(id) + "\":" + cue_laser_to_json(l);
        first = false;
    }
    return out + "}}";
}
static CueData cue_from_json(const std::string& obj) {
    CueData c;
    std::string mainPart = obj;
    std::size_t b = 0, e = 0, open = 0;
    if (json_value_span(obj, "lasers", b, e, open) && obj[open] == '{') {
        const std::size_t close = e - 1;
        std::size_t i = open + 1;
        while (true) {
            const auto q1 = obj.find('"', i);      if (q1 == std::string::npos || q1 > close) break;
            const auto q2 = obj.find('"', q1 + 1); if (q2 == std::string::npos || q2 > close) break;
            const auto ob = obj.find('{', q2);     if (ob == std::string::npos || ob > close) break;
            const auto oe = json_match(obj, ob);   if (oe == std::string::npos || oe > close) break;
            try { c.lasers[std::stoi(obj.substr(q1 + 1, q2 - q1 - 1))] = cue_laser_from_json(obj.substr(ob, oe - ob + 1)); }
            catch (...) {}
            i = oe + 1;
        }
        mainPart = obj.substr(0, b) + obj.substr(e); // the main state's own keys only
    }
    c.main = cue_laser_from_json(mainPart);
    return c;
}

static void save_cues_to_disk() {
    std::lock_guard<std::mutex> lk(G_cues_mtx);
    std::ofstream f(CUES_FILE, std::ios::trunc);
    if (!f) return;
    f << "{";
    bool first = true;
    for (auto& [num, c] : G_cues) {
        if (!first) f << ",";
        first = false;
        f << "\"" << num << "\":" << cue_to_json(c);
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
        const auto closeBrace = json_match(body, brace); // skips strings (texts may contain braces)
        if (closeBrace == std::string::npos) break; // malformed, bail out
        const std::size_t j = closeBrace + 1;
        try {
            int num = std::stoi(key);
            if (num >= 1 && num <= MAX_CUES)
                G_cues[num] = cue_from_json(body.substr(brace, j - brace));
        } catch (...) {}
        i = j;
    }
    std::cout << "[laser_daemon] Loaded " << G_cues.size() << " cue(s) from " << CUES_FILE << "\n";
}

// Shared cue operations - used by both the HTTP /api/cue/* routes and the
// MIDI dispatcher below, so there's exactly one place that knows the correct
// (deadlock-safe) lock order: G_mtx and G_cues_mtx are NEVER held at the same
// time by any of these.
using LaserTargets = std::optional<std::vector<int>>; // nullopt = every look (see look_targets())

// A laser's complete current state for a cue (see CueLaser). G_mtx held.
static CueLaser capture_cue_laser(const LookSlot& sl) {
    CueLaser c;
    c.look = sl.look;
    c.look.rotation_phase = sl.current_angle();
    c.has_anim = true;
    c.move_phase = sl.anim_pending ? sl.pending_move_phase : sl.move_phase;
    c.wave_phase = sl.anim_pending ? sl.pending_wave_phase : sl.wave_phase;
    c.rainbow_phase = sl.anim_pending ? sl.pending_rainbow_phase : sl.rainbow_phase;
    c.ilda_time = sl.anim_pending ? sl.pending_ilda_time : sl.ilda_time;
    c.has_layers = true;
    c.layers = sl.layers;
    return c;
}
// Puts a saved state onto a laser: instantly (no glide), at its saved
// rotation angle and animation state, with its saved layers. G_mtx held.
static void apply_cue_laser(LookSlot& sl, const CueLaser& c) {
    sl.look = c.look;
    sl.snap_all = true;
    sl.request_angle(c.look.rotation_phase);
    if (c.has_anim) {
        sl.anim_pending = true;
        sl.pending_move_phase = c.move_phase;
        sl.pending_wave_phase = c.wave_phase;
        sl.pending_rainbow_phase = c.rainbow_phase;
        sl.pending_ilda_time = c.ilda_time;
    }
    if (c.has_layers) sl.layers = c.layers;
}
// A plain look (reset, defaults): no saved animation, layers untouched.
static void set_look(LookSlot& sl, const LaserState& look) {
    CueLaser c;
    c.look = look;
    apply_cue_laser(sl, c);
}

// Saves everything about the target lasers into slot n (see CueData):
// per laser its look, rotation angle, animation state and active layers.
static bool do_cue_save(int n, const LaserTargets& targets = std::nullopt) {
    if (n < 1 || n > MAX_CUES) return false;
    CueData cue;
    {
        const std::vector<int> order = laser_order();
        std::lock_guard<std::mutex> lk(G_mtx);
        const std::vector<int> ids = look_targets(targets, order);
        if (ids.empty()) return false;
        for (int id : ids) {
            const CueLaser c = capture_cue_laser(G_looks.at(id));
            if (id == ids.front()) cue.main = c;
            if (id != kVirtualLook) cue.lasers[id] = c;
        }
    }
    { std::lock_guard<std::mutex> lk(G_cues_mtx); G_cues[n] = cue; }
    save_cues_to_disk();
    return true;
}
// Recalls slot n onto the target lasers (see CueData): each gets exactly its
// saved state back - look, angle, animation, layers. Lasers that aren't
// targeted keep theirs; global settings never change.
static bool do_cue_recall(int n, const LaserTargets& targets = std::nullopt) {
    if (n < 1 || n > MAX_CUES) return false;
    CueData cue;
    { std::lock_guard<std::mutex> lk(G_cues_mtx);
      auto it = G_cues.find(n);
      if (it == G_cues.end()) return false;
      cue = it->second; }
    const std::vector<int> order = laser_order();
    std::lock_guard<std::mutex> lk(G_mtx);
    for (int id : look_targets(targets, order)) apply_cue_laser(G_looks.at(id), cue.for_laser(id));
    return true;
}
// Cue layers (see ShowLayer) on the target lasers: mode 1 = on, 0 = off,
// 2 = toggle (decided by the first target, then the same for all). Turning a
// layer on starts it fresh at the cue's saved rotation angle; at most
// MAX_SHOW_LAYERS per laser (the oldest is dropped). Returns false for an
// empty cue slot (turning off always works).
static bool do_cue_layer(int n, int mode, const LaserTargets& targets = std::nullopt) {
    if (n < 1 || n > MAX_CUES) return false;
    CueData cue; bool found = false;
    { std::lock_guard<std::mutex> lk(G_cues_mtx);
      auto it = G_cues.find(n);
      if (it != G_cues.end()) { cue = it->second; found = true; } }
    const std::vector<int> order = laser_order();
    std::lock_guard<std::mutex> lk(G_mtx);
    const std::vector<int> ids = look_targets(targets, order);
    if (ids.empty()) return true;
    auto findLayer = [&](LookSlot& sl) {
        return std::find_if(sl.layers.begin(), sl.layers.end(), [&](const ShowLayer& L) { return L.cue == n; });
    };
    LookSlot& firstSlot = G_looks.at(ids.front());
    const bool wantOn = mode == 2 ? findLayer(firstSlot) == firstSlot.layers.end() : mode == 1;
    if (wantOn && !found) return false;
    for (int id : ids) {
        LookSlot& sl = G_looks.at(id);
        auto it = findLayer(sl);
        if (!wantOn) { if (it != sl.layers.end()) sl.layers.erase(it); continue; }
        if (it != sl.layers.end()) continue;
        ShowLayer L;
        // The cue's look for this laser (a cue's own layers don't stack in).
        L.cue = n; L.s = cue.for_laser(id).look;
        L.rotation_phase = L.s.rotation_phase;
        sl.layers.push_back(L);
        if (sl.layers.size() > MAX_SHOW_LAYERS) sl.layers.erase(sl.layers.begin());
    }
    return true;
}
static void do_layers_clear(const LaserTargets& targets = std::nullopt) {
    for_target_looks(targets, [](LookSlot& sl) { sl.layers.clear(); });
}

// ── ILDA file store (REST: /api/ilda..., /api/cue/<n>/ilda) ──────────────────
// Files live in ilda_dir() under their content-hash name (see ilda_clip());
// the original file name of the last upload sits next to it in
// "<hash>.name" (display only - cues keep their own copy in ilda_name).
static constexpr std::size_t kMaxIldaBytes = 64u * 1024u * 1024u;

static std::string ilda_stored_name(const std::string& file) {
    std::ifstream f(ilda_dir() + "/" + file.substr(0, 16) + ".name");
    std::string name; std::getline(f, name);
    return name.empty() ? std::string() : ilda_display_name(name);
}

struct IldaStoreResult {
    int status = 200;          // HTTP status, 200 = stored
    std::string error;         // set when status != 200
    std::string file, name;    // storage name, display name
    std::size_t frames = 0;
};
// Checks an uploaded file (must parse as ILDA with at least one frame),
// stores it (once - the same bytes always get the same name) and caches it.
static IldaStoreResult ilda_store_upload(const std::string& body, const std::string& originalName) {
    IldaStoreResult r;
    if (body.empty() || body.size() > kMaxIldaBytes) { r.status = 400; r.error = "empty or too large file (max 64 MB)"; return r; }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(body.data());
    std::vector<IldaFrame> frames;
    if (!parse_ilda(bytes, body.size(), frames) || frames.empty()) { r.status = 400; r.error = "not a valid ILDA file"; return r; }
    r.file = ilda_storage_name(bytes, body.size());
    const std::string path = ilda_dir() + "/" + r.file;
    if (!std::filesystem::exists(path)) {
        const std::string tmp = path + ".tmp";
        { std::ofstream out(tmp, std::ios::binary | std::ios::trunc); out.write(body.data(), (std::streamsize)body.size()); }
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (ec) { r.status = 500; r.error = "could not store the file"; return r; }
    }
    r.name = ilda_display_name(originalName);
    if (!originalName.empty()) { std::ofstream(ilda_dir() + "/" + r.file.substr(0, 16) + ".name", std::ios::trunc) << r.name << "\n"; }
    else if (const std::string stored = ilda_stored_name(r.file); !stored.empty()) r.name = stored;
    r.frames = frames.size();
    ilda_cache_put(r.file, std::move(frames));
    return r;
}

// Cue numbers whose look plays `file`.
static std::vector<int> ilda_used_by(const std::string& file) {
    std::vector<int> out;
    std::lock_guard<std::mutex> lk(G_cues_mtx);
    for (const auto& [num, c] : G_cues) {
        bool uses = false;
        c.for_each_look([&](const LaserState& l) { uses = uses || (l.shape == "ilda" && l.ilda_file == file); });
        if (uses) out.push_back(num);
    }
    return out;
}

// Display name of a stored file: from its upload, else from a cue playing it.
static std::string ilda_known_name(const std::string& file) {
    std::string name = ilda_stored_name(file);
    if (!name.empty()) return name;
    std::lock_guard<std::mutex> lk(G_cues_mtx);
    std::string found;
    for (const auto& [num, c] : G_cues)
        c.for_each_look([&](const LaserState& l) {
            if (found.empty() && l.shape == "ilda" && l.ilda_file == file && !l.ilda_name.empty()) found = l.ilda_name;
        });
    return found;
}

// Slot n becomes a fresh ILDA look playing `file` (default position/scale,
// no movement - tweak and Save to change), replacing the slot, and is
// recalled right away onto the target lasers.
static void ilda_cue_assign(int n, const std::string& file, const std::string& name, const LaserTargets& targets) {
    CueData cue;
    cue.main.look.shape = "ilda";
    cue.main.look.ilda_file = file;
    cue.main.look.ilda_name = name;
    { std::lock_guard<std::mutex> lk(G_cues_mtx); G_cues[n] = cue; }
    save_cues_to_disk();
    do_cue_recall(n, targets);
    std::cout << "[ilda] cue " << n << " <- " << name << " (" << file << ")\n" << std::flush;
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
    bool found = false; CueData snap;
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
    // Look parameters go to every laser's look (MIDI has no laser selection
    // yet); intensity, flash release, dot timing and scan rate are global.
    if (action=="r")              { for_all_looks([&](LookSlot& sl) { sl.look.r=std::clamp(out,0.0f,1.0f); }); return; }
    if (action=="g")              { for_all_looks([&](LookSlot& sl) { sl.look.g=std::clamp(out,0.0f,1.0f); }); return; }
    if (action=="b")              { for_all_looks([&](LookSlot& sl) { sl.look.b=std::clamp(out,0.0f,1.0f); }); return; }
    if (action=="intensity")      { std::lock_guard<std::mutex> lk(G_mtx); G.intensity=std::clamp(out,0.0f,1.0f); g_brightness_gate_open.store(true); return; }
    if (action=="flash_release_ms") { std::lock_guard<std::mutex> lk(G_mtx); G.flash_release_ms=std::clamp(out,0.0f,2000.0f); return; }
    if (action=="shape_scale")    { for_all_looks([&](LookSlot& sl) { sl.look.shape_scale=std::clamp(out,-1.0f,1.0f); }); return; }
    if (action=="rotation_speed") { for_all_looks([&](LookSlot& sl) { sl.look.rotation_speed=out; }); return; }
    if (action=="pos_x")          { for_all_looks([&](LookSlot& sl) { sl.look.pos_x=std::clamp(out,-1.0f,1.0f); }); return; }
    if (action=="pos_y")          { for_all_looks([&](LookSlot& sl) { sl.look.pos_y=std::clamp(out,-1.0f,1.0f); }); return; }
    if (action=="dot_amount")     { for_all_looks([&](LookSlot& sl) { sl.look.dot_amount=std::clamp(out,0.0f,1.0f); }); return; }
    if (action=="flicker_hz")     { for_all_looks([&](LookSlot& sl) { sl.look.flicker_hz=std::max(0.0f,out); }); return; }
    if (action=="flicker_on_ms")  { for_all_looks([&](LookSlot& sl) { sl.look.flicker_on_ms=std::clamp(out,0.0f,kFlickerOnMsMax); }); return; }
    if (action=="dot_dwell_us")   { std::lock_guard<std::mutex> lk(G_mtx); G.dot_dwell_us=std::clamp(out,30.0f,3000.0f); return; }
    if (action=="dot_blank_us")   { std::lock_guard<std::mutex> lk(G_mtx); G.dot_blank_us=std::clamp(out,0.0f,3000.0f); return; }
    if (action=="flicker_phase_degrees") { for_all_looks([&](LookSlot& sl) { sl.look.flicker_phase_degrees=std::clamp(out,0.0f,360.0f); }); return; }
    if (action=="wave_frequency") { for_all_looks([&](LookSlot& sl) { sl.look.wave_frequency=std::max(0.1f,out); }); return; }
    if (action=="wave_amplitude") { for_all_looks([&](LookSlot& sl) { sl.look.wave_amplitude=std::clamp(out,0.0f,1.0f); }); return; }
    if (action=="wave_speed")     { for_all_looks([&](LookSlot& sl) { sl.look.wave_speed=out; }); return; }
    if (action=="stars_amount")   { for_all_looks([&](LookSlot& sl) { sl.look.stars_amount=std::clamp((int)std::lround(out),1,kMaxStars); }); return; }
    if (action=="rainbow_amount") { for_all_looks([&](LookSlot& sl) { sl.look.rainbow_amount=std::clamp(out,0.0f,1.0f); }); return; }
    if (action=="rainbow_speed")  { for_all_looks([&](LookSlot& sl) { sl.look.rainbow_speed=out; }); return; }
    if (action=="sparkle_amount") { for_all_looks([&](LookSlot& sl) { sl.look.sparkle_amount=std::clamp(out,0.0f,1.0f); }); return; }
    if (action=="sparkle_intensity") { for_all_looks([&](LookSlot& sl) { sl.look.sparkle_intensity=std::clamp(out,0.0f,1.0f); }); return; }
    if (action=="sparkle_speed")  { for_all_looks([&](LookSlot& sl) { sl.look.sparkle_speed=std::clamp(out,0.0f,kSparkleMaxHz); }); return; }
    if (action=="rainbow_phase_degrees") { for_all_looks([&](LookSlot& sl) { sl.look.rainbow_phase_degrees=std::clamp(out,0.0f,360.0f); }); return; }
    if (action=="move_size")      { for_all_looks([&](LookSlot& sl) { sl.look.move_size=std::clamp(out,0.0f,6.0f); }); return; }
    if (action=="move_speed")     { for_all_looks([&](LookSlot& sl) { sl.look.move_speed=std::max(0.0f,out); }); return; }
    if (action=="move_phase_degrees") { for_all_looks([&](LookSlot& sl) { sl.look.move_phase_degrees=std::clamp(out,0.0f,360.0f); }); return; }
    if (action=="rate_kpps")      { std::lock_guard<std::mutex> lk(G_mtx); G.rate_kpps=std::clamp(out,0.0f,G.max_rate_kpps); return; }
}

// State for the momentary (hold-to-preview) cue buttons and the momentary
// flash button - both need to remember what to restore on release.
static bool      g_midi_cue_save_armed = false;
static std::map<int, CueLaser> g_cue_momentary_snapshot; // laser id -> its complete state before the first momentary press
// Guards g_cue_momentary_snapshot (MIDI thread + HTTP threads).
// Lock order: always taken *before* G_mtx / G_cues_mtx, never inside them.
static std::mutex g_cue_momentary_mtx;

// Momentary cue (active only while held) - shared by the MIDI
// "cue_momentary:<n>" action and POST /api/cue/<n>/momentary/<0|1>. The first press remembers the looks (and rotation angles)
// of the lasers it's pressed on, the final release puts them back. Several
// held at once (multitouch) only remember each laser once, the look from
// before the first press. Global settings aren't touched.
static bool do_cue_momentary_press(int n, const LaserTargets& targets = std::nullopt) {
    std::lock_guard<std::mutex> mlk(g_cue_momentary_mtx);
    {
        const std::vector<int> order = laser_order();
        std::lock_guard<std::mutex> lk(G_mtx);
        for (int id : look_targets(targets, order))
            if (!g_cue_momentary_snapshot.count(id)) g_cue_momentary_snapshot[id] = capture_cue_laser(G_looks.at(id));
    }
    return do_cue_recall(n, targets);
}
static void do_cue_momentary_release() {
    std::lock_guard<std::mutex> mlk(g_cue_momentary_mtx);
    std::lock_guard<std::mutex> lk(G_mtx);
    for (const auto& [id, saved] : g_cue_momentary_snapshot) {
        auto it = G_looks.find(id);
        if (it != G_looks.end()) apply_cue_laser(it->second, saved); // back instantly, exactly as it was
    }
    g_cue_momentary_snapshot.clear();
}
static bool       g_motion_held = false;        // for "motion_hold" (freeze movement while held)
// g_flash_active is declared earlier so make_frame() can see it too.

// Shared flash press/release - used by both the MIDI dispatcher and the
// /flash/<0|1> HTTP route, so a flash triggered from either input behaves
// identically: every laser shows its look in full white at full brightness
// while held (applied when drawing - look_render_input() - so no look's own
// color or the master brightness is changed), and goes back on release.
static void do_flash_press() {
    std::lock_guard<std::mutex> lk(G_mtx);
    if (g_flash_active) return;
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
    // Flash's own release is always instant - flash_release_ms only drives
    // the footswitch gate's close-fade (see g_gate_level), not this.
    g_flash_active = false;
}

// Shared motion_hold press/release - used by both the MIDI dispatcher and
// the /motion/hold/<0|1> HTTP route: stops the movement pattern, rotation,
// *and* the rainbow hue cycle in place on every laser while held, resumes
// all three on release.
static void do_motion_hold_press() {
    {
        std::lock_guard<std::mutex> lk(G_mtx);
        if (g_motion_held) return;
        g_motion_held = true;
        G_motion_frozen.store(true); // cue layers freeze too
    }
    for_all_looks([](LookSlot& sl) {
        sl.pre_motion_move_speed = sl.look.move_speed;         sl.look.move_speed = 0.0f;
        sl.pre_motion_rotation_speed = sl.look.rotation_speed; sl.look.rotation_speed = 0.0f;
        sl.pre_motion_rainbow_speed = sl.look.rainbow_speed;   sl.look.rainbow_speed = 0.0f;
        sl.snap_speeds = true; // freeze instantly, don't glide to a stop
    });
}
static void do_motion_hold_release() {
    {
        std::lock_guard<std::mutex> lk(G_mtx);
        if (!g_motion_held) return;
        g_motion_held = false;
        G_motion_frozen.store(false);
    }
    for_all_looks([](LookSlot& sl) {
        sl.look.move_speed = sl.pre_motion_move_speed;
        sl.look.rotation_speed = sl.pre_motion_rotation_speed;
        sl.look.rainbow_speed = sl.pre_motion_rainbow_speed;
    });
}

// One-shot rotation reset - used by both the MIDI "rotation_reset" note
// action and the /rotation/reset REST route: snaps the rotation angle back
// to 0 and stops it from spinning further (rotation_speed to 0), rather
// than the momentary hold/restore pattern the other do_* helpers use.
static void do_rotation_reset(const LaserTargets& targets = std::nullopt) {
    for_target_looks(targets, [](LookSlot& sl) {
        sl.look.rotation_speed = 0.0f;
        sl.request_angle(0.0); // angle 0, stop instantly (no glide)
    });
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
        static const std::set<std::string> SHAPES{"circle","line","triangle","square","wave","stars","text","staticwave"};
        std::string s = action.substr(6);
        if (SHAPES.count(s)) for_all_looks([&](LookSlot& sl) { sl.look.shape = s; apply_legacy_shape(sl.look); });
        return;
    }
    if (action.rfind("move:",0)==0) {
        // See shape:'s comment above - same alternating-LED pad behavior.
        static const std::set<std::string> MOVES{"none","circle","pan","tilt","tiltup","tiltdown","eight","random"};
        std::string m = action.substr(5);
        if (MOVES.count(m)) for_all_looks([&](LookSlot& sl) { sl.look.move_mode = m; });
        return;
    }
    if (action.rfind("color:",0)==0) {
        // See shape:'s comment above - same alternating-LED pad behavior.
        std::string c = action.substr(6);
        float cr,cg,cb;
        if (!named_color_rgb(c, cr, cg, cb)) return;
        // A plain color-select button is mutually exclusive with the
        // rainbow preset (matches the original mapping's "colors" exclusive
        // group, which included both) - picking a solid color should always
        // show that color immediately, not stay hidden behind a still-active
        // rainbow blend.
        for_all_looks([&](LookSlot& sl) { sl.look.r = cr; sl.look.g = cg; sl.look.b = cb; sl.look.rainbow_amount = 0.0f; });
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
        for_all_looks([](LookSlot& sl) { sl.look.rainbow_amount = 1.0f; sl.look.rainbow_speed = 0.15f; });
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
        if (isPress) for_all_looks([](LookSlot& sl) { sl.look.mirror_x = true; });
        else if (isRelease) for_all_looks([](LookSlot& sl) { sl.look.mirror_x = false; });
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
    // Cue layers: "cue_layer:<n>" toggles cue n as a layer on top of the
    // current look on every press; "cue_layer_flash:<n>" plays it as a layer
    // only while held. "layers_clear" releases all layers.
    if (action.rfind("cue_layer:",0)==0) {
        int n = 0; try { n = std::stoi(action.substr(10)); } catch (...) { return; }
        if (isPress) do_cue_layer(n, 2);
        return;
    }
    if (action.rfind("cue_layer_flash:",0)==0) {
        int n = 0; try { n = std::stoi(action.substr(16)); } catch (...) { return; }
        if (isPress) do_cue_layer(n, 1); else if (isRelease) do_cue_layer(n, 0);
        return;
    }
    if (action=="layers_clear") { if (isPress) do_layers_clear(); return; }
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
    LaserState snap;
    {
        const std::vector<int> order = laser_order();
        std::lock_guard<std::mutex> lk(G_mtx);
        sync_look_slots(order);
        snap = G_looks.at(primary_look_id(order)).look; // the first laser's look
    }

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

// Apply a body key to `T_` - G for global settings, a laser's look for look fields.
#define APPLY_F(key, field) { float _v=json_float(req.body,key,-9999); if(_v!=-9999) T_.field=_v; }
#define APPLY_S(key, field) { auto _v=json_str(req.body,key); if(!_v.empty()) T_.field=_v; }
#define APPLY_B(key, field) { if(json_has(req.body,key)) T_.field=json_bool(req.body,key,T_.field); }
#define APPLY_CLAMP(key, field, lo, hi) { float _v=json_float(req.body,key,-9999); if(_v!=-9999) T_.field=std::clamp(_v,(float)(lo),(float)(hi)); }

// ── main ───────────────────────────────────────────────────────────────────────
// ── Shapes and the parameters they use ─────────────────────────────────────
// Which shape-dependent look parameters each shape uses - the single source
// for the UI (GET /api/shapes): controls no selected laser's shape uses are
// greyed out, shape options (wave, ILDA, ...) only shown for shapes in the
// selection. Parameters not in kShapeDependentParams (position, rotation,
// movement, scale, mirror, flicker) work with every shape. A new shape only
// needs a row here. `selectable` = offered in the shape picker (an ILDA look
// comes from dropping a file onto a cue).
struct ShapeInfo { std::string id, label; bool selectable; std::vector<std::string> params; };
static const std::vector<std::string> kShapeDependentParams = {
    "radius", "points", "r", "g", "b", "rainbow_amount", "rainbow_speed", "rainbow_phase_degrees", "dot_amount",
    "wave_frequency", "wave_amplitude", "wave_speed", "stars_amount", "ilda_fps",
    "text", "font", "text_size", "text_spacing", "text_bold", "text_italic", "text_underline",
};
static const std::vector<ShapeInfo>& shape_catalog() {
    static const std::vector<ShapeInfo> c = [] {
        const std::vector<std::string> outline = {"radius", "points", "r", "g", "b", "rainbow_amount", "rainbow_speed",
                                                  "rainbow_phase_degrees", "dot_amount"};
        auto with = [&](std::vector<std::string> extra) { auto v = outline; v.insert(v.end(), extra.begin(), extra.end()); return v; };
        return std::vector<ShapeInfo>{
            {"circle", "Circle", true, outline},
            {"line", "Line", true, outline},
            {"triangle", "Triangle", true, outline},
            {"square", "Square", true, outline},
            {"wave", "Wave", true, with({"wave_frequency", "wave_amplitude", "wave_speed"})},
            {"stars", "Stars", true, {"radius", "r", "g", "b", "rainbow_amount", "rainbow_speed", "rainbow_phase_degrees",
                                      "stars_amount"}},
            {"text", "Text", true, {"r", "g", "b", "rainbow_amount", "rainbow_speed", "rainbow_phase_degrees",
                                    "text", "font", "text_size", "text_spacing", "text_bold", "text_italic", "text_underline"}},
            {"ilda", "ILDA file", false, {"ilda_fps"}}, // the file's own colors, points and size
        };
    }();
    return c;
}
static std::string shapes_json() {
    std::ostringstream ss;
    auto list = [&](const std::vector<std::string>& v) {
        ss << "[";
        for (std::size_t i = 0; i < v.size(); ++i) ss << (i ? "," : "") << '"' << v[i] << '"';
        ss << "]";
    };
    ss << "{\"dependent\":";
    list(kShapeDependentParams);
    ss << ",\"shapes\":[";
    const auto& c = shape_catalog();
    for (std::size_t i = 0; i < c.size(); ++i) {
        ss << (i ? "," : "") << "{\"id\":\"" << c[i].id << "\",\"label\":\"" << c[i].label
           << "\",\"selectable\":" << (c[i].selectable ? "true" : "false") << ",\"params\":";
        list(c[i].params);
        ss << "}";
    }
    ss << "]}";
    return ss.str();
}

// The lasers a request targets: ?lasers=2,3 or a "lasers":[2,3] array in a
// JSON body - the browser's laser selection. None = every look (see
// look_targets()).
static LaserTargets request_lasers(const httplib::Request& req, bool fromBody = true) {
    std::vector<int> ids;
    if (req.has_param("lasers")) {
        std::stringstream ss(req.get_param_value("lasers"));
        std::string tok;
        while (std::getline(ss, tok, ',')) { try { if (!tok.empty()) ids.push_back(std::stoi(tok)); } catch (...) {} }
        return ids;
    }
    if (fromBody && json_int_array(req.body, "lasers", ids)) return ids;
    return std::nullopt;
}

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

    seed_ilda_files();
    load_cues_from_disk();
    {
        // Load every ILDA file a cue uses now, not on the laser thread's
        // first frame. Stored files are never deleted automatically (a
        // broken cues.json must not cost the files); releases only ship the
        // ones a cue uses (packaging/sync_default_data.sh).
        std::set<std::string> used;
        { std::lock_guard<std::mutex> lk(G_cues_mtx);
          for (const auto& [num, c] : G_cues)
              c.for_each_look([&](const LaserState& l) { if (!l.ilda_file.empty()) used.insert(l.ilda_file); }); }
        for (const auto& f : used) ilda_clip(f);
    }
    load_lasers_from_disk();
    load_magic_from_disk();
    load_settings_from_disk();

    std::thread laser_t(laser_thread);
    // Persists the central Beam settings shortly after any change.
    std::thread settings_t([]() {
        while (G_running) { std::this_thread::sleep_for(1s); save_settings_if_changed(); }
        save_settings_if_changed();
    });
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
    // Global settings go to G; look fields go to the look of every laser in
    // `lasers` (the browser's selection), or of every laser without it.
    svr.Post("/api/state",[](const httplib::Request& req,httplib::Response& res){
        const LaserTargets targets = request_lasers(req);
        { // scope the lock so it's released before state_to_json() re-locks G_mtx
        std::lock_guard<std::mutex> lk(G_mtx);
        LaserState& T_ = G;
        APPLY_F("max_rate_kpps",max_rate_kpps)  APPLY_CLAMP("max_rate_kpps",max_rate_kpps,1,100)
        APPLY_F("output_latency_ms",output_latency_ms) APPLY_CLAMP("output_latency_ms",output_latency_ms,10,200)
        APPLY_F("dot_dwell_us",dot_dwell_us) APPLY_CLAMP("dot_dwell_us",dot_dwell_us,30,3000)
        APPLY_F("dot_blank_us",dot_blank_us) APPLY_CLAMP("dot_blank_us",dot_blank_us,0,3000)
        APPLY_F("rate_kpps",rate_kpps)  APPLY_CLAMP("rate_kpps",rate_kpps,0,G.max_rate_kpps)
        APPLY_F("intensity",intensity)  APPLY_CLAMP("intensity",intensity,0,1)
        // Directly setting the brightness fader (any source: UI, REST, MIDI)
        // implicitly opens the footswitch gate too, so it's visible right
        // away without needing the pedal held - see g_brightness_gate_open's
        // comment for the full footswitch/fader interaction.
        if (json_float(req.body,"intensity",-9999) != -9999) g_brightness_gate_open.store(true);
        APPLY_F("flash_release_ms",flash_release_ms) APPLY_CLAMP("flash_release_ms",flash_release_ms,0,2000)
        APPLY_B("blackout",blackout)
        } // lock released here
        for_target_looks(targets, [&](LookSlot& sl) {
            LaserState& T_ = sl.look;
            APPLY_S("shape",shape)  APPLY_S("move_mode",move_mode)
            apply_legacy_shape(T_);
            APPLY_F("radius",radius)  APPLY_CLAMP("radius",radius,0,1)
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
            APPLY_F("stars_amount",stars_amount) APPLY_CLAMP("stars_amount",stars_amount,1,kMaxStars)
            APPLY_F("rainbow_amount",rainbow_amount) APPLY_CLAMP("rainbow_amount",rainbow_amount,0,1)
            APPLY_F("rainbow_speed",rainbow_speed)
            APPLY_F("rainbow_phase_degrees",rainbow_phase_degrees) APPLY_CLAMP("rainbow_phase_degrees",rainbow_phase_degrees,0,360)
            APPLY_F("sparkle_amount",sparkle_amount) APPLY_CLAMP("sparkle_amount",sparkle_amount,0,1)
            APPLY_F("sparkle_intensity",sparkle_intensity) APPLY_CLAMP("sparkle_intensity",sparkle_intensity,0,1)
            APPLY_F("sparkle_speed",sparkle_speed) APPLY_CLAMP("sparkle_speed",sparkle_speed,0,kSparkleMaxHz)
            APPLY_F("sparkle_r",sparkle_r) APPLY_CLAMP("sparkle_r",sparkle_r,0,1)
            APPLY_F("sparkle_g",sparkle_g) APPLY_CLAMP("sparkle_g",sparkle_g,0,1)
            APPLY_F("sparkle_b",sparkle_b) APPLY_CLAMP("sparkle_b",sparkle_b,0,1)
            APPLY_F("dot_amount",dot_amount) APPLY_CLAMP("dot_amount",dot_amount,0,1)
            APPLY_F("flicker_hz",flicker_hz)
            APPLY_F("flicker_on_ms",flicker_on_ms) APPLY_CLAMP("flicker_on_ms",flicker_on_ms,0,kFlickerOnMsMax)
            APPLY_F("flicker_phase_degrees",flicker_phase_degrees) APPLY_CLAMP("flicker_phase_degrees",flicker_phase_degrees,0,360)
            APPLY_F("points",points)
            APPLY_F("ilda_fps",ilda_fps) APPLY_CLAMP("ilda_fps",ilda_fps,1,120)
            if (std::string t; json_string(req.body,"text",t)) T_.text = t.substr(0, 4 * kMaxTextChars);
            if (auto f = json_str(req.body,"font"); find_text_font(f)) T_.font = f;
            APPLY_F("text_size",text_size) APPLY_CLAMP("text_size",text_size,0.02,2)
            APPLY_F("text_spacing",text_spacing) APPLY_CLAMP("text_spacing",text_spacing,-0.3,2)
            APPLY_B("text_bold",text_bold) APPLY_B("text_italic",text_italic) APPLY_B("text_underline",text_underline)
        });
        res.set_content(state_to_json(),"application/json");
    });

    // ── GET /api/shapes — shapes and the look parameters each uses ─────────────
    svr.Get("/api/shapes",[](const httplib::Request&,httplib::Response& res){
        res.set_content(shapes_json(),"application/json");
    });

    // ── GET /api/fonts — the text shape's fonts and the styles each has ────────
    // [{"id","label","bold":bool,"italic":bool}] (laser_text.h).
    svr.Get("/api/fonts",[](const httplib::Request&,httplib::Response& res){
        std::ostringstream ss; ss << "[";
        const auto& c = text_font_catalog();
        for (std::size_t i = 0; i < c.size(); ++i)
            ss << (i ? "," : "") << "{\"id\":\"" << c[i].id << "\",\"label\":\"" << c[i].label << "\",\"bold\":"
               << (text_font_has_bold(c[i]) ? "true" : "false") << ",\"italic\":" << (text_font_has_italic(c[i]) ? "true" : "false") << "}";
        ss << "]";
        res.set_content(ss.str(),"application/json");
    });

    // ── POST /laser/shape/<shape> ───────────────────────────────────────────────
    // "staticwave" is still accepted from older clients: a wave with speed 0.
    static const std::set<std::string> SHAPES{"circle","line","triangle","square","wave","stars","text","staticwave"};
    svr.Post(R"(/laser/shape/([a-z]+))",[](const httplib::Request& req,httplib::Response& res){
        std::string s=req.matches[1];
        if(!SHAPES.count(s)){res.status=400;res.set_content("{\"error\":\"unknown shape\"}","application/json");return;}
        for_target_looks(request_lasers(req), [&](LookSlot& sl) { sl.look.shape = s; apply_legacy_shape(sl.look); });
        res.set_content("{\"shape\":\""+std::string(s == "staticwave" ? "wave" : s)+"\"}","application/json");
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
        try{float rv=std::stof(std::string(req.matches[1]));
            float gv=std::stof(std::string(req.matches[2]));
            float bv=std::stof(std::string(req.matches[3]));
            auto norm=[](float v){return std::clamp(v>1.0f?v/255.0f:v,0.0f,1.0f);};
            for_target_looks(request_lasers(req), [&](LookSlot& sl) { sl.look.r=norm(rv); sl.look.g=norm(gv); sl.look.b=norm(bv); });
            }catch(...){}
        res.set_content(state_to_json(),"application/json");
    });

    // ── POST /laser/position/<x>/<y> ───────────────────────────────────────────
    svr.Post(R"(/laser/position/(-?[\d.]+)/(-?[\d.]+))",[](const httplib::Request& req,httplib::Response& res){
        try{const float x=std::clamp(std::stof(std::string(req.matches[1])),-1.0f,1.0f);
            const float y=std::clamp(std::stof(std::string(req.matches[2])),-1.0f,1.0f);
            for_target_looks(request_lasers(req), [&](LookSlot& sl) { sl.look.pos_x=x; sl.look.pos_y=y; });}catch(...){}
        res.set_content(state_to_json(),"application/json");
    });

    // ── POST /laser/rotation/speed/<val> ───────────────────────────────────────
    svr.Post(R"(/laser/rotation/speed/(-?[\d.]+))",[](const httplib::Request& req,httplib::Response& res){
        try{const float v=std::stof(std::string(req.matches[1])); for_target_looks(request_lasers(req), [&](LookSlot& sl) { sl.look.rotation_speed=v; });}catch(...){}
        res.set_content(state_to_json(),"application/json");
    });

    // ── POST /rotation/reset — snap angle back to 0 and stop spinning ──────────
    // Shares do_rotation_reset with the MIDI "rotation_reset" note action.
    svr.Post("/rotation/reset",[](const httplib::Request& req,httplib::Response& res){
        do_rotation_reset(request_lasers(req));
        res.set_content(state_to_json(),"application/json");
    });

    // ── POST /move/mode/<mode> ─────────────────────────────────────────────────
    static const std::set<std::string> MOVES{"none","circle","pan","tilt","tiltup","tiltdown","eight","random"};
    svr.Post(R"(/move/mode/([a-z]+))",[](const httplib::Request& req,httplib::Response& res){
        std::string m=req.matches[1];
        if(!MOVES.count(m)){res.status=400;res.set_content("{\"error\":\"unknown mode\"}","application/json");return;}
        for_target_looks(request_lasers(req), [&](LookSlot& sl) { sl.look.move_mode = m; });
        res.set_content("{\"move_mode\":\""+m+"\"}","application/json");
    });

    // ── POST /move/speed/<val>  /move/size/<val> ───────────────────────────────
    svr.Post(R"(/move/speed/(-?[\d.]+))",[](const httplib::Request& req,httplib::Response& res){
        try{const float v=std::stof(std::string(req.matches[1])); for_target_looks(request_lasers(req), [&](LookSlot& sl) { sl.look.move_speed=v; });}catch(...){}
        res.set_content(state_to_json(),"application/json");
    });
    svr.Post(R"(/move/size/([\d.]+))",[](const httplib::Request& req,httplib::Response& res){
        try{const float v=std::clamp(std::stof(std::string(req.matches[1])),0.0f,6.0f); for_target_looks(request_lasers(req), [&](LookSlot& sl) { sl.look.move_size=v; });}catch(...){}
        res.set_content(state_to_json(),"application/json");
    });

    // ── POST /laser/rainbow/amount/<val>  /speed/<val> ─────────────────────────
    svr.Post(R"(/laser/rainbow/amount/([\d.]+))",[](const httplib::Request& req,httplib::Response& res){
        try{const float v=std::clamp(std::stof(std::string(req.matches[1])),0.0f,1.0f); for_target_looks(request_lasers(req), [&](LookSlot& sl) { sl.look.rainbow_amount=v; });}catch(...){}
        res.set_content(state_to_json(),"application/json");
    });
    // ── POST /laser/rainbow/phase/<deg> ─ hue offset per laser, 0..360 ────────
    svr.Post(R"(/laser/rainbow/phase/([\d.]+))",[](const httplib::Request& req,httplib::Response& res){
        try{const float v=std::clamp(std::stof(std::string(req.matches[1])),0.0f,360.0f); for_target_looks(request_lasers(req), [&](LookSlot& sl) { sl.look.rainbow_phase_degrees=v; });}catch(...){}
        res.set_content(state_to_json(),"application/json");
    });
    svr.Post(R"(/laser/rainbow/speed/(-?[\d.]+))",[](const httplib::Request& req,httplib::Response& res){
        try{const float v=std::stof(std::string(req.matches[1])); for_target_looks(request_lasers(req), [&](LookSlot& sl) { sl.look.rainbow_speed=v; });}catch(...){}
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
        const bool on = req.matches[1]=="1";
        for_target_looks(request_lasers(req), [&](LookSlot& sl) { sl.look.mirror_x = on; });
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
    svr.Post("/api/reset",[](const httplib::Request& req,httplib::Response& res){
        // The target lasers (all without `lasers`) go back to the default
        // look, without layers; master brightness goes back to full too
        // when every laser is reset.
        const LaserTargets targets = request_lasers(req);
        for_target_looks(targets, [](LookSlot& sl) {
            set_look(sl, LaserState{});
            sl.layers.clear(); // reset = back to a single default look
        });
        if (!targets) { std::lock_guard<std::mutex> lk(G_mtx); G.intensity = LaserState{}.intensity; }
        res.set_content(state_to_json(),"application/json");
    });

    // ── Test patterns (test_patterns.h) ─────────────────────────────────────────
    // GET lists the official patterns; POST {"pattern":"off"|<id>, "size":0.05..1}
    // switches armed lasers to a pattern (either key optional). A pattern
    // that needs a higher point rate than the max scan rate is refused.
    svr.Get("/api/test-patterns",[](const httplib::Request&,httplib::Response& res){
        std::ostringstream ss; ss << "[";
        const auto& c = test_pattern_catalog();
        for (std::size_t i = 0; i < c.size(); ++i)
            ss << (i ? "," : "") << "{\"id\":\"" << c[i].id << "\",\"label\":\"" << c[i].label
               << "\",\"kpps\":" << c[i].kpps << "}";
        ss << "]";
        res.set_content(ss.str(),"application/json");
    });
    svr.Post("/api/test-pattern",[](const httplib::Request& req,httplib::Response& res){
        const std::string pattern = json_str(req.body, "pattern");
        const float size = json_float(req.body, "size", -1.0f);
        const TestPatternInfo* tp = pattern.empty() || pattern == "off" ? nullptr : find_test_pattern(pattern);
        if (!pattern.empty() && pattern != "off" && !tp) {
            res.status=400; res.set_content("{\"error\":\"unknown test pattern\"}","application/json"); return;
        }
        {
            std::lock_guard<std::mutex> lk(G_mtx);
            if (tp && tp->kpps > G.max_rate_kpps) {
                std::ostringstream ss;
                ss << "{\"error\":\"" << tp->label << " needs " << tp->kpps << " kpps, max scan rate is "
                   << G.max_rate_kpps << " kpps\"}";
                res.status=400; res.set_content(ss.str(),"application/json"); return;
            }
            if (!pattern.empty()) {
                // Scan rate follows the pattern (Beam page shows the real
                // value); switching off - not from one pattern to the other -
                // restores the rate from before the first pattern.
                const bool wasActive = G_rate_before_test_pattern >= 0.0f;
                if (tp) {
                    if (!wasActive) G_rate_before_test_pattern = G.rate_kpps;
                    G.rate_kpps = tp->kpps;
                    G_test_pattern = tp->id;
                } else {
                    if (wasActive) G.rate_kpps = std::clamp(G_rate_before_test_pattern, 0.0f, G.max_rate_kpps);
                    G_rate_before_test_pattern = -1.0f;
                    G_test_pattern = "off";
                }
            }
            if (size >= 0.0f && std::isfinite(size)) G_test_pattern_size = std::clamp(size, kTestPatternMinSize, 1.0f);
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
    // Body: any of {"name", "ip", "armed":true|false, "mirror_x", "mirror_y", "canvas", "zone_x", "zone_y",
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
              if (json_has(req.body,"ip")) l.ip = json_str(req.body,"ip");
              if (json_has(req.body,"armed")) l.armed = json_bool(req.body,"armed",l.armed);
              if (json_has(req.body,"mirror_x")) l.mirror_x = json_bool(req.body,"mirror_x",l.mirror_x);
              if (json_has(req.body,"mirror_y")) l.mirror_y = json_bool(req.body,"mirror_y",l.mirror_y);
              if (json_has(req.body,"canvas")) l.canvas = json_bool(req.body,"canvas",l.canvas);
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
              ss<<"\""<<num<<"\":"<<cue_to_json(s);
          } }
        ss<<"}";
        res.set_content(ss.str(),"application/json");
    });

    // ── POST /api/cue/<n>/save — snapshot current show params into slot n ─────
    svr.Post(R"(/api/cue/(\d+)/save)",[](const httplib::Request& req,httplib::Response& res){
        int n=std::stoi(std::string(req.matches[1]));
        if(n<1||n>MAX_CUES){res.status=400;res.set_content("{\"error\":\"invalid cue number\"}","application/json");return;}
        if(!do_cue_save(n, request_lasers(req))){res.status=400;res.set_content("{\"error\":\"no laser to save\"}","application/json");return;}
        res.set_content("{\"saved\":"+std::to_string(n)+"}","application/json");
    });

    // ── ILDA files ────────────────────────────────────────────────────────────
    // Stored in the data dir's ilda/ folder under content-hash names
    // ("<16 hex>.ild", see ilda_storage_name()). The Cues view uploads by
    // dropping a file onto a slot (POST /api/cue/<n>/ilda).
    auto ildaError = [](httplib::Response& res, int status, const std::string& msg) {
        res.status = status; res.set_content("{\"error\":\"" + msg + "\"}", "application/json");
    };

    // POST /api/cue/<n>/ilda?name=<file name> - body: a raw .ild file; or,
    // with Content-Type application/json, {"file":"<stored>.ild","name":"..."}
    // to use an already stored file. Slot n becomes an ILDA look playing it
    // (replacing the slot) and is recalled right away. Returns the new state
    // like /recall.
    svr.Post(R"(/api/cue/(\d+)/ilda)",[ildaError](const httplib::Request& req,httplib::Response& res){
        int n=std::stoi(std::string(req.matches[1]));
        if(n<1||n>MAX_CUES){ildaError(res,400,"invalid cue number");return;}
        std::string file, name;
        if (req.get_header_value("Content-Type").find("application/json") != std::string::npos) {
            file = json_str(req.body, "file");
            if (!ilda_valid_storage_name(file) || !ilda_clip(file)) { ildaError(res,404,"unknown ILDA file"); return; }
            const std::string given = json_str(req.body, "name");
            name = !given.empty() ? ilda_display_name(given) : ilda_known_name(file);
            if (name.empty()) name = file;
        } else {
            const IldaStoreResult r = ilda_store_upload(req.body, req.has_param("name") ? req.get_param_value("name") : "");
            if (r.status != 200) { ildaError(res, r.status, r.error); return; }
            file = r.file; name = r.name;
        }
        ilda_cue_assign(n, file, name, request_lasers(req, /*fromBody=*/false));
        res.set_content(state_to_json(),"application/json");
    });

    // GET /api/ilda - every stored file:
    // [{"file","name","frames","bytes","cues":[n,...]}, ...]
    svr.Get("/api/ilda",[](const httplib::Request&,httplib::Response& res){
        std::vector<std::string> files;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(ilda_dir(), ec)) {
            const std::string name = e.path().filename().string();
            if (ilda_valid_storage_name(name)) files.push_back(name);
        }
        std::sort(files.begin(), files.end());
        std::ostringstream ss; ss << "[";
        for (std::size_t i = 0; i < files.size(); ++i) {
            const std::string& f = files[i];
            const IldaClip clip = ilda_clip(f);
            std::error_code sec;
            const auto bytes = std::filesystem::file_size(ilda_dir() + "/" + f, sec);
            ss << (i ? "," : "") << "{\"file\":\"" << f << "\",\"name\":\"" << ilda_known_name(f)
               << "\",\"frames\":" << (clip ? clip->size() : 0) << ",\"bytes\":" << (sec ? 0 : bytes) << ",\"cues\":[";
            const auto used = ilda_used_by(f);
            for (std::size_t k = 0; k < used.size(); ++k) ss << (k ? "," : "") << used[k];
            ss << "]}";
        }
        ss << "]";
        res.set_content(ss.str(),"application/json");
    });

    // POST /api/ilda?name=<file name> - body: a raw .ild file. Stores it
    // without touching any cue: {"file","name","frames"}. 400 if invalid.
    svr.Post("/api/ilda",[ildaError](const httplib::Request& req,httplib::Response& res){
        const IldaStoreResult r = ilda_store_upload(req.body, req.has_param("name") ? req.get_param_value("name") : "");
        if (r.status != 200) { ildaError(res, r.status, r.error); return; }
        res.set_content("{\"file\":\"" + r.file + "\",\"name\":\"" + r.name + "\",\"frames\":" + std::to_string(r.frames) + "}",
                        "application/json");
    });

    // GET /api/ilda/<file> - the stored file itself (download).
    svr.Get(R"(/api/ilda/([0-9a-f]{16}\.ild))",[ildaError](const httplib::Request& req,httplib::Response& res){
        const std::string file = req.matches[1];
        std::ifstream f(ilda_dir() + "/" + file, std::ios::binary);
        if (!f) { ildaError(res,404,"unknown ILDA file"); return; }
        std::ostringstream buf; buf << f.rdbuf();
        const std::string name = ilda_known_name(file);
        res.set_header("Content-Disposition", "attachment; filename=\"" + (name.empty() ? file : name) + "\"");
        res.set_content(buf.str(), "application/octet-stream");
    });

    // DELETE /api/ilda/<file> - removes a stored file. 409 while a cue, the
    // current look or a cue layer still plays it.
    svr.Delete(R"(/api/ilda/([0-9a-f]{16}\.ild))",[ildaError](const httplib::Request& req,httplib::Response& res){
        const std::string file = req.matches[1];
        const std::string path = ilda_dir() + "/" + file;
        if (!std::filesystem::exists(path)) { ildaError(res,404,"unknown ILDA file"); return; }
        const auto used = ilda_used_by(file);
        if (!used.empty()) {
            std::string list;
            for (std::size_t k = 0; k < used.size(); ++k) list += (k ? ", " : "") + std::to_string(used[k]);
            ildaError(res, 409, "still used by cue " + list); return;
        }
        {
            std::lock_guard<std::mutex> lk(G_mtx);
            bool playing = false;
            for (const auto& [id, sl] : G_looks) {
                playing = playing || (sl.look.shape == "ilda" && sl.look.ilda_file == file);
                for (const auto& L : sl.layers) playing = playing || (L.s.shape == "ilda" && L.s.ilda_file == file);
            }
            if (playing) { ildaError(res, 409, "currently playing"); return; }
        }
        std::error_code ec;
        std::filesystem::remove(path, ec);
        if (ec) { ildaError(res,500,"could not delete the file"); return; }
        std::filesystem::remove(ilda_dir() + "/" + file.substr(0, 16) + ".name", ec);
        { std::lock_guard<std::mutex> lk(G_ilda_mtx); G_ilda_cache.erase(file); }
        res.set_content("{\"deleted\":\"" + file + "\"}","application/json");
    });

    // ── GET /api/ilda/<file>/preview — thumbnail data for a cue slot ───────────
    // {"frames":N,"points":[[x,y,r,g,b],...]}: the frame with the most lit
    // points (an animation's first frame is often nearly empty), thinned to
    // at most 600 points, colors 0..255, blanked points 0,0,0.
    svr.Get(R"(/api/ilda/([0-9a-f]{16}\.ild)/preview)",[ildaError](const httplib::Request& req,httplib::Response& res){
        const IldaClip clip = ilda_clip(req.matches[1]);
        if (!clip || clip->empty()) { ildaError(res,404,"unknown ILDA file"); return; }
        const IldaFrame* best = &clip->front();
        std::size_t bestLit = 0;
        for (const auto& f : *clip) {
            std::size_t lit = 0;
            for (const auto& q : f.points) lit += (q.r > 0.0f || q.g > 0.0f || q.b > 0.0f);
            if (lit > bestLit) { bestLit = lit; best = &f; }
        }
        const auto pts = ilda_fit_points(best->points, 600);
        std::ostringstream ss; ss << std::fixed << std::setprecision(3);
        ss << "{\"frames\":" << clip->size() << ",\"points\":[";
        for (std::size_t i = 0; i < pts.size(); ++i) {
            const auto& q = pts[i];
            ss << (i ? "," : "") << '[' << q.x << ',' << q.y << ',' << (int)std::lround(q.r * 255) << ','
               << (int)std::lround(q.g * 255) << ',' << (int)std::lround(q.b * 255) << ']';
        }
        ss << "]}";
        res.set_header("Cache-Control", "max-age=86400"); // content-hash names never change
        res.set_content(ss.str(),"application/json");
    });

    // ── POST /api/cue/<n>/recall — apply slot n's saved show params ────────────
    svr.Post(R"(/api/cue/(\d+)/recall)",[](const httplib::Request& req,httplib::Response& res){
        int n=std::stoi(std::string(req.matches[1]));
        if(n<1||n>MAX_CUES){res.status=400;res.set_content("{\"error\":\"invalid cue number\"}","application/json");return;}
        if(!do_cue_recall(n, request_lasers(req))){res.status=404;res.set_content("{\"error\":\"empty cue\"}","application/json");return;}
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
            if(!do_cue_momentary_press(n, request_lasers(req))){res.status=404;res.set_content("{\"error\":\"empty cue\"}","application/json");return;}
        } else do_cue_momentary_release();
        res.set_content(state_to_json(),"application/json");
    });

    // ── POST /api/cue/<n>/layer/<on|off|toggle> — play cue n on top ────────────
    // Layer playback: cue n runs *in addition* to the current look instead of
    // replacing it (see ShowLayer). 404 for an empty cue slot.
    svr.Post(R"(/api/cue/(\d+)/layer/(on|off|toggle))",[](const httplib::Request& req,httplib::Response& res){
        int n=std::stoi(std::string(req.matches[1]));
        const std::string m = req.matches[2];
        if(n<1||n>MAX_CUES){res.status=400;res.set_content("{\"error\":\"invalid cue number\"}","application/json");return;}
        if(!do_cue_layer(n, m=="on" ? 1 : m=="off" ? 0 : 2, request_lasers(req))){res.status=404;res.set_content("{\"error\":\"empty cue\"}","application/json");return;}
        res.set_content(state_to_json(),"application/json");
    });

    // ── POST /api/layers/clear — release all cue layers ─────────────────────────
    svr.Post("/api/layers/clear",[](const httplib::Request& req,httplib::Response& res){
        do_layers_clear(request_lasers(req));
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
              << "  POST /laser/shape/<circle|line|triangle|square|wave|stars|text>\n"
              << "  POST /laser/brightness/<0-1>  /laser/color/<r>/<g>/<b>\n"
              << "  POST /laser/position/<x>/<y>  /laser/rotation/speed/<v>\n"
              << "  POST /move/mode/<none|circle|pan|tilt|tiltup|tiltdown|eight|random>\n"
              << "  POST /laser/rainbow/amount/<v>  /laser/rainbow/speed/<v>\n"
              << "  POST /blackout/<0|1>  /flash/<0|1>  /mirror/x/<0|1>  /motion/hold/<0|1>\n"
              << "  POST /brightness/gate/<0|1>  /rotation/reset\n"
              << "  GET /api/lasers   POST /api/lasers   POST /api/lasers/<id>   POST /api/lasers/<id>/masks   DELETE /api/lasers/<id>\n"
              << "  GET /api/test-patterns   POST /api/test-pattern   (ILDA test patterns on armed lasers)\n"
              << "  POST /api/cue/<n>/ilda   GET/POST /api/ilda   GET/DELETE /api/ilda/<file>   GET /api/ilda/<file>/preview\n"
              << "  GET/POST /api/magic-touch   (shared Magic Touch layout)\n"
              << "  WS   /ws/points\n"
              << "  MIDI: optional, see backend/midi_map.json (Akai APC40 mkII or any USB controller)\n";

    std::thread stop_t([&](){while(G_running)std::this_thread::sleep_for(100ms);svr.stop();});
    svr.listen("0.0.0.0",http_port);
    stop_t.join(); laser_t.join(); settings_t.join();
    return 0;
}
