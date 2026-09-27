@echo off
rem 编译判定条 / 面板的离线预览（输出 PNG 到 out\），需要 Visual Studio 2022
setlocal
cd /d "%~dp0"
where cl >nul 2>nul || (
    for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VS=%%i"
    if not defined VS (echo Visual Studio not found & exit /b 1)
    call "%%VS%%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
)
cl /nologo /O2 /std:c++17 /utf-8 /EHsc /W3 /DUNICODE /D_UNICODE /I..\..\src preview.cpp ..\..\src\overlay.cpp /link user32.lib gdi32.lib gdiplus.lib dwmapi.lib /SUBSYSTEM:CONSOLE
