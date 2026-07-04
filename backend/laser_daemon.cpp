// BeamCommander3 — single C++ binary
//   • libera-laser output (Ether Dream, Helios, LaserCube, …)
//   • HTTP REST API on :8000
//   • WebSocket /ws/points — ~30fps preview frames to browser
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
#include <iomanip>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using namespace libera;

// ── Shared laser state ─────────────────────────────────────────────────────────
struct LaserParams {
    std::string shape     = "circle";
    std::string target_ip;
    float radius          = 0.7f;
    int   points          = 300;
    float rate_kpps       = 30.0f;
    float intensity       = 1.0f;
    float r               = 0.0f;
    float g               = 1.0f;
    float b               = 0.314f;
};
static LaserParams        G_params;
static std::mutex         G_mtx;
static std::atomic<bool>  G_armed{false};
static std::atomic<bool>  G_running{true};

// ── Shape generators ───────────────────────────────────────────────────────────
static core::Frame make_frame(const LaserParams& p) {
    const float pi  = std::acos(-1.0f);
    const float tau = 2.0f * pi;
    const int   n   = std::max(8, p.points);
    const float rad = std::max(0.01f, p.radius);
    const float rr  = p.r * p.intensity;
    const float rg  = p.g * p.intensity;
    const float rb  = p.b * p.intensity;

    core::Frame frame;
    frame.points.reserve(static_cast<std::size_t>(n));
    auto push = [&](float x, float y) {
        frame.points.emplace_back(core::LaserPoint{x, y, rr, rg, rb, 1.0f});
    };

    if (p.shape == "circle") {
        for (int i=0;i<n;++i) { float a=(float)i/n*tau; push(rad*std::cos(a),rad*std::sin(a)); }
    } else if (p.shape == "line") {
        for (int i=0;i<n;++i) { float t=(float)i/(n-1)*2.0f-1.0f; push(t*rad,0.0f); }
    } else if (p.shape == "triangle") {
        float vx[3]={0.0f,-rad,rad}, vy[3]={rad,-rad*0.577f,-rad*0.577f};
        int third=n/3;
        for (int e=0;e<3;++e) { int nx=(e+1)%3;
            for (int i=0;i<third;++i) { float t=(float)i/third;
                push(vx[e]+t*(vx[nx]-vx[e]),vy[e]+t*(vy[nx]-vy[e])); } }
    } else if (p.shape == "square") {
        float cx[4]={-rad,rad,rad,-rad},cy[4]={-rad,-rad,rad,rad};
        int side=n/4;
        for (int e=0;e<4;++e) { int nx=(e+1)%4;
            for (int i=0;i<side;++i) { float t=(float)i/side;
                push(cx[e]+t*(cx[nx]-cx[e]),cy[e]+t*(cy[nx]-cy[e])); } }
    } else if (p.shape == "wave") {
        static float phase=0.0f; phase+=0.05f;
        for (int i=0;i<n;++i) { float t=(float)i/(n-1)*tau;
            push((t/tau*2.0f-1.0f)*rad,rad*0.5f*std::sin(t+phase)); }
    } else if (p.shape == "staticwave") {
        for (int i=0;i<n;++i) { float t=(float)i/(n-1)*tau;
            push((t/tau*2.0f-1.0f)*rad,rad*0.5f*std::sin(t)); }
    } else {
        for (int i=0;i<n;++i) push(0.0f,0.0f);
    }
    return frame;
}

// ── JSON helpers ───────────────────────────────────────────────────────────────
static std::string frame_to_ws_json(const core::Frame& fr) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(4) << "{\"pts\":[";
    for (std::size_t i=0;i<fr.points.size();++i) {
        if (i) ss << ',';
        const auto& pt=fr.points[i];
        ss<<'['<<pt.x<<','<<pt.y<<','<<pt.r<<','<<pt.g<<','<<pt.b<<']';
    }
    ss << "]}"; return ss.str();
}
static std::string params_to_json(const LaserParams& p) {
    std::ostringstream ss; ss << std::fixed << std::setprecision(4);
    ss << "{"
       << "\"shape\":\""    << p.shape      << "\","
       << "\"radius\":"     << p.radius     << ","
       << "\"points\":"     << p.points     << ","
       << "\"rate_kpps\":"  << p.rate_kpps  << ","
       << "\"intensity\":"  << p.intensity  << ","
       << "\"r\":"          << p.r          << ","
       << "\"g\":"          << p.g          << ","
       << "\"b\":"          << p.b          << ","
       << "\"armed\":"      << (G_armed.load()?"true":"false") << ","
       << "\"ip\":\""       << p.target_ip  << "\""
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

// ── Signal handler ─────────────────────────────────────────────────────────────
static void on_signal(int) { G_running = false; }

// ── Laser output thread ────────────────────────────────────────────────────────
// libera System lives here — created once so it only binds UDP 45457 once.
static void laser_thread() {
    System sys;

    // Loop the last frame indefinitely — no auto-blank
    core::LaserController::setMaxFrameHoldTime(std::chrono::milliseconds(0));

    using Clock = std::chrono::steady_clock;
    auto last_preview = Clock::now() - 1s;

    std::shared_ptr<core::LaserController> ctrl;
    std::string connected_ip;
    float       connected_rate = 0;

    while (G_running) {
        std::string ip; float rate;
        {
            std::lock_guard<std::mutex> lk(G_mtx);
            ip   = G_params.target_ip;
            rate = G_params.rate_kpps;
        }

        // Disarm
        if (ip.empty() || !G_armed.load()) {
            if (ctrl) {
                ctrl->setArmed(false);
                ctrl.reset();
                connected_ip.clear();
                std::cout << "[laser] disarmed\n" << std::flush;
            }
            std::this_thread::sleep_for(200ms);
            continue;
        }

        // (Re)connect when IP or rate changed
        if (!ctrl || connected_ip != ip || std::abs(rate - connected_rate) > 0.1f) {
            if (ctrl) { ctrl->setArmed(false); ctrl.reset(); }

            // Build info directly — no UDP discovery needed for Ether Dream
            etherdream::EtherDreamControllerInfo info(
                ip,              // id
                "Ether Dream",   // label
                ip,              // ip
                7765,            // port
                4095             // bufferSizePoints — standard ED buffer
            );
            ctrl = sys.connectController(info);
            if (!ctrl) {
                std::cout << "[laser] connect failed " << ip << ", retrying...\n" << std::flush;
                std::this_thread::sleep_for(1s);
                continue;
            }
            ctrl->setPointRate(static_cast<std::uint32_t>(rate * 1000));
            ctrl->setArmed(true);
            connected_ip   = ip;
            connected_rate = rate;
            std::cout << "[laser] streaming " << ip << " @ " << rate << " kpps\n" << std::flush;
        }

        // Update point rate live without reconnect
        {
            std::lock_guard<std::mutex> lk(G_mtx);
            if (std::abs(G_params.rate_kpps - connected_rate) > 0.1f) {
                ctrl->setPointRate(static_cast<std::uint32_t>(G_params.rate_kpps * 1000));
                connected_rate = G_params.rate_kpps;
            }
        }

        if (!ctrl->isReadyForNewFrame()) {
            std::this_thread::sleep_for(1ms);
            continue;
        }

        LaserParams snap;
        { std::lock_guard<std::mutex> lk(G_mtx); snap = G_params; }

        auto frame = make_frame(snap);

        // WebSocket preview at ~30fps
        auto now = Clock::now();
        if (now - last_preview >= 33ms) {
            last_preview = now;
            ws_broadcast(frame_to_ws_json(frame));
        }

        ctrl->sendFrame(std::move(frame));
    }

    if (ctrl) ctrl->setArmed(false);
    sys.shutdown();
}

// ── JSON field parsers ─────────────────────────────────────────────────────────
static std::string json_str(const std::string& b, const std::string& k) {
    auto p=b.find("\""+k+"\""); if(p==std::string::npos) return "";
    p=b.find(':',p); if(p==std::string::npos) return "";
    p=b.find('"',p+1); if(p==std::string::npos) return "";
    auto e=b.find('"',p+1); if(e==std::string::npos) return "";
    return b.substr(p+1,e-p-1);
}
static float json_float(const std::string& b, const std::string& k, float d) {
    auto p=b.find("\""+k+"\""); if(p==std::string::npos) return d;
    p=b.find(':',p); if(p==std::string::npos) return d;
    ++p; while(p<b.size()&&b[p]==' ')++p;
    try{return std::stof(b.substr(p));}catch(...){return d;}
}
static int json_int(const std::string& b,const std::string& k,int d){
    return (int)json_float(b,k,(float)d);
}

// ── main ───────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    int http_port = 8000;
    for (int i=1;i<argc;++i) {
        if (!strcmp(argv[i],"--port")&&i+1<argc) http_port=std::stoi(argv[++i]);
        else if (!strcmp(argv[i],"-h")||!strcmp(argv[i],"--help")){
            std::cout<<"Usage: laser_daemon [--port 8000]\n"; return 0; }
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::thread laser_t(laser_thread);

    httplib::Server svr;
    svr.set_default_headers({
        {"Access-Control-Allow-Origin","*"},
        {"Access-Control-Allow-Methods","GET,POST,OPTIONS"},
        {"Access-Control-Allow-Headers","Content-Type"},
    });
    svr.Options(".*",[](const httplib::Request&,httplib::Response& r){r.status=204;});

    svr.Get("/api/state",[](const httplib::Request&,httplib::Response& res){
        LaserParams p;{std::lock_guard<std::mutex> lk(G_mtx);p=G_params;}
        res.set_content(params_to_json(p),"application/json");
    });
    svr.Post("/api/state",[](const httplib::Request& req,httplib::Response& res){
        std::lock_guard<std::mutex> lk(G_mtx);
        auto s=json_str(req.body,"shape");   if(!s.empty()) G_params.shape=s;
        auto ip=json_str(req.body,"ip");     if(!ip.empty()) G_params.target_ip=ip;
        float rv=json_float(req.body,"radius",-1);    if(rv>=0) G_params.radius=rv;
        int   pv=json_int(req.body,"points",-1);      if(pv>0)  G_params.points=pv;
        float rk=json_float(req.body,"rate_kpps",-1); if(rk>0)  G_params.rate_kpps=rk;
        float iv=json_float(req.body,"intensity",-1); if(iv>=0) G_params.intensity=iv;
        float rv2=json_float(req.body,"r",-1); if(rv2>=0) G_params.r=rv2;
        float gv=json_float(req.body,"g",-1);  if(gv>=0)  G_params.g=gv;
        float bv=json_float(req.body,"b",-1);  if(bv>=0)  G_params.b=bv;
        res.set_content(params_to_json(G_params),"application/json");
    });

    static const std::set<std::string> SHAPES{
        "circle","line","triangle","square","wave","staticwave"};
    svr.Post(R"(/laser/shape/([a-z]+))",[](const httplib::Request& req,httplib::Response& res){
        std::string shape=req.matches[1];
        if(!SHAPES.count(shape)){res.status=400;res.set_content("{\"error\":\"unknown shape\"}","application/json");return;}
        {std::lock_guard<std::mutex> lk(G_mtx);G_params.shape=shape;}
        res.set_content("{\"shape\":\""+shape+"\"}","application/json");
    });
    svr.Post(R"(/laser/brightness/([\d.]+))",[](const httplib::Request& req,httplib::Response& res){
        try{float v=std::stof(std::string(req.matches[1]));
            std::lock_guard<std::mutex> lk(G_mtx);G_params.intensity=std::clamp(v,0.0f,1.0f);}catch(...){}
        LaserParams p;{std::lock_guard<std::mutex> lk(G_mtx);p=G_params;}
        res.set_content(params_to_json(p),"application/json");
    });
    svr.Post(R"(/laser/color/([\d.]+)/([\d.]+)/([\d.]+))",[](const httplib::Request& req,httplib::Response& res){
        try{std::lock_guard<std::mutex> lk(G_mtx);
            G_params.r=std::clamp(std::stof(std::string(req.matches[1])),0.0f,1.0f);
            G_params.g=std::clamp(std::stof(std::string(req.matches[2])),0.0f,1.0f);
            G_params.b=std::clamp(std::stof(std::string(req.matches[3])),0.0f,1.0f);}catch(...){}
        LaserParams p;{std::lock_guard<std::mutex> lk(G_mtx);p=G_params;}
        res.set_content(params_to_json(p),"application/json");
    });
    svr.Post(R"(/laser/connect/(.+))",[](const httplib::Request& req,httplib::Response& res){
        std::string ip=req.matches[1];
        {std::lock_guard<std::mutex> lk(G_mtx);G_params.target_ip=ip;}
        G_armed=true;
        res.set_content("{\"ip\":\""+ip+"\",\"armed\":true}","application/json");
    });
    svr.Post("/laser/disconnect",[](const httplib::Request&,httplib::Response& res){
        G_armed=false;
        {std::lock_guard<std::mutex> lk(G_mtx);G_params.target_ip="";}
        res.set_content("{\"armed\":false}","application/json");
    });
    svr.WebSocket("/ws/points",[](const httplib::Request&,httplib::ws::WebSocket& ws){
        {std::lock_guard<std::mutex> lk(G_ws_mtx);G_ws_clients.insert(&ws);}
        std::string msg;
        while(ws.read(msg)!=httplib::ws::ReadResult::Fail){}
        {std::lock_guard<std::mutex> lk(G_ws_mtx);G_ws_clients.erase(&ws);}
    });

    std::cout << "[laser_daemon] Listening on :" << http_port << "\n"
              << "  Supports: Ether Dream, Helios, LaserCube, IDN (via libera)\n"
              << "  GET/POST /api/state\n"
              << "  POST /laser/shape/<circle|line|triangle|square|wave|staticwave>\n"
              << "  POST /laser/connect/<ip>  /laser/disconnect\n"
              << "  WS   /ws/points\n";

    std::thread stop_t([&](){while(G_running)std::this_thread::sleep_for(100ms);svr.stop();});
    svr.listen("0.0.0.0", http_port);
    stop_t.join();
    laser_t.join();
    return 0;
}
