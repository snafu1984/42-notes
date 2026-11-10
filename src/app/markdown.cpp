#include "Arduino.h"
#include "markdown.h"
#include "config.h"
#include "globals.h"
#include "config_store.h"
#include "notes.h"
#include "WiFi.h"
#include "WiFiClientSecure.h"
#include "SD_MMC.h"
#include <ArduinoJson.h>
#include <vector>
#include <stdlib.h>

extern const uint8_t x509_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509_crt_bundle_end[]   asm("_binary_x509_crt_bundle_end");

static String dechunkBody(const String& in) {
  String out; int i = 0, n = in.length();
  while (i < n) {
    int eol = in.indexOf('\n', i);
    if (eol < 0) break;
    String sizeLine = in.substring(i, eol);
    int semi = sizeLine.indexOf(';');                 // ignore chunk extensions
    if (semi >= 0) sizeLine = sizeLine.substring(0, semi);
    sizeLine.trim();
    long sz = strtol(sizeLine.c_str(), nullptr, 16);
    i = eol + 1;
    if (sz <= 0) break;                               // terminating 0-chunk
    if (i + sz > n) sz = n - i;                        // guard against truncation
    out += in.substring(i, i + sz);
    i += sz;
    while (i < n && (in[i] == '\r' || in[i] == '\n')) i++;  // skip CRLF after data
  }
  return out;
}

// ── Generic HTTPS request (reuses the transcribeOnce pattern) ───────────────
static bool httpsSend(const char* host, const String& method, const String& path,
                      const std::vector<String>& headers, const String& body,
                      int& outStatus, String& outBody) {
  WiFiClientSecure client;
  client.setCACertBundle(x509_crt_bundle_start,
                         (size_t)(x509_crt_bundle_end - x509_crt_bundle_start));
  client.setHandshakeTimeout(15);
  Serial.printf("[HTTPS] %s %s connect... body=%u bytes\n",
                method.c_str(), host, (unsigned)body.length());
  if (!client.connect(host, 443, 15000)) {
    Serial.printf("[HTTPS] %s connect FAILED\n", host);
    return false;
  }

  String req = method + " " + path + " HTTP/1.1\r\n";
  req += "Host: " + String(host) + "\r\n";
  for (size_t i = 0; i < headers.size(); i++) req += headers[i] + "\r\n";
  req += "Content-Length: " + String(body.length()) + "\r\n";
  req += "Connection: close\r\n\r\n";
  client.print(req);
  if (body.length()) client.print(body);

  uint32_t deadline = millis() + 30000;
  while (!client.available() && millis() < deadline) delay(10);

  outStatus = 0; outBody = "";
  bool inBody = false, statusParsed = false, chunked = false;
  while (client.available() || (client.connected() && millis() < deadline)) {
    if (!client.available()) { delay(5); continue; }
    if (!inBody) {
      String line = client.readStringUntil('\n');
      if (!statusParsed && line.startsWith("HTTP/")) {
        int sp = line.indexOf(' ');
        if (sp > 0) outStatus = line.substring(sp + 1, sp + 4).toInt();
        statusParsed = true;
      }
      String low = line; low.toLowerCase();
      if (low.startsWith("transfer-encoding:") && low.indexOf("chunked") >= 0) chunked = true;
      if (line == "\r" || line == "") inBody = true;
    } else {
      while (client.available() && outBody.length() < 131072) outBody += (char)client.read();
    }
  }
  client.stop();
  if (chunked) outBody = dechunkBody(outBody);        // OpenAI chat replies chunked
  Serial.printf("[HTTPS] %s %s status=%d parsed=%d response=%u bytes chunked=%d\n",
                method.c_str(), host, outStatus, statusParsed,
                (unsigned)outBody.length(), chunked);
  return statusParsed;
}

static bool enrichNote(const String& transcript, const String& nowLocal,
                       String& cleaned) {
  cleaned = "";
  Serial.printf("[Enrich] START transcript=%u bytes wifi=%d\n",
                (unsigned)transcript.length(), WiFi.status());
  String key = cfg::openaiKey();
  if (key.length() == 0) {
    Serial.println("[Enrich] ABORT: OpenAI API key missing");
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Enrich] ABORT: Wi-Fi not connected");
    return false;
  }

  String input = transcript;
  if (input.length() > 6000) input = input.substring(0, 6000);

  JsonDocument reqDoc;
  reqDoc["model"] = "gpt-4o-mini";
  reqDoc["temperature"] = 0;
  reqDoc["response_format"]["type"] = "json_object";
  JsonArray msgs = reqDoc["messages"].to<JsonArray>();
  JsonObject sys = msgs.add<JsonObject>();
  sys["role"] = "system";
  sys["content"] =
    "You convert a German voice-note transcript into clean Markdown. "
    "Return ONLY a JSON object with exactly one field: {\"cleaned\": string}. "
    "Preserve all factual content, names, numbers, dates and intent. Correct obvious "
    "speech-recognition errors, grammar and punctuation. Remove filler words, stutters "
    "and repetition without changing meaning. Do not add information. "
    "Recognize only these exact German formatting commands, case-insensitively, "
    "optionally followed by a period or comma: 'Liste', 'Listenpunkt', 'Aufzählung', "
    "'Aufzählungspunkt', 'Absatz' and 'Überschrift'. Remove command words from output. "
    "'Liste' starts a numbered Markdown list. 'Listenpunkt' starts a new numbered item. "
    "'Aufzählung' starts an unordered Markdown bullet list. 'Aufzählungspunkt' starts "
    "a new bullet item. Text following a list-point command belongs to that item until "
    "the next supported command or end of transcript. 'Absatz' ends the current block "
    "and starts a new paragraph, separated by one blank line. 'Überschrift' starts a "
    "level-2 Markdown heading (##); the following dictated words form the heading until "
    "the next command or end of transcript. Do not invent list structures or headings "
    "without the corresponding spoken command. Preserve the dictated order. "
    "Return the complete note in the cleaned field as valid Markdown, without code fences "
    "or explanation. If the transcript is empty or unintelligible, return {\"cleaned\":\"\"}.";
  JsonObject usr = msgs.add<JsonObject>();
  usr["role"] = "user";
  usr["content"] = "Current local date/time: " + nowLocal + "\n\nTranscript:\n" + input;
  String body; serializeJson(reqDoc, body);
  Serial.printf("[Enrich] request prepared model=gpt-4o-mini input=%u request=%u bytes\n",
                (unsigned)input.length(), (unsigned)body.length());

  std::vector<String> headers = {
    "Authorization: Bearer " + key,
    "Content-Type: application/json"
  };
  int status = 0; String resp;
  if (!httpsSend("api.openai.com", "POST", "/v1/chat/completions", headers, body, status, resp)) {
    Serial.println("[Enrich] FAIL: HTTPS transport/no HTTP status");
    return false;
  }
  if (status != 200) {
    Serial.printf("[Enrich] FAIL: OpenAI HTTP %d response=%u bytes\n",
                  status, (unsigned)resp.length());
    return false;
  }

  JsonDocument outer;
  DeserializationError outerErr = deserializeJson(outer, resp);
  if (outerErr) {
    Serial.printf("[Enrich] FAIL: outer JSON parse: %s\n", outerErr.c_str());
    return false;
  }
  String content = outer["choices"][0]["message"]["content"] | "";
  if (content.length() == 0) {
    Serial.println("[Enrich] FAIL: response has no choices[0].message.content");
    return false;
  }
  Serial.printf("[Enrich] model content=%u bytes\n", (unsigned)content.length());

  JsonDocument inner;
  DeserializationError innerErr = deserializeJson(inner, content);
  if (innerErr) {
    Serial.printf("[Enrich] FAIL: model JSON parse: %s\n", innerErr.c_str());
    return false;
  }
  cleaned = String((const char*)(inner["cleaned"] | ""));
  cleaned.trim();
  Serial.printf("[Enrich] OK cleaned=%u bytes\n", (unsigned)cleaned.length());
  return true;
}

void markdownProcessPending() {
  if (!cfg::hasOpenAiKey()) {
    Serial.println("[Markdown] OpenAI API key missing; local conversion deferred");
    return;
  }

  int pending = 0;
  for (int i = 0; i < (int)noteIndex.size(); i++) {
    if (!noteIndex[i].hasText) continue;
    char mp[64]; snprintf(mp, sizeof(mp), "%s/note_%03d.md", NOTES_DIR, noteIndex[i].num);
    if (!SD_MMC.exists(mp)) pending++;
  }
  Serial.printf("[Markdown] pending local conversions=%d\n", pending);

  int done = 0;
  for (int i = 0; i < (int)noteIndex.size(); i++) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[Markdown] Wi-Fi lost; remaining notes deferred");
      break;
    }
    if (!noteIndex[i].hasText) continue;

    int num = noteIndex[i].num;
    char mp[64], tp[64];
    snprintf(mp, sizeof(mp), "%s/note_%03d.md", NOTES_DIR, num);
    if (SD_MMC.exists(mp)) continue;
    snprintf(tp, sizeof(tp), "%s/note_%03d.txt", NOTES_DIR, num);

    File tf = SD_MMC.open(tp, FILE_READ);
    if (!tf) {
      Serial.printf("[Markdown] note %03d has no transcript file\n", num);
      continue;
    }
    String transcript;
    while (tf.available() && transcript.length() < 131072) transcript += (char)tf.read();
    tf.close();
    if (!transcript.length()) {
      Serial.printf("[Markdown] note %03d transcript empty\n", num);
      continue;
    }

    String cleaned;
    Serial.printf("[Markdown] converting note %03d (%u bytes)\n", num,
                  (unsigned)transcript.length());
    if (!enrichNote(transcript, noteCreatedDeviceLabel(num), cleaned) || !cleaned.length()) {
      Serial.printf("[Markdown] note %03d conversion failed; will retry next sync\n", num);
      continue;
    }

    File mf = SD_MMC.open(mp, FILE_WRITE);
    if (!mf) {
      Serial.printf("[Markdown] note %03d cannot create Markdown file\n", num);
      continue;
    }
    mf.print(cleaned);
    if (!cleaned.endsWith("\n")) mf.print("\n");
    mf.close();
    done++;
    Serial.printf("[Markdown] note %03d saved: %s\n", num, mp);
  }
  Serial.printf("[Markdown] converted %d/%d notes\n", done, pending);
}

