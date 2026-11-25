// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2018 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <string>

#include <QEventLoop>
#include <boost/algorithm/string/replace.hpp>
#include "common/httplib.h"

#include <discord_rpc.h>
#include <fmt/format.h>

#include "common/common_types.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/loader/loader.h"

#include "discord_impl.h"

#ifdef YUZU_BUNDLED_OPENSSL
#include <openssl/cert.h>
#endif

namespace DiscordRPC {

DiscordImpl::DiscordImpl(Core::System& system_) : system{system_} {
    DiscordEventHandlers handlers{};
    // The number is the client ID for Eden, it's used for images and the
    // application name
    Discord_Initialize("1397286652128264252", &handlers, 1, nullptr);
}

DiscordImpl::~DiscordImpl() {
    Discord_ClearPresence();
    Discord_Shutdown();
}

void DiscordImpl::Pause() {
    Discord_ClearPresence();
}

std::string DiscordImpl::GetGameString(const std::string& title) {
    // Convert to lowercase
    std::string icon_name = Common::ToLower(title);

    // Replace spaces with dashes
    std::replace(icon_name.begin(), icon_name.end(), ' ', '-');
    boost::replace_all(icon_name, "é", "e");

    // Remove non-alphanumeric characters but keep dashes
    std::erase_if(icon_name, [](char c) { return !std::isalnum(c) && c != '-'; });

    // Remove dashes from the start and end of the string
    icon_name.erase(icon_name.begin(), std::find_if(icon_name.begin(), icon_name.end(),
                                                    [](int ch) { return ch != '-'; }));
    icon_name.erase(
        std::find_if(icon_name.rbegin(), icon_name.rend(), [](int ch) { return ch != '-'; }).base(),
        icon_name.end());

    // Remove double dashes
    icon_name.erase(std::unique(icon_name.begin(), icon_name.end(),
                                [](char a, char b) { return a == '-' && b == '-'; }),
                    icon_name.end());

    return icon_name;
}

static constexpr char DEFAULT_DISCORD_TEXT[] = "Eden is an emulator for the Nintendo Switch";
static constexpr char DEFAULT_DISCORD_IMAGE[] =
    "https://git.eden-emu.dev/eden-emu/eden/raw/branch/master/dist/qt_themes/default/icons/256x256/"
    "eden.png";

// #62: Fortune's Eden only runs Fire Emblem: Fortune's Weave, so the status always names it (the
// game's own title reads "FE Fortunes Weave", without the apostrophe), from the moment
// Fortune's Eden opens. Its picture is the one on Nintendo's own eShop page: nothing is shipped
// or hosted here, Discord loads it from Nintendo.
static constexpr char FEFW_DISCORD_TITLE[] = "FE Fortune's Weave";
static constexpr char FEFW_DISCORD_IMAGE[] =
    "https://img-eshop.cdn.nintendo.net/i/"
    "159bdf6c9f126feb35ae1123c370cf357bcf069fd5dda84d8d0c4f64ae097e40.jpg";
// Discord loads it itself; a check from here (a HEAD request) failed against Nintendo's server
// and swapped in the Eden logo once a game started, so it is used as it is.
static const char* FefwImage() {
    return FEFW_DISCORD_IMAGE;
}

void DiscordImpl::UpdateGameStatus(std::string_view, bool) {
    s64 start_time = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    DiscordRichPresence presence{};

    presence.largeImageKey = FefwImage();
    presence.largeImageText = FEFW_DISCORD_TITLE;
    presence.smallImageKey = DEFAULT_DISCORD_IMAGE;
    presence.smallImageText = DEFAULT_DISCORD_TEXT;
    presence.state = FEFW_DISCORD_TITLE;
    presence.status_display_type = DiscordStatusDisplayType_State;
    presence.details = "Currently in game";
    presence.startTimestamp = start_time;
    Discord_UpdatePresence(&presence);
}

void DiscordImpl::Update() {
    if (system.IsPoweredOn()) {
        system.GetAppLoader().ReadTitle(game_title);
        UpdateGameStatus({}, true);
        return;
    }

    s64 start_time = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    DiscordRichPresence presence{};
    presence.largeImageKey = FefwImage();
    presence.largeImageText = FEFW_DISCORD_TITLE;
    presence.smallImageKey = DEFAULT_DISCORD_IMAGE;
    presence.smallImageText = DEFAULT_DISCORD_TEXT;
    presence.state = FEFW_DISCORD_TITLE;
    presence.status_display_type = DiscordStatusDisplayType_State;
    presence.details = "Getting ready to play";
    presence.startTimestamp = start_time;
    Discord_UpdatePresence(&presence);
}
} // namespace DiscordRPC
