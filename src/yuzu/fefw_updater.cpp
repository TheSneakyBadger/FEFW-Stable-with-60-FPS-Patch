// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/fefw_updater.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

#include <QApplication>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrl>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common/fefw_version.h"
#include "common/fs/path_util.h"
#include "common/httplib.h"
#include "common/logging.h"

#ifdef YUZU_BUNDLED_OPENSSL
#include <openssl/cert.h>
#endif

namespace FefwUpdater {
namespace {

constexpr const char* kApiHost = "https://api.github.com";
constexpr const char* kLatestPath =
    "/repos/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/releases/latest";
constexpr const char* kAssetName = "FEFW-Stable-60FPS-Portable.zip";
constexpr const char* kUserAgent = "FortunesEden-Updater";

QString Tr(const char* text) {
    return QCoreApplication::translate("FefwUpdater", text);
}

std::unique_ptr<httplib::Client> MakeClient(const std::string& host) {
    auto client = std::make_unique<httplib::Client>(host);
    client->set_connection_timeout(15);
    client->set_read_timeout(30);
    client->set_write_timeout(30);
#ifdef YUZU_BUNDLED_OPENSSL
    client->load_ca_cert_store(kCert, sizeof(kCert));
#endif
    return client;
}

/// "v1.6.3" -> {1, 6, 3}; anything that is not digits and dots gives nothing.
std::optional<std::vector<u32>> ParseVersion(std::string text) {
    if (!text.empty() && (text[0] == 'v' || text[0] == 'V')) {
        text.erase(0, 1);
    }
    std::vector<u32> parts;
    u32 value = 0;
    bool digits = false;
    for (const char c : text) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            value = value * 10 + static_cast<u32>(c - '0');
            digits = true;
        } else if (c == '.' && digits) {
            parts.push_back(value);
            value = 0;
            digits = false;
        } else {
            return std::nullopt;
        }
    }
    if (!digits) {
        return std::nullopt;
    }
    parts.push_back(value);
    return parts;
}

bool IsNewer(const std::string& tag, const std::string& current) {
    auto latest = ParseVersion(tag);
    auto mine = ParseVersion(current);
    if (!latest || !mine) {
        return false;
    }
    const size_t size = (std::max)(latest->size(), mine->size());
    latest->resize(size);
    mine->resize(size);
    return *latest > *mine;
}

/// This build's version. EDEN_UPDATE_TEST_VERSION=v1.0 pretends to be an older build, to test
/// the updater against the current release.
std::string CurrentVersion() {
    if (const char* pretend = std::getenv("EDEN_UPDATE_TEST_VERSION"); pretend && *pretend) {
        return pretend;
    }
    return Common::FefwVersion;
}

QString SkipFilePath() {
    return QString::fromStdString(
        Common::FS::PathToUTF8String(Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) /
                                     "skipped-update.txt"));
}

std::string SkippedTag() {
    QFile file(SkipFilePath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll()).trimmed().toStdString();
}

void SkipTag(const std::string& tag) {
    QFile file(SkipFilePath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        file.write(QByteArray::fromStdString(tag));
    }
}

/// Downloads url to path, following redirects (GitHub sends assets to its storage host).
/// progress(bytes) returns false to cancel.
bool Download(std::string url, const QString& path, const std::function<bool(u64)>& progress,
              std::string& error) {
    QFile file(path);
    for (int redirect = 0; redirect < 8; ++redirect) {
        const size_t scheme = url.find("://");
        const size_t slash = scheme == std::string::npos ? std::string::npos : url.find('/', scheme + 3);
        if (slash == std::string::npos || url.compare(0, 8, "https://") != 0) {
            error = "unexpected download address";
            return false;
        }
        const std::string host = url.substr(0, slash);
        const std::string target = url.substr(slash);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            error = "cannot write the download";
            return false;
        }
        int status = 0;
        std::string location;
        u64 received = 0;
        bool canceled = false;
        const auto result = MakeClient(host)->Get(
            target, httplib::Headers{{"User-Agent", kUserAgent}, {"Accept", "application/octet-stream"}},
            [&](const httplib::Response& response) {
                status = response.status;
                location = response.get_header_value("Location");
                return true;
            },
            [&](const char* data, size_t length) {
                if (status != 200) {
                    return true; // a redirect's body
                }
                if (file.write(data, static_cast<qint64>(length)) != static_cast<qint64>(length)) {
                    return false;
                }
                received += length;
                if (progress && !progress(received)) {
                    canceled = true;
                    return false;
                }
                return true;
            });
        file.close();
        if (canceled) {
            error = "canceled";
            return false;
        }
        if (!result) {
            error = httplib::to_string(result.error());
            return false;
        }
        if (status >= 300 && status < 400 && !location.empty()) {
            url = location;
            continue;
        }
        if (status != 200) {
            error = fmt::format("HTTP {}", status);
            return false;
        }
        return true;
    }
    error = "too many redirects";
    return false;
}

QString FileSha256(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        return {};
    }
    return QString::fromLatin1(hash.result().toHex());
}

/// Installs the zip once this process has exited: copies everything but the user folder over the
/// program folder, then starts the new version. Runs hidden; writes update-log.txt beside the zip
/// and shows a message if anything fails.
constexpr const char* kInstallScript = R"PS(param(
    [int]$ProcessId,
    [string]$Zip,
    [string]$Dest
)
$ErrorActionPreference = 'Stop'
$work = Split-Path -Parent $Zip
Start-Transcript -LiteralPath (Join-Path $work 'update-log.txt') -Force | Out-Null
try {
    Wait-Process -Id $ProcessId -Timeout 120 -ErrorAction SilentlyContinue
    $extracted = Join-Path $work 'extracted'
    if (Test-Path -LiteralPath $extracted) { Remove-Item -LiteralPath $extracted -Recurse -Force }
    Expand-Archive -LiteralPath $Zip -DestinationPath $extracted -Force
    $root = $extracted
    $top = @(Get-ChildItem -LiteralPath $extracted -Force)
    if ($top.Count -eq 1 -and $top[0].PSIsContainer -and
        -not (Get-ChildItem -LiteralPath $extracted -Filter '*.exe' -File)) {
        $root = $top[0].FullName
    }
    $exe = Get-ChildItem -LiteralPath $root -Filter "Fortune*.exe" -File | Select-Object -First 1
    if (-not $exe) { throw 'The download has no Fortune''s Eden program in it.' }
    foreach ($item in Get-ChildItem -LiteralPath $root -Force) {
        if ($item.Name -ieq 'user') { continue }  # saves, settings, keys and mods stay as they are
        for ($attempt = 1; ; $attempt++) {
            try {
                Copy-Item -LiteralPath $item.FullName -Destination $Dest -Recurse -Force
                break
            } catch {
                if ($attempt -ge 40) { throw }
                Start-Sleep -Milliseconds 500  # the old program may still be closing
            }
        }
    }
    Remove-Item -LiteralPath $extracted -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $Zip -Force -ErrorAction SilentlyContinue
    Start-Process -FilePath (Join-Path $Dest $exe.Name) -WorkingDirectory $Dest
} catch {
    Add-Type -AssemblyName System.Windows.Forms
    [System.Windows.Forms.MessageBox]::Show(
        "Fortune's Eden could not finish updating:`n`n$($_.Exception.Message)`n`nYour saves and settings are unchanged. You can download the update from GitHub instead.",
        "Fortune's Eden update") | Out-Null
} finally {
    Stop-Transcript | Out-Null
}
)PS";

} // Anonymous namespace

std::optional<Release> CheckForUpdate() {
    try {
        const auto result = MakeClient(kApiHost)->Get(
            kLatestPath, httplib::Headers{{"User-Agent", kUserAgent},
                                          {"Accept", "application/vnd.github+json"}});
        if (!result || result->status != 200) {
            LOG_INFO(Frontend, "Update check: no answer ({})",
                     result ? fmt::format("HTTP {}", result->status) : httplib::to_string(result.error()));
            return std::nullopt;
        }
        const auto json = nlohmann::json::parse(result->body);
        Release release;
        release.tag = json.value("tag_name", "");
        release.name = json.value("name", release.tag);
        release.html_url = json.value("html_url", "");
        if (json.value("draft", false) || json.value("prerelease", false)) {
            return std::nullopt;
        }
        if (!IsNewer(release.tag, CurrentVersion())) {
            LOG_INFO(Frontend, "Update check: {} is the newest release", CurrentVersion());
            return std::nullopt;
        }
        if (release.tag == SkippedTag()) {
            LOG_INFO(Frontend, "Update check: {} is available but was skipped", release.tag);
            return std::nullopt;
        }
        for (const auto& asset : json.value("assets", nlohmann::json::array())) {
            if (asset.value("name", "") != kAssetName) {
                continue;
            }
            release.asset_url = asset.value("browser_download_url", "");
            release.asset_size = asset.value("size", u64{0});
            const std::string digest = asset.value("digest", "");
            if (digest.rfind("sha256:", 0) == 0) {
                release.sha256 = digest.substr(7);
            }
        }
        if (release.asset_url.empty()) {
            LOG_WARNING(Frontend, "Update check: {} has no {}", release.tag, kAssetName);
            return std::nullopt;
        }
        LOG_INFO(Frontend, "Update check: {} is available (this is {})", release.tag,
                 CurrentVersion());
        return release;
    } catch (const std::exception& e) {
        LOG_WARNING(Frontend, "Update check failed: {}", e.what());
        return std::nullopt;
    }
}

bool OfferUpdate(QWidget* parent, const Release& release) {
    QMessageBox box(QMessageBox::Information, Tr("Update available"),
                    Tr("Fortune's Eden %1 is available. You have %2.\n\n"
                       "Update now? It downloads the new version, closes the emulator and "
                       "starts the new one. Your saves, settings, keys and mods are kept.")
                        .arg(QString::fromStdString(release.tag),
                             QString::fromStdString(CurrentVersion())),
                    QMessageBox::NoButton, parent);
    QPushButton* update_button = box.addButton(Tr("Update Now"), QMessageBox::AcceptRole);
    QPushButton* notes_button = box.addButton(Tr("What's New"), QMessageBox::HelpRole);
    QPushButton* skip_button = box.addButton(Tr("Skip This Version"), QMessageBox::RejectRole);
    box.addButton(Tr("Later"), QMessageBox::RejectRole);
    box.setDefaultButton(update_button);
    for (;;) {
        box.exec();
        if (box.clickedButton() == notes_button) {
            QDesktopServices::openUrl(QUrl(QString::fromStdString(release.html_url)));
            continue;
        }
        break;
    }
    if (box.clickedButton() == skip_button) {
        SkipTag(release.tag);
        return false;
    }
    if (box.clickedButton() != update_button) {
        return false;
    }
    if (release.sha256.size() != 64) {
        QMessageBox::warning(parent, Tr("Update"),
                             Tr("GitHub did not list a checksum for this download, so it can't be "
                                "verified. Please download the update from the release page."));
        QDesktopServices::openUrl(QUrl(QString::fromStdString(release.html_url)));
        return false;
    }

    const QString work = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                             .filePath(QStringLiteral("FortunesEdenUpdate"));
    QDir().mkpath(work);
    const QString zip_path = QDir(work).filePath(QString::fromLatin1(kAssetName));

    const int maximum = release.asset_size > 0 ? 1000 : 0;
    QProgressDialog progress(Tr("Downloading Fortune's Eden %1...").arg(QString::fromStdString(release.tag)),
                             Tr("Cancel"), 0, maximum, parent);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setValue(0);
    QApplication::processEvents();
    std::string error;
    const bool downloaded = Download(
        release.asset_url, zip_path,
        [&](u64 bytes) {
            if (release.asset_size > 0) {
                progress.setValue(static_cast<int>((std::min)(bytes * 1000 / release.asset_size, u64{999})));
            }
            QApplication::processEvents();
            return !progress.wasCanceled();
        },
        error);
    progress.close();
    if (!downloaded) {
        if (error != "canceled") {
            QMessageBox::warning(parent, Tr("Update"),
                                 Tr("The download failed (%1). You can try again next time, or "
                                    "download the update from the release page.")
                                     .arg(QString::fromStdString(error)));
        }
        QFile::remove(zip_path);
        return false;
    }
    const QString actual = FileSha256(zip_path);
    if (actual.compare(QString::fromStdString(release.sha256), Qt::CaseInsensitive) != 0) {
        LOG_ERROR(Frontend, "Update: checksum mismatch (expected {}, got {})", release.sha256,
                  actual.toStdString());
        QMessageBox::warning(parent, Tr("Update"),
                             Tr("The download didn't match GitHub's checksum, so it wasn't "
                                "installed. Please try again later."));
        QFile::remove(zip_path);
        return false;
    }

    const QString script_path = QDir(work).filePath(QStringLiteral("install-update.ps1"));
    {
        QFile script(script_path);
        if (!script.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            QMessageBox::warning(parent, Tr("Update"), Tr("Couldn't prepare the update."));
            return false;
        }
        // UTF-8 with a byte order mark, so Windows PowerShell reads the apostrophes right.
        script.write("\xEF\xBB\xBF");
        script.write(kInstallScript);
    }
    const QString dest = QDir::toNativeSeparators(QCoreApplication::applicationDirPath());
    const bool started = QProcess::startDetached(
        QStringLiteral("powershell.exe"),
        {QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
         QStringLiteral("-WindowStyle"), QStringLiteral("Hidden"), QStringLiteral("-File"),
         QDir::toNativeSeparators(script_path), QStringLiteral("-ProcessId"),
         QString::number(QCoreApplication::applicationPid()), QStringLiteral("-Zip"),
         QDir::toNativeSeparators(zip_path), QStringLiteral("-Dest"), dest});
    if (!started) {
        QMessageBox::warning(parent, Tr("Update"), Tr("Couldn't start the updater."));
        return false;
    }
    LOG_INFO(Frontend, "Update: installing {} after the emulator closes", release.tag);
    return true;
}

} // namespace FefwUpdater
