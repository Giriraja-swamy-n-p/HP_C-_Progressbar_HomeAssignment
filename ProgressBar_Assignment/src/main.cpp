#include "ProgressWindow.h"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

// Console menu.
//
//   1. Start Process  -> show the progress dialog and run a fake task
//   2. Exit
//
// The fake task runs on a worker thread so the dialog stays responsive.
// That thread calls updateProgress() and isCancelled(). Only the UI thread
// paints the window.

namespace {

constexpr int kStepMs = 80;       // time spent on each percent; full run is about 8 seconds
constexpr int kCancelCheckMs = 20;

std::string trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

void printMenu() {
    std::cout << "\nProcess Runner\n"
              << "1. Start Process\n"
              << "2. Exit\n"
              << "Select an option: " << std::flush;
}

// Returns true when the task reaches 100%. Returns false when the user cancels.
bool runFakeTask(ProgressWindow& progress) {
    for (int percent = 0; percent <= 100; ++percent) {
        if (progress.isCancelled()) {
            return false;
        }
        progress.updateProgress(percent);

        for (int waited = 0; waited < kStepMs; waited += kCancelCheckMs) {
            if (progress.isCancelled()) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kCancelCheckMs));
        }
    }
    return !progress.isCancelled();
}

void startProcess() {
    ProgressWindow progress;
    if (!progress.show()) {
        std::cout << "Could not open the progress window.\n";
        return;
    }

    std::cout << "Process started. Cancel it from the progress window if needed.\n";

    bool finished = false;
    std::thread worker([&progress, &finished] { finished = runFakeTask(progress); });
    worker.join();

    // Leave 100% on screen for a moment so the finished bar is visible.
    if (finished) {
        progress.updateProgress(100);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    progress.closeWindow();
    std::cout << (finished ? "Process completed.\n" : "Process cancelled.\n");
}

}  // namespace

int main() {
    std::cout << "Console host with a GUI progress bar.\n";

    for (;;) {
        printMenu();

        std::string line;
        if (!std::getline(std::cin, line)) {
            break;
        }

        const std::string choice = trim(std::move(line));
        if (choice == "1") {
            startProcess();
        } else if (choice == "2") {
            std::cout << "Exiting.\n";
            break;
        } else {
            std::cout << "Please enter 1 or 2.\n";
        }
    }

    return 0;
}
