// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>

#include <QKeyEvent>
#include <QList>
#include <QWidget>

namespace Core {
class System;
}

class QCheckBox;
class QComboBox;
class QString;
class QTimer;

class ConfigureInputAdvanced;
class ConfigureInputPlayer;

class InputProfiles;

namespace InputCommon {
class InputSubsystem;
}

namespace Ui {
class ConfigureInput;
}

void OnDockedModeChanged(bool last_state, bool new_state, Core::System& system);

class ConfigureInput : public QWidget {
    Q_OBJECT

public:
    explicit ConfigureInput(Core::System& system_, QWidget* parent = nullptr);
    ~ConfigureInput() override;

    /// Initializes the input dialog with the given input subsystem.
    void Initialize(InputCommon::InputSubsystem* input_subsystem_, std::size_t max_players = 8);

    /// Save all button configurations to settings file.
    void ApplyConfiguration();

    QList<QWidget*> GetSubTabs() const;

private:
    void changeEvent(QEvent* event) override;
    void RetranslateUI();
    void ClearAll();

    void UpdateDockedState(bool is_handheld);
    void UpdateAllInputDevices();
    void UpdateAllInputProfiles(std::size_t player_index);
    // Enable preceding controllers or disable following ones
    void PropagatePlayerNumberChanged(size_t player_index, bool checked,
                                      bool reconnect_current = false);

    /// Load configuration settings.
    void LoadConfiguration();
    void LoadPlayerControllerIndices();

    /// Restore all buttons to their default values.
    void RestoreDefaults();

    /// Fortune's Eden: shows Player 1's normal or mouse controls for editing.
    void ShowControlSet(bool mouse);
    /// Stores the shown set and puts the set that matches mouse emulation back on Player 1.
    void FinishControlSets(bool keep_edits);

    std::unique_ptr<Ui::ConfigureInput> ui;

    std::unique_ptr<InputProfiles> profiles;

    std::array<ConfigureInputPlayer*, 8> player_controllers{};
    std::array<QWidget*, 8> player_tabs{};
    // Checkboxes representing the "Connected Controllers".
    std::array<QCheckBox*, 8> connected_controller_checkboxes{};
    ConfigureInputAdvanced* advanced = nullptr;

    // Fortune's Eden: which of Player 1's two sets of controls (normal or mouse) is shown.
    QComboBox* control_set_combo = nullptr;
    bool showing_mouse_set = false;
    bool control_sets_finished = false;

    Core::System& system;
};
