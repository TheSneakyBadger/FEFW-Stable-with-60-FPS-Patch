// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/fefw_dldsr.h"

#ifdef _WIN32
#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>

#include <QString>
#include <QTimer>
#include <QWidget>

#include "common/logging.h"

#include <windows.h>
#endif

namespace FefwDldsr {

#ifdef _WIN32
namespace {

/// The monitor switched to a DLDSR resolution (empty: none).
std::wstring active_device;
std::chrono::steady_clock::time_point last_switch{};

HWND WindowOf(QWidget* window) {
    return reinterpret_cast<HWND>(window->window()->winId());
}

std::optional<MONITORINFOEXW> MonitorOf(HWND hwnd) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info)) {
        return std::nullopt;
    }
    return info;
}

std::string Narrow(const wchar_t* text) {
    return QString::fromWCharArray(text).toStdString();
}

/// The factor's mode: the desktop's size times the factor's square root (4/3 for 1.78x, 3/2 for
/// 2.25x), at the desktop's refresh rate if offered, otherwise the highest.
std::optional<DEVMODEW> FindMode(const wchar_t* device, u32 factor) {
    long num = 0;
    long den = 1;
    switch (factor) {
    case 178:
        num = 4;
        den = 3;
        break;
    case 225:
        num = 3;
        den = 2;
        break;
    default:
        return std::nullopt;
    }
    DEVMODEW desktop{};
    desktop.dmSize = sizeof(desktop);
    if (!EnumDisplaySettingsW(device, ENUM_REGISTRY_SETTINGS, &desktop)) {
        return std::nullopt;
    }
    const long width = static_cast<long>(desktop.dmPelsWidth) * num / den;
    const long height = static_cast<long>(desktop.dmPelsHeight) * num / den;
    std::optional<DEVMODEW> best;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    for (DWORD i = 0; EnumDisplaySettingsW(device, i, &mode); ++i) {
        const bool fits = std::labs(static_cast<long>(mode.dmPelsWidth) - width) <= 2 &&
                          std::labs(static_cast<long>(mode.dmPelsHeight) - height) <= 2 &&
                          mode.dmBitsPerPel == desktop.dmBitsPerPel &&
                          (mode.dmDisplayFlags & DM_INTERLACED) == 0;
        if (fits) {
            const bool same_rate = mode.dmDisplayFrequency == desktop.dmDisplayFrequency;
            const bool best_same_rate =
                best && best->dmDisplayFrequency == desktop.dmDisplayFrequency;
            if (!best || (same_rate && !best_same_rate) ||
                (!best_same_rate && mode.dmDisplayFrequency > best->dmDisplayFrequency)) {
                best = mode;
            }
        }
        mode = DEVMODEW{};
        mode.dmSize = sizeof(mode);
    }
    return best;
}

/// Fits the (fullscreen) window to its monitor, as Windows sees it now.
void Fit(QWidget* window, bool borderless) {
    const HWND hwnd = WindowOf(window);
    const auto info = MonitorOf(hwnd);
    if (!info) {
        return;
    }
    const RECT& rect = info->rcMonitor;
    // Borderless is one pixel taller, as ShowFullscreen makes it.
    SetWindowPos(hwnd, nullptr, rect.left, rect.top, rect.right - rect.left,
                 rect.bottom - rect.top + (borderless ? 1 : 0), SWP_NOZORDER | SWP_NOACTIVATE);
}

} // Anonymous namespace

std::vector<u32> AvailableFactors(QWidget* window) {
    std::vector<u32> factors;
    const auto info = MonitorOf(WindowOf(window));
    if (!info) {
        return factors;
    }
    for (const u32 factor : {178U, 225U}) {
        if (FindMode(info->szDevice, factor)) {
            factors.push_back(factor);
        }
    }
    return factors;
}

void Apply(QWidget* window, u32 factor, bool borderless) {
    if (factor == 0) {
        return;
    }
    const auto info = MonitorOf(WindowOf(window));
    if (!info) {
        return;
    }
    if (!active_device.empty() && active_device != info->szDevice) {
        Restore();
    }
    const auto mode = FindMode(info->szDevice, factor);
    if (!mode) {
        LOG_INFO(Frontend, "DLDSR: {} has no {}.{:02}x resolution (turned off in the driver?)",
                 Narrow(info->szDevice), factor / 100, factor % 100);
        return;
    }
    DEVMODEW current{};
    current.dmSize = sizeof(current);
    const bool already = EnumDisplaySettingsW(info->szDevice, ENUM_CURRENT_SETTINGS, &current) &&
                         current.dmPelsWidth == mode->dmPelsWidth &&
                         current.dmPelsHeight == mode->dmPelsHeight;
    if (!already) {
        DEVMODEW target = *mode;
        target.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
        const LONG result =
            ChangeDisplaySettingsExW(info->szDevice, &target, nullptr, CDS_FULLSCREEN, nullptr);
        if (result != DISP_CHANGE_SUCCESSFUL) {
            LOG_WARNING(Frontend, "DLDSR: switching {} to {}x{} failed ({})",
                        Narrow(info->szDevice), mode->dmPelsWidth, mode->dmPelsHeight, result);
            return;
        }
        last_switch = std::chrono::steady_clock::now();
        LOG_INFO(Frontend, "DLDSR: {} at {}x{}, {} Hz ({}.{:02}x)", Narrow(info->szDevice),
                 mode->dmPelsWidth, mode->dmPelsHeight, mode->dmDisplayFrequency, factor / 100,
                 factor % 100);
    }
    active_device = info->szDevice;
    // Qt and Windows (a display scale change) may still move the window after the switch.
    Fit(window, borderless);
    for (const int delay : {250, 1000}) {
        QTimer::singleShot(delay, window, [window, borderless] {
            if (!active_device.empty()) {
                Fit(window, borderless);
            }
        });
    }
}

void Restore() {
    if (active_device.empty()) {
        return;
    }
    ChangeDisplaySettingsExW(active_device.c_str(), nullptr, nullptr, 0, nullptr);
    last_switch = std::chrono::steady_clock::now();
    LOG_INFO(Frontend, "DLDSR: {} back at the desktop resolution", Narrow(active_device.c_str()));
    active_device.clear();
}

bool RecentlySwitched() {
    return std::chrono::steady_clock::now() - last_switch < std::chrono::milliseconds{1500};
}

#else

std::vector<u32> AvailableFactors(QWidget*) {
    return {};
}

void Apply(QWidget*, u32, bool) {}

void Restore() {}

bool RecentlySwitched() {
    return false;
}

#endif

} // namespace FefwDldsr
