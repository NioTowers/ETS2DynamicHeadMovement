#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace ets2dhm {

struct Pose {
    float yaw = 0, pitch = 0, roll = 0, x = 0, y = 0, z = 0;
};

struct GlanceTarget {
    const char* name;
    float weight, yaw, pitch, hold_min, hold_max;
};

constexpr int kNumTargets = 5;
constexpr GlanceTarget kDefaultTargets[kNumTargets] = {
    {"Left mirror", 0.28f, -30.0f, -3.0f, 0.5f, 1.4f},
    {"Right mirror", 0.28f, 34.0f, -3.0f, 0.5f, 1.4f},
    {"Dashboard", 0.20f, 2.0f, -11.0f, 0.3f, 0.9f},
    {"Scan left", 0.12f, -14.0f, 1.0f, 0.3f, 0.8f},
    {"Scan right", 0.12f, 14.0f, 1.0f, 0.3f, 0.8f},
};

constexpr int kNumCustom = 3;

struct CustomPose {
    char name[24] = "";
    bool enabled = true;
    int key = 0;
    int mode = 0;
    int flips = 0;
    float time = 0.35f;
    float yaw = 0, pitch = 0, roll = 0;
    float x = 0, y = 0, z = 0;
};

struct Config {
    Config() {
        std::copy(std::begin(kDefaultTargets), std::end(kDefaultTargets), targets);
        std::snprintf(custom[0].name, sizeof custom[0].name, "Look left");
        custom[0].yaw = -50.0f;
        std::snprintf(custom[1].name, sizeof custom[1].name, "Look right");
        custom[1].yaw = 50.0f;
        std::snprintf(custom[2].name, sizeof custom[2].name, "Lean forward");
        custom[2].z = 10.0f;
        custom[2].pitch = 4.0f;
    }
    GlanceTarget targets[kNumTargets];
    CustomPose custom[kNumCustom];
    bool enabled = true;
    float mirror_repeat = 6.0f;
    float amp = 1.0f;
    float smooth = 0.15f;
    float rate = 120.0f;
    bool glance = true;
    bool blinker_glance = true;
    float mirror_scale = 1.0f;
    bool flip_yaw = true;
    bool flip_pitch = true;
    float glance_min = 20.0f;
    float glance_max = 45.0f;
    float steer_deg = 10.0f;
    float steer_power = 1.3f;
    float blinker_deg = 2.5f;
    bool steer_enabled = true;
    bool blinker_enabled = true;
    bool dyn_enabled = true;
    float dyn_amp = 1.0f;
    float dyn_freq = 1.5f;
    float dyn_damp = 0.55f;
    float lean_roll = 0.8f;
    float lean_pitch = 0.8f;
    float pos_scale = 0.5f;
    float pos_gain = 1.0f;
    float custom_gain = 1.0f;
    float slope_comp = 0.25f;
    float camber_comp = 0.3f;
    float cabin_comp = 0.5f;
    float speed_gate = 2.0f;
    bool flip_x = false;
    bool flip_y = false;
    bool flip_z = false;
};

struct Telemetry {
    bool have_speed = false;
    float speed = 0;
    float acc_x = 0, acc_y = 0, acc_z = 0;
    float world_pitch = 0, world_roll = 0;
    float cab_pitch = 0, cab_roll = 0;
};

struct Spring {
    float x = 0, v = 0;
    void step(float accel, float w, float zeta, float dt) {
        const int n = std::max(1, int(std::ceil(dt / 0.004f)));
        const float h = dt / float(n);
        for (int i = 0; i < n; i++) {
            v += (-accel - w * w * x - 2.0f * zeta * w * v) * h;
            x += v * h;
        }
    }
};

inline float min_jerk(float u) {
    u = std::clamp(u, 0.0f, 1.0f);
    return u * u * u * (10.0f + u * (-15.0f + 6.0f * u));
}

constexpr int kMirrorLeft = 0;
constexpr int kMirrorRight = 1;

class DynamicHead {
public:
    enum State { Idle, Move, Hold, Return };

    explicit DynamicHead(const Config& cfg, unsigned seed = std::random_device{}())
        : cfg_(cfg), rng_(seed) {
        const float sigma[6] = {1.1f, 0.7f, 0.5f, 0.35f, 0.25f, 0.30f};
        for (int i = 0; i < 6; i++) make_waves(i, sigma[i]);
        state_dur_ = idle_time();
    }

    void set_config(const Config& c) { cfg_ = c; }

    void set_context(float steer_right, int blinker) {
        steer_ = std::clamp(steer_right, -1.0f, 1.0f);
        blinker = std::clamp(blinker, -1, 1);
        if (blinker != 0 && blinker != blinker_) last_forced_t_ = -99.0f;
        blinker_ = blinker;
    }

    void set_telemetry(const Telemetry& t) {
        tel_ = t;
        auto fin = [](float v) { return std::isfinite(v) ? v : 0.0f; };
        tel_.speed = fin(t.speed);
        tel_.acc_x = fin(t.acc_x);
        tel_.acc_y = fin(t.acc_y);
        tel_.acc_z = fin(t.acc_z);
        tel_.world_pitch = fin(t.world_pitch);
        tel_.world_roll = fin(t.world_roll);
        tel_.cab_pitch = fin(t.cab_pitch);
        tel_.cab_roll = fin(t.cab_roll);
    }

    void set_custom_active(const bool a[kNumCustom]) {
        for (int i = 0; i < kNumCustom; i++) custom_active_[i] = a[i];
    }

    Pose custom_pose() const { return custom_final_; }

    State state() const { return state_; }
    const char* target_name() const { return target_name_; }
    float time() const { return t_; }

    Pose step(float dt) {
        dt = std::clamp(dt, 1e-4f, 0.1f);
        t_ += dt;

        float sway[6];
        for (int i = 0; i < 6; i++) {
            float s = 0;
            for (const Wave& w : waves_[i]) s += w.a * std::sin(6.2831853f * w.f * t_ + w.ph);
            sway[i] = cfg_.amp * s;
        }

        update_glance(dt);

        const float a = 1.0f - std::exp(-dt / 0.5f);
        const float gate = (tel_.have_speed && cfg_.speed_gate > 0.0f)
                               ? std::clamp(std::fabs(tel_.speed) / cfg_.speed_gate, 0.0f, 1.0f)
                               : 1.0f;
        const float steer_in = cfg_.steer_enabled ? steer_ * gate : 0.0f;
        const float curved = std::copysign(std::pow(std::fabs(steer_in), cfg_.steer_power), steer_in);
        steer_f_ += (curved - steer_f_) * a;
        const float bl_target = (cfg_.blinker_enabled && blinker_ != 0) ? float(blinker_) : 0.0f;
        blink_f_ += (bl_target - blink_f_) * a;

        const float yaw_ctx = cfg_.amp * (cfg_.steer_deg * steer_f_ + cfg_.blinker_deg * blink_f_);

        float dyn_pitch = 0, dyn_roll = 0, dyn_x = 0, dyn_y = 0, dyn_z = 0;
        if (cfg_.dyn_enabled) {
            const float la = 1.0f - std::exp(-dt / 0.04f);
            acc_f_[0] += (std::clamp(tel_.acc_x, -15.0f, 15.0f) - acc_f_[0]) * la;
            acc_f_[1] += (std::clamp(tel_.acc_y, -15.0f, 15.0f) - acc_f_[1]) * la;
            acc_f_[2] += (std::clamp(tel_.acc_z, -15.0f, 15.0f) - acc_f_[2]) * la;
            const float w = 6.2831853f * std::max(0.2f, cfg_.dyn_freq);
            const float k = gate * cfg_.dyn_amp;
            sp_[0].step(acc_f_[0] * k, w, cfg_.dyn_damp, dt);
            sp_[1].step(acc_f_[1] * k, w, cfg_.dyn_damp, dt);
            sp_[2].step(acc_f_[2] * k, w, cfg_.dyn_damp, dt);
            const float x_cm = sp_[0].x * 100.0f;
            const float y_cm = sp_[1].x * 100.0f;
            const float z_cm = sp_[2].x * 100.0f;

            const float sl = 1.0f - std::exp(-dt / 0.8f);
            slope_f_ += (tel_.world_pitch - slope_f_) * sl;
            camber_f_ += (tel_.world_roll - camber_f_) * sl;

            dyn_roll = cfg_.lean_roll * x_cm + cfg_.camber_comp * camber_f_ + cfg_.cabin_comp * tel_.cab_roll;
            dyn_pitch = cfg_.lean_pitch * z_cm - cfg_.slope_comp * slope_f_ - cfg_.cabin_comp * tel_.cab_pitch;
            dyn_pitch = std::clamp(dyn_pitch, -5.0f, 5.0f) * cfg_.dyn_amp;
            dyn_roll = std::clamp(dyn_roll, -5.0f, 5.0f) * cfg_.dyn_amp;
            dyn_x = std::clamp(x_cm, -5.0f, 5.0f) * cfg_.pos_scale;
            dyn_y = std::clamp(y_cm, -4.0f, 4.0f) * cfg_.pos_scale;
            dyn_z = std::clamp(-z_cm, -5.0f, 5.0f) * cfg_.pos_scale;
        }

        Pose cu;
        for (int i = 0; i < kNumCustom; i++) {
            const CustomPose& cp = cfg_.custom[i];
            const float target = (cp.enabled && custom_active_[i]) ? 1.0f : 0.0f;
            const float om = 6.0f / std::max(0.05f, cp.time);
            const int n = std::max(1, int(std::ceil(dt / 0.004f)));
            const float h = dt / float(n);
            for (int k = 0; k < n; k++) {
                const float acc = om * om * (target - custom_w_[i]) - 2.0f * om * custom_v_[i];
                custom_v_[i] += acc * h;
                custom_w_[i] += custom_v_[i] * h;
            }
            const float w = custom_w_[i];
            auto fs = [&](int bit) { return (cp.flips >> bit) & 1 ? -1.0f : 1.0f; };
            cu.yaw += w * cp.yaw * fs(0);
            cu.pitch += w * cp.pitch * fs(1);
            cu.roll += w * cp.roll * fs(2);
            cu.x += w * cp.x * fs(3);
            cu.y += w * cp.y * fs(4);
            cu.z += w * cp.z * fs(5);
        }

        const float yaw = sway[0] + g_off_[0] + yaw_ctx + cu.yaw;
        const float pitch = sway[1] + g_off_[1] + dyn_pitch + cu.pitch;
        const float roll = sway[2] + 0.07f * (g_off_[0] + yaw_ctx) + dyn_roll + cu.roll;
        const float z = sway[5] + 0.15f * cfg_.amp * std::sin(6.2831853f * 0.22f * t_) + dyn_z;
        const float sign = cfg_.flip_yaw ? -1.0f : 1.0f;
        const float psign = cfg_.flip_pitch ? -1.0f : 1.0f;
        const float fx = cfg_.flip_x ? -1.0f : 1.0f;
        const float fy = cfg_.flip_y ? -1.0f : 1.0f;
        const float fz = cfg_.flip_z ? -1.0f : 1.0f;

        custom_final_.yaw = sign * std::clamp(cu.yaw, -90.0f, 90.0f);
        custom_final_.pitch = psign * std::clamp(cu.pitch, -60.0f, 60.0f);
        custom_final_.roll = sign * std::clamp(cu.roll, -45.0f, 45.0f);
        const float cg = cfg_.custom_gain;
        custom_final_.x = fx * cu.x * cg;
        custom_final_.y = fy * cu.y * cg;
        custom_final_.z = fz * cu.z * cg;

        Pose p;
        p.yaw = sign * std::clamp(yaw, -90.0f, 90.0f);
        p.pitch = psign * std::clamp(pitch, -60.0f, 60.0f);
        p.roll = sign * std::clamp(roll, -45.0f, 45.0f);
        const float pg = cfg_.pos_gain;
        p.x = fx * ((sway[3] + dyn_x) * pg + cu.x * cg);
        p.y = fy * ((sway[4] + dyn_y) * pg + cu.y * cg);
        p.z = fz * (z * pg + cu.z * cg);
        return p;
    }

private:
    struct Wave {
        float a, f, ph;
    };

    float uni(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng_); }

    void make_waves(int axis, float sigma) {
        constexpr int n = 4;
        float f[n], w[n], norm = 0;
        for (int i = 0; i < n; i++) {
            f[i] = uni(0.04f, 0.45f);
            w[i] = 1.0f / std::sqrt(f[i]);
            norm += w[i] * w[i];
        }
        norm = std::sqrt(norm / 2.0f);
        for (int i = 0; i < n; i++) waves_[axis].push_back({sigma * w[i] / norm, f[i], uni(0.0f, 6.2831853f)});
    }

    float idle_time() { return uni(cfg_.glance_min, std::max(cfg_.glance_min, cfg_.glance_max)); }

    void start_glance(int idx, float scale = 1.0f) {
        const GlanceTarget& g = cfg_.targets[idx];
        const float yaw = (g.yaw + uni(-3.0f, 3.0f)) * scale;
        const float pitch = (g.pitch + uni(-1.5f, 1.5f)) * scale;
        g_from_[0] = g_off_[0];
        g_from_[1] = g_off_[1];
        g_to_[0] = yaw * cfg_.amp;
        g_to_[1] = pitch * cfg_.amp;
        const float dist = std::hypot(g_to_[0] - g_from_[0], g_to_[1] - g_from_[1]);
        move_dur_ = std::clamp(0.20f + 0.008f * dist, 0.20f, 0.60f);
        hold_dur_ = uni(g.hold_min, g.hold_max);
        target_name_ = g.name;
        state_ = Move;
        state_t_ = 0;
        state_dur_ = move_dur_;
    }

    int pick_random_target() {
        float total = 0;
        for (const GlanceTarget& g : cfg_.targets) total += std::max(0.0f, g.weight);
        if (total <= 0.0f) return 0;
        const float r = uni(0.0f, total);
        float acc = 0;
        for (int i = 0; i < kNumTargets; i++) {
            acc += std::max(0.0f, cfg_.targets[i].weight);
            if (r <= acc) return i;
        }
        return kNumTargets - 1;
    }

    void update_glance(float dt) {
        state_t_ += dt;
        switch (state_) {
        case Idle: {
            const bool forced = cfg_.blinker_enabled && cfg_.blinker_glance && blinker_ != 0 &&
                                t_ - last_forced_t_ > cfg_.mirror_repeat;
            if (forced) {
                last_forced_t_ = t_;
                start_glance(blinker_ < 0 ? kMirrorLeft : kMirrorRight, cfg_.mirror_scale);
            } else if (state_t_ >= state_dur_) {
                if (cfg_.glance) {
                    start_glance(pick_random_target());
                } else {
                    state_t_ = 0;
                    state_dur_ = idle_time();
                }
            }
            break;
        }
        case Move: {
            const float k = min_jerk(state_t_ / state_dur_);
            g_off_[0] = g_from_[0] + (g_to_[0] - g_from_[0]) * k;
            g_off_[1] = g_from_[1] + (g_to_[1] - g_from_[1]) * k;
            if (state_t_ >= state_dur_) {
                g_off_[0] = g_to_[0];
                g_off_[1] = g_to_[1];
                state_ = Hold;
                state_t_ = 0;
                state_dur_ = hold_dur_;
            }
            break;
        }
        case Hold:
            if (state_t_ >= state_dur_) {
                g_from_[0] = g_off_[0];
                g_from_[1] = g_off_[1];
                g_to_[0] = g_to_[1] = 0;
                state_ = Return;
                state_t_ = 0;
                state_dur_ = move_dur_ * 1.15f;
            }
            break;
        case Return: {
            const float k = min_jerk(state_t_ / state_dur_);
            g_off_[0] = g_from_[0] * (1 - k);
            g_off_[1] = g_from_[1] * (1 - k);
            if (state_t_ >= state_dur_) {
                g_off_[0] = g_off_[1] = 0;
                state_ = Idle;
                state_t_ = 0;
                state_dur_ = idle_time();
            }
            break;
        }
        }
    }

    Config cfg_;
    bool custom_active_[kNumCustom] = {false, false, false};
    float custom_w_[kNumCustom] = {0, 0, 0};
    float custom_v_[kNumCustom] = {0, 0, 0};
    Pose custom_final_;
    std::mt19937 rng_;
    std::vector<Wave> waves_[6];
    float t_ = 0;
    State state_ = Idle;
    float state_t_ = 0, state_dur_ = 0, hold_dur_ = 0.5f, move_dur_ = 0.4f;
    float g_from_[2] = {0, 0}, g_to_[2] = {0, 0}, g_off_[2] = {0, 0};
    const char* target_name_ = "";
    float last_forced_t_ = -99.0f;
    float steer_ = 0, steer_f_ = 0, blink_f_ = 0;
    Telemetry tel_;
    float acc_f_[3] = {0, 0, 0};
    Spring sp_[3];
    float slope_f_ = 0, camber_f_ = 0;
    int blinker_ = 0;
};

}  // namespace ets2dhm
