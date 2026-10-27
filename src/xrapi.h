#pragma once
#include <windows.h>

// WinlatorXR's XR API (https://winlatorxr.github.io/xrapi.html), used instead of OpenXR when
// the game runs in a WinlatorXR container on a standalone headset. Version 0.5 of the API:
// - tracking arrives as a text UDP packet on 127.0.0.1:7872 (OpenXR convention: +Y up,
//   -Z forward, meters, relative to the origin WinlatorXR captured);
// - the game sends "L_HAPTICS R_HAPTICS MODE_VR MODE_3D FOVX FOVY" to 127.0.0.1:7278;
// - every presented frame carries the HMD_SYNC value of its pose as the red channel of the
//   top-left pixels (and, in alternate-eye mode, the eye as blue: 0 left, 255 right).
namespace xrapi {

struct Packet {
    float leftQ[4], leftStick[2], leftPos[3];
    float rightQ[4], rightStick[2], rightPos[3];
    float headQ[4], headPos[3];
    float ipd;              // meters
    float fovX, fovY;       // degrees
    int sync;
    bool buttons[19];       // L_GRIP L_MENU L_STICK L_LEFT L_RIGHT L_UP L_DOWN L_TRIGGER X Y
                            // A B R_GRIP R_STICK R_LEFT R_RIGHT R_UP R_DOWN R_TRIGGER
};

enum VrMode { kVrOff = 0, kVrImmersive = 1, kVrScreen = 2 };
enum Mode3D { k3DFlat = 0, k3DSideBySide = 1, k3DAlternateEye = 2 };

// The container exposes Z:\tmp\xr\system when the XR API is enabled for it.
bool Available();
// Writes the API version and VR marker files, opens the sockets, starts the receive thread.
bool Start();
// Latest tracking packet; false until the first one arrives. `ageMs`: time since it arrived.
bool Latest(Packet* out, DWORD* ageMs, unsigned* received);
void SendState(int vrMode, int mode3D, float fovX, float fovY);

} // namespace xrapi
