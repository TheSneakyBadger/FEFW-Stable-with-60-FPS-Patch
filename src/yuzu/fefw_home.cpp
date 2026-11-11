// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/fefw_home.h"

#include <QBoxLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QPushButton>
#include <QSlider>
#include <QSignalBlocker>
#include <QStandardItemModel>

#include "common/fefw_version.h"
#include "common/settings.h"
#ifdef EGLANTINE_STREAMLINE
#include "video_core/vulkan_common/streamline.h"
#endif
#include "qt_common/game_list/game_list_p.h"

namespace {

/// The game's icon with rounded corners.
QPixmap Rounded(const QPixmap& source, int size, int radius) {
    QPixmap result(size, size);
    result.fill(Qt::transparent);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath path;
    path.addRoundedRect(0, 0, size, size, radius, radius);
    painter.setClipPath(path);
    painter.drawPixmap(0, 0, source.scaled(size, size, Qt::KeepAspectRatioByExpanding,
                                           Qt::SmoothTransformation));
    return result;
}

/// The Play button's triangle, drawn rather than a "▶" character: that glyph comes from a
/// fallback font whose taller metrics push the text down and clip "Play" at Windows scaling.
QIcon PlayIcon() {
    QIcon icon;
    for (const qreal ratio : {1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0}) {
        QPixmap pixmap(QSize(14, 14) * ratio);
        pixmap.setDevicePixelRatio(ratio);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0x1b, 0x18, 0x24));
        painter.drawPolygon(QPolygonF{{QPointF(2.5, 1.0), QPointF(13.0, 7.0), QPointF(2.5, 13.0)}});
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

QString UpscalerName(Settings::Upscaler upscaler) {
    switch (upscaler) {
    case Settings::Upscaler::Fsr3:
        return QObject::tr("AMD FSR 3.1");
    case Settings::Upscaler::GameDlss:
        return QObject::tr("Game's own DLSS");
    case Settings::Upscaler::NvidiaDlss:
        return QObject::tr("NVIDIA DLSS");
    case Settings::Upscaler::Fsr4:
        return QObject::tr("AMD FSR 4");
    }
    return {};
}

QString QualityName(Settings::UpscalerQuality quality, bool dlss) {
    switch (quality) {
    case Settings::UpscalerQuality::Native:
        return dlss ? QObject::tr("DLAA") : QObject::tr("Native");
    case Settings::UpscalerQuality::UltraQuality:
        return QObject::tr("Ultra Quality");
    case Settings::UpscalerQuality::Quality:
        return QObject::tr("Quality");
    case Settings::UpscalerQuality::Balanced:
        return QObject::tr("Balanced");
    case Settings::UpscalerQuality::Performance:
        return QObject::tr("Performance");
    }
    return {};
}

/// Depth-first search of the game list's model (folders hold games) for the program.
QStandardItem* FindGame(QStandardItem* parent, u64 program_id) {
    for (int row = 0; row < parent->rowCount(); ++row) {
        QStandardItem* const item = parent->child(row, 0);
        if (!item) {
            continue;
        }
        const QVariant id = item->data(GameListItemPath::ProgramIdRole);
        if (id.isValid() && id.toULongLong() == program_id &&
            !item->data(GameListItemPath::FullPathRole).toString().isEmpty()) {
            return item;
        }
        if (QStandardItem* const found = FindGame(item, program_id)) {
            return found;
        }
    }
    return nullptr;
}

} // Anonymous namespace

FefwHome::FefwHome(QWidget* list, QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("fefwHome"));

    card = new QWidget(this);
    card->setObjectName(QStringLiteral("fefwHomeCard"));
    card->setAttribute(Qt::WA_StyledBackground, true);
    card->setStyleSheet(QStringLiteral(
        "#fefwHomeCard { background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #2a2236,"
        " stop:0.55 #1f1a2b, stop:1 #2b2416); border: 1px solid #3a3448; border-radius: 14px; }"
        "#fefwHomeTitle { font-size: 22pt; font-weight: bold; color: #ece8f3; background: transparent; }"
        "#fefwHomeVersion { font-size: 10pt; color: #a79fb8; background: transparent; }"
        "#fefwHomeStatus { font-size: 10pt; color: #e0b04a; background: transparent; }"
        "#fefwHomeIcon { background: transparent; }"
        "#fefwHomePlay { padding: 4px 28px;"
        " color: #1b1824; background-color: #e0b04a; border: 1px solid #e0b04a; border-radius: 9px; }"
        "#fefwHomePlay:hover { background-color: #f0c463; }"
        "#fefwHomePlay:pressed { background-color: #c39232; }"
        "#fefwHome60 { color: #ece8f3; background: transparent; }"
        "#fefwHomeCaption { font-size: 8pt; color: #a79fb8; background: transparent; }"));

    icon_label = new QLabel(card);
    icon_label->setObjectName(QStringLiteral("fefwHomeIcon"));
    icon_label->setFixedSize(144, 144);

    title_label = new QLabel(tr("Fire Emblem: Fortune's Weave"), card);
    title_label->setObjectName(QStringLiteral("fefwHomeTitle"));
    version_label = new QLabel(card);
    version_label->setObjectName(QStringLiteral("fefwHomeVersion"));
    status_label = new QLabel(card);
    status_label->setObjectName(QStringLiteral("fefwHomeStatus"));

    play_button = new QPushButton(tr("Play"), card);
    play_button->setObjectName(QStringLiteral("fefwHomePlay"));
    play_button->setIcon(PlayIcon());
    play_button->setIconSize(QSize(14, 14));
    // The font is set on the button, not in the style sheet: Qt sizes a button from its own
    // font, so a bigger style sheet font overflowed the bottom of it at Windows display scaling.
    QFont play_font = play_button->font();
    play_font.setPointSize(12);
    play_font.setBold(true);
    play_button->setFont(play_font);
    // The theme's "QPushButton { min-height: 13px }" replaces a setMinimumHeight() when the style
    // sheet is applied, so the height goes in the button's own style sheet: the text's full
    // height (descenders included) plus room above and below, in the padding's logical pixels.
    const int play_text_height = QFontMetrics(play_font).height();
    play_button->setStyleSheet(
        QStringLiteral("min-height: %1px;").arg(play_text_height + 8));
    play_button->setMinimumHeight(play_text_height + 18);
    play_button->setCursor(Qt::PointingHandCursor);
    // The 60 FPS mode, beside Play (it takes effect when the game starts).
    sixty_fps = new QCheckBox(tr("60 FPS"), card);
    sixty_fps->setObjectName(QStringLiteral("fefwHome60"));
    sixty_fps->setCursor(Qt::PointingHandCursor);
    sixty_fps->setToolTip(tr("Fortune's Weave runs at up to 60 FPS instead of 30 (game version "
                             "1.1.0). Takes effect when the game starts."));
    sixty_fps->setChecked(Settings::values.fefw_60fps.GetValue());
    // The settings people most often look for, beside Play: the upscaler, its quality and
    // frame generation (all take effect when the game starts), and the Performance page.
    upscaler_box = new QComboBox(card);
    for (const Settings::Upscaler upscaler :
         {Settings::Upscaler::Fsr3, Settings::Upscaler::NvidiaDlss, Settings::Upscaler::Fsr4,
          Settings::Upscaler::GameDlss}) {
        upscaler_box->addItem(UpscalerName(upscaler), static_cast<int>(upscaler));
    }
    upscaler_box->setToolTip(tr("What upscales the game's 3D scene: AMD FSR 3.1 (any card), "
                                "NVIDIA DLSS (GeForce RTX), AMD FSR 4 (Radeon RX 7000/9000) or "
                                "the game's own DLSS. Takes effect when the game starts."));
    quality_box = new QComboBox(card);
    quality_box->setToolTip(tr("How big the 3D scene is drawn before it is upscaled: Native "
                               "(DLAA with NVIDIA DLSS) is the sharpest and heaviest, Performance "
                               "the lightest. Applies at 1X resolution. Takes effect when the "
                               "game starts."));
    frame_gen_box = new QComboBox(card);
    frame_gen_box->addItem(tr("Off"), static_cast<int>(Settings::FrameGeneration::Off));
    frame_gen_box->addItem(tr("AMD FSR 3.1"), static_cast<int>(Settings::FrameGeneration::AmdFsr3));
    frame_gen_box->addItem(tr("NVIDIA DLSS"),
                           static_cast<int>(Settings::FrameGeneration::NvidiaDlss));
    frame_gen_box->setToolTip(
        tr("Makes a frame between each two of the game's (60 FPS shows as 120) in the 3D scenes. "
           "Needs a display faster than the game's frame rate. AMD FSR 3.1: any card. NVIDIA "
           "DLSS: GeForce RTX 40 and 50, with NVIDIA Streamline's DLLs in the user folder's "
           "streamline folder. Takes effect when the game starts. Experimental."));
    for (QComboBox* const box : {upscaler_box, quality_box, frame_gen_box}) {
        box->setCursor(Qt::PointingHandCursor);
        box->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    }
    // HDR output and its paper white (also in the Graphics settings).
    hdr_box = new QCheckBox(tr("HDR"), card);
    hdr_box->setObjectName(QStringLiteral("fefwHome60"));
    hdr_box->setCursor(Qt::PointingHandCursor);
    hdr_box->setToolTip(tr("HDR10 output when Windows HDR is on: the game sends its own HDR "
                           "picture. Takes effect when the game starts."));
    ultrawide_box = new QCheckBox(tr("Ultrawide"), card);
    ultrawide_box->setObjectName(QStringLiteral("fefwHome60"));
    ultrawide_box->setCursor(Qt::PointingHandCursor);
    ultrawide_box->setToolTip(tr("Kompas9's ultrawide patch: 3440x1440 (21:9), with the HUD, menus "
                                 "and movies kept at 16:9. For 21:9 monitors only; play "
                                 "fullscreen. Game version 1.1.0 only. Takes effect when the game "
                                 "starts."));
    paper_white = new QSlider(Qt::Horizontal, card);
    paper_white->setRange(80, 500);
    paper_white->setSingleStep(10);
    paper_white->setPageStep(50);
    paper_white->setMinimumWidth(130);
    paper_white->setCursor(Qt::PointingHandCursor);
    paper_white->setToolTip(tr("How bright the game's white (menus, text, daylight) is in HDR, "
                               "in nits. 250 suits most HDR monitors; lower it if the picture "
                               "looks too bright. Takes effect when the game starts."));
    paper_white_label = new QLabel(card);
    paper_white_label->setObjectName(QStringLiteral("fefwHomeCaption"));
    QPushButton* const performance = new QPushButton(tr("Performance Settings"), card);
    QPushButton* const game_settings = new QPushButton(tr("Game Settings"), card);
    QPushButton* const settings = new QPushButton(tr("Settings"), card);
    QPushButton* const updates = new QPushButton(tr("Check for Updates"), card);
    for (QPushButton* const button : {performance, game_settings, settings, updates}) {
        button->setCursor(Qt::PointingHandCursor);
    }
    // A small caption above each drop-down.
    const auto captioned = [this](const QString& caption, QWidget* widget) {
        QLabel* const label = new QLabel(caption, card);
        label->setObjectName(QStringLiteral("fefwHomeCaption"));
        QVBoxLayout* const column = new QVBoxLayout;
        column->setSpacing(2);
        column->addWidget(label);
        column->addWidget(widget);
        return column;
    };

    QHBoxLayout* const buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    buttons->addWidget(play_button, 0, Qt::AlignBottom);
    buttons->addSpacing(4);
    buttons->addWidget(sixty_fps, 0, Qt::AlignBottom);
    buttons->addSpacing(6);
    buttons->addWidget(ultrawide_box, 0, Qt::AlignBottom);
    buttons->addSpacing(6);
    buttons->addWidget(hdr_box, 0, Qt::AlignBottom);
    {
        QVBoxLayout* const column = new QVBoxLayout;
        column->setSpacing(2);
        column->addWidget(paper_white_label);
        column->addWidget(paper_white);
        buttons->addLayout(column);
    }
    buttons->addSpacing(10);
    buttons->addLayout(captioned(tr("Upscaler"), upscaler_box));
    buttons->addLayout(captioned(tr("Upscaler quality"), quality_box));
    buttons->addLayout(captioned(tr("Frame generation"), frame_gen_box));
    buttons->addStretch();

    QHBoxLayout* const more = new QHBoxLayout;
    more->setSpacing(8);
    more->addWidget(performance);
    more->addWidget(game_settings);
    more->addWidget(settings);
    more->addWidget(updates);
    more->addStretch();

    QVBoxLayout* const text = new QVBoxLayout;
    text->setSpacing(4);
    text->addStretch();
    text->addWidget(title_label);
    text->addWidget(version_label);
    text->addWidget(status_label);
    text->addSpacing(10);
    text->addLayout(buttons);
    text->addSpacing(4);
    text->addLayout(more);
    text->addStretch();

    QHBoxLayout* const card_layout = new QHBoxLayout(card);
    card_layout->setContentsMargins(20, 18, 20, 18);
    card_layout->setSpacing(22);
    card_layout->addWidget(icon_label, 0, Qt::AlignVCenter);
    card_layout->addLayout(text, 1);

    QVBoxLayout* const layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 0);
    layout->setSpacing(10);
    layout->addWidget(card);
    layout->addWidget(list, 1);

    card->hide();
    list->installEventFilter(this);
    setVisible(list->isVisibleTo(parent));

    connect(play_button, &QPushButton::clicked, this, [this] {
        if (!game_path.isEmpty()) {
            emit PlayRequested(game_path);
        }
    });
    connect(game_settings, &QPushButton::clicked, this, [this] {
        if (!game_path.isEmpty()) {
            emit GameSettingsRequested(TitleId, game_path);
        }
    });
    connect(sixty_fps, &QCheckBox::toggled, this, [this](bool checked) {
        if (Settings::values.fefw_60fps.GetValue() != checked) {
            Settings::values.fefw_60fps.SetValue(checked);
            emit SettingChanged();
        }
    });
    connect(upscaler_box, &QComboBox::currentIndexChanged, this, [this](int index) {
        const auto upscaler = static_cast<Settings::Upscaler>(upscaler_box->itemData(index).toInt());
        if (index >= 0 && Settings::values.upscaler.GetValue() != upscaler) {
            Settings::values.upscaler.SetValue(upscaler);
            UpdateStatusLine();
            emit SettingChanged();
        }
    });
    connect(quality_box, &QComboBox::currentIndexChanged, this, [this](int index) {
        const auto quality =
            static_cast<Settings::UpscalerQuality>(quality_box->itemData(index).toInt());
        if (index >= 0 && Settings::values.upscaler_quality.GetValue() != quality) {
            Settings::values.upscaler_quality.SetValue(quality);
            emit SettingChanged();
        }
    });
    connect(frame_gen_box, &QComboBox::currentIndexChanged, this, [this](int index) {
        const auto frame_gen =
            static_cast<Settings::FrameGeneration>(frame_gen_box->itemData(index).toInt());
        if (index >= 0 && Settings::values.frame_generation.GetValue() != frame_gen) {
            Settings::values.frame_generation.SetValue(frame_gen);
            UpdateStatusLine();
            emit SettingChanged();
        }
    });
    connect(performance, &QPushButton::clicked, this, &FefwHome::PerformanceSettingsRequested);
    connect(ultrawide_box, &QCheckBox::toggled, this, [this](bool checked) {
        if (Settings::values.fefw_ultrawide.GetValue() != checked) {
            Settings::values.fefw_ultrawide.SetValue(checked);
            emit SettingChanged();
        }
    });
    connect(hdr_box, &QCheckBox::toggled, this, [this](bool checked) {
        paper_white->setEnabled(checked);
        if (Settings::values.hdr_output.GetValue() != checked) {
            Settings::values.hdr_output.SetValue(checked);
            emit SettingChanged();
        }
    });
    connect(paper_white, &QSlider::valueChanged, this, [this](int value) {
        paper_white_label->setText(tr("Paper white: %1 nits").arg(value));
        Settings::values.hdr_paper_white.SetValue(value);
    });
    connect(paper_white, &QSlider::sliderReleased, this, &FefwHome::SettingChanged);
    connect(settings, &QPushButton::clicked, this, &FefwHome::SettingsRequested);
    connect(updates, &QPushButton::clicked, this, &FefwHome::UpdateCheckRequested);
}

bool FefwHome::eventFilter(QObject* object, QEvent* event) {
    // The main window shows and hides the game list (a game starting, the empty-list placeholder):
    // the card and the margins go with it.
    if (event->type() == QEvent::ShowToParent) {
        show();
    } else if (event->type() == QEvent::HideToParent) {
        hide();
    }
    return QWidget::eventFilter(object, event);
}

void FefwHome::Refresh(const QStandardItemModel& model) {
    QStandardItem* const item = FindGame(model.invisibleRootItem(), TitleId);
    if (!item) {
        game_path.clear();
        card->hide();
        return;
    }
    game_path = item->data(GameListItemPath::FullPathRole).toString();

    const QPixmap picture = item->data(Qt::DecorationRole).value<QPixmap>();
    if (!picture.isNull()) {
        icon_label->setPixmap(Rounded(picture, 144, 18));
    }
    const QString title = item->data(GameListItemPath::TitleRole).toString();
    if (!title.isEmpty()) {
        title_label->setText(title);
    }
    // The list's tooltip holds the play time and the game's update version, one per line.
    const QStringList lines = item->data(Qt::ToolTipRole).toString().split(QLatin1Char('\n'));
    version_label->setText(QStringLiteral("%1   ·   %2   ·   %3")
                               .arg(lines.value(1), lines.value(0),
                                    QString::fromUtf8(Common::FefwTitle)));
    UpdateStatusLine();
    card->show();
}

void FefwHome::UpdateStatusLine() {
    const Settings::Upscaler upscaler = Settings::values.upscaler.GetValue();
    const bool at_1x = Settings::values.resolution_setup.GetValue() == Settings::ResolutionSetup::Res1X;
    // The drop-downs show the choices; the line only says when the quality doesn't apply.
    QStringList notes;
    if (upscaler == Settings::Upscaler::GameDlss) {
        notes << tr("The game's own DLSS always draws at Performance.");
    } else if (!at_1x) {
        notes << tr("Upscaler quality applies at 1X resolution (Performance at this one).");
    }
#ifdef EGLANTINE_STREAMLINE
    if (Settings::values.frame_generation.GetValue() == Settings::FrameGeneration::NvidiaDlss &&
        !Vulkan::Streamline::MissingFrameGenFiles().empty()) {
        notes << tr("NVIDIA DLSS frame generation needs NVIDIA's files: Settings → Graphics → "
                    "Frame generation has a download button (until then AMD's is used).");
    }
#endif
    status_label->setText(notes.join(QLatin1Char('\n')));
    status_label->setVisible(!status_label->text().isEmpty());

    const QSignalBlocker sixty_blocker{sixty_fps};
    sixty_fps->setChecked(Settings::values.fefw_60fps.GetValue());
    const QSignalBlocker upscaler_blocker{upscaler_box};
    upscaler_box->setCurrentIndex(upscaler_box->findData(static_cast<int>(upscaler)));
    // The quality names depend on the upscaler (Native is DLAA with NVIDIA DLSS).
    const QSignalBlocker quality_blocker{quality_box};
    quality_box->clear();
    for (const Settings::UpscalerQuality quality :
         {Settings::UpscalerQuality::Native, Settings::UpscalerQuality::UltraQuality,
          Settings::UpscalerQuality::Quality, Settings::UpscalerQuality::Balanced,
          Settings::UpscalerQuality::Performance}) {
        quality_box->addItem(QualityName(quality, upscaler == Settings::Upscaler::NvidiaDlss),
                             static_cast<int>(quality));
    }
    quality_box->setCurrentIndex(
        quality_box->findData(static_cast<int>(Settings::values.upscaler_quality.GetValue())));
    quality_box->setEnabled(upscaler != Settings::Upscaler::GameDlss);
    const QSignalBlocker ultrawide_blocker{ultrawide_box};
    ultrawide_box->setChecked(Settings::values.fefw_ultrawide.GetValue());
    const QSignalBlocker hdr_blocker{hdr_box};
    hdr_box->setChecked(Settings::values.hdr_output.GetValue());
    paper_white->setEnabled(hdr_box->isChecked());
    const QSignalBlocker paper_white_blocker{paper_white};
    paper_white->setValue(Settings::values.hdr_paper_white.GetValue());
    paper_white_label->setText(tr("Paper white: %1 nits").arg(paper_white->value()));
    const QSignalBlocker frame_gen_blocker{frame_gen_box};
    frame_gen_box->setCurrentIndex(
        frame_gen_box->findData(static_cast<int>(Settings::values.frame_generation.GetValue())));
}
