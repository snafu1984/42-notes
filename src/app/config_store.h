#pragma once
#include <Arduino.h>

// Runtime configuration / secrets, persisted in NVS (the `nvs` partition) rather
// than baked into the app image. Keeps Wi-Fi and API credentials out of the
// firmware binary that gets flashed/shared. Provisioned at runtime over the
// setup portal (SoftAP) or seeded once from secrets.h for backward compat.
namespace cfg {
  void   begin();                                       // load NVS, one-time seed from secrets.h
  String wifiSsid();
  String wifiPass();
  String openaiKey();
  bool   hasWifi();                                     // Wi-Fi credentials present
  bool   hasOpenAiKey();                                // transcription key present
  bool   setWifi(const String& ssid, const String& pass);
  bool   setOpenAiKey(const String& key);

  void   factoryReset();                                // wipe all stored config
}
