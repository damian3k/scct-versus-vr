#pragma once
#include <d3d9.h>

// Patches into the game's Unreal Engine 2.5 code (SCCT Versus 3.6, 32-bit exe at 0x10900000).
namespace enginepatch {

// Installs the hooks that are enabled in the config (does nothing otherwise).
void Init();

// Called once per game frame (at Present): scene nodes of the next frame start fresh.
void NewFrame();

// The engine builds the main view's scene node from the head's orientation (head culling).
// The matrices that reach the device are the game camera's again after these two calls:
// they convert a matrix the engine produced from a modified node back to the unmodified one.
// Return false (output untouched) when the input does not come from a modified node.
bool UndoView(const D3DMATRIX& in, D3DMATRIX* out);
bool UndoProj(const D3DMATRIX& in, D3DMATRIX* out);

// The engine also derives other things from the modified node: texture matrices for projected
// lights, shadows and reflections take camera space coordinates of the modified camera. q maps
// coordinates in the game camera's space (what the mod's eye-to-game conversion produces) to the
// modified camera's space: multiply it in before the game's texture matrix. False when the view
// the device holds is not a modified node's.
bool CameraSpaceFix(D3DMATRIX* q);

} // namespace enginepatch
