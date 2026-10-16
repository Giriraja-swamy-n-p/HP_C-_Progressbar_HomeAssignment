@echo off
setlocal EnableExtensions

REM Builds Release x64 with the static CRT and copies the exe to bin\ProgressBarApp.exe.
cd /d "%~dp0"

where msbuild >nul 2>&1
if %ERRORLEVEL%==0 goto :build

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio C++ tools were not found.
    echo Install the "Desktop development with C++" workload, then run this script from a Developer Prompt,
    echo or configure with CMake and pass -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded.
    exit /b 1
)

for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VSINSTALL=%%I"
)

if not defined VSINSTALL (
    echo Visual Studio was found, but the C++ toolset is not installed.
    exit /b 1
)

call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo Failed to initialize the Visual Studio build environment.
    exit /b 1
)

:build
msbuild "%~dp0ProgressBarApp.sln" /m /p:Configuration=Release /p:Platform=x64 /v:minimal
if errorlevel 1 exit /b 1

if not exist "%~dp0bin" mkdir "%~dp0bin"
copy /Y "%~dp0bin\x64\Release\ProgressBarApp.exe" "%~dp0bin\ProgressBarApp.exe" >nul
echo.
echo Built: %~dp0bin\ProgressBarApp.exe
exit /b 0
