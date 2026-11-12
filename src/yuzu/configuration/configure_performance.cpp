// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/configuration/configure_performance.h"

#include <QEvent>
#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>

#include "common/settings.h"
#include "core/core.h"
#include "yuzu/configuration/shared_widget.h"

ConfigurePerformance::ConfigurePerformance(
    const Core::System& system_, std::shared_ptr<std::vector<ConfigurationShared::Tab*>> group,
    const ConfigurationShared::Builder& builder, QWidget* parent)
    : Tab(group, parent), system{system_} {
    setAccessibleName(tr("Performance"));

    auto* const layout = new QVBoxLayout(this);

    auto* const box = new QGroupBox(tr("Fortune's Weave"), this);
    auto* const box_layout = new QVBoxLayout(box);

    intro = new QLabel(box);
    intro->setWordWrap(true);
    box_layout->addWidget(intro);

    for (Settings::BasicSetting* const setting :
         {static_cast<Settings::BasicSetting*>(&Settings::values.fefw_60fps),
          static_cast<Settings::BasicSetting*>(&Settings::values.fefw_reflections),
          static_cast<Settings::BasicSetting*>(&Settings::values.fefw_no_ambient_occlusion),
          static_cast<Settings::BasicSetting*>(&Settings::values.fefw_reduced_crowd_detail),
          static_cast<Settings::BasicSetting*>(&Settings::values.fefw_reduced_scenery_detail)}) {
        auto* const widget = builder.BuildWidget(setting, apply_funcs);
        if (widget == nullptr) {
            continue;
        }
        if (!widget->Valid()) {
            widget->deleteLater();
            continue;
        }
        box_layout->addWidget(widget);
    }

    restart_note = new QLabel(box);
    restart_note->setWordWrap(true);
    box_layout->addWidget(restart_note);

    layout->addWidget(box);

    sixty_note = new QLabel(this);
    sixty_note->setWordWrap(true);
    layout->addWidget(sixty_note);

    layout->addStretch(1);

    RetranslateUI();
    SetConfiguration();
}

ConfigurePerformance::~ConfigurePerformance() = default;

bool ConfigurePerformance::Shows(u32 setting_id) {
    return setting_id == Settings::values.fefw_60fps.Id() ||
           setting_id == Settings::values.fefw_reflections.Id() ||
           setting_id == Settings::values.fefw_no_ambient_occlusion.Id() ||
           setting_id == Settings::values.fefw_reduced_crowd_detail.Id() ||
           setting_id == Settings::values.fefw_reduced_scenery_detail.Id();
}

void ConfigurePerformance::SetConfiguration() {}

void ConfigurePerformance::ApplyConfiguration() {
    const bool is_powered_on = system.IsPoweredOn();
    for (const auto& func : apply_funcs) {
        func(is_powered_on);
    }
}

void ConfigurePerformance::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }
    QWidget::changeEvent(event);
}

void ConfigurePerformance::RetranslateUI() {
    setAccessibleName(tr("Performance"));
    intro->setText(tr("60 FPS mode, and lighter versions of the game's own graphics effects for "
                      "PCs that can't hold the frame rate. Nothing is changed on disk."));
    restart_note->setText(
        tr("<i>Changes take effect the next time the game starts.</i>"));
    sixty_note->setText(tr("60 FPS mode is made and tested for game version 1.1.0. On other "
                           "versions it turns on only if the game's code matches (untested: turn "
                           "it off if anything looks wrong); otherwise the game runs at 30 FPS."));
}
