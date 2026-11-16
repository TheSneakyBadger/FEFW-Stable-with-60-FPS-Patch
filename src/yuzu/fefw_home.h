// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QImage>
#include <QPixmap>
#include <QString>
#include <QWidget>

#include "common/common_types.h"

class QAction;
class QCheckBox;
class QComboBox;
class QSlider;
class QLabel;
class QPushButton;
class QStackedWidget;
class QStandardItemModel;

/// Fortune's Eden's home screen, in place of the game list (Fortune's Weave is the only game it is
/// for): the game's icon and title on the left, and Play with the settings people look for most on
/// the right. Until the game is found, a page that asks for the game's folder. The game list stays
/// (hidden) for finding the game; this screen is shown and hidden with it.
class FefwHome : public QWidget {
    Q_OBJECT

public:
    static constexpr u64 TitleId = 0x04007EF01F8AE000ULL;

    explicit FefwHome(QWidget* list, QWidget* parent = nullptr);

    /// Looks for Fortune's Weave in the game list's model: the game's page, or the folder page.
    void Refresh(const QStandardItemModel& model);
    /// Re-reads the settings shown (after the settings change).
    void UpdateStatusLine();

signals:
    void PlayRequested(const QString& path);
    void GameSettingsRequested(u64 title_id, const QString& path);
    void SettingsRequested();
    void UpdateCheckRequested();
    /// A setting on the home screen changed (60 FPS, upscaler, quality, frame generation): it is
    /// already set; save it.
    void SettingChanged();
    void PerformanceSettingsRequested();
    void AddGameFolderRequested();
    void InstallRequested();
    void OpenSaveFolderRequested(u64 title_id, const QString& path);
    void OpenModsFolderRequested(u64 title_id, const QString& path);

protected:
    bool eventFilter(QObject* object, QEvent* event) override;

private:
    void BuildGamePage(QWidget* page);
    void BuildSetupPage(QWidget* page);

    QStackedWidget* pages{};
    QWidget* game_page{};
    QWidget* setup_page{};
    QLabel* setup_text{};
    QAction* open_saves{};
    QAction* open_mods{};
    QLabel* icon_label{};
    QWidget* title_block{};
    QLabel* logo_label{};
    QImage logo;
    QString logo_path;
    QLabel* title_label{};
    QLabel* version_label{};
    QLabel* status_label{};
    QPushButton* play_button{};
    QCheckBox* sixty_fps{};
    QComboBox* upscaler_box{};
    QComboBox* quality_box{};
    QComboBox* frame_gen_box{};
    QComboBox* button_icons_box{};
    QCheckBox* ultrawide_box{};
    QCheckBox* hdr_box{};
    QCheckBox* hd_rumble_box{};
    QSlider* paper_white{};
    QLabel* paper_white_label{};
    QString game_path;
};
