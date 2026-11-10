#include "Arduino.h"
#include "SD_MMC.h"
#include "WiFi.h"
#include "HTTPClient.h"
#include "WiFiClientSecure.h"
#include <WebServer.h>
#include <vector>
#include "driver/i2c_master.h"
#include "esp_heap_caps.h"

extern "C" {
#include "config.h"
#include "i2c_bsp/i2c_bsp.h"
#include "audio/audio_bsp.h"
}

extern void startTransferMode();
extern void stopTransferMode();

#include "power/board_power_bsp.h"
#include "display/epaper_driver_bsp.h"
#include "logo_bitmap.h"
#include "secrets.h"
#include "sounds.h"

#include <Adafruit_GFX.h>
#include <pgmspace.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <strings.h>

#include "types.h"
#include "globals.h"
#include "app/draw.h"
#include "app/battery.h"
#include "app/rtc.h"
#include "app/notes.h"
#include "app/ui.h"
#include "app/buttons.h"
#include "app/network.h"
#include "app/sleep.h"
#include "app/record.h"
#include "app/config_store.h"
#include "app/markdown.h"

// All pin, timing, path and threshold constants live in config.h.

// ─── Content arrays ───────────────────────────────────────────────────────
const char* MENU_ITEMS[]     = { "Notes", "Sync", "Settings" };
const char* SETTINGS_ITEMS[] = { "Sounds", "Transfer", "Device", "Erase All", "Reset" };

// ─── Global variable definitions ─────────────────────────────────────────
board_power_bsp_t      board(EPD_PWR_PIN, Audio_PWR_PIN, VBAT_PWR_PIN);
epaper_driver_display* display = nullptr;

std::vector<NoteEntry> noteIndex;

AppState state          = STATE_IDLE;
int      listCursor     = 0;
int      menuCursor     = 0;
int      settingsCursor = 0;
int      activeFilter   = -1;
int      lastRecNum     = -1;

uint32_t lastActivityMs      = 0;
bool     wokeFromUltraSleep  = false;
bool     wakeToMenuRequested = false;
bool     wakeToRecRequested  = false;

uint32_t tickerLastMs = 0;
int      tickerOffset = 0;
int      tickerCursor = -1;

WebServer transferServer(80);
bool      transferServerActive = false;
String    transferUrl          = "";
AppState  transferReturnState  = STATE_IDLE;
DNSServer dnsServer;
bool      captivePortalActive  = false;

bool timeReady    = false;
bool audioPlaying = false;
bool stopPlayback = false;

int detailScrollPage = 0;
int detailTotalLines = 0;

uint32_t lastBatCheckMs    = 0;
bool     batLowWarned      = false;
bool     batWarnActive     = false;
uint32_t batWarnShowUntilMs = 0;

// ─── REC + POWER combination: enter Transfer from the idle screen ────────
bool handleTransferCombo() {
  static bool active = false;
  static bool fired = false;
  static uint32_t startedMs = 0;

  const bool recDown = isDown(BTN_REC);
  const bool pwrDown = isDown(BTN_PWR);
  const bool bothDown = recDown && pwrDown;

  if (!active && bothDown && state == STATE_IDLE) {
    active = true;
    fired = false;
    startedMs = millis();
    resetButtonEvents();
    resetActivity();
  }

  if (!active) return false;

  // Do not let the normal UI interpret either key while the chord is held.
  resetButtonEvents();

  if (!recDown && !pwrDown) {
    active = false;
    fired = false;
    resetButtonEvents();
    return true;
  }

  if (!fired && millis() - startedMs >= 2000UL) {
    fired = true;
    soundSelect();
    startTransferMode();
  }
  return true;
}

// Transfer mode owns button handling completely. It is armed only after both
// keys have been released once, preventing the entry gesture from also being
// interpreted as an exit gesture.
void leaveTransferMode() {
  soundBack();
  stopTransferMode();
  resetButtonEvents();
  resetActivity();
  if (transferReturnState == STATE_SETTINGS) {
    state = STATE_SETTINGS;
    showSettings(settingsCursor);
  } else {
    state = STATE_IDLE;
    showIdle();
  }
}

void handleTransferMode() {
  static bool exitArmed = false;
  static uint32_t exitHoldStartedMs = 0;

  if (captivePortalActive) dnsServer.processNextRequest();
  if (transferServerActive) transferServer.handleClient();
  handleArduinoOta();

  const bool recDown = isDown(BTN_REC);
  const bool pwrDown = isDown(BTN_PWR);

  if (!exitArmed) {
    if (!recDown && !pwrDown) {
      exitArmed = true;
      exitHoldStartedMs = 0;
      resetButtonEvents();
    }
    return;
  }

  if (recDown && pwrDown) {
    // A chord takes priority over the single-button back action.
    resetButtonEvents();
    if (exitHoldStartedMs == 0) exitHoldStartedMs = millis();
    if (millis() - exitHoldStartedMs >= 2000UL) {
      exitArmed = false;
      exitHoldStartedMs = 0;
      leaveTransferMode();
    }
    return;
  }

  exitHoldStartedMs = 0;

  // Keep the original quick exit as well: holding REC alone exits after the
  // normal long-press threshold. This avoids making the chord the only way out.
  if (readButtonEvent(BTN_REC) == EV_LONG) {
    exitArmed = false;
    leaveTransferMode();
  }
}

// ─── Power latch ──────────────────────────────────────────────────────────
void keepBatteryPowerOn() {
  pinMode(PWR_HOLD_PIN, OUTPUT);
  digitalWrite(PWR_HOLD_PIN, HIGH);
}

// ─── Flow functions ───────────────────────────────────────────────────────
void startRecordFlow() {
  state = STATE_RECORDING;
  showRecording();

  palaSoundSetEnabled(false);
  bool recOk = record();
  palaSoundSetEnabled(true);

  if (!recOk) {
    showError("REC FAIL");
    delay(1600);
    state = STATE_IDLE;
    showIdle();
    return;
  }

  // Register the new recording immediately in the local note index.
  addToIndex(lastRecNum, false);

  soundSaved();

  state = STATE_SAVED;
  showSaved(lastRecNum);
  delay(900);
  resetActivity();
  state = STATE_IDLE;
  showIdle();
}

void startSyncFlow() {
  if (!cfg::hasWifi()) {
    showError("NO WIFI CFG");
    delay(1800);
    showIdle();
    return;
  }

  const int MAX_TRIES = 20;
  showWifiConnecting(0, MAX_TRIES);

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);        // creds live in our NVS; don't wear the WiFi NVS
  WiFi.setAutoReconnect(true);   // SDK recovers (with backoff) if the link drops mid-sync
  String ssid = cfg::wifiSsid(), pass = cfg::wifiPass();
  WiFi.begin(ssid.c_str(), pass.c_str());
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < MAX_TRIES) {
    delay(500); tries++;
    showWifiConnecting(tries, MAX_TRIES);
  }

  if (WiFi.status() == WL_CONNECTED) {
    syncTimeFromNTP(6000);
    transcribeAll();
    loadIndex();
    markdownProcessPending(); // create local Markdown independently of any cloud vault
    WiFi.disconnect(true);
    showDone();
    soundSuccess();
    delay(1600);
  } else {
    showError("NO WIFI");
    delay(1800);
  }

  if (wakeToMenuRequested) {
    menuCursor = 0;
    state = STATE_MENU;
    showMenu(menuCursor);
  } else {
    showIdle();
  }
}

void startTransferMode() {
  transferReturnState = state;
  state = STATE_TRANSFER;
  resetButtonEvents();
  showTransferConnecting();

  // First-time setup: no Wi-Fi credentials yet -> host a SoftAP so the user can
  // open the portal and provision Wi-Fi + OpenAI key (saved to NVS).
  if (!cfg::hasWifi()) {
    WiFi.persistent(false);
    WiFi.mode(WIFI_AP);
    bool apOk = WiFi.softAP("snafuNote-Setup");
    delay(200);                                   // let the AP + DHCP server come up
    IPAddress apIP = WiFi.softAPIP();
    Serial.printf("[Transfer] SoftAP '%s' start=%d ip=%s\n",
                  "snafuNote-Setup", apOk, apIP.toString().c_str());

    // Captive portal: resolve every DNS query to us so any URL (and the OS's
    // own connectivity probe) lands on the device and pops the setup page.
    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(53, "*", apIP);
    captivePortalActive = true;

    setupTransferServer();
    transferServer.begin();
    transferServerActive = true;
    transferUrl = apIP.toString();
    Serial.println("[Transfer] HTTP server started on :80");
    showTransferMode(transferUrl.c_str());
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname("snafuNote");
  String ssid = cfg::wifiSsid(), pass = cfg::wifiPass();
  WiFi.begin(ssid.c_str(), pass.c_str());

  const int MAX_TRIES = 24;
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < MAX_TRIES) {
    delay(500); tries++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    showError("NO WIFI");
    delay(1600);
    state = STATE_SETTINGS;
    showSettings(settingsCursor);
    return;
  }

  syncTimeFromNTP(8000);
  startArduinoOta();
  setupTransferServer();
  transferServer.begin();
  transferServerActive = true;

  IPAddress ip = WiFi.localIP();
  transferUrl = ip.toString();
  showTransferMode(transferUrl.c_str());
}

// ─── Setup ────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== " DEVICE_NAME_NOTE " " FIRMWARE_VERSION " ===");

  cfg::begin();   // load Wi-Fi / API secrets from NVS (seeded from secrets.h once)

  pinMode(BTN_REC, INPUT_PULLUP);
  pinMode(BTN_PWR, INPUT_PULLUP);

  board.VBAT_POWER_ON();

  wokeFromUltraSleep  = (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1);
  delay(50);

  wakeToMenuRequested = (wokeFromUltraSleep && digitalRead(BTN_PWR) == LOW);
  wakeToRecRequested  = (wokeFromUltraSleep && digitalRead(BTN_REC) == LOW);

  resetActivity();
  keepBatteryPowerOn();
  delay(20);

  board.POWEER_EPD_ON();
  board.POWEER_Audio_ON();
  delay(200);

  custom_lcd_spi_t dispCfg = {};
  dispCfg.cs       = EPD_CS_PIN;
  dispCfg.dc       = EPD_DC_PIN;
  dispCfg.rst      = EPD_RST_PIN;
  dispCfg.busy     = EPD_BUSY_PIN;
  dispCfg.mosi     = EPD_MOSI_PIN;
  dispCfg.scl      = EPD_SCK_PIN;
  dispCfg.spi_host = EPD_SPI_NUM;
  dispCfg.buffer_len = (200*200)/8;

  display = new epaper_driver_display(200, 200, dispCfg);
  display->EPD_Init();
  display->EPD_Clear();
  display->EPD_DisplayPartBaseImage();
  display->EPD_Init_Partial();

  i2c_master_Init();
  delay(50);

  audio_bsp_init();
  audio_play_init();

  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  if (!SD_MMC.begin("/sdcard", true)) {
    showError("SD ERR");
    while (true) delay(1000);
  }
  if (!SD_MMC.exists(NOTES_DIR)) SD_MMC.mkdir(NOTES_DIR);
  loadIndex();
  Serial.printf("[SD] %d notes\n", (int)noteIndex.size());

  if (wakeToMenuRequested) {
    menuCursor = 0;
    state = STATE_MENU;
    showMenu(menuCursor);
  } else if (wakeToRecRequested) {
    startRecordFlow();
  } else {
    showIdle();
  }
}

// ─── Serial provisioning ────────────────────────────────────────────────────
// Reliable setup over USB (no SoftAP/captive-portal needed). Send line commands:
//   SSID=<network>     PASS=<password>     KEY=<openai key>     SHOW     RESET
// Wi-Fi is saved when PASS= follows a SSID=. Credentials are stored in NVS.
void handleSerialConfig() {
  static String line;
  static String pendingSsid;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') {
      line += c;
      if (line.length() > 256) line = "";   // overflow guard
      continue;
    }
    line.trim();
    if (line.length() == 0) { line = ""; continue; }

    if (line.startsWith("SSID=")) {
      pendingSsid = line.substring(5);
      Serial.printf("[cfg] ssid buffered ('%s'); now send PASS=<password>\n", pendingSsid.c_str());
    } else if (line.startsWith("PASS=")) {
      if (pendingSsid.length() > 0) {
        cfg::setWifi(pendingSsid, line.substring(5));
        Serial.printf("[cfg] wifi saved for ssid '%s'\n", pendingSsid.c_str());
        pendingSsid = "";
      } else {
        Serial.println("[cfg] send SSID=<network> first");
      }
    } else if (line.startsWith("KEY=")) {
      cfg::setOpenAiKey(line.substring(4));
      Serial.println("[cfg] openai key saved");
    } else if (line == "SHOW") {
      Serial.printf("[cfg] wifi=%s  openai_key=%s\n",
        cfg::hasWifi() ? cfg::wifiSsid().c_str() : "(none)",
        cfg::hasOpenAiKey() ? "set" : "(none)");
    } else if (line == "RESET") {
      cfg::factoryReset();
      Serial.println("[cfg] factory reset done");
    } else {
      Serial.println("[cfg] cmds: SSID= PASS= KEY= SHOW RESET");
    }
    line = "";
  }
}

// ─── Main loop ────────────────────────────────────────────────────────────
void loop() {
  handleSerialConfig();
  serviceDisplay();      // paint any pending (coalesced) redraw when the panel is free

  // Transfer mode is an isolated UI state: only its web server, OTA service
  // and explicit two-button exit gesture are processed here.
  if (state == STATE_TRANSFER) {
    handleTransferMode();
    delay(LOOP_DELAY_MS);
    return;
  }

  if (state != STATE_RECORDING && state != STATE_TRANSFER) {
    if (millis() - lastActivityMs > ULTRA_SLEEP_MS) {
      enterUltraSleep();
      return;
    }
  }

  if (state == STATE_NOTE_LIST && activeTickerNeedsScroll(listCursor)) {
    if (millis() - tickerLastMs > TICKER_INTERVAL_MS) {
      tickerLastMs = millis();
      tickerOffset++;
      showNoteList(listCursor);
      return;
    }
  }

  if (captivePortalActive)  dnsServer.processNextRequest();
  if (transferServerActive) transferServer.handleClient();
  handleArduinoOta();

  // Battery warning: dismiss after 2.5 s without blocking
  if (batWarnActive && millis() >= batWarnShowUntilMs) {
    batWarnActive = false;
    switch (state) {
      case STATE_IDLE:           showIdle();                     break;
      case STATE_MENU:           showMenu(menuCursor);           break;
      case STATE_NOTE_LIST:      showNoteList(listCursor);       break;
      case STATE_NOTE_DETAIL:    showNoteDetail(listCursor);     break;
      case STATE_SETTINGS:       showSettings(settingsCursor);   break;
      case STATE_DEVICE_INFO:    showDeviceInfo();               break;
      case STATE_DELETE_CONFIRM: {
        int idx = noteAtFilteredIndex(listCursor);
        if (idx >= 0) showDeleteConfirm(noteIndex[idx].num);
        break;
      }
      default: break;
    }
  }

  // Periodic battery check
  if (state != STATE_RECORDING && !audioPlaying && !batWarnActive) {
    if (millis() - lastBatCheckMs > BAT_CHECK_INTERVAL_MS) {
      lastBatCheckMs = millis();
      int pct = readBatteryPercent();
      if (pct >= 0 && pct <= BAT_LOW_THRESHOLD && !batLowWarned) {
        batLowWarned        = true;
        batWarnActive       = true;
        batWarnShowUntilMs  = millis() + 2500;
        showBatteryLow(pct);
      } else if (pct > BAT_RECOVER_THRESHOLD) {
        batLowWarned = false;
      }
    }
  }

   // REC + POWER combination takes priority over normal button handling.
  if (handleTransferCombo()) {
    // Combination handled; skip normal button processing this iteration.
  }

  // IDLE ─────────────────────────────────────────────────────────────────
  else if (state == STATE_IDLE) {
    if (handleIdleRec()) return;

    ButtonEvent pwr = readButtonEvent(BTN_PWR);
    if (pwr == EV_SINGLE || pwr == EV_LONG) {
      soundSelect();
      menuCursor = 0;
      state = STATE_MENU;
      showMenu(menuCursor);
    }
  }

  // MENU ────────────────────────────────────────────────────────────────
  else if (state == STATE_MENU) {
    ButtonEvent rec = readButtonEvent(BTN_REC);
    ButtonEvent pwr = readButtonEvent(BTN_PWR);

    if (pwr == EV_SINGLE) {
      soundNext();
      menuCursor = (menuCursor + 1) % MENU_COUNT;
      requestRedraw();
    } else if (rec == EV_SINGLE) {
      soundSelect();
      if (menuCursor == 0) {
        activeFilter = -1; listCursor = 0;
        state = STATE_NOTE_LIST;
        showNoteList(listCursor);
      } else if (menuCursor == 1) {
        startSyncFlow();
      } else {
        settingsCursor = 0;
        state = STATE_SETTINGS;
        showSettings(settingsCursor);
      }
    } else if (rec == EV_LONG) {
      soundBack();
      state = STATE_IDLE;
      showIdle();
    }
  }

  // SETTINGS ────────────────────────────────────────────────────────────
  else if (state == STATE_SETTINGS) {
    ButtonEvent rec = readButtonEvent(BTN_REC);
    ButtonEvent pwr = readButtonEvent(BTN_PWR);

    if (pwr == EV_SINGLE) {
      soundNext();
      settingsCursor = (settingsCursor + 1) % SETTINGS_COUNT;
      requestRedraw();
    } else if (rec == EV_SINGLE) {
      soundSelect();
      if (settingsCursor == 0) {
        palaSoundSetEnabled(!palaSoundIsEnabled());
        showSettings(settingsCursor);
      } else if (settingsCursor == 1) {
        startTransferMode();
      } else if (settingsCursor == 2) {
        state = STATE_DEVICE_INFO;
        showDeviceInfo();
      } else if (settingsCursor == 3) {
        state = STATE_DELETE_ALL_CONFIRM;
        showDeleteAllConfirm((int)noteIndex.size());
      } else {
        state = STATE_RESET_CONFIRM;
        showResetConfirm();
      }
    } else if (rec == EV_LONG) {
      soundBack();
      state = STATE_MENU;
      showMenu(menuCursor);
    }
  }

  // RESET CONFIRM ───────────────────────────────────────────────────────
  else if (state == STATE_RESET_CONFIRM) {
    ButtonEvent rec = readButtonEvent(BTN_REC);
    ButtonEvent pwr = readButtonEvent(BTN_PWR);

    if (rec == EV_SINGLE) {
      cfg::factoryReset();          // wipe stored Wi-Fi + OpenAI key (notes on SD are kept)
      soundDelete();
      showResetDone();
      delay(1400);
      ESP.restart();                // reboot unprovisioned -> SoftAP setup available again
    } else if (pwr == EV_SINGLE || rec == EV_LONG) {
      soundBack();
      state = STATE_SETTINGS;
      showSettings(settingsCursor);
    }
  }

  // ERASE ALL CONFIRM ───────────────────────────────────────────────────
  else if (state == STATE_DELETE_ALL_CONFIRM) {
    ButtonEvent rec = readButtonEvent(BTN_REC);
    ButtonEvent pwr = readButtonEvent(BTN_PWR);

    if (rec == EV_SINGLE) {
      deleteAllNotes();
      activeFilter = -1; listCursor = 0;
      soundDelete();
      showDeleteAllDone();
      delay(1300);
      menuCursor = 0;
      state = STATE_MENU;
      showMenu(menuCursor);
    } else if (pwr == EV_SINGLE || rec == EV_LONG) {
      soundBack();
      state = STATE_SETTINGS;
      showSettings(settingsCursor);
    }
  }

  // DEVICE INFO ─────────────────────────────────────────────────────────
  else if (state == STATE_DEVICE_INFO) {
    ButtonEvent rec = readButtonEvent(BTN_REC);
    ButtonEvent pwr = readButtonEvent(BTN_PWR);

    if (rec == EV_LONG || rec == EV_SINGLE || pwr == EV_SINGLE) {
      soundBack();
      state = STATE_SETTINGS;
      showSettings(settingsCursor);
    }
  }

  // NOTE LIST ───────────────────────────────────────────────────────────
  else if (state == STATE_NOTE_LIST) {
    ButtonEvent rec = readButtonEvent(BTN_REC);
    ButtonEvent pwr = readButtonEvent(BTN_PWR);
    int count = filteredCount();

    if (pwr == EV_SINGLE && count > 0) {
      soundNext();
      listCursor = (listCursor + 1) % count;
      requestRedraw();
    } else if (rec == EV_SINGLE && count > 0) {
      soundSelect();
      detailScrollPage = 0;
      state = STATE_NOTE_DETAIL;
      showNoteDetail(listCursor);
    } else if (rec == EV_LONG) {
      soundBack();
      state = STATE_MENU;
      showMenu(menuCursor);
    }
  }

  // NOTE DETAIL ─────────────────────────────────────────────────────────
  else if (state == STATE_NOTE_DETAIL) {
    ButtonEvent rec = readButtonEvent(BTN_REC);
    ButtonEvent pwr = readButtonEvent(BTN_PWR);

    if (pwr == EV_SINGLE) {
      soundNext();
      const int linesPerPage = 7;
      int totalPages = (detailTotalLines + linesPerPage - 1) / linesPerPage;
      if (detailScrollPage + 1 < totalPages) {
        detailScrollPage++;
      } else {
        detailScrollPage = 0;
        int count = filteredCount();
        if (count > 0) listCursor = (listCursor + 1) % count;
      }
      requestRedraw();
    } else if (rec == EV_SINGLE) {
      int idx = noteAtFilteredIndex(listCursor);
      if (idx >= 0) {
        char wavPath[64];
        snprintf(wavPath, sizeof(wavPath), "%s/note_%03d.wav", NOTES_DIR, noteIndex[idx].num);
        showPlaybackOverlay();
        playWavFile(wavPath);
        showNoteDetail(listCursor);
      }
    } else if (pwr == EV_LONG) {                 // hold power = delete
      int idx = noteAtFilteredIndex(listCursor);
      if (idx >= 0) {
        state = STATE_DELETE_CONFIRM;
        showDeleteConfirm(noteIndex[idx].num);
      }
    } else if (rec == EV_LONG) {                  // hold record = back to list
      soundBack();
      detailScrollPage = 0;
      state = STATE_NOTE_LIST;
      showNoteList(listCursor);
    }
  }

  // DELETE CONFIRM ──────────────────────────────────────────────────────
  else if (state == STATE_DELETE_CONFIRM) {
    ButtonEvent rec = readButtonEvent(BTN_REC);
    ButtonEvent pwr = readButtonEvent(BTN_PWR);

    if (rec == EV_SINGLE) {
      int idx = noteAtFilteredIndex(listCursor);
      if (idx >= 0) {
        deleteNote(noteIndex[idx].num);
        soundDelete();
      }
      detailScrollPage = 0;
      listCursor = constrain(listCursor, 0, max(filteredCount() - 1, 0));
      state = STATE_NOTE_LIST;
      showNoteList(listCursor);
    } else if (pwr == EV_SINGLE || rec == EV_LONG) {
      soundBack();
      state = STATE_NOTE_DETAIL;
      showNoteDetail(listCursor);
    }
  }

  delay(LOOP_DELAY_MS);
}
