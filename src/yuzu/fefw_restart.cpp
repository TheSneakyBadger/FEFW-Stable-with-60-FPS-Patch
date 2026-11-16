// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fefw_restart.h"

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <optional>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QDir>
#include <QProcess>

#include "common/logging.h"

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

/// The MFG Unlocker's environment variables ("NAME=value") this process started with.
std::vector<std::wstring> mfg_environment_at_start;

bool IsMfgVariable(const std::wstring& entry) {
    return entry.starts_with(L"RTX_MFG_") || entry.starts_with(L"RTXMFG_");
}

std::vector<std::wstring> MfgEnvironment() {
    std::vector<std::wstring> entries;
    if (wchar_t* const block = GetEnvironmentStringsW()) {
        for (const wchar_t* entry = block; *entry != L'\0'; entry += wcslen(entry) + 1) {
            if (IsMfgVariable(entry)) {
                entries.emplace_back(entry);
            }
        }
        FreeEnvironmentStringsW(block);
    }
    return entries;
}

/// The MFG Unlocker sets variables of its own in the process it runs in (its state); a new
/// Fortune's Eden that inherited them read its settings as "fixed 2x" instead of following the
/// game. Back to what this one started with.
void RestoreMfgEnvironment() {
    for (const std::wstring& entry : MfgEnvironment()) {
        if (std::ranges::find(mfg_environment_at_start, entry) != mfg_environment_at_start.end()) {
            continue;
        }
        const std::wstring name = entry.substr(0, entry.find(L'='));
        LOG_INFO(Frontend, "NVIDIA DLSS: not passed on to the new Fortune's Eden: {}",
                 QString::fromStdWString(entry).toStdString());
        SetEnvironmentVariableW(name.c_str(), nullptr);
    }
    for (const std::wstring& entry : mfg_environment_at_start) {
        const size_t equals = entry.find(L'=');
        SetEnvironmentVariableW(entry.substr(0, equals).c_str(),
                                entry.substr(equals + 1).c_str());
    }
}
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

    RestoreMfgEnvironment();

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
    mfg_environment_at_start = MfgEnvironment();
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
