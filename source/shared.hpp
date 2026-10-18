#pragma once
#include <atomic>
#include <mutex>
#include <string>

#include "ets2dhm_core.hpp"

namespace shared {

struct UiSettings {
    int hotkey = 0x2D;
    float scale = 0.0f;
    float alpha = 0.92f;
};

struct Live {
    std::atomic<bool> have_speed{false};
    std::atomic<float> speed{0.0f};
    std::atomic<float> steer_left{0.0f};
    std::atomic<float> acc[3] = {0.0f, 0.0f, 0.0f};
    std::atomic<float> world_pitch{0.0f}, world_roll{0.0f};
    std::atomic<float> cab_pitch{0.0f}, cab_roll{0.0f};
    std::atomic<int> lblinker{0}, rblinker{0};
    std::atomic<float> out[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    std::atomic<bool> shm_ok{false};
    std::atomic<bool> custom_active[ets2dhm::kNumCustom] = {false, false, false};
};

inline std::mutex mu;
inline ets2dhm::Config cfg;
inline UiSettings ui;
inline Live live;

bool save_config();
bool reload_config();
std::string ini_path();
void log_msg(const std::string& s);

}  // namespace shared

namespace overlay {
void start(void* module);
void stop();
}
