@echo off
rem 编译 SBParry：需要 Visual Studio 2022（含“使用 C++ 的桌面开发”）
rem Build SBParry: requires Visual Studio 2022 with "Desktop development with C++"
setlocal
cd /d "%~dp0"
rem cl / ml64 / rc 任一不在 PATH 就载入 VS 开发环境
set NEEDVS=0
where cl >nul 2>nul || set NEEDVS=1
where ml64 >nul 2>nul || set NEEDVS=1
where rc >nul 2>nul || set NEEDVS=1
if %NEEDVS%==1 (
    for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VS=%%i"
    if not defined VS (echo Visual Studio not found & exit /b 1)
    call "%%VS%%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
)
if not exist build mkdir build
ml64 /nologo /c /Fo build\hooks.obj src\hooks.asm || exit /b 1
rc /nologo /fo build\sbparry.res res\sbparry.rc || exit /b 1
cl /nologo /O2 /std:c++17 /utf-8 /EHsc /W3 /MT /DUNICODE /D_UNICODE /Fo:build\ /Fe:build\sbparry.exe src\*.cpp build\hooks.obj build\sbparry.res ^
   /link /INCREMENTAL:NO user32.lib gdi32.lib gdiplus.lib dwmapi.lib shell32.lib advapi32.lib /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup || exit /b 1
echo OK: build\sbparry.exe
