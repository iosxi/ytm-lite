@echo off
rem Build ytm-lite.exe with MSVC (VS 2022 Build Tools). Output: ytm-lite.exe here.
rem /MT links the CRT statically, so the exe needs no runtime install.
rem
rem   build.cmd          release build -> ytm-lite.exe
rem   build.cmd debug    debug build   -> build\ytm-lite-debug.exe
rem
rem ASCII only and CRLF on purpose: cmd reads .cmd files in the OEM code page.
setlocal
cd /d "%~dp0"
if defined VCINSTALLDIR goto build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || exit /b 1
:build
if not exist build\obj mkdir build\obj
rc /nologo /fo build\obj\ytm-lite.res src\ytm-lite.rc || exit /b 1
set "SRC=src\main.c src\ui.c src\slider.c src\ffcookie.c src\worker.c src\yt.c src\player.c src\net.c src\json.c src\config.c src\theme.c src\util.c"
set "LIBS=user32.lib gdi32.lib comctl32.lib shell32.lib ole32.lib advapi32.lib"
set "CFLAGS=/nologo /utf-8 /W4 /MT /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00"
if /i "%~1"=="debug" goto debug
cl %CFLAGS% /O1 /GS- /Gy /Fobuild\obj\ /Feytm-lite.exe %SRC% build\obj\ytm-lite.res /link /SUBSYSTEM:WINDOWS /MANIFEST:NO /OPT:REF /OPT:ICF %LIBS% || exit /b 1
for %%F in (ytm-lite.exe) do echo OK: ytm-lite.exe (%%~zF bytes)
exit /b 0
:debug
cl %CFLAGS% /Zi /Od /Fobuild\obj\ /Fdbuild\obj\ /Febuild\ytm-lite-debug.exe %SRC% build\obj\ytm-lite.res /link /SUBSYSTEM:WINDOWS /MANIFEST:NO /DEBUG %LIBS% || exit /b 1
echo OK: build\ytm-lite-debug.exe
