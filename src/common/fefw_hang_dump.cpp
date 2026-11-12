// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <cstdlib>

#include "common/fefw_hang_dump.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#endif

namespace Common::FefwHangDump {

std::string Write([[maybe_unused]] const char* reason) {
#ifdef _WIN32
    static std::atomic<bool> written{false};
    const char* const dir = std::getenv("EDEN_HANG_DUMP_DIR");
    if (dir == nullptr || dir[0] == '\0' || written.exchange(true)) {
        return {};
    }
    std::string path = std::string{dir} + "\\fortunes-eden-hang-" +
                       std::to_string(GetCurrentProcessId()) + ".dmp";
    const HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }
    using WriteDump = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                    PMINIDUMP_EXCEPTION_INFORMATION,
                                    PMINIDUMP_USER_STREAM_INFORMATION,
                                    PMINIDUMP_CALLBACK_INFORMATION);
    bool ok = false;
    if (const HMODULE dbghelp = LoadLibraryA("dbghelp.dll")) {
        if (const auto write_dump =
                reinterpret_cast<WriteDump>(GetProcAddress(dbghelp, "MiniDumpWriteDump"))) {
            // Stacks and thread information only: no heap, so none of the game's memory.
            ok = write_dump(GetCurrentProcess(), GetCurrentProcessId(), file,
                            static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo),
                            nullptr, nullptr, nullptr) != FALSE;
        }
    }
    CloseHandle(file);
    return ok ? path : std::string{};
#else
    return {};
#endif
}

} // namespace Common::FefwHangDump
