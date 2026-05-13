#pragma once

class CrossPointSettings;
class CrossPointState;
#if CROSSPOINT_ENABLE_WIFI_SETTINGS
class WifiCredentialStore;
#endif
#if CROSSPOINT_ENABLE_KOREADER_SYNC
class KOReaderCredentialStore;
#endif
class RecentBooksStore;

namespace JsonSettingsIO {

// CrossPointSettings
bool saveSettings(const CrossPointSettings& s, const char* path);
bool loadSettings(CrossPointSettings& s, const char* json, bool* needsResave = nullptr);

// CrossPointState
bool saveState(const CrossPointState& s, const char* path);
bool loadState(CrossPointState& s, const char* json);

#if CROSSPOINT_ENABLE_WIFI_SETTINGS
// WifiCredentialStore
bool saveWifi(const WifiCredentialStore& store, const char* path);
bool loadWifi(WifiCredentialStore& store, const char* json, bool* needsResave = nullptr);
#endif

#if CROSSPOINT_ENABLE_KOREADER_SYNC
// KOReaderCredentialStore
bool saveKOReader(const KOReaderCredentialStore& store, const char* path);
bool loadKOReader(KOReaderCredentialStore& store, const char* json, bool* needsResave = nullptr);
#endif

// RecentBooksStore
bool saveRecentBooks(const RecentBooksStore& store, const char* path);
bool loadRecentBooks(RecentBooksStore& store, const char* json);

}  // namespace JsonSettingsIO
