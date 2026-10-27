#pragma once
#include <d3d9.h>
#include "vrmath.h"

namespace vr {

// Creates the OpenXR instance/session and the D3D12 device that D3D9On12 must run on.
// Returns false (and leaves the game in plain flat mode) if anything is missing.
bool Init();
bool Available();
// Running on WinlatorXR's XR API (standalone headset): plain D3D9, no OpenXR / D3D9On12.
bool UsesXrApi();
IUnknown* D3D12Device();
IUnknown* D3D12Queue();

void OnDeviceCreated(IDirect3DDevice9* dev, HWND wnd);
void OnPreReset();
void OnPostReset(IDirect3DDevice9* dev);

// Called right before the real Present: copies the finished eye image and advances the XR frame.
void OnPresent(IDirect3DDevice9* dev);

// Replaces the game's main camera view/projection with the current eye's.
// Returns false when this view must be left untouched.
bool EyeTransform(const D3DMATRIX& view, const D3DMATRIX& proj, D3DMATRIX* outView, D3DMATRIX* outProj);

// Render target for 2D drawing while VR is running (AddRef'd, caller releases), else nullptr.
IDirect3DSurface9* HudSurface();
// XR API: the HUD texture and its panel in the current eye image as 4 pretransformed vertices
// (x, y, z, rhw, u, v; triangle strip) to draw into the backbuffer before Present.
bool HudOverlay(IDirect3DTexture9** tex, float verts[4][6]);
void NoteHudDrawn();

// PC desktop window (for streaming): the eye this game frame renders and the tangents of its
// field of view. False when the frame is not stereo gameplay (menus, zoom), which the window
// shows as the game drew it.
struct MirrorFrame {
    int eye;
    float left, right, up, down;
    bool hud;           // this frame drew HUD
    bool hudPanel;      // hudVerts valid: the HUD panel as the headset shows it in this eye
    float hudVerts[4][6];   // x, y (backbuffer pixels), z, rhw, u, v; triangle strip
};
bool MirrorFrameInfo(MirrorFrame* out);
// The HUD texture (AddRef'd, caller releases), or nullptr.
IDirect3DTexture9* HudTexture();

// World matrix that moves a first-person weapon (placed relative to the game camera) into the
// right hand. False when aim mode or controller tracking is not active.
bool WeaponWorld(const D3DMATRIX& world, const D3DMATRIX& gameView, D3DMATRIX* out);

// The previous frame pair was 3D gameplay (not a menu / loading screen).
bool SceneActive();

// Menu mouse cursor position as drawn by the game this frame (game's virtual 2D space).
void NoteMenuCursor(bool found, float x, float y, float spaceW, float spaceH);

// Whether this frame drew the in-game menu (detected by its panel texture).
void NoteInGameMenu(bool seen);

// Ends the XR session before the process exits (the runtime can hang during DLL unload).
void Shutdown();

// Called by the device hooks for every stereo 3D draw / first-person weapon draw.
void NoteSceneDraw();

// Spy (third person) model search. True when this draw is now the best candidate for the
// spy's body: the caller then reads its top vertex and passes it to NoteSpyHead.
bool SpyModelCandidate(const D3DMATRIX& world, unsigned prims);
void NoteSpyHead(Vec3 top);
// Spy first-person view: true for draws of the spy's own model (skip them).
bool HideDraw(const D3DMATRIX& world, unsigned prims);
void NoteWeaponDraw();

// Laser beam quad for the current eye (body camera space) with the matrices to draw it.
bool LaserQuad(Vec3* corners, D3DMATRIX* view, D3DMATRIX* proj, D3DCOLOR* color);

} // namespace vr
