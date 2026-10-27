// d3d9.dll proxy. Reloaded.Core.dll imports d3d9.dll and the game's System folder is searched
// first, so this module is loaded instead of the system one. Everything except
// Direct3DCreate9 is forwarded to the real System32 (SysWOW64) d3d9.dll.

#include <windows.h>
#include <d3d9.h>
#include <d3d9on12.h>
#include <psapi.h>
#include "log.h"
#include "config.h"
#include "vr.h"
#include "hooks.h"
#include "dihook.h"

HMODULE g_self;

static HMODULE g_real;
static INIT_ONCE g_realOnce = INIT_ONCE_STATIC_INIT;

static const char* const kNamed[] = {
    "D3DPERF_BeginEvent", "D3DPERF_EndEvent", "D3DPERF_GetStatus", "D3DPERF_QueryRepeatFrame",
    "D3DPERF_SetMarker", "D3DPERF_SetOptions", "D3DPERF_SetRegion", "DebugSetLevel", "DebugSetMute",
    "Direct3D9EnableMaximizedWindowedModeShim", "Direct3DCreate9Ex", "Direct3DCreate9On12",
    "Direct3DCreate9On12Ex", "Direct3DShaderValidatorCreate9", "PSGPError", "PSGPSampleTexture",
};
static const WORD kOrdinals[] = { 16, 17, 18, 19, 22, 23 };
static const int kNamedCount = sizeof(kNamed) / sizeof(kNamed[0]);
static const int kOrdinalCount = sizeof(kOrdinals) / sizeof(kOrdinals[0]);

extern "C" FARPROC g_procs[kNamedCount + kOrdinalCount] = {};
static FARPROC g_realCreate9;

// The ordinal-only exports are compatibility shims taking one int (e.g.
// Direct3D9SetSwapEffectUpgradeShim). Wine's d3d9 does not have them; callers that find our
// export would otherwise jump to null.
extern "C" int WINAPI MissingShim(int)
{
    return 0;
}

static BOOL CALLBACK LoadReal(PINIT_ONCE, PVOID, PVOID*)
{
    wchar_t path[MAX_PATH];
    GetSystemDirectoryW(path, MAX_PATH);
    wcscat_s(path, L"\\d3d9.dll");
    g_real = LoadLibraryW(path);
    if (!g_real) return TRUE;
    for (int i = 0; i < kNamedCount; i++) g_procs[i] = GetProcAddress(g_real, kNamed[i]);
    for (int i = 0; i < kOrdinalCount; i++) {
        g_procs[kNamedCount + i] = GetProcAddress(g_real, MAKEINTRESOURCEA(kOrdinals[i]));
        if (!g_procs[kNamedCount + i]) g_procs[kNamedCount + i] = (FARPROC)MissingShim;
    }
    g_realCreate9 = GetProcAddress(g_real, "Direct3DCreate9");
    return TRUE;
}

extern "C" void __cdecl EnsureReal()
{
    InitOnceExecuteOnce(&g_realOnce, LoadReal, nullptr, nullptr);
}

// Forwarding stubs: load the real DLL, then tail-jump with the caller's stack untouched.
#define STUB(i)                                                  \
    extern "C" __declspec(naked) void Stub##i()                  \
    {                                                            \
        __asm call EnsureReal                                    \
        __asm jmp dword ptr [g_procs + 4 * i]                    \
    }
STUB(0) STUB(1) STUB(2) STUB(3) STUB(4) STUB(5) STUB(6) STUB(7) STUB(8) STUB(9) STUB(10)
STUB(11) STUB(12) STUB(13) STUB(14) STUB(15) STUB(16) STUB(17) STUB(18) STUB(19) STUB(20) STUB(21)

// The OpenXR runtime can deadlock while DLLs are unloaded at process exit (its threads are
// already gone), leaving a hung game window. Exit through here: end the XR session, then
// terminate without running DLL detach.
// After a crash (Reloaded's handler exits with the exception code), record where every module
// was loaded so the addresses in SCCT_Versus_crash.log can be matched to a module.
static void LogModules()
{
    HMODULE mods[512];
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return;
    for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 512; i++) {
        MODULEINFO mi = {};
        wchar_t name[MAX_PATH] = L"?";
        K32GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi));
        GetModuleFileNameW(mods[i], name, MAX_PATH);
        DWORD base = (DWORD)(ULONG_PTR)mi.lpBaseOfDll;
        Log("  module 0x%08lX-0x%08lX %ls", base, base + mi.SizeOfImage, name);
    }
}

static void WINAPI HookedExitProcess(UINT code)
{
    Log("ExitProcess(%u)", code);
    if (code >= 0xC0000000) {
        Log("Exit with exception code 0x%08X - loaded modules:", code);
        LogModules();
    }
    vr::Shutdown();
    TerminateProcess(GetCurrentProcess(), code);
}

static void HookExitProcess()
{
    FARPROC target = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ExitProcess");
    HMODULE mods[512];
    DWORD needed = 0;
    if (!target || !K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return;
    int patched = 0;
    for (DWORD m = 0; m < needed / sizeof(HMODULE) && m < 512; m++) {
        BYTE* base = (BYTE*)mods[m];
        if (mods[m] == g_self) continue;
        auto* dos = (IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) continue;
        auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
        IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress) continue;
        for (auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
            for (auto* thunk = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk); thunk->u1.Function; thunk++) {
                if ((FARPROC)thunk->u1.Function != target) continue;
                DWORD old;
                VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
                thunk->u1.Function = (ULONG_PTR)HookedExitProcess;
                VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &old);
                patched++;
            }
        }
    }
    Log("ExitProcess hooked in %d import(s)", patched);
}

extern "C" IDirect3D9* WINAPI Proxy_Direct3DCreate9(UINT sdk)
{
    EnsureReal();
    LogInit();
    using Create9_t = IDirect3D9*(WINAPI*)(UINT);
    using Create9On12_t = IDirect3D9*(WINAPI*)(UINT, D3D9ON12_ARGS*, UINT);
    if (!g_realCreate9) return nullptr;

    static bool configLoaded = false;
    if (!configLoaded) { LoadConfig(); configLoaded = true; }

    if (g_cfg.enabled && vr::Init() && vr::UsesXrApi()) {
        // WinlatorXR: a plain D3D9 device (Wine), the game window is what the headset shows.
        dihook::InstallEarly();     // in case dinput8 was not loaded yet at DllMain
        IDirect3D9* d3d = ((Create9_t)g_realCreate9)(sdk);
        Log("Direct3DCreate9(%u) -> plain D3D9 for the XR API (%p)", sdk, d3d);
        if (d3d) {
            HookDirect3D9(d3d);
            static bool exitHooked = false;
            if (!exitHooked) { HookExitProcess(); exitHooked = true; }
        }
        return d3d;
    }
    if (g_cfg.enabled && vr::Available()) {
        auto create9On12 = (Create9On12_t)g_procs[11];
        if (create9On12) {
            D3D9ON12_ARGS args = {};
            args.Enable9On12 = TRUE;
            args.pD3D12Device = vr::D3D12Device();
            args.ppD3D12Queues[0] = vr::D3D12Queue();
            args.NumQueues = 1;
            IDirect3D9* d3d = create9On12(sdk, &args, 1);
            if (d3d) {
                Log("Direct3DCreate9(%u) -> D3D9On12", sdk);
                HookDirect3D9(d3d);
                static bool exitHooked = false;
                if (!exitHooked) { HookExitProcess(); exitHooked = true; }
                return d3d;
            }
            Log("Direct3DCreate9On12 failed, falling back to plain D3D9");
        }
    }
    Log("Direct3DCreate9(%u) -> plain D3D9 (flat mode)", sdk);
    return ((Create9_t)g_realCreate9)(sdk);
}

BOOL WINAPI DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = mod;
        DisableThreadLibraryCalls(mod);
        // WinlatorXR container: the game's DirectInput keyboard must be wrapped before the game
        // creates it (it does not see keys injected under Wine).
        if (GetFileAttributesW(L"Z:\\tmp\\xr\\system") != INVALID_FILE_ATTRIBUTES) {
            LogInit();
            dihook::InstallEarly();
        }
    }
    return TRUE;
}
