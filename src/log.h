#pragma once
#include <windows.h>

// Directory containing this DLL (the game's System folder), with trailing backslash.
const wchar_t* ModuleDir();

void LogInit();
void Log(const char* fmt, ...);
