// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QPixmap>
#include <QString>
#include <QWidget>

#include "common/common_types.h"

class QCheckBox;
class QLabel;
class QPushButton;
class QStandardItemModel;

/// Fortune's Eden's launch screen: a card above the game list with Fire Emblem: Fortune's
/// Weave's icon, title and version, the upscaler in use and Play (with the 60 FPS mode beside it)
/// / Game Settings / Settings / Check for Updates. Shown and hidden with the game list (it holds the list), and only while
/// the game is in the list.
class FefwHome : public QWidget {
    Q_OBJECT

public:
    static constexpr u64 TitleId = 0x04007EF01F8AE000ULL;

    explicit FefwHome(QWidget* list, QWidget* parent = nullptr);

    /// Looks for Fortune's Weave in the game list's model and shows or hides the card.
    void Refresh(const QStandardItemModel& model);
    /// Re-reads the upscaler line and the 60 FPS box (after the settings change).
    void UpdateStatusLine();

signals:
    void PlayRequested(const QString& path);
    void GameSettingsRequested(u64 title_id, const QString& path);
    void SettingsRequested();
    void UpdateCheckRequested();
    /// The 60 FPS box was ticked or unticked (the setting is already changed; save it).
    void SixtyFpsChanged();

protected:
    bool eventFilter(QObject* object, QEvent* event) override;

private:
    QWidget* card{};
    QLabel* icon_label{};
    QLabel* title_label{};
    QLabel* version_label{};
    QLabel* status_label{};
    QPushButton* play_button{};
    QCheckBox* sixty_fps{};
    QString game_path;
};
