ETS2DynamicHeadMovement - source

Files
  plugin.cpp        SCS telemetry plugin, FT_SharedMem output, ini load/save
  overlay.cpp       settings window (Dear ImGui, software rendered layered window), hotkeys
  shared.hpp        state shared between plugin and overlay
  ets2dhm_core.hpp  head motion model (portable, no Windows dependency)

Build guide (64-bit Windows DLL, mingw-w64: Linux or MSYS2 with x86_64-w64-mingw32-g++)
  1. Download Dear ImGui v1.91.9 and keep these files in one folder (IMGUI):
       imgui.cpp, imgui_draw.cpp, imgui_tables.cpp, imgui_widgets.cpp, imgui_impl_win32.cpp
       plus their headers (imgui.h, imgui_internal.h, imconfig.h, imstb_*.h, imgui_impl_win32.h)
  2. Download the SCS Telemetry SDK and keep scssdk_telemetry.h and the common/ folder in one folder (SDK).
  3. Put the four source files from this archive in the current folder, then run:

     x86_64-w64-mingw32-g++ -std=c++17 -O2 -shared -static -s \
       -DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD \
       -I SDK -I IMGUI -I . \
       -o ETS2DynamicHeadMovement.dll \
       plugin.cpp overlay.cpp \
       IMGUI/imgui.cpp IMGUI/imgui_draw.cpp IMGUI/imgui_tables.cpp IMGUI/imgui_widgets.cpp \
       IMGUI/imgui_impl_win32.cpp \
       -lwinmm -ldwmapi -lgdi32 -limm32

     Replace SDK and IMGUI with the real folder paths.
  4. Output: ETS2DynamicHeadMovement.dll (64-bit).

Install
  Copy the DLL to <ETS2>\bin\win_x64\plugins\
  The settings file ETS2DynamicHeadMovement.ini is created next to the DLL on first run.
