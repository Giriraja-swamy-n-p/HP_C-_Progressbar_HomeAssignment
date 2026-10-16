#include "ProgressWindow.h"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")

// ---------------------------------------------------------------------------
// How this file is organized
//
//   1. Small helpers for position and Z-order.
//   2. ProgressWindow::Impl, the real window.
//   3. The public API: show, updateProgress, closeWindow, isCancelled.
//   4. The UI thread and its message loop.
//   5. Creating the label, bar, and Cancel button.
//   6. keepWithConsole(): the rule that ties the dialog to the console.
// ---------------------------------------------------------------------------

namespace {

constexpr UINT WM_APP_PROGRESS = WM_APP + 1;
constexpr UINT WM_APP_CLOSE = WM_APP + 2;
constexpr UINT_PTR kWatchTimerId = 1;

constexpr int kClientWidth = 460;
constexpr int kClientHeight = 176;
constexpr int IDC_LABEL = 101;
constexpr int IDC_PROGRESS = 102;
constexpr int IDC_CANCEL = 103;

// Move in Z-order only. Do not activate the window and do not make it topmost.
constexpr UINT kReorderOnly = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;

// SetWinEventHook cannot carry a user pointer. Only one dialog exists at a time.
std::atomic<void*> g_activeDialog{nullptr};

int Scale(int pixels, int dpi) {
    return MulDiv(pixels, dpi, 96);
}

bool IsAbove(HWND upper, HWND lower) {
    if (upper == nullptr || lower == nullptr || upper == lower) {
        return false;
    }
    HWND cursor = upper;
    for (int guard = 0; guard < 4096 && cursor != nullptr; ++guard) {
        cursor = GetWindow(cursor, GW_HWNDNEXT);  // next window down in Z-order
        if (cursor == lower) {
            return true;
        }
    }
    return false;
}

bool IsTopmost(HWND hwnd) {
    if (hwnd == nullptr) {
        return false;
    }
    return (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
}

bool IsLargeEnough(const RECT& rect) {
    return (rect.right - rect.left) >= 80 && (rect.bottom - rect.top) >= 80;
}

// GetConsoleWindow() is not always the window the user sees.
// Classic console: it is the visible console.
// Windows Terminal: it is a 0x0 PseudoConsoleWindow whose owner is the terminal.
HWND VisibleConsole(HWND console, HWND dialog) {
    if (console == nullptr) {
        return nullptr;
    }
    const HWND root = GetAncestor(console, GA_ROOTOWNER);
    if (root != nullptr && root != dialog && IsWindow(root)) {
        return root;
    }
    return console;
}

bool ForegroundIsOurs(HWND foreground, HWND dialog, HWND console, HWND host) {
    const HWND root = foreground != nullptr ? GetAncestor(foreground, GA_ROOT) : nullptr;
    return foreground == dialog || foreground == console || foreground == host ||
           root == dialog || root == console || root == host;
}

// Center the dialog on the visible console and keep it inside the monitor.
// Centering on the 0x0 pseudoconsole used to place the dialog off the screen.
void CenterOnVisibleConsole(HWND dialog, HWND console) {
    RECT dialogRect{};
    if (dialog == nullptr || !GetWindowRect(dialog, &dialogRect)) {
        return;
    }
    const int width = dialogRect.right - dialogRect.left;
    const int height = dialogRect.bottom - dialogRect.top;
    if (width <= 0 || height <= 0) {
        return;
    }

    RECT target{};
    const HWND host = VisibleConsole(console, dialog);
    const bool hostIsVisible = host != nullptr && GetWindowRect(host, &target) && IsLargeEnough(target);
    const bool consoleIsVisible = !hostIsVisible && console != nullptr && GetWindowRect(console, &target) && IsLargeEnough(target);
    if (!hostIsVisible && !consoleIsVisible) {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &target, 0);
    }

    int x = target.left + ((target.right - target.left) - width) / 2;
    int y = target.top + ((target.bottom - target.top) - height) / 2;

    POINT anchor{(target.left + target.right) / 2, (target.top + target.bottom) / 2};
    HMONITOR monitor = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor != nullptr && GetMonitorInfoW(monitor, &info)) {
        const RECT& work = info.rcWork;
        const int left = static_cast<int>(work.left);
        const int top = static_cast<int>(work.top);
        const int right = static_cast<int>(work.right);
        const int bottom = static_cast<int>(work.bottom);
        x = std::clamp(x, left, std::max(left, right - width));
        y = std::clamp(y, top, std::max(top, bottom - height));
    }

    SetWindowPos(dialog, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

bool MostlyOnScreen(HWND dialog) {
    RECT rect{};
    if (!GetWindowRect(dialog, &rect)) {
        return false;
    }
    const HMONITOR monitor = MonitorFromWindow(dialog, MONITOR_DEFAULTTONULL);
    if (monitor == nullptr) {
        return false;
    }
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return false;
    }
    RECT visible{};
    if (!IntersectRect(&visible, &rect, &info.rcWork)) {
        return false;
    }
    const int visibleArea = (visible.right - visible.left) * (visible.bottom - visible.top);
    const int totalArea = (rect.right - rect.left) * (rect.bottom - rect.top);
    return totalArea > 0 && visibleArea * 2 >= totalArea;
}

// Put `dialog` immediately above `anchor`.
// SetWindowPos's second argument is the window that stays ABOVE the one we move,
// so passing `anchor` itself would place the dialog underneath it.
void MoveJustAbove(HWND dialog, HWND anchor) {
    if (dialog == nullptr || anchor == nullptr || IsAbove(dialog, anchor)) {
        return;
    }
    HWND aboveAnchor = GetWindow(anchor, GW_HWNDPREV);
    if (aboveAnchor == nullptr || aboveAnchor == dialog || IsTopmost(aboveAnchor)) {
        SetWindowPos(dialog, HWND_TOP, 0, 0, 0, 0, kReorderOnly);
        return;
    }
    SetWindowPos(dialog, aboveAnchor, 0, 0, 0, 0, kReorderOnly);
}

}  // namespace

struct ProgressWindow::Impl {
    std::thread uiThread;
    std::atomic<DWORD> uiThreadId{0};
    std::atomic<bool> cancelled{false};
    std::atomic<bool> closePosted{false};

    std::mutex readyMutex;
    std::condition_variable readyCv;
    bool ready = false;
    bool failed = false;

    HWND hwnd = nullptr;
    HWND console = nullptr;
    HWND label = nullptr;
    HWND progressBar = nullptr;
    HWND cancelButton = nullptr;
    HFONT font = nullptr;
    HWINEVENTHOOK foregroundHook = nullptr;
    bool closingFromCode = false;
    bool hiddenBecauseConsoleMinimized = false;
    int percent = 0;
    int insideKeepWithConsole = 0;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static void CALLBACK OnForegroundChanged(HWINEVENTHOOK, DWORD event, HWND, LONG objectId, LONG childId, DWORD, DWORD);

    void uiThreadMain();
    bool registerWindowClass();
    bool createWindow();
    void ownTheConsole();
    void applyFont();
    void layoutControls();
    void showPercent(int value);
    void requestCancel();
    void keepWithConsole();
    void destroyFont();
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

ProgressWindow::ProgressWindow() : impl_(std::make_unique<Impl>()) {}

ProgressWindow::~ProgressWindow() {
    closeWindow();
}

bool ProgressWindow::show() {
    if (impl_->hwnd != nullptr) {
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(impl_->readyMutex);
        impl_->ready = false;
        impl_->failed = false;
    }

    impl_->uiThread = std::thread([this] { impl_->uiThreadMain(); });

    std::unique_lock<std::mutex> lock(impl_->readyMutex);
    impl_->readyCv.wait(lock, [this] { return impl_->ready || impl_->failed; });
    return impl_->ready && impl_->hwnd != nullptr;
}

void ProgressWindow::updateProgress(int percent) {
    percent = std::clamp(percent, 0, 100);
    const DWORD threadId = impl_->uiThreadId.load();
    // Post to the UI thread. The worker must not call SendMessage on the bar.
    if (threadId != 0) {
        PostThreadMessageW(threadId, WM_APP_PROGRESS, static_cast<WPARAM>(percent), 0);
    }
}

void ProgressWindow::closeWindow() {
    if (impl_->closePosted.exchange(true)) {
        if (impl_->uiThread.joinable()) {
            impl_->uiThread.join();
        }
        return;
    }

    const DWORD threadId = impl_->uiThreadId.load();
    if (threadId != 0) {
        PostThreadMessageW(threadId, WM_APP_CLOSE, 0, 0);
    }
    if (impl_->uiThread.joinable()) {
        impl_->uiThread.join();
    }
}

bool ProgressWindow::isCancelled() const {
    return impl_->cancelled.load();
}

// ---------------------------------------------------------------------------
// UI thread
// ---------------------------------------------------------------------------

void ProgressWindow::Impl::uiThreadMain() {
    uiThreadId.store(GetCurrentThreadId());

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    if (!registerWindowClass() || !createWindow()) {
        std::lock_guard<std::mutex> lock(readyMutex);
        failed = true;
        readyCv.notify_all();
        uiThreadId.store(0);
        return;
    }

    ownTheConsole();
    layoutControls();
    applyFont();
    showPercent(0);

    g_activeDialog.store(this);
    foregroundHook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND,
        EVENT_SYSTEM_FOREGROUND,
        nullptr,
        &Impl::OnForegroundChanged,
        0,
        0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNTHREAD);
    SetTimer(hwnd, kWatchTimerId, 150, nullptr);
    keepWithConsole();

    {
        std::lock_guard<std::mutex> lock(readyMutex);
        ready = true;
    }
    readyCv.notify_all();

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        // Messages posted with PostThreadMessage have a null hwnd.
        // GetMessage delivers them here; DispatchMessage does not.
        if (message.hwnd == nullptr && message.message == WM_APP_PROGRESS) {
            showPercent(static_cast<int>(message.wParam));
            continue;
        }
        if (message.hwnd == nullptr && message.message == WM_APP_CLOSE) {
            closingFromCode = true;
            if (hwnd != nullptr) {
                DestroyWindow(hwnd);
            }
            continue;
        }
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE && hwnd != nullptr &&
            (message.hwnd == hwnd || IsChild(hwnd, message.hwnd))) {
            requestCancel();
            continue;
        }
        if (hwnd == nullptr || !IsDialogMessageW(hwnd, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    g_activeDialog.store(nullptr);
    if (foregroundHook != nullptr) {
        UnhookWinEvent(foregroundHook);
        foregroundHook = nullptr;
    }
    uiThreadId.store(0);
}

// ---------------------------------------------------------------------------
// Window contents
// ---------------------------------------------------------------------------

bool ProgressWindow::Impl::registerWindowClass() {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &Impl::WndProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    windowClass.lpszClassName = L"ProgressBarApp.ProgressWindow";

    if (RegisterClassExW(&windowClass) != 0) {
        return true;
    }
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool ProgressWindow::Impl::createWindow() {
    console = GetConsoleWindow();

    int dpi = 96;
    if (console != nullptr && IsWindow(console)) {
        dpi = std::max(96, static_cast<int>(GetDpiForWindow(console)));
    }

    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    const DWORD exStyle = WS_EX_WINDOWEDGE;
    RECT bounds{0, 0, Scale(kClientWidth, dpi), Scale(kClientHeight, dpi)};
    AdjustWindowRectEx(&bounds, style, FALSE, exStyle);

    // Popup, not WS_CHILD. A child would be clipped inside the console.
    // The console handle passed here becomes the owner. ownTheConsole() sets
    // that link again with SetWindowLongPtr, which is the call the exercise asks for.
    hwnd = CreateWindowExW(
        exStyle,
        L"ProgressBarApp.ProgressWindow",
        L"Progress",
        style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        bounds.right - bounds.left,
        bounds.bottom - bounds.top,
        console,
        nullptr,
        GetModuleHandleW(nullptr),
        this);

    if (hwnd == nullptr && console != nullptr) {
        hwnd = CreateWindowExW(
            exStyle,
            L"ProgressBarApp.ProgressWindow",
            L"Progress",
            style,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            bounds.right - bounds.left,
            bounds.bottom - bounds.top,
            nullptr,
            nullptr,
            GetModuleHandleW(nullptr),
            this);
    }
    return hwnd != nullptr;
}

void ProgressWindow::Impl::ownTheConsole() {
    if (hwnd == nullptr) {
        return;
    }
    console = GetConsoleWindow();
    if (console == nullptr || !IsWindow(console)) {
        console = nullptr;
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        return;
    }

    // On a top-level window, GWLP_HWNDPARENT sets the owner. It does not make
    // this a child. An owned window:
    //   - stays above its owner
    //   - is covered when another application is activated
    //   - comes back when the owner is activated
    //   - is not globally topmost (WS_EX_TOPMOST is never set)
    SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(console));
    CenterOnVisibleConsole(hwnd, console);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
}

void ProgressWindow::Impl::applyFont() {
    if (hwnd == nullptr) {
        return;
    }
    const int dpi = static_cast<int>(GetDpiForWindow(hwnd));
    font = CreateFontW(
        -Scale(15, dpi),
        0,
        0,
        0,
        FW_NORMAL,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
    if (font == nullptr) {
        return;
    }
    if (label != nullptr) {
        SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    if (cancelButton != nullptr) {
        SendMessageW(cancelButton, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

void ProgressWindow::Impl::layoutControls() {
    if (hwnd == nullptr) {
        return;
    }

    RECT client{};
    GetClientRect(hwnd, &client);
    const int dpi = static_cast<int>(GetDpiForWindow(hwnd));
    const int margin = Scale(16, dpi);
    const int width = std::max(1, static_cast<int>(client.right - client.left) - margin * 2);
    const int labelHeight = Scale(24, dpi);
    const int barHeight = Scale(28, dpi);
    const int buttonWidth = Scale(110, dpi);
    const int buttonHeight = Scale(28, dpi);
    const int labelY = margin;
    const int barY = labelY + labelHeight + Scale(8, dpi);
    const int buttonY = barY + barHeight + Scale(16, dpi);
    const int buttonX = std::max(margin, (static_cast<int>(client.right) - buttonWidth) / 2);

    auto ensure = [&](HWND& control, const wchar_t* className, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h) {
        if (control == nullptr) {
            control = CreateWindowExW(
                0,
                className,
                text,
                WS_CHILD | WS_VISIBLE | style,
                x,
                y,
                w,
                h,
                hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                GetModuleHandleW(nullptr),
                nullptr);
        } else {
            SetWindowPos(control, nullptr, x, y, w, h, SWP_NOZORDER);
        }
    };

    ensure(label, L"STATIC", L"Progress: 0%", SS_LEFT, IDC_LABEL, margin, labelY, width, labelHeight);
    ensure(progressBar, PROGRESS_CLASSW, nullptr, PBS_SMOOTH, IDC_PROGRESS, margin, barY, width, barHeight);
    ensure(cancelButton, L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, IDC_CANCEL, buttonX, buttonY, buttonWidth, buttonHeight);

    if (progressBar != nullptr) {
        SendMessageW(progressBar, PBM_SETRANGE32, 0, 100);
        SendMessageW(progressBar, PBM_SETPOS, static_cast<WPARAM>(percent), 0);
    }
}

void ProgressWindow::Impl::showPercent(int value) {
    percent = std::clamp(value, 0, 100);
    if (progressBar != nullptr) {
        SendMessageW(progressBar, PBM_SETPOS, static_cast<WPARAM>(percent), 0);
    }
    if (label != nullptr && !cancelled.load()) {
        const std::wstring text = L"Progress: " + std::to_wstring(percent) + L"%";
        SetWindowTextW(label, text.c_str());
    }
}

void ProgressWindow::Impl::requestCancel() {
    if (cancelled.exchange(true)) {
        return;
    }
    if (label != nullptr) {
        SetWindowTextW(label, L"Cancelling...");
    }
    if (cancelButton != nullptr) {
        EnableWindow(cancelButton, FALSE);
    }
}

void ProgressWindow::Impl::destroyFont() {
    if (font != nullptr) {
        DeleteObject(font);
        font = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Stay with the console. This is the behavior the exercise asks for.
//
//   Console active  -> dialog is directly above the console.
//   Another app     -> dialog goes behind that app. It is not always-on-top.
//   Console returns -> dialog comes back above the console.
//   Console minimized -> dialog hides, then returns when the console is restored.
// ---------------------------------------------------------------------------

void ProgressWindow::Impl::keepWithConsole() {
    if (insideKeepWithConsole > 0 || hwnd == nullptr || !IsWindow(hwnd)) {
        return;
    }
    ++insideKeepWithConsole;

    if (console == nullptr || !IsWindow(console)) {
        console = GetConsoleWindow();
    }
    const HWND host = VisibleConsole(console, hwnd);
    if (console == nullptr || host == nullptr) {
        --insideKeepWithConsole;
        return;
    }

    if (IsIconic(host) || IsIconic(console)) {
        if (IsWindowVisible(hwnd)) {
            ShowWindow(hwnd, SW_HIDE);
        }
        hiddenBecauseConsoleMinimized = true;
        --insideKeepWithConsole;
        return;
    }

    if (hiddenBecauseConsoleMinimized) {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        hiddenBecauseConsoleMinimized = false;
    }
    if (!MostlyOnScreen(hwnd)) {
        CenterOnVisibleConsole(hwnd, console);
    }

    const HWND foreground = GetForegroundWindow();
    if (ForegroundIsOurs(foreground, hwnd, console, host)) {
        MoveJustAbove(hwnd, host);
    } else {
        const HWND otherApp = foreground != nullptr ? GetAncestor(foreground, GA_ROOT) : nullptr;
        if (otherApp != nullptr && !IsTopmost(otherApp) && IsAbove(hwnd, otherApp)) {
            // Second argument stays above us, so this drops us just under that app.
            SetWindowPos(hwnd, otherApp, 0, 0, 0, 0, kReorderOnly);
        }
    }

    --insideKeepWithConsole;
}

void CALLBACK ProgressWindow::Impl::OnForegroundChanged(HWINEVENTHOOK, DWORD event, HWND, LONG objectId, LONG childId, DWORD, DWORD) {
    if (event != EVENT_SYSTEM_FOREGROUND || objectId != OBJID_WINDOW || childId != CHILDID_SELF) {
        return;
    }
    auto* dialog = static_cast<Impl*>(g_activeDialog.load());
    if (dialog != nullptr) {
        dialog->keepWithConsole();
    }
}

LRESULT CALLBACK ProgressWindow::Impl::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Impl* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        const auto* created = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<Impl*>(created->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self != nullptr) {
            self->hwnd = hwnd;
        }
    }
    if (self == nullptr) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_CANCEL && HIWORD(wParam) == BN_CLICKED) {
            self->requestCancel();
            return 0;
        }
        break;
    case WM_TIMER:
        self->keepWithConsole();
        return 0;
    case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(
            hwnd,
            nullptr,
            suggested->left,
            suggested->top,
            suggested->right - suggested->left,
            suggested->bottom - suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        self->destroyFont();
        self->applyFont();
        self->layoutControls();
        return 0;
    }
    case WM_SIZE:
        self->layoutControls();
        return 0;
    case WM_CLOSE:
        // The title-bar button cancels the task. closeWindow() sets
        // closingFromCode so a finished task is not reported as cancelled.
        if (!self->closingFromCode) {
            self->requestCancel();
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kWatchTimerId);
        self->destroyFont();
        self->hwnd = nullptr;
        self->label = nullptr;
        self->progressBar = nullptr;
        self->cancelButton = nullptr;
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
