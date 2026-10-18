#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "shared.hpp"
#include "scssdk_telemetry.h"
#include "common/scssdk_telemetry_truck_common_channels.h"

namespace {

constexpr const char* kShmName = "FT_SharedMem";
constexpr DWORD kShmSize = 1024;
constexpr float kSmoothTick = 0.016f;
constexpr int kIniVersion = 4;

#pragma pack(push, 1)
struct FTData {
    uint32_t data_id;
    int32_t cam_w, cam_h;
    float yaw, pitch, roll, x, y, z;
    float pad[14];
};
#pragma pack(pop)
static_assert(sizeof(FTData) == 92, "FTData layout");

HMODULE g_module = nullptr;
scs_log_t g_log = nullptr;
std::atomic<bool> g_running{false};
std::thread g_thread;
HANDLE g_map = nullptr;
FTData* g_view = nullptr;

float rf(const std::string& path, const char* sec, const char* key, float def) {
    char buf[64];
    char dflt[64];
    std::snprintf(dflt, sizeof dflt, "%g", def);
    GetPrivateProfileStringA(sec, key, dflt, buf, sizeof buf, path.c_str());
    char* end = nullptr;
    const float v = std::strtof(buf, &end);
    return (end == buf || !std::isfinite(v)) ? def : v;
}

bool rb(const std::string& path, const char* sec, const char* key, bool def) {
    return rf(path, sec, key, def ? 1.0f : 0.0f) != 0.0f;
}

float lim(float v, float lo, float hi) { return std::clamp(v, lo, hi); }

void read_all(const std::string& p, ets2dhm::Config& c, shared::UiSettings& u) {
    const char* S = "ETS2DynamicHeadMovement";
    c.enabled = rb(p, S, "enabled", c.enabled);
    c.amp = lim(rf(p, S, "amp", c.amp), 0.0f, 3.0f);
    c.smooth = lim(rf(p, S, "smooth", c.smooth), 0.001f, 1.0f);
    c.rate = lim(rf(p, S, "rate", c.rate), 30.0f, 240.0f);
    c.flip_yaw = rb(p, S, "flip_yaw", c.flip_yaw);
    c.flip_pitch = rb(p, S, "flip_pitch", c.flip_pitch);
    c.flip_x = rb(p, S, "flip_x", c.flip_x);
    c.flip_y = rb(p, S, "flip_y", c.flip_y);
    c.flip_z = rb(p, S, "flip_z", c.flip_z);

    c.glance = rb(p, S, "glance", c.glance);
    c.glance_min = lim(rf(p, S, "glance_min", c.glance_min), 1.0f, 300.0f);
    c.glance_max = lim(rf(p, S, "glance_max", c.glance_max), c.glance_min, 600.0f);
    for (int i = 0; i < ets2dhm::kNumTargets; i++) {
        char k[32];
        ets2dhm::GlanceTarget& t = c.targets[i];
        std::snprintf(k, sizeof k, "t%d_weight", i);
        t.weight = lim(rf(p, S, k, t.weight), 0.0f, 10.0f);
        std::snprintf(k, sizeof k, "t%d_yaw", i);
        t.yaw = lim(rf(p, S, k, t.yaw), -90.0f, 90.0f);
        std::snprintf(k, sizeof k, "t%d_pitch", i);
        t.pitch = lim(rf(p, S, k, t.pitch), -45.0f, 45.0f);
        std::snprintf(k, sizeof k, "t%d_hold_min", i);
        t.hold_min = lim(rf(p, S, k, t.hold_min), 0.0f, 10.0f);
        std::snprintf(k, sizeof k, "t%d_hold_max", i);
        t.hold_max = lim(rf(p, S, k, t.hold_max), t.hold_min, 10.0f);
    }

    c.steer_enabled = rb(p, S, "steer_enabled", c.steer_enabled);
    c.steer_deg = lim(rf(p, S, "steer_deg", c.steer_deg), 0.0f, 40.0f);
    c.steer_power = lim(rf(p, S, "steer_power", c.steer_power), 0.5f, 3.0f);
    c.speed_gate = lim(rf(p, S, "speed_gate", c.speed_gate), 0.0f, 20.0f);

    c.blinker_enabled = rb(p, S, "blinker_enabled", c.blinker_enabled);
    c.blinker_deg = lim(rf(p, S, "blinker_deg", c.blinker_deg), 0.0f, 15.0f);
    c.blinker_glance = rb(p, S, "blinker_glance", c.blinker_glance);
    c.mirror_scale = lim(rf(p, S, "mirror_scale", c.mirror_scale), 0.0f, 1.5f);
    c.mirror_repeat = lim(rf(p, S, "mirror_repeat", c.mirror_repeat), 1.0f, 60.0f);

    c.dyn_enabled = rb(p, S, "dyn_enabled", c.dyn_enabled);
    c.dyn_amp = lim(rf(p, S, "dyn_amp", c.dyn_amp), 0.0f, 3.0f);
    c.dyn_freq = lim(rf(p, S, "dyn_freq", c.dyn_freq), 0.5f, 5.0f);
    c.dyn_damp = lim(rf(p, S, "dyn_damp", c.dyn_damp), 0.1f, 2.0f);
    c.lean_roll = lim(rf(p, S, "lean_roll", c.lean_roll), 0.0f, 3.0f);
    c.lean_pitch = lim(rf(p, S, "lean_pitch", c.lean_pitch), 0.0f, 3.0f);
    c.pos_scale = lim(rf(p, S, "pos_scale", c.pos_scale), 0.0f, 5.0f);
    c.pos_gain = lim(rf(p, S, "pos_gain", c.pos_gain), 0.1f, 10.0f);
    c.custom_gain = lim(rf(p, S, "custom_gain", c.custom_gain), 0.1f, 10.0f);
    c.slope_comp = lim(rf(p, S, "slope_comp", c.slope_comp), -1.0f, 1.0f);
    c.camber_comp = lim(rf(p, S, "camber_comp", c.camber_comp), -1.0f, 1.0f);
    c.cabin_comp = lim(rf(p, S, "cabin_comp", c.cabin_comp), -1.0f, 1.0f);

    for (int i = 0; i < ets2dhm::kNumCustom; i++) {
        ets2dhm::CustomPose& q = c.custom[i];
        char k[32];
        auto key = [&](const char* n) {
            std::snprintf(k, sizeof k, "c%d_%s", i, n);
            return k;
        };
        char nm[32] = {0};
        GetPrivateProfileStringA(S, key("name"), q.name, nm, sizeof nm, p.c_str());
        std::snprintf(q.name, sizeof q.name, "%s", nm);
        q.enabled = rb(p, S, key("enabled"), q.enabled);
        q.key = int(rf(p, S, key("key"), float(q.key)));
        if (q.key < 0 || q.key > 254) q.key = 0;
        q.mode = rf(p, S, key("mode"), float(q.mode)) >= 0.5f ? 1 : 0;
        q.flips = int(rf(p, S, key("flip"), float(q.flips))) & 63;
        q.time = lim(rf(p, S, key("time"), q.time), 0.05f, 3.0f);
        q.yaw = lim(rf(p, S, key("yaw"), q.yaw), -90.0f, 90.0f);
        q.pitch = lim(rf(p, S, key("pitch"), q.pitch), -60.0f, 60.0f);
        q.roll = lim(rf(p, S, key("roll"), q.roll), -45.0f, 45.0f);
        q.x = lim(rf(p, S, key("x"), q.x), -100.0f, 100.0f);
        q.y = lim(rf(p, S, key("y"), q.y), -100.0f, 100.0f);
        q.z = lim(rf(p, S, key("z"), q.z), -100.0f, 100.0f);
    }

    u.hotkey = int(rf(p, "ui", "hotkey", float(u.hotkey)));
    if (u.hotkey < 8 || u.hotkey > 254) u.hotkey = 0x2D;
    u.scale = lim(rf(p, "ui", "scale", u.scale), 0.0f, 3.0f);
    u.alpha = lim(rf(p, "ui", "alpha", u.alpha), 0.3f, 1.0f);
}

bool write_all(const std::string& path, const ets2dhm::Config& c, const shared::UiSettings& u) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    std::fprintf(f, "[ETS2DynamicHeadMovement]\nini_version=%d\nenabled=%d\namp=%g\nsmooth=%g\nrate=%g\n", kIniVersion,
                 c.enabled ? 1 : 0, c.amp, c.smooth, c.rate);
    std::fprintf(f, "flip_yaw=%d\nflip_pitch=%d\nflip_x=%d\nflip_y=%d\nflip_z=%d\n", c.flip_yaw ? 1 : 0,
                 c.flip_pitch ? 1 : 0, c.flip_x ? 1 : 0, c.flip_y ? 1 : 0, c.flip_z ? 1 : 0);
    std::fprintf(f, "glance=%d\nglance_min=%g\nglance_max=%g\n", c.glance ? 1 : 0, c.glance_min, c.glance_max);
    for (int i = 0; i < ets2dhm::kNumTargets; i++) {
        const ets2dhm::GlanceTarget& t = c.targets[i];
        std::fprintf(f, "t%d_weight=%g\nt%d_yaw=%g\nt%d_pitch=%g\nt%d_hold_min=%g\nt%d_hold_max=%g\n", i, t.weight, i,
                     t.yaw, i, t.pitch, i, t.hold_min, i, t.hold_max);
    }
    std::fprintf(f, "steer_enabled=%d\nsteer_deg=%g\nsteer_power=%g\nspeed_gate=%g\n", c.steer_enabled ? 1 : 0,
                 c.steer_deg, c.steer_power, c.speed_gate);
    std::fprintf(f, "blinker_enabled=%d\nblinker_deg=%g\nblinker_glance=%d\nmirror_scale=%g\nmirror_repeat=%g\n",
                 c.blinker_enabled ? 1 : 0, c.blinker_deg, c.blinker_glance ? 1 : 0, c.mirror_scale,
                 c.mirror_repeat);
    std::fprintf(f, "dyn_enabled=%d\ndyn_amp=%g\ndyn_freq=%g\ndyn_damp=%g\nlean_roll=%g\nlean_pitch=%g\n",
                 c.dyn_enabled ? 1 : 0, c.dyn_amp, c.dyn_freq, c.dyn_damp, c.lean_roll, c.lean_pitch);
    std::fprintf(f, "pos_gain=%g\ncustom_gain=%g\npos_scale=%g\nslope_comp=%g\ncamber_comp=%g\ncabin_comp=%g\n", c.pos_gain, c.custom_gain, c.pos_scale, c.slope_comp,
                 c.camber_comp, c.cabin_comp);
    for (int i = 0; i < ets2dhm::kNumCustom; i++) {
        const ets2dhm::CustomPose& q = c.custom[i];
        std::fprintf(f, "c%d_name=%s\nc%d_enabled=%d\nc%d_key=%d\nc%d_mode=%d\nc%d_flip=%d\nc%d_time=%g\n", i, q.name, i,
                     q.enabled ? 1 : 0, i, q.key, i, q.mode, i, q.flips, i, q.time);
        std::fprintf(f, "c%d_yaw=%g\nc%d_pitch=%g\nc%d_roll=%g\nc%d_x=%g\nc%d_y=%g\nc%d_z=%g\n", i, q.yaw, i,
                     q.pitch, i, q.roll, i, q.x, i, q.y, i, q.z);
    }
    std::fprintf(f, "\n[ui]\nhotkey=%d\nscale=%g\nalpha=%g\n", u.hotkey, u.scale, u.alpha);
    std::fclose(f);
    return true;
}

void write_pose(uint32_t id, const float v[6]) {
    if (!g_view) return;
    FTData d{};
    d.data_id = id;
    d.cam_w = 640;
    d.cam_h = 480;
    d.yaw = v[0];
    d.pitch = v[1];
    d.roll = v[2];
    d.x = v[3];
    d.y = v[4];
    d.z = v[5];
    std::memcpy(g_view, &d, sizeof d);
}

void worker() {
    timeBeginPeriod(1);
    ets2dhm::Config cfg;
    {
        std::lock_guard<std::mutex> lk(shared::mu);
        cfg = shared::cfg;
    }
    ets2dhm::DynamicHead head(cfg);
    float cur[6] = {0, 0, 0, 0, 0, 0};
    uint32_t id = 0;
    LARGE_INTEGER freq, now, last;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    double next = 0;
    const LARGE_INTEGER t0 = last;
    constexpr float kPi = 3.14159265f;
    shared::Live& L = shared::live;

    while (g_running.load()) {
        QueryPerformanceCounter(&now);
        const float dt = float(double(now.QuadPart - last.QuadPart) / double(freq.QuadPart));
        last = now;

        {
            std::lock_guard<std::mutex> lk(shared::mu);
            cfg = shared::cfg;
        }
        head.set_config(cfg);

        const int lb = L.lblinker.load();
        const int rb_ = L.rblinker.load();
        const int blinker = (lb && !rb_) ? -1 : ((rb_ && !lb) ? 1 : 0);
        head.set_context(-L.steer_left.load(), blinker);
        ets2dhm::Telemetry tel;
        tel.have_speed = L.have_speed.load();
        tel.speed = L.speed.load();
        tel.acc_x = L.acc[0].load();
        tel.acc_y = L.acc[1].load();
        tel.acc_z = L.acc[2].load();
        tel.world_pitch = L.world_pitch.load();
        tel.world_roll = L.world_roll.load();
        tel.cab_pitch = L.cab_pitch.load();
        tel.cab_roll = L.cab_roll.load();
        head.set_telemetry(tel);
        bool act[ets2dhm::kNumCustom];
        for (int i = 0; i < ets2dhm::kNumCustom; i++) act[i] = L.custom_active[i].load();
        head.set_custom_active(act);

        const ets2dhm::Pose p = head.step(dt);
        const ets2dhm::Pose q = cfg.enabled ? p : head.custom_pose();
        float target[6];
        target[0] = std::clamp(q.yaw * kPi / 180.0f, -kPi, kPi);
        target[1] = std::clamp(q.pitch * kPi / 180.0f, -kPi, kPi);
        target[2] = std::clamp(q.roll * kPi / 180.0f, -kPi, kPi);
        target[3] = q.x;
        target[4] = q.y;
        target[5] = q.z;

        const float a = 1.0f - std::pow(1.0f - cfg.smooth, std::max(0.0f, dt) / kSmoothTick);
        for (int i = 0; i < 6; i++) cur[i] += (target[i] - cur[i]) * a;
        write_pose(++id, cur);
        for (int i = 0; i < 3; i++) L.out[i].store(cur[i] * 180.0f / kPi);
        for (int i = 3; i < 6; i++) L.out[i].store(cur[i]);

        next += 1.0 / cfg.rate;
        const double elapsed = double(now.QuadPart - t0.QuadPart) / double(freq.QuadPart);
        const double wait = next - elapsed;
        if (wait > 0) Sleep(DWORD(wait * 1000.0));
        else next = elapsed;
    }

    const float zero[6] = {0, 0, 0, 0, 0, 0};
    write_pose(++id, zero);
    timeEndPeriod(1);
}

SCSAPI_VOID on_steering(const scs_string_t, const scs_u32_t, const scs_value_t* v, const scs_context_t) {
    if (v && v->type == SCS_VALUE_TYPE_float) shared::live.steer_left.store(v->value_float.value);
}
SCSAPI_VOID on_lblinker(const scs_string_t, const scs_u32_t, const scs_value_t* v, const scs_context_t) {
    shared::live.lblinker.store(v && v->type == SCS_VALUE_TYPE_bool && v->value_bool.value ? 1 : 0);
}
SCSAPI_VOID on_rblinker(const scs_string_t, const scs_u32_t, const scs_value_t* v, const scs_context_t) {
    shared::live.rblinker.store(v && v->type == SCS_VALUE_TYPE_bool && v->value_bool.value ? 1 : 0);
}
SCSAPI_VOID on_speed(const scs_string_t, const scs_u32_t, const scs_value_t* v, const scs_context_t) {
    if (v && v->type == SCS_VALUE_TYPE_float) {
        shared::live.speed.store(v->value_float.value);
        shared::live.have_speed.store(true);
    }
}
SCSAPI_VOID on_accel(const scs_string_t, const scs_u32_t, const scs_value_t* v, const scs_context_t) {
    if (v && v->type == SCS_VALUE_TYPE_fvector) {
        shared::live.acc[0].store(v->value_fvector.x);
        shared::live.acc[1].store(v->value_fvector.y);
        shared::live.acc[2].store(v->value_fvector.z);
    }
}
SCSAPI_VOID on_world(const scs_string_t, const scs_u32_t, const scs_value_t* v, const scs_context_t) {
    if (v && v->type == SCS_VALUE_TYPE_dplacement) {
        shared::live.world_pitch.store(v->value_dplacement.orientation.pitch * 360.0f);
        shared::live.world_roll.store(v->value_dplacement.orientation.roll * 360.0f);
    }
}
SCSAPI_VOID on_cabin(const scs_string_t, const scs_u32_t, const scs_value_t* v, const scs_context_t) {
    if (v && v->type == SCS_VALUE_TYPE_fplacement) {
        shared::live.cab_pitch.store(v->value_fplacement.orientation.pitch * 360.0f);
        shared::live.cab_roll.store(v->value_fplacement.orientation.roll * 360.0f);
    }
}

}  // namespace

namespace shared {

std::string ini_path() {
    char buf[MAX_PATH] = {0};
    GetModuleFileNameA(g_module, buf, MAX_PATH);
    std::string p = buf;
    const size_t dot = p.find_last_of('.');
    if (dot != std::string::npos) p.resize(dot);
    return p + ".ini";
}

void log_msg(const std::string& s) {
    if (g_log) g_log(SCS_LOG_TYPE_message, ("[ETS2DynamicHeadMovement] " + s).c_str());
}

bool save_config() {
    ets2dhm::Config c;
    UiSettings u;
    {
        std::lock_guard<std::mutex> lk(mu);
        c = cfg;
        u = ui;
    }
    return write_all(ini_path(), c, u);
}

bool reload_config() {
    ets2dhm::Config c;
    UiSettings u;
    read_all(ini_path(), c, u);
    std::lock_guard<std::mutex> lk(mu);
    cfg = c;
    ui = u;
    return true;
}

}  // namespace shared

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

extern "C" {

__declspec(dllexport) SCSAPI_RESULT scs_telemetry_init(const scs_u32_t version,
                                                         const scs_telemetry_init_params_t* const params) {
    if (version != SCS_TELEMETRY_VERSION_1_00 && version != SCS_TELEMETRY_VERSION_1_01) {
        return SCS_RESULT_unsupported;
    }
    const auto* p = static_cast<const scs_telemetry_init_params_v100_t*>(params);
    g_log = p->common.log;

    g_map = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kShmSize, kShmName);
    if (!g_map) {
        shared::log_msg("failed to create FT_SharedMem");
        return SCS_RESULT_generic_error;
    }
    const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    g_view = static_cast<FTData*>(MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, kShmSize));
    if (!g_view) {
        CloseHandle(g_map);
        g_map = nullptr;
        shared::log_msg("failed to map FT_SharedMem");
        return SCS_RESULT_generic_error;
    }
    shared::live.shm_ok.store(true);
    if (existed) {
        shared::log_msg("FT_SharedMem already exists: make sure receiver.py / the old DLL are not running at the same time");
    }

    bool ok = true;
    auto reg = [&](const char* name, scs_value_type_t type, scs_telemetry_channel_callback_t cb) {
        ok &= p->register_for_channel(name, SCS_U32_NIL, type, SCS_TELEMETRY_CHANNEL_FLAG_none, cb, nullptr) ==
              SCS_RESULT_ok;
    };
    reg(SCS_TELEMETRY_TRUCK_CHANNEL_effective_steering, SCS_VALUE_TYPE_float, on_steering);
    reg(SCS_TELEMETRY_TRUCK_CHANNEL_lblinker, SCS_VALUE_TYPE_bool, on_lblinker);
    reg(SCS_TELEMETRY_TRUCK_CHANNEL_rblinker, SCS_VALUE_TYPE_bool, on_rblinker);
    reg(SCS_TELEMETRY_TRUCK_CHANNEL_speed, SCS_VALUE_TYPE_float, on_speed);
    reg(SCS_TELEMETRY_TRUCK_CHANNEL_local_linear_acceleration, SCS_VALUE_TYPE_fvector, on_accel);
    reg(SCS_TELEMETRY_TRUCK_CHANNEL_world_placement, SCS_VALUE_TYPE_dplacement, on_world);
    reg(SCS_TELEMETRY_TRUCK_CHANNEL_cabin_offset, SCS_VALUE_TYPE_fplacement, on_cabin);
    if (!ok) shared::log_msg("some channels failed to register (the game may not support them)");

    const std::string path = shared::ini_path();
    if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        const int ver = GetPrivateProfileIntA("ETS2DynamicHeadMovement", "ini_version", 0, path.c_str());
        if (ver < kIniVersion) {
            const std::string bak = path + ".bak";
            DeleteFileA(bak.c_str());
            MoveFileA(path.c_str(), bak.c_str());
        }
    }
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        write_all(path, shared::cfg, shared::ui);
    }
    shared::reload_config();

    g_running.store(true);
    g_thread = std::thread(worker);
    overlay::start(g_module);
    shared::log_msg("active");
    return SCS_RESULT_ok;
}

__declspec(dllexport) SCSAPI_VOID scs_telemetry_shutdown(void) {
    overlay::stop();
    g_running.store(false);
    if (g_thread.joinable()) g_thread.join();
    if (g_view) {
        UnmapViewOfFile(g_view);
        g_view = nullptr;
    }
    if (g_map) {
        CloseHandle(g_map);
        g_map = nullptr;
    }
    shared::log_msg("stopped");
    g_log = nullptr;
}

}  // extern "C"
