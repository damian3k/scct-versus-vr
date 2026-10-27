#pragma once
#include <d3d9.h>

// Patches IDirect3D9::CreateDevice so the created device gets the VR hooks.
void HookDirect3D9(IDirect3D9* d3d);
