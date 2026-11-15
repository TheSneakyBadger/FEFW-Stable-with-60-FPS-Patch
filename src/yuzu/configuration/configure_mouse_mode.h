// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QWidget>

class QCheckBox;
class QLabel;
class QSlider;

/// Fortune's Eden: the Mouse mode page's settings tab (sensitivity and the wheel). The page's
/// other tab is Player 1's mouse controls (see ConfigureInput::ShowMouseControls).
class ConfigureMouseMode : public QWidget {
    Q_OBJECT

public:
    explicit ConfigureMouseMode(QWidget* parent = nullptr);
    ~ConfigureMouseMode() override;

    void ApplyConfiguration();
    void SetConfiguration();

private:
    void UpdateSensitivityLabel(int value);

    QSlider* sensitivity = nullptr;
    QLabel* sensitivity_label = nullptr;
    QCheckBox* wheel_zoom = nullptr;
};
