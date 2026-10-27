#pragma once
#include <windows.h>

// The game reads its keyboard with DirectInput 8, and under Wine (WinlatorXR) DirectInput does
// not see keys injected with SendInput. So in a WinlatorXR container the game's DirectInput
// keyboard is wrapped: keys the controllers press are added to its device state and buffered
// data, held for as long as the controller holds them.
namespace dihook {

// Redirects the game's import of dinput8!DirectInput8Create (must run before the game creates
// its devices: called from DllMain when a WinlatorXR container is detected).
void InstallEarly();
bool Active();

// dik: DirectInput key code (scan code, | 0x80 for extended keys).
void Key(BYTE dik, bool down);

// The game's DirectInput mouse (it turns the camera with it): relative motion, wheel and
// buttons (0 left, 1 right, 2 middle) from the controllers.
bool MouseActive();
void MouseMove(LONG dx, LONG dy, LONG wheel);
void MouseButton(int index, bool down);

void Stats(char* buf, size_t size);

} // namespace dihook
