#include <windows.h>
#include <cmath>
#include <thread>
#include <atomic>
#include <string>
#include <algorithm>

#include "scssdk_telemetry.h"
#include "eurotrucks2/scssdk_eut2.h"
#include "amtrucks/scssdk_amt.h"
#include "shared.hpp"

#define SCS_TELEMETRY_PLUGIN_API __declspec(dllexport)

static std::atomic<bool> g_running(false);
static std::thread g_workerThread;
static std::atomic<float> g_outYaw(0.0f);
static std::atomic<float> g_outPitch(0.0f);
static std::atomic<float> g_outRoll(0.0f);
SCS_TELEMETRY_PLUGIN_API scs_value_t scs_input_trackir_yaw(const scs_string_t name, const scs_u32_t index, const scs_value_t *value, const scs_context_t context) {
    scs_value_t result; result.type = SCS_VALUE_TYPE_float;
    result.value_float.value = g_outYaw.load() / 360.0f;
    return result;
}
SCS_TELEMETRY_PLUGIN_API scs_value_t scs_input_trackir_pitch(const scs_string_t name, const scs_u32_t index, const scs_value_t *value, const scs_context_t context) {
    scs_value_t result; result.type = SCS_VALUE_TYPE_float;
    result.value_float.value = g_outPitch.load() / 360.0f;
    return result;
}
SCS_TELEMETRY_PLUGIN_API scs_value_t scs_input_trackir_roll(const scs_string_t name, const scs_u32_t index, const scs_value_t *value, const scs_context_t context) {
    scs_value_t result; result.type = SCS_VALUE_TYPE_float;
    result.value_float.value = g_outRoll.load() / 360.0f;
    return result;
}
void NativeWorkerLoop() {
    while (g_running) {
        g_outYaw.store(g_sharedState.currentPose.yaw);
        g_outPitch.store(g_sharedState.currentPose.pitch);
        g_outRoll.store(g_sharedState.currentPose.roll);
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
}
extern "C" {
SCS_TELEMETRY_PLUGIN_API int scs_telemetry_init(void* params) {
    scs_telemetry_init_params_v1_t *scs_params = (scs_telemetry_init_params_v1_t *)params;
    if (scs_params && scs_params->register_node) {
        scs_params->register_node("input.trackir.yaw", scs_input_trackir_yaw, NULL);
        scs_params->register_node("input.trackir.pitch", scs_input_trackir_pitch, NULL);
        scs_params->register_node("input.trackir.roll", scs_input_trackir_roll, NULL);
    }
    g_running = true;
    g_workerThread = std::thread(NativeWorkerLoop);
    return 0;
}
SCS_TELEMETRY_PLUGIN_API void scs_telemetry_shutdown() {
    g_running = false;
    if (g_workerThread.joinable()) g_workerThread.join();
}
}
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) { return TRUE; }
