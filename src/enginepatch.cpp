#include <windows.h>
#include <cstring>
#include <cmath>
#include <cstdio>
#include "enginepatch.h"
#include "vrmath.h"
#include "vr.h"
#include "config.h"
#include "log.h"

namespace enginepatch {

// ---- Head culling -------------------------------------------------------------------------
// The engine culls (and draws) from its camera, the "scene node", which follows the game's camera
// rotation. In VR that camera is steered to the controller's aim, so whatever the head looks at
// but the controller does not can be culled away (black holes, missing geometry).
//
// The scene node's matrices are built by one function (0x10A16670, stdcall, one argument: the node)
// from the origin at node+0x194, the rotation (3 ints, 65536 = 360 deg: pitch, yaw, roll) at
// node+0x1D8 and the horizontal FOV in degrees at node+0x1E4. A hook at its start replaces the
// rotation (and optionally widens the FOV) of the main view's node by the head's orientation.
// The engine then culls with it, while the game's logic (firing, the controller's rotation) is
// untouched. The hook keeps both versions of the node's world->camera matrix so that the device
// hooks can turn the camera matrix the game sends back into the unmodified one: the rest of the
// mod, built around the game camera, sees no difference.
//
//   10A16670  81 EC 8C 00 00 00   sub esp, 8Ch      (entry; 10A16676 = push ebx)
static const DWORD kSite = 0x10A16670;
static const DWORD kResume = 0x10A16676;
static const BYTE kOrig[7] = { 0x81, 0xEC, 0x8C, 0x00, 0x00, 0x00, 0x53 };

static const int kMaxPairs = 8;
struct Pair {
    D3DMATRIX over, overInv, tru;   // world -> camera (node+0x14) of the modified / unmodified node
    float fovOver, fovTrue;         // degrees
};
static Pair g_pairs[kMaxPairs];
static volatile int g_pairCount = 0;

static volatile bool g_on = false;
static bool g_haveMain = false;     // the frame's main rotation is known
static int g_mainRot[3] = {};
static float g_mainOrg[3] = {};
static float g_lastHeadYaw = 0, g_lastHeadPitch = 0;
static int g_logFrames = 0;         // diagnostics: scene nodes still to log
static int g_logCount = 0;

static __declspec(naked) void OrigStub()
{
    __asm {
        sub esp, 0x8C
        push 0x10A16676
        ret
    }
}
typedef void(__stdcall* OrigFn)(BYTE*);

static void __cdecl OnNode(BYTE* node)
{
    if (!g_on) return;
    int* rot = (int*)(node + 0x1D8);
    float* org = (float*)(node + 0x194);
    float* fov = (float*)(node + 0x1E4);
    bool log = g_logFrames > 0 && g_logCount < 60;

    float yaw, pitch, newOrg[3];
    bool moved = false;
    if (!vr::HeadCullCamera(org, rot, &yaw, &pitch, newOrg, &moved)) {
        if (log) { g_logCount++; Log("HeadCull: node %p origin (%.0f %.0f %.0f) rot %d %d %d fov %.1f - no head angles", node, org[0], org[1], org[2], rot[0], rot[1], rot[2], *fov); }
        return;
    }
    // Zoom or small windows (weapon scope etc.): not the main view.
    bool wide = *fov >= 80.0f && *fov < 170.0f;
    if (wide && !g_haveMain) {
        g_haveMain = true;
        memcpy(g_mainRot, rot, sizeof(g_mainRot));
        memcpy(g_mainOrg, org, sizeof(g_mainOrg));
    }
    bool same = g_haveMain && memcmp(g_mainRot, rot, sizeof(g_mainRot)) == 0 && wide;
    if (log) {
        g_logCount++;
        Log("HeadCull: node %p origin (%.0f %.0f %.0f) rot %d %d %d fov %.1f -> %s%s", node, org[0], org[1], org[2],
            rot[0], rot[1], rot[2], *fov, same ? "head" : "left alone", moved ? " (+ spy camera origin)" : "");
    }
    int idx = g_pairCount;
    if (!same || idx >= kMaxPairs) return;

    Pair& p = g_pairs[idx];
    OrigFn orig = (OrigFn)(void*)&OrigStub;
    orig(node);                                     // matrices of the unmodified node
    memcpy(&p.tru, node + 0x14, sizeof(D3DMATRIX));
    p.fovTrue = *fov;

    const float k = 65536.0f / 6.2831853f;
    rot[0] = (int)(pitch * k);
    rot[1] = (int)(yaw * k);
    rot[2] = 0;
    // Spy: the VR view is somewhere else than the game camera. Not for the skybox camera (far away).
    if (moved) {
        float dx = org[0] - g_mainOrg[0], dy = org[1] - g_mainOrg[1], dz = org[2] - g_mainOrg[2];
        if (dx * dx + dy * dy + dz * dz < 400.0f * 400.0f) memcpy(org, newOrg, sizeof(newOrg));
    }
    if (g_cfg.headCullFov > *fov && g_cfg.headCullFov < 170.0f) *fov = g_cfg.headCullFov;
    p.fovOver = *fov;
    orig(node);                                     // matrices of the modified node
    memcpy(&p.over, node + 0x14, sizeof(D3DMATRIX));
    if (!Inverse(p.over, &p.overInv)) return;
    g_lastHeadYaw = yaw;
    g_lastHeadPitch = pitch;
    g_pairCount = idx + 1;                          // published last
    // the original function now runs once more on the modified node
}

static __declspec(naked) void NodeCave()
{
    __asm {
        pushad
        mov eax, [esp + 0x24]       // the scene node (above the return address)
        push eax
        call OnNode
        add esp, 4
        popad
        sub esp, 0x8C
        push 0x10A16676
        ret
    }
}

static bool WriteCode(DWORD addr, const void* data, size_t n)
{
    DWORD old;
    if (!VirtualProtect((void*)addr, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy((void*)addr, data, n);
    VirtualProtect((void*)addr, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)addr, n);
    return true;
}

static bool InstallHook()
{
    if ((DWORD)(ULONG_PTR)GetModuleHandleW(nullptr) != 0x10900000) {
        Log("EnginePatch: game executable is not at 0x10900000 - skipped");
        return false;
    }
    if (memcmp((void*)kSite, kOrig, sizeof(kOrig)) != 0) {
        Log("EnginePatch: unexpected code at 0x%08X (different game version?) - skipped", kSite);
        return false;
    }
    BYTE jmp[6] = { 0xE9, 0, 0, 0, 0, 0x90 };
    *(int*)(jmp + 1) = (int)((DWORD)(ULONG_PTR)&NodeCave - (kSite + 5));
    if (!WriteCode(kSite, jmp, sizeof(jmp))) {
        Log("EnginePatch: could not write 0x%08X", kSite);
        return false;
    }
    return true;
}

void NewFrame()
{
    g_pairCount = 0;
    g_haveMain = false;
    if (g_logFrames > 0) g_logFrames--;
}

static int g_undoLogs = 0;

// Camera-space conversion of the view the device holds now (see CameraSpaceFix).
static bool g_qValid = false;
static D3DMATRIX g_q;

bool UndoView(const D3DMATRIX& in, D3DMATRIX* out)
{
    int n = g_pairCount;
    g_qValid = false;
    if (!g_on || n <= 0) return false;
    int best = -1;
    float bestDev = 1e30f;
    D3DMATRIX bestS = {};
    for (int i = 0; i < n; i++) {
        // in = over * S, S = a fixed axis permutation (engine space -> D3D space)
        D3DMATRIX S = Mul(g_pairs[i].overInv, in);
        float dev = 0;
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) {
                float v = S.m[r][c];
                dev = fmaxf(dev, fminf(fabsf(v), fminf(fabsf(v - 1), fabsf(v + 1))));
            }
        for (int c = 0; c < 3; c++) dev = fmaxf(dev, fabsf(S.m[3][c]) * 0.02f);
        dev = fmaxf(dev, fabsf(S._44 - 1));
        if (dev < bestDev) { bestDev = dev; best = i; bestS = S; }
    }
    if (g_cfg.headCullLog && g_undoLogs < 12) {
        g_undoLogs++;
        Log("HeadCull: view %d candidate(s), best %d dev %.4f; S rows (%.2f %.2f %.2f) (%.2f %.2f %.2f) (%.2f %.2f %.2f) t (%.2f %.2f %.2f)",
            n, best, bestDev, bestS._11, bestS._12, bestS._13, bestS._21, bestS._22, bestS._23,
            bestS._31, bestS._32, bestS._33, bestS._41, bestS._42, bestS._43);
        D3DMATRIX inv;
        if (Inverse(in, &inv)) {
            Vec3 f = Row(inv, 2);
            Log("HeadCull: received view looks yaw %.1f pitch %.1f deg; head yaw %.1f pitch %.1f deg",
                atan2f(f.y, f.x) * 57.29578f, asinf(fmaxf(-1.0f, fminf(1.0f, f.z / Length(f)))) * 57.29578f,
                g_lastHeadYaw * 57.29578f, g_lastHeadPitch * 57.29578f);
        }
    }
    if (best < 0 || bestDev > 0.01f) return false;
    *out = Mul(g_pairs[best].tru, bestS);
    D3DMATRIX trueInv;
    if (Inverse(*out, &trueInv)) { g_q = Mul(trueInv, in); g_qValid = true; }
    return true;
}

bool CameraSpaceFix(D3DMATRIX* q)
{
    if (!g_on || !g_qValid) return false;
    *q = g_q;
    return true;
}

bool UndoProj(const D3DMATRIX& in, D3DMATRIX* out)
{
    int n = g_pairCount;
    if (!g_on || n <= 0 || fabsf(in._11) < 1e-6f) return false;
    float tx = in._34 / in._11;
    for (int i = 0; i < n; i++) {
        const Pair& p = g_pairs[i];
        if (p.fovOver == p.fovTrue) continue;
        float tOver = tanf(p.fovOver * 0.00872665f), tTrue = tanf(p.fovTrue * 0.00872665f);
        if (fabsf(tx - tOver) > 0.003f * tOver) continue;
        float ratio = tOver / tTrue;
        *out = in;
        for (int r = 0; r < 4; r++) { out->m[r][0] *= ratio; out->m[r][1] *= ratio; }
        return true;
    }
    return false;
}

static DWORD WINAPI Worker(LPVOID)
{
    bool prev = false;
    for (;;) {
        Sleep(50);
        if (!g_cfg.headCullToggleKey) continue;
        bool down = (GetAsyncKeyState(g_cfg.headCullToggleKey) & 0x8000) != 0;
        if (down && !prev) {
            g_on = !g_on;
            g_logFrames = 3; g_logCount = 0; g_undoLogs = 0;
            Log("EnginePatch: head culling %s", g_on ? "ON" : "off");
        }
        prev = down;
    }
}

void Init()
{
    if (!g_cfg.headCull) return;
    if (!InstallHook()) return;
    g_on = true;
    if (g_cfg.headCullLog) g_logFrames = 3;
    Log("EnginePatch: head culling hook installed (widen FOV %.0f, toggle key 0x%X)", g_cfg.headCullFov, g_cfg.headCullToggleKey);
    if (g_cfg.headCullToggleKey) CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
}

}  // namespace enginepatch
