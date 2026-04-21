#!/usr/bin/env python3
"""
네트워크 기능 제거 빌드 설정이 적용되었는지 검사한다.
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


def main() -> int:
    failures: list[str] = []

    platformio = PLATFORMIO_INI.read_text(encoding="utf-8")
    if "-DCROSSPOINT_ENABLE_NETWORK=0" not in platformio:
        failures.append("platformio.ini should disable network features via CROSSPOINT_ENABLE_NETWORK=0")
    for excluded in [
        "-<activities/network/>",
        "-<network/>",
        "-<WifiCredentialStore.cpp>",
        "-<activities/browser/OpdsBookBrowserActivity.cpp>",
        "-<activities/reader/KOReaderSyncActivity.cpp>",
    ]:
        if excluded not in platformio:
            failures.append(f"platformio.ini should exclude {excluded} from src build")
    for ignored in [
        "KOReaderSync",
        "OpdsParser",
        "WebSockets",
    ]:
        if ignored not in platformio:
            failures.append(f"platformio.ini should ignore {ignored} in a networkless build")

    settings_cpp = SETTINGS_CPP.read_text(encoding="utf-8")
    if "#if CROSSPOINT_ENABLE_NETWORK" not in settings_cpp:
        failures.append("SettingsActivity.cpp should gate network-only actions with CROSSPOINT_ENABLE_NETWORK")

    home_cpp = HOME_CPP.read_text(encoding="utf-8")
    if "#if CROSSPOINT_ENABLE_NETWORK" not in home_cpp:
        failures.append("HomeActivity.cpp should gate file-transfer / OPDS menu entries with CROSSPOINT_ENABLE_NETWORK")

    activity_manager_cpp = ACTIVITY_MANAGER_CPP.read_text(encoding="utf-8")
    if "#if CROSSPOINT_ENABLE_NETWORK" not in activity_manager_cpp:
        failures.append("ActivityManager.cpp should gate network activity includes and navigation with CROSSPOINT_ENABLE_NETWORK")

    epub_menu_cpp = EPUB_MENU_CPP.read_text(encoding="utf-8")
    if "#if CROSSPOINT_ENABLE_NETWORK" not in epub_menu_cpp:
        failures.append("EpubReaderMenuActivity.cpp should gate the sync menu entry with CROSSPOINT_ENABLE_NETWORK")

    epub_reader_cpp = EPUB_READER_CPP.read_text(encoding="utf-8")
    if "#if CROSSPOINT_ENABLE_NETWORK" not in epub_reader_cpp:
        failures.append("EpubReaderActivity.cpp should gate sync handling with CROSSPOINT_ENABLE_NETWORK")

    main_cpp = MAIN_CPP.read_text(encoding="utf-8")
    if "#if CROSSPOINT_ENABLE_NETWORK" not in main_cpp:
        failures.append("main.cpp should gate KOReader store loading with CROSSPOINT_ENABLE_NETWORK")

    json_settings_cpp = JSON_SETTINGS_CPP.read_text(encoding="utf-8")
    if "#if CROSSPOINT_ENABLE_NETWORK" not in json_settings_cpp:
        failures.append("JsonSettingsIO.cpp should gate KOReader/WiFi serialization helpers with CROSSPOINT_ENABLE_NETWORK")

    power_manager_cpp = POWER_MANAGER_CPP.read_text(encoding="utf-8")
    if "#if CROSSPOINT_ENABLE_NETWORK" not in power_manager_cpp:
        failures.append("HalPowerManager.cpp should gate WiFi power-saving checks with CROSSPOINT_ENABLE_NETWORK")

    if failures:
        print("FAIL: Networkless build config is incomplete.")
        for failure in failures:
            print(f" - {failure}")
        return 1

    print("PASS: Networkless build config is applied.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
