// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <vector>
#include "common/common_types.h"
#include "yuzu/configuration/configuration_shared.h"

namespace Core {
class System;
}

namespace ConfigurationShared {
class Builder;
}

class QLabel;

// Fortune's Eden: the Performance page (Settings > Performance). It holds Fortune's Weave's own
// performance options (the graphics settings the game reads from its RomFS, changed as it loads
// them), kept apart from the emulator's graphics options so they are easy to find.
class ConfigurePerformance : public ConfigurationShared::Tab {
    Q_OBJECT

public:
    explicit ConfigurePerformance(const Core::System& system_,
                                  std::shared_ptr<std::vector<ConfigurationShared::Tab*>> group,
                                  const ConfigurationShared::Builder& builder,
                                  QWidget* parent = nullptr);
    ~ConfigurePerformance() override;

    void ApplyConfiguration() override;
    void SetConfiguration() override;

    /// True for the settings this page shows (the Graphics page leaves them out).
    [[nodiscard]] static bool Shows(u32 setting_id);

private:
    void changeEvent(QEvent* event) override;
    void RetranslateUI();

    const Core::System& system;
    std::vector<std::function<void(bool)>> apply_funcs;
    QLabel* intro = nullptr;
    QLabel* restart_note = nullptr;
};
