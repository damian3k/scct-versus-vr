#include <winsock2.h>
#include <ws2tcpip.h>
#include "xrapi.h"
#include "log.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <locale.h>

namespace xrapi {

namespace {

const char* const kDir = "Z:\\tmp\\xr";
const unsigned short kInPort = 7872, kInPortAlt = 7873, kOutPort = 7278;

SOCKET g_in = INVALID_SOCKET, g_out = INVALID_SOCKET;
CRITICAL_SECTION g_lock;
Packet g_packet;
bool g_valid;
DWORD g_tick;
unsigned g_received, g_rejected;
_locale_t g_c;      // numbers are always written with '.', whatever the game's locale

bool WriteText(const char* name, const char* text)
{
    char path[MAX_PATH];
    sprintf_s(path, "%s\\%s", kDir, name);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") || !f) return false;
    fputs(text, f);
    fclose(f);
    return true;
}

// Splits on whitespace into at most `max` tokens.
int Tokenize(char* text, char** tokens, int max)
{
    int n = 0;
    for (char* p = text; *p && n < max;) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;
        tokens[n++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
        if (*p) *p++ = 0;
    }
    return n;
}

bool Number(const char* s, float* v)
{
    char* end = nullptr;
    *v = _strtof_l(s, &end, g_c);
    return end && end != s && !*end && *v == *v;
}

// [client label] 28 floats, HMD_SYNC, 19 T/F button flags (more data may follow).
bool Parse(char* text, Packet* p)
{
    char* t[80];
    int n = Tokenize(text, t, 80), i = 0;
    float probe;
    if (n > 0 && !Number(t[0], &probe)) i = 1;     // "client0" or similar
    if (n < i + 30) return false;
    float f[28];
    for (int k = 0; k < 28; k++)
        if (!Number(t[i + k], &f[k])) return false;
    float sync;
    if (!Number(t[i + 28], &sync)) return false;
    const char* b = t[i + 29];
    if (strlen(b) < 19) return false;
    memcpy(p->leftQ, f + 0, 16);
    memcpy(p->leftStick, f + 4, 8);
    memcpy(p->leftPos, f + 6, 12);
    memcpy(p->rightQ, f + 9, 16);
    memcpy(p->rightStick, f + 13, 8);
    memcpy(p->rightPos, f + 15, 12);
    memcpy(p->headQ, f + 18, 16);
    memcpy(p->headPos, f + 22, 12);
    p->ipd = f[25] > 1.0f ? f[25] * 0.001f : f[25];  // tolerate millimeters
    p->fovX = f[26];
    p->fovY = f[27];
    p->sync = (int)(sync + 0.5f);
    for (int k = 0; k < 19; k++) p->buttons[k] = b[k] == 'T';
    return true;
}

DWORD WINAPI ReceiveThread(LPVOID)
{
    char buf[2048];
    bool loggedFirst = false;
    for (;;) {
        int len = recv(g_in, buf, sizeof(buf) - 1, 0);
        if (len <= 0) {
            if (len == SOCKET_ERROR && WSAGetLastError() != WSAETIMEDOUT) Sleep(10);
            continue;
        }
        buf[len] = 0;
        if (!loggedFirst) {
            loggedFirst = true;
            Log("XrAPI: first packet: %s", buf);
        }
        Packet p;
        char copy[2048];
        memcpy(copy, buf, len + 1);
        bool ok = Parse(copy, &p);
        EnterCriticalSection(&g_lock);
        if (ok) {
            g_packet = p;
            g_valid = true;
            g_tick = GetTickCount();
            g_received++;
        } else if (g_rejected++ < 5) {
            Log("XrAPI: packet not understood: %s", buf);
        }
        LeaveCriticalSection(&g_lock);
    }
}

bool Bind(unsigned short port)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) {
        closesocket(s);
        return false;
    }
    DWORD timeout = 200;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    g_in = s;
    Log("XrAPI: listening on 127.0.0.1:%u", port);
    return true;
}

} // namespace

bool Available()
{
    return GetFileAttributesA("Z:\\tmp\\xr\\system") != INVALID_FILE_ATTRIBUTES;
}

bool Start()
{
    g_c = _create_locale(LC_NUMERIC, "C");
    InitializeCriticalSection(&g_lock);

    char system[512] = {};
    FILE* f = nullptr;
    if (!fopen_s(&f, "Z:\\tmp\\xr\\system", "rb") && f) {
        size_t n = fread(system, 1, sizeof(system) - 1, f);
        fclose(f);
        for (size_t i = 0; i < n; i++)
            if (system[i] == '\r' || system[i] == '\n') system[i] = ' ';
    }
    Log("XrAPI: headset info: %s", system[0] ? system : "(none)");

    if (!WriteText("version", "0.5") || !WriteText("vr", "VR")) {
        Log("XrAPI: cannot write Z:\\tmp\\xr\\version - is the XR API enabled for this container?");
        return false;
    }
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) { Log("XrAPI: WSAStartup failed"); return false; }
    if (!Bind(kInPort) && !Bind(kInPortAlt)) {
        Log("XrAPI: cannot listen on UDP 7872 / 7873");
        return false;
    }
    g_out = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_out == INVALID_SOCKET) { Log("XrAPI: cannot create the sending socket"); return false; }
    HANDLE th = CreateThread(nullptr, 0, ReceiveThread, nullptr, 0, nullptr);
    if (!th) return false;
    CloseHandle(th);
    Log("XrAPI 0.5 started");
    return true;
}

bool Latest(Packet* out, DWORD* ageMs, unsigned* received)
{
    EnterCriticalSection(&g_lock);
    bool valid = g_valid;
    if (valid) *out = g_packet;
    if (ageMs) *ageMs = GetTickCount() - g_tick;
    if (received) *received = g_received;
    LeaveCriticalSection(&g_lock);
    return valid;
}

void SendState(int vrMode, int mode3D, float fovX, float fovY)
{
    if (g_out == INVALID_SOCKET) return;
    char text[96];
    int n = _sprintf_s_l(text, sizeof(text), "0.000 0.000 %d %d %.3f %.3f", g_c, vrMode, mode3D, fovX, fovY);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(kOutPort);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(g_out, text, n, 0, (sockaddr*)&a, sizeof(a));
}

} // namespace xrapi
