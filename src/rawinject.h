#pragma once
#include <windows.h>

// Synthetic raw input for the game. Reloaded reads the keyboard and mouse with raw input
// (WM_INPUT / GetRawInputData / GetRawInputBuffer), and under Wine input injected with
// SendInput produces no raw input. So in a WinlatorXR container the controller's keys, mouse
// buttons and mouse motion are handed to Reloaded's raw input calls directly: hooked
// GetRawInputData / GetRawInputBuffer return queued synthetic events, and WM_INPUT messages
// with a marker handle announce them.
namespace rawinject {

// Hooks the raw input functions in every loaded module's import table. `wnd`: game window,
// used when no raw input device was registered with a target window.
void Install(HWND wnd);
bool Active();

void Key(WORD scan, bool extended, bool down);
// buttonFlags: RI_MOUSE_* button flags; wheel: WHEEL_DELTA multiples (0 = none).
void Mouse(LONG dx, LONG dy, USHORT buttonFlags, short wheel);

// Redirects `target` in every loaded module's import table (except this mod's). Returns the
// number of patched imports.
int PatchAllImports(FARPROC target, void* hook);

// Diagnostics for the statistics line.
void Stats(char* buf, size_t size);

} // namespace rawinject
