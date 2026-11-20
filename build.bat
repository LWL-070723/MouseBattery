@echo off
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSPATH="
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
)
if not defined VSPATH (
    echo [ERROR] Visual Studio C++ build tools not found.
    exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo [ERROR] vcvars64.bat failed.
    exit /b 1
)

if not exist build mkdir build
if not exist dist mkdir dist

cl /nologo /std:c++17 /utf-8 /EHsc /O2 /MT /W3 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 ^
   /Fobuild\ /Fdbuild\MouseBattery.pdb ^
   src\main.cpp /Fe:dist\MouseBattery.exe ^
   /link /SUBSYSTEM:WINDOWS comctl32.lib

if errorlevel 1 (
    echo [ERROR] Build failed.
    exit /b 1
)

echo.
echo Built: %~dp0dist\MouseBattery.exe
exit /b 0
