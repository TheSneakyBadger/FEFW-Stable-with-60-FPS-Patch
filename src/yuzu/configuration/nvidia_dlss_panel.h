// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Shown under the NVIDIA DLSS upscaler and frame generation settings: which of NVIDIA's files
// were found, and a button that fetches missing ones from NVIDIA's own Streamline release.

#pragma once

#ifdef EGLANTINE_STREAMLINE

#include <QWidget>

class QLabel;
class QPushButton;

/// Whether the community MFG Unlocker (for NVIDIA DLSS 3x and 4x on GeForce RTX 40 and older) is
/// in place, and the release Fortune's Eden downloads.
[[nodiscard]] bool HasMfgUnlocker();

/// Asks, then downloads the MFG Unlocker into the Streamline folder. True when it is in place.
bool DownloadMfgUnlocker(QWidget* parent);

class NvidiaDlssPanel : public QWidget {
public:
    /// frame_generation: the files DLSS frame generation needs, else those of DLSS upscaling.
    explicit NvidiaDlssPanel(QWidget* parent, bool frame_generation = false);

    void Refresh();

protected:
    void showEvent(QShowEvent* event) override;

private:
    void Download();
    /// The community MFG Unlocker, for 3x and 4x on GeForce RTX 40 and older.
    void DownloadMfgUnlocker();

    QLabel* icon_label;
    QLabel* status_label;
    QPushButton* download_button;
    QPushButton* folder_button;
    QLabel* mfg_label{};
    QPushButton* mfg_button{};
    bool frame_generation;
};

#endif
