#include "config_store.h"
#include <Preferences.h>
#include <string.h>
#include "secrets.h"

namespace {
  Preferences prefs;
  const char* NS = "snafu";

  // secrets.h ships with "...." placeholders; treat all-dots or empty as unset.
  bool isPlaceholder(const char* s) {
    if (!s || s[0] == '\0') return true;
    return strspn(s, ".") == strlen(s);
  }
}

namespace cfg {

void begin() {
  prefs.begin(NS, false);
  // One-time migration: seed NVS from compiled secrets.h only for real (non
  // placeholder) values that aren't already stored. After this, secrets live in
  // NVS and can be rotated at runtime without reflashing.
  if (!prefs.isKey("ssid") && !isPlaceholder(WIFI_SSID)) {
    prefs.putString("ssid", WIFI_SSID);
    prefs.putString("pass", WIFI_PASS);
  }
  if (!prefs.isKey("oaikey") && !isPlaceholder(OPENAI_KEY)) {
    prefs.putString("oaikey", OPENAI_KEY);
  }

  // Remove credentials left by the retired GitHub/Obsidian integration.
  prefs.remove("ghtok");
  prefs.remove("ghrepo");
  prefs.remove("ghbranch");
  prefs.remove("ghdir");
  prefs.remove("ghon");
  prefs.remove("ghai");
  prefs.remove("cfgv");
}

String wifiSsid()  { return prefs.getString("ssid", ""); }
String wifiPass()  { return prefs.getString("pass", ""); }
String openaiKey() { return prefs.getString("oaikey", ""); }

bool hasWifi()      { return wifiSsid().length() > 0; }
bool hasOpenAiKey() { return openaiKey().length() > 0; }

bool setWifi(const String& ssid, const String& pass) {
  if (ssid.length() == 0) return false;
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  return true;
}

bool setOpenAiKey(const String& key) {
  prefs.putString("oaikey", key);
  return true;
}

void factoryReset() { prefs.clear(); }

}  // namespace cfg
