// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * GeekMagic Open Firmware
 * Copyright (C) 2026 Times-Z
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <Arduino.h>
#include <LittleFS.h>
#include <Arduino_GFX_Library.h>
#include <SPI.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPUpdateServer.h>

#include <Logger.h>
#include "project_version.h"
#include "config/ConfigManager.h"
#include "wireless/WiFiManager.h"
#include "display/DisplayManager.h"
#include "web/Webserver.h"
#include "web/Api.h"
#include "ntp/NTPClient.h"
#include "boot/RescueMode.h"
#include <array>

ConfigManager configManager;
const char* AP_SSID = "GeekMagic";
const char* AP_PASSWORD = "$str0ngPa$$w0rd";
WiFiManager* wifiManager = nullptr;
ESP8266HTTPUpdateServer httpUpdater;
static constexpr const char* KV_SALT_STR = "GeekMagicOpenFirmwareIsAwesome";
static size_t initial_free_heap = 0;
static constexpr size_t FREE_BUF_SIZE = 32;
static constexpr size_t MSG_BUF_SIZE = 96;

static constexpr uint32_t SERIAL_BAUD_RATE = 115200;
static constexpr uint32_t BOOT_DELAY_MS = 200;

Webserver* webserver = nullptr;
NTPClient* ntpClient = nullptr;

// Boot-time GIF autoplay is deferred: the device stays web-reachable until it has been idle
// (no web requests and no AP client connected) for AUTOPLAY_IDLE_MS, so the UI/OTA can be
// used right after boot. Any web activity or AP connection stops playback and re-opens the
// window, keeping the device always recoverable over the network.
//
// Sequence once the idle window elapses: play default.gif ONCE (intro), then default_1.gif
// on repeat. Either file may be absent (falls back to looping whichever exists).
static constexpr uint32_t AUTOPLAY_IDLE_MS = 60000;
static const char* autoplayIntroPath = nullptr;  // default.gif, played once
static const char* autoplayLoopPath = nullptr;   // default_1.gif, looped forever
static uint32_t setupDoneMs = 0;

enum class AutoplayPhase { Idle, Intro, Loop };
static AutoplayPhase autoplayPhase = AutoplayPhase::Idle;

// Once autoplay starts the radio is powered down (we can't be reached during playback
// anyway). It only comes back on the next boot, which re-opens the idle window for OTA.
static bool wifiDisabled = false;

/**
 * @brief Formats bytes into a human-readable string
 *
 * @param value Size in bytes
 * @return Formatted string
 */
static void formatBytes(size_t value, char* outBuf, size_t outBufSize) {
    constexpr std::array<const char*, 5> UNITS = {"B", "KB", "MB", "GB", "TB"};
    constexpr double THRESHOLD = 1024.0;

    auto val = static_cast<double>(value);
    int unit = 0;
    while (val >= THRESHOLD && unit < static_cast<int>(UNITS.size()) - 1) {
        val /= THRESHOLD;
        ++unit;
    }

    if (unit == 0) {
        snprintf(outBuf, outBufSize, "%u %s", static_cast<unsigned int>(value), UNITS[unit]);
    } else {
        snprintf(outBuf, outBufSize, "%.1f %s", val, UNITS[unit]);
    }
}

/**
 * @brief Check whether LittleFS contains at least one entry
 *
 * @return true if filesystem root has any file/dir entry
 */
static auto littleFsHasEntries() -> bool {
    Dir dir = LittleFS.openDir("/");
    return dir.next();
}

/**
 * @brief Return the first of two candidate LittleFS paths that exists, else nullptr.
 *        Both arguments must be string literals (their storage outlives the device).
 */
static auto firstExistingPath(const char* pathA, const char* pathB) -> const char* {
    if (LittleFS.exists(pathA)) {
        return pathA;
    }
    if (LittleFS.exists(pathB)) {
        return pathB;
    }
    return nullptr;
}

/**
 * @brief Initializes the system
 *
 */
void setup() {
    Serial.begin(SERIAL_BAUD_RATE);
    delay(BOOT_DELAY_MS);
    Serial.println("");
    Logger::info(("GeekMagic Open Firmware " + String(PROJECT_VER_STR)).c_str());

    const bool littleFsMounted = LittleFS.begin();
    bool littleFsReadyForStatic = littleFsMounted;

    if (!littleFsMounted) {
        Logger::error("Failed to mount LittleFS");
        Logger::warn("LittleFS unavailable, static web UI disabled", "Global");
    } else if (!littleFsHasEntries()) {
        littleFsReadyForStatic = false;
        Logger::warn("LittleFS mounted but empty, static web UI disabled", "Global");
    }

    SecureStorage::setSalt(KV_SALT_STR);

    if (configManager.secure.begin()) {
        Logger::info("SecureStorage initialized successfully", "ConfigManager");
    }

    if (configManager.load()) {
        Logger::info("Configuration loaded successfully");
    }

    if (RescueMode::checkBootLoop()) {
        RescueMode::run();
        EspClass::wdtEnable(WDTO_2S);

        return;
    }

    DisplayManager::begin();

    // Boot splash: logo baked into the firmware (scripts/mkbootlogo.sh regenerates it
    // from any image) instead of the old loading bar / RGB-flash / IP screen.
    DisplayManager::drawSplash();

    wifiManager = new WiFiManager(configManager.getSSID(), configManager.getPassword(), AP_SSID, AP_PASSWORD);
    wifiManager->begin();

    ntpClient = new NTPClient();
    ntpClient->begin();

    webserver = new Webserver();
    webserver->begin();

    initial_free_heap = ESP.getFreeHeap();  // NOLINT(readability-static-accessed-through-instance)

    registerApiEndpoints(webserver);

    if (!littleFsReadyForStatic) {
        httpUpdater.setup(&webserver->raw(), "/legacyupdate");
        Logger::warn("Enabled legacy OTA route because LittleFS is unavailable or empty", "Global");
    } else {
        webserver->serveStaticC("/", "/web/index.html", "text/html");
        webserver->serveStaticC("/config.json", "/config.json", "application/json");
        webserver->registerGenericStaticFallback("/web", true);
    }

    if (littleFsMounted) {
        autoplayIntroPath = firstExistingPath("/gifs/default.gif", "/gif/default.gif");
        autoplayLoopPath = firstExistingPath("/gifs/default_1.gif", "/gif/default_1.gif");
    }

    std::array<char, MSG_BUF_SIZE> autoplayBuf{};
    snprintf(autoplayBuf.data(), autoplayBuf.size(), "Autoplay: fsMounted=%s intro=%s loop=%s freeHeap=%u",
             littleFsMounted ? "true" : "false", autoplayIntroPath != nullptr ? autoplayIntroPath : "none",
             autoplayLoopPath != nullptr ? autoplayLoopPath : "none",
             ESP.getFreeHeap());  // NOLINT(readability-static-accessed-through-instance)
    Logger::info(autoplayBuf.data(), "Global");

    if (littleFsMounted && autoplayIntroPath == nullptr && autoplayLoopPath == nullptr) {
        Logger::warn("No default.gif/default_1.gif at /gifs/ or /gif/, listing LittleFS:", "Global");
        for (const char* dirPath : {"/gif", "/gifs"}) {
            Dir gifDir = LittleFS.openDir(dirPath);
            bool anyEntry = false;
            while (gifDir.next()) {
                anyEntry = true;
                snprintf(autoplayBuf.data(), autoplayBuf.size(), "  %s/%s (%u B)", dirPath, gifDir.fileName().c_str(),
                         static_cast<unsigned int>(gifDir.fileSize()));
                Logger::warn(autoplayBuf.data(), "Global");
            }
            if (!anyEntry) {
                snprintf(autoplayBuf.data(), autoplayBuf.size(), "  %s is empty or missing", dirPath);
                Logger::warn(autoplayBuf.data(), "Global");
            }
        }
    } else if (autoplayIntroPath != nullptr || autoplayLoopPath != nullptr) {
        snprintf(autoplayBuf.data(), autoplayBuf.size(), "Autoplay scheduled after %lus idle (web defers it)",
                 static_cast<unsigned long>(AUTOPLAY_IDLE_MS / 1000));
        Logger::info(autoplayBuf.data(), "Global");
    }

    setupDoneMs = millis();

    // enable watchdog before going to loop()
    // 2 seconds should be way more than the main loop needs to do stuff
    EspClass::wdtEnable(WDTO_2S);
}

/**
 * @brief Power down the WiFi radio for playback (safety + power; unreachable until reboot).
 */
static auto disableWifiForPlayback() -> void {
    if (wifiDisabled) {
        return;
    }
    Logger::warn("Idle window over: powering down WiFi for playback (reboot to restore)", "Global");
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    WiFi.forceSleepBegin();
    yield();
    wifiDisabled = true;
}

/**
 * @brief Start the looping phase: default_1.gif on repeat, or fall back to the intro gif.
 */
static auto startAutoplayLoop() -> void {
    const char* loopPath = (autoplayLoopPath != nullptr) ? autoplayLoopPath : autoplayIntroPath;
    if (loopPath == nullptr) {
        autoplayPhase = AutoplayPhase::Idle;
        return;
    }
    Logger::info((String("Autoplay loop: ") + loopPath).c_str(), "Global");
    autoplayPhase = DisplayManager::playGifFullScreen(loopPath, 0) ? AutoplayPhase::Loop : AutoplayPhase::Idle;
}

/**
 * @brief Deferred GIF autoplay gate.
 *
 * Once the device has been idle (no web requests, no AP client) for AUTOPLAY_IDLE_MS, plays
 * default.gif ONCE (intro) then default_1.gif on repeat. Any web activity or AP connection
 * stops playback, redraws the splash and re-opens the window, so the UI/OTA stay reachable.
 */
static auto updateAutoplay() -> void {
    if (autoplayIntroPath == nullptr && autoplayLoopPath == nullptr) {
        return;
    }

    const uint32_t lastActivity = Webserver::lastActivityMs();
    const uint32_t idleSince = (lastActivity != 0) ? lastActivity : setupDoneMs;
    const bool clientConnected = WiFi.softAPgetStationNum() > 0;
    const bool idleEnough = !clientConnected && (millis() - idleSince >= AUTOPLAY_IDLE_MS);

    if (!idleEnough) {
        if (autoplayPhase != AutoplayPhase::Idle) {
            Logger::info("Web activity / AP connection, stopping autoplay so UI stays reachable", "Global");
            DisplayManager::stopGif();
            DisplayManager::drawSplash();
            autoplayPhase = AutoplayPhase::Idle;
        }
        return;
    }

    switch (autoplayPhase) {
        case AutoplayPhase::Idle:
            // Committing to playback: shut the radio down first (kiosk mode until reboot).
            disableWifiForPlayback();
            if (autoplayIntroPath != nullptr) {
                Logger::info("Idle window elapsed, playing intro default.gif once", "Global");
                autoplayPhase =
                    DisplayManager::playGifOnce(autoplayIntroPath) ? AutoplayPhase::Intro : AutoplayPhase::Idle;
                if (autoplayPhase == AutoplayPhase::Idle) {
                    startAutoplayLoop();  // intro failed to start: go straight to loop
                }
            } else {
                startAutoplayLoop();
            }
            break;
        case AutoplayPhase::Intro:
            if (!DisplayManager::isGifPlaying()) {
                Logger::info("Intro finished, switching to looping default_1.gif", "Global");
                startAutoplayLoop();
            }
            break;
        case AutoplayPhase::Loop:
            break;
    }
}

void loop() {
    if (RescueMode::isActive()) {
        RescueMode::loop();
        return;
    }

    static bool bootStableMarked = false;
    if (!bootStableMarked && millis() >= BOOT_STABLE_MS) {
        RescueMode::markBootStable();
        bootStableMarked = true;
    }

    // Once WiFi is powered down for playback, the webserver/NTP have nothing to do.
    if (!wifiDisabled) {
        if (webserver != nullptr) {
            webserver->handleClient();
        }

        if (ntpClient != nullptr) {
            ntpClient->loop();
        }
    }

    updateAutoplay();

    DisplayManager::update();

    static unsigned long last_free_heap_log = 0;
    static constexpr unsigned long FREE_HEAP_LOG_INTERVAL_MS = 10000UL;
    unsigned long now = millis();

    if (now - last_free_heap_log >= FREE_HEAP_LOG_INTERVAL_MS) {
        last_free_heap_log = now;
        char freeBuf[FREE_BUF_SIZE];
        char initBuf[FREE_BUF_SIZE];
        char msgBuf[MSG_BUF_SIZE];

        formatBytes(ESP.getFreeHeap(), freeBuf,  // NOLINT(readability-static-accessed-through-instance)
                    sizeof(freeBuf));
        formatBytes(initial_free_heap, initBuf, sizeof(initBuf));

        snprintf(msgBuf, sizeof(msgBuf), "Free heap: %s (initial: %s)", freeBuf, initBuf);
        Logger::info(msgBuf);
    }

    EspClass::wdtFeed();  // kick watchdog
}
