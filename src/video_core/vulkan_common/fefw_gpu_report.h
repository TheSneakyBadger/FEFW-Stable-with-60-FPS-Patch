// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <thread>

namespace Vulkan {
class Device;
}

namespace FefwGpuReport {

/// Fortune's Eden: a log line every 10 s on how the graphics card is doing, for slowdowns where
/// the card falls behind while the CPU idles (a 12 GB RTX 3060 dropped to 5 FPS after a loading
/// screen, with its present thread waiting up to 1.2 s for a swapchain image). On NVIDIA cards
/// (NVML, read once a second): how busy it was, its clock, temperature and power, and what held
/// its clock back (power, heat). Everywhere: the memory this program has on the card and in
/// system memory, and on Windows how much of it the system has put in shared GPU memory (the
/// part read over PCIe). Reading only. EDEN_GPU_REPORT=0: none.
class Reporter {
public:
    explicit Reporter(const Vulkan::Device& device);
    ~Reporter();

    Reporter(const Reporter&) = delete;
    Reporter& operator=(const Reporter&) = delete;

private:
    std::jthread thread;
};

} // namespace FefwGpuReport
