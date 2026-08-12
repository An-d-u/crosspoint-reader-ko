#!/usr/bin/env python3
"""
AP 웹 파일 전송과 저장형 Wi-Fi 시간 동기화만 활성화되었는지 검사한다.
"""

from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PLATFORMIO_INI = ROOT / "platformio.ini"
SETTINGS_CPP = ROOT / "src" / "activities" / "settings" / "SettingsActivity.cpp"
HOME_CPP = ROOT / "src" / "activities" / "home" / "HomeActivity.cpp"
ACTIVITY_MANAGER_CPP = ROOT / "src" / "activities" / "ActivityManager.cpp"
EPUB_MENU_CPP = ROOT / "src" / "activities" / "reader" / "EpubReaderMenuActivity.cpp"
EPUB_READER_CPP = ROOT / "src" / "activities" / "reader" / "EpubReaderActivity.cpp"
MAIN_CPP = ROOT / "src" / "main.cpp"
JSON_SETTINGS_CPP = ROOT / "src" / "JsonSettingsIO.cpp"
POWER_MANAGER_CPP = ROOT / "lib" / "hal" / "HalPowerManager.cpp"
WEB_SERVER_ACTIVITY_CPP = ROOT / "src" / "activities" / "network" / "CrossPointWebServerActivity.cpp"
WEB_SERVER_CPP = ROOT / "src" / "network" / "CrossPointWebServer.cpp"


def main() -> int:
    failures: list[str] = []

    platformio = PLATFORMIO_INI.read_text(encoding="utf-8")
    for required_flag in [
        "-DCROSSPOINT_ENABLE_NETWORK=1",
        "-DCROSSPOINT_ENABLE_WEB_TRANSFER=1",
        "-DCROSSPOINT_ENABLE_WIFI_SETTINGS=1",
        "-DCROSSPOINT_ENABLE_OPDS=0",
        "-DCROSSPOINT_ENABLE_CALIBRE=0",
        "-DCROSSPOINT_ENABLE_KOREADER_SYNC=0",
        "-DCROSSPOINT_ENABLE_OTA=0",
    ]:
        if required_flag not in platformio:
            failures.append(f"platformio.ini should contain {required_flag}")

    for forbidden_exclusion in [
        "-<activities/network/>",
        "-<network/>",
    ]:
        if forbidden_exclusion in platformio:
            failures.append(f"web transfer build should not exclude {forbidden_exclusion}")

    for excluded in [
        "-<activities/network/NetworkModeSelectionActivity.cpp>",
        "-<activities/network/CalibreConnectActivity.cpp>",
        "-<network/HttpDownloader.cpp>",
        "-<network/OtaUpdater.cpp>",
        "-<network/WebDAVHandler.cpp>",
        "-<activities/browser/OpdsBookBrowserActivity.cpp>",
        "-<activities/reader/KOReaderSyncActivity.cpp>",
        "-<activities/settings/CalibreSettingsActivity.cpp>",
        "-<activities/settings/KOReaderAuthActivity.cpp>",
        "-<activities/settings/KOReaderSettingsActivity.cpp>",
        "-<activities/settings/OtaUpdateActivity.cpp>",
    ]:
        if excluded not in platformio:
            failures.append(f"platformio.ini should exclude {excluded} from src build")

    for required_source in [
        "-<WifiCredentialStore.cpp>",
        "-<activities/network/WifiSelectionActivity.cpp>",
    ]:
        if required_source in platformio:
            failures.append(f"study time sync should compile {required_source[2:-1]}")

    for ignored in [
        "KOReaderSync",
        "OpdsParser",
        "WebSockets",
    ]:
        if ignored not in platformio:
            failures.append(f"platformio.ini should ignore {ignored} while only simple AP upload is enabled")

    if "pre:scripts/build_html.py" in platformio:
        failures.append("simple AP upload should not run the large web UI generator")

    settings_cpp = SETTINGS_CPP.read_text(encoding="utf-8")
    for gate in [
        "#if CROSSPOINT_ENABLE_WIFI_SETTINGS",
        "#if CROSSPOINT_ENABLE_KOREADER_SYNC",
        "#if CROSSPOINT_ENABLE_OPDS",
        "#if CROSSPOINT_ENABLE_OTA",
    ]:
        if gate not in settings_cpp:
            failures.append(f"SettingsActivity.cpp should gate optional network action with {gate}")

    home_cpp = HOME_CPP.read_text(encoding="utf-8")
    if "#if CROSSPOINT_ENABLE_WEB_TRANSFER" not in home_cpp:
        failures.append("HomeActivity.cpp should show file transfer only with CROSSPOINT_ENABLE_WEB_TRANSFER")
    if "#if CROSSPOINT_ENABLE_OPDS" not in home_cpp:
        failures.append("HomeActivity.cpp should keep OPDS gated separately")

    activity_manager_cpp = ACTIVITY_MANAGER_CPP.read_text(encoding="utf-8")
    if "#if CROSSPOINT_ENABLE_WEB_TRANSFER" not in activity_manager_cpp:
        failures.append("ActivityManager.cpp should gate web transfer navigation separately")
    if "#if CROSSPOINT_ENABLE_OPDS" not in activity_manager_cpp:
        failures.append("ActivityManager.cpp should keep OPDS navigation gated separately")

    epub_menu_cpp = EPUB_MENU_CPP.read_text(encoding="utf-8")
    if "#if CROSSPOINT_ENABLE_KOREADER_SYNC" not in epub_menu_cpp:
        failures.append("EpubReaderMenuActivity.cpp should gate the sync menu entry with CROSSPOINT_ENABLE_KOREADER_SYNC")

    epub_reader_cpp = EPUB_READER_CPP.read_text(encoding="utf-8")
    if "#if CROSSPOINT_ENABLE_KOREADER_SYNC" not in epub_reader_cpp:
        failures.append("EpubReaderActivity.cpp should gate sync handling with CROSSPOINT_ENABLE_KOREADER_SYNC")

    main_cpp = MAIN_CPP.read_text(encoding="utf-8")
    if "#if CROSSPOINT_ENABLE_KOREADER_SYNC" not in main_cpp:
        failures.append("main.cpp should gate KOReader store loading with CROSSPOINT_ENABLE_KOREADER_SYNC")

    json_settings_cpp = JSON_SETTINGS_CPP.read_text(encoding="utf-8")
    if "#if CROSSPOINT_ENABLE_WIFI_SETTINGS" not in json_settings_cpp:
        failures.append("JsonSettingsIO.cpp should only compile WiFi credential serialization with WiFi settings")
    if "#if CROSSPOINT_ENABLE_KOREADER_SYNC" not in json_settings_cpp:
        failures.append("JsonSettingsIO.cpp should keep KOReader serialization gated separately")

    power_manager_cpp = POWER_MANAGER_CPP.read_text(encoding="utf-8")
    if "#if CROSSPOINT_ENABLE_NETWORK" not in power_manager_cpp:
        failures.append("HalPowerManager.cpp should gate WiFi power-saving checks with CROSSPOINT_ENABLE_NETWORK")

    web_server_activity_cpp = WEB_SERVER_ACTIVITY_CPP.read_text(encoding="utf-8")
    if "NetworkModeSelectionActivity" in web_server_activity_cpp or "WifiSelectionActivity" in web_server_activity_cpp:
        failures.append("CrossPointWebServerActivity.cpp should be AP-only without STA/network selection")

    web_server_cpp = WEB_SERVER_CPP.read_text(encoding="utf-8")
    for forbidden in ["WebSocketsServer", "WebDAVHandler", "SettingsPageHtml", "jszip_minJs"]:
        if forbidden in web_server_cpp:
            failures.append(f"simple upload server should not reference {forbidden}")
    if 'server->on("/upload"' not in web_server_cpp:
        failures.append("simple upload server should expose /upload")

    if failures:
        print("FAIL: AP web transfer and saved-WiFi time sync config is incomplete.")
        for failure in failures:
            print(f" - {failure}")
        return 1

    print("PASS: AP web transfer and saved-WiFi time sync config is applied.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
