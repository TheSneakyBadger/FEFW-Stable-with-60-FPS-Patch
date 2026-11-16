// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fefw_restart.h"

#include <cstdlib>
#include <optional>

#include <string>

#include <QCoreApplication>
#include <QDir>
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

    // Started without this process's handles: Qt's startDetached let the new one inherit them
    // (the log, the MFG Unlocker's log), so it couldn't open its own log or the MFG Unlocker's,
    // however long it waited, and the MFG Unlocker fell back to 2x.
    const auto quote = [](const QString& argument) {
        QString quoted = argument;
        quoted.replace(QStringLiteral("\""), QStringLiteral("\\\""));
        return QStringLiteral("\"") + quoted + QStringLiteral("\"");
    };
    QString command_line = quote(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()));
    for (const QString& argument : *requested) {
        command_line += QLatin1Char(' ') + quote(argument);
    }
    std::wstring command = command_line.toStdWString();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                       &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
#else
    QProcess::startDetached(QCoreApplication::applicationFilePath(), *requested);
#endif
    static_cast<void>(exit_code);
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
