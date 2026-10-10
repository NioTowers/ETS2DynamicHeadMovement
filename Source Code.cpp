#include <windows.h>
#include <cmath>
#include <thread>
#include <atomic>
#include <string>
#include <algorithm>

#define SCS_TELEMETRY_PLUGIN_API __declspec(dllexport)

struct HeadPose {
    float yaw;
    float pitch;
    float roll;
    float x;
    float y;
    float z;
};

#pragma pack(push, 1)
struct FT_SharedMem {
    int dataID;
    int camWidth;
    int camHeight;
    HeadPose pose;
};
#pragma pack(pop)

struct Config {
    int ini_version = 1;
    float amp = 1.0f;
    float smooth = 0.5f;
    float rate = 60.0f;
    int glance = 1;
    int blinker_glance = 1;
    float mirror_scale = 1.0f;
    int flip_yaw = 0;
    int flip_pitch = 0;
    float glance_min = 0.0f;
    float glance_max = 0.0f;
    int steer_enabled = 1;
    float steer_deg = 30.0f;
    float steer_power = 1.0f;
    int blinker_enabled = 1;
    float blinker_deg = 45.0f;
    int dyn_enabled = 1;
    float dyn_amp = 1.0f;
    float dyn_freq = 5.0f;
    float dyn_damp = 0.8f;
    float lean_roll = 0.0f;
    float lean_pitch = 0.0f;
    float pos_scale = 1.0f;
    float slope_comp = 0.0f;
    float camber_comp = 0.0f;
    float cabin_comp = 0.0f;
    float speed_gate = 0.0f;
    int flip_x = 0;
    int flip_y = 0;
    int flip_z = 0;
};

static Config g_config;
static std::atomic<bool> g_running(false);
static std::thread g_workerThread;
static HANDLE g_hMapFile = NULL;
static FT_SharedMem* g_pSharedMem = nullptr;

static float g_truckSteering = 0.0f;
static int g_truckLBlinker = 0;
static int g_truckRBlinker = 0;
static float g_truckSpeed = 0.0f;
static float g_linearAcc[3] = {0.0f, 0.0f, 0.0f};

void LoadConfig(const char* iniPath) {
    char buf[64];
    auto readFloat = [&](const char* key, float def) -> float {
        GetPrivateProfileStringA("autohead", key, "", buf, sizeof(buf), iniPath);
        return buf[0] ? (float)atof(buf) : def;
    };
    auto readInt = [&](const char* key, int def) -> int {
        return GetPrivateProfileIntA("autohead", key, def, iniPath);
    };

    g_config.ini_version = readInt("ini_version", 1);
    g_config.amp = readFloat("amp", 1.0f);
    g_config.smooth = readFloat("smooth", 0.5f);
    g_config.rate = readFloat("rate", 60.0f);
    g_config.glance = readInt("glance", 1);
    g_config.blinker_glance = readInt("blinker_glance", 1);
    g_config.mirror_scale = readFloat("mirror_scale", 1.0f);
    g_config.flip_yaw = readInt("flip_yaw", 0);
    g_config.flip_pitch = readInt("flip_pitch", 0);
    g_config.glance_min = readFloat("glance_min", 0.0f);
    g_config.glance_max = readFloat("glance_max", 0.0f);
    g_config.steer_enabled = readInt("steer_enabled", 1);
    g_config.steer_deg = readFloat("steer_deg", 30.0f);
    g_config.steer_power = readFloat("steer_power", 1.0f);
    g_config.blinker_enabled = readInt("blinker_enabled", 1);
    g_config.blinker_deg = readFloat("blinker_deg", 45.0f);
    g_config.dyn_enabled = readInt("dyn_enabled", 1);
    g_config.dyn_amp = readFloat("dyn_amp", 1.0f);
    g_config.dyn_freq = readFloat("dyn_freq", 5.0f);
    g_config.dyn_damp = readFloat("dyn_damp", 0.8f);
    g_config.lean_roll = readFloat("lean_roll", 0.0f);
    g_config.lean_pitch = readFloat("lean_pitch", 0.0f);
    g_config.pos_scale = readFloat("pos_scale", 1.0f);
    g_config.slope_comp = readFloat("slope_comp", 0.0f);
    g_config.camber_comp = readFloat("camber_comp", 0.0f);
    g_config.cabin_comp = readFloat("cabin_comp", 0.0f);
    g_config.speed_gate = readFloat("speed_gate", 0.0f);
    g_config.flip_x = readInt("flip_x", 0);
    g_config.flip_y = readInt("flip_y", 0);
    g_config.flip_z = readInt("flip_z", 0);
}

void WorkerLoop() {
    HeadPose targetPose = {0};
    HeadPose currentPose = {0};

    float intervalMs = 1000.0f / (g_config.rate > 0.0f ? g_config.rate : 60.0f);

    while (g_running) {
        float targetYaw = 0.0f;
        if (g_config.steer_enabled) {
            float sign = (g_truckSteering < 0.0f) ? -1.0f : 1.0f;
            targetYaw += sign * std::pow(std::abs(g_truckSteering), g_config.steer_power) * g_config.steer_deg;
        }

        if (g_config.blinker_enabled) {
            if (g_truckLBlinker) targetYaw += g_config.blinker_deg;
            if (g_truckRBlinker) targetYaw -= g_config.blinker_deg;
        }

        float targetPitch = 0.0f;
        float targetRoll = 0.0f;
        if (g_config.dyn_enabled) {
            targetPitch += g_linearAcc[2] * g_config.lean_pitch * g_config.dyn_amp;
            targetRoll  += g_linearAcc[0] * g_config.lean_roll * g_config.dyn_amp;
        }

        targetPose.yaw = targetYaw * (g_config.flip_yaw ? -1.0f : 1.0f) * g_config.amp;
        targetPose.pitch = targetPitch * (g_config.flip_pitch ? -1.0f : 1.0f) * g_config.amp;
        targetPose.roll = targetRoll * g_config.amp;

        float alpha = std::clamp(1.0f - g_config.smooth, 0.01f, 1.0f);
        currentPose.yaw += (targetPose.yaw - currentPose.yaw) * alpha;
        currentPose.pitch += (targetPose.pitch - currentPose.pitch) * alpha;
        currentPose.roll += (targetPose.roll - currentPose.roll) * alpha;

        if (g_pSharedMem) {
            g_pSharedMem->dataID++;
            g_pSharedMem->pose = currentPose;
        }

        Sleep(static_cast<DWORD>(intervalMs));
    }
}

extern "C" {

SCS_TELEMETRY_PLUGIN_API int scs_telemetry_init(void* params) {
    char modulePath[MAX_PATH];
    GetModuleFileNameA(NULL, modulePath, MAX_PATH);
    std::string iniPath = "autohead.ini";
    LoadConfig(iniPath.c_str());

    g_hMapFile = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(FT_SharedMem), "FT_SharedMem");
    if (!g_hMapFile) {
        return -1;
    }

    g_pSharedMem = (FT_SharedMem*)MapViewOfFile(g_hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(FT_SharedMem));
    if (!g_pSharedMem) {
        CloseHandle(g_hMapFile);
        g_hMapFile = NULL;
        return -1;
    }

    g_running = true;
    g_workerThread = std::thread(WorkerLoop);
    return 0;
}

SCS_TELEMETRY_PLUGIN_API void scs_telemetry_shutdown() {
    g_running = false;
    if (g_workerThread.joinable()) {
        g_workerThread.join();
    }
    if (g_pSharedMem) {
        UnmapViewOfFile(g_pSharedMem);
        g_pSharedMem = nullptr;
    }
    if (g_hMapFile) {
        CloseHandle(g_hMapFile);
        g_hMapFile = NULL;
    }
}

}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    return TRUE;
}