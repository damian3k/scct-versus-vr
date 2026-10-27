// vorbisfile.dll proxy for running SCCT Versus under Wine (WinlatorXR).
// The Reloaded launcher injects Reloaded.Core.dll into a suspended game process, which fails
// under Wine + FEX. The game loads vorbisfile.dll from its own folder at startup, so this proxy
// forwards every export to vorbisfile_orig.dll and loads Reloaded.Core.dll at the same point
// in startup where the launcher's injection would have run (before the game's entry point).
#include <windows.h>
#include <cstdio>

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

BOOL WINAPI DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(mod, path, MAX_PATH);
        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash) {
            wchar_t logPath[MAX_PATH];
            wcscpy_s(logPath, path);
            wcscpy_s(logPath + (slash + 1 - path), MAX_PATH - (slash + 1 - path), L"SCCT_Quest.log");
            wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"Reloaded.Core.dll");
            HMODULE r = LoadLibraryW(path);
            DWORD err = r ? 0 : GetLastError();
            HANDLE f = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                char line[160];
                int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "vorbisfile proxy: Reloaded.Core.dll %s (error %lu)\r\n",
                                    r ? "loaded" : "FAILED to load", err);
                DWORD written;
                WriteFile(f, line, n, &written, nullptr);
                CloseHandle(f);
            }
        }
    }
    return TRUE;
}
