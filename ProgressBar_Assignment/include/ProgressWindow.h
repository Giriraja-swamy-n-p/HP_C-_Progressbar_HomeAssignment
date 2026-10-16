#pragma once

#include <memory>

// Progress dialog tied to the console window.
//
// Call these from the worker thread. They do not touch the controls themselves.
// The window lives on its own UI thread.
//
//   show()            create the dialog and wait until it is visible
//   updateProgress()  move the bar (0..100)
//   isCancelled()     true after Cancel, Escape, or the title-bar close button
//   closeWindow()     close the dialog and wait until the UI thread has exited
//
// While the console is the active application, the dialog stays above it.
// Switching to another application covers the dialog as well. It is not
// always-on-top.
class ProgressWindow {
public:
    ProgressWindow();
    ~ProgressWindow();

    ProgressWindow(const ProgressWindow&) = delete;
    ProgressWindow& operator=(const ProgressWindow&) = delete;

    bool show();
    void updateProgress(int percent);
    void closeWindow();
    bool isCancelled() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
