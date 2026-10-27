#include "log.h"
#include <cstdio>
#include <cstdarg>
#include <mutex>

extern HMODULE g_self;

static FILE* g_log;
static std::mutex g_logMutex;

const wchar_t* ModuleDir()
{
    static wchar_t dir[MAX_PATH];
    if (!dir[0]) {
        GetModuleFileNameW(g_self, dir, MAX_PATH);
        wchar_t* slash = wcsrchr(dir, L'\\');
        if (slash) slash[1] = 0;
    }
    return dir;
}

void LogInit()
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_log) return;
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%sSCCT_VR.log", ModuleDir());
    g_log = _wfsopen(path, L"w", _SH_DENYWR);
}

void Log(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}
