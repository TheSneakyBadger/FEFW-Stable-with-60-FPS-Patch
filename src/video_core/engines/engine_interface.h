// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <bitset>
#include <limits>
#include <vector>

#include "common/common_types.h"

namespace Core {
class System;
}

namespace Tegra::Engines {

enum class EngineTypes : u32 {
    Nv01Timer,
    KeplerCompute,
    Maxwell3D,
    Fermi2D,
    MaxwellDMA,
    KeplerMemory,
};

class EngineInterface {
public:
    virtual ~EngineInterface() = default;

    /// Write the value to the register identified by method.
    virtual void CallMethod(Core::System& system, u32 method, u32 method_argument, bool is_last_call) = 0;

    /// Write multiple values to the register identified by method.
    virtual void CallMultiMethod(Core::System& system, u32 method, const u32* base_start, u32 amount, u32 methods_pending) = 0;

    /// Test 97b: consecutive macro writes of one method (inline_run_method), executed together.
    /// Must do what CallMethod(method, word, true) does for each word, in order.
    virtual void CallMacroRun(Core::System& system, u32 method, const u32* words, u32 count) {
        for (u32 i = 0; i < count; ++i) {
            CallMethod(system, method, words[i], true);
        }
    }

    void ConsumeSink(Core::System& system) {
        if (!method_sink.empty()) {
            ConsumeSinkImpl(system);
        }
    }

    std::bitset<(std::numeric_limits<u16>::max)()> execution_mask{};
    /// Test 94k: a macro's write outside execution_mask may go into method_sink (CallMethod
    /// does nothing more for it). Maxwell3D only: KeplerCompute's native methods act on more.
    bool macro_writes_may_sink{};
    /// Test 97b: the inline upload data method, whose macro writes the split GPU thread may send
    /// as one run (CallMacroRun). ~0: none.
    u32 inline_run_method{~0U};
    std::vector<std::pair<u32, u32>> method_sink{};
    GPUVAddr current_dma_segment;
    bool current_dirty{};
    /// The channel's native macro failure flags (NativeMacroContext), if any. On one GPU thread
    /// DmaPusher clears them after each method; with the GPU thread split (gpu_split.h) the back
    /// thread does, after each method it executes, so a failure skips the same draws.
    using NativeFailureFlags = std::array<bool*, 2>;
    NativeFailureFlags native_failure_flags{};
protected:
    virtual void ConsumeSinkImpl(Core::System& system) {
        for (auto [method, value] : method_sink) {
            CallMethod(system, method, value, true);
        }
        method_sink.clear();
    }
};

} // namespace Tegra::Engines
