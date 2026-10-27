// SCCT_Quest.exe: starts "SCCT Versus" (the game executable has no .exe extension) from the
// same folder, without the Reloaded launcher's code injection. Reloaded is loaded by the
// vorbisfile.dll proxy instead. Everything is logged to SCCT_Quest.log next to the exe.
#include <windows.h>
#include <cstdio>
#include <cstdarg>

static wchar_t g_dir[MAX_PATH];

static void Log(const char* fmt, ...)
{
    wchar_t path[MAX_PATH];
    wsprintfW(path, L"%sSCCT_Quest.log", g_dir);
    FILE* f = _wfopen(path, L"a");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// Loads one DLL on a worker thread so a DllMain that never returns shows up as a timeout.
static DWORD WINAPI LoadOne(LPVOID p)
{
    HMODULE m = LoadLibraryW((const wchar_t*)p);
    return m ? 0 : (GetLastError() ? GetLastError() : 1);
}

// Startup diagnosis: the game hangs before its imports finish initialising under Wine,
// so try each of them here first and log which one blocks.
static void CheckImports()
{
    static const wchar_t* const kSystem[] = {
        L"msvcr70.dll", L"msvcp70.dll", L"ws2_32.dll", L"winmm.dll", L"comctl32.dll", L"comdlg32.dll",
        L"imm32.dll", L"ole32.dll", L"dinput8.dll", L"ddraw.dll", L"d3d8.dll", L"d3d9.dll",
    };
    static const wchar_t* const kLocal[] = {
        L"ogg.dll", L"vorbis.dll", L"vorbisfile_orig.dll", L"eax.dll", L"dsound.dll",
        L"window_resources_release.dll", L"unicows.dll", L"encvag.dll", L"D3DCompiler_47.dll",
    };
    wchar_t path[MAX_PATH];
    for (int pass = 0; pass < 2; pass++) {
        const wchar_t* const* list = pass ? kLocal : kSystem;
        int count = pass ? (int)(sizeof(kLocal) / sizeof(*kLocal)) : (int)(sizeof(kSystem) / sizeof(*kSystem));
        for (int i = 0; i < count; i++) {
            // Local DLLs by full path (that is what the game gets); system DLLs by name.
            if (pass) wsprintfW(path, L"%s%s", g_dir, list[i]);
            else lstrcpyW(path, list[i]);
            DWORD start = GetTickCount();
            HANDLE th = CreateThread(nullptr, 0, LoadOne, path, 0, nullptr);
            DWORD wait = WaitForSingleObject(th, 20000);
            DWORD code = 0;
            if (wait == WAIT_OBJECT_0) GetExitCodeThread(th, &code);
            CloseHandle(th);
            if (wait != WAIT_OBJECT_0) {
                Log("  %ls: HANGS (no return after 20 s) <-- this blocks the game", list[i]);
                return; // the loader lock is now held, further loads would block too
            }
            Log("  %ls: %s in %lu ms%s", list[i], code ? "FAILED" : "ok", GetTickCount() - start,
                code ? " (error code below)" : "");
            if (code) Log("    error %lu", code);
        }
    }
}

// ---- Startup debugger: runs the game under the Win32 debug API and logs DLL loads, exceptions
// and where the main thread is, to find where it stalls under Wine. Detaches after a while.

struct Module { DWORD base, size; wchar_t name[64]; };
static Module g_mods[256];
static int g_modCount;

static void AddModule(HANDLE proc, DWORD base, HANDLE file, LPVOID namePtr, BOOL unicode)
{
    if (g_modCount >= 256) return;
    Module& m = g_mods[g_modCount++];
    m.base = base;
    m.size = 0;
    lstrcpyW(m.name, L"?");
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS32 nt;
    SIZE_T got;
    if (ReadProcessMemory(proc, (LPCVOID)(ULONG_PTR)base, &dos, sizeof(dos), &got) &&
        ReadProcessMemory(proc, (LPCVOID)(ULONG_PTR)(base + dos.e_lfanew), &nt, sizeof(nt), &got))
        m.size = nt.OptionalHeader.SizeOfImage;
    wchar_t full[MAX_PATH] = L"";
    if (file && GetFinalPathNameByHandleW(file, full, MAX_PATH, 0)) {
        wchar_t* s = wcsrchr(full, L'\\');
        lstrcpynW(m.name, s ? s + 1 : full, 64);
    } else if (namePtr) {
        ULONG_PTR strPtr = 0;
        if (ReadProcessMemory(proc, namePtr, &strPtr, sizeof(DWORD), &got) && strPtr) {
            if (unicode) {
                wchar_t buf[MAX_PATH] = L"";
                ReadProcessMemory(proc, (LPCVOID)strPtr, buf, sizeof(buf) - 2, &got);
                wchar_t* s = wcsrchr(buf, L'\\');
                lstrcpynW(m.name, s ? s + 1 : buf, 64);
            }
        }
    }
}

static const wchar_t* ModuleAt(DWORD addr, DWORD* offset)
{
    for (int i = 0; i < g_modCount; i++)
        if (addr >= g_mods[i].base && addr < g_mods[i].base + g_mods[i].size) {
            *offset = addr - g_mods[i].base;
            return g_mods[i].name;
        }
    *offset = addr;
    return L"(unknown)";
}

// Prints readable text found at addr: as UTF-16 if it looks like it, else as ANSI.
static bool LogTextAt(HANDLE proc, DWORD addr, const char* label)
{
    if (addr < 0x10000) return false;
    BYTE buf[600] = {};
    SIZE_T got = 0;
    if (!ReadProcessMemory(proc, (LPCVOID)(ULONG_PTR)addr, buf, sizeof(buf) - 4, &got) || got < 8) return false;
    // UTF-16: every second byte zero for ASCII text
    int wideOk = 0;
    for (int i = 0; i + 1 < 16; i += 2) if (buf[i] >= 0x20 && buf[i] < 0x7F && buf[i + 1] == 0) wideOk++;
    if (wideOk >= 6) {
        char out[300] = {};
        int n = 0;
        for (int i = 0; i + 1 < (int)got && n < 299; i += 2) {
            WCHAR c = buf[i] | (buf[i + 1] << 8);
            if (!c) break;
            out[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : (c == L'\n' ? '|' : '?');
        }
        Log("      %s: \"%s\"", label, out);
        return true;
    }
    int printable = 0;
    for (int i = 0; i < 8; i++) if (buf[i] >= 0x20 && buf[i] < 0x7F) printable++;
    if (printable >= 6) {
        char out[300] = {};
        int n = 0;
        for (int i = 0; i < (int)got && n < 299 && buf[i]; i++) out[n++] = (buf[i] >= 0x20 && buf[i] < 0x7F) ? buf[i] : '|';
        Log("      %s: \"%s\"", label, out);
        return true;
    }
    return false;
}

static void LogThrownObject(HANDLE proc, DWORD obj)
{
    // The thrown object is either a string pointer (TCHAR* throw) or an object holding one.
    DWORD words[4] = {};
    SIZE_T got = 0;
    if (!ReadProcessMemory(proc, (LPCVOID)(ULONG_PTR)obj, words, sizeof(words), &got)) return;
    if (LogTextAt(proc, words[0], "thrown text")) return;
    if (LogTextAt(proc, obj, "thrown object text")) return;
    for (int i = 1; i < 4; i++) if (LogTextAt(proc, words[i], "thrown member text")) return;
}

static void DebugRun(PROCESS_INFORMATION& pi)
{
    HANDLE mainThread = nullptr, proc = nullptr;
    DWORD start = GetTickCount(), lastReport = start;
    unsigned exceptions = 0;
    for (;;) {
        DEBUG_EVENT ev;
        if (!WaitForDebugEvent(&ev, 1000)) {
            DWORD now = GetTickCount();
            if (mainThread && now - lastReport >= 10000) {
                lastReport = now;
                CONTEXT ctx = {};
                ctx.ContextFlags = CONTEXT_CONTROL;
                SuspendThread(mainThread);
                if (GetThreadContext(mainThread, &ctx)) {
                    DWORD off;
                    const wchar_t* mod = ModuleAt(ctx.Eip, &off);
                    Log("  [%lus] main thread at %ls+0x%lX (EIP 0x%08lX), exceptions so far: %u",
                        (now - start) / 1000, mod, off, ctx.Eip, exceptions);
                    // Return addresses near the top of the stack show the call chain.
                    DWORD stack[64];
                    SIZE_T got = 0;
                    if (ReadProcessMemory(proc, (LPCVOID)(ULONG_PTR)ctx.Esp, stack, sizeof(stack), &got)) {
                        int shown = 0;
                        for (int i = 0; i < 64 && shown < 8; i++) {
                            DWORD o2;
                            const wchar_t* m2 = ModuleAt(stack[i], &o2);
                            if (m2[0] != L'(') {
                                Log("      stack: %ls+0x%lX", m2, o2);
                                shown++;
                            }
                        }
                    }
                }
                ResumeThread(mainThread);
            }
            if (now - start > 60000) {
                Log("Detaching debugger after 60 s");
                DebugActiveProcessStop(pi.dwProcessId);
                return;
            }
            continue;
        }

        DWORD cont = DBG_CONTINUE;
        switch (ev.dwDebugEventCode) {
        case CREATE_PROCESS_DEBUG_EVENT:
            proc = ev.u.CreateProcessInfo.hProcess;
            mainThread = ev.u.CreateProcessInfo.hThread;
            AddModule(proc, (DWORD)(ULONG_PTR)ev.u.CreateProcessInfo.lpBaseOfImage, ev.u.CreateProcessInfo.hFile, nullptr, FALSE);
            lstrcpyW(g_mods[g_modCount - 1].name, L"SCCT Versus");
            Log("  process created, image at 0x%08lX", (DWORD)(ULONG_PTR)ev.u.CreateProcessInfo.lpBaseOfImage);
            if (ev.u.CreateProcessInfo.hFile) CloseHandle(ev.u.CreateProcessInfo.hFile);
            break;
        case LOAD_DLL_DEBUG_EVENT:
            AddModule(proc, (DWORD)(ULONG_PTR)ev.u.LoadDll.lpBaseOfDll, ev.u.LoadDll.hFile, ev.u.LoadDll.lpImageName, ev.u.LoadDll.fUnicode);
            Log("  [%lums] load %ls", GetTickCount() - start, g_mods[g_modCount - 1].name);
            if (ev.u.LoadDll.hFile) CloseHandle(ev.u.LoadDll.hFile);
            break;
        case EXCEPTION_DEBUG_EVENT: {
            const EXCEPTION_RECORD& er = ev.u.Exception.ExceptionRecord;
            DWORD addr = (DWORD)(ULONG_PTR)er.ExceptionAddress, off;
            exceptions++;
            if (exceptions <= 40 || !ev.u.Exception.dwFirstChance) {
                const wchar_t* mod = ModuleAt(addr, &off);
                Log("  [%lums] exception 0x%08lX at %ls+0x%lX (%s chance)", GetTickCount() - start, er.ExceptionCode,
                    mod, off, ev.u.Exception.dwFirstChance ? "first" : "SECOND");
            }
            // C++ exception (Unreal's appErrorf/appThrowf throw a pointer to the message text):
            // show what was thrown.
            if (er.ExceptionCode == 0xE06D7363 && er.NumberParameters >= 3)
                LogThrownObject(proc, (DWORD)er.ExceptionInformation[1]);
            if (er.ExceptionCode != EXCEPTION_BREAKPOINT || !ev.u.Exception.dwFirstChance)
                cont = DBG_EXCEPTION_NOT_HANDLED;
            break;
        }
        case OUTPUT_DEBUG_STRING_EVENT: {
            char buf[512] = "";
            SIZE_T got = 0;
            DWORD len = ev.u.DebugString.nDebugStringLength;
            if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
            if (!ev.u.DebugString.fUnicode)
                ReadProcessMemory(proc, ev.u.DebugString.lpDebugStringData, buf, len, &got);
            Log("  debug string: %s", buf);
            break;
        }
        case EXIT_PROCESS_DEBUG_EVENT:
            Log("Game process exited with code 0x%08lX", ev.u.ExitProcess.dwExitCode);
            ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE);
            return;
        default:
            break;
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont);
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR args, int)
{
    wchar_t exe[MAX_PATH], cmd[2048];
    GetModuleFileNameW(nullptr, g_dir, MAX_PATH);
    wchar_t* slash = wcsrchr(g_dir, L'\\');
    if (slash) slash[1] = 0;
    wsprintfW(exe, L"%sSCCT Versus", g_dir);
    wsprintfW(cmd, L"\"%s\" %s", exe, args ? args : L"");
    Log("SCCT_Quest launcher started, exe: %ls", exe);

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    // The startup debugger slows the game down; only use it when SCCT_Quest.debug exists.
    wchar_t flag[MAX_PATH];
    wsprintfW(flag, L"%sSCCT_Quest.debug", g_dir);
    bool debug = GetFileAttributesW(flag) != INVALID_FILE_ATTRIBUTES;
    if (!CreateProcessW(exe, cmd, nullptr, nullptr, FALSE, debug ? DEBUG_ONLY_THIS_PROCESS : 0, nullptr, g_dir, &si, &pi)) {
        DWORD err = GetLastError();
        Log("CreateProcess failed, error %lu", err);
        wchar_t msg[512];
        wsprintfW(msg, L"Could not start %s (error %lu)", exe, err);
        MessageBoxW(nullptr, msg, L"SCCT Quest launcher", MB_ICONERROR);
        return 1;
    }
    Log("Game process started%s, pid %lu", debug ? " under the startup debugger" : "", pi.dwProcessId);
    if (debug) DebugRun(pi);
    CloseHandle(pi.hThread);

    // Stay alive until the game ends so its exit code is recorded
    // (0xC0000135 = DLL not found, 0xC0000005 = crash, 0xC0000142 = DLL init failed).
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    Log("Game process exited with code 0x%08lX", code);
    CloseHandle(pi.hProcess);
    return 0;
}
