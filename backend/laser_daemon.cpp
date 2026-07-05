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
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using namespace libera;

// ── Laser state — full BeamCommander feature set ───────────────────────────────
struct LaserState {
    // Controller
    std::string target_ip;
    float rate_kpps       = 30.0f;

    // Shape
    std::string shape     = "circle";
    float radius          = 0.7f;     // normalised 0..1
    int   points          = 300;
    float shape_scale     = 0.0f;     // -1..1 (0 = no extra scale)

    // Color (0..1)
    float r = 0.0f, g = 1.0f, b = 0.314f;
    float intensity       = 1.0f;     // master brightness

    // Position -1..1
    float pos_x = 0.0f, pos_y = 0.0f;

    // Rotation
    float rotation_speed  = 0.0f;     // rotations/sec

    // Movement
    std::string move_mode = "none";   // none circle pan tilt eight random
    float move_speed      = 0.30f;    // cycles/sec
    float move_size       = 0.50f;    // 0..1

    // Wave shape params
    float wave_frequency  = 1.0f;     // cycles across width
    float wave_amplitude  = 0.45f;    // fraction of radius
    float wave_speed      = 0.0f;     // cycles/sec (animated wave)

    // Rainbow
    float rainbow_amount  = 0.0f;     // 0..1 blend
    float rainbow_speed   = 0.0f;     // hue cycles/sec

    // FX
    bool  blackout        = false;
    float dot_amount      = 1.0f;     // 0..1 fraction of points shown
    float flicker_hz      = 0.0f;     // strobe frequency

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
// Never locked - only laser_thread writes and reads these (single-threaded).
static double G_move_phase     = 0.0; // radians
static double G_rotation_phase = 0.0; // radians
static double G_wave_phase     = 0.0; // radians
static double G_rainbow_phase  = 0.0; // cycles (0..1, wraps via fmod)

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
            pts.push_back({(t/tau*2.0f-1.0f)*r, r*s.wave_amplitude*std::sin(s.wave_frequency*t+phase)});
        }
    } else if (s.shape == "staticwave") {
        for (int i=0;i<n;++i) {
            float t=(float)i/(n-1)*tau;
            pts.push_back({(t/tau*2.0f-1.0f)*r, r*s.wave_amplitude*std::sin(s.wave_frequency*t)});
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

    // Rotation
    if (s.rotation_speed != 0.0f) {
        rotate_pts(base, (float)G_rotation_phase);
    }

    // Position + movement
    Pt mv = calc_movement(s);
    float ox = s.pos_x + mv.x;
    float oy = s.pos_y + mv.y;

    // Dot amount
    int step = 1;
    if (s.dot_amount < 1.0f && s.dot_amount > 0.0f)
        step = std::max(1, (int)(1.0f / std::max(0.01f, s.dot_amount)));

    // Flicker/strobe blackout
    bool frame_blank = s.blackout;
    if (!frame_blank && s.flicker_hz > 0.0f) {
        double period = 1.0 / s.flicker_hz;
        double phase  = std::fmod(G_time, period);
        frame_blank = (phase > period * 0.5);
    }

    // Rainbow phase
    float rainbow_phase = (float)std::fmod(G_rainbow_phase, 1.0);

    core::Frame frame;
    int n = (int)base.size();
    frame.points.reserve((n + step - 1) / step);

    for (int i = 0; i < n; i += step) {
        float fr = s.r, fg = s.g, fb = s.b;

        if (s.rainbow_amount > 0.0f) {
            float hue = std::fmod(rainbow_phase + (float)i/n * s.rainbow_amount, 1.0f);
            float rr, rg, rb;
            hsv_to_rgb(hue, 1.0f, 1.0f, rr, rg, rb);
            fr = fr*(1-s.rainbow_amount) + rr*s.rainbow_amount;
            fg = fg*(1-s.rainbow_amount) + rg*s.rainbow_amount;
            fb = fb*(1-s.rainbow_amount) + rb*s.rainbow_amount;
        }

        float bri = frame_blank ? 0.0f : s.intensity;
        frame.points.emplace_back(core::LaserPoint{
            base[i].x + ox,
            base[i].y + oy,
            fr * bri, fg * bri, fb * bri, 1.0f
        });
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
       << "\"intensity\":"     << G.intensity      << ","
       << "\"r\":"             << G.r              << ","
       << "\"g\":"             << G.g              << ","
       << "\"b\":"             << G.b              << ","
       << "\"shape_scale\":"   << G.shape_scale    << ","
       << "\"pos_x\":"         << G.pos_x          << ","
       << "\"pos_y\":"         << G.pos_y          << ","
       << "\"rotation_speed\":" << G.rotation_speed << ","
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
            G_wave_phase     = std::fmod(G_wave_phase     + G.wave_speed     * dt * tau, tau);
            G_rainbow_phase  = std::fmod(G_rainbow_phase  + G.rainbow_speed  * dt, 1.0);
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


        bool hardwareReady = ctrl && ctrl->isReadyForNewFrame();
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
            ctrl->sendFrame(std::move(frame));
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

// ── Cue save/recall ──────────────────────────────────────────────────────────
// A cue is a saved snapshot of every show parameter (shape, color, transform,
// movement, wave, rainbow, FX) — NOT the controller connection (target_ip),
// so recalling a cue never drops/changes the current DAC link. Persisted to
// a small JSON file on disk so cues survive a daemon restart, mirroring the
// cue system from the original BeamCommander (Python edition).
static constexpr int MAX_CUES = 32;
static std::map<int, LaserState> G_cues;
static std::mutex                G_cues_mtx;
static const std::string         CUES_FILE = "cues.json";

static std::string cue_state_to_json(const LaserState& s) {
    std::ostringstream ss; ss << std::fixed << std::setprecision(4);
    ss << "{"
       << "\"shape\":\""       << s.shape         << "\","
       << "\"radius\":"        << s.radius        << ","
       << "\"points\":"        << s.points        << ","
       << "\"rate_kpps\":"     << s.rate_kpps     << ","
       << "\"intensity\":"     << s.intensity     << ","
       << "\"r\":"             << s.r             << ","
       << "\"g\":"             << s.g             << ","
       << "\"b\":"             << s.b             << ","
       << "\"shape_scale\":"   << s.shape_scale   << ","
       << "\"pos_x\":"         << s.pos_x         << ","
       << "\"pos_y\":"         << s.pos_y         << ","
       << "\"rotation_speed\":" << s.rotation_speed << ","
       << "\"move_mode\":\""   << s.move_mode     << "\","
       << "\"move_speed\":"    << s.move_speed    << ","
       << "\"move_size\":"     << s.move_size     << ","
       << "\"wave_frequency\":" << s.wave_frequency << ","
       << "\"wave_amplitude\":" << s.wave_amplitude << ","
       << "\"wave_speed\":"    << s.wave_speed    << ","
       << "\"rainbow_amount\":" << s.rainbow_amount << ","
       << "\"rainbow_speed\":" << s.rainbow_speed << ","
       << "\"blackout\":"      << (s.blackout?"true":"false") << ","
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
    s.intensity       = json_float(obj,"intensity",s.intensity);
    s.r               = json_float(obj,"r",s.r);
    s.g               = json_float(obj,"g",s.g);
    s.b               = json_float(obj,"b",s.b);
    s.shape_scale     = json_float(obj,"shape_scale",s.shape_scale);
    s.pos_x           = json_float(obj,"pos_x",s.pos_x);
    s.pos_y           = json_float(obj,"pos_y",s.pos_y);
    s.rotation_speed  = json_float(obj,"rotation_speed",s.rotation_speed);
    auto mv = json_str(obj,"move_mode");  if(!mv.empty()) s.move_mode = mv;
    s.move_speed      = json_float(obj,"move_speed",s.move_speed);
    s.move_size       = json_float(obj,"move_size",s.move_size);
    s.wave_frequency  = json_float(obj,"wave_frequency",s.wave_frequency);
    s.wave_amplitude  = json_float(obj,"wave_amplitude",s.wave_amplitude);
    s.wave_speed      = json_float(obj,"wave_speed",s.wave_speed);
    s.rainbow_amount  = json_float(obj,"rainbow_amount",s.rainbow_amount);
    s.rainbow_speed   = json_float(obj,"rainbow_speed",s.rainbow_speed);
    s.blackout        = json_bool(obj,"blackout",s.blackout);
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
    { std::lock_guard<std::mutex> lk(G_mtx); std::string ip = G.target_ip; G = snap; G.target_ip = ip; }
    return true;
}
static void do_cue_clear(int n) {
    if (n < 1 || n > MAX_CUES) return;
    { std::lock_guard<std::mutex> lk(G_cues_mtx); G_cues.erase(n); }
    save_cues_to_disk();
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
        if ((b.type == "note" || b.type == "cc") && b.number >= 0 && !b.action.empty())
            G_midi_bindings.push_back(b);
        i = body.find('{', j);
    }
    std::cout << "[midi] loaded " << G_midi_bindings.size() << " binding(s) from " << path << "\n";
}

// Converts a raw 0..127 CC value into the binding's configured output range,
// replicating the original BeamCommander's MidiToOscMapper.h curve engine:
//  - "centered" bipolar knobs treat raw 63/64 as the exact zero point (not
//    just val/127*2-1), so the physical center detent reads as exact 0.
//  - "gamma" applies a response curve (symmetric around center when
//    centered) for a less twitchy/more natural feel on fine controls.
//  - "deadzone" (centered only) snaps small values near center to exactly 0.
//  - non-centered controls use the simpler invert/scale/offset/gamma chain.
static float midi_cc_value(const MidiBinding& b, int raw) {
    raw = std::clamp(raw, 0, 127);
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
static void midi_apply_cc_action(const std::string& action, float out) {
    if (action=="r")              { std::lock_guard<std::mutex> lk(G_mtx); G.r=std::clamp(out,0.0f,1.0f); return; }
    if (action=="g")              { std::lock_guard<std::mutex> lk(G_mtx); G.g=std::clamp(out,0.0f,1.0f); return; }
    if (action=="b")              { std::lock_guard<std::mutex> lk(G_mtx); G.b=std::clamp(out,0.0f,1.0f); return; }
    if (action=="intensity")      { std::lock_guard<std::mutex> lk(G_mtx); G.intensity=std::clamp(out,0.0f,1.0f); return; }
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
    if (action=="move_size")      { std::lock_guard<std::mutex> lk(G_mtx); G.move_size=std::clamp(out,0.0f,1.0f); return; }
    if (action=="move_speed")     { std::lock_guard<std::mutex> lk(G_mtx); G.move_speed=std::max(0.0f,out); return; }
    if (action=="rate_kpps")      { std::lock_guard<std::mutex> lk(G_mtx); G.rate_kpps=std::clamp(out,1.0f,100.0f); return; }
}

// State for the momentary (hold-to-preview) cue buttons and the momentary
// flash button - both need to remember what to restore on release.
static float     g_pre_flash_intensity = -1.0f; // <0 = flash not currently held
static bool      g_midi_cue_save_armed = false;
static LaserState g_cue_momentary_snapshot;
static bool       g_cue_momentary_active = false;
static bool       g_motion_held = false;        // for "motion_hold" (freeze movement while held)
static float      g_pre_motion_speed = 0.0f;

// Shared flash press/release - used by both the MIDI dispatcher and the
// /flash/<0|1> HTTP route, so a flash triggered from either input behaves
// identically: full brightness only while held, restored on release.
static void do_flash_press() {
    std::lock_guard<std::mutex> lk(G_mtx);
    if (g_pre_flash_intensity < 0.0f) { g_pre_flash_intensity = G.intensity; G.intensity = 1.0f; }
}
static void do_flash_release() {
    std::lock_guard<std::mutex> lk(G_mtx);
    if (g_pre_flash_intensity >= 0.0f) { G.intensity = g_pre_flash_intensity; g_pre_flash_intensity = -1.0f; }
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
    // Preset button from the original mapping (rainbow/preset/slowfull):
    // full rainbow blend at a slow animation speed, one press away.
    if (action=="rainbow_preset_slowfull") {
        if (!isPress) return;
        std::lock_guard<std::mutex> lk(G_mtx); G.rainbow_amount=1.0f; G.rainbow_speed=0.15f;
        return;
    }
    // Momentary movement freeze (motion/hold in the original mapping):
    // stops the movement pattern in place while held, resumes on release.
    if (action=="motion_hold") {
        std::lock_guard<std::mutex> lk(G_mtx);
        if (isPress && !g_motion_held) { g_pre_motion_speed = G.move_speed; G.move_speed = 0.0f; g_motion_held = true; }
        else if (isRelease && g_motion_held) { G.move_speed = g_pre_motion_speed; g_motion_held = false; }
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
                        midi_apply_cc_action(match.action, midi_cc_value(match, d2));
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
    load_midi_map("midi_map.json");

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

    load_cues_from_disk();

    std::thread laser_t(laser_thread);
    std::thread midi_t(midi_thread);
    midi_t.detach(); // pumps a CFRunLoop forever; torn down on process exit
    httplib::Server svr;

    svr.set_default_headers({{"Access-Control-Allow-Origin","*"},{"Access-Control-Allow-Methods","GET,POST,OPTIONS"},{"Access-Control-Allow-Headers","Content-Type"}});
    svr.Options(".*",[](const httplib::Request&,httplib::Response& r){r.status=204;});

    // ── GET /api/state ──────────────────────────────────────────────────────────
    svr.Get("/api/state",[](const httplib::Request&,httplib::Response& res){
        res.set_content(state_to_json(),"application/json");
    });

    // ── POST /api/state — bulk update ──────────────────────────────────────────
    svr.Post("/api/state",[](const httplib::Request& req,httplib::Response& res){
        { // scope the lock so it's released before state_to_json() re-locks G_mtx
        std::lock_guard<std::mutex> lk(G_mtx);
        APPLY_S("shape",shape)  APPLY_S("ip",target_ip)  APPLY_S("move_mode",move_mode)
        APPLY_F("radius",radius)  APPLY_CLAMP("radius",radius,0,1)
        APPLY_F("rate_kpps",rate_kpps)  APPLY_CLAMP("rate_kpps",rate_kpps,1,100)
        APPLY_F("intensity",intensity)  APPLY_CLAMP("intensity",intensity,0,1)
        APPLY_F("r",r) APPLY_CLAMP("r",r,0,1)
        APPLY_F("g",g) APPLY_CLAMP("g",g,0,1)
        APPLY_F("b",b) APPLY_CLAMP("b",b,0,1)
        APPLY_F("shape_scale",shape_scale) APPLY_CLAMP("shape_scale",shape_scale,-1,1)
        APPLY_F("pos_x",pos_x) APPLY_CLAMP("pos_x",pos_x,-1,1)
        APPLY_F("pos_y",pos_y) APPLY_CLAMP("pos_y",pos_y,-1,1)
        APPLY_F("rotation_speed",rotation_speed)
        APPLY_F("move_speed",move_speed)
        APPLY_F("move_size",move_size) APPLY_CLAMP("move_size",move_size,0,1)
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
            G.intensity=std::clamp(v>1.0f?v/255.0f:v,0.0f,1.0f);}catch(...){}
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
        try{std::lock_guard<std::mutex> lk(G_mtx);G.move_size=std::clamp(std::stof(std::string(req.matches[1])),0.0f,1.0f);}catch(...){}
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

    // ── POST /flash/<0|1> — momentary full brightness while held ──────────────
    // 1 = press (force full brightness, remembering the prior value), 0 =
    // release (restore it). Shares do_flash_press/release with the MIDI
    // dispatcher so a flash button behaves identically from either input.
    svr.Post(R"(/flash/([01]))",[](const httplib::Request& req,httplib::Response& res){
        if (req.matches[1]=="1") do_flash_press(); else do_flash_release();
        res.set_content(state_to_json(),"application/json");
    });

    // ── POST /api/reset — restore all show params to defaults ─────────────────
    // Preserves the current controller connection (target_ip / armed state)
    // so resetting the show doesn't drop the laser link.
    svr.Post("/api/reset",[](const httplib::Request&,httplib::Response& res){
        {
            std::lock_guard<std::mutex> lk(G_mtx);
            std::string ip = G.target_ip;
            G = LaserState{};
            G.target_ip = ip;
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

    // ── WebSocket /ws/points ───────────────────────────────────────────────────
    svr.WebSocket("/ws/points",[](const httplib::Request&,httplib::ws::WebSocket& ws){
        {std::lock_guard<std::mutex> lk(G_ws_mtx);G_ws_clients.insert(&ws);}
        std::string msg;
        while(ws.read(msg)!=httplib::ws::ReadResult::Fail){}
        {std::lock_guard<std::mutex> lk(G_ws_mtx);G_ws_clients.erase(&ws);}
    });

    std::cout << "[laser_daemon] Listening on :" << http_port << "\n"
              << "  GET/POST /api/state   POST /api/reset\n"
              << "  GET /api/cues   POST /api/cue/<1-" << MAX_CUES << ">/save|recall|clear\n"
              << "  POST /laser/shape/<circle|line|triangle|square|wave|staticwave>\n"
              << "  POST /laser/brightness/<0-1>  /laser/color/<r>/<g>/<b>\n"
              << "  POST /laser/position/<x>/<y>  /laser/rotation/speed/<v>\n"
              << "  POST /move/mode/<none|circle|pan|tilt|eight|random>\n"
              << "  POST /laser/rainbow/amount/<v>  /laser/rainbow/speed/<v>\n"
              << "  POST /blackout/<0|1>  /flash/<0|1>  /laser/connect/<ip>  /laser/disconnect\n"
              << "  WS   /ws/points\n"
              << "  MIDI: optional, see backend/midi_map.json (Akai APC40 mkII or any USB controller)\n";

    std::thread stop_t([&](){while(G_running)std::this_thread::sleep_for(100ms);svr.stop();});
    svr.listen("0.0.0.0",http_port);
    stop_t.join(); laser_t.join();
    return 0;
}
