// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fefw_restart.h"

#include <cstdlib>
#include <optional>

#include <QCoreApplication>
#include <QProcess>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace FefwRestart {
namespace {
std::optional<QStringList> requested;
#ifdef _WIN32
constexpr wchar_t MutexName[] = L"Local\\FortunesEdenRestarting";
#endif
} // Anonymous namespace

void Request(const QStringList& arguments) {
    requested = arguments;
}

void RunIfRequested(int exit_code) {
    if (!requested) {
        return;
    }
    qputenv("FEFW_WAIT_FOR_PID",
            QByteArray::number(static_cast<qulonglong>(QCoreApplication::applicationPid())));
#ifdef _WIN32
    // Held by this process until it has gone (never closed: the system releases it then).
    void(CreateMutexW(nullptr, TRUE, MutexName));
#endif
    QProcess::startDetached(QCoreApplication::applicationFilePath(), *requested);
#ifdef _WIN32
    // Ended here rather than through the normal exit: after DLSS frame generation, the normal
    // exit went on for many seconds after the game had closed (with the log and the MFG
    // Unlocker's files still open), so the new one lost its log and the MFG Unlocker fell back
    // to 2x. Everything worth keeping (settings, saves) was written as the window closed.
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(exit_code));
#else
    void(exit_code);
#endif
}

void WaitForPrevious() {
#ifdef _WIN32
    if (const HANDLE restarting = OpenMutexW(SYNCHRONIZE, FALSE, MutexName)) {
        const DWORD result = WaitForSingleObject(restarting, 20000);
        if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) {
            ReleaseMutex(restarting);
        }
        CloseHandle(restarting);
    }
    if (const char* const wait_for = std::getenv("FEFW_WAIT_FOR_PID")) {
        const DWORD pid = static_cast<DWORD>(std::strtoul(wait_for, nullptr, 10));
        _putenv("FEFW_WAIT_FOR_PID=");
        if (pid != 0) {
            if (const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
                WaitForSingleObject(process, 20000);
                CloseHandle(process);
            }
        }
    }
#endif
}

} // namespace FefwRestart
