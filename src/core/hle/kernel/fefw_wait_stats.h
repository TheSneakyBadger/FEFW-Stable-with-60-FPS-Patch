// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <utility>

namespace Kernel {
class KThread;
class KSynchronizationObject;
} // namespace Kernel

// Statistics with EDEN_BACK_THREAD_STATS (the "Start with full stats" launcher): what the game's
// rendering thread and its movie thread wait on, and how long the emulator's services take to
// answer the game. Fortune's Weave's heavy cutscenes fell to ~6 FPS with the CPU, the GPU and the
// emulator's GPU thread mostly idle (issue #22): this shows where the game is blocked.
namespace FefwWaitStats {

enum class Role { Render, Video };

[[nodiscard]] bool Enabled();

/// The guest thread that presents frames (Render) or fetches movie frames (Video).
void MarkThread(Role role, const Kernel::KThread* thread);

/// A name for a kernel object the emulator created (a service's event), for the wait report.
void NameObject(const Kernel::KSynchronizationObject* object, std::string name);

/// The label of a wait on this object: its name, or its kernel class.
[[nodiscard]] std::string ObjectLabel(Kernel::KSynchronizationObject* object);

/// Accounts time a guest thread spent blocked (no-op unless it is a marked thread).
void AccountWait(const Kernel::KThread* thread, std::string_view label,
                 std::chrono::steady_clock::duration duration);

/// The service command a guest thread called last (set while handling it), for its IPC wait.
void SetCommand(const Kernel::KThread* thread, std::string label);
[[nodiscard]] std::string TakeCommand(const Kernel::KThread* thread);

/// Time an emulator service spent handling one command (from any guest thread).
void AccountHandler(std::string_view label, std::chrono::steady_clock::duration duration);

/// Logs the report about every 10 seconds (call it often).
void MaybeReport();

/// Times a blocking call of the current guest thread.
class Scope {
public:
    Scope(const Kernel::KThread* thread_, std::string label_)
        : thread{Enabled() ? thread_ : nullptr}, label{std::move(label_)},
          start{thread ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}} {}
    ~Scope() {
        if (thread) AccountWait(thread, label, std::chrono::steady_clock::now() - start);
    }
    void SetLabel(std::string label_) { label = std::move(label_); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    const Kernel::KThread* thread;
    std::string label;
    std::chrono::steady_clock::time_point start;
};

} // namespace FefwWaitStats
