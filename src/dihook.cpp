#define DIRECTINPUT_VERSION 0x0800
#include "dihook.h"
#include "log.h"
#include <dinput.h>
#include <psapi.h>
#include <cstdio>
#include <cstring>

extern HMODULE g_self;

namespace dihook {

namespace {

const GUID kSysKeyboard = { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
const GUID kIDirectInput8W = { 0xBF798031, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };

using Create8_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(void*, REFGUID, void**, LPUNKNOWN);
using GetDeviceState_t = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPVOID);
using GetDeviceData_t = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);

Create8_t oCreate8;
CreateDevice_t oCreateDeviceA, oCreateDeviceW;

bool g_active;

template <class T>
void PatchSlot(void* obj, int slot, T hook, T* orig)
{
    void** vt = *(void***)obj;
    if (vt[slot] == (void*)hook) return;
    DWORD old;
    VirtualProtect(&vt[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    *orig = (T)vt[slot];
    vt[slot] = (void*)hook;
    VirtualProtect(&vt[slot], sizeof(void*), old, &old);
}

// Wrapped devices. Different device types can have different vtables, so the original
// methods are kept per vtable.
enum Kind { kKeyboard, kMouse };
struct Device { void* dev; Kind kind; };
Device g_devices[8];
int g_deviceCount;
struct Vtable { void** vt; GetDeviceState_t state; GetDeviceData_t data; };
Vtable g_vtables[4];
int g_vtableCount;

CRITICAL_SECTION g_lock;
BYTE g_state[256];                  // keys held by the controllers (0x80)
struct Event { DWORD ofs; DWORD data; };
Event g_keyQueue[128], g_mouseQueue[256];
unsigned g_kRead, g_kWrite, g_mRead, g_mWrite;
LONG g_mouseX, g_mouseY, g_mouseZ;  // motion not yet read through GetDeviceState
BYTE g_buttons[8];                  // mouse buttons held by the controllers (0x80)
DWORD g_sequence = 0x40000000;
unsigned g_stateCalls, g_dataCalls, g_injected, g_failed, g_mouseReads;

const Vtable* FindVtable(void* dev)
{
    void** vt = *(void***)dev;
    for (int i = 0; i < g_vtableCount; i++)
        if (g_vtables[i].vt == vt) return &g_vtables[i];
    return nullptr;
}

const Device* FindDevice(void* dev)
{
    for (int i = 0; i < g_deviceCount; i++)
        if (g_devices[i].dev == dev) return &g_devices[i];
    return nullptr;
}

void Queue(Event* q, unsigned size, unsigned& read, unsigned& write, DWORD ofs, DWORD data)
{
    if (write - read < size) q[write++ % size] = { ofs, data };
}

HRESULT STDMETHODCALLTYPE hkGetDeviceState(void* dev, DWORD cb, LPVOID data)
{
    const Vtable* vt = FindVtable(dev);
    if (!vt) return DIERR_GENERIC;
    HRESULT hr = vt->state(dev, cb, data);
    const Device* d = FindDevice(dev);
    if (!d || !data) return hr;
    g_stateCalls++;
    if (FAILED(hr)) {
        // Wine may refuse to read the device (not acquired): the controller input still counts.
        g_failed++;
        memset(data, 0, cb);
        hr = DI_OK;
    }
    EnterCriticalSection(&g_lock);
    if (d->kind == kKeyboard && cb >= 256) {
        BYTE* keys = (BYTE*)data;
        for (int i = 0; i < 256; i++) keys[i] |= g_state[i];
    } else if (d->kind == kMouse && cb >= sizeof(DIMOUSESTATE)) {
        auto* m = (DIMOUSESTATE*)data;
        m->lX += g_mouseX;
        m->lY += g_mouseY;
        m->lZ += g_mouseZ;
        g_mouseX = g_mouseY = g_mouseZ = 0;
        int buttons = cb >= sizeof(DIMOUSESTATE2) ? 8 : 4;
        for (int i = 0; i < buttons; i++) m->rgbButtons[i] |= g_buttons[i];
        g_mouseReads++;
    }
    LeaveCriticalSection(&g_lock);
    return hr;
}

HRESULT STDMETHODCALLTYPE hkGetDeviceData(void* dev, DWORD cbObject, LPDIDEVICEOBJECTDATA data, LPDWORD inOut, DWORD flags)
{
    const Vtable* vt = FindVtable(dev);
    if (!vt) return DIERR_GENERIC;
    DWORD requested = inOut ? *inOut : 0;
    HRESULT hr = vt->data(dev, cbObject, data, inOut, flags);
    const Device* d = FindDevice(dev);
    if (!d || !inOut) return hr;
    g_dataCalls++;
    if (!data || (flags & DIGDD_PEEK) || cbObject < sizeof(DIDEVICEOBJECTDATA_DX3)) return hr;
    if (FAILED(hr)) {
        g_failed++;
        *inOut = 0;
        hr = DI_OK;
    }
    EnterCriticalSection(&g_lock);
    Event* q = d->kind == kKeyboard ? g_keyQueue : g_mouseQueue;
    unsigned size = d->kind == kKeyboard ? 128 : 256;
    unsigned& read = d->kind == kKeyboard ? g_kRead : g_mRead;
    unsigned write = d->kind == kKeyboard ? g_kWrite : g_mWrite;
    DWORD got = *inOut;
    while (got < requested && read != write) {
        const Event& e = q[read++ % size];
        auto* od = (DIDEVICEOBJECTDATA*)((BYTE*)data + got * cbObject);
        memset(od, 0, cbObject);
        od->dwOfs = e.ofs;
        od->dwData = e.data;
        od->dwTimeStamp = GetTickCount();
        od->dwSequence = g_sequence++;
        got++;
        g_injected++;
    }
    // The game reads the mouse both ways every frame and takes the motion from the buffered
    // data, so the state and the buffer each get their own copy of it.
    if (d->kind == kMouse) g_mouseReads++;
    *inOut = got;
    LeaveCriticalSection(&g_lock);
    return hr;
}

void Wrap(void* dev, Kind kind)
{
    if (g_deviceCount < 8) g_devices[g_deviceCount++] = { dev, kind };
    void** vt = *(void***)dev;
    if (vt[9] == (void*)hkGetDeviceState) return;
    if (g_vtableCount >= 4) return;
    Vtable& v = g_vtables[g_vtableCount++];
    v.vt = vt;
    v.state = (GetDeviceState_t)vt[9];
    v.data = (GetDeviceData_t)vt[10];
    DWORD old;
    VirtualProtect(&vt[9], 2 * sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    vt[9] = (void*)hkGetDeviceState;
    vt[10] = (void*)hkGetDeviceData;
    VirtualProtect(&vt[9], 2 * sizeof(void*), old, &old);
}

HRESULT CreateDeviceCommon(CreateDevice_t orig, void* di, REFGUID guid, void** out, LPUNKNOWN outer)
{
    HRESULT hr = orig(di, guid, out, outer);
    // The game creates its devices from their instance GUIDs, so they are recognised by their
    // device type (GetCapabilities, slot 3).
    DWORD type = 0;
    if (SUCCEEDED(hr) && out && *out) {
        DIDEVCAPS caps = {};
        caps.dwSize = sizeof(caps);
        using GetCaps_t = HRESULT(STDMETHODCALLTYPE*)(void*, LPDIDEVCAPS);
        if (SUCCEEDED(((GetCaps_t)(*(void***)*out)[3])(*out, &caps))) type = caps.dwDevType;
        if (GET_DIDEVICE_TYPE(type) == DI8DEVTYPE_KEYBOARD || IsEqualGUID(guid, kSysKeyboard))
            Wrap(*out, kKeyboard);
        else if (GET_DIDEVICE_TYPE(type) == DI8DEVTYPE_MOUSE)
            Wrap(*out, kMouse);
    }
    Log("DirectInput CreateDevice: type 0x%lX%s -> 0x%08lX", type,
        GET_DIDEVICE_TYPE(type) == DI8DEVTYPE_KEYBOARD ? " (keyboard)" : GET_DIDEVICE_TYPE(type) == DI8DEVTYPE_MOUSE ? " (mouse)" : "", hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE hkCreateDeviceA(void* di, REFGUID guid, void** out, LPUNKNOWN outer)
{
    return CreateDeviceCommon(oCreateDeviceA, di, guid, out, outer);
}

HRESULT STDMETHODCALLTYPE hkCreateDeviceW(void* di, REFGUID guid, void** out, LPUNKNOWN outer)
{
    return CreateDeviceCommon(oCreateDeviceW, di, guid, out, outer);
}

HRESULT WINAPI hkDirectInput8Create(HINSTANCE inst, DWORD version, REFIID riid, LPVOID* out, LPUNKNOWN outer)
{
    HRESULT hr = oCreate8(inst, version, riid, out, outer);
    Log("DirectInput8Create(version 0x%lX) -> 0x%08lX", version, hr);
    if (SUCCEEDED(hr) && out && *out) {
        // IDirectInput8A and W have the same layout; CreateDevice is slot 3.
        if (IsEqualIID(riid, kIDirectInput8W))
            PatchSlot(*out, 3, (CreateDevice_t)hkCreateDeviceW, &oCreateDeviceW);
        else
            PatchSlot(*out, 3, (CreateDevice_t)hkCreateDeviceA, &oCreateDeviceA);
    }
    return hr;
}

} // namespace

void InstallEarly()
{
    if (g_active) return;
    HMODULE dinput8 = GetModuleHandleW(L"dinput8.dll");
    if (!dinput8) return;
    InitializeCriticalSection(&g_lock);
    FARPROC target = GetProcAddress(dinput8, "DirectInput8Create");
    oCreate8 = (Create8_t)target;
    HMODULE mods[1024];
    DWORD needed = 0;
    int patched = 0;
    if (target && K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
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
                    thunk->u1.Function = (ULONG_PTR)hkDirectInput8Create;
                    VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &old);
                    patched++;
                }
        }
    }
    g_active = patched > 0;
    Log("DirectInput keyboard hook: DirectInput8Create redirected in %d import(s)", patched);
}

bool Active() { return g_active; }

bool MouseActive()
{
    for (int i = 0; i < g_deviceCount; i++)
        if (g_devices[i].kind == kMouse) return true;
    return false;
}

void Key(BYTE dik, bool down)
{
    if (!g_active) return;
    EnterCriticalSection(&g_lock);
    g_state[dik] = down ? 0x80 : 0;
    Queue(g_keyQueue, 128, g_kRead, g_kWrite, dik, down ? 0x80 : 0);
    LeaveCriticalSection(&g_lock);
}

void MouseMove(LONG dx, LONG dy, LONG wheel)
{
    if (!g_active) return;
    EnterCriticalSection(&g_lock);
    g_mouseX += dx;
    g_mouseY += dy;
    g_mouseZ += wheel;
    if (dx) Queue(g_mouseQueue, 256, g_mRead, g_mWrite, DIMOFS_X, (DWORD)dx);
    if (dy) Queue(g_mouseQueue, 256, g_mRead, g_mWrite, DIMOFS_Y, (DWORD)dy);
    if (wheel) Queue(g_mouseQueue, 256, g_mRead, g_mWrite, DIMOFS_Z, (DWORD)wheel);
    LeaveCriticalSection(&g_lock);
}

void MouseButton(int index, bool down)
{
    if (!g_active || index < 0 || index >= 8) return;
    EnterCriticalSection(&g_lock);
    g_buttons[index] = down ? 0x80 : 0;
    Queue(g_mouseQueue, 256, g_mRead, g_mWrite, DIMOFS_BUTTON0 + index, down ? 0x80 : 0);
    LeaveCriticalSection(&g_lock);
}

void Stats(char* buf, size_t size)
{
    _snprintf_s(buf, size, _TRUNCATE, "DirectInput: %d device(s) wrapped (mouse %d), GetDeviceState %u / GetDeviceData %u calls (%u failed), %u mouse reads, %u events injected",
                g_deviceCount, MouseActive(), g_stateCalls, g_dataCalls, g_failed, g_mouseReads, g_injected);
    g_stateCalls = g_dataCalls = g_failed = g_mouseReads = 0;
}

} // namespace dihook
