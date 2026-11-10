#include "Arduino.h"
#include "config.h"
#include "globals.h"
#include "types.h"
#include "network.h"
#include "notes.h"
#include "rtc.h"
#include "ui.h"
#include "config_store.h"
#include "WiFi.h"
#include "WiFiClientSecure.h"
#include <WebServer.h>
#include <Update.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include "SD_MMC.h"
#include "esp_heap_caps.h"
#include "secrets.h"

// IDF built-in Mozilla CA root bundle (libmbedtls.a). Auto-maintained with the
// esp32 core, so server certs validate without shipping/rotating a pinned PEM.
extern const uint8_t x509_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509_crt_bundle_end[]   asm("_binary_x509_crt_bundle_end");

static bool transcribeOnce(const String& wavPath, int noteNum) {
  String oaiKey = cfg::openaiKey();
  if (oaiKey.length() == 0) { Serial.println("[Whisper] no API key set"); return false; }

  File f = SD_MMC.open(wavPath.c_str());
  if (!f) return false;
  size_t fileSize = f.size();

  String bnd = "----PalaBoundary";
  String pre = "--" + bnd + "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\nwhisper-1\r\n"
               "--" + bnd + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"note.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
  String post = "\r\n--" + bnd + "--\r\n";
  size_t totalLen = pre.length() + fileSize + post.length();

  WiFiClientSecure client;
  client.setCACertBundle(x509_crt_bundle_start,
                         (size_t)(x509_crt_bundle_end - x509_crt_bundle_start));
  client.setHandshakeTimeout(15);  // seconds

  if (!client.connect("api.openai.com", 443, 15000 /* ms */)) { f.close(); return false; }

  client.printf("POST /v1/audio/transcriptions HTTP/1.1\r\n"
                "Host: api.openai.com\r\n"
                "Authorization: Bearer %s\r\n"
                "Content-Type: multipart/form-data; boundary=%s\r\n"
                "Content-Length: %u\r\n"
                "Connection: close\r\n\r\n",
                oaiKey.c_str(), bnd.c_str(), (unsigned)totalLen);
  client.print(pre);

  uint8_t* chunk = (uint8_t*)heap_caps_malloc(4096, MALLOC_CAP_8BIT);
  if (!chunk) { f.close(); client.stop(); return false; }
  while (f.available()) {
    int n = f.read(chunk, 4096);
    if (n <= 0) break;
    client.write(chunk, n);
  }
  heap_caps_free(chunk);
  f.close();
  client.print(post);

  uint32_t deadline = millis() + 90000;
  while (!client.available() && millis() < deadline) delay(20);

  String resp = "";
  bool inBody = false;
  while (client.available() || (client.connected() && millis() < deadline)) {
    if (!client.available()) { delay(10); continue; }
    String line = client.readStringUntil('\n');
    if (!inBody) {
      if (line == "\r" || line == "") inBody = true;
      if (line.startsWith("HTTP/") && line.indexOf(" 200 ") < 0) {
        Serial.printf("[Whisper] %s\n", line.c_str());
        client.stop(); return false;
      }
    } else {
      resp += line;
      if (resp.length() > 131072) break;   // safety bound on body size
    }
  }
  client.stop();

  // Robust JSON parse of {"text":"..."} — handles \uXXXX, escapes, and long
  // transcripts that the old hand-rolled scanner would corrupt or truncate.
  JsonDocument doc;
  DeserializationError jerr = deserializeJson(doc, resp);
  if (jerr) { Serial.printf("[Whisper] json: %s\n", jerr.c_str()); return false; }
  String text = doc["text"] | "";
  if (text.length() == 0) { Serial.println("[Whisper] empty response"); return false; }

  String tp = wavPath; tp.replace(".wav", ".txt");
  File tf = SD_MMC.open(tp.c_str(), FILE_WRITE);
  if (tf) { tf.print(text); tf.close(); }

  updateIndexHasText(noteNum);
  return true;
}

bool transcribe(const String& wavPath, int noteNum) {
  for (int attempt = 0; attempt < 3; attempt++) {
    if (WiFi.status() != WL_CONNECTED) return false;   // offline: keep note queued, don't burn retries
    if (transcribeOnce(wavPath, noteNum)) return true;
    if (attempt < 2) { Serial.printf("[Whisper] retry %d/2\n", attempt + 1); delay(3000); }
  }
  return false;
}

// Offline-first queue: notes with hasText==false are the pending work. This drains
// them while online; any that fail (no Wi-Fi, API error) simply stay pending and
// their WAV is preserved for the next sync. Nothing is ever lost on failure.
void transcribeAll() {
  if (!cfg::hasOpenAiKey()) { Serial.println("[Whisper] no API key; skipping sync"); return; }

  int pending = 0;
  for (int i=0; i<(int)noteIndex.size(); i++) if(!noteIndex[i].hasText) pending++;
  int done = 0;
  for (int i=0; i<(int)noteIndex.size(); i++) {
    if (noteIndex[i].hasText) continue;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.printf("[Whisper] wifi lost; %d note(s) stay pending\n", pending - done);
      break;
    }
    showTranscribing(done, pending);
    char wp[64]; snprintf(wp, sizeof(wp), "%s/note_%03d.wav", NOTES_DIR, noteIndex[i].num);
    if (transcribe(String(wp), noteIndex[i].num)) done++;
  }
  Serial.printf("[Whisper] synced %d/%d pending\n", done, pending);
}

// ─── Portal helpers ────────────────────────────────────────────────────────

String htmlEscape(const String& s) {
  String out = s;
  out.replace("&", "&amp;"); out.replace("<", "&lt;");
  out.replace(">", "&gt;"); out.replace("\"", "&quot;");
  return out;
}

String readSmallFile(const char* path, size_t maxLen) {
  File f = SD_MMC.open(path);
  if (!f) return "";
  String out;
  while (f.available() && out.length() < maxLen) out += (char)f.read();
  f.close();
  return out;
}

String urlDecodeSimple(String s) {
  s.replace("+", " ");
  String out = "";
  for (int i = 0; i < (int)s.length(); i++) {
    if (s[i] == '%' && i + 2 < (int)s.length()) {
      String hex = s.substring(i + 1, i + 3);
      out += (char)strtol(hex.c_str(), nullptr, 16);
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

String portalCss() {
  return String(
    "<style>"
    ":root{font-family:-apple-system,BlinkMacSystemFont,'Inter','Segoe UI',sans-serif;color:#111;background:#f3f0e9;}"
    "body{margin:0;padding:24px;background:#f3f0e9;}"
    ".wrap{max-width:780px;margin:0 auto;}"
    ".top{display:flex;align-items:flex-end;justify-content:space-between;gap:16px;margin-bottom:24px;}"
    "h1{font-size:44px;letter-spacing:-.06em;line-height:.9;margin:0;font-weight:800;}"
    ".sub{font-size:13px;text-transform:uppercase;letter-spacing:.12em;color:#6a665f;margin-top:10px;}"
    ".pill{display:inline-flex;border:1px solid #111;border-radius:999px;padding:8px 12px;font-size:13px;background:#fffaf1;}"
    ".grid{display:grid;grid-template-columns:1fr;gap:14px;}"
    ".card{background:#fffaf1;border:1.5px solid #111;border-radius:24px;padding:18px;box-shadow:4px 4px 0 #111;}"
    ".row{display:flex;justify-content:space-between;gap:16px;align-items:flex-start;}"
    ".num{font-size:13px;letter-spacing:.08em;text-transform:uppercase;color:#6a665f;margin-bottom:8px;}"
    ".date{font-size:13px;color:#6a665f;margin:-4px 0 12px;}"
    ".title{font-size:24px;line-height:1.05;letter-spacing:-.04em;font-weight:750;margin:0 0 12px;}"
    ".text{font-size:15px;line-height:1.45;color:#222;margin:0 0 14px;white-space:pre-wrap;}"
    ".md-preview{margin:12px 0;padding:12px;border:1px solid #d7d1c6;border-radius:12px;background:#f8f5ee;overflow:auto;}"
    ".md-preview summary{cursor:pointer;font-size:13px;font-weight:650;}"
    ".md-preview pre{font:13px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace;white-space:pre-wrap;overflow-wrap:anywhere;margin:10px 0 0;}"
    ".actions{display:flex;flex-wrap:wrap;gap:8px;margin-top:14px;}"
    "a.btn{color:#111;text-decoration:none;border:1px solid #111;border-radius:999px;padding:8px 12px;background:#f3f0e9;font-size:13px;}"
    "a.btn.primary{background:#111;color:#fff;}"
    "button.btn{font:inherit;cursor:pointer;}"
    ".btn.danger{color:#c0392b;border-color:#c0392b;background:#fff5f3;}"
    ".empty{border:1.5px dashed #111;border-radius:24px;padding:34px;text-align:center;color:#6a665f;}"
    "audio{width:100%;margin-top:8px;}"
    "@media(max-width:520px){body{padding:16px}h1{font-size:36px}.card{border-radius:20px}.title{font-size:21px}}"
    "</style>"
  );
}

// ─── Portal handlers ───────────────────────────────────────────────────────

void handlePortalRoot() {
  loadIndex();

  Serial.println("[HTTP] GET /");

  // Stream the page in bounded chunks so RAM use stays flat regardless of how
  // many notes exist (a single accumulated String would grow unboundedly).
  transferServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  transferServer.send(200, "text/html", "");

  String html = "<!doctype html><html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>snafu Portal</title>" + portalCss() + "</head><body><div class='wrap'>";

  html += "<div class='top'><div><h1>snafu<br>portal</h1>"
          "<div class='sub'>local note transfer · <a href=\"/provision\" style=\"color:inherit\">setup</a> · <a href=\"/ota\" style=\"color:inherit\">update</a></div></div>"
          "<div class='pill'>" + String((int)noteIndex.size()) + " notes</div></div>";

  html += "<div class='actions' style='margin-bottom:24px'>";
  html += "<a class='btn primary' href='/export.txt'>Download all TXT</a>";
  html += "<a class='btn' href='/export.md'>Download all MD</a>";
  html += "<form action='/notes/erase-all' method='post' style='display:inline;margin:0' "
          "onsubmit=\"return confirm('ERASE ALL NOTES?\\n\\nThis permanently deletes every WAV, TXT and MD file and all note index entries.\\n\\nDevice and Wi-Fi settings will remain unchanged.');\">";
  html += "<button class='btn danger' type='submit'>Erase all</button></form>";
  html += "</div>";

  int visibleCount = (int)noteIndex.size();

  if (visibleCount <= 0) {
    html += "<div class='empty'>No notes yet.</div>";
  } else {
    html += "<div class='grid'>";
    for (int v = 0; v < (int)noteIndex.size(); v++) {
      int i = (int)noteIndex.size() - 1 - v;
      int num = noteIndex[i].num;

      char txtPath[64], wavPath[64], mdPath[64];
      snprintf(txtPath, sizeof(txtPath), "%s/note_%03d.txt", NOTES_DIR, num);
      snprintf(wavPath, sizeof(wavPath), "%s/note_%03d.wav", NOTES_DIR, num);
      snprintf(mdPath, sizeof(mdPath), "%s/note_%03d.md", NOTES_DIR, num);

      String transcript = readSmallFile(txtPath, 1200);
      bool hasMarkdown = SD_MMC.exists(mdPath);
      String markdown = hasMarkdown ? readSmallFile(mdPath, 3000) : "";
      if (transcript.length() == 0)
        transcript = noteIndex[i].hasText ? "(empty transcript)" : "Not transcribed yet.";

      String title = transcript; title.replace("\n", " "); title.trim();
      if (title.length() > 58) title = title.substring(0, 58) + "...";
      if (title.length() == 0 || title == "Not transcribed yet.")
        title = String("Voice note ") + String(num);

      html += "<div class='card'>";
      html += "<div class='row'><div><div class='num'>#" + String(num) + "</div>";
      html += "<h2 class='title'>" + htmlEscape(title) + "</h2>";
      String createdUtc = noteCreatedUtc(num);
      if (createdUtc.length() > 0)
        html += "<div class='date' data-utc='" + createdUtc + "'>" + createdUtc + "</div>";
      else
        html += "<div class='date'>time not set</div>";
      html += "</div>";
      html += "</div>";
      html += "<p class='text'>" + htmlEscape(transcript) + "</p>";
      if (hasMarkdown) {
        html += "<details class='md-preview'><summary>Markdown preview</summary><pre>";
        html += htmlEscape(markdown);
        if (markdown.length() >= 3000) html += "\n\n[Preview truncated]";
        html += "</pre></details>";
      } else {
        html += "<p class='date'>Markdown not generated yet.</p>";
      }
      if (SD_MMC.exists(wavPath))
        html += "<audio controls src='/audio?num=" + String(num) + "'></audio>";
      html += "<div class='actions'>";
      html += "<a class='btn primary' href='/txt?num=" + String(num) + "'>Download TXT</a>";
      if (hasMarkdown) {
        html += "<a class='btn' href='/md?num=" + String(num) + "'>View MD</a>";
        html += "<a class='btn' href='/md-download?num=" + String(num) + "'>Download MD</a>";
      }
      if (SD_MMC.exists(wavPath))
        html += "<a class='btn' href='/wav?num=" + String(num) + "'>Download WAV</a>";
      html += "<a class='btn' style='margin-left:auto;color:#c0392b;border-color:#c0392b' "
              "href='/note/delete?num=" + String(num) + "' "
              "onclick=\"return confirm('Delete note #" + String(num) + "? This cannot be undone.')\">Delete</a>";
      html += "</div></div>";
      if (html.length() > 2048) { transferServer.sendContent(html); html = ""; }
    }
    html += "</div>";
  }

  html += "<script>"
          "document.querySelectorAll('[data-utc]').forEach(function(el){"
          "var d=new Date(el.dataset.utc);"
          "if(!isNaN(d)){el.textContent=d.toLocaleString([],{year:'numeric',month:'short',day:'2-digit',hour:'2-digit',minute:'2-digit'});}"
          "});"
          "</script>";
  html += "</div></body></html>";
  transferServer.sendContent(html);
  transferServer.sendContent("");   // terminate chunked response
}

void handlePortalJson() {
  loadIndex();
  String json = "[";
  for (int v = 0; v < (int)noteIndex.size(); v++) {
    int i = (int)noteIndex.size() - 1 - v;
    if (v > 0) json += ",";
    json += "{";
    json += "\"num\":" + String(noteIndex[i].num) + ",";
    json += "\"hasText\":" + String(noteIndex[i].hasText ? "true" : "false") + ",";
    char mdPath[64]; snprintf(mdPath, sizeof(mdPath), "%s/note_%03d.md", NOTES_DIR, noteIndex[i].num);
    json += "\"hasMarkdown\":" + String(SD_MMC.exists(mdPath) ? "true" : "false");
    json += "}";
  }
  json += "]";
  transferServer.send(200, "application/json", json);
}

void handleExportTxt() {
  loadIndex();

  String filename = "snafu_notes_export.txt";

  // Stream chunked so the full export never has to fit in RAM at once (the old
  // path capped at 55 KB and truncated). No cap now — all notes are exported.
  transferServer.sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  transferServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  transferServer.send(200, "text/plain", "");

  String chunk = String(DEVICE_NAME_NOTE) + " Export\n------------------------------\n\n";

  for (int v = 0; v < (int)noteIndex.size(); v++) {
    int i = (int)noteIndex.size() - 1 - v;
    int num = noteIndex[i].num;
    char txtPath[64]; snprintf(txtPath, sizeof(txtPath), "%s/note_%03d.txt", NOTES_DIR, num);
    String transcript = readSmallFile(txtPath, 4000);
    if (transcript.length() == 0)
      transcript = noteIndex[i].hasText ? "(empty transcript)" : "Not transcribed yet.";
    chunk += "#";
    if (num < 100) chunk += "0";
    if (num < 10)  chunk += "0";
    chunk += String(num) + "\n";
    String createdUtc = noteCreatedUtc(num);
    if (createdUtc.length() > 0) chunk += createdUtc + "\n";
    chunk += "\n" + transcript + "\n\n------------------------------\n\n";
    if (chunk.length() > 2048) { transferServer.sendContent(chunk); chunk = ""; }
  }

  transferServer.sendContent(chunk);
  transferServer.sendContent("");   // terminate chunked response
}

void handleExportMd() {
  loadIndex();

  int available = 0;
  for (int i = 0; i < (int)noteIndex.size(); i++) {
    char path[64];
    snprintf(path, sizeof(path), "%s/note_%03d.md", NOTES_DIR, noteIndex[i].num);
    if (SD_MMC.exists(path)) available++;
  }
  if (available == 0) {
    transferServer.send(404, "text/plain", "No Markdown files available");
    return;
  }

  transferServer.sendHeader("Content-Disposition", "attachment; filename=\"snafu_notes_export.md\"");
  transferServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  transferServer.send(200, "text/markdown; charset=utf-8", "");

  String chunk = String(DEVICE_NAME_NOTE) + " Markdown Export\n==============================\n\n";
  bool firstFile = true;
  for (int v = 0; v < (int)noteIndex.size(); v++) {
    int i = (int)noteIndex.size() - 1 - v;
    int num = noteIndex[i].num;
    char path[64];
    snprintf(path, sizeof(path), "%s/note_%03d.md", NOTES_DIR, num);
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) continue;

    if (!firstFile) chunk += "\n\n---\n\n";
    firstFile = false;
    chunk += "<!-- Note #" + String(num) + " -->\n\n";
    transferServer.sendContent(chunk);
    chunk = "";

    char buffer[513];
    while (f.available()) {
      size_t n = f.readBytes(buffer, 512);
      if (n == 0) break;
      buffer[n] = '\0';
      transferServer.sendContent(String(buffer));
    }
    f.close();
  }
  transferServer.sendContent("\n");
  transferServer.sendContent("");
}

void sendFileByNum(const char* ext, const char* mime, bool attachment) {
  if (!transferServer.hasArg("num")) { transferServer.send(400, "text/plain", "Missing num"); return; }
  int num = transferServer.arg("num").toInt();
  if (num <= 0) { transferServer.send(400, "text/plain", "Invalid num"); return; }
  char path[64]; snprintf(path, sizeof(path), "%s/note_%03d.%s", NOTES_DIR, num, ext);
  File f = SD_MMC.open(path);
  if (!f) { transferServer.send(404, "text/plain", "File not found"); return; }
  if (attachment) {
    String filename = String("note_") + String(num) + "." + String(ext);
    transferServer.sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  }
  transferServer.streamFile(f, mime);
  f.close();
}

void handleNoteDelete() {
  if (!transferServer.hasArg("num")) { transferServer.send(400, "text/plain", "Missing num"); return; }
  int num = transferServer.arg("num").toInt();
  if (num <= 0) { transferServer.send(400, "text/plain", "Invalid num"); return; }
  deleteNote(num);
  transferServer.sendHeader("Location", "/");
  transferServer.send(303);
}

void handleEraseAllNotes() {
  int erased = deleteAllNotes();
  Serial.printf("[HTTP] erase all notes requested; indexed notes removed=%d\n", erased);
  transferServer.sendHeader("Location", "/");
  transferServer.send(303);
}

void handleProvisionPage() {
  Serial.println("[HTTP] GET /provision");
  String html = "<!doctype html><html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>snafu Setup</title>" + portalCss() + "</head><body><div class='wrap'>";
  html += "<div class='top'><div><h1>snafu<br>setup</h1>"
          "<div class='sub'>device provisioning</div></div></div>";
  html += "<div class='card'>";
  html += "<p class='hint'>Wi-Fi: " + String(cfg::hasWifi() ? "configured" : "not set") +
          " &middot; OpenAI key: " + String(cfg::hasOpenAiKey() ? "configured" : "not set") +
          "</p>";
  html += "<form action='/provision/save' method='post'>";
  html += "<p><input name='ssid' placeholder='Wi-Fi network (SSID)'></p>";
  html += "<p><input name='pass' type='password' placeholder='Wi-Fi password'></p>";
  html += "<p><input name='openai' type='password' placeholder='OpenAI API key (sk-...)'></p>";
  html += "<p class='hint'>Leave a text field blank to keep its current value.</p>";
  html += "<button type='submit'>Save</button></form></div>";
  html += "<a class='btn' href='/'>Back to notes</a>";
  html += "</div></body></html>";
  transferServer.send(200, "text/html", html);
}

void handleProvisionSave() {
  String ssid = transferServer.hasArg("ssid")   ? transferServer.arg("ssid")   : "";
  String pass = transferServer.hasArg("pass")   ? transferServer.arg("pass")   : "";
  String key  = transferServer.hasArg("openai") ? transferServer.arg("openai") : "";
  ssid.trim(); key.trim();
  bool changed = false;
  if (ssid.length() > 0) { cfg::setWifi(ssid, pass); changed = true; }
  if (key.length()  > 0) { cfg::setOpenAiKey(key);   changed = true; }


  String html = "<!doctype html><html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>snafu Setup</title>" + portalCss() + "</head><body><div class='wrap'>";
  html += "<div class='card'><h1>" + String(changed ? "saved" : "no change") + "</h1>";
  html += "<p class='hint'>" + String(changed
            ? "Settings stored to the device. Re-open Transfer or Sync to use them."
            : "Nothing was submitted.") + "</p>";
  html += "<a class='btn' href='/provision'>Back to setup</a></div></div></body></html>";
  transferServer.send(200, "text/html", html);
}


// ─── ArduinoOTA (PlatformIO upload over Wi-Fi) ───────────────────────────────
static bool arduinoOtaActive = false;

void startArduinoOta() {
  if (WiFi.status() != WL_CONNECTED || arduinoOtaActive) return;

  ArduinoOTA.setHostname("snafuNote");
  ArduinoOTA.onStart([]() {
    Serial.println("[ArduinoOTA] update started");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\n[ArduinoOTA] update finished; rebooting");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("[ArduinoOTA] progress: %u%%\r",
                  (progress * 100U) / total);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("\n[ArduinoOTA] error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("authentication failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("begin failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("connection failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("receive failed");
    else if (error == OTA_END_ERROR) Serial.println("end failed");
  });

  ArduinoOTA.begin();
  arduinoOtaActive = true;
  Serial.printf("[ArduinoOTA] ready at %s.local\n", "snafuNote");
}

void handleArduinoOta() {
  if (arduinoOtaActive) ArduinoOTA.handle();
}

void stopArduinoOta() {
  if (!arduinoOtaActive) return;
  ArduinoOTA.end();
  arduinoOtaActive = false;
  Serial.println("[ArduinoOTA] stopped");
}

// ─── Local firmware upload OTA ──────────────────────────────────────────────
static bool otaUploadStarted = false;
static bool otaUploadOk = false;
static String otaUploadError;

static void handleOtaUpload() {
  HTTPUpload& upload = transferServer.upload();

  if (upload.status == UPLOAD_FILE_START) {
    otaUploadStarted = false;
    otaUploadOk = false;
    otaUploadError = "";

    String filename = upload.filename;
    filename.toLowerCase();
    if (!filename.endsWith(".bin")) {
      otaUploadError = "Please select a compiled .bin firmware file.";
      Serial.println("[OTA] rejected upload: file is not .bin");
      return;
    }

    Serial.printf("[OTA] upload started: %s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      otaUploadError = String("Could not start flash update: ") + Update.errorString();
      Serial.printf("[OTA] begin failed: %s\n", Update.errorString());
      return;
    }
    otaUploadStarted = true;
  }
  else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!otaUploadStarted || otaUploadError.length() > 0) return;

    size_t written = Update.write(upload.buf, upload.currentSize);
    if (written != upload.currentSize) {
      otaUploadError = String("Flash write failed: ") + Update.errorString();
      Serial.printf("[OTA] write failed: %s\n", Update.errorString());
      Update.abort();
      otaUploadStarted = false;
    }
  }
  else if (upload.status == UPLOAD_FILE_END) {
    if (!otaUploadStarted || otaUploadError.length() > 0) return;

    if (Update.end(true)) {
      otaUploadOk = true;
      Serial.printf("[OTA] upload complete: %u bytes\n", upload.totalSize);
    } else {
      otaUploadError = String("Firmware validation failed: ") + Update.errorString();
      Serial.printf("[OTA] end failed: %s\n", Update.errorString());
    }
    otaUploadStarted = false;
  }
  else if (upload.status == UPLOAD_FILE_ABORTED) {
    otaUploadError = "Upload was interrupted.";
    Serial.println("[OTA] upload aborted");
    if (otaUploadStarted) Update.abort();
    otaUploadStarted = false;
  }
}

static void handleOtaUploadResult() {
  if (!otaUploadOk || otaUploadError.length() > 0) {
    String message = otaUploadError.length() > 0
                       ? otaUploadError
                       : "No valid firmware image was received.";
    transferServer.send(400, "text/html",
      "<!doctype html><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<h1>Update failed</h1><p>" + htmlEscape(message) +
      "</p><p>The update did not complete. Return to the portal and try again with a valid .bin file.</p>"
      "<p><a href='/ota'>Back</a></p>");
    otaUploadOk = false;
    return;
  }

  transferServer.send(200, "text/html",
    "<!doctype html><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<h1>Update successful</h1><p>Firmware written and validated. The device is restarting now.</p>");
  Serial.println("[OTA] update successful; restarting");
  delay(700);
  ESP.restart();
}

void handleOtaPage() {
  String html = "<!doctype html><html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>snafu OTA</title>" + portalCss() + "</head><body><div class='wrap'>";
  html += "<div class='top'><div><h1>snafu<br>update</h1>"
          "<div class='sub'>firmware " FW_VERSION "</div></div></div>";
  html += "<div class='card'>";
  html += "<p class='hint'>Select the firmware .bin file built by PlatformIO. "
          "The device writes it to the OTA application slot and restarts when validation succeeds. "
          "Keep the device powered during the update.</p>";
  html += "<form action='/ota/upload' method='post' enctype='multipart/form-data'>"
          "<p><input type='file' name='firmware' accept='.bin,application/octet-stream' required></p>"
          "<button type='submit'>Upload firmware</button></form></div>";
  html += "<a class='btn' href='/'>Back to notes</a>";
  html += "</div></body></html>";
  transferServer.send(200, "text/html", html);
}

void setupTransferServer() {
  transferServer.on("/", HTTP_GET, handlePortalRoot);
  transferServer.on("/provision", HTTP_GET, handleProvisionPage);
  transferServer.on("/provision/save", HTTP_POST, handleProvisionSave);
  transferServer.on("/ota", HTTP_GET, handleOtaPage);
  transferServer.on("/ota/upload", HTTP_POST, handleOtaUploadResult, handleOtaUpload);
  transferServer.on("/note/delete", HTTP_GET, handleNoteDelete);
  transferServer.on("/notes/erase-all", HTTP_POST, handleEraseAllNotes);
  transferServer.on("/api/notes", HTTP_GET, handlePortalJson);
  transferServer.on("/export.txt", HTTP_GET, handleExportTxt);
  transferServer.on("/export.md", HTTP_GET, handleExportMd);
  transferServer.on("/txt",   HTTP_GET, [](){ sendFileByNum("txt", "text/plain", true); });
  transferServer.on("/md",    HTTP_GET, [](){ sendFileByNum("md", "text/plain; charset=utf-8", false); });
  transferServer.on("/md-download", HTTP_GET, [](){ sendFileByNum("md", "text/markdown; charset=utf-8", true); });
  transferServer.on("/wav",   HTTP_GET, [](){ sendFileByNum("wav", "audio/wav",  true); });
  transferServer.on("/audio", HTTP_GET, [](){ sendFileByNum("wav", "audio/wav",  false); });
  transferServer.onNotFound([](){
    Serial.printf("[HTTP] miss: %s\n", transferServer.uri().c_str());
    if (captivePortalActive) {
      // Captive portal: bounce any unknown URL (incl. the OS connectivity probe)
      // to the setup page so it opens automatically.
      transferServer.sendHeader("Location", "http://" + transferUrl + "/provision", true);
      transferServer.send(302, "text/plain", "");
    } else {
      transferServer.send(404, "text/plain", "Not found");
    }
  });
}

void stopTransferMode() {
  stopArduinoOta();
  if (transferServerActive) {
    transferServer.stop();
    transferServerActive = false;
  }
  if (captivePortalActive) {
    dnsServer.stop();
    captivePortalActive = false;
  }
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  transferUrl = "";
}
