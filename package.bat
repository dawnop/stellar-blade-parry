@echo off
rem 打发布包：先编译，再打出 dist\sbparry.exe（单文件即可用）和 dist\SBParry-<版本>.zip（含 UE4SS 兜底用的 SBParryBridge 和说明）
rem Package a release: build, then produce dist\sbparry.exe (works on its own) and dist\SBParry-<version>.zip (plus the SBParryBridge fallback and docs)
rem 用法 / usage:  package.bat 1.0.0
setlocal
cd /d "%~dp0"
set VER=%~1
if "%VER%"=="" set VER=dev
call build.bat || exit /b 1
set NAME=SBParry-%VER%
set OUT=dist\%NAME%
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%\SBParryBridge\Scripts" || exit /b 1
copy /y build\sbparry.exe "%OUT%\" >nul || exit /b 1
copy /y build\sbparry.exe dist\ >nul || exit /b 1
copy /y mod\SBParryBridge\Scripts\main.lua "%OUT%\SBParryBridge\Scripts\" >nul || exit /b 1
copy /y mod\SBParryBridge\enabled.txt "%OUT%\SBParryBridge\" >nul || exit /b 1
copy /y README.md "%OUT%\" >nul
copy /y README.en.md "%OUT%\" >nul
copy /y LICENSE "%OUT%\LICENSE.txt" >nul
if exist "%OUT%.zip" del "%OUT%.zip"
rem Windows 10 起自带的 tar（bsdtar）打标准 zip（路径用 /）
tar -a -c -f "%OUT%.zip" -C dist "%NAME%" || exit /b 1
echo OK: dist\sbparry.exe  %OUT%.zip
