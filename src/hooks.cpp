// Vtable hooks on the D3D9 device created by Reloaded's D3D8->D3D9 layer.
//
// The game's camera arrives through SetTransform(VIEW/PROJECTION); vertex shaders receive
// premultiplied World*View*Projection matrices through SetVertexShaderConstantF, which are
// detected and re-projected for the current eye.

#include "hooks.h"
#include "vr.h"
#include "vrmath.h"
#include "config.h"
#include "log.h"
#include <cstdio>
#include <cstdarg>
#include <share.h>

namespace {

// IDirect3D9 / IDirect3DDevice9 / IDirect3DSwapChain9 vtable slots
enum {
    kD3D9_CreateDevice = 16,
    kDev_Reset = 16,
    kDev_Present = 17,
    kDev_GetSwapChain = 14,
    kDev_SetRenderTarget = 37,
    kDev_GetRenderTarget = 38,
    kDev_Clear = 43,
    kDev_SetTransform = 44,
    kDev_GetTransform = 45,
    kDev_MultiplyTransform = 46,
    kDev_SetTexture = 65,
    kDev_SetTextureStageState = 67,
    kDev_DrawPrimitive = 81,
    kDev_DrawIndexedPrimitive = 82,
    kDev_DrawPrimitiveUP = 83,
    kDev_DrawIndexedPrimitiveUP = 84,
    kDev_SetFVF = 89,
    kDev_SetVertexShader = 92,
    kDev_SetVertexShaderConstantF = 94,
    kDev_SetVertexDeclaration = 87,
    kDev_SetPixelShader = 107,
    kDev_SetPixelShaderConstantF = 109,
    kSC_Present = 3,
};

using SetTSS_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, D3DTEXTURESTAGESTATETYPE, DWORD);
SetTSS_t oSetTSS;
using SetVDecl_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DVertexDeclaration9*);
using SetPS_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DPixelShader9*);
SetVDecl_t oSetVDecl;
SetPS_t oSetPS;
using SetPSConstF_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);
SetPSConstF_t oSetPSConstF;

using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using Reset_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using SCPresent_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
using SetRenderTarget_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using SetTransform_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
using GetTransform_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, D3DMATRIX*);
using SetVSConstF_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);
using GetRenderTarget_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9**);
using Clear_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
using SetTexture_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
using DrawPrimitive_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedPrimitive_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawPrimitiveUP_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIndexedPrimitiveUP_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
using SetFVF_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD);
using SetVertexShader_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DVertexShader9*);

GetRenderTarget_t oGetRenderTarget;
Clear_t oClear;
SetTexture_t oSetTexture;
DrawPrimitive_t oDrawPrimitive;
DrawIndexedPrimitive_t oDrawIndexedPrimitive;
DrawPrimitiveUP_t oDrawPrimitiveUP;
DrawIndexedPrimitiveUP_t oDrawIndexedPrimitiveUP;
SetFVF_t oSetFVF;
SetVertexShader_t oSetVertexShader;

CreateDevice_t oCreateDevice;
Reset_t oReset;
Present_t oPresent;
SCPresent_t oSCPresent;
SetRenderTarget_t oSetRenderTarget;
SetTransform_t oSetTransform;
GetTransform_t oGetTransform;
SetTransform_t oMultiplyTransform;
SetVSConstF_t oSetVSConstF;

struct DevState {
    IDirect3DDevice9* dev = nullptr;
    D3DMATRIX view = Identity(), proj = Identity();
    bool haveView = false, haveProj = false;
    UINT bbW = 0, bbH = 0;
    bool rtMain = true;
    bool stereoNow = false;
    D3DMATRIX vpInv, vpNew;     // original View*Proj inverse, and the eye's View*Proj
    bool vpValid = false;
    bool inPresent = false;
    // 2D redirection into the VR HUD layer
    DWORD fvf = 0;
    bool vsBound = false;
    bool stage0IsRT = false;      // post-processing passes sample render targets: keep them in the scene
    bool redirected = false;
    IDirect3DSurface9* savedRT = nullptr;
    bool hudCleared = false;
    UINT tex0W = 0, tex0H = 0;
    // first-person weapon: drawn after a depth-only clear, close to the camera
    D3DMATRIX world = Identity();
    Vec3 camPos = { 0, 0, 0 };
    int zClears = 0;              // depth-only clears after 3D drawing started (weapon pass)
    int draws3D = 0;              // stereo 3D draws since the last colour clear
    bool frameStereo = false;
    // Camera-space texture coordinate generation (projected lights like the flashlight, shadow
    // projectors): the game builds texture matrices for its own camera space, so with the eye
    // camera they must be prefixed with eye-space -> game-camera-space.
    D3DMATRIX eyeToGame = Identity();       // inv(eye view) * game view
    // Screen-space lookups (the spy's camouflage samples a copy of the rendered image at the
    // pixel behind it): the image is the eye's, so the game's projection in the texture matrix
    // is swapped for the eye's: eye proj * inv(game proj).
    D3DMATRIX screenFix = Identity();
    bool screenFixValid = false;
    D3DMATRIX texT[8];                       // texture matrices as set by the game
    bool texTSet[8] = {};
    DWORD texCI[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };   // D3DTSS_TEXCOORDINDEX
    DWORD texFlags[8] = {};                  // D3DTSS_TEXTURETRANSFORMFLAGS
    bool texFixed[8] = {};                   // device currently holds our corrected matrix/flags
    D3DMATRIX texApplied[8];                 // the corrected matrix we put on the device
    unsigned texgenFixes = 0, psUploads = 0;
    unsigned frame3D = 0;
    // The game renders the world into an offscreen target and composites it onto the
    // backbuffer with a 2D quad; that composite belongs to the scene, not the HUD.
    IDirect3DSurface9* bbSurf = nullptr;   // not AddRef'd, identity only
    bool rtIsBB = true;
    bool offscreenScene = false;
    bool compositeDone = false;
    bool hudStarted = false;      // depth-only clear on the backbuffer after the composite
    // menu cursor sprite (game's virtual 2D coordinates, e.g. 640x480)
    bool cursorFound = false;
    unsigned twoDCount = 0, cursorIdx = 0;
    bool menuPanelSeen = false;   // in-game menu background drawn this frame
    float cursorX = 0, cursorY = 0, menuW = 0, menuH = 0;
    IDirect3DStateBlock9* laserState = nullptr;
    IDirect3DStateBlock9* hudState = nullptr;
    IDirect3DTexture9* mirrorTex = nullptr;  // desktop window image during gameplay (left eye + HUD)
    bool mirrorValid = false;
    LONGLONG hookDrawTicks = 0, driverDrawTicks = 0, hookTransformTicks = 0;
    // frame dump
    FILE* dump = nullptr;
    int dumpLines = 0;
    bool dumpKeyDown = false, dumpRequested = false, autoDumped = false, menuDumped = false;
    const wchar_t* dumpName = L"SCCT_VR_frame.txt";
    unsigned menuFrames = 0;
    unsigned stereoFrames = 0;
    // stats
    DWORD statsStart = 0;
    unsigned frames = 0, viewSets = 0, projSets = 0, stereoApplies = 0, shaderFixes = 0, shaderUploads = 0;
    unsigned draws = 0, hudDraws = 0, fullscreen2D = 0, weaponDraws = 0;
    bool loggedProj = false;
} D;

void Dump(const char* fmt, ...)
{
    if (!D.dump || D.dumpLines > 20000) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(D.dump, fmt, ap);
    va_end(ap);
    fputc('\n', D.dump);
    D.dumpLines++;
}

template <class T>
void Patch(void* obj, int slot, T hook, T* orig)
{
    void** vt = *(void***)obj;
    if (vt[slot] == (void*)hook) return;
    DWORD old;
    VirtualProtect(&vt[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    *orig = (T)vt[slot];
    vt[slot] = (void*)hook;
    VirtualProtect(&vt[slot], sizeof(void*), old, &old);
}

void UpdateBackbufferSize(IDirect3DDevice9* dev)
{
    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
        D3DSURFACE_DESC d;
        bb->GetDesc(&d);
        D.bbW = d.Width;
        D.bbH = d.Height;
        D.bbSurf = bb;
        bb->Release();
    }
    D.rtMain = true;
    D.rtIsBB = true;
}

// Pushes either the eye camera or the game's original camera to the device.
void Apply(IDirect3DDevice9* dev)
{
    D3DMATRIX v = D.view, p = D.proj;
    D.stereoNow = false;
    if (D.haveView && D.haveProj && (!g_cfg.requireMainRenderTarget || D.rtMain) &&
        vr::EyeTransform(D.view, D.proj, &v, &p)) {
        D.stereoNow = true;
        D.frameStereo = true;
        D.stereoApplies++;
        if (g_cfg.fixShaderMatrices) {
            D.vpValid = Inverse(Mul(D.view, D.proj), &D.vpInv);
            D.vpNew = Mul(v, p);
        }
        D3DMATRIX eyeInv, projInv;
        D.eyeToGame = Inverse(v, &eyeInv) ? Mul(eyeInv, D.view) : Identity();
        D.screenFixValid = Inverse(D.proj, &projInv);
        if (D.screenFixValid) D.screenFix = Mul(p, projInv);
    }
    if (D.haveView) oSetTransform(dev, D3DTS_VIEW, &v);
    if (D.haveProj) oSetTransform(dev, D3DTS_PROJECTION, &p);
}

// m = X * View * Proj with X affine (a world matrix) -> X * EyeView * EyeProj
bool FixWvp(D3DMATRIX& m)
{
    if (IsAffine(m, 1e-4f)) return false;
    D3DMATRIX x = Mul(m, D.vpInv);
    if (!IsAffine(x, g_cfg.shaderMatrixTolerance)) return false;
    // A world transform has a real 3D basis. Other constants that happen to pass the affine
    // test (a scale vector next to a colour, rows of zeros) give a flat one: not a matrix.
    // Compared with the rows' lengths, so the test does not depend on the values' scale.
    float det = x._11 * (x._22 * x._33 - x._23 * x._32) - x._12 * (x._21 * x._33 - x._23 * x._31) +
                x._13 * (x._21 * x._32 - x._22 * x._31);
    float lengths = Length(Row(x, 0)) * Length(Row(x, 1)) * Length(Row(x, 2));
    if (!(lengths > 0.0f) || fabsf(det) < 0.1f * lengths) return false;
    x.m[0][3] = x.m[1][3] = x.m[2][3] = 0.0f;
    x._44 = 1.0f;
    m = Mul(x, D.vpNew);
    return true;
}

void PrintStats()
{
    DWORD now = GetTickCount();
    if (!D.statsStart) D.statsStart = now;
    if (g_cfg.statsIntervalSec <= 0 || now - D.statsStart < (DWORD)g_cfg.statsIntervalSec * 1000) return;
    unsigned f = D.frames ? D.frames : 1;
    Log("Device stats per frame: SetTransform VIEW %.1f, PROJ %.1f, stereo camera %.1f, VS constant uploads %.1f, WVP fixed %.1f, draws %.1f (HUD %.1f, fullscreen 2D %.1f, weapon %.1f)",
        D.viewSets / (float)f, D.projSets / (float)f, D.stereoApplies / (float)f, D.shaderUploads / (float)f, D.shaderFixes / (float)f,
        D.draws / (float)f, D.hudDraws / (float)f, D.fullscreen2D / (float)f, D.weaponDraws / (float)f);
    Log("Device stats per frame: camera-space texgen corrected %.1f, PS constant uploads %.1f", D.texgenFixes / (float)f, D.psUploads / (float)f);
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    double ms = 1000.0 / freq.QuadPart / f;
    Log("Time per frame: mod draw hooks %.2f ms (game draw calls inside them %.2f ms), transform hooks %.2f ms",
        (D.hookDrawTicks - D.driverDrawTicks) * ms, D.driverDrawTicks * ms, D.hookTransformTicks * ms);
    D.hookDrawTicks = D.driverDrawTicks = D.hookTransformTicks = 0;
    D.texgenFixes = D.psUploads = 0;
    D.frames = D.viewSets = D.projSets = D.stereoApplies = D.shaderFixes = D.shaderUploads = D.draws = D.hudDraws = 0;
    D.fullscreen2D = D.weaponDraws = 0;
    D.statsStart = now;
}

bool Is2D()
{
    if (D.vsBound) return false;
    if ((D.fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW) return true;
    return D.haveProj && !IsPerspective(D.proj);
}

// Switches the device's render target 0 between the game's target and the HUD texture,
// keeping the game's viewport and scissor rect.
void Unredirect(IDirect3DDevice9* dev)
{
    if (!D.redirected) return;
    D3DVIEWPORT9 vp;
    RECT sc;
    dev->GetViewport(&vp);
    dev->GetScissorRect(&sc);
    oSetRenderTarget(dev, 0, D.savedRT);
    dev->SetViewport(&vp);
    dev->SetScissorRect(&sc);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    D.savedRT->Release();
    D.savedRT = nullptr;
    D.redirected = false;
}

void Redirect(IDirect3DDevice9* dev)
{
    IDirect3DSurface9* hud = vr::HudSurface();
    if (!hud) return;
    D3DVIEWPORT9 vp;
    RECT sc;
    dev->GetViewport(&vp);
    dev->GetScissorRect(&sc);
    oGetRenderTarget(dev, 0, &D.savedRT);
    oSetRenderTarget(dev, 0, hud);
    hud->Release();
    if (!D.hudCleared) {
        oClear(dev, 0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
        D.hudCleared = true;
    }
    dev->SetViewport(&vp);
    dev->SetScissorRect(&sc);
    // Accumulate coverage in alpha so the layer can be composited as premultiplied alpha.
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
    dev->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
    D.redirected = true;
    vr::NoteHudDrawn();
}

struct DrawInfo {
    const char* kind;
    D3DPRIMITIVETYPE type;
    UINT primCount;
    UINT firstVertex, numVertices;   // vertex range used by the draw
    const void* upData;              // DrawPrimitiveUP data, else stream 0
    UINT upStride;
    bool indexed;
};

// Largest fraction of the viewport covered by a single primitive of a 2D draw. Sprite batches
// (many small HUD quads spread over the screen) stay small; a full-screen pass is ~1.
// Triangle lists are measured per triangle, indexed draws per group of 4 vertices (quads).
float ScreenCoverage(IDirect3DDevice9* dev, const DrawInfo& di, bool rhw, float* box = nullptr)
{
    if (di.numVertices == 0 || di.numVertices > 1024) return -1.0f;
    const BYTE* p = nullptr;
    UINT stride = 0;
    // Stream 0 as bound on the device right now (AddRef'd). The game often binds it through
    // state blocks and frees vertex buffers at any time, so a pointer remembered from the
    // SetStreamSource hook can be stale.
    IDirect3DVertexBuffer9* vb = nullptr;
    bool locked = false;
    auto done = [&](float result) {
        if (vb) {
            if (locked) vb->Unlock();
            vb->Release();
        }
        return result;
    };
    if (di.upData) {
        p = (const BYTE*)di.upData + di.firstVertex * di.upStride;
        stride = di.upStride;
    } else {
        UINT vbOffset = 0;
        if (FAILED(dev->GetStreamSource(0, &vb, &vbOffset, &stride)) || !vb || !stride) return done(-1.0f);
        D3DVERTEXBUFFER_DESC vd;
        if (FAILED(vb->GetDesc(&vd))) return done(-1.0f);
        UINT off = vbOffset + di.firstVertex * stride, size = di.numVertices * stride;
        if (off + size > vd.Size) return done(-1.0f);
        void* ptr = nullptr;
        DWORD flags = D3DLOCK_READONLY | ((vd.Usage & D3DUSAGE_DYNAMIC) ? D3DLOCK_NOOVERWRITE : 0);
        if (FAILED(vb->Lock(off, size, &ptr, flags)) || !ptr) return done(-1.0f);
        p = (const BYTE*)ptr;
        locked = true;
    }
    if (stride < 12) return done(-1.0f);

    D3DVIEWPORT9 vp;
    dev->GetViewport(&vp);
    if (!vp.Width || !vp.Height) return done(-1.0f);
    D3DMATRIX wvp = Mul(Mul(D.world, D.view), D.proj);

    UINT group = di.numVertices;
    if (di.indexed) group = 4;
    else if (di.type == D3DPT_TRIANGLELIST) group = 3;

    float best = 0.0f;
    float all[4] = { 1e30f, 1e30f, -1e30f, -1e30f };
    for (UINT g = 0; g < di.numVertices; g += group) {
        float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
        for (UINT i = g; i < g + group && i < di.numVertices; i++) {
            const float* v = (const float*)(p + i * stride);
            float x, y;
            if (rhw) {
                x = v[0];
                y = v[1];
            } else {
                float cx = v[0] * wvp._11 + v[1] * wvp._21 + v[2] * wvp._31 + wvp._41;
                float cy = v[0] * wvp._12 + v[1] * wvp._22 + v[2] * wvp._32 + wvp._42;
                float cw = v[0] * wvp._14 + v[1] * wvp._24 + v[2] * wvp._34 + wvp._44;
                if (fabsf(cw) < 1e-6f) continue;
                x = vp.X + (cx / cw * 0.5f + 0.5f) * vp.Width;
                y = vp.Y + (0.5f - cy / cw * 0.5f) * vp.Height;
            }
            minX = fminf(minX, x); maxX = fmaxf(maxX, x);
            minY = fminf(minY, y); maxY = fmaxf(maxY, y);
        }
        if (maxX < minX) continue;
        all[0] = fminf(all[0], minX); all[1] = fminf(all[1], minY);
        all[2] = fmaxf(all[2], maxX); all[3] = fmaxf(all[3], maxY);
        float cx0 = fmaxf(minX, (float)vp.X), cx1 = fminf(maxX, (float)(vp.X + vp.Width));
        float cy0 = fmaxf(minY, (float)vp.Y), cy1 = fminf(maxY, (float)(vp.Y + vp.Height));
        best = fmaxf(best, fminf((cx1 - cx0) / vp.Width, (cy1 - cy0) / vp.Height));
    }
    if (box) for (int i = 0; i < 4; i++) box[i] = all[i];
    return done(best);
}

// Highest point (world up = Z) of a 3D draw within `radius` horizontally of its own origin: the
// top of the spy's head (the model is skinned on the CPU, so its vertices are in the buffer).
bool HighestVertex(IDirect3DDevice9* dev, const DrawInfo& di, const D3DMATRIX& world, float radius, Vec3* top)
{
    if (di.upData || di.numVertices == 0 || di.numVertices > 20000 || g_cfg.worldUpAxis != 2) return false;
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT vbOffset = 0, stride = 0;
    if (FAILED(dev->GetStreamSource(0, &vb, &vbOffset, &stride)) || !vb) return false;
    bool found = false;
    D3DVERTEXBUFFER_DESC vd;
    void* ptr = nullptr;
    UINT off = vbOffset + di.firstVertex * stride, size = di.numVertices * stride;
    if (stride >= 12 && SUCCEEDED(vb->GetDesc(&vd)) && off + size <= vd.Size &&
        SUCCEEDED(vb->Lock(off, size, &ptr, D3DLOCK_READONLY | ((vd.Usage & D3DUSAGE_DYNAMIC) ? D3DLOCK_NOOVERWRITE : 0))) && ptr) {
        Vec3 o = Row(world, 3);
        float best = -1e30f;
        for (UINT i = 0; i < di.numVertices; i++) {
            const float* v = (const float*)((const BYTE*)ptr + i * stride);
            Vec3 w = { v[0] * world._11 + v[1] * world._21 + v[2] * world._31 + world._41,
                       v[0] * world._12 + v[1] * world._22 + v[2] * world._32 + world._42,
                       v[0] * world._13 + v[1] * world._23 + v[2] * world._33 + world._43 };
            float dx = w.x - o.x, dy = w.y - o.y;
            if (dx * dx + dy * dy < radius * radius && w.z > best && w.z == w.z) {
                best = w.z;
                *top = w;
                found = true;
            }
        }
        vb->Unlock();
    }
    vb->Release();
    return found;
}

// Size of the stage-0 texture as bound on the device (0x0 if none).
void Stage0Size(IDirect3DDevice9* dev, UINT* w, UINT* h)
{
    *w = *h = 0;
    IDirect3DBaseTexture9* t = nullptr;
    if (FAILED(dev->GetTexture(0, &t)) || !t) return;
    if (t->GetType() == D3DRTYPE_TEXTURE) {
        D3DSURFACE_DESC d;
        if (SUCCEEDED(static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &d))) {
            *w = d.Width;
            *h = d.Height;
        }
    }
    t->Release();
}

// Stage 0 texture as bound on the device (state blocks bypass the SetTexture hook).
bool Stage0IsRenderTarget(IDirect3DDevice9* dev)
{
    IDirect3DBaseTexture9* t = nullptr;
    if (FAILED(dev->GetTexture(0, &t)) || !t) return false;
    bool rt = false;
    if (t->GetType() == D3DRTYPE_TEXTURE) {
        D3DSURFACE_DESC d;
        if (SUCCEEDED(static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &d)))
            rt = (d.Usage & D3DUSAGE_RENDERTARGET) != 0;
    }
    t->Release();
    return rt;
}

const char* MatchGameMatrix(const float* r);

// Frame dump: render target, blending, texture stages and vertex shader matrices of a 3D draw
// (to find effects drawn with the game's camera instead of the eye's).
void DumpDrawDetails(IDirect3DDevice9* dev)
{
    IDirect3DSurface9* rt = nullptr;
    D3DSURFACE_DESC rd = {};
    if (SUCCEEDED(oGetRenderTarget(dev, 0, &rt)) && rt) { rt->GetDesc(&rd); rt->Release(); }
    DWORD ab = 0, sb = 0, db = 0, zw = 0, zf = 0;
    dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &ab);
    dev->GetRenderState(D3DRS_SRCBLEND, &sb);
    dev->GetRenderState(D3DRS_DESTBLEND, &db);
    dev->GetRenderState(D3DRS_ZWRITEENABLE, &zw);
    dev->GetRenderState(D3DRS_ZFUNC, &zf);
    Dump("    rt=%p %ux%u fmt=%d blend=%lu %lu/%lu zwrite=%lu zfunc=%lu stereo=%d",
         rt, rd.Width, rd.Height, rd.Format, ab, sb, db, zw, zf, D.stereoNow);
    for (DWORD s = 0; s < 4; s++) {
        DWORD op = D3DTOP_DISABLE, ci = 0, ff = 0;
        dev->GetTextureStageState(s, D3DTSS_COLOROP, &op);
        if (op == D3DTOP_DISABLE) break;
        dev->GetTextureStageState(s, D3DTSS_TEXCOORDINDEX, &ci);
        dev->GetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, &ff);
        IDirect3DBaseTexture9* t = nullptr;
        D3DSURFACE_DESC td = {};
        int type = 0;
        if (SUCCEEDED(dev->GetTexture(s, &t)) && t) {
            type = t->GetType();
            if (type == D3DRTYPE_TEXTURE) static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &td);
            t->Release();
        }
        Dump("    stage%lu op=%lu tex=%p type=%d %ux%u fmt=%d%s tci=0x%lX ttff=0x%lX", s, op, t, type, td.Width, td.Height,
             td.Format, (td.Usage & D3DUSAGE_RENDERTARGET) ? " RT" : "", ci, ff);
        if ((ff & 0xFF) != D3DTTFF_DISABLE) {
            D3DMATRIX m;
            if (SUCCEEDED(oGetTransform(dev, (D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + s), &m)))
                Dump("      texmatrix %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f",
                     m._11, m._12, m._13, m._14, m._21, m._22, m._23, m._24, m._31, m._32, m._33, m._34, m._41, m._42, m._43, m._44);
        }
    }
    if (D.vsBound) {
        IDirect3DVertexShader9* vs = nullptr;
        dev->GetVertexShader(&vs);
        if (vs) vs->Release();
        // The shader's bytecode, once per shader, for offline disassembly.
        static IDirect3DVertexShader9* saved[64];
        static int savedCount = 0;
        bool known = false;
        for (int i = 0; i < savedCount; i++) known |= saved[i] == vs;
        UINT size = 0;
        if (vs && !known && savedCount < 64 && SUCCEEDED(vs->GetFunction(nullptr, &size)) && size && size < 65536) {
            saved[savedCount++] = vs;
            BYTE* code = new BYTE[size];
            if (SUCCEEDED(vs->GetFunction(code, &size))) {
                wchar_t path[MAX_PATH];
                swprintf_s(path, L"%sSCCT_VR_vs_%p.bin", ModuleDir(), vs);
                FILE* f = _wfsopen(path, L"wb", _SH_DENYWR);
                if (f) { fwrite(code, 1, size, f); fclose(f); }
            }
            delete[] code;
        }
        float c[96 * 4];
        if (SUCCEEDED(dev->GetVertexShaderConstantF(0, c, 96))) {
            Dump("      vs=%p", vs);
            for (int r = 0; r < 96; r++) {
                if (r >= 16 && !c[r * 4] && !c[r * 4 + 1] && !c[r * 4 + 2] && !c[r * 4 + 3]) continue;
                Dump("      c%d = %.4f %.4f %.4f %.4f", r, c[r * 4], c[r * 4 + 1], c[r * 4 + 2], c[r * 4 + 3]);
            }
            for (UINT i = 0; i + 4 <= 32; i++) {
                const char* match = MatchGameMatrix(c + i * 4);
                if (match) Dump("      c%u..c%u = %s (game camera, not corrected)", i, i + 3, match);
            }
        }
    }
}

// Decides where a draw goes: HUD layer, scene, or scene with the weapon moved to the hand.
// Returns true for a first-person weapon draw.
bool RouteDraw(IDirect3DDevice9* dev, const DrawInfo& di)
{
    D.draws++;
    bool is2D = Is2D();
    bool hud = false, weapon = false;
    float cover = -1.0f;
    const char* why = "";

    if (!is2D && D.stereoNow) {
        if (g_cfg.weaponInHand && D.zClears > 0 && D.rtMain) {
            Vec3 t = Row(D.world, 3);
            weapon = Length(t - D.camPos) < g_cfg.weaponRadius;
            if (weapon) vr::NoteWeaponDraw();
        }
        D.draws3D++;
        D.frame3D++;
        if (!D.rtIsBB) D.offscreenScene = true;
        vr::NoteSceneDraw();
    }

    // Only 2D drawn onto the real backbuffer during gameplay can be HUD. Every gameplay frame has
    // the same structure: world offscreen -> composite onto the backbuffer -> full-screen post
    // passes -> depth-only clear -> HUD. (Vertex data of the game's dynamic buffers cannot be read
    // back reliably, so the decision is purely structural.)
    if (is2D && g_cfg.hudLayer && D.rtIsBB && D.offscreenScene && vr::SceneActive()) {
        if (!D.compositeDone) {
            why = "composite";
            D.compositeDone = true;
        } else if (!D.hudStarted && Stage0IsRenderTarget(dev)) {
            // Full-screen post passes sample the rendered scene. Everything else drawn between
            // the composite and the HUD's depth clear (objective markers, distances) is HUD.
            why = "post";
        } else {
            hud = true;
        }
        if (!hud) D.fullscreen2D++;
    }

    // Menus (also the in-game menu): the mouse cursor is the last small pretransformed sprite of
    // the frame. Any later 2D draw means the candidate was not the cursor.
    bool rhw2D = is2D && D.rtIsBB && (D.fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
    if (rhw2D) D.twoDCount++;   // only pretransformed 2D counts: the in-game HUD after it uses another format
    // (Under Wine reading a vertex buffer back stalls the renderer, so with the XR API the cursor
    // is only looked for in menus, not over gameplay.)
    if (rhw2D && (!vr::SceneActive() || (di.numVertices <= 6 && !vr::UsesXrApi())) && di.numVertices <= 1024) {
        float box[4];
        if (ScreenCoverage(dev, di, true, box) >= 0.0f && box[2] >= box[0]) {
            float w = box[2] - box[0], h = box[3] - box[1];
            if (w > 100 && h > 100) {
                D.menuW = fmaxf(D.menuW, box[2]);
                D.menuH = fmaxf(D.menuH, box[3]);
            }
            if (di.numVertices <= 6 && w >= 6 && w <= 48 && h >= 6 && h <= 48) {
                D.cursorX = box[0];
                D.cursorY = box[1];
                D.cursorFound = true;
                D.cursorIdx = D.twoDCount;
            }
        }
    }
    if (hud) {
        D.hudDraws++;
        // The in-game menu is drawn on top of the HUD with the menu textures; its background
        // panel is 512x512, which the normal HUD never uses.
        UINT tw, th;
        Stage0Size(dev, &tw, &th);
        if (tw == 512 && th == 512) D.menuPanelSeen = true;
    }
    if (g_cfg.hudLayer && hud != D.redirected) {
        if (hud) Redirect(dev);
        else Unredirect(dev);
    }
    if (D.redirected) {
        // Coverage (alpha) of the HUD layer: only draws that hide what is behind them add to it.
        // Additive sprites (the merc's crosshair dot on a black square) only add light; counting
        // their square as coverage darkened the world behind it.
        DWORD destBlend = D3DBLEND_INVSRCALPHA, blendOn = FALSE;
        dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blendOn);
        dev->GetRenderState(D3DRS_DESTBLEND, &destBlend);
        bool additive = blendOn && (destBlend == D3DBLEND_ONE || destBlend == D3DBLEND_INVSRCCOLOR);
        dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_SRCBLENDALPHA, additive ? D3DBLEND_ZERO : D3DBLEND_ONE);
        dev->SetRenderState(D3DRS_DESTBLENDALPHA, additive ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
        if (D.dump) {
            DWORD srcBlend = 0;
            dev->GetRenderState(D3DRS_SRCBLEND, &srcBlend);
            Dump("    hud blend=%lu %lu/%lu%s", blendOn, srcBlend, destBlend, additive ? " (additive: no coverage)" : "");
        }
    }

    float box[4] = { 0, 0, 0, 0 };
    if (D.dump && is2D) ScreenCoverage(dev, di, (D.fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW, box);
    if (D.dump && is2D) {
        // Real stage-0 texture (state blocks bypass the SetTexture hook).
        IDirect3DBaseTexture9* t0 = nullptr;
        D3DSURFACE_DESC td = {};
        if (SUCCEEDED(dev->GetTexture(0, &t0)) && t0) {
            if (t0->GetType() == D3DRTYPE_TEXTURE) static_cast<IDirect3DTexture9*>(t0)->GetLevelDesc(0, &td);
            t0->Release();
        }
        Dump("    2D tex0=%p %ux%u", t0, td.Width, td.Height);
    }
    if (D.dump) {
        Vec3 t = Row(D.world, 3);
        Dump("DRAW %-5s prims=%u verts=%u fvf=%03X vs=%d persp=%d bb=%d tex0=%ux%u%s zclr=%d dist=%.0f cover=%.2f box=%.0f,%.0f-%.0f,%.0f -> %s%s",
             di.kind, di.primCount, di.numVertices, D.fvf, D.vsBound, D.haveProj && IsPerspective(D.proj), D.rtIsBB,
             D.tex0W, D.tex0H, D.stage0IsRT ? "RT" : "", D.zClears, Length(t - D.camPos), cover, box[0], box[1], box[2], box[3],
             hud ? "HUD" : weapon ? "WEAPON" : is2D ? "scene-2D " : "scene", why);
        if (!is2D) DumpDrawDetails(dev);
    }
    return weapon;
}

// The stage's texture is a render target of the backbuffer's size: a copy of the screen image
// (shadow maps and projected light textures have their own sizes).
bool StageSamplesScreen(IDirect3DDevice9* dev, DWORD stage)
{
    IDirect3DBaseTexture9* t = nullptr;
    if (FAILED(dev->GetTexture(stage, &t)) || !t) return false;
    bool screen = false;
    if (t->GetType() == D3DRTYPE_TEXTURE) {
        D3DSURFACE_DESC d;
        if (SUCCEEDED(static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &d)))
            screen = (d.Usage & D3DUSAGE_RENDERTARGET) && d.Width == D.bbW && d.Height == D.bbH;
    }
    t->Release();
    return screen;
}

bool IsCameraSpaceTexgen(DWORD ci)
{
    DWORD gen = ci & 0xFFFF0000;
    return gen == D3DTSS_TCI_CAMERASPACEPOSITION || gen == D3DTSS_TCI_CAMERASPACENORMAL ||
           gen == D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR;
}

// Before each draw: give stages with camera-space texgen a texture matrix that first maps the
// eye camera space back to the game's camera space; restore the game's own matrix otherwise.
// Texture stage states are read back from the device: the game (through Reloaded's D3D8 layer)
// sets them with state blocks, which bypass the hooked setters.
// Texture matrices can arrive through state blocks too (e.g. the reflection matrix of glass and
// other environment-mapped surfaces), so the game's matrix is read back from the device: if the
// device still holds what we put there, the game has not changed it since.
void FixCameraSpaceTexgen(IDirect3DDevice9* dev)
{
    if (!g_cfg.fixProjectedLights) return;
    for (int i = 0; i < 8; i++) {
        DWORD op = D3DTOP_DISABLE;
        dev->GetTextureStageState(i, D3DTSS_COLOROP, &op);
        if (op == D3DTOP_DISABLE) break;    // later stages are not used
        DWORD ci = 0, flags = 0;
        bool want = D.stereoNow;
        if (want) {
            dev->GetTextureStageState(i, D3DTSS_TEXCOORDINDEX, &ci);
            dev->GetTextureStageState(i, D3DTSS_TEXTURETRANSFORMFLAGS, &flags);
            want = IsCameraSpaceTexgen(ci) && (flags & 0xFF) != D3DTTFF_DISABLE;
        }
        if (want) {
            D3DMATRIX cur;
            if (SUCCEEDED(oGetTransform(dev, (D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + i), &cur)) &&
                !(D.texFixed[i] && !memcmp(&cur, &D.texApplied[i], sizeof(cur)))) {
                D.texT[i] = cur;            // set by the game (hooked or through a state block)
                D.texTSet[i] = true;
                D.texFixed[i] = false;
            }
        }
        if (!want) {
            if (D.texFixed[i]) {
                // Put the game's matrix back, unless the game has replaced ours in the meantime.
                D3DMATRIX cur;
                if (SUCCEEDED(oGetTransform(dev, (D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + i), &cur)) &&
                    !memcmp(&cur, &D.texApplied[i], sizeof(cur)))
                    oSetTransform(dev, (D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + i), &D.texT[i]);
                D.texFixed[i] = false;
            }
            continue;
        }
        D3DMATRIX m = D.eyeToGame;
        if ((ci & 0xFFFF0000) != D3DTSS_TCI_CAMERASPACEPOSITION)
            m._41 = m._42 = m._43 = 0.0f;   // normals / reflection vectors: rotation only
        else if (D.screenFixValid && StageSamplesScreen(dev, i))
            m = D.screenFix;
        D3DMATRIX t = Mul(m, D.texT[i]);
        oSetTransform(dev, (D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + i), &t);
        D.texApplied[i] = t;
        D.texFixed[i] = true;
        D.texgenFixes++;
    }
}

bool FixWvp(D3DMATRIX& m);
const UINT kVsConsts = 96;      // vs_1_1 constant registers

// Before a vertex shader draw with the eye camera: rewrites every World*View*Proj in the
// shader's constants (as the game put them on the device) for the eye. `orig` receives the
// constants to put back after the draw. False if nothing needed changing.
bool FixShaderConstantsForDraw(IDirect3DDevice9* dev, float* orig)
{
    if (!D.stereoNow || !D.vsBound || !D.vpValid || !g_cfg.fixShaderMatrices) return false;
    if (FAILED(dev->GetVertexShaderConstantF(0, orig, kVsConsts))) return false;
    float buf[kVsConsts * 4];
    memcpy(buf, orig, sizeof(buf));
    unsigned fixes = 0;
    for (UINT i = 0; i + 4 <= kVsConsts; i++) {
        float* r = &buf[i * 4];
        D3DMATRIX m;
        for (int a = 0; a < 4; a++)
            for (int b = 0; b < 4; b++)
                m.m[a][b] = r[b * 4 + a];
        bool fixed = FixWvp(m);
        if (fixed) {
            for (int a = 0; a < 4; a++)
                for (int b = 0; b < 4; b++)
                    r[b * 4 + a] = m.m[a][b];
        } else {
            memcpy(&m, r, sizeof(m));
            fixed = FixWvp(m);
            if (fixed) memcpy(r, &m, sizeof(m));
        }
        if (fixed) {
            if (D.dump) Dump("      c%u..c%u = WVP of the game camera -> corrected for the eye", i, i + 3);
            fixes++;
            i += 3;
        }
    }
    if (!fixes) return false;
    static bool logged = false;
    if (!logged) {
        logged = true;
        Log("Vertex shader draw: %u game-camera matrices corrected in its constants", fixes);
    }
    D.shaderFixes += fixes;
    oSetVSConstF(dev, 0, buf, kVsConsts);
    return true;
}

// FVF and vertex shader can also come from state blocks: read the device's current values.
void RefreshDrawState(IDirect3DDevice9* dev)
{
    DWORD fvf = 0;
    if (SUCCEEDED(dev->GetFVF(&fvf))) D.fvf = fvf;
    IDirect3DVertexShader9* vs = nullptr;
    if (SUCCEEDED(dev->GetVertexShader(&vs))) {
        D.vsBound = vs != nullptr;
        if (vs) vs->Release();
    }
}

// Time spent in the mod's own code (the game's draw calls themselves excluded), for the
// statistics: under emulation (standalone headsets) every hook costs noticeably.
struct HookTimer {
    LARGE_INTEGER start;
    LONGLONG* total;
    explicit HookTimer(LONGLONG* t) : total(t) { QueryPerformanceCounter(&start); }
    ~HookTimer()
    {
        LARGE_INTEGER end;
        QueryPerformanceCounter(&end);
        *total += end.QuadPart - start.QuadPart;
    }
};

template <class F>
HRESULT DoDraw(IDirect3DDevice9* dev, const DrawInfo& di, F drawCall)
{
    HookTimer timer(&D.hookDrawTicks);
    auto draw = [&]() {
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        HRESULT h = drawCall();
        QueryPerformanceCounter(&b);
        D.driverDrawTicks += b.QuadPart - a.QuadPart;
        return h;
    };
    RefreshDrawState(dev);
    if (D.stereoNow && !Is2D()) {
        // Spy: find its model (and the top of its head) and, in first person, do not draw it.
        if (vr::SpyModelCandidate(D.world, di.primCount)) {
            Vec3 top;
            if (HighestVertex(dev, di, D.world, 30.0f, &top)) vr::NoteSpyHead(top);
        }
        if (vr::HideDraw(D.world, di.primCount)) return D3D_OK;
    }
    FixCameraSpaceTexgen(dev);
    bool weapon = RouteDraw(dev, di);
    // Vertex shader draws (the spy's camouflage): the game's matrices reach the shader constants
    // without passing the SetVertexShaderConstantF hook, so they are corrected for this draw only.
    float vsOrig[kVsConsts * 4];
    bool vsFixed = FixShaderConstantsForDraw(dev, vsOrig);
    HRESULT hr;
    D3DMATRIX moved;
    if (weapon && vr::WeaponWorld(D.world, D.view, &moved)) {
        oSetTransform(dev, D3DTS_WORLD, &moved);
        hr = draw();
        oSetTransform(dev, D3DTS_WORLD, &D.world);
        D.weaponDraws++;
    } else {
        hr = draw();
    }
    if (vsFixed) oSetVSConstF(dev, 0, vsOrig, kVsConsts);
    return hr;
}

UINT VertexCount(D3DPRIMITIVETYPE type, UINT prims)
{
    switch (type) {
    case D3DPT_POINTLIST: return prims;
    case D3DPT_LINELIST: return prims * 2;
    case D3DPT_LINESTRIP: return prims + 1;
    case D3DPT_TRIANGLELIST: return prims * 3;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN: return prims + 2;
    default: return 0;
    }
}

void FrameDumpControl()
{
    if (D.dump) {
        fclose(D.dump);
        D.dump = nullptr;
        Log("Frame dump written");
    }
    bool key = (GetAsyncKeyState(VK_PAUSE) & 0x8000) != 0;
    if (key && !D.dumpKeyDown) {
        // Each Pause press gets its own file, so several situations can be captured in one game.
        static wchar_t name[64];
        static int presses = 0;
        swprintf_s(name, L"SCCT_VR_frame_pause%d.txt", ++presses);
        D.dumpName = name;
        D.dumpRequested = true;
        Log("Frame dump requested (Pause): %ls", name);
    }
    D.dumpKeyDown = key;
    // Diagnostics: a dump every DumpEverySec seconds of gameplay, numbered 1..20 and reused.
    static unsigned lastStereoFrames = 0;
    bool gameplayFrame = D.stereoFrames != lastStereoFrames;   // counted just before this call
    lastStereoFrames = D.stereoFrames;
    if (g_cfg.dumpEverySec > 0 && gameplayFrame && !D.dumpRequested) {
        static DWORD last = 0;
        static int n = 0;
        static wchar_t name[64];
        DWORD now = GetTickCount();
        if (now - last >= (DWORD)g_cfg.dumpEverySec * 1000) {
            last = now;
            n = n % 20 + 1;
            swprintf_s(name, L"SCCT_VR_frame_auto%d.txt", n);
            D.dumpName = name;
            D.dumpRequested = true;
            Log("Frame dump %ls", name);
        }
    }
    if (!D.autoDumped && D.stereoFrames == 1500) {
        D.dumpRequested = true;
        D.autoDumped = true;
    }
    if (!D.menuDumped && D.menuFrames == 600) {
        D.dumpRequested = true;
        D.menuDumped = true;
        D.dumpName = L"SCCT_VR_menu.txt";
    }
    if (D.dumpRequested) {
        D.dumpRequested = false;
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s%s", ModuleDir(), D.dumpName);
        D.dumpName = L"SCCT_VR_frame.txt";
        D.dump = _wfsopen(path, L"w", _SH_DENYWR);
        D.dumpLines = 0;
        Dump("Frame dump. backbuffer %ux%u", D.bbW, D.bbH);
    }
}

// Laser beam from the right controller, drawn into the finished eye image.
void DrawLaser(IDirect3DDevice9* dev)
{
    Vec3 q[4];
    D3DMATRIX view, proj;
    D3DCOLOR color;
    if (!D.rtIsBB || !vr::LaserQuad(q, &view, &proj, &color)) return;
    if (!D.laserState && FAILED(dev->CreateStateBlock(D3DSBT_ALL, &D.laserState))) return;
    D.laserState->Capture();

    D3DMATRIX id = Identity();
    oSetTransform(dev, D3DTS_WORLD, &id);
    oSetTransform(dev, D3DTS_VIEW, &view);
    oSetTransform(dev, D3DTS_PROJECTION, &proj);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    oSetTexture(dev, 0, nullptr);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    oSetVertexShader(dev, nullptr);
    dev->SetPixelShader(nullptr);
    oSetFVF(dev, D3DFVF_XYZ | D3DFVF_DIFFUSE);
    D3DVIEWPORT9 vp = { 0, 0, D.bbW, D.bbH, 0.0f, 1.0f };
    dev->SetViewport(&vp);

    struct V { float x, y, z; DWORD c; } v[4];
    for (int i = 0; i < 4; i++) v[i] = { q[i].x, q[i].y, q[i].z, color };
    oDrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof(V));
    D.laserState->Apply();
}

// A texture with premultiplied alpha drawn over `target` as 4 pretransformed vertices
// (x, y, z, rhw, u, v; triangle strip). The device state is restored afterwards.
void DrawPremultipliedQuad(IDirect3DDevice9* dev, IDirect3DSurface9* target, IDirect3DTexture9* tex, float v[4][6])
{
    if (!D.hudState && FAILED(dev->CreateStateBlock(D3DSBT_ALL, &D.hudState))) return;
    IDirect3DSurface9* rt = nullptr;
    D3DSURFACE_DESC td;
    if (FAILED(target->GetDesc(&td))) return;
    oGetRenderTarget(dev, 0, &rt);
    D.hudState->Capture();

    oSetRenderTarget(dev, 0, target);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    oSetTexture(dev, 0, tex);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    oSetVertexShader(dev, nullptr);
    dev->SetPixelShader(nullptr);
    oSetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
    D3DVIEWPORT9 vp = { 0, 0, td.Width, td.Height, 0.0f, 1.0f };
    dev->SetViewport(&vp);
    oDrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof(v[0]));

    // Render target first: setting it resets the viewport, which the state block restores.
    if (rt) { oSetRenderTarget(dev, 0, rt); rt->Release(); }
    D.hudState->Apply();
}

// XR API: the HUD texture drawn as a panel into the finished eye image (premultiplied alpha).
void DrawHudOverlay(IDirect3DDevice9* dev)
{
    IDirect3DTexture9* tex = nullptr;
    float v[4][6];
    if (!vr::HudOverlay(&tex, v)) return;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    DrawPremultipliedQuad(dev, bb, tex, v);
    bb->Release();
}

// PC desktop window: during gameplay the backbuffer alternates between the eyes. For people
// watching or streaming, the window shows the right eye only (cropped to the window's
// proportions around where the eye looks): its image is kept, and put back into the backbuffer
// on left-eye frames. Runs after the eye image went to the headset, so only the window changes.
void UpdateDesktopMirror(IDirect3DDevice9* dev, const vr::MirrorFrame& mf)
{
    if (!D.bbW || !D.bbH) return;
    if (!D.mirrorTex && FAILED(dev->CreateTexture(D.bbW, D.bbH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                                  D3DPOOL_DEFAULT, &D.mirrorTex, nullptr))) return;
    IDirect3DSurface9 *bb = nullptr, *mirror = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    D.mirrorTex->GetSurfaceLevel(0, &mirror);
    if (mirror && mf.eye == 1) {
        // Largest rectangle with the window's proportions centred on the eye's forward direction
        float aspect = D.bbW / (float)D.bbH;
        float hx = fminf(fminf(mf.left, mf.right), fminf(mf.up, mf.down) * aspect);
        float hy = hx / aspect;
        float w = mf.left + mf.right, h = mf.up + mf.down;
        RECT src = { (LONG)((mf.left - hx) / w * D.bbW), (LONG)((mf.up - hy) / h * D.bbH),
                     (LONG)((mf.left + hx) / w * D.bbW), (LONG)((mf.up + hy) / h * D.bbH) };
        if (SUCCEEDED(dev->StretchRect(bb, &src, mirror, nullptr, D3DTEXF_LINEAR))) {
            D.mirrorValid = true;
            // The HUD (it goes to its own VR layer, so the eye image has none) over the whole window.
            // Taken only now, after the frame went to the headset: the texture may have been
            // recreated meanwhile.
            IDirect3DTexture9* hud = mf.hud ? vr::HudTexture() : nullptr;
            if (hud) {
                float fw = (float)D.bbW, fh = (float)D.bbH;
                float v[4][6] = { { -0.5f, -0.5f, 0, 1, 0, 0 }, { fw - 0.5f, -0.5f, 0, 1, 1, 0 },
                                  { -0.5f, fh - 0.5f, 0, 1, 0, 1 }, { fw - 0.5f, fh - 0.5f, 0, 1, 1, 1 } };
                DrawPremultipliedQuad(dev, mirror, hud, v);
                hud->Release();
            }
        }
    }
    if (mirror && D.mirrorValid) dev->StretchRect(mirror, nullptr, bb, nullptr, D3DTEXF_NONE);
    if (mirror) mirror->Release();
    bb->Release();
}

void BeforePresent(IDirect3DDevice9* dev)
{
    if (D.inPresent) return;
    D.inPresent = true;
    D.frames++;
    PrintStats();
    Unredirect(dev);
    DrawLaser(dev);
    DrawHudOverlay(dev);
    vr::NoteMenuCursor(D.cursorFound && D.cursorIdx == D.twoDCount, D.cursorX, D.cursorY, D.menuW, D.menuH);
    vr::NoteInGameMenu(D.menuPanelSeen);
    D.menuPanelSeen = false;
    D.cursorFound = false;
    D.twoDCount = D.cursorIdx = 0;
    D.menuW = D.menuH = 0;
    vr::MirrorFrame mirror;
    bool mirrorFrame = g_cfg.desktopMirror && vr::MirrorFrameInfo(&mirror);
    vr::OnPresent(dev);
    if (mirrorFrame) UpdateDesktopMirror(dev, mirror);
    else D.mirrorValid = false;
    D.hudCleared = false;
    D.zClears = 0;
    D.draws3D = 0;
    D.offscreenScene = false;
    D.compositeDone = false;
    D.hudStarted = false;
    if (D.frame3D >= 50) D.stereoFrames++;
    else if (D.frame3D < 10 && vr::Available()) D.menuFrames++;
    D.frame3D = 0;
    D.frameStereo = false;
    FrameDumpControl();
    // The next frame may not set its camera again, so push the new eye's camera now.
    Apply(dev);
    D.inPresent = false;
}

HRESULT STDMETHODCALLTYPE hkGetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9** out)
{
    if (index == 0 && D.redirected && out) {
        *out = D.savedRT;
        D.savedRT->AddRef();
        return D3D_OK;
    }
    return oGetRenderTarget(dev, index, out);
}

HRESULT STDMETHODCALLTYPE hkClear(IDirect3DDevice9* dev, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
    Unredirect(dev);
    if (D.dump) {
        D3DVIEWPORT9 vp;
        dev->GetViewport(&vp);
        Dump("CLEAR flags=%X rects=%u main=%d bb=%d vp=%u,%u %ux%u", flags, count, D.rtMain, D.rtIsBB, vp.X, vp.Y, vp.Width, vp.Height);
    }
    if (D.rtMain && D.rtIsBB) {
        if ((flags & D3DCLEAR_ZBUFFER) && !(flags & D3DCLEAR_TARGET) && D.compositeDone) D.hudStarted = true;
    } else if (D.rtMain) {
        if (flags & D3DCLEAR_TARGET) D.zClears = D.draws3D = 0;
        else if ((flags & D3DCLEAR_ZBUFFER) && D.draws3D > 0) D.zClears++;

        // Start of the offscreen world: clear the whole target, otherwise areas the game does not
        // draw (outside its own camera's view, visible when turning the head) keep old frames.
        if (D.draws3D == 0 && !count && vr::SceneActive()) {
            D3DVIEWPORT9 vp, full = { 0, 0, D.bbW, D.bbH, 0.0f, 1.0f };
            dev->GetViewport(&vp);
            dev->SetViewport(&full);
            HRESULT hr = oClear(dev, 0, nullptr, flags | D3DCLEAR_TARGET, (flags & D3DCLEAR_TARGET) ? color : 0, z, stencil);
            dev->SetViewport(&vp);
            return hr;
        }
    }
    return oClear(dev, count, rects, flags, color, z, stencil);
}

HRESULT STDMETHODCALLTYPE hkSetTexture(IDirect3DDevice9* dev, DWORD stage, IDirect3DBaseTexture9* tex)
{
    if (D.dump) {
        D3DSURFACE_DESC d = {};
        if (tex && tex->GetType() == D3DRTYPE_TEXTURE) static_cast<IDirect3DTexture9*>(tex)->GetLevelDesc(0, &d);
        Dump("TEX s%lu %p %ux%u fmt=%d%s%s", stage, tex, d.Width, d.Height, d.Format,
             (d.Usage & D3DUSAGE_RENDERTARGET) ? " RT" : "", (d.Usage & D3DUSAGE_DEPTHSTENCIL) ? " DEPTH" : "");
    }
    if (stage == 0) {
        D.stage0IsRT = false;
        D.tex0W = D.tex0H = 0;
        if (tex && tex->GetType() == D3DRTYPE_TEXTURE) {
            D3DSURFACE_DESC d;
            if (SUCCEEDED(static_cast<IDirect3DTexture9*>(tex)->GetLevelDesc(0, &d))) {
                D.stage0IsRT = (d.Usage & D3DUSAGE_RENDERTARGET) != 0;
                D.tex0W = d.Width;
                D.tex0H = d.Height;
            }
        }
    }
    return oSetTexture(dev, stage, tex);
}

HRESULT STDMETHODCALLTYPE hkSetTSS(IDirect3DDevice9* dev, DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
    if (stage < 8) {
        if (type == D3DTSS_TEXCOORDINDEX) {
            D.texCI[stage] = value;
            if (D.dump) Dump("STAGE%lu texcoordindex 0x%lX", stage, value);
        } else if (type == D3DTSS_TEXTURETRANSFORMFLAGS) {
            D.texFlags[stage] = value;
        }
    }
    return oSetTSS(dev, stage, type, value);
}

HRESULT STDMETHODCALLTYPE hkSetFVF(IDirect3DDevice9* dev, DWORD fvf)
{
    D.fvf = fvf;
    if (D.dump) Dump("FVF %03lX", fvf);
    return oSetFVF(dev, fvf);
}

HRESULT STDMETHODCALLTYPE hkSetVertexShader(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs)
{
    D.vsBound = vs != nullptr;
    if (D.dump) Dump("VSHADER %p", vs);
    return oSetVertexShader(dev, vs);
}

HRESULT STDMETHODCALLTYPE hkDrawPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT start, UINT count)
{
    DrawInfo di = { "DP", type, count, start, VertexCount(type, count), nullptr, 0, false };
    return DoDraw(dev, di, [&] { return oDrawPrimitive(dev, type, start, count); });
}

HRESULT STDMETHODCALLTYPE hkDrawIndexedPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, INT base, UINT minIdx, UINT num, UINT start, UINT count)
{
    DrawInfo di = { "DIP", type, count, (UINT)(base + (INT)minIdx), num, nullptr, 0, true };
    return DoDraw(dev, di, [&] { return oDrawIndexedPrimitive(dev, type, base, minIdx, num, start, count); });
}

HRESULT STDMETHODCALLTYPE hkDrawPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride)
{
    DrawInfo di = { "DPUP", type, count, 0, VertexCount(type, count), data, stride, false };
    return DoDraw(dev, di, [&] { return oDrawPrimitiveUP(dev, type, count, data, stride); });
}

HRESULT STDMETHODCALLTYPE hkDrawIndexedPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT minIdx, UINT num, UINT count,
                                                   const void* idx, D3DFORMAT fmt, const void* data, UINT stride)
{
    DrawInfo di = { "DIPUP", type, count, minIdx, num, data, stride, true };
    return DoDraw(dev, di, [&] { return oDrawIndexedPrimitiveUP(dev, type, minIdx, num, count, idx, fmt, data, stride); });
}

HRESULT STDMETHODCALLTYPE hkPresent(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty)
{
    BeforePresent(dev);
    bool outer = !D.inPresent;
    D.inPresent = true;
    HRESULT hr = oPresent(dev, src, dst, wnd, dirty);
    if (outer) D.inPresent = false;
    return hr;
}

HRESULT STDMETHODCALLTYPE hkSCPresent(IDirect3DSwapChain9* sc, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty, DWORD flags)
{
    if (!D.inPresent && D.dev) BeforePresent(D.dev);
    bool outer = !D.inPresent;
    D.inPresent = true;
    HRESULT hr = oSCPresent(sc, src, dst, wnd, dirty, flags);
    if (outer) D.inPresent = false;
    return hr;
}

HRESULT STDMETHODCALLTYPE hkReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
{
    Log("Device Reset %ux%u", pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0);
    Unredirect(dev);
    if (D.laserState) { D.laserState->Release(); D.laserState = nullptr; }
    if (D.hudState) { D.hudState->Release(); D.hudState = nullptr; }
    if (D.mirrorTex) { D.mirrorTex->Release(); D.mirrorTex = nullptr; }
    D.mirrorValid = false;
    vr::OnPreReset();
    HRESULT hr = oReset(dev, pp);
    if (SUCCEEDED(hr)) {
        UpdateBackbufferSize(dev);
        vr::OnPostReset(dev);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hkSetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* surf)
{
    if (index == 0 && D.redirected) {
        // The game picks a new target itself; our redirection ends here.
        D.savedRT->Release();
        D.savedRT = nullptr;
        D.redirected = false;
        dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    }
    HRESULT hr = oSetRenderTarget(dev, index, surf);
    if (index == 0 && surf && SUCCEEDED(hr)) {
        D3DSURFACE_DESC d;
        bool main = SUCCEEDED(surf->GetDesc(&d)) && d.Width == D.bbW && d.Height == D.bbH;
        D.rtIsBB = surf == D.bbSurf;
        if (D.dump) Dump("SETRT %ux%u usage=%X fmt=%d main=%d backbuffer=%d", d.Width, d.Height, d.Usage, d.Format, main, D.rtIsBB);
        if (main != D.rtMain) {
            D.rtMain = main;
            Apply(dev);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hkSetTransform(IDirect3DDevice9* dev, D3DTRANSFORMSTATETYPE state, const D3DMATRIX* m)
{
    HookTimer timer(&D.hookTransformTicks);
    if (m && state == D3DTS_WORLD) D.world = *m;
    if (m && state >= D3DTS_TEXTURE0 && state <= D3DTS_TEXTURE7) {
        int i = state - D3DTS_TEXTURE0;
        D.texT[i] = *m;
        D.texTSet[i] = true;
        D.texFixed[i] = false;
        if (D.dump) Dump("TEXTURE%d matrix set", i);
    }
    if (!m || (state != D3DTS_VIEW && state != D3DTS_PROJECTION)) return oSetTransform(dev, state, m);
    if (state == D3DTS_VIEW) {
        D.view = *m; D.haveView = true; D.viewSets++;
        D3DMATRIX cam;
        if (Inverse(*m, &cam)) D.camPos = Row(cam, 3);
        if (D.dump) Dump("VIEW cam=(%.0f %.0f %.0f)", D.camPos.x, D.camPos.y, D.camPos.z);
    } else {
        if (D.dump && IsPerspective(*m))
            Dump("PROJ persp hfov=%.1f vfov=%.1f", 2 * atanf(m->_34 / m->_11) * 57.2958f, 2 * atanf(m->_34 / m->_22) * 57.2958f);
        else if (D.dump)
            Dump("PROJ ortho");
        D.proj = *m; D.haveProj = true; D.projSets++;
        if (!D.loggedProj && IsPerspective(*m)) {
            D.loggedProj = true;
            Log("First perspective projection: hfov %.1f deg, vfov %.1f deg, _33=%f _43=%f _34=%f",
                2 * atanf(1.0f / m->_11) * 57.2958f, 2 * atanf(1.0f / m->_22) * 57.2958f, m->_33, m->_43, m->_34);
        }
    }
    Apply(dev);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE hkGetTransform(IDirect3DDevice9* dev, D3DTRANSFORMSTATETYPE state, D3DMATRIX* m)
{
    if (D.dump) Dump("GETTRANSFORM %d", (int)state);
    if (m && state == D3DTS_VIEW && D.haveView) { *m = D.view; return D3D_OK; }
    if (m && state == D3DTS_PROJECTION && D.haveProj) { *m = D.proj; return D3D_OK; }
    if (m && state >= D3DTS_TEXTURE0 && state <= D3DTS_TEXTURE7 && D.texTSet[state - D3DTS_TEXTURE0]) {
        *m = D.texT[state - D3DTS_TEXTURE0];
        return D3D_OK;
    }
    return oGetTransform(dev, state, m);
}

HRESULT STDMETHODCALLTYPE hkMultiplyTransform(IDirect3DDevice9* dev, D3DTRANSFORMSTATETYPE state, const D3DMATRIX* m)
{
    if (m && state == D3DTS_VIEW && D.haveView) return hkSetTransform(dev, state, &(D.view = Mul(*m, D.view)));
    if (m && state == D3DTS_PROJECTION && D.haveProj) return hkSetTransform(dev, state, &(D.proj = Mul(*m, D.proj)));
    return oMultiplyTransform(dev, state, m);
}

// Frame dump helper: which of the game's camera matrices a 4-register block equals.
const char* MatchGameMatrix(const float* r)
{
    static char out[96];
    D3DMATRIX m;
    memcpy(&m, r, sizeof(m));
    D3DMATRIX vp = Mul(D.view, D.proj), wv = Mul(D.world, D.view), wvp = Mul(wv, D.proj);
    D3DMATRIX iv = Identity(), ivp = Identity(), ip = Identity();
    Inverse(D.view, &iv);
    Inverse(vp, &ivp);
    Inverse(D.proj, &ip);
    struct Cand { const char* name; D3DMATRIX v; } c[] = {
        { "View", D.view }, { "Proj", D.proj }, { "ViewProj", vp }, { "World", D.world },
        { "WorldView", wv }, { "WVP", wvp }, { "InvView", iv }, { "InvViewProj", ivp }, { "InvProj", ip },
    };
    for (int t = 0; t < 2; t++) {
        D3DMATRIX a = t ? Transpose(m) : m;
        for (auto& cand : c) {
            float scale = 1.0f, diff = 0.0f;
            for (int i = 0; i < 16; i++) {
                scale = fmaxf(scale, fabsf((&cand.v._11)[i]));
                diff = fmaxf(diff, fabsf((&a._11)[i] - (&cand.v._11)[i]));
            }
            if (diff <= 1e-3f * scale) {
                sprintf_s(out, "%s%s", cand.name, t ? "^T" : "");
                return out;
            }
        }
    }
    return nullptr;
}

void DumpConstants(const char* kind, UINT start, const float* data, UINT count)
{
    if (!D.dump || !data) return;
    Dump("%s c%u x%u: %.3f %.3f %.3f %.3f", kind, start, count, data[0], data[1], data[2], data[3]);
    for (UINT i = 0; i + 4 <= count; i++) {
        const char* match = MatchGameMatrix(data + i * 4);
        if (match) Dump("    c%u..c%u = %s", start + i, start + i + 3, match);
    }
}

HRESULT STDMETHODCALLTYPE hkSetPSConstF(IDirect3DDevice9* dev, UINT start, const float* data, UINT count)
{
    D.psUploads++;
    DumpConstants("PSCONST", start, data, count);
    return oSetPSConstF(dev, start, data, count);
}

HRESULT STDMETHODCALLTYPE hkSetVDecl(IDirect3DDevice9* dev, IDirect3DVertexDeclaration9* decl)
{
    if (D.dump) Dump("VDECL %p", decl);
    return oSetVDecl(dev, decl);
}

HRESULT STDMETHODCALLTYPE hkSetPS(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps)
{
    if (D.dump) Dump("PSHADER %p", ps);
    return oSetPS(dev, ps);
}

HRESULT STDMETHODCALLTYPE hkSetVSConstF(IDirect3DDevice9* dev, UINT start, const float* data, UINT count)
{
    DumpConstants("VSCONST", start, data, count);
    D.shaderUploads++;
    if (!D.stereoNow || !D.vpValid || !g_cfg.fixShaderMatrices || !data || count < 4 || count > 256)
        return oSetVSConstF(dev, start, data, count);

    float buf[256 * 4];
    memcpy(buf, data, count * 4 * sizeof(float));
    bool changed = false;
    for (UINT i = 0; i + 4 <= count; i += 4) {
        float* r = &buf[i * 4];
        // Shaders usually read the matrix transposed (one column per register).
        D3DMATRIX m;
        for (int a = 0; a < 4; a++)
            for (int b = 0; b < 4; b++)
                m.m[a][b] = r[b * 4 + a];
        if (FixWvp(m)) {
            for (int a = 0; a < 4; a++)
                for (int b = 0; b < 4; b++)
                    r[b * 4 + a] = m.m[a][b];
            changed = true;
            D.shaderFixes++;
            continue;
        }
        memcpy(&m, r, sizeof(m));
        if (FixWvp(m)) {
            memcpy(r, &m, sizeof(m));
            changed = true;
            D.shaderFixes++;
        }
    }
    return oSetVSConstF(dev, start, changed ? buf : data, count);
}

void HookDevice(IDirect3DDevice9* dev)
{
    D = DevState();
    D.dev = dev;
    UpdateBackbufferSize(dev);
    Patch(dev, kDev_Reset, (Reset_t)hkReset, &oReset);
    Patch(dev, kDev_Present, (Present_t)hkPresent, &oPresent);
    Patch(dev, kDev_SetRenderTarget, (SetRenderTarget_t)hkSetRenderTarget, &oSetRenderTarget);
    Patch(dev, kDev_SetTransform, (SetTransform_t)hkSetTransform, &oSetTransform);
    Patch(dev, kDev_GetTransform, (GetTransform_t)hkGetTransform, &oGetTransform);
    Patch(dev, kDev_MultiplyTransform, (SetTransform_t)hkMultiplyTransform, &oMultiplyTransform);
    Patch(dev, kDev_SetVertexShaderConstantF, (SetVSConstF_t)hkSetVSConstF, &oSetVSConstF);
    Patch(dev, kDev_GetRenderTarget, (GetRenderTarget_t)hkGetRenderTarget, &oGetRenderTarget);
    Patch(dev, kDev_Clear, (Clear_t)hkClear, &oClear);
    Patch(dev, kDev_SetTexture, (SetTexture_t)hkSetTexture, &oSetTexture);
    Patch(dev, kDev_DrawPrimitive, (DrawPrimitive_t)hkDrawPrimitive, &oDrawPrimitive);
    Patch(dev, kDev_DrawIndexedPrimitive, (DrawIndexedPrimitive_t)hkDrawIndexedPrimitive, &oDrawIndexedPrimitive);
    Patch(dev, kDev_DrawPrimitiveUP, (DrawPrimitiveUP_t)hkDrawPrimitiveUP, &oDrawPrimitiveUP);
    Patch(dev, kDev_DrawIndexedPrimitiveUP, (DrawIndexedPrimitiveUP_t)hkDrawIndexedPrimitiveUP, &oDrawIndexedPrimitiveUP);
    Patch(dev, kDev_SetFVF, (SetFVF_t)hkSetFVF, &oSetFVF);
    Patch(dev, kDev_SetVertexShader, (SetVertexShader_t)hkSetVertexShader, &oSetVertexShader);
    Patch(dev, kDev_SetTextureStageState, (SetTSS_t)hkSetTSS, &oSetTSS);
    Patch(dev, kDev_SetVertexDeclaration, (SetVDecl_t)hkSetVDecl, &oSetVDecl);
    Patch(dev, kDev_SetPixelShader, (SetPS_t)hkSetPS, &oSetPS);
    Patch(dev, kDev_SetPixelShaderConstantF, (SetPSConstF_t)hkSetPSConstF, &oSetPSConstF);

    IDirect3DSwapChain9* sc = nullptr;
    if (SUCCEEDED(dev->GetSwapChain(0, &sc))) {
        Patch(sc, kSC_Present, (SCPresent_t)hkSCPresent, &oSCPresent);
        sc->Release();
    }
    Log("Device hooks installed (backbuffer %ux%u)", D.bbW, D.bbH);
}

HRESULT STDMETHODCALLTYPE hkCreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND wnd, DWORD flags,
                                         D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out)
{
    HRESULT hr = oCreateDevice(d3d, adapter, type, wnd, flags, pp, out);
    Log("CreateDevice(adapter %u, %ux%u, windowed %d, msaa %d) -> 0x%08X", adapter,
        pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0, pp ? pp->Windowed : -1,
        pp ? (int)pp->MultiSampleType : -1, hr);
    if (SUCCEEDED(hr) && out && *out) {
        HookDevice(*out);
        vr::OnDeviceCreated(*out, pp && pp->hDeviceWindow ? pp->hDeviceWindow : wnd);
    }
    return hr;
}

} // namespace

void HookDirect3D9(IDirect3D9* d3d)
{
    Patch(d3d, kD3D9_CreateDevice, (CreateDevice_t)hkCreateDevice, &oCreateDevice);
}
