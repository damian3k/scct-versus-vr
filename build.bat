@echo off
setlocal
cd /d "%~dp0"

set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VCVARS%" goto havevc
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars32.bat"
:havevc
call "%VCVARS%" >nul || exit /b 1

if not exist obj mkdir obj
if not exist bin mkdir bin

cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   /I deps\openxr\include src\*.cpp /LD /Fo:obj\ /Fe:bin\d3d9.dll ^
   /link /DEF:src\d3d9.def /SAFESEH:NO ^
   deps\openxr\Win32\lib\openxr_loader.lib d3d12.lib dxgi.lib user32.lib ws2_32.lib delayimp.lib ^
   /DELAYLOAD:openxr_loader.dll || exit /b 1

copy /y deps\openxr\Win32\bin\openxr_loader.dll bin\ >nul
copy /y SCCT_VR.ini bin\ >nul
echo Build OK: %~dp0bin
