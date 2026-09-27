@echo off
rem 打发布包：先编译，再把程序、Bridge、说明打成 dist\SBParry-<版本>.zip
rem Package a release: build, then zip the program, the bridge and the docs into dist\SBParry-<version>.zip
rem 用法 / usage:  package.bat 1.0.0
setlocal
cd /d "%~dp0"
set VER=%~1
if "%VER%"=="" set VER=dev
call build.bat || exit /b 1
set OUT=dist\SBParry-%VER%
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%\SBParryBridge\Scripts" || exit /b 1
copy /y build\sbparry.exe "%OUT%\" >nul || exit /b 1
copy /y data\calib_default.tsv "%OUT%\" >nul || exit /b 1
copy /y mod\SBParryBridge\Scripts\main.lua "%OUT%\SBParryBridge\Scripts\" >nul || exit /b 1
copy /y mod\SBParryBridge\enabled.txt "%OUT%\SBParryBridge\" >nul || exit /b 1
copy /y README.md "%OUT%\" >nul
copy /y README.en.md "%OUT%\" >nul
copy /y LICENSE "%OUT%\LICENSE.txt" >nul
if exist "%OUT%.zip" del "%OUT%.zip"
powershell -NoProfile -Command "Compress-Archive -Path '%OUT%\*' -DestinationPath '%OUT%.zip'" || exit /b 1
echo OK: %OUT%.zip
