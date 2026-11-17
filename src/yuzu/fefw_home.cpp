// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/fefw_home.h"

#include <algorithm>
#include <utility>
#include <filesystem>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include <QBoxLayout>
#include <QGridLayout>
#include <QMenu>
#include <QMessageBox>
#include <QStackedWidget>
#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QPushButton>
#include <QScrollArea>
#include <QFrame>
#include <QSlider>
#include <QSpinBox>
#include <QSignalBlocker>
#include <QStandardItemModel>

#include "common/fefw_version.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/fefw_title_logo.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/loader/loader.h"
#include "qt_common/qt_common.h"
#ifdef EGLANTINE_STREAMLINE
#include "video_core/vulkan_common/streamline.h"
#include "yuzu/configuration/nvidia_dlss_panel.h"
#endif
#include "qt_common/game_list/game_list_p.h"

namespace {

/// The game's icon with rounded corners, at the screen's pixel density (size and radius in
/// logical pixels). All of it is shown: one that isn't square is fitted in and centred.
QPixmap Rounded(const QPixmap& source, int size, int radius, qreal pixel_ratio) {
    const int pixels = qRound(size * pixel_ratio);
    QPixmap result(pixels, pixels);
    result.setDevicePixelRatio(pixel_ratio);
    result.fill(Qt::transparent);
    QPixmap scaled =
        source.scaled(pixels, pixels, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    scaled.setDevicePixelRatio(pixel_ratio);
    const QRectF area((pixels - scaled.width()) / 2.0 / pixel_ratio,
                      (pixels - scaled.height()) / 2.0 / pixel_ratio,
                      scaled.width() / pixel_ratio, scaled.height() / pixel_ratio);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath path;
    path.addRoundedRect(area, radius, radius);
    painter.setClipPath(path);
    painter.drawPixmap(area.topLeft(), scaled);
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

namespace {

constexpr int IconSize = 180;
constexpr int LogoWidth = 400;

const char* const HomeStyle =
    "#fefwHome { background: #141418; }"
    "#fefwHomeScroll, #fefwHomeContent { background: transparent; }"
    "#fefwHomePanel { background: #1b1b21; border: 1px solid #2a2a33; border-radius: 14px; }"
    "#fefwHomeDivider { background: #2a2a33; border: none; }"
    "#fefwHomeMoreButton::menu-indicator { image: none; width: 0px; }"
    "#fefwHomeSetup { background: #141418; }"
    "#fefwHomeKicker { color: #d9a63a; background: transparent; }"
    "#fefwHomeTitle { color: #f3efe6; background: transparent; }"
    "#fefwHomeVersion { font-size: 10pt; color: #a7a5ae; background: transparent; }"
    "#fefwHomeText { font-size: 12pt; color: #c9c7cf; background: transparent; }"
    "#fefwHomeStatus { font-size: 9pt; color: #e0b04a; background: transparent; }"
    "#fefwHomeIcon { background: transparent; }"
    "#fefwHomeTitleBlock { background: transparent; }"
    "#fefwHomeSection { font-size: 8pt; font-weight: bold; color: #a7a5ae; background: transparent; }"
    "#fefwHomeCaption { font-size: 8pt; color: #a7a5ae; background: transparent; }"
    "#fefwHomeCheck { color: #ece8f3; background: transparent; }"
    "#fefwHomePlay { padding: 4px 28px;"
    " color: #1b1824; background-color: #e0b04a; border: 1px solid #e0b04a; border-radius: 12px; }"
    "#fefwHomePlay:hover { background-color: #f0c463; }"
    "#fefwHomePlay:pressed { background-color: #c39232; }"
    "#fefwHomeMore { color: #b9b7c0; background: transparent; border: none; }"
    "#fefwHomeMore:hover { color: #ece8f3; }";

/// The big gold button (Play, or Choose game folder), its font set on the button and its height in
/// its own style sheet: Qt sizes a button from its own font, so a bigger style sheet font overflowed
/// it at Windows display scaling, and the theme's "QPushButton { min-height: 13px }" replaces a
/// setMinimumHeight() when the style sheet is applied.
void MakeBigButton(QPushButton* button, int point_size, int extra_height) {
    button->setObjectName(QStringLiteral("fefwHomePlay"));
    QFont font = button->font();
    font.setPointSize(point_size);
    font.setBold(true);
    button->setFont(font);
    const int text_height = QFontMetrics(font).height();
    button->setStyleSheet(QStringLiteral("min-height: %1px;").arg(text_height + extra_height));
    button->setMinimumHeight(text_height + extra_height + 10);
    button->setCursor(Qt::PointingHandCursor);
}

/// "FIRE EMBLEM" above "Fortune's Weave", in a serif display font (until the game's own logo is
/// read). Returns the widget holding both.
QWidget* AddTitle(QBoxLayout* layout, QWidget* outer, int title_points, Qt::Alignment align) {
    QWidget* const parent = new QWidget(outer);
    parent->setObjectName(QStringLiteral("fefwHomeTitleBlock"));
    QVBoxLayout* const block = new QVBoxLayout(parent);
    block->setContentsMargins(0, 0, 0, 0);
    block->setSpacing(layout->spacing());
    QLabel* const kicker = new QLabel(QObject::tr("FIRE EMBLEM"), parent);
    kicker->setObjectName(QStringLiteral("fefwHomeKicker"));
    QFont kicker_font = kicker->font();
    kicker_font.setFamilies({QStringLiteral("Georgia"), QStringLiteral("Cambria"),
                             QStringLiteral("Times New Roman"), QStringLiteral("serif")});
    kicker_font.setPointSize(title_points * 2 / 5);
    kicker_font.setLetterSpacing(QFont::AbsoluteSpacing, 3);
    kicker->setFont(kicker_font);
    QLabel* const title = new QLabel(QObject::tr("Fortune's Weave"), parent);
    title->setObjectName(QStringLiteral("fefwHomeTitle"));
    QFont title_font = kicker_font;
    title_font.setLetterSpacing(QFont::AbsoluteSpacing, 0);
    title_font.setPointSize(title_points);
    title_font.setBold(true);
    title->setFont(title_font);
    kicker->setAlignment(align);
    title->setAlignment(align);
    block->addWidget(kicker);
    block->addWidget(title);
    layout->addWidget(parent);
    return parent;
}

/// The game's language code for its logo texture.
std::string_view LogoLanguage() {
    switch (Settings::values.language_index.GetValue()) {
    case Settings::Language::Japanese:
        return "JPja";
    case Settings::Language::Korean:
        return "KRko";
    case Settings::Language::Chinese:
    case Settings::Language::ChineseSimplified:
        return "CNzh";
    case Settings::Language::Taiwanese:
    case Settings::Language::ChineseTraditional:
        return "TWzh";
    default:
        return "USen";
    }
}

/// Fortune's Weave's title logo from the player's copy of the game (base game romfs), cropped to
/// its drawing; null when it can't be read.
QImage ReadLogo(const QString& path) {
    const auto file =
        QtCommon::vfs->OpenFile(path.toStdString(), FileSys::OpenMode::Read);
    if (file == nullptr) {
        return {};
    }
    const auto loader = Loader::GetLoader(*QtCommon::system, file);
    FileSys::VirtualFile romfs_file;
    if (loader == nullptr || loader->ReadRomFS(romfs_file) != Loader::ResultStatus::Success ||
        romfs_file == nullptr) {
        return {};
    }
    const auto logo =
        FileSys::FefwTitleLogo::Read(FileSys::ExtractRomFS(romfs_file), LogoLanguage());
    if (!logo) {
        return {};
    }
    const QImage image(logo->rgba.data(), static_cast<int>(logo->width),
                       static_cast<int>(logo->height), static_cast<int>(logo->width) * 4,
                       QImage::Format_RGBA8888);
    // Crop the clear margin around the lettering.
    int left = image.width(), top = image.height(), right = -1, bottom = -1;
    for (int y = 0; y < image.height(); ++y) {
        const auto* line = reinterpret_cast<const uchar*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (line[x * 4 + 3] > 8) {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
    }
    if (right < left || bottom < top) {
        return {};
    }
    return image.copy(left, top, right - left + 1, bottom - top + 1);
}

} // Anonymous namespace

FefwHome::FefwHome(QWidget* list, QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("fefwHome"));
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QString::fromLatin1(HomeStyle));

    pages = new QStackedWidget(this);
    game_page = new QWidget(pages);
    setup_page = new QWidget(pages);
    BuildGamePage(game_page);
    BuildSetupPage(setup_page);
    pages->addWidget(game_page);
    pages->addWidget(setup_page);
    pages->setCurrentWidget(setup_page);

    // The game list still finds the game (and its icon, title and play time), but isn't shown:
    // Fortune's Weave is the only game Fortune's Eden is for. It sits in a hidden holder rather
    // than being hidden itself, so the main window can still show and hide it (and this screen
    // with it) as before.
    QWidget* const list_holder = new QWidget(this);
    QVBoxLayout* const holder_layout = new QVBoxLayout(list_holder);
    holder_layout->setContentsMargins(0, 0, 0, 0);
    holder_layout->addWidget(list);
    list_holder->hide();

    QVBoxLayout* const layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(pages, 1);
    layout->addWidget(list_holder);

    list->installEventFilter(this);
    setVisible(list->isVisibleTo(parent));
}

void FefwHome::BuildGamePage(QWidget* page) {
    // One centred column: the game's icon, its title logo and version, Play, then a panel with
    // the settings people look for most and the other pages. It scrolls when the window is short.
    QScrollArea* const scroll = new QScrollArea(page);
    scroll->setObjectName(QStringLiteral("fefwHomeScroll"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    QWidget* const content = new QWidget(scroll);
    content->setObjectName(QStringLiteral("fefwHomeContent"));
    scroll->setWidget(content);
    scroll->viewport()->setAutoFillBackground(false);

    icon_label = new QLabel(content);
    icon_label->setObjectName(QStringLiteral("fefwHomeIcon"));
    icon_label->setFixedSize(IconSize, IconSize);
    title_label = new QLabel(content); // The game list's title, for the tooltip only
    title_label->hide();
    version_label = new QLabel(content);
    version_label->setObjectName(QStringLiteral("fefwHomeVersion"));
    version_label->setAlignment(Qt::AlignHCenter);
    version_label->setWordWrap(true);
    logo_label = new QLabel(content);
    logo_label->setObjectName(QStringLiteral("fefwHomeIcon"));
    logo_label->setAlignment(Qt::AlignCenter);
    logo_label->hide();

    QFrame* const right = new QFrame(content); // the settings panel
    right->setObjectName(QStringLiteral("fefwHomePanel"));
    right->setMaximumWidth(900);

    play_button = new QPushButton(tr("Play"), content);
    play_button->setIcon(PlayIcon());
    play_button->setIconSize(QSize(16, 16));
    MakeBigButton(play_button, 15, 22);
    play_button->setFixedWidth(280);

    const auto caption = [right](const QString& name) {
        QLabel* const label = new QLabel(name, right);
        label->setObjectName(QStringLiteral("fefwHomeCaption"));
        return label;
    };
    const auto check = [right](const QString& name) {
        QCheckBox* const box = new QCheckBox(name, right);
        box->setObjectName(QStringLiteral("fefwHomeCheck"));
        box->setCursor(Qt::PointingHandCursor);
        return box;
    };

    sixty_fps = check(tr("60 FPS"));
    sixty_fps->setToolTip(tr("Fortune's Weave runs at up to 60 FPS instead of 30 (game version "
                             "1.1.0). Takes effect when the game starts."));
    sixty_fps->setChecked(Settings::values.fefw_60fps.GetValue());
    ultrawide_box = check(tr("Ultrawide"));
    ultrawide_box->setToolTip(tr("Kompas9's ultrawide patch: 3440x1440 (21:9), with the HUD, menus "
                                 "and movies kept at 16:9. For 21:9 monitors only; play "
                                 "fullscreen. Game version 1.1.0 only. Takes effect when the game "
                                 "starts."));
    hdr_box = check(tr("HDR"));
    hdr_box->setToolTip(tr("HDR10 output when Windows HDR is on: the game sends its own HDR "
                           "picture. Takes effect when the game starts."));
    // RenHDrum HD Rumble (experimental, off by default; also in Controls > Vibration).
    hd_rumble_box = check(tr("HD Rumble"));
    hd_rumble_box->setToolTip(
        tr("Experimental: RenHDrum's HD Rumble for the Switch Pro Controller and Joy-Con, with "
           "haptics for DualSense, rumble for Xbox controllers and the grips of the Steam "
           "Controller (2026), closer to a real Switch. Off: "
           "the standard rumble. Takes effect for controllers connected after it is changed, "
           "or after restarting Fortune's Eden. Its options are in Settings > Controls > "
           "Vibration > HD Rumble."));
    hd_rumble_box->setChecked(Settings::values.hd_rumble_enabled.GetValue());
    paper_white = new QSlider(Qt::Horizontal, right);
    paper_white->setRange(80, 500);
    paper_white->setSingleStep(10);
    paper_white->setPageStep(50);
    paper_white->setCursor(Qt::PointingHandCursor);
    paper_white->setToolTip(tr("How bright the game's white (menus, text, daylight) is in HDR, "
                               "in nits. 250 suits most HDR monitors; lower it if the picture "
                               "looks too bright. Takes effect when the game starts."));
    paper_white_label = caption(QString{});
    paper_white_label->setMinimumWidth(140);

    upscaler_box = new QComboBox(right);
    for (const Settings::Upscaler upscaler :
         {Settings::Upscaler::Fsr3, Settings::Upscaler::NvidiaDlss, Settings::Upscaler::Fsr4,
          Settings::Upscaler::GameDlss}) {
        upscaler_box->addItem(UpscalerName(upscaler), static_cast<int>(upscaler));
    }
    upscaler_box->setToolTip(tr("What upscales the game's 3D scene: AMD FSR 3.1 (any card), "
                                "NVIDIA DLSS (GeForce RTX), AMD FSR 4 (Radeon RX 7000/9000) or "
                                "the game's own DLSS. Takes effect when the game starts."));
    quality_box = new QComboBox(right);
    quality_box->setToolTip(tr("How big the 3D scene is drawn before it is upscaled: Native "
                               "(DLAA with NVIDIA DLSS) is the sharpest and heaviest, Performance "
                               "the lightest. Applies at 1X resolution. Takes effect when the "
                               "game starts."));
    frame_gen_box = new QComboBox(right);
    frame_gen_box->addItem(tr("Off"), static_cast<int>(Settings::FrameGeneration::Off));
    frame_gen_box->addItem(tr("AMD FSR 3.1"), static_cast<int>(Settings::FrameGeneration::AmdFsr3));
    frame_gen_box->addItem(tr("NVIDIA DLSS"),
                           static_cast<int>(Settings::FrameGeneration::NvidiaDlss));
    frame_gen_box->setToolTip(
        tr("Makes a frame between each two of the game's (60 FPS shows as 120) in the 3D scenes. "
           "Needs a display faster than the game's frame rate. AMD FSR 3.1: any card. NVIDIA "
           "DLSS: GeForce RTX 40 and 50, with NVIDIA Streamline's DLLs in the user folder's "
           "streamline folder. Takes effect when the game starts. Experimental."));
    button_icons_box = new QComboBox(right);
    button_icons_box->addItem(tr("Nintendo Switch"),
                              static_cast<int>(Settings::FefwButtonIcons::Switch));
    button_icons_box->addItem(tr("Xbox"), static_cast<int>(Settings::FefwButtonIcons::Xbox));
    button_icons_box->addItem(tr("PlayStation"),
                              static_cast<int>(Settings::FefwButtonIcons::PlayStation));
    button_icons_box->setToolTip(
        tr("The button icons the game shows. Each shows the controller button Player 1 has it "
           "on, so they follow your mapping. Also in Settings > Controls. Takes effect when the "
           "game starts."));
    for (QComboBox* const box : {upscaler_box, quality_box, frame_gen_box, button_icons_box}) {
        box->setCursor(Qt::PointingHandCursor);
    }
    status_label = new QLabel(right);
    status_label->setObjectName(QStringLiteral("fefwHomeStatus"));
    status_label->setWordWrap(true);

    QPushButton* const performance = new QPushButton(tr("Performance Settings"), right);
    QPushButton* const game_settings = new QPushButton(tr("Game Settings"), right);
    QPushButton* const settings = new QPushButton(tr("Settings"), right);
    QPushButton* const updates = new QPushButton(tr("Check for Updates"), right);
    for (QPushButton* const button : {performance, game_settings, settings, updates}) {
        button->setCursor(Qt::PointingHandCursor);
        button->setMinimumHeight(34);
    }
    // What the game list's right-click menu was used for, now in one menu.
    QPushButton* const more = new QPushButton(QStringLiteral("\u22EF"), right);
    more->setObjectName(QStringLiteral("fefwHomeMoreButton"));
    more->setToolTip(tr("Game folder, saves, mods and updates"));
    more->setCursor(Qt::PointingHandCursor);
    more->setMinimumSize(44, 34);
    QMenu* const more_menu = new QMenu(more);
    QAction* const add_folder = more_menu->addAction(tr("Add a game folder..."));
    open_saves = more_menu->addAction(tr("Open the save folder"));
    open_mods = more_menu->addAction(tr("Open the mods folder"));
    QAction* const install = more_menu->addAction(tr("Install an update or DLC..."));
    more->setMenu(more_menu);

    // A caption above a control.
    const auto labelled = [&caption](const QString& name, QWidget* widget) {
        QVBoxLayout* const column = new QVBoxLayout;
        column->setSpacing(4);
        column->addWidget(caption(name));
        column->addWidget(widget);
        return column;
    };

    // First row: the switches and the HDR brightness.
    QHBoxLayout* const switches = new QHBoxLayout;
    switches->setSpacing(22);
    switches->addWidget(sixty_fps);
    switches->addWidget(ultrawide_box);
    switches->addWidget(hdr_box);
    {
        QVBoxLayout* const column = new QVBoxLayout;
        column->setSpacing(4);
        column->addWidget(paper_white_label);
        column->addWidget(paper_white);
        paper_white->setMinimumWidth(150);
        switches->addLayout(column);
    }
#ifdef _WIN32
    switches->addLayout(labelled(tr("Experimental"), hd_rumble_box));
#else
    // RenHDrum ships for Windows only (renhdrum.dll); other builds keep the standard rumble.
    hd_rumble_box->hide();
#endif
    // The texture cache on disk, with a note on who it helps most.
    texture_cache_box = new QComboBox(right);
    for (const auto& [size, name] :
         {std::pair{Settings::TextureDiskCache::Off, tr("Off")},
          std::pair{Settings::TextureDiskCache::Gb2, tr("2 GB")},
          std::pair{Settings::TextureDiskCache::Gb4, tr("4 GB")},
          std::pair{Settings::TextureDiskCache::Gb8, tr("8 GB")}}) {
        texture_cache_box->addItem(name, static_cast<int>(size));
    }
    texture_cache_box->setCursor(Qt::PointingHandCursor);
    texture_cache_box->setMinimumWidth(110);
    texture_cache_box->setToolTip(
        tr("Keeps the game's textures, once converted for your graphics card, on disk (in "
           "user/cache/textures), so a texture seen before is loaded instead of converted again. "
           "The oldest are dropped once it's full. Takes effect when the game starts."));
    {
        QVBoxLayout* const column = new QVBoxLayout;
        column->setSpacing(4);
        column->addWidget(caption(tr("Texture cache on disk")));
        column->addWidget(texture_cache_box);
        QLabel* const note = new QLabel(
            tr("Stores textures so places you've been load without stutter. Most useful on "
               "graphics cards with less video memory."),
            right);
        note->setObjectName(QStringLiteral("fefwHomeCaption"));
        note->setWordWrap(true);
        note->setMaximumWidth(240);
        column->addWidget(note);
        switches->addLayout(column);
    }
    switches->addStretch();

    // Second row: upscaling, frame generation and the button icons.
    QHBoxLayout* const choices = new QHBoxLayout;
    choices->setSpacing(14);
    choices->addLayout(labelled(tr("Upscaler"), upscaler_box));
    choices->addLayout(labelled(tr("Upscaler quality"), quality_box));
    choices->addLayout(labelled(tr("Frame generation"), frame_gen_box));
    // Multi frame generation (2x, 3x, 4x): while frame generation is on.
    mfg_box = new QComboBox(right);
    for (const auto& [mfg, name] : {std::pair{Settings::MfgMultiplier::X2, tr("2x")},
                                     std::pair{Settings::MfgMultiplier::X3, tr("3x")},
                                     std::pair{Settings::MfgMultiplier::X4, tr("4x")}}) {
        mfg_box->addItem(name, static_cast<int>(mfg));
    }
    mfg_box->setCursor(Qt::PointingHandCursor);
    mfg_box->setToolTip(
        tr("How many frames frame generation shows for each of the game's. AMD FSR 3.1: any card. "
           "NVIDIA DLSS: 3x and 4x on GeForce RTX 50 cards, or on RTX 40 and older with the "
           "community MFG Unlocker (offered when you choose them). Keep the game's frame rate "
           "times this at or under your display's refresh rate and any frame rate cap, or it "
           "stutters. Takes effect when the game starts."));
    mfg_column = new QWidget(right);
    {
        QVBoxLayout* const mfg_layout = new QVBoxLayout(mfg_column);
        mfg_layout->setContentsMargins(0, 0, 0, 0);
        mfg_layout->setSpacing(4);
        mfg_layout->addWidget(caption(tr("Frames")));
        mfg_layout->addWidget(mfg_box);
    }
    choices->addWidget(mfg_column);
    // Your frame rate cap, if you have one: frame generation's pacing stays under it.
    mfg_cap_spin = new QSpinBox(right);
    mfg_cap_spin->setRange(0, 1000);
    mfg_cap_spin->setSpecialValueText(tr("None"));
    mfg_cap_spin->setSuffix(tr(" FPS"));
    mfg_cap_spin->setMinimumWidth(110);
    mfg_cap_spin->setToolTip(
        tr("If you cap your frame rate (in the NVIDIA app or Control Panel, NVIDIA Reflex, RTSS or "
           "the MFG Unlocker's menu), set this to the same number, so frame generation's pacing "
           "stays just under it. If you don't have a cap, leave it at None: Fortune's Eden caps "
           "to your display's refresh rate itself. Takes effect right away."));
    mfg_cap_column = new QWidget(right);
    {
        QVBoxLayout* const cap_layout = new QVBoxLayout(mfg_cap_column);
        cap_layout->setContentsMargins(0, 0, 0, 0);
        cap_layout->setSpacing(4);
        cap_layout->addWidget(caption(tr("Your frame rate cap")));
        cap_layout->addWidget(mfg_cap_spin);
    }
    choices->addWidget(mfg_cap_column);
    choices->addLayout(labelled(tr("Button icons"), button_icons_box));
    for (QComboBox* const box : {upscaler_box, quality_box, frame_gen_box, button_icons_box}) {
        box->setMinimumWidth(150);
    }
    choices->addStretch();

    QFrame* const divider = new QFrame(right);
    divider->setObjectName(QStringLiteral("fefwHomeDivider"));
    divider->setFixedHeight(1);

    QHBoxLayout* const buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    buttons->addWidget(performance);
    buttons->addWidget(game_settings);
    buttons->addWidget(settings);
    buttons->addWidget(updates);
    buttons->addStretch();
    buttons->addWidget(more);

    QVBoxLayout* const panel_layout = new QVBoxLayout(right);
    panel_layout->setContentsMargins(22, 18, 22, 18);
    panel_layout->setSpacing(14);
    panel_layout->addLayout(switches);
    panel_layout->addLayout(choices);
    panel_layout->addWidget(status_label);
    panel_layout->addWidget(divider);
    panel_layout->addLayout(buttons);

    QVBoxLayout* const column = new QVBoxLayout(content);
    column->setContentsMargins(32, 24, 32, 24);
    column->setSpacing(0);
    column->addStretch(1);
    column->addWidget(icon_label, 0, Qt::AlignHCenter);
    column->addSpacing(18);
    title_block = AddTitle(column, content, 34, Qt::AlignHCenter);
    column->addWidget(logo_label, 0, Qt::AlignHCenter);
    column->addSpacing(10);
    column->addWidget(version_label);
    column->addSpacing(18);
    column->addWidget(play_button, 0, Qt::AlignHCenter);
    column->addSpacing(18);
    QHBoxLayout* const centre = new QHBoxLayout;
    centre->addStretch(1);
    centre->addWidget(right, 100);
    centre->addStretch(1);
    column->addLayout(centre);
    column->addStretch(1);

    QHBoxLayout* const layout = new QHBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(scroll);

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
            OfferMfgUnlocker();
            UpdateStatusLine();
            emit SettingChanged();
        }
    });
    connect(mfg_box, &QComboBox::currentIndexChanged, this, [this](int index) {
        const auto mfg = static_cast<Settings::MfgMultiplier>(mfg_box->itemData(index).toInt());
        if (index >= 0 && Settings::values.mfg_multiplier.GetValue() != mfg) {
            Settings::values.mfg_multiplier.SetValue(mfg);
            OfferMfgUnlocker();
            UpdateStatusLine();
            emit SettingChanged();
        }
    });
    connect(mfg_cap_spin, &QSpinBox::valueChanged, this, [](int value) {
        if (Settings::values.mfg_frame_cap_fps.GetValue() != value) {
            Settings::values.mfg_frame_cap_fps.SetValue(value);
        }
    });
    connect(mfg_cap_spin, &QSpinBox::editingFinished, this, &FefwHome::SettingChanged);
    connect(texture_cache_box, &QComboBox::currentIndexChanged, this, [this](int index) {
        const auto size =
            static_cast<Settings::TextureDiskCache>(texture_cache_box->itemData(index).toInt());
        if (index >= 0 && Settings::values.texture_disk_cache.GetValue() != size) {
            Settings::values.texture_disk_cache.SetValue(size);
            emit SettingChanged();
        }
    });
    connect(button_icons_box, &QComboBox::currentIndexChanged, this, [this](int index) {
        const auto icons =
            static_cast<Settings::FefwButtonIcons>(button_icons_box->itemData(index).toInt());
        if (index >= 0 && Settings::values.fefw_button_icons.GetValue() != icons) {
            Settings::values.fefw_button_icons.SetValue(icons);
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
    connect(hd_rumble_box, &QCheckBox::toggled, this, [this](bool checked) {
        if (Settings::values.hd_rumble_enabled.GetValue() != checked) {
            Settings::values.hd_rumble_enabled.SetValue(checked);
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
    connect(add_folder, &QAction::triggered, this, &FefwHome::AddGameFolderRequested);
    connect(install, &QAction::triggered, this, &FefwHome::InstallRequested);
    connect(open_saves, &QAction::triggered, this, [this] {
        if (!game_path.isEmpty()) {
            emit OpenSaveFolderRequested(TitleId, game_path);
        }
    });
    connect(open_mods, &QAction::triggered, this, [this] {
        if (!game_path.isEmpty()) {
            emit OpenModsFolderRequested(TitleId, game_path);
        }
    });
}

void FefwHome::BuildSetupPage(QWidget* page) {
    page->setObjectName(QStringLiteral("fefwHomeSetup"));
    page->setAttribute(Qt::WA_StyledBackground, true);

    setup_text = new QLabel(tr("Looking for Fortune's Weave..."), page);
    setup_text->setObjectName(QStringLiteral("fefwHomeText"));
    setup_text->setWordWrap(true);
    setup_text->setAlignment(Qt::AlignCenter);
    setup_text->setMaximumWidth(640);

    QPushButton* const choose = new QPushButton(tr("Choose game folder"), page);
    MakeBigButton(choose, 13, 18);
    QPushButton* const install = new QPushButton(tr("Install update or DLC"), page);
    QPushButton* const settings = new QPushButton(tr("Settings"), page);
    QPushButton* const updates = new QPushButton(tr("Check for Updates"), page);
    for (QPushButton* const button : {install, settings, updates}) {
        button->setCursor(Qt::PointingHandCursor);
        button->setMinimumHeight(34);
    }

    QVBoxLayout* const column = new QVBoxLayout;
    column->setSpacing(6);
    AddTitle(column, page, 40, Qt::AlignHCenter);
    column->addSpacing(18);
    column->addWidget(setup_text, 0, Qt::AlignHCenter);
    column->addSpacing(18);
    {
        QHBoxLayout* const row = new QHBoxLayout;
        row->setSpacing(10);
        row->addStretch();
        row->addWidget(choose);
        row->addWidget(install, 0, Qt::AlignVCenter);
        row->addStretch();
        column->addLayout(row);
    }

    QHBoxLayout* const bottom = new QHBoxLayout;
    bottom->setSpacing(8);
    bottom->addStretch();
    bottom->addWidget(settings);
    bottom->addWidget(updates);
    bottom->addStretch();

    QVBoxLayout* const layout = new QVBoxLayout(page);
    layout->setContentsMargins(48, 32, 48, 24);
    layout->addStretch(2);
    layout->addLayout(column);
    layout->addStretch(3);
    layout->addLayout(bottom);

    connect(choose, &QPushButton::clicked, this, &FefwHome::AddGameFolderRequested);
    connect(install, &QPushButton::clicked, this, &FefwHome::InstallRequested);
    connect(settings, &QPushButton::clicked, this, &FefwHome::SettingsRequested);
    connect(updates, &QPushButton::clicked, this, &FefwHome::UpdateCheckRequested);
}

bool FefwHome::eventFilter(QObject* object, QEvent* event) {
    // The main window shows and hides the game list (a game starting): this screen goes with it.
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
        setup_text->setText(
            tr("Fortune's Eden couldn't find the game yet. Choose the folder that holds your copy "
               "of Fortune's Weave (an .nsp or .xci file, or the extracted game), then install "
               "its update if you have it."));
        pages->setCurrentWidget(setup_page);
        return;
    }
    game_path = item->data(GameListItemPath::FullPathRole).toString();

    // The game's icon at its own size (256 px), at the screen's pixel density (#68), with the
    // game's own title logo under it in place of the written title (read once). The game list
    // puts a small stand-in there when it can't read the icon: then the logo alone.
    const QPixmap picture = item->data(GameListItemPath::FullIconRole).value<QPixmap>();
    const bool has_icon = !picture.isNull() && std::min(picture.width(), picture.height()) >= 128;
    if (has_icon) {
        // The icon's top rows hold the bottom edge of lettering Nintendo's crop cut through:
        // trimmed (and the sides a little, to stay square).
        const int top = picture.height() * 10 / 256;
        const int side = top / 2;
        const QPixmap trimmed =
            picture.copy(side, top, picture.width() - 2 * side, picture.height() - top);
        icon_label->setPixmap(Rounded(trimmed, IconSize, 30, icon_label->devicePixelRatioF()));
    } else if (!picture.isNull()) {
        icon_label->setPixmap(Rounded(picture, IconSize, 30, icon_label->devicePixelRatioF()));
    }
    if (logo_path != game_path) {
        logo_path = game_path;
        logo = ReadLogo(game_path);
        LOG_INFO(Frontend, "Home screen: title logo {}",
                 logo.isNull() ? std::string{"not found, showing the written title"}
                               : fmt::format("{}x{}", logo.width(), logo.height()));
        if (!logo.isNull()) {
            const qreal ratio = logo_label->devicePixelRatioF();
            QPixmap pixmap = QPixmap::fromImage(
                logo.scaledToWidth(qRound(LogoWidth * ratio), Qt::SmoothTransformation));
            pixmap.setDevicePixelRatio(ratio);
            logo_label->setPixmap(pixmap);
        }
    }
    logo_label->setVisible(!logo.isNull());
    title_block->setVisible(logo.isNull());
    icon_label->setVisible(has_icon || logo.isNull());
    const QString title = item->data(GameListItemPath::TitleRole).toString();
    if (!title.isEmpty()) {
        title_label->setText(title);
        icon_label->setToolTip(title);
    }
    // The list's tooltip holds the play time and the game's update version, one per line.
    const QStringList lines = item->data(Qt::ToolTipRole).toString().split(QLatin1Char('\n'));
    version_label->setText(QStringLiteral("%1   ·   %2   ·   %3")
                               .arg(lines.value(1), lines.value(0),
                                    QString::fromUtf8(Common::FefwTitle)));
    UpdateStatusLine();
    pages->setCurrentWidget(game_page);
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
    // 3x or 4x where the card was held at 2x last time, without the MFG Unlocker.
    if (Settings::values.frame_generation.GetValue() == Settings::FrameGeneration::NvidiaDlss &&
        Settings::values.mfg_multiplier.GetValue() != Settings::MfgMultiplier::X2 &&
        Settings::values.dlss_mfg_max_seen.GetValue() == 1) {
        std::error_code ec;
        if (!std::filesystem::exists(Vulkan::Streamline::MfgUnlockerPath(), ec)) {
            notes << tr("NVIDIA allows 3x and 4x on GeForce RTX 50 cards only: Settings → Graphics "
                        "→ Frame generation can download the community MFG Unlocker for this "
                        "card (until then 2x is used).");
        }
    }
#endif
    status_label->setText(notes.join(QLatin1Char('\n')));
    status_label->setVisible(!notes.isEmpty());
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
    const QSignalBlocker hd_rumble_blocker{hd_rumble_box};
    hd_rumble_box->setChecked(Settings::values.hd_rumble_enabled.GetValue());
    const QSignalBlocker paper_white_blocker{paper_white};
    paper_white->setValue(Settings::values.hdr_paper_white.GetValue());
    paper_white_label->setText(tr("Paper white: %1 nits").arg(paper_white->value()));
    const QSignalBlocker button_icons_blocker{button_icons_box};
    button_icons_box->setCurrentIndex(button_icons_box->findData(
        static_cast<int>(Settings::values.fefw_button_icons.GetValue())));
    const QSignalBlocker frame_gen_blocker{frame_gen_box};
    frame_gen_box->setCurrentIndex(
        frame_gen_box->findData(static_cast<int>(Settings::values.frame_generation.GetValue())));
    const QSignalBlocker texture_cache_blocker{texture_cache_box};
    texture_cache_box->setCurrentIndex(texture_cache_box->findData(
        static_cast<int>(Settings::values.texture_disk_cache.GetValue())));
    const QSignalBlocker mfg_blocker{mfg_box};
    mfg_box->setCurrentIndex(
        mfg_box->findData(static_cast<int>(Settings::values.mfg_multiplier.GetValue())));
    mfg_column->setVisible(Settings::values.frame_generation.GetValue() !=
                           Settings::FrameGeneration::Off);
    const QSignalBlocker cap_blocker{mfg_cap_spin};
    mfg_cap_spin->setValue(Settings::values.mfg_frame_cap_fps.GetValue());
    mfg_cap_column->setVisible(Settings::values.frame_generation.GetValue() !=
                                   Settings::FrameGeneration::Off &&
                               Settings::values.mfg_frame_cap.GetValue() ==
                                   Settings::MfgFrameCap::Display);
}

void FefwHome::OfferMfgUnlocker() {
#ifdef EGLANTINE_STREAMLINE
    // NVIDIA DLSS at 3x or 4x without the MFG Unlocker, on a card not known to do more than 2x by
    // itself (RTX 50): offer to download it.
    if (Settings::values.frame_generation.GetValue() != Settings::FrameGeneration::NvidiaDlss ||
        Settings::values.mfg_multiplier.GetValue() == Settings::MfgMultiplier::X2 ||
        Settings::values.dlss_mfg_max_seen.GetValue() > 1 || HasMfgUnlocker()) {
        return;
    }
    const auto answer = QMessageBox::question(
        this, tr("Multi frame generation"),
        tr("NVIDIA allows 3x and 4x DLSS frame generation on GeForce RTX 50 cards only. On RTX 40 "
           "and older cards, the community MFG Unlocker lifts that limit.\n\nOn those cards, AMD "
           "FSR 3.1 frame generation at 3x or 4x is usually smoother (RTX 40 cards lack the RTX "
           "50's hardware frame pacing).\n\nDownload the MFG Unlocker now? (Not needed on an RTX "
           "50 card. Without it, 2x is used.)"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (answer == QMessageBox::Yes) {
        DownloadMfgUnlocker(this);
    }
#endif
}
