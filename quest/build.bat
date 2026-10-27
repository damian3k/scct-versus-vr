@echo off
setlocal
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
if not exist out mkdir out
cl /nologo /O2 /MT vorbisproxy.cpp /LD /Fo:out\ /Fe:out\vorbisfile.dll || exit /b 1
cl /nologo /O2 /MT /DUNICODE /D_UNICODE launcher.cpp /Fo:out\ /Fe:out\SCCT_Quest.exe /link /SUBSYSTEM:WINDOWS user32.lib || exit /b 1
echo Build OK
