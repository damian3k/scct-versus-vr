// vorbisfile.dll proxy for running SCCT Versus under Wine (WinlatorXR).
// The Reloaded launcher injects Reloaded.Core.dll into a suspended game process, which fails
// under Wine + FEX. The game loads vorbisfile.dll from its own folder at startup, so this proxy
// forwards every export to vorbisfile_orig.dll and loads Reloaded.Core.dll at the same point
// in startup where the launcher's injection would have run (before the game's entry point).
#include <windows.h>
#include <intrin.h>
#include <psapi.h>
#include <d3d9.h>
#include <cstdarg>
#include <cstdio>
#include <cwchar>

// Every export is forwarded to the original library (ordinals kept).
#pragma comment(linker, "/export:ov_bitrate=vorbisfile_orig.ov_bitrate,@1")
#pragma comment(linker, "/export:ov_bitrate_instant=vorbisfile_orig.ov_bitrate_instant,@2")
#pragma comment(linker, "/export:ov_clear=vorbisfile_orig.ov_clear,@3")
#pragma comment(linker, "/export:ov_comment=vorbisfile_orig.ov_comment,@4")
#pragma comment(linker, "/export:ov_info=vorbisfile_orig.ov_info,@5")
#pragma comment(linker, "/export:ov_open=vorbisfile_orig.ov_open,@6")
#pragma comment(linker, "/export:ov_open_callbacks=vorbisfile_orig.ov_open_callbacks,@7")
#pragma comment(linker, "/export:ov_pcm_seek=vorbisfile_orig.ov_pcm_seek,@8")
#pragma comment(linker, "/export:ov_pcm_seek_page=vorbisfile_orig.ov_pcm_seek_page,@9")
#pragma comment(linker, "/export:ov_pcm_tell=vorbisfile_orig.ov_pcm_tell,@10")
#pragma comment(linker, "/export:ov_pcm_total=vorbisfile_orig.ov_pcm_total,@11")
#pragma comment(linker, "/export:ov_raw_seek=vorbisfile_orig.ov_raw_seek,@12")
#pragma comment(linker, "/export:ov_raw_tell=vorbisfile_orig.ov_raw_tell,@13")
#pragma comment(linker, "/export:ov_raw_total=vorbisfile_orig.ov_raw_total,@14")
#pragma comment(linker, "/export:ov_read=vorbisfile_orig.ov_read,@15")
#pragma comment(linker, "/export:ov_seekable=vorbisfile_orig.ov_seekable,@16")
#pragma comment(linker, "/export:ov_serialnumber=vorbisfile_orig.ov_serialnumber,@17")
#pragma comment(linker, "/export:ov_streams=vorbisfile_orig.ov_streams,@18")
#pragma comment(linker, "/export:ov_test=vorbisfile_orig.ov_test,@19")
#pragma comment(linker, "/export:ov_test_callbacks=vorbisfile_orig.ov_test_callbacks,@20")
#pragma comment(linker, "/export:ov_test_open=vorbisfile_orig.ov_test_open,@21")
#pragma comment(linker, "/export:ov_time_seek=vorbisfile_orig.ov_time_seek,@22")
#pragma comment(linker, "/export:ov_time_seek_page=vorbisfile_orig.ov_time_seek_page,@23")
#pragma comment(linker, "/export:ov_time_tell=vorbisfile_orig.ov_time_tell,@24")
#pragma comment(linker, "/export:ov_time_total=vorbisfile_orig.ov_time_total,@25")

// ---------------------------------------------------------------------------------------------
// Diagnostics for the "game runs and plays sound but nothing is shown" case: Reloaded.Core
// imports d3d9!Direct3DCreate9, so its import is redirected here to log the device creation,
// how many frames are presented, the game window's state, and to save two backbuffer
// screenshots (SCCT_Quest_frameN.bmp) showing whether the game renders at all.

static wchar_t g_dir[MAX_PATH];     // game System folder, with trailing backslash
static HMODULE g_self;

static void Log(const char* fmt, ...)
{
    wchar_t logPath[MAX_PATH];
    swprintf_s(logPath, L"%sSCCT_Quest.log", g_dir);
    HANDLE f = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[1024];
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    n += _vsnprintf_s(line + n, sizeof(line) - n, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < (int)sizeof(line) - 2) { line[n++] = '\r'; line[n++] = '\n'; }
    DWORD written;
    WriteFile(f, line, n, &written, nullptr);
    CloseHandle(f);
}

template <class T>
static void PatchSlot(void* obj, int slot, T hook, T* orig)
{
    void** vt = *(void***)obj;
    if (vt[slot] == (void*)hook) return;
    DWORD old;
    VirtualProtect(&vt[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    *orig = (T)vt[slot];
    vt[slot] = (void*)hook;
    VirtualProtect(&vt[slot], sizeof(void*), old, &old);
}

static bool ModuleContains(HMODULE m, const char* text)
{
    auto* dos = (IMAGE_DOS_HEADER*)m;
    auto* nt = (IMAGE_NT_HEADERS*)((BYTE*)m + dos->e_lfanew);
    const BYTE* p = (const BYTE*)m;
    size_t size = nt->OptionalHeader.SizeOfImage, len = strlen(text);
    MEMORY_BASIC_INFORMATION mbi;
    for (size_t off = 0; off + len < size;) {
        if (!VirtualQuery(p + off, &mbi, sizeof(mbi))) break;
        size_t end = (size_t)((BYTE*)mbi.BaseAddress + mbi.RegionSize - p);
        if (end > size) end = size;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            for (size_t i = off; i + len < end; i++)
                if (p[i] == (BYTE)text[0] && !_strnicmp((const char*)p + i, text, len)) return true;
        off = end;
    }
    return false;
}

static HWND g_wnd;
static unsigned g_presents, g_presentsTotal, g_endScenes;
static bool g_screenshots;          // SCCT_Quest.screenshots exists next to the game
static DWORD g_statTick;

static void LogWindow(const char* when)
{
    HWND w = g_wnd;
    RECT r = {}, c = {};
    if (w) { GetWindowRect(w, &r); GetClientRect(w, &c); }
    Log("%s: window %p exists %d visible %d minimized %d foreground %d rect %ld,%ld-%ld,%ld client %ldx%ld style %08lX exstyle %08lX, screen %dx%d",
        when, w, w && IsWindow(w), w && IsWindowVisible(w), w && IsIconic(w), w && GetForegroundWindow() == w,
        r.left, r.top, r.right, r.bottom, c.right, c.bottom, w ? (DWORD)GetWindowLongW(w, GWL_STYLE) : 0,
        w ? (DWORD)GetWindowLongW(w, GWL_EXSTYLE) : 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
}

static void SaveBackbuffer(IDirect3DDevice9* dev, unsigned frame)
{
    IDirect3DSurface9 *bb = nullptr, *sys = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) { Log("screenshot: GetBackBuffer failed"); return; }
    D3DSURFACE_DESC d;
    bb->GetDesc(&d);
    HRESULT hr = dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    if (SUCCEEDED(hr)) hr = dev->GetRenderTargetData(bb, sys);
    D3DLOCKED_RECT lr = {};
    if (SUCCEEDED(hr)) hr = sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);
    if (FAILED(hr)) {
        Log("screenshot at frame %u failed: 0x%08lX (format %d %ux%u)", frame, hr, d.Format, d.Width, d.Height);
    } else if (d.Format != D3DFMT_X8R8G8B8 && d.Format != D3DFMT_A8R8G8B8) {
        Log("screenshot at frame %u: backbuffer format %d not saved", frame, d.Format);
        sys->UnlockRect();
    } else {
        // Average brightness tells "black" from "rendered" even without looking at the picture.
        unsigned long long sum = 0;
        for (UINT y = 0; y < d.Height; y += 4)
            for (UINT x = 0; x < d.Width; x += 4) {
                const BYTE* px = (const BYTE*)lr.pBits + y * lr.Pitch + x * 4;
                sum += px[0] + px[1] + px[2];
            }
        double avg = (double)sum / (3.0 * ((d.Height + 3) / 4) * ((d.Width + 3) / 4));
        wchar_t name[MAX_PATH];
        swprintf_s(name, L"%sSCCT_Quest_frame%u.bmp", g_dir, frame);
        HANDLE f = CreateFileW(name, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            BITMAPFILEHEADER fh = {};
            BITMAPINFOHEADER ih = {};
            ih.biSize = sizeof(ih);
            ih.biWidth = d.Width;
            ih.biHeight = -(LONG)d.Height;
            ih.biPlanes = 1;
            ih.biBitCount = 32;
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof(fh) + sizeof(ih);
            fh.bfSize = fh.bfOffBits + d.Width * d.Height * 4;
            DWORD w;
            WriteFile(f, &fh, sizeof(fh), &w, nullptr);
            WriteFile(f, &ih, sizeof(ih), &w, nullptr);
            for (UINT y = 0; y < d.Height; y++) WriteFile(f, (BYTE*)lr.pBits + y * lr.Pitch, d.Width * 4, &w, nullptr);
            CloseHandle(f);
        }
        sys->UnlockRect();
        Log("screenshot at frame %u: %ux%u, average brightness %.1f / 255 -> %ls", frame, d.Width, d.Height, avg, name);
    }
    if (sys) sys->Release();
    bb->Release();
}

// The game window gets minimized a few seconds after the first frame (the game keeps running
// and playing sound, but a minimized UE2 window shows and renders nothing). Log who does it and
// keep the window restored: ShowWindow calls that minimize are turned into restores, and the
// window procedure ignores minimize requests and deactivation (the game minimizes itself when
// it thinks it lost focus).
static WNDPROC g_origProc;
using ShowWindow_t = BOOL(WINAPI*)(HWND, int);
static ShowWindow_t oShowWindow;

static bool IsMinimizeCmd(int cmd)
{
    return cmd == SW_MINIMIZE || cmd == SW_SHOWMINIMIZED || cmd == SW_SHOWMINNOACTIVE || cmd == SW_FORCEMINIMIZE;
}

static BOOL WINAPI hkShowWindow(HWND w, int cmd)
{
    void* caller = _ReturnAddress();
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)caller, &m);
    wchar_t name[MAX_PATH] = L"?";
    if (m) GetModuleFileNameW(m, name, MAX_PATH);
    bool block = w == g_wnd && IsMinimizeCmd(cmd);
    Log("ShowWindow(%p, %d) from %ls+0x%lX%s", w, cmd, wcsrchr(name, L'\\') ? wcsrchr(name, L'\\') + 1 : name,
        (DWORD)((BYTE*)caller - (BYTE*)m), block ? " - minimize blocked, restoring instead" : "");
    return oShowWindow(w, block ? SW_RESTORE : cmd);
}

static LRESULT CALLBACK WndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ACTIVATEAPP:
        Log("WM_ACTIVATEAPP %d%s", (int)wp, wp ? "" : " - passed on as active");
        if (!wp) return CallWindowProcW(g_origProc, w, msg, TRUE, lp);
        break;
    case WM_ACTIVATE:
        Log("WM_ACTIVATE %d minimized %d%s", LOWORD(wp), HIWORD(wp), LOWORD(wp) == WA_INACTIVE ? " - passed on as active" : "");
        if (LOWORD(wp) == WA_INACTIVE) return CallWindowProcW(g_origProc, w, msg, MAKEWPARAM(WA_ACTIVE, 0), lp);
        break;
    case WM_KILLFOCUS:
        Log("WM_KILLFOCUS (new focus %p) - ignored", (HWND)wp);
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_MINIMIZE) { Log("WM_SYSCOMMAND SC_MINIMIZE - blocked"); return 0; }
        break;
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) Log("WM_SIZE minimized");
        break;
    case WM_WINDOWPOSCHANGING: {
        auto* p = (WINDOWPOS*)lp;
        if (p && !(p->flags & SWP_NOMOVE) && p->x <= -30000) {
            Log("WM_WINDOWPOSCHANGING to %d,%d (minimizing) - kept on screen", p->x, p->y);
            p->flags |= SWP_NOMOVE | SWP_NOSIZE;
        }
        break;
    }
    }
    return CallWindowProcW(g_origProc, w, msg, wp, lp);
}

// Every loaded module with its address range, so crash addresses can be mapped to a module.
static void LogModules(const char* why)
{
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return;
    Log("loaded modules (%s):", why);
    for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 1024; i++) {
        MODULEINFO mi = {};
        wchar_t name[MAX_PATH] = L"?";
        K32GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi));
        GetModuleFileNameW(mods[i], name, MAX_PATH);
        DWORD base = (DWORD)(ULONG_PTR)mi.lpBaseOfDll;
        Log("  module 0x%08lX-0x%08lX %ls", base, base + mi.SizeOfImage, name);
    }
}

// Reloaded's crash handler exits with the exception code: list the modules first.
using ExitProcess_t = void(WINAPI*)(UINT);
static ExitProcess_t oExitProcess;
static void WINAPI hkExitProcess(UINT code)
{
    Log("ExitProcess(0x%08X)", code);
    if (code >= 0xC0000000) LogModules("crash exit");
    oExitProcess(code);
}

// Redirect `func` in every loaded module's import table.
static int PatchImports(FARPROC target, void* hook)
{
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!target || !K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return 0;
    int patched = 0;
    for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 1024; i++) {
        BYTE* base = (BYTE*)mods[i];
        if (mods[i] == g_self) continue;
        auto* dos = (IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) continue;
        auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
        IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress) continue;
        for (auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++)
            for (auto* thunk = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk); thunk->u1.Function; thunk++) {
                if ((FARPROC)thunk->u1.Function != target) continue;
                DWORD old;
                VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
                thunk->u1.Function = (ULONG_PTR)hook;
                VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &old);
                patched++;
            }
    }
    return patched;
}

static void HookExitProcess()
{
    FARPROC target = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ExitProcess");
    oExitProcess = (ExitProcess_t)target;
    Log("ExitProcess hooked in %d import(s)", PatchImports(target, (void*)hkExitProcess));
}

// Redirect user32!ShowWindow in every loaded module's import table.
static void HookShowWindow()
{
    FARPROC target = GetProcAddress(GetModuleHandleW(L"user32.dll"), "ShowWindow");
    oShowWindow = (ShowWindow_t)target;
    Log("ShowWindow hooked in %d import(s)", PatchImports(target, (void*)hkShowWindow));
}

using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using Reset_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using EndScene_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using Create9_t = IDirect3D9*(WINAPI*)(UINT);
static Present_t oPresent;
static Reset_t oReset;
static EndScene_t oEndScene;
static CreateDevice_t oCreateDevice;
static Create9_t oCreate9;

static HRESULT STDMETHODCALLTYPE hkPresent(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty)
{
    HRESULT hr = oPresent(dev, src, dst, wnd, dirty);
    g_presents++;
    g_presentsTotal++;
    if (g_wnd && IsIconic(g_wnd)) {
        static unsigned restores;
        if (restores++ < 20) Log("window found minimized at frame %u - restoring", g_presentsTotal);
        oShowWindow(g_wnd, SW_RESTORE);
    }
    if (g_presentsTotal == 1) { Log("first Present -> 0x%08lX", hr); LogWindow("first frame"); }
    // (Backbuffer screenshots stall the renderer under Wine; enable for diagnostics only.)
    if (g_screenshots && (g_presentsTotal == 300 || g_presentsTotal == 3000)) SaveBackbuffer(dev, g_presentsTotal);
    DWORD now = GetTickCount();
    if (now - g_statTick >= 5000) {
        Log("%.1f presents/s, %.1f EndScene/s, total %u, last Present 0x%08lX", g_presents * 1000.0 / (now - g_statTick),
            g_endScenes * 1000.0 / (now - g_statTick), g_presentsTotal, hr);
        LogWindow("window");
        g_presents = g_endScenes = 0;
        g_statTick = now;
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hkEndScene(IDirect3DDevice9* dev)
{
    g_endScenes++;
    return oEndScene(dev);
}

static void LogParams(const char* what, const D3DPRESENT_PARAMETERS* pp)
{
    Log("%s: backbuffer %ux%u format %d count %u, windowed %d, swap effect %d, device window %p, refresh %u Hz, interval 0x%X, msaa %d, flags 0x%lX",
        what, pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat, pp->BackBufferCount, pp->Windowed,
        pp->SwapEffect, pp->hDeviceWindow, pp->FullScreen_RefreshRateInHz, pp->PresentationInterval, pp->MultiSampleType, pp->Flags);
}

static HRESULT STDMETHODCALLTYPE hkReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
{
    if (pp) LogParams("Reset", pp);
    HRESULT hr = oReset(dev, pp);
    Log("Reset -> 0x%08lX", hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hkCreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                                D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out)
{
    D3DADAPTER_IDENTIFIER9 id = {};
    d3d->GetAdapterIdentifier(adapter, 0, &id);
    D3DDISPLAYMODE mode = {};
    d3d->GetAdapterDisplayMode(adapter, &mode);
    Log("CreateDevice: adapter %u '%s' (driver %s), desktop mode %ux%u %u Hz format %d, type %d, focus window %p, flags 0x%lX",
        adapter, id.Description, id.Driver, mode.Width, mode.Height, mode.RefreshRate, mode.Format, type, focus, flags);
    if (pp) LogParams("CreateDevice", pp);
    HRESULT hr = oCreateDevice(d3d, adapter, type, focus, flags, pp, out);
    Log("CreateDevice -> 0x%08lX", hr);
    if (SUCCEEDED(hr) && out && *out) {
        g_wnd = (pp && pp->hDeviceWindow) ? pp->hDeviceWindow : focus;
        LogWindow("after CreateDevice");
        HookShowWindow();
        HookExitProcess();
        LogModules("after CreateDevice");
        if (g_wnd && !g_origProc) {
            g_origProc = (WNDPROC)SetWindowLongPtrW(g_wnd, GWLP_WNDPROC, (LONG_PTR)WndProc);
            Log("game window procedure wrapped (minimize / deactivation blocked)");
        }
        PatchSlot(*out, 16, (Reset_t)hkReset, &oReset);
        PatchSlot(*out, 17, (Present_t)hkPresent, &oPresent);
        PatchSlot(*out, 42, (EndScene_t)hkEndScene, &oEndScene);
        g_statTick = GetTickCount();
    }
    return hr;
}

static IDirect3D9* WINAPI hkDirect3DCreate9(UINT sdk)
{
    IDirect3D9* d3d = oCreate9(sdk);
    Log("Direct3DCreate9(%u) -> %p", sdk, d3d);
    if (d3d) PatchSlot(d3d, 16, (CreateDevice_t)hkCreateDevice, &oCreateDevice);
    return d3d;
}

// Redirect Reloaded.Core's import of d3d9!Direct3DCreate9.
static void HookReloaded(HMODULE reloaded)
{
    typedef const char* (CDECL* WineVersion_t)();
    auto wineVersion = (WineVersion_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version");
    Log("Wine %s", wineVersion ? wineVersion() : "(not Wine)");

    HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");
    wchar_t d3d9Path[MAX_PATH] = L"?";
    if (d3d9) GetModuleFileNameW(d3d9, d3d9Path, MAX_PATH);
    Log("d3d9.dll: %ls (%s)", d3d9Path, d3d9 ? (ModuleContains(d3d9, "dxvk") ? "DXVK" : "no DXVK strings - WineD3D?") : "not loaded");

    BYTE* base = (BYTE*)reloaded;
    auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); dir.VirtualAddress && imp->Name; imp++) {
        if (_stricmp((const char*)(base + imp->Name), "d3d9.dll")) continue;
        auto* names = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto* thunk = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, thunk++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* byName = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((const char*)byName->Name, "Direct3DCreate9")) continue;
            DWORD old;
            VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            oCreate9 = (Create9_t)thunk->u1.Function;
            thunk->u1.Function = (ULONG_PTR)hkDirect3DCreate9;
            VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &old);
            Log("Reloaded.Core's Direct3DCreate9 import redirected for diagnostics");
            return;
        }
    }
    Log("Reloaded.Core does not import d3d9!Direct3DCreate9 - no D3D diagnostics");
}

BOOL WINAPI DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        g_self = mod;
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(mod, path, MAX_PATH);
        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash) {
            slash[1] = 0;
            wcscpy_s(g_dir, path);
            wchar_t flag[MAX_PATH];
            swprintf_s(flag, L"%sSCCT_Quest.screenshots", g_dir);
            g_screenshots = GetFileAttributesW(flag) != INVALID_FILE_ATTRIBUTES;
            wcscat_s(path, L"Reloaded.Core.dll");
            HMODULE r = LoadLibraryW(path);
            DWORD err = r ? 0 : GetLastError();
            Log("vorbisfile proxy: Reloaded.Core.dll %s (error %lu)", r ? "loaded" : "FAILED to load", err);
            if (r) HookReloaded(r);
        }
    }
    return TRUE;
}
