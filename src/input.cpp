// OpenXR controller input, emulated as keyboard/mouse with SendInput so that the game and
// Reloaded see ordinary DirectInput/raw input events (works with the game's own key bindings).

#include "input.h"
#include "log.h"
#include "rawinject.h"
#include "dihook.h"
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <initializer_list>
#include <utility>

namespace input {

namespace {

enum class Kind { None, Key, Mouse, Wheel, AimToggle, Recenter, MenuReset };

struct Bind {
    Kind kind = Kind::None;
    WORD scan = 0;
    bool extended = false;
    int button = 0;   // 1 left, 2 right, 3 middle
    int wheel = 0;
};

enum Ctl {
    kLeftTrigger, kRightTrigger, kLeftGrip, kRightGrip, kA, kB, kX, kY, kLeftClick, kRightClick, kMenu,
    kLeftUp, kLeftDown, kLeftLeft, kLeftRight, kRightUp, kRightDown, kCtlCount
};

const wchar_t* const kCtlNames[kCtlCount] = {
    L"LeftTrigger", L"RightTrigger", L"LeftGrip", L"RightGrip", L"A", L"B", L"X", L"Y",
    L"LeftStickClick", L"RightStickClick", L"Menu",
    L"LeftStickUp", L"LeftStickDown", L"LeftStickLeft", L"LeftStickRight", L"RightStickUp", L"RightStickDown",
};

// Defaults follow the game's stock keyboard layout (Default_kbd_0.ini).
const wchar_t* const kDefaults[kCtlCount] = {
    L"RMB",   // left trigger: AltFire / gadget
    L"LMB",   // right trigger: Fire
    L"Q",     // left grip: Roll / laser
    L"Shift", // right grip: Use
    L"Space", // A: Jump
    L"C",     // B: Crouch
    L"R",     // X: Reload
    L"Tab",   // Y: vision modes
    L"MMB",   // left stick click: walk/run
    L"E",     // right stick click: fire mode / torch
    L"Esc",   // menu
    L"W", L"S", L"A", L"D",
    L"", L"",
};

// Menus: the left stick acts as arrow keys, A confirms, B goes back. Controls left empty here
// keep their game binding.
const wchar_t* const kMenuDefaults[kCtlCount] = {
    L"", L"", L"", L"", L"Enter", L"Esc", L"", L"", L"", L"", L"",
    L"Up", L"Down", L"Left", L"Right",
    L"", L"",
};

// Holding a control for HoldTimeMs sends its "<Name>Hold" binding instead of the normal one.
// Y: tap = vision modes, hold = co-op move (T in the stock keyboard layout).
const wchar_t* const kHoldDefaults[kCtlCount] = {
    L"", L"", L"", L"", L"", L"", L"", L"T", L"", L"", L"",
    L"", L"", L"", L"",
    L"", L"",
};

// Spy (no controller aiming): the right stick is the camera, so gadgets move to X:
// tap = next gadget, hold = previous gadget. Controls left empty keep their game binding.
const wchar_t* const kSpyDefaults[kCtlCount] = {
    L"", L"", L"", L"", L"", L"", L"WheelDown", L"", L"", L"", L"",
    L"", L"", L"", L"",
    L"", L"",
};
const wchar_t* const kSpyHoldDefaults[kCtlCount] = {
    L"", L"", L"", L"", L"", L"", L"WheelUp", L"", L"", L"", L"",
    L"", L"", L"", L"",
    L"", L"",
};

enum Context { kCtxGame, kCtxSpy, kCtxMenu };
const DWORD kHoldTimeMs = 350;
const DWORD kTapLengthMs = 60;   // a tap is held this long so polled (DirectInput) state sees it

HWND g_wnd;
Bind g_binds[kCtlCount], g_holdBinds[kCtlCount];
Bind g_spyBinds[kCtlCount], g_spyHoldBinds[kCtlCount];
Bind g_menuBinds[kCtlCount];
Bind g_active[kCtlCount];      // binding a held control was pressed with
bool g_held[kCtlCount];
bool g_pending[kCtlCount];     // pressed, waiting to see whether it becomes a hold
bool g_ignore[kCtlCount];      // still physically held after a context switch: ignore until released
Bind g_tapUp[kCtlCount];       // tap waiting for its key-up
DWORD g_tapUpTick[kCtlCount];
int g_context = kCtxGame;
DWORD g_pressTick[kCtlCount], g_repeatTick[kCtlCount];
bool g_logSends;                // XR API: log the first injected inputs (diagnostics)
Bind g_scopeKey;
bool g_scopeUpPending;
DWORD g_scopeUpTick;
const DWORD kChordMs = 120;     // both stick clicks within this time = chord
bool g_clickWait[2];            // left / right stick click held back, waiting for the other
DWORD g_clickTick[2];
bool g_chordHeld;               // chord done, ignore both clicks until both are released

XrActionSet g_set = XR_NULL_HANDLE;
XrAction g_move, g_look, g_trigL, g_trigR, g_gripL, g_gripR, g_a, g_b, g_x, g_y, g_clickL, g_clickR, g_menu, g_aim;
XrSpace g_aimSpace = XR_NULL_HANDLE;
XrPath g_left, g_right;
XrInstance g_instance;
bool g_ok;

const wchar_t* IniPath()
{
    static wchar_t path[MAX_PATH];
    if (!path[0]) swprintf_s(path, L"%sSCCT_VR.ini", ModuleDir());
    return path;
}

struct NamedKey { const wchar_t* name; UINT vk; bool extended; };
const NamedKey kNamedKeys[] = {
    { L"Space", VK_SPACE, false }, { L"Shift", VK_LSHIFT, false }, { L"Ctrl", VK_LCONTROL, false },
    { L"Alt", VK_LMENU, false }, { L"Tab", VK_TAB, false }, { L"Esc", VK_ESCAPE, false },
    { L"Enter", VK_RETURN, false }, { L"Backspace", VK_BACK, false },
    { L"Home", VK_HOME, true }, { L"End", VK_END, true }, { L"PgUp", VK_PRIOR, true }, { L"PgDn", VK_NEXT, true },
    { L"Insert", VK_INSERT, true }, { L"Delete", VK_DELETE, true },
    { L"Up", VK_UP, true }, { L"Down", VK_DOWN, true }, { L"Left", VK_LEFT, true }, { L"Right", VK_RIGHT, true },
};

Bind Parse(const wchar_t* s)
{
    Bind b;
    if (!s[0]) return b;
    if (!_wcsicmp(s, L"LMB")) { b.kind = Kind::Mouse; b.button = 1; return b; }
    if (!_wcsicmp(s, L"RMB")) { b.kind = Kind::Mouse; b.button = 2; return b; }
    if (!_wcsicmp(s, L"MMB")) { b.kind = Kind::Mouse; b.button = 3; return b; }
    if (!_wcsicmp(s, L"WheelUp")) { b.kind = Kind::Wheel; b.wheel = WHEEL_DELTA; return b; }
    if (!_wcsicmp(s, L"WheelDown")) { b.kind = Kind::Wheel; b.wheel = -WHEEL_DELTA; return b; }
    if (!_wcsicmp(s, L"AimToggle")) { b.kind = Kind::AimToggle; return b; }
    if (!_wcsicmp(s, L"Recenter")) { b.kind = Kind::Recenter; return b; }
    if (!_wcsicmp(s, L"MenuReset")) { b.kind = Kind::MenuReset; return b; }

    UINT vk = 0;
    bool ext = false;
    if (!s[1] && iswalnum(s[0])) vk = towupper(s[0]);
    else if ((s[0] == L'F' || s[0] == L'f') && iswdigit(s[1])) vk = VK_F1 + _wtoi(s + 1) - 1;
    else if (!_wcsnicmp(s, L"0x", 2)) vk = wcstoul(s, nullptr, 16);
    else
        for (auto& k : kNamedKeys)
            if (!_wcsicmp(s, k.name)) { vk = k.vk; ext = k.extended; }
    if (!vk) { Log("Unknown key name in [Bindings]: %ls", s); return b; }
    b.kind = Kind::Key;
    b.scan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    b.extended = ext;
    return b;
}

void LoadBindings()
{
    for (int i = 0; i < kCtlCount; i++) {
        wchar_t buf[64];
        GetPrivateProfileStringW(L"Bindings", kCtlNames[i], kDefaults[i], buf, 64, IniPath());
        g_binds[i] = Parse(buf);
        GetPrivateProfileStringW(L"MenuBindings", kCtlNames[i], kMenuDefaults[i], buf, 64, IniPath());
        g_menuBinds[i] = Parse(buf);
        wchar_t holdName[64];
        swprintf_s(holdName, L"%sHold", kCtlNames[i]);
        GetPrivateProfileStringW(L"Bindings", holdName, kHoldDefaults[i], buf, 64, IniPath());
        g_holdBinds[i] = Parse(buf);
        GetPrivateProfileStringW(L"SpyBindings", kCtlNames[i], kSpyDefaults[i], buf, 64, IniPath());
        g_spyBinds[i] = Parse(buf);
        GetPrivateProfileStringW(L"SpyBindings", holdName, kSpyHoldDefaults[i], buf, 64, IniPath());
        g_spyHoldBinds[i] = Parse(buf);
    }
    wchar_t buf[64];
    GetPrivateProfileStringW(L"Bindings", L"ScopeKey", L"B", buf, 64, IniPath());
    g_scopeKey = Parse(buf);
}

// Keys held through window messages (XR API): the game also checks GetKeyState every frame and
// lets go of keys that are not really down, so these report as down there.
BYTE g_vkHeld[256];
using GetKeyState_t = SHORT(WINAPI*)(int);
GetKeyState_t oGetKeyState, oGetAsyncKeyState;

SHORT WINAPI hkGetKeyState(int vk)
{
    SHORT r = oGetKeyState(vk);
    if (vk >= 0 && vk < 256 && g_vkHeld[vk]) r |= (SHORT)0x8000;
    return r;
}

SHORT WINAPI hkGetAsyncKeyState(int vk)
{
    SHORT r = oGetAsyncKeyState(vk);
    if (vk >= 0 && vk < 256 && g_vkHeld[vk]) r |= (SHORT)0x8000;
    return r;
}

void SetHeld(UINT vk, bool down)
{
    if (vk >= 256) return;
    g_vkHeld[vk] = down;
    // The generic modifier codes follow their left/right variants.
    if (vk == VK_LSHIFT || vk == VK_RSHIFT) g_vkHeld[VK_SHIFT] = g_vkHeld[VK_LSHIFT] | g_vkHeld[VK_RSHIFT];
    if (vk == VK_LCONTROL || vk == VK_RCONTROL) g_vkHeld[VK_CONTROL] = g_vkHeld[VK_LCONTROL] | g_vkHeld[VK_RCONTROL];
    if (vk == VK_LMENU || vk == VK_RMENU) g_vkHeld[VK_MENU] = g_vkHeld[VK_LMENU] | g_vkHeld[VK_RMENU];
}

// Under Wine (WinlatorXR) injected input does not reach the game: keys go to its window as
// messages, the mouse to its DirectInput mouse (dihook.cpp) or Reloaded's raw input.
bool SendRaw(const Bind& b, bool down)
{
    if (!rawinject::Active()) return false;
    static const USHORT downFlags[] = { 0, RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_DOWN };
    static const USHORT upFlags[] = { 0, RI_MOUSE_LEFT_BUTTON_UP, RI_MOUSE_RIGHT_BUTTON_UP, RI_MOUSE_MIDDLE_BUTTON_UP };
    switch (b.kind) {
    case Kind::Key: {
        // The game reads the keyboard from its window messages (Reloaded reads only the mouse
        // with raw input, and the game's DirectInput devices are mouse and gamepads), so the key
        // goes straight to the game window as WM_KEYDOWN / WM_KEYUP.
        UINT vk = MapVirtualKeyW(b.scan | (b.extended ? 0xE000 : 0), MAPVK_VSC_TO_VK_EX);
        bool sys = vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU || vk == VK_F10;
        LPARAM lp = 1 | ((LPARAM)b.scan << 16) | (b.extended ? (1 << 24) : 0) | (down ? 0 : (3u << 30));
        UINT msg = down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN) : (sys ? WM_SYSKEYUP : WM_KEYUP);
        SetHeld(vk, down);
        // Key messages carry the generic modifier codes (as Windows sends them).
        UINT msgVk = vk;
        if (vk == VK_LSHIFT || vk == VK_RSHIFT) msgVk = VK_SHIFT;
        else if (vk == VK_LCONTROL || vk == VK_RCONTROL) msgVk = VK_CONTROL;
        else if (vk == VK_LMENU || vk == VK_RMENU) msgVk = VK_MENU;
        if (g_wnd) PostMessageW(g_wnd, msg, msgVk, lp);
        break;
    }
    case Kind::Mouse:
        if (dihook::MouseActive()) dihook::MouseButton(b.button - 1, down);
        else rawinject::Mouse(0, 0, down ? downFlags[b.button] : upFlags[b.button], 0);
        break;
    case Kind::Wheel:
        if (!down) break;
        if (dihook::MouseActive()) dihook::MouseMove(0, 0, b.wheel);
        else rawinject::Mouse(0, 0, 0, (short)b.wheel);
        break;
    default: return true;
    }
    static unsigned logged;
    if (g_logSends && logged < 30) {
        logged++;
        Log("Raw input %s %s (scan 0x%X button %d wheel %d)", b.kind == Kind::Key ? "key" : "mouse",
            down ? "down" : "up", b.scan, b.button, b.wheel);
    }
    return true;
}

void SendKey(const Bind& b, bool down)
{
    if (SendRaw(b, down)) return;
    INPUT in = {};
    switch (b.kind) {
    case Kind::Key:
        in.type = INPUT_KEYBOARD;
        in.ki.wScan = b.scan;
        in.ki.dwFlags = KEYEVENTF_SCANCODE | (b.extended ? KEYEVENTF_EXTENDEDKEY : 0) | (down ? 0 : KEYEVENTF_KEYUP);
        break;
    case Kind::Mouse: {
        static const DWORD downFlags[] = { 0, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_MIDDLEDOWN };
        static const DWORD upFlags[] = { 0, MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_MIDDLEUP };
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = down ? downFlags[b.button] : upFlags[b.button];
        break;
    }
    case Kind::Wheel:
        if (!down) return;
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_WHEEL;
        in.mi.mouseData = (DWORD)b.wheel;
        break;
    default:
        return;
    }
    UINT sent = SendInput(1, &in, sizeof(in));
    static unsigned logged;
    if (g_logSends && logged < 30) {
        logged++;
        Log("SendInput %s %s (scan 0x%X button %d wheel %d) -> %u%s", in.type == INPUT_KEYBOARD ? "key" : "mouse",
            down ? "down" : "up", b.scan, b.button, b.wheel, sent, sent ? "" : " FAILED");
    }
}

XrPath Path(const char* s)
{
    XrPath p = XR_NULL_PATH;
    xrStringToPath(g_instance, s, &p);
    return p;
}

bool MakeAction(XrAction* a, XrActionType type, const char* name, const char* label, XrPath sub = XR_NULL_PATH)
{
    XrActionCreateInfo ci{ XR_TYPE_ACTION_CREATE_INFO };
    ci.actionType = type;
    strcpy_s(ci.actionName, name);
    strcpy_s(ci.localizedActionName, label);
    if (sub != XR_NULL_PATH) {
        ci.countSubactionPaths = 1;
        ci.subactionPaths = &sub;
    }
    XrResult r = xrCreateAction(g_set, &ci, a);
    if (XR_FAILED(r)) Log("xrCreateAction(%s) failed %d", name, (int)r);
    return XR_SUCCEEDED(r);
}

void Suggest(const char* profile, std::initializer_list<std::pair<XrAction, const char*>> list)
{
    XrActionSuggestedBinding b[32];
    uint32_t n = 0;
    for (auto& e : list) b[n++] = { e.first, Path(e.second) };
    XrInteractionProfileSuggestedBinding s{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    s.interactionProfile = Path(profile);
    s.countSuggestedBindings = n;
    s.suggestedBindings = b;
    XrResult r = xrSuggestInteractionProfileBindings(g_instance, &s);
    if (XR_FAILED(r)) Log("Bindings for %s rejected (%d)", profile, (int)r);
}

bool GetBool(XrSession s, XrAction a)
{
    XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = a;
    XrActionStateBoolean st{ XR_TYPE_ACTION_STATE_BOOLEAN };
    return XR_SUCCEEDED(xrGetActionStateBoolean(s, &gi, &st)) && st.isActive && st.currentState;
}

float GetFloat(XrSession s, XrAction a)
{
    XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = a;
    XrActionStateFloat st{ XR_TYPE_ACTION_STATE_FLOAT };
    return XR_SUCCEEDED(xrGetActionStateFloat(s, &gi, &st)) && st.isActive ? st.currentState : 0.0f;
}

XrVector2f GetVec2(XrSession s, XrAction a)
{
    XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = a;
    XrActionStateVector2f st{ XR_TYPE_ACTION_STATE_VECTOR2F };
    if (XR_SUCCEEDED(xrGetActionStateVector2f(s, &gi, &st)) && st.isActive) return st.currentState;
    return { 0, 0 };
}

// Analog -> digital with hysteresis
bool Threshold(float v, bool wasDown, float on = 0.6f, float off = 0.4f)
{
    return wasDown ? v > off : v > on;
}

float Deadzone(float v, float dz)
{
    if (fabsf(v) < dz) return 0.0f;
    return (v - (v > 0 ? dz : -dz)) / (1.0f - dz);
}

} // namespace

void SetGameWindow(HWND wnd) { g_wnd = wnd; }

bool GameHasFocus()
{
    HWND fg = GetForegroundWindow();
    if (!g_wnd || !fg) return false;
    return GetAncestor(fg, GA_ROOT) == GetAncestor(g_wnd, GA_ROOT);
}

bool Init(XrInstance instance, XrSession session)
{
    g_instance = instance;
    LoadBindings();

    XrActionSetCreateInfo sci{ XR_TYPE_ACTION_SET_CREATE_INFO };
    strcpy_s(sci.actionSetName, "gameplay");
    strcpy_s(sci.localizedActionSetName, "Gameplay");
    if (XR_FAILED(xrCreateActionSet(instance, &sci, &g_set))) { Log("xrCreateActionSet failed"); return false; }

    g_left = Path("/user/hand/left");
    g_right = Path("/user/hand/right");
    bool ok = MakeAction(&g_move, XR_ACTION_TYPE_VECTOR2F_INPUT, "move", "Move") &&
              MakeAction(&g_look, XR_ACTION_TYPE_VECTOR2F_INPUT, "look", "Turn / look") &&
              MakeAction(&g_trigL, XR_ACTION_TYPE_FLOAT_INPUT, "trigger_left", "Left trigger") &&
              MakeAction(&g_trigR, XR_ACTION_TYPE_FLOAT_INPUT, "trigger_right", "Right trigger") &&
              MakeAction(&g_gripL, XR_ACTION_TYPE_FLOAT_INPUT, "grip_left", "Left grip") &&
              MakeAction(&g_gripR, XR_ACTION_TYPE_FLOAT_INPUT, "grip_right", "Right grip") &&
              MakeAction(&g_a, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_a", "A") &&
              MakeAction(&g_b, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_b", "B") &&
              MakeAction(&g_x, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_x", "X") &&
              MakeAction(&g_y, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_y", "Y") &&
              MakeAction(&g_clickL, XR_ACTION_TYPE_BOOLEAN_INPUT, "stick_click_left", "Left stick click") &&
              MakeAction(&g_clickR, XR_ACTION_TYPE_BOOLEAN_INPUT, "stick_click_right", "Right stick click") &&
              MakeAction(&g_menu, XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Menu") &&
              MakeAction(&g_aim, XR_ACTION_TYPE_POSE_INPUT, "aim", "Weapon aim", g_right);
    if (!ok) return false;

    Suggest("/interaction_profiles/oculus/touch_controller", {
        { g_move, "/user/hand/left/input/thumbstick" },
        { g_look, "/user/hand/right/input/thumbstick" },
        { g_trigL, "/user/hand/left/input/trigger/value" },
        { g_trigR, "/user/hand/right/input/trigger/value" },
        { g_gripL, "/user/hand/left/input/squeeze/value" },
        { g_gripR, "/user/hand/right/input/squeeze/value" },
        { g_a, "/user/hand/right/input/a/click" },
        { g_b, "/user/hand/right/input/b/click" },
        { g_x, "/user/hand/left/input/x/click" },
        { g_y, "/user/hand/left/input/y/click" },
        { g_clickL, "/user/hand/left/input/thumbstick/click" },
        { g_clickR, "/user/hand/right/input/thumbstick/click" },
        { g_menu, "/user/hand/left/input/menu/click" },
        { g_aim, "/user/hand/right/input/aim/pose" },
    });
    Suggest("/interaction_profiles/khr/simple_controller", {
        { g_trigR, "/user/hand/right/input/select/click" },
        { g_menu, "/user/hand/left/input/menu/click" },
        { g_aim, "/user/hand/right/input/aim/pose" },
    });

    XrSessionActionSetsAttachInfo ai{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    ai.countActionSets = 1;
    ai.actionSets = &g_set;
    if (XR_FAILED(xrAttachSessionActionSets(session, &ai))) { Log("xrAttachSessionActionSets failed"); return false; }

    XrActionSpaceCreateInfo asi{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
    asi.action = g_aim;
    asi.subactionPath = g_right;
    asi.poseInActionSpace.orientation.w = 1.0f;
    if (XR_FAILED(xrCreateActionSpace(session, &asi, &g_aimSpace))) { Log("xrCreateActionSpace failed"); return false; }

    Log("Controller input ready");
    g_ok = true;
    return true;
}

void ReleaseAll()
{
    for (int i = 0; i < kCtlCount; i++) {
        if (g_held[i] && !g_pending[i]) SendKey(g_active[i], false);
        if (g_tapUpTick[i]) SendKey(g_tapUp[i], false);
        g_held[i] = g_pending[i] = g_ignore[i] = false;
        g_tapUpTick[i] = 0;
    }
    if (g_scopeUpPending) SendKey(g_scopeKey, false);
    g_scopeUpPending = false;
}

void TapScopeKey()
{
    if (g_scopeKey.kind == Kind::None) return;
    if (g_scopeUpPending) SendKey(g_scopeKey, false);
    SendKey(g_scopeKey, true);
    g_scopeUpPending = true;
    g_scopeUpTick = GetTickCount() + kTapLengthMs;
}

void SendMouseMove(int dx, int dy)
{
    if (!dx && !dy) return;
    if (rawinject::Active()) {
        // The game turns its camera with its DirectInput mouse; Reloaded's raw input otherwise.
        if (dihook::MouseActive()) dihook::MouseMove(dx, dy, 0);
        else rawinject::Mouse(dx, dy, 0, 0);
        return;
    }
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(in));
}

// Normal and hold binding of a control in the given context (spy and menu fall back to the game set).
void Resolve(int i, int context, Bind* tap, Bind* hold)
{
    *tap = g_binds[i];
    *hold = g_holdBinds[i];
    if (context == kCtxSpy && (g_spyBinds[i].kind != Kind::None || g_spyHoldBinds[i].kind != Kind::None)) {
        *tap = g_spyBinds[i];
        *hold = g_spyHoldBinds[i];
    } else if (context == kCtxMenu && g_menuBinds[i].kind != Kind::None) {
        *tap = g_menuBinds[i];
        *hold = Bind();
    }
}

void Fire(const Bind& b, bool down, Controls* out)
{
    if (b.kind == Kind::AimToggle) { if (down) out->toggleAim = true; }
    else if (b.kind == Kind::Recenter) { if (down) out->recenter = true; }
    else if (b.kind == Kind::MenuReset) { if (down) out->menuReset = true; }
    else SendKey(b, down);
}

namespace {

// Controller state as read from OpenXR or from WinlatorXR's XR API.
struct Raw {
    XrVector2f move = {}, look = {};
    float trigL = 0, trigR = 0, gripL = 0, gripR = 0;
    bool a = false, b = false, x = false, y = false, clickL = false, clickR = false, menu = false;
    bool aimValid = false;
    XrPosef aim = {};
};

void Process(const Raw& r, bool rightStickButtons, bool menuMode, bool spyMode, Controls* out);

// Tap of the zoom key still waiting for its key-up.
void ScopeKeyUp()
{
    if (g_scopeUpPending && GetTickCount() >= g_scopeUpTick) {
        SendKey(g_scopeKey, false);
        g_scopeUpPending = false;
    }
}

} // namespace

void Update(XrSession session, XrSpace localSpace, XrTime time, bool focused, bool rightStickButtons, bool menuMode,
            bool spyMode, Controls* out)
{
    *out = Controls();
    if (!g_ok) return;
    ScopeKeyUp();

    XrActiveActionSet active = { g_set, XR_NULL_PATH };
    XrActionsSyncInfo si{ XR_TYPE_ACTIONS_SYNC_INFO };
    si.countActiveActionSets = 1;
    si.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(session, &si)) || !focused) {
        ReleaseAll();
        return;
    }

    Raw r;
    r.move = GetVec2(session, g_move);
    r.look = GetVec2(session, g_look);
    r.trigL = GetFloat(session, g_trigL);
    r.trigR = GetFloat(session, g_trigR);
    r.gripL = GetFloat(session, g_gripL);
    r.gripR = GetFloat(session, g_gripR);
    r.a = GetBool(session, g_a);
    r.b = GetBool(session, g_b);
    r.x = GetBool(session, g_x);
    r.y = GetBool(session, g_y);
    r.clickL = GetBool(session, g_clickL);
    r.clickR = GetBool(session, g_clickR);
    r.menu = GetBool(session, g_menu);
    XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
    if (XR_SUCCEEDED(xrLocateSpace(g_aimSpace, localSpace, time, &loc)) &&
        (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
        (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
        r.aimValid = true;
        r.aim = loc.pose;
    }
    Process(r, rightStickButtons, menuMode, spyMode, out);
}

void InstallKeyStateHooks()
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    oGetKeyState = (GetKeyState_t)GetProcAddress(user32, "GetKeyState");
    oGetAsyncKeyState = (GetKeyState_t)GetProcAddress(user32, "GetAsyncKeyState");
    int a = rawinject::PatchAllImports((FARPROC)oGetKeyState, (void*)hkGetKeyState);
    int b = rawinject::PatchAllImports((FARPROC)oGetAsyncKeyState, (void*)hkGetAsyncKeyState);
    Log("Key state hooks: GetKeyState in %d, GetAsyncKeyState in %d import(s)", a, b);
}

void InitXrApi()
{
    LoadBindings();
    g_logSends = true;
    Log("Controller input ready (XR API)");
}

void UpdateXrApi(const xrapi::Packet& p, bool focused, bool rightStickButtons, bool menuMode, bool spyMode,
                 float controllerRoll, Controls* out)
{
    *out = Controls();
    ScopeKeyUp();
    // Button order of the XR API packet: L_GRIP L_MENU L_STICK L_LEFT L_RIGHT L_UP L_DOWN
    // L_TRIGGER X Y A B R_GRIP R_STICK R_LEFT R_RIGHT R_UP R_DOWN R_TRIGGER
    const bool* b = p.buttons;
    {
        // Diagnostics: the first changes of the controller state as received.
        static char last[20];
        static unsigned logged;
        char now[20];
        for (int i = 0; i < 19; i++) now[i] = b[i] ? 'T' : 'F';
        now[19] = 0;
        bool sticks = fabsf(p.leftStick[0]) + fabsf(p.leftStick[1]) + fabsf(p.rightStick[0]) + fabsf(p.rightStick[1]) > 0.3f;
        if ((strcmp(now, last) || sticks) && logged < 40) {
            logged++;
            Log("XrAPI input: buttons %s sticks %.2f %.2f / %.2f %.2f, game window focused %d", now, p.leftStick[0],
                p.leftStick[1], p.rightStick[0], p.rightStick[1], focused);
            memcpy(last, now, sizeof(now));
        }
    }
    if (!focused) {
        ReleaseAll();
        return;
    }
    Raw r;
    r.move = { p.leftStick[0], p.leftStick[1] };
    r.look = { p.rightStick[0], p.rightStick[1] };
    r.gripL = b[0] ? 1.0f : 0.0f;
    r.menu = b[1];
    r.clickL = b[2];
    r.trigL = b[7] ? 1.0f : 0.0f;
    r.x = b[8];
    r.y = b[9];
    r.a = b[10];
    r.b = b[11];
    r.gripR = b[12] ? 1.0f : 0.0f;
    r.clickR = b[13];
    r.trigR = b[18] ? 1.0f : 0.0f;
    // The XR API's controller orientation is rolled compared to an OpenXR aim pose.
    float h = controllerRoll * 0.5f / 57.29578f;
    float qx = p.rightQ[0], qy = p.rightQ[1], qz = p.rightQ[2], qw = p.rightQ[3];
    float rz = sinf(h), rw = cosf(h);   // q * (0, 0, sin, cos): roll about the controller's own Z
    r.aim.orientation = { qx * rw + qy * rz, qy * rw - qx * rz, qz * rw + qw * rz, qw * rw - qz * rz };
    r.aim.position = { p.rightPos[0], p.rightPos[1], p.rightPos[2] };
    float len = qx * qx + qy * qy + qz * qz + qw * qw;
    r.aimValid = len > 0.5f && len < 1.5f;
    Process(r, rightStickButtons, menuMode, spyMode, out);
}

namespace {

void Process(const Raw& r, bool rightStickButtons, bool menuMode, bool spyMode, Controls* out)
{
    const XrVector2f move = r.move, look = r.look;
    bool state[kCtlCount];
    state[kLeftTrigger] = Threshold(r.trigL, g_held[kLeftTrigger]);
    state[kRightTrigger] = Threshold(r.trigR, g_held[kRightTrigger]);
    state[kLeftGrip] = Threshold(r.gripL, g_held[kLeftGrip]);
    state[kRightGrip] = Threshold(r.gripR, g_held[kRightGrip]);
    state[kA] = r.a;
    state[kB] = r.b;
    state[kX] = r.x;
    state[kY] = r.y;
    state[kLeftClick] = r.clickL;
    state[kRightClick] = r.clickR;
    state[kMenu] = r.menu;
    state[kLeftUp] = Threshold(move.y, g_held[kLeftUp], 0.5f, 0.35f);
    state[kLeftDown] = Threshold(-move.y, g_held[kLeftDown], 0.5f, 0.35f);
    state[kLeftLeft] = Threshold(-move.x, g_held[kLeftLeft], 0.5f, 0.35f);
    state[kLeftRight] = Threshold(move.x, g_held[kLeftRight], 0.5f, 0.35f);
    state[kRightUp] = rightStickButtons && Threshold(look.y, g_held[kRightUp], 0.7f, 0.5f);
    state[kRightDown] = rightStickButtons && Threshold(-look.y, g_held[kRightDown], 0.7f, 0.5f);

    // Switching between game, spy and menu controls: let go of everything pressed with the old set.
    int context = menuMode ? kCtxMenu : spyMode ? kCtxSpy : kCtxGame;
    // Controls that are still held stay "held" but are ignored until released, so the press that
    // caused the switch is not seen again as a new press.
    if (context != g_context) {
        for (int i = 0; i < kCtlCount; i++) {
            if (g_held[i] && !g_pending[i]) SendKey(g_active[i], false);
            if (g_tapUpTick[i]) SendKey(g_tapUp[i], false);
            g_tapUpTick[i] = 0;
            g_pending[i] = false;
            g_ignore[i] = g_held[i];
        }
        g_context = context;
    }

    DWORD now = GetTickCount();

    // Both stick clicks together toggle the spy's first-person view. A stick click is held back
    // for kChordMs to see whether the other one follows; a chord sends neither key.
    {
        const int sticks[2] = { kLeftClick, kRightClick };
        if (context == kCtxMenu) {
            g_clickWait[0] = g_clickWait[1] = false;
        } else if (g_chordHeld) {
            if (!state[kLeftClick] && !state[kRightClick]) g_chordHeld = false;
            state[kLeftClick] = state[kRightClick] = false;
        } else {
            for (int k = 0; k < 2; k++)
                if (state[sticks[k]] && !g_held[sticks[k]] && !g_ignore[sticks[k]] && !g_clickWait[k]) {
                    g_clickWait[k] = true;
                    g_clickTick[k] = now;
                }
            if (g_clickWait[0] && g_clickWait[1]) {
                out->fpvToggle = true;
                g_chordHeld = true;
                g_clickWait[0] = g_clickWait[1] = false;
                state[kLeftClick] = state[kRightClick] = false;
            }
            for (int k = 0; k < 2; k++) {
                if (!g_clickWait[k]) continue;
                int c = sticks[k];
                if (!state[c]) {
                    // Released while waiting: a normal quick press.
                    Bind tap, hold;
                    Resolve(c, context, &tap, &hold);
                    Fire(tap, true, out);
                    if (tap.kind == Kind::Key || tap.kind == Kind::Mouse) {
                        g_tapUp[c] = tap;
                        g_tapUpTick[c] = now + kTapLengthMs;
                    }
                    g_clickWait[k] = false;
                } else if (now - g_clickTick[k] >= kChordMs) {
                    g_clickWait[k] = false;     // pressed alone: handled below as a normal press
                } else {
                    state[c] = false;           // still waiting for the other stick
                }
            }
        }
    }

    for (int i = 0; i < kCtlCount; i++) {
        if (g_tapUpTick[i] && now >= g_tapUpTick[i]) {
            SendKey(g_tapUp[i], false);
            g_tapUpTick[i] = 0;
        }
        Bind tap, hold;
        Resolve(i, context, &tap, &hold);

        if (g_ignore[i]) {
            if (!state[i]) g_ignore[i] = g_held[i] = false;
            continue;
        }

        if (state[i] && !g_held[i]) {                       // pressed
            g_held[i] = true;
            if (i == kB) out->bPressed = true;
            g_pressTick[i] = g_repeatTick[i] = now;
            if (hold.kind != Kind::None) {
                g_pending[i] = true;                        // decide tap vs hold later
            } else {
                g_active[i] = tap;
                Fire(tap, true, out);
                if (i == kMenu) out->menuPressed = true;
            }
        } else if (!state[i] && g_held[i]) {                // released
            g_held[i] = false;
            if (g_pending[i]) {
                // Short press of a control with a hold binding: send its normal binding as a tap.
                g_pending[i] = false;
                Fire(tap, true, out);
                if (i == kMenu) out->menuPressed = true;
                if (tap.kind == Kind::Key || tap.kind == Kind::Mouse) {
                    g_tapUp[i] = tap;
                    g_tapUpTick[i] = now + kTapLengthMs;
                }
            } else {
                Fire(g_active[i], false, out);
            }
        } else if (state[i]) {                              // still held
            if (g_pending[i] && now - g_pressTick[i] >= kHoldTimeMs) {
                g_pending[i] = false;
                g_active[i] = hold;
                Fire(hold, true, out);
            }
            // Menus: holding a direction repeats it like a held arrow key.
            bool arrow = i >= kLeftUp && i <= kLeftRight;
            if (!g_pending[i] && context == kCtxMenu && arrow && g_active[i].kind == Kind::Key &&
                now - g_pressTick[i] > 400 && now - g_repeatTick[i] > 150) {
                SendKey(g_active[i], false);
                SendKey(g_active[i], true);
                g_repeatTick[i] = now;
            }
        }
    }

    out->lookX = Deadzone(look.x, 0.15f);
    out->lookY = Deadzone(look.y, 0.15f);
    out->moveX = move.x;
    out->moveY = move.y;
    out->aimValid = r.aimValid;
    out->aim = r.aim;
}

} // namespace

} // namespace input
