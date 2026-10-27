// Minimal D3D9 fixed-function app that loads ..\bin\d3d9.dll like Reloaded does and renders
// a field of cubes through SetTransform, to exercise the proxy without the game.
// Usage: testapp.exe [seconds]
#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>

struct Vtx { float x, y, z; DWORD c; };

static D3DMATRIX Mat(float a11, float a12, float a13, float a14, float a21, float a22, float a23, float a24,
                     float a31, float a32, float a33, float a34, float a41, float a42, float a43, float a44)
{
    D3DMATRIX m = { a11, a12, a13, a14, a21, a22, a23, a24, a31, a32, a33, a34, a41, a42, a43, a44 };
    return m;
}

int main(int argc, char** argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 10;
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wcsrchr(path, L'\\')[1] = 0;
    wcscat_s(path, L"d3d9.dll");
    HMODULE d3d9 = LoadLibraryW(path);
    if (!d3d9) { printf("cannot load %ls\n", path); return 1; }
    auto create = (IDirect3D9*(WINAPI*)(UINT))GetProcAddress(d3d9, "Direct3DCreate9");

    WNDCLASSW wc = { 0, DefWindowProcW, 0, 0, GetModuleHandleW(nullptr), 0, 0, 0, 0, L"vrtest" };
    RegisterClassW(&wc);
    HWND wnd = CreateWindowW(L"vrtest", L"SCCT VR test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 1280, 720, 0, 0, 0, 0);

    IDirect3D9* d3d = create(D3D_SDK_VERSION);
    if (!d3d) { printf("Direct3DCreate9 failed\n"); return 1; }
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = 1280;
    pp.BackBufferHeight = 720;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(0, D3DDEVTYPE_HAL, wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) { printf("CreateDevice failed 0x%08X\n", hr); return 1; }

    const Vtx cube[] = {
        {-1,-1,-1,0xffff0000},{1,-1,-1,0xff00ff00},{1,1,-1,0xff0000ff},{-1,1,-1,0xffffff00},
        {-1,-1, 1,0xffff00ff},{1,-1, 1,0xff00ffff},{1,1, 1,0xffffffff},{-1,1, 1,0xff808080},
    };
    const WORD idx[] = { 0,2,1, 0,3,2, 4,5,6, 4,6,7, 0,1,5, 0,5,4, 2,3,7, 2,7,6, 1,2,6, 1,6,5, 0,4,7, 0,7,3 };

    // Unreal-style Z-up world, camera at eye height looking down +X.
    float n = 10.0f, f = 50000.0f, hfov = 90.0f * 3.14159f / 180.0f;
    float sx = 1.0f / tanf(hfov / 2), sy = sx * 1280.0f / 720.0f;
    D3DMATRIX proj = Mat(sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, f / (f - n), 1, 0, 0, -n * f / (f - n), 0);
    // world (x fwd, y right, z up) -> view (x right, y up, z fwd), camera at z=170
    D3DMATRIX view = Mat(0, 0, 1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -170, 0, 1);

    DWORD start = GetTickCount();
    int frames = 0;
    MSG msg;
    while (GetTickCount() - start < (DWORD)seconds * 1000) {
        while (PeekMessageW(&msg, 0, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff203040, 1.0f, 0);
        dev->BeginScene();
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
        dev->SetTransform(D3DTS_PROJECTION, &proj);
        dev->SetTransform(D3DTS_VIEW, &view);
        for (int i = -5; i <= 5; i++)
            for (int j = 1; j <= 8; j++) {
                float s = 20.0f;
                D3DMATRIX w = Mat(s, 0, 0, 0, 0, s, 0, 0, 0, 0, s, 0, j * 150.0f, i * 150.0f, 100.0f + 60.0f * sinf(i + j + frames * 0.01f), 1);
                dev->SetTransform(D3DTS_WORLD, &w);
                dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 8, 12, idx, D3DFMT_INDEX16, cube, sizeof(Vtx));
            }
        // 2D HUD element (pretransformed, alpha blended) - goes to the VR HUD layer
        struct V2 { float x, y, z, rhw; DWORD c; };
        const V2 hud[] = { { 560, 330, 0, 1, 0xc0ff8000 }, { 720, 330, 0, 1, 0xc0ff8000 }, { 560, 390, 0, 1, 0xc0ff8000 }, { 720, 390, 0, 1, 0xc0ff8000 } };
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, hud, sizeof(V2));
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        dev->EndScene();
        dev->Present(nullptr, nullptr, nullptr, nullptr);
        frames++;
    }
    printf("rendered %d frames in %d s\n", frames, seconds);
    dev->Release();
    d3d->Release();
    return 0;
}
