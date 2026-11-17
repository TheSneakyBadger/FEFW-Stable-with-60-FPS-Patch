// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/fefw_dldsr.h"

#ifdef _WIN32
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

#include <fmt/format.h>
#include <fmt/ranges.h>

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

/// The graphics card driving the monitor (its name, as Windows reports it).
std::wstring AdapterOf(const wchar_t* device) {
    DISPLAY_DEVICEW adapter{};
    adapter.cb = sizeof(adapter);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &adapter, 0); ++i) {
        if (std::wstring_view{adapter.DeviceName} == device) {
            return adapter.DeviceString;
        }
        adapter = DISPLAY_DEVICEW{};
        adapter.cb = sizeof(adapter);
    }
    return {};
}

bool IsAmdAdapter(const std::wstring& adapter) {
    return adapter.find(L"AMD") != std::wstring::npos ||
           adapter.find(L"Radeon") != std::wstring::npos;
}

/// The factor of a mode above the desktop's resolution: its pixel count over the desktop's, times
/// 100 (178, 225; 120 to 400 with the older DSR), or 0 when it isn't one (another shape, smaller,
/// interlaced, another colour depth).
u32 FactorOf(const DEVMODEW& mode, const DEVMODEW& desktop) {
    if (mode.dmPelsWidth <= desktop.dmPelsWidth || mode.dmPelsHeight <= desktop.dmPelsHeight ||
        mode.dmBitsPerPel != desktop.dmBitsPerPel || (mode.dmDisplayFlags & DM_INTERLACED) != 0) {
        return 0;
    }
    // The same shape as the desktop, within 1%.
    const u64 cross_a = u64{mode.dmPelsWidth} * desktop.dmPelsHeight;
    const u64 cross_b = u64{mode.dmPelsHeight} * desktop.dmPelsWidth;
    if ((cross_a > cross_b ? cross_a - cross_b : cross_b - cross_a) * 100 > cross_b) {
        return 0;
    }
    const u64 pixels = u64{mode.dmPelsWidth} * mode.dmPelsHeight;
    const u64 desktop_pixels = u64{desktop.dmPelsWidth} * desktop.dmPelsHeight;
    return static_cast<u32>((pixels * 100 + desktop_pixels / 2) / desktop_pixels);
}

std::optional<DEVMODEW> DesktopMode(const wchar_t* device) {
    DEVMODEW desktop{};
    desktop.dmSize = sizeof(desktop);
    if (!EnumDisplaySettingsW(device, ENUM_REGISTRY_SETTINGS, &desktop)) {
        return std::nullopt;
    }
    return desktop;
}

/// Calls `visit(mode, factor)` for each mode above the desktop's resolution.
template <typename Visit>
void ForEachLargerMode(const wchar_t* device, const DEVMODEW& desktop, Visit&& visit) {
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    for (DWORD i = 0; EnumDisplaySettingsW(device, i, &mode); ++i) {
        if (const u32 factor = FactorOf(mode, desktop); factor != 0) {
            visit(mode, factor);
        }
        mode = DEVMODEW{};
        mode.dmSize = sizeof(mode);
    }
}

/// The factor's mode (within 2 of it, as drivers round the sizes), at the desktop's refresh rate if
/// offered, otherwise the highest.
std::optional<DEVMODEW> FindMode(const wchar_t* device, u32 factor) {
    const auto desktop = DesktopMode(device);
    if (!desktop) {
        return std::nullopt;
    }
    std::optional<DEVMODEW> best;
    ForEachLargerMode(device, *desktop, [&](const DEVMODEW& mode, u32 mode_factor) {
        if ((mode_factor > factor ? mode_factor - factor : factor - mode_factor) > 2) {
            return;
        }
        const bool same_rate = mode.dmDisplayFrequency == desktop->dmDisplayFrequency;
        const bool best_same_rate =
            best && best->dmDisplayFrequency == desktop->dmDisplayFrequency;
        if (!best || (same_rate && !best_same_rate) ||
            (!best_same_rate && mode.dmDisplayFrequency > best->dmDisplayFrequency)) {
            best = mode;
        }
    });
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
    const auto desktop = DesktopMode(info->szDevice);
    if (!desktop) {
        return factors;
    }
    std::vector<std::string> sizes;
    ForEachLargerMode(info->szDevice, *desktop, [&](const DEVMODEW& mode, u32 factor) {
        // One entry per factor (the sizes are rounded, so neighbours within 2 are the same).
        const bool known = std::ranges::any_of(factors, [factor](u32 known_factor) {
            return (known_factor > factor ? known_factor - factor : factor - known_factor) <= 2;
        });
        if (!known) {
            factors.push_back(factor);
            sizes.push_back(fmt::format("{}x{} ({}.{:02}x)", mode.dmPelsWidth, mode.dmPelsHeight,
                                        factor / 100, factor % 100));
        }
    });
    const std::wstring adapter = AdapterOf(info->szDevice);
    const bool amd = IsAmdAdapter(adapter);
    // NVIDIA (and anything else): only DLDSR's two factors (the older DSR's are left out). AMD:
    // every VSR factor the driver offers.
    std::vector<u32> dldsr;
    if (amd) {
        dldsr = factors;
        std::ranges::sort(dldsr);
    } else {
        for (const u32 wanted : {178U, 225U}) {
            if (std::ranges::any_of(factors, [wanted](u32 factor) {
                    return (factor > wanted ? factor - wanted : wanted - factor) <= 2;
                })) {
                dldsr.push_back(wanted);
            }
        }
    }
    // Once a run (the main screen asks each time it updates).
    static bool logged = false;
    if (!logged) {
        logged = true;
        LOG_INFO(Frontend, "DLDSR: {} ({}{}) at {}x{}; larger resolutions offered: {}",
                 Narrow(info->szDevice), Narrow(adapter.c_str()), amd ? ", AMD VSR" : "",
                 desktop->dmPelsWidth, desktop->dmPelsHeight,
                 sizes.empty() ? std::string{"none"} : fmt::format("{}", fmt::join(sizes, ", ")));
    }
    return dldsr;
}

bool UsesAmdVsr(QWidget* window) {
    const auto info = MonitorOf(WindowOf(window));
    return info && IsAmdAdapter(AdapterOf(info->szDevice));
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
    } else if (active_device.empty()) {
        LOG_INFO(Frontend, "DLDSR: {} already at {}x{}", Narrow(info->szDevice),
                 mode->dmPelsWidth, mode->dmPelsHeight);
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

bool UsesAmdVsr(QWidget*) {
    return false;
}

void Apply(QWidget*, u32, bool) {}

void Restore() {}

bool RecentlySwitched() {
    return false;
}

#endif

} // namespace FefwDldsr
