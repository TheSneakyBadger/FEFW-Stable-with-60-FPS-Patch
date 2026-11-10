// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/fefw_home.h"

#include <QBoxLayout>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QPushButton>
#include <QStandardItemModel>

#include "common/fefw_version.h"
#include "common/settings.h"
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
        "#fefwHomePlay:pressed { background-color: #c39232; }"));

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
    play_button->setMinimumHeight(QFontMetrics(play_font).height() + 16);
    play_button->setCursor(Qt::PointingHandCursor);
    QPushButton* const game_settings = new QPushButton(tr("Game Settings"), card);
    QPushButton* const settings = new QPushButton(tr("Settings"), card);
    QPushButton* const updates = new QPushButton(tr("Check for Updates"), card);
    for (QPushButton* const button : {game_settings, settings, updates}) {
        button->setCursor(Qt::PointingHandCursor);
    }

    QHBoxLayout* const buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    buttons->addWidget(play_button);
    buttons->addSpacing(6);
    buttons->addWidget(game_settings);
    buttons->addWidget(settings);
    buttons->addWidget(updates);
    buttons->addStretch();

    QVBoxLayout* const text = new QVBoxLayout;
    text->setSpacing(4);
    text->addStretch();
    text->addWidget(title_label);
    text->addWidget(version_label);
    text->addWidget(status_label);
    text->addSpacing(10);
    text->addLayout(buttons);
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
    QString line = tr("Upscaler: %1").arg(UpscalerName(upscaler));
    if (upscaler != Settings::Upscaler::GameDlss) {
        const Settings::UpscalerQuality quality =
            at_1x ? Settings::values.upscaler_quality.GetValue()
                  : Settings::UpscalerQuality::Performance;
        line += QStringLiteral(" – %1").arg(
            QualityName(quality, upscaler == Settings::Upscaler::NvidiaDlss));
    }
    status_label->setText(line);
}
