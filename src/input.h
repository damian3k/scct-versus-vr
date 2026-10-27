#pragma once
#include <windows.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#include <d3d12.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "xrapi.h"

namespace input {

struct Controls {
    float lookX = 0, lookY = 0;        // right thumbstick
    float moveX = 0, moveY = 0;        // left thumbstick
    bool aimValid = false;
    XrPosef aim;                       // right controller aim pose in LOCAL space
    bool leftValid = false;
    XrPosef left;                      // left controller (grip) pose in LOCAL space
    bool toggleAim = false;            // edge-triggered pseudo actions
    bool recenter = false;
    bool menuPressed = false;          // Menu button tapped (its key was sent) this update
    bool bPressed = false;
    bool menuReset = false;            // Menu button held: back to game controls, no key sent
    bool fpvToggle = false;            // both stick clicks pressed together (no key sent)
};

void SetGameWindow(HWND wnd);
bool Init(XrInstance instance, XrSession session);

// Reads the controllers, emulates the bound keys/mouse buttons and returns analog state.
// `focused`: the XR session is focused and the game window is in front.
// `rightStickButtons`: right stick up/down act as bound buttons (otherwise the stick is analog look).
// `menuMode`: a menu is on screen; [MenuBindings] (arrows, Enter, Esc) override the game bindings.
// `spyMode`: gameplay without controller aiming; [SpyBindings] override the game bindings.
void Update(XrSession session, XrSpace localSpace, XrTime time, bool focused, bool rightStickButtons, bool menuMode,
            bool spyMode, Controls* out);

// WinlatorXR's XR API: the controllers come in its tracking packet instead of OpenXR actions.
// `controllerRoll`: degrees the packet's controller orientation is rolled against an OpenXR aim pose.
void InitXrApi();
// Makes GetKeyState / GetAsyncKeyState report keys held through window messages (XR API).
void InstallKeyStateHooks();
void UpdateXrApi(const xrapi::Packet& p, bool focused, bool rightStickButtons, bool menuMode, bool spyMode,
                 float controllerRoll, Controls* out);

bool GameHasFocus();
void SendMouseMove(int dx, int dy);
// Taps the [Bindings] ScopeKey (the game's zoom toggle, B by default).
void TapScopeKey();
// Taps the [Bindings] ReloadKey (R by default).
void TapReloadKey();
void ReleaseAll();

} // namespace input
