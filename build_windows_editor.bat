@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found. Install Visual Studio with the C++ workload.
    exit /b 1
)

set "VS_PATH="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%i"
if not defined VS_PATH (
    echo ERROR: MSVC not found. Install Visual Studio Build Tools with the C++ workload.
    exit /b 1
)

echo Using Visual Studio: %VS_PATH%
call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo ERROR: Failed to initialize the MSVC environment.
    exit /b 1
)

REM Unlock the editor binary if a previous Godot editor instance is still running.
taskkill /F /IM godot.windows.editor.x86_64.exe >nul 2>&1
taskkill /F /IM godot.windows.editor.x86_64.console.exe >nul 2>&1

if "%NUMBER_OF_PROCESSORS%"=="" set "NUMBER_OF_PROCESSORS=8"
echo Incremental build: Godot Windows editor
scons platform=windows target=editor -j%NUMBER_OF_PROCESSORS% %*
if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

echo Build succeeded.
exit /b 0
