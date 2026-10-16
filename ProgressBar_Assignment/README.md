# Progress Bar App

## 1. How to build the project

From a Developer Command Prompt, or by double-clicking the script (it locates Visual Studio itself):

```text
build.bat
```

The script builds Release x64 and copies the executable to `bin\ProgressBarApp.exe`.

In Visual Studio, open `ProgressBarApp.sln`, select **Release | x64**, and build.

CMake alternative, from a prompt where `cl` is already available:

```text
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## 2. Dependencies and build instructions

- Windows 10 or Windows 11.
- Visual Studio 2022 with the **Desktop development with C++** workload (toolset v143, Windows 10 SDK).
- No third-party libraries. The project uses Windows system libraries (`user32`, `gdi32`, `comctl32`), which are present on a normal Windows 10 or Windows 11 machine.
- The executable and `ProgressBarLib` are built with the static C runtime (`/MT`), so the exe does not need the Visual C++ redistributable.
- `app.manifest` is embedded in the executable at link time (common controls v6 and per-monitor DPI awareness). The static library does not embed a manifest.

The dialog is a static library (`ProgressBarLib`). The console program in `src\main.cpp` links that library and calls `ProgressWindow`.

Files required to build: `ProgressBarApp.sln`, `ProgressBarApp.vcxproj`, `ProgressBarLib.vcxproj`, `app.manifest`, `build.bat`, `include\ProgressWindow.h`, and `src\`. `CMakeLists.txt` is an optional second build path. It builds the same static library under the target name `ProgressBar`.

## Using the library in another Windows project

After `build.bat`, the library file is `bin\x64\Release\ProgressBarLib.lib`.

1. Add this project's `include` directory to the include path and `#include "ProgressWindow.h"`.
2. Link `ProgressBarLib.lib`. On MSVC, `user32`, `gdi32`, and `comctl32` are pulled in by the library.
3. Build that project with the static CRT (`/MT` for Release, `/MTd` for Debug), the same runtime this library uses.
4. Embed `app.manifest` (or another manifest that requests common controls v6). Without it, the bar draws unthemed.

```cpp
#include "ProgressWindow.h"

ProgressWindow progress;
if (progress.show()) {
    progress.updateProgress(40);
    if (progress.isCancelled()) {
        // worker should stop
    }
    progress.closeWindow();
}
```

## 3. GUI framework

No third-party GUI toolkit. The window is Win32 (`CreateWindowEx`) with the standard `PROGRESS_CLASS` control from `comctl32`.
