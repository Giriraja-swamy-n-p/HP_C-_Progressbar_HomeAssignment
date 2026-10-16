# Design: Console Host with a Dependent Progress Window

## 1. Requirements

The program is a console application with two menu actions: start a process, or exit.

Starting a process:

- Simulates work from 0% to 100% on a background thread.
- Shows a separate window with a progress bar and a Cancel button.
- Updates that bar as the work advances.
- Stops the work as soon as the user cancels.
- Prints `Process completed.` or `Process cancelled.` on the console.
- Closes the progress window afterward.

The window is tied to the console, not to the whole desktop:

- When the console is the active application, the progress window is visible above it and can be clicked.
- When the user activates another application, the progress window is covered with the console. It must not use a global always-on-top style.
- When the user returns to the console, the progress window comes back above it.
- Minimizing the console hides the progress window.

Required operations, all safe to call from the worker thread:

| Operation | Behavior |
| --- | --- |
| `updateProgress(int percent)` | Clamps to 0..100 and updates the bar. |
| `closeWindow()` | Closes the window and waits until the UI thread has exited. |
| `isCancelled()` | Returns true after Cancel, Escape, or the title-bar close button. |

The shipped binary must start on a clean 64-bit Windows machine with no installer and no Visual C++ redistributable.

## Where to look in the code

Read the files in this order:

1. `src/main.cpp` — menu, worker thread, and the completed/cancelled message.
2. `include/ProgressWindow.h` — the four calls the rest of the program uses. This is the public header of the static library.
3. `src/ProgressWindow.cpp` — the dialog. The public methods are near the top. `ownTheConsole()` links the dialog to the console. `keepWithConsole()` is the Z-order rule.

`obj\` is compiler output. It is not part of the program.

## 2. Architecture

Three threads cooperate. Only the UI thread touches window handles.

```text
 main thread                         UI thread                         worker thread
 -----------                         ---------                         --------------
 menu loop
   show() -------------------------> create owned popup
                                     message loop  <----------------- updateProgress()
                                     apply PBM_SETPOS                   (posted message)
   wait for worker ----------------+
                                    |
                                    +------------ run 0..100 ---------> isCancelled()?
   closeWindow() -----------------> WM_APP_CLOSE
   print result
```

`main` owns the menu and the lifetime of one `ProgressWindow`. The window object starts a UI thread inside `show()` and does not return until the window is on screen. The simulated process then runs on a second thread so the bar stays responsive while the work sleeps. `main` joins that thread, then calls `closeWindow()`, which posts a close to the UI thread and joins it.

Progress updates are not painted from the worker. `updateProgress` posts `WM_APP_PROGRESS` to the UI thread queue. `PostThreadMessage` is used instead of a raw `HWND` so a late update cannot hit a handle the UI thread has already destroyed. The UI thread applies `PBM_SETPOS` and refreshes the percent label.

Cancellation is an `std::atomic<bool>` set by the UI thread and polled by the worker every 20 ms. The window stays up, with the label set to "Cancelling...", until the worker notices and `main` calls `closeWindow()`. Closing the window from the title bar sets the same flag. A programmatic close sets a separate flag so a finished run is not reported as cancelled.

## 3. Components

### Console host (`src/main.cpp`)

Prints the menu, reads a line, and either exits or runs one process to completion. It does not create windows or call Win32 UI functions. The dummy process holds each percent for 80 ms (about eight seconds for a full run) and aborts on the next poll after cancel.

### `ProgressWindow` (`include/ProgressWindow.h`, `src/ProgressWindow.cpp`)

Public surface used by the host. The Win32 types stay in the `.cpp` behind a private `Impl`, so the console code does not depend on `windows.h`.

`Impl` is responsible for:

- Registering a window class and creating the popup, label, progress bar, and Cancel button.
- Linking the popup to the console as an owned window.
- Running the message loop, including keyboard navigation via `IsDialogMessage`.
- Marshalling progress and close requests onto the UI thread.
- Keeping Z-order aligned with the console.
- Publishing the cancel flag.

The window class is a normal top-level popup (`WS_POPUP | WS_CAPTION | WS_SYSMENU`). It is not `WS_CHILD`.

## 4. Why the window is owned, not a child

The brief asks for child/parent linking, for example `SetWindowLongPtr`, so the GUI is logically tied to the console.

`SetParent()` plus `WS_CHILD` would clip the progress UI to the console's client area and would scroll/resize with that client. A console window is not a usable parent for that: its client area is the text buffer, and on current Windows the visible console belongs to the console host. A child also cannot float above the console frame, which is where a progress dialog needs to be.

The matching Win32 mechanism is **ownership**. `CreateWindowEx` is called with the console `HWND` as `hwndParent` while the style is a popup, not `WS_CHILD`. After creation the link is set again with:

```text
SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, (LONG_PTR)GetConsoleWindow());
```

On a top-level window, `GWLP_HWNDPARENT` changes the owner. Documented consequences, and the ones this program relies on:

- The owned window stays above its owner in Z-order.
- Another application's window can cover both. Ownership is not `WS_EX_TOPMOST`.
- Activating the owner brings the owned window forward with it, still above the owner.
- The owned window is hidden while the owner is minimized, and destroyed if the owner is destroyed.

`WS_EX_TOPMOST` and `HWND_TOPMOST` are never used.

### Z-order fallback

The console window is not always the window the user is looking at. `GetConsoleWindow()` is still the owner passed to `SetWindowLongPtr`:

- Classic console host: that HWND is the visible console.
- Windows Terminal: that HWND is a `PseudoConsoleWindow` in this process, and it is already owned by the Terminal window (`CASCADIA_HOSTING_WINDOW_CLASS`). Owning the pseudoconsole joins the Terminal's owner chain. `GetAncestor(console, GA_ROOTOWNER)` is the window used for Z-order and minimize checks.

A foreground hook plus a 150 ms timer call the same `keepWithConsole()` routine:

1. If the visible console host is minimized, hide the progress window.
2. If the foreground window is the console host, the pseudoconsole, or the progress window, place the progress window immediately above the visible host.
3. If some other application is foreground and the progress window is covering it, move the progress window to just below that application.

Step 2 uses `GetWindow(host, GW_HWNDPREV)` and `SetWindowPos`. In Win32, `hWndInsertAfter` is the window that **precedes** (stays above) the window being moved. Passing the host itself would put the progress window *under* the console. The code therefore inserts after the host's predecessor, or uses `HWND_TOP` when the host is already at the top of the non-topmost band. `HWND_TOP` only raises within the normal band; it does not pin the window above other applications after they are activated.

`SWP_NOACTIVATE` is set so restacking does not steal keyboard focus from the console.

The hook is `SetWinEventHook(EVENT_SYSTEM_FOREGROUND)` with `WINEVENT_OUTOFCONTEXT`, so it runs on the UI thread's message pump and does not need an injected DLL. `WINEVENT_SKIPOWNTHREAD` avoids reacting to Z-order changes this thread just made.

## 5. Technologies

| Choice | Why |
| --- | --- |
| C++17 | `std::thread`, `std::atomic`, and `std::clamp` cover the concurrency without extra libraries. |
| Win32 + `comctl32` progress bar | The ownership and Z-order rules are Win32 primitives. A toolkit window would only wrap the same calls, and would add DLLs to ship. |
| Common Controls v6 manifest | Gives the themed progress bar. Without it, `comctl32` v5 draws the classic flat bar. |
| Per-monitor DPI manifest | Keeps the dialog the right size on scaled displays. Layout is scaled with `GetDpiForWindow`. |
| Static CRT (`/MT`) | The Release exe does not depend on `vcruntime140.dll` or `msvcp140.dll`. |
| Visual Studio 2022 vcxproj, plus CMake | The solution is what the submission asked for. CMake is a second way to build the same sources. |
| No Qt, MFC, wxWidgets, or .NET | Each one either needs a runtime install or hides the owner-window behavior the exercise is about. |

`user32`, `gdi32`, and `comctl32` are operating-system components, not redistributables.

## 6. Cross-platform

This program is Windows-only.

The behavior depends on console `HWND` ownership, `SetWindowPos` Z-order, and WinEvents. Those do not exist on macOS or Linux in a form that can attach a window to the terminal emulator the same way. A portable version would need a platform interface: Win32 on Windows, and on other systems a window that is raised only while the terminal process is the active application (for example an accessibility or focus observer). The console menu, the worker thread, and the `updateProgress` / `closeWindow` / `isCancelled` API could stay the same behind that interface. Shipping that abstraction was out of scope; the interesting constraint is the Win32 Z-order rule, and implementing it directly keeps the sample small and statically linked.

## 7. Build and deployment

`ProgressWindow` is compiled into the static library `ProgressBarLib.lib`. The console program links that library and does not compile the dialog itself. Release x64 is built with `/MT`, `/SUBSYSTEM:CONSOLE`, and `app.manifest` embedded. `build.bat` produces `bin\ProgressBarApp.exe`.

No DLL is shipped next to the exe. A clean Windows 10 or Windows 11 x64 machine can run it by double-click. Another Windows project uses the same dialog by including `include/ProgressWindow.h` and linking `ProgressBarLib.lib`, with the static CRT and a common-controls v6 manifest.

## 8. Limitations

- The Z-order fallback polls every 150 ms in addition to the foreground hook. A focus change is applied when the foreground event arrives; minimize and restore are also corrected on the timer if ownership did not already hide the window.
- A console host that does not own `GetConsoleWindow()` and is not its root owner will not be tracked. Classic conhost and Windows Terminal are the two hosts this was written for.
- One progress window exists at a time. The menu does not start a second process until the current one returns.
- The workload is a sleep loop, not real work. Replacing `runSimulatedProcess` does not require UI changes as long as the replacement calls the same three methods.
