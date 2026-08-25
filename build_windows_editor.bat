@echo off
setlocal EnableExtensions
cd /d "%~dp0"

rem Incremental Windows Editor build. Extra scons flags can be passed through:
rem   build_windows_editor.bat compiledb=yes
rem SCons itself is incremental; this script does not clean.

set "SCONS_CMD="
where scons >nul 2>nul
if %ERRORLEVEL%==0 (
	set "SCONS_CMD=scons"
	goto :run
)
where python >nul 2>nul
if %ERRORLEVEL%==0 (
	python -c "import SCons" >nul 2>nul
	if %ERRORLEVEL%==0 (
		set "SCONS_CMD=python -m SCons"
		goto :run
	)
)

if exist "%USERPROFILE%\.workbuddy\binaries\python\envs\default\Scripts\scons.exe" (
	set "SCONS_CMD=%USERPROFILE%\.workbuddy\binaries\python\envs\default\Scripts\scons.exe"
	goto :run
)

echo Could not find scons or python in PATH.
exit /b 1

:run

echo Building Godot Windows Editor from %CD%
echo Command: %SCONS_CMD% platform=windows target=editor windows_subsystem=console %*
%SCONS_CMD% platform=windows target=editor windows_subsystem=console %*
exit /b %ERRORLEVEL%
