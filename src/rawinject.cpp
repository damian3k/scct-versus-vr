#include "rawinject.h"
#include "log.h"
#include <psapi.h>
#include <cstdio>
#include <cstring>

extern HMODULE g_self;

namespace rawinject {

namespace {

// Synthetic events live in a ring; WM_INPUT messages carry kMarker + slot as their handle.
const ULONG_PTR kMarker = 0x7E5A0000;
const unsigned kSlots = 1024;

struct Event {
    RAWINPUT data;
    bool delivered;
};

CRITICAL_SECTION g_lock;
Event g_ring[kSlots];
unsigned g_write;           // next slot to fill
unsigned g_read;            // oldest slot not yet handed out through GetRawInputBuffer
bool g_active;
HWND g_target;

unsigned g_posted, g_servedData, g_servedBuffer, g_realData, g_realBuffer;

using GetRawInputData_t = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using GetRawInputBuffer_t = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);
GetRawInputData_t oGetRawInputData;
GetRawInputBuffer_t oGetRawInputBuffer;

bool IsMarker(HRAWINPUT h)
{
    return (ULONG_PTR)h - kMarker < kSlots;
}

UINT WINAPI hkGetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT headerSize)
{
    if (!IsMarker(h)) {
        g_realData++;
        return oGetRawInputData(h, cmd, data, size, headerSize);
    }
    if (!size || headerSize != sizeof(RAWINPUTHEADER)) return (UINT)-1;
    EnterCriticalSection(&g_lock);
    Event& e = g_ring[(ULONG_PTR)h - kMarker];
    UINT result;
    if (e.delivered) {
        result = (UINT)-1;      // already handed out through GetRawInputBuffer
    } else {
        UINT need = cmd == RID_HEADER ? sizeof(RAWINPUTHEADER) : e.data.header.dwSize;
        if (!data) {
            *size = need;
            result = 0;
        } else if (*size < need) {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            result = (UINT)-1;
        } else {
            memcpy(data, &e.data, need);
            if (cmd == RID_INPUT) {
                e.delivered = true;
                g_servedData++;
            }
            result = need;
        }
    }
    LeaveCriticalSection(&g_lock);
    return result;
}

// Buffered reading: when no real input is waiting, hand out the queued synthetic events.
// Records are 32 (keyboard) or 40 (mouse) bytes, so they line up with any alignment rule.
UINT WINAPI hkGetRawInputBuffer(PRAWINPUT data, PUINT size, UINT headerSize)
{
    UINT n = oGetRawInputBuffer(data, size, headerSize);
    if (n == (UINT)-1 || !size || headerSize != sizeof(RAWINPUTHEADER)) return n;
    if (n > 0) { g_realBuffer += n; return n; }
    EnterCriticalSection(&g_lock);
    while (g_read != g_write && g_ring[g_read % kSlots].delivered) g_read++;
    bool pending = g_read != g_write;
    if (!data) {
        if (pending && *size < sizeof(RAWINPUT)) *size = sizeof(RAWINPUT);
    } else if (pending) {
        BYTE* p = (BYTE*)data;
        UINT left = *size;
        for (; g_read != g_write; g_read++) {
            Event& e = g_ring[g_read % kSlots];
            if (e.delivered) continue;
            if (e.data.header.dwSize > left) break;
            memcpy(p, &e.data, e.data.header.dwSize);
            p += e.data.header.dwSize;
            left -= e.data.header.dwSize;
            e.delivered = true;
            n++;
        }
        g_servedBuffer += n;
    }
    LeaveCriticalSection(&g_lock);
    return n;
}

int PatchImports(FARPROC target, void* hook)
{
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!target || !K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return 0;
    int patched = 0;
    for (DWORD m = 0; m < needed / sizeof(HMODULE) && m < 1024; m++) {
        if (mods[m] == g_self) continue;
        BYTE* base = (BYTE*)mods[m];
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

void Push(const RAWINPUT& r)
{
    if (!g_active) return;
    EnterCriticalSection(&g_lock);
    unsigned slot = g_write % kSlots;
    g_ring[slot].data = r;
    g_ring[slot].delivered = false;
    g_write++;
    if (g_write - g_read > kSlots) g_read = g_write - kSlots;   // overflow: drop the oldest
    LeaveCriticalSection(&g_lock);
    PostMessageW(g_target, WM_INPUT, RIM_INPUT, (LPARAM)(kMarker + slot));
    g_posted++;
}

// Where the game registered its raw input (the messages must go to that window). Reloaded may
// register after the device is created, so this is checked again later.
void FindTarget()
{
    static UINT logged;
    RAWINPUTDEVICE devs[16];
    UINT count = 16;
    UINT n = GetRegisteredRawInputDevices(devs, &count, sizeof(RAWINPUTDEVICE));
    for (UINT i = 0; n != (UINT)-1 && i < n; i++) {
        if (n != logged)
            Log("Raw input registered: usage page %u usage %u flags 0x%lX window %p", devs[i].usUsagePage,
                devs[i].usUsage, devs[i].dwFlags, devs[i].hwndTarget);
        if (devs[i].hwndTarget) g_target = devs[i].hwndTarget;
    }
    if (n != (UINT)-1) logged = n;
}

} // namespace

void Install(HWND wnd)
{
    if (g_active) return;
    InitializeCriticalSection(&g_lock);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    oGetRawInputData = (GetRawInputData_t)GetProcAddress(user32, "GetRawInputData");
    oGetRawInputBuffer = (GetRawInputBuffer_t)GetProcAddress(user32, "GetRawInputBuffer");
    int a = PatchImports((FARPROC)oGetRawInputData, (void*)hkGetRawInputData);
    int b = PatchImports((FARPROC)oGetRawInputBuffer, (void*)hkGetRawInputBuffer);
    g_target = wnd;
    FindTarget();
    g_active = true;
    Log("Raw input injection ready: GetRawInputData hooked in %d, GetRawInputBuffer in %d import(s), window %p", a, b, g_target);
}

bool Active() { return g_active; }

int PatchAllImports(FARPROC target, void* hook)
{
    return PatchImports(target, hook);
}

void Key(WORD scan, bool extended, bool down)
{
    RAWINPUT r = {};
    r.header.dwType = RIM_TYPEKEYBOARD;
    r.header.dwSize = sizeof(RAWINPUTHEADER) + sizeof(RAWKEYBOARD);
    r.header.wParam = RIM_INPUT;
    r.data.keyboard.MakeCode = scan;
    r.data.keyboard.Flags = (USHORT)((down ? RI_KEY_MAKE : RI_KEY_BREAK) | (extended ? RI_KEY_E0 : 0));
    r.data.keyboard.VKey = (USHORT)MapVirtualKeyW(scan | (extended ? 0xE000 : 0), MAPVK_VSC_TO_VK_EX);
    r.data.keyboard.Message = down ? WM_KEYDOWN : WM_KEYUP;
    Push(r);
}

void Mouse(LONG dx, LONG dy, USHORT buttonFlags, short wheel)
{
    RAWINPUT r = {};
    r.header.dwType = RIM_TYPEMOUSE;
    r.header.dwSize = sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE);
    r.header.wParam = RIM_INPUT;
    r.data.mouse.usFlags = MOUSE_MOVE_RELATIVE;
    r.data.mouse.usButtonFlags = (USHORT)(buttonFlags | (wheel ? RI_MOUSE_WHEEL : 0));
    r.data.mouse.usButtonData = (USHORT)wheel;
    r.data.mouse.lLastX = dx;
    r.data.mouse.lLastY = dy;
    Push(r);
}

void Stats(char* buf, size_t size)
{
    if (g_active) FindTarget();
    _snprintf_s(buf, size, _TRUNCATE, "raw input: %u synthetic posted, served %u by GetRawInputData / %u by GetRawInputBuffer; real %u / %u",
                g_posted, g_servedData, g_servedBuffer, g_realData, g_realBuffer);
}

} // namespace rawinject
