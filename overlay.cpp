#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "shared.hpp"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
using Clock = std::chrono::steady_clock;

constexpr float kBaseW = 640.0f;
constexpr float kBaseH = 700.0f;
constexpr float kToastW = 560.0f;
constexpr float kToastH = 44.0f;
constexpr float kCaption = 26.0f;
constexpr int kHist = 240;

HMODULE g_module = nullptr;
std::atomic<bool> g_running{false};
HANDLE g_thread = nullptr;
HWND g_hwnd = nullptr;
HWND g_game = nullptr;
bool g_imgui_ready = false;
std::atomic<bool> g_visible{false};

SetCursorPosFn g_orig_setcursorpos = nullptr;
ClipCursorFn g_orig_clipcursor = nullptr;
void** g_iat_setcursorpos = nullptr;
void** g_iat_clipcursor = nullptr;

ets2dhm::Config g_lc;
shared::UiSettings g_lu;
bool g_local_valid = false;
bool g_dirty = false;
Clock::time_point g_dirty_time;
std::string g_status_msg;
Clock::time_point g_status_time;
Clock::time_point g_toast_start;
bool g_toast_started = false;

int g_capture = -1;
bool g_prev_key = false;
bool g_prev_custom[ets2dhm::kNumCustom] = {false, false, false};
bool g_toggle_state[ets2dhm::kNumCustom] = {false, false, false};
bool g_preview[ets2dhm::kNumCustom] = {false, false, false};

float g_hist[3][kHist];
int g_hist_pos = 0;

int g_win_x = 60, g_win_y = 60;
bool g_pos_set = false;
bool g_first_vis_frame = false;

HDC g_memdc = nullptr;
HBITMAP g_bmp = nullptr;
uint32_t* g_pixels = nullptr;
int g_bw = 0, g_bh = 0;

const unsigned char* g_font_alpha = nullptr;
int g_font_w = 0, g_font_h = 0;

void dlog(const std::string& m) { shared::log_msg(m); }

const char* key_name(int vk, char* buf, int n) {
    UINT sc = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    LONG lp = LONG(sc) << 16;
    switch (vk) {
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
            lp |= 1 << 24;
            break;
        default:
            break;
    }
    if (GetKeyNameTextA(lp, buf, n) <= 0) std::snprintf(buf, size_t(n), "0x%02X", vk);
    return buf;
}

BOOL WINAPI hk_setcursorpos(int x, int y) {
    if (g_visible.load()) return TRUE;
    return g_orig_setcursorpos ? g_orig_setcursorpos(x, y) : FALSE;
}

BOOL WINAPI hk_clipcursor(const RECT* r) {
    if (g_visible.load()) return TRUE;
    return g_orig_clipcursor ? g_orig_clipcursor(r) : FALSE;
}

void** find_iat(HMODULE m, const char* dll, const char* name) {
    auto base = reinterpret_cast<BYTE*>(m);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; imp->Name; imp++) {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), dll) != 0) continue;
        auto thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        auto orig = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        for (; orig->u1.AddressOfData; orig++, thunk++) {
            if (IMAGE_SNAP_BY_ORDINAL(orig->u1.Ordinal)) continue;
            auto ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + orig->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(ibn->Name), name) == 0)
                return reinterpret_cast<void**>(&thunk->u1.Function);
        }
    }
    return nullptr;
}

bool patch_slot(void** slot, void* value, void** old) {
    DWORD prot = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &prot)) return false;
    if (old) *old = *slot;
    *slot = value;
    DWORD tmp = 0;
    VirtualProtect(slot, sizeof(void*), prot, &tmp);
    return true;
}

void hook_cursor_calls() {
    HMODULE exe = GetModuleHandleA(nullptr);
    g_iat_setcursorpos = find_iat(exe, "user32.dll", "SetCursorPos");
    g_iat_clipcursor = find_iat(exe, "user32.dll", "ClipCursor");
    void* old = nullptr;
    if (g_iat_setcursorpos && patch_slot(g_iat_setcursorpos, reinterpret_cast<void*>(hk_setcursorpos), &old))
        g_orig_setcursorpos = reinterpret_cast<SetCursorPosFn>(old);
    else
        g_iat_setcursorpos = nullptr;
    if (g_iat_clipcursor && patch_slot(g_iat_clipcursor, reinterpret_cast<void*>(hk_clipcursor), &old))
        g_orig_clipcursor = reinterpret_cast<ClipCursorFn>(old);
    else
        g_iat_clipcursor = nullptr;
}

void unhook_cursor_calls() {
    if (g_iat_setcursorpos && g_orig_setcursorpos) patch_slot(g_iat_setcursorpos, reinterpret_cast<void*>(g_orig_setcursorpos), nullptr);
    if (g_iat_clipcursor && g_orig_clipcursor) patch_slot(g_iat_clipcursor, reinterpret_cast<void*>(g_orig_clipcursor), nullptr);
    g_iat_setcursorpos = g_iat_clipcursor = nullptr;
}

struct FindCtx {
    DWORD pid;
    HWND best;
    long best_area;
};

BOOL CALLBACK enum_windows(HWND h, LPARAM lp) {
    auto* c = reinterpret_cast<FindCtx*>(lp);
    if (h == g_hwnd || !IsWindowVisible(h)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid) return TRUE;
    if (GetWindow(h, GW_OWNER)) return TRUE;
    RECT rc{};
    GetClientRect(h, &rc);
    const long area = long(rc.right) * long(rc.bottom);
    if (rc.right < 320 || rc.bottom < 240) return TRUE;
    if (area > c->best_area) {
        c->best_area = area;
        c->best = h;
    }
    return TRUE;
}

HWND find_game_window() {
    FindCtx c{GetCurrentProcessId(), nullptr, 0};
    EnumWindows(enum_windows, reinterpret_cast<LPARAM>(&c));
    return c.best;
}

bool process_foreground() {
    HWND fg = GetForegroundWindow();
    if (!fg) return true;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

bool game_rect(RECT* out) {
    if (!g_game || !IsWindow(g_game)) g_game = find_game_window();
    if (!g_game) return false;
    RECT rc{};
    if (!GetClientRect(g_game, &rc)) return false;
    POINT p{0, 0};
    ClientToScreen(g_game, &p);
    out->left = p.x;
    out->top = p.y;
    out->right = p.x + rc.right;
    out->bottom = p.y + rc.bottom;
    return true;
}

float auto_scale() {
    RECT rc{};
    if (game_rect(&rc) && rc.bottom > rc.top) return std::clamp(float(rc.bottom - rc.top) / 1080.0f, 0.8f, 2.5f);
    return 1.0f;
}

float cur_scale() { return g_lu.scale > 0.0f ? g_lu.scale : auto_scale(); }

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_imgui_ready && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return TRUE;
    switch (msg) {
        case WM_NCHITTEST: {
            if (!g_visible.load()) return HTTRANSPARENT;
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            if (pt.y >= 0 && pt.y < int(kCaption * cur_scale())) return HTCAPTION;
            return HTCLIENT;
        }
        case WM_MOUSEACTIVATE:
            return g_visible.load() ? MA_ACTIVATE : MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ensure_surface(int w, int h) {
    if (g_bmp && g_bw == w && g_bh == h) return;
    if (g_bmp) { DeleteObject(g_bmp); g_bmp = nullptr; }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    g_bmp = CreateDIBSection(g_memdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    g_pixels = static_cast<uint32_t*>(bits);
    g_bw = w;
    g_bh = h;
    SelectObject(g_memdc, g_bmp);
}

inline float font_alpha_at(float u, float v) {
    float fx = u * g_font_w - 0.5f;
    float fy = v * g_font_h - 0.5f;
    int x0 = int(std::floor(fx));
    int y0 = int(std::floor(fy));
    float tx = fx - x0;
    float ty = fy - y0;
    auto px = [&](int x, int y) -> float {
        x = std::clamp(x, 0, g_font_w - 1);
        y = std::clamp(y, 0, g_font_h - 1);
        return float(g_font_alpha[y * g_font_w + x]);
    };
    float a = px(x0, y0) * (1 - tx) + px(x0 + 1, y0) * tx;
    float b = px(x0, y0 + 1) * (1 - tx) + px(x0 + 1, y0 + 1) * tx;
    return (a * (1 - ty) + b * ty) * (1.0f / 255.0f);
}

void raster_triangle(const ImDrawVert& a, const ImDrawVert& b, const ImDrawVert& c, const ImVec4& clip, bool flat) {
    float ax = a.pos.x, ay = a.pos.y, bx = b.pos.x, by = b.pos.y, cx = c.pos.x, cy = c.pos.y;
    float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (std::fabs(area) < 1e-6f) return;
    const ImDrawVert *v0 = &a, *v1 = &b, *v2 = &c;
    if (area < 0) {
        std::swap(v1, v2);
        area = -area;
    }
    const float x0 = v0->pos.x, y0 = v0->pos.y, x1 = v1->pos.x, y1 = v1->pos.y, x2 = v2->pos.x, y2 = v2->pos.y;
    int minx = int(std::floor(std::min({x0, x1, x2})));
    int maxx = int(std::ceil(std::max({x0, x1, x2})));
    int miny = int(std::floor(std::min({y0, y1, y2})));
    int maxy = int(std::ceil(std::max({y0, y1, y2})));
    minx = std::max(minx, int(clip.x));
    miny = std::max(miny, int(clip.y));
    maxx = std::min(maxx, int(clip.z));
    maxy = std::min(maxy, int(clip.w));
    minx = std::max(minx, 0);
    miny = std::max(miny, 0);
    maxx = std::min(maxx, g_bw);
    maxy = std::min(maxy, g_bh);
    if (minx >= maxx || miny >= maxy) return;

    const float inv = 1.0f / area;
    const float c0[4] = {float(v0->col & 255), float((v0->col >> 8) & 255), float((v0->col >> 16) & 255), float(v0->col >> 24)};
    const float c1[4] = {float(v1->col & 255), float((v1->col >> 8) & 255), float((v1->col >> 16) & 255), float(v1->col >> 24)};
    const float c2[4] = {float(v2->col & 255), float((v2->col >> 8) & 255), float((v2->col >> 16) & 255), float(v2->col >> 24)};

    for (int y = miny; y < maxy; y++) {
        const float py = float(y) + 0.5f;
        uint32_t* row = g_pixels + size_t(y) * size_t(g_bw);
        for (int x = minx; x < maxx; x++) {
            const float px = float(x) + 0.5f;
            const float w0 = ((x1 - px) * (y2 - py) - (y1 - py) * (x2 - px)) * inv;
            const float w1 = ((x2 - px) * (y0 - py) - (y2 - py) * (x0 - px)) * inv;
            const float w2 = 1.0f - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            float r, g, bl, al;
            if (flat) {
                r = c0[0]; g = c0[1]; bl = c0[2]; al = c0[3];
            } else {
                r = c0[0] * w0 + c1[0] * w1 + c2[0] * w2;
                g = c0[1] * w0 + c1[1] * w1 + c2[1] * w2;
                bl = c0[2] * w0 + c1[2] * w1 + c2[2] * w2;
                al = c0[3] * w0 + c1[3] * w1 + c2[3] * w2;
            }
            const float u = v0->uv.x * w0 + v1->uv.x * w1 + v2->uv.x * w2;
            const float v = v0->uv.y * w0 + v1->uv.y * w1 + v2->uv.y * w2;
            al *= font_alpha_at(u, v);
            if (al <= 0.5f) continue;
            const float sa = al * (1.0f / 255.0f);
            const uint32_t d = row[x];
            const float inv_a = 1.0f - sa;
            const uint32_t dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255, da = d >> 24;
            const uint32_t nr = uint32_t(std::min(255.0f, r * sa + dr * inv_a));
            const uint32_t ng = uint32_t(std::min(255.0f, g * sa + dg * inv_a));
            const uint32_t nb = uint32_t(std::min(255.0f, bl * sa + db * inv_a));
            const uint32_t na = uint32_t(std::min(255.0f, al + da * inv_a));
            row[x] = (na << 24) | (nr << 16) | (ng << 8) | nb;
        }
    }
}

void raster_draw_data(const ImDrawData* dd) {
    for (int n = 0; n < dd->CmdListsCount; n++) {
        const ImDrawList* cl = dd->CmdLists[n];
        const ImDrawVert* vtx = cl->VtxBuffer.Data;
        const ImDrawIdx* idx = cl->IdxBuffer.Data;
        for (const ImDrawCmd& cmd : cl->CmdBuffer) {
            if (cmd.UserCallback) continue;
            ImVec4 clip(cmd.ClipRect.x - dd->DisplayPos.x, cmd.ClipRect.y - dd->DisplayPos.y,
                        cmd.ClipRect.z - dd->DisplayPos.x, cmd.ClipRect.w - dd->DisplayPos.y);
            if (clip.z <= clip.x || clip.w <= clip.y) continue;
            const ImDrawVert* vb = vtx + cmd.VtxOffset;
            const ImDrawIdx* ib = idx + cmd.IdxOffset;
            for (unsigned i = 0; i + 2 < cmd.ElemCount; i += 3) {
                const ImDrawVert& a = vb[ib[i]];
                const ImDrawVert& b = vb[ib[i + 1]];
                const ImDrawVert& c = vb[ib[i + 2]];
                const bool flat = a.col == b.col && b.col == c.col;
                raster_triangle(a, b, c, clip, flat);
            }
        }
    }
}

void present_surface(int x, int y, int w, int h) {
    HDC screen = GetDC(nullptr);
    POINT dst{x, y};
    SIZE sz{w, h};
    POINT src{0, 0};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(g_hwnd, screen, &dst, &sz, g_memdc, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

void load_local() {
    std::lock_guard<std::mutex> lk(shared::mu);
    g_lc = shared::cfg;
    g_lu = shared::ui;
    g_local_valid = true;
}

void push_cfg() {
    {
        std::lock_guard<std::mutex> lk(shared::mu);
        shared::cfg = g_lc;
        shared::ui = g_lu;
    }
    g_dirty = true;
    g_dirty_time = Clock::now();
}

void set_status(const std::string& s) {
    g_status_msg = s;
    g_status_time = Clock::now();
}

void flush_save() {
    if (!g_dirty) return;
    g_dirty = false;
    set_status(shared::save_config() ? "Saved" : "Save failed");
}

void apply_window_style(bool interactive) {
    LONG_PTR ex = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
    if (!interactive) ex |= WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;
    SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, ex);
}

void set_visible(bool v) {
    if (v == g_visible.load()) return;
    if (v) {
        load_local();
        RECT gr{};
        if (game_rect(&gr) && !g_pos_set) {
            g_win_x = gr.left + 40;
            g_win_y = gr.top + 40;
            g_pos_set = true;
        }
        g_first_vis_frame = true;
        g_visible.store(true);
        apply_window_style(true);
        ClipCursor(nullptr);
        ShowWindow(g_hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(g_hwnd);
    } else {
        g_visible.store(false);
        g_capture = -1;
        flush_save();
        apply_window_style(false);
        ShowWindow(g_hwnd, SW_HIDE);
        if (g_game && IsWindow(g_game)) SetForegroundWindow(g_game);
    }
}

bool tip(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", text);
    }
    return true;
}

bool slider(const char* label, float* v, float lo, float hi, const char* fmt, const char* help) {
    bool c = ImGui::SliderFloat(label, v, lo, hi, fmt);
    if (help) tip(help);
    return c;
}

bool check(const char* label, bool* v, const char* help) {
    bool c = ImGui::Checkbox(label, v);
    if (help) tip(help);
    return c;
}

void plot(const char* label, int idx, float range) {
    float tmp[kHist];
    for (int i = 0; i < kHist; i++) tmp[i] = g_hist[idx][(g_hist_pos + i) % kHist];
    char ov[48];
    std::snprintf(ov, sizeof(ov), "%+.1f", tmp[kHist - 1]);
    ImGui::PlotLines(label, tmp, kHist, 0, ov, -range, range, ImVec2(0, 46));
}

void tab_status() {
    using namespace shared;
    ImGui::TextColored(live.shm_ok.load() ? ImVec4(0.4f, 1, 0.5f, 1) : ImVec4(1, 0.4f, 0.4f, 1),
                       live.shm_ok.load() ? "FT_SharedMem: OK" : "FT_SharedMem: not available");
    ImGui::Separator();
    ImGui::Text("Telemetry");
    if (live.have_speed.load())
        ImGui::Text("Speed: %.0f km/h", live.speed.load() * 3.6f);
    else
        ImGui::TextDisabled("Speed: waiting for the game...");
    ImGui::Text("Steering: %+.2f", -live.steer_left.load());
    ImGui::Text("Blinkers: L %s   R %s", live.lblinker.load() ? "ON" : "off", live.rblinker.load() ? "ON" : "off");
    ImGui::Text("Accel: %+.2f %+.2f %+.2f m/s2", live.acc[0].load(), live.acc[1].load(), live.acc[2].load());
    ImGui::Text("Truck pitch/roll: %+.1f / %+.1f deg", live.world_pitch.load(), live.world_roll.load());
    ImGui::Separator();
    ImGui::Text("Output (deg, before game smoothing)");
    plot("Yaw", 0, 30.0f);
    plot("Pitch", 1, 12.0f);
    plot("Roll", 2, 6.0f);
}

void tab_motion(bool& ch) {
    ch |= check("Enabled", &g_lc.enabled, "Master switch. Off = camera stays centered.");
    ch |= slider("Overall amplitude", &g_lc.amp, 0.0f, 3.0f, "%.2f", "Scales sway and glances. 1.0 = default.");
    ch |= slider("Output smoothing", &g_lc.smooth, 0.01f, 1.0f, "%.2f", "Higher = snappier, lower = smoother.");
    ch |= slider("Update rate (Hz)", &g_lc.rate, 30.0f, 240.0f, "%.0f", "How many times per second the head pose is computed and sent to the game. 120 is plenty; lower only if you see CPU load.");
    ImGui::SeparatorText("Directions");
    ch |= check("Flip yaw (left/right)", &g_lc.flip_yaw, "Default ON. Also flips roll and steering/blinker direction.");
    ch |= check("Flip pitch (up/down)", &g_lc.flip_pitch, "Default ON.");
    ch |= check("Flip X", &g_lc.flip_x, nullptr);
    ImGui::SameLine();
    ch |= check("Flip Y", &g_lc.flip_y, nullptr);
    ImGui::SameLine();
    ch |= check("Flip Z", &g_lc.flip_z, nullptr);
    ImGui::SeparatorText("Positional (x,y,z)");
    ch |= slider("Position gain (auto movement)", &g_lc.pos_gain, 0.1f, 10.0f, "%.2f", "Multiplies x/y/z movement from sway, braking and cornering only. Custom keys have their own gain.");
    ch |= slider("Position scale (auto movement)", &g_lc.pos_scale, 0.0f, 5.0f, "%.2f", "Strength of the physical head shift (x/y/z) from sway, braking and cornering. 0 = rotation only.");
}

void tab_glance(bool& ch) {
    ch |= check("Random glances", &g_lc.glance, "Occasional looks at mirrors / dashboard / road.");
    ImGui::BeginDisabled(!g_lc.glance);
    ch |= slider("Min interval (s)", &g_lc.glance_min, 3.0f, 300.0f, "%.0f", nullptr);
    ch |= slider("Max interval (s)", &g_lc.glance_max, 3.0f, 600.0f, "%.0f", nullptr);
    if (g_lc.glance_max < g_lc.glance_min) g_lc.glance_max = g_lc.glance_min;
    ImGui::EndDisabled();
    ImGui::SeparatorText("Targets (degrees, +yaw = right)");
    if (ImGui::BeginTable("targets", 6, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Weight");
        ImGui::TableSetupColumn("Yaw");
        ImGui::TableSetupColumn("Pitch");
        ImGui::TableSetupColumn("Hold min");
        ImGui::TableSetupColumn("Hold max");
        ImGui::TableHeadersRow();
        for (int i = 0; i < ets2dhm::kNumTargets; i++) {
            auto& t = g_lc.targets[i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(t.name);
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            ch |= ImGui::DragFloat("##w", &t.weight, 0.01f, 0.0f, 1.0f, "%.2f");
            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(-FLT_MIN);
            ch |= ImGui::DragFloat("##y", &t.yaw, 0.2f, -90.0f, 90.0f, "%.1f");
            ImGui::TableSetColumnIndex(3);
            ImGui::SetNextItemWidth(-FLT_MIN);
            ch |= ImGui::DragFloat("##p", &t.pitch, 0.2f, -45.0f, 45.0f, "%.1f");
            ImGui::TableSetColumnIndex(4);
            ImGui::SetNextItemWidth(-FLT_MIN);
            ch |= ImGui::DragFloat("##h0", &t.hold_min, 0.02f, 0.1f, 5.0f, "%.2f");
            ImGui::TableSetColumnIndex(5);
            ImGui::SetNextItemWidth(-FLT_MIN);
            ch |= ImGui::DragFloat("##h1", &t.hold_max, 0.02f, 0.1f, 5.0f, "%.2f");
            if (t.hold_max < t.hold_min) t.hold_max = t.hold_min;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Drag values, or Ctrl+click to type. Weight 0 disables a target.");
    if (ImGui::Button("Reset targets")) {
        ets2dhm::Config d;
        std::copy(std::begin(d.targets), std::end(d.targets), g_lc.targets);
        ch = true;
    }
}

void tab_steer(bool& ch) {
    ImGui::SeparatorText("Steering follow");
    ch |= check("Camera follows steering", &g_lc.steer_enabled, nullptr);
    ImGui::BeginDisabled(!g_lc.steer_enabled);
    ch |= slider("Max rotation (deg)", &g_lc.steer_deg, 0.0f, 40.0f, "%.1f", "Head yaw at full steering lock.");
    ch |= slider("Response curve", &g_lc.steer_power, 0.5f, 3.0f, "%.2f", "1 = linear. Higher = only reacts to bigger turns.");
    ch |= slider("Speed gate (m/s)", &g_lc.speed_gate, 0.0f, 15.0f, "%.1f", "Speed (m/s) at which steering/dynamics reach full effect. Below it they fade out, so the head stays still when parked.");
    ImGui::EndDisabled();
    ImGui::SeparatorText("Blinker");
    ch |= check("Head bias toward blinker side", &g_lc.blinker_enabled, nullptr);
    ImGui::BeginDisabled(!g_lc.blinker_enabled);
    ch |= slider("Bias (deg)", &g_lc.blinker_deg, 0.0f, 15.0f, "%.1f", nullptr);
    ImGui::EndDisabled();
    ch |= check("Mirror glance on blinker", &g_lc.blinker_glance, "Looks at the side mirror when a blinker turns on.");
    ImGui::BeginDisabled(!g_lc.blinker_glance);
    ch |= slider("Mirror glance size", &g_lc.mirror_scale, 0.0f, 1.5f, "%.2f", "1.0 = full, 0.3 = subtle.");
    ch |= slider("Repeat guard (s)", &g_lc.mirror_repeat, 1.0f, 30.0f, "%.0f", "Minimum time between blinker glances.");
    ImGui::EndDisabled();
}

void tab_dyn(bool& ch) {
    ch |= check("Vehicle dynamics", &g_lc.dyn_enabled, "Head reacts to braking, acceleration, cornering and road bumps.");
    ImGui::BeginDisabled(!g_lc.dyn_enabled);
    ch |= slider("Intensity", &g_lc.dyn_amp, 0.0f, 3.0f, "%.2f", nullptr);
    ch |= slider("Spring frequency (Hz)", &g_lc.dyn_freq, 0.5f, 4.0f, "%.2f", "Neck stiffness. Higher = tighter.");
    ch |= slider("Damping", &g_lc.dyn_damp, 0.1f, 1.5f, "%.2f", "Higher = less wobble.");
    ch |= slider("Lean roll", &g_lc.lean_roll, 0.0f, 3.0f, "%.2f", "How much the head tilts (roll) when it is pushed sideways in corners. Higher = leans more into turns.");
    ch |= slider("Lean pitch", &g_lc.lean_pitch, 0.0f, 3.0f, "%.2f", "How much the head nods (pitch) when braking or accelerating. Higher = bigger nod.");
    ImGui::SeparatorText("Road and cabin compensation");
    ch |= slider("Slope", &g_lc.slope_comp, 0.0f, 1.0f, "%.2f", "Uphill/downhill: how much the head tilts to stay looking at the horizon. 0 = follow the truck, 1 = fully level.");
    ch |= slider("Camber", &g_lc.camber_comp, -1.0f, 1.0f, "%.2f", "Road tilt left/right: how much the head tilts to keep the horizon level. Negative reverses the direction.");
    ch |= slider("Cabin suspension", &g_lc.cabin_comp, 0.0f, 1.0f, "%.2f", "Counters the cabin rocking on its suspension, so the head moves less with bumps. 0 = cabin rocking passes through fully.");
    ImGui::EndDisabled();
}

void tab_settings(bool& ch) {
    ImGui::SeparatorText("Config file");
    if (ImGui::Button("Save now")) { push_cfg(); flush_save(); }
    ImGui::SameLine();
    if (ImGui::Button("Reload from file")) {
        shared::reload_config();
        load_local();
        set_status("Reloaded");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset to defaults")) { g_lc = ets2dhm::Config(); ch = true; }
    ImGui::TextDisabled("%s", shared::ini_path().c_str());
    ImGui::SeparatorText("Overlay");
    char kb[64];
    if (g_capture == 0) {
        ImGui::Button("Press a key... (Esc = cancel)");
    } else {
        std::string lbl = std::string("Menu hotkey: ") + key_name(g_lu.hotkey, kb, sizeof(kb)) + "  (click to change)";
        if (ImGui::Button(lbl.c_str())) g_capture = 0;
    }
    float sc = g_lu.scale;
    if (ImGui::SliderFloat("UI scale (0 = auto)", &sc, 0.0f, 3.0f, "%.2f")) {
        g_lu.scale = sc < 0.3f ? 0.0f : sc;
        ch = true;
    }
    ch |= ImGui::SliderFloat("Window opacity", &g_lu.alpha, 0.3f, 1.0f, "%.2f");
    ImGui::Separator();
    ImGui::TextDisabled("UI: Dear ImGui (MIT)");
}


void tab_custom(bool& ch) {
    ImGui::TextWrapped("Hold or toggle a key to move the head to a fixed pose. Use a key the game does not use.");
    ch |= slider("Position gain (custom keys)", &g_lc.custom_gain, 0.1f, 10.0f, "%.2f", "Multiplies x/y/z of all custom poses. Independent from the auto movement gain.");
    ImGui::Spacing();
    for (int i = 0; i < ets2dhm::kNumCustom; i++) {
        ets2dhm::CustomPose& q = g_lc.custom[i];
        ImGui::PushID(i);
        char title[64];
        std::snprintf(title, sizeof title, "Function %d: %s###f%d", i + 1, q.name, i);
        if (ImGui::CollapsingHeader(title, i == 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            ch |= check("Enabled", &q.enabled, nullptr);
            ImGui::SetNextItemWidth(200.0f * ImGui::GetIO().FontGlobalScale);
            ch |= ImGui::InputText("Name", q.name, sizeof q.name);
            char kb[64];
            if (g_capture == i + 1) {
                ImGui::Button("Press a key... (Esc = cancel)");
            } else {
                std::string lbl = q.key ? std::string("Key: ") + key_name(q.key, kb, sizeof kb) : std::string("Key: none");
                lbl += "  (click to change)";
                if (ImGui::Button(lbl.c_str())) g_capture = i + 1;
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear key")) { q.key = 0; ch = true; }
            int mode = q.mode;
            if (ImGui::RadioButton("Hold key", mode == 0)) mode = 0;
            ImGui::SameLine();
            if (ImGui::RadioButton("Toggle on/off", mode == 1)) mode = 1;
            if (mode != q.mode) { q.mode = mode; ch = true; }
            ch |= slider("Move time (s)", &q.time, 0.05f, 3.0f, "%.2f", "How long the head takes to reach this pose and to come back.");
            ImGui::SeparatorText("Pose");
            ch |= slider("Look left / right (deg)", &q.yaw, -90.0f, 90.0f, "%.1f", "Negative = left, positive = right.");
            ch |= slider("Look down / up (deg)", &q.pitch, -60.0f, 60.0f, "%.1f", "Negative = down, positive = up.");
            ch |= slider("Tilt (deg)", &q.roll, -45.0f, 45.0f, "%.1f", "Head tilt toward the shoulder.");
            ch |= slider("Move left / right (cm)", &q.x, -100.0f, 100.0f, "%.1f", "Negative = left, positive = right. Use Flip X in Motion if reversed.");
            ch |= slider("Move down / up (cm)", &q.y, -100.0f, 100.0f, "%.1f", "Use Flip Y in Motion if reversed.");
            ch |= slider("Move back / forward (cm)", &q.z, -100.0f, 100.0f, "%.1f", "Use Flip Z in Motion if forward/back is reversed.");
            ImGui::TextUnformatted("Flip direction:");
            const char* fl[6] = {"Look L/R", "Look U/D", "Tilt", "Move L/R", "Move U/D", "Move B/F"};
            for (int b = 0; b < 6; b++) {
                bool on = (q.flips >> b) & 1;
                ImGui::PushID(b);
                if (ImGui::Checkbox(fl[b], &on)) {
                    q.flips = on ? (q.flips | (1 << b)) : (q.flips & ~(1 << b));
                    ch = true;
                }
                ImGui::PopID();
                if (b == 2) continue;
                if (b < 5) ImGui::SameLine();
            }
            ImGui::Button("Hold to preview");
            g_preview[i] = ImGui::IsItemActive();
        }
        ImGui::PopID();
    }
}


void draw_ui(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::GetStyle().Colors[ImGuiCol_WindowBg].w = std::clamp(g_lu.alpha, 0.3f, 1.0f);
    io.FontGlobalScale = scale;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    bool ch = false;
    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 6));
    ImGui::Begin("##main", nullptr, wf);
    ImGui::SetCursorPosY(4.0f * scale);
    ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1), "ETS2DynamicHeadMovement");
    ImGui::SameLine();
    ImGui::TextDisabled("(drag this bar to move)");
    ImGui::SameLine(ImGui::GetWindowWidth() - 70.0f * scale);
    if (ImGui::SmallButton("Close")) set_visible(false);
    ImGui::SetCursorPosY(kCaption * scale);
    ImGui::PushItemWidth(ImGui::GetWindowWidth() * 0.5f);
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Status")) { tab_status(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Motion")) { tab_motion(ch); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Glance")) { tab_glance(ch); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Steering")) { tab_steer(ch); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Dynamics")) { tab_dyn(ch); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Custom keys")) { tab_custom(ch); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Settings")) { tab_settings(ch); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::PopItemWidth();
    ImGui::Separator();
    char kb[64];
    ImGui::TextDisabled("%s toggles this window", key_name(g_lu.hotkey, kb, sizeof(kb)));
    if (!g_status_msg.empty() && Clock::now() - g_status_time < std::chrono::seconds(3)) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.5f, 1, 0.6f, 1), "  %s", g_status_msg.c_str());
    }
    ImGui::End();
    ImGui::PopStyleVar();
    if (ch) push_cfg();
    if (g_dirty && !ImGui::IsAnyItemActive() && Clock::now() - g_dirty_time > std::chrono::milliseconds(1500)) flush_save();
}

void draw_toast(float scale, const shared::UiSettings& u) {
    ImGuiIO& io = ImGui::GetIO();
    io.FontGlobalScale = scale;
    const float age = std::chrono::duration<float>(Clock::now() - g_toast_start).count();
    const float a = std::clamp(7.0f - age, 0.0f, 1.0f);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::SetNextWindowBgAlpha(0.85f * a);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::max(a, 0.01f));
    ImGui::Begin("##toast", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);
    char kb[64];
    ImGui::Text("ETS2DynamicHeadMovement loaded - press %s to open settings", key_name(u.hotkey, kb, sizeof(kb)));
    ImGui::End();
    ImGui::PopStyleVar();
}

void poll_keys() {
    const bool fg = process_foreground();
    if (!fg) {
        g_prev_key = false;
        for (int i = 0; i < ets2dhm::kNumCustom; i++) g_prev_custom[i] = false;
    }

    shared::UiSettings u;
    ets2dhm::Config c;
    {
        std::lock_guard<std::mutex> lk(shared::mu);
        u = shared::ui;
        c = shared::cfg;
    }

    if (g_capture >= 0 && g_visible.load()) {
        for (int vk = 8; vk < 255; vk++) {
            if (vk >= 1 && vk <= 6) continue;
            if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) continue;
            if (GetAsyncKeyState(vk) & 0x8000) {
                if (vk != VK_ESCAPE) {
                    if (g_capture == 0) g_lu.hotkey = vk;
                    else g_lc.custom[g_capture - 1].key = vk;
                    push_cfg();
                }
                g_capture = -1;
                g_prev_key = true;
                break;
            }
        }
        return;
    }

    if (fg) {
        const bool alt = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
                         (GetAsyncKeyState('H') & 0x8000);
        const bool down = (GetAsyncKeyState(u.hotkey) & 0x8000) != 0 || alt;
        if (down && !g_prev_key) {
            set_visible(!g_visible.load());
            dlog(g_visible.load() ? "overlay: opened" : "overlay: closed");
        }
        g_prev_key = down;
    }

    const bool typing = g_visible.load() && ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput;
    for (int i = 0; i < ets2dhm::kNumCustom; i++) {
        const ets2dhm::CustomPose& q = c.custom[i];
        bool active = false;
        if (fg && !typing && q.enabled && q.key > 0) {
            const bool down = (GetAsyncKeyState(q.key) & 0x8000) != 0;
            if (q.mode == 0) {
                active = down;
            } else {
                if (down && !g_prev_custom[i]) g_toggle_state[i] = !g_toggle_state[i];
                active = g_toggle_state[i];
            }
            g_prev_custom[i] = down;
        } else {
            g_prev_custom[i] = false;
            if (!q.enabled || q.key <= 0) g_toggle_state[i] = false;
        }
        if (g_visible.load() && g_preview[i]) active = true;
        shared::live.custom_active[i].store(active);
    }
}

void render_frame() {
    RECT gr{};
    const bool have_game = game_rect(&gr);
    const bool vis = g_visible.load();
    const float scale = vis ? cur_scale() : cur_scale();
    int w, h, x, y;
    if (vis) {
        w = int(kBaseW * scale);
        h = int(kBaseH * scale);
        if (have_game) h = std::min(h, std::max(200, int(gr.bottom - gr.top) - 20));
        if (g_first_vis_frame) {
            g_first_vis_frame = false;
        } else {
            RECT wr{};
            GetWindowRect(g_hwnd, &wr);
            g_win_x = wr.left;
            g_win_y = wr.top;
        }
        x = g_win_x;
        y = g_win_y;
    } else {
        w = int(kToastW * scale);
        h = int(kToastH * scale);
        x = have_game ? gr.left + 20 : 20;
        y = have_game ? gr.top + 20 : 20;
    }

    ImGuiIO& io = ImGui::GetIO();
    ensure_surface(w, h);
    if (!g_pixels) return;
    std::memset(g_pixels, 0, size_t(w) * size_t(h) * 4);

    SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);

    for (int i = 0; i < 3; i++) g_hist[i][g_hist_pos] = shared::live.out[i].load();
    g_hist_pos = (g_hist_pos + 1) % kHist;

    ImGui_ImplWin32_NewFrame();
    io.DisplaySize = ImVec2(float(w), float(h));
    ImGui::NewFrame();
    if (vis) {
        if (!g_local_valid) load_local();
        draw_ui(scale);
    } else {
        shared::UiSettings u;
        {
            std::lock_guard<std::mutex> lk(shared::mu);
            u = shared::ui;
        }
        draw_toast(scale, u);
    }
    ImGui::Render();
    raster_draw_data(ImGui::GetDrawData());
    present_surface(x, y, w, h);
}

DWORD WINAPI ui_thread(LPVOID) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = g_module;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = L"ETS2DynamicHeadMovementOverlay";
    RegisterClassExW(&wc);
    g_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                             wc.lpszClassName, L"ETS2DynamicHeadMovement", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr,
                             g_module, nullptr);
    if (!g_hwnd) {
        dlog("overlay: failed to create window");
        return 0;
    }
    HDC screen = GetDC(nullptr);
    g_memdc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    ImFontConfig fc;
    fc.SizePixels = 16.0f;
    io.Fonts->AddFontDefault(&fc);
    unsigned char* pix = nullptr;
    io.Fonts->GetTexDataAsAlpha8(&pix, &g_font_w, &g_font_h);
    g_font_alpha = pix;
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 6.0f;
    st.FrameRounding = 4.0f;
    st.GrabRounding = 4.0f;
    st.TabRounding = 4.0f;
    st.WindowBorderSize = 1.0f;
    st.AntiAliasedLines = false;
    st.AntiAliasedFill = true;
    st.Colors[ImGuiCol_Header] = ImVec4(0.20f, 0.45f, 0.75f, 0.45f);
    st.Colors[ImGuiCol_SliderGrab] = ImVec4(0.30f, 0.62f, 0.95f, 1.0f);
    st.Colors[ImGuiCol_CheckMark] = ImVec4(0.30f, 0.62f, 0.95f, 1.0f);
    if (!ImGui_ImplWin32_Init(g_hwnd)) {
        dlog("overlay: ImGui init failed");
        return 0;
    }
    g_imgui_ready = true;
    hook_cursor_calls();

    load_local();
    for (int i = 0; i < 600 && g_running.load() && !g_game; i++) {
        g_game = find_game_window();
        if (!g_game) Sleep(500);
    }
    dlog(g_game ? "overlay: game window found" : "overlay: game window not found yet");
    g_toast_start = Clock::now();
    g_toast_started = true;

    auto last_render = Clock::now() - std::chrono::seconds(1);
    bool logged_draw = false;
    while (g_running.load()) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        poll_keys();
        const bool vis = g_visible.load();
        const bool toast = g_toast_started &&
                           std::chrono::duration<float>(Clock::now() - g_toast_start).count() < 7.0f;
        const auto now = Clock::now();
        if ((vis || toast) && now - last_render >= std::chrono::milliseconds(vis ? 25 : 50)) {
            last_render = now;
            render_frame();
            if (!logged_draw) {
                logged_draw = true;
                dlog("overlay: first draw");
            }
        } else if (!vis && !toast && IsWindowVisible(g_hwnd)) {
            ShowWindow(g_hwnd, SW_HIDE);
        }
        Sleep(8);
    }

    shared::live.custom_active[0].store(false);
    shared::live.custom_active[1].store(false);
    shared::live.custom_active[2].store(false);
    unhook_cursor_calls();
    flush_save();
    g_imgui_ready = false;
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
    UnregisterClassW(wc.lpszClassName, g_module);
    if (g_bmp) DeleteObject(g_bmp);
    if (g_memdc) DeleteDC(g_memdc);
    return 0;
}

}  // namespace

namespace overlay {

void start(void* module) {
    g_module = static_cast<HMODULE>(module);
    g_running.store(true);
    g_thread = CreateThread(nullptr, 0, ui_thread, nullptr, 0, nullptr);
}

void stop() {
    g_running.store(false);
    if (g_thread) {
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
}

}  // namespace overlay
