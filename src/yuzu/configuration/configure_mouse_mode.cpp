// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/configuration/configure_mouse_mode.h"

#include <QCheckBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QVBoxLayout>

#include "common/settings.h"

ConfigureMouseMode::ConfigureMouseMode(QWidget* parent) : QWidget(parent) {
    setAccessibleName(tr("Mouse mode"));

    auto* const layout = new QVBoxLayout(this);

    auto* const intro = new QLabel(
        tr("Mouse mode turns your PC mouse into the game's Joy-Con mouse cursor. Turn it on with "
           "the MOUSE button at the bottom of the main window (Ctrl + Enter turns it off), and turn "
           "on Mouse Controls in the game's options.<br><br>While mouse mode is on, Player 1 uses "
           "its own set of controls: set them on the <b>Mouse controls</b> tab. Choose the "
           "keyboard or a controller there; <i>Restore Defaults</i> gives the keyboard controls "
           "(WASD to move)."),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* const mouse_group = new QGroupBox(tr("Mouse"), this);
    auto* const mouse_layout = new QVBoxLayout(mouse_group);

    auto* const sensitivity_row = new QHBoxLayout();
    sensitivity_label = new QLabel(mouse_group);
    sensitivity = new QSlider(Qt::Horizontal, mouse_group);
    sensitivity->setRange(1, 20);
    sensitivity->setSingleStep(1);
    sensitivity->setPageStep(2);
    sensitivity->setTickPosition(QSlider::TicksBelow);
    sensitivity->setTickInterval(1);
    sensitivity->setToolTip(tr("How far the game's cursor moves for each movement of the mouse."));
    sensitivity_row->addWidget(sensitivity_label);
    sensitivity_row->addWidget(sensitivity, 1);
    mouse_layout->addLayout(sensitivity_row);
    connect(sensitivity, &QSlider::valueChanged, this,
            &ConfigureMouseMode::UpdateSensitivityLabel);

    wheel_zoom = new QCheckBox(tr("Mouse wheel zooms on battle maps"), mouse_group);
    wheel_zoom->setToolTip(
        tr("The wheel zooms the battle map in and out, as the left stick does. Everywhere else "
           "the wheel does nothing."));
    mouse_layout->addWidget(wheel_zoom);

    layout->addWidget(mouse_group);
    layout->addStretch(1);

    SetConfiguration();
}

ConfigureMouseMode::~ConfigureMouseMode() = default;

void ConfigureMouseMode::UpdateSensitivityLabel(int value) {
    sensitivity_label->setText(tr("Sensitivity: %1").arg(value));
}

void ConfigureMouseMode::SetConfiguration() {
    sensitivity->setValue(Settings::values.fefw_mouse_sensitivity.GetValue());
    UpdateSensitivityLabel(sensitivity->value());
    wheel_zoom->setChecked(Settings::values.fefw_mouse_wheel_zoom.GetValue());
}

void ConfigureMouseMode::ApplyConfiguration() {
    Settings::values.fefw_mouse_sensitivity.SetValue(sensitivity->value());
    Settings::values.fefw_mouse_wheel_zoom.SetValue(wheel_zoom->isChecked());
}
