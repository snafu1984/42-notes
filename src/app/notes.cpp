#include "Arduino.h"
#include "config.h"
#include "globals.h"
#include "types.h"
#include "notes.h"
#include "rtc.h"
#include "draw.h"
#include "SD_MMC.h"

// ─── Local note index ──────────────────────────────────────────────────────
// Reads both legacy rows (number,tag,hasText) and current rows (number,hasText).
void loadIndex() {
  noteIndex.clear();
  // These files belonged to the retired tag and remote-vault workflow.
  if (SD_MMC.exists("/notes/tags.txt")) SD_MMC.remove("/notes/tags.txt");
  if (SD_MMC.exists("/notes/tombs.csv")) SD_MMC.remove("/notes/tombs.csv");
  File f = SD_MMC.open(INDEX_FILE);
  if (!f) return;
  while (f.available()) {
    String ln = f.readStringUntil('\n'); ln.trim();
    if (!ln.length()) continue;
    int c1 = ln.indexOf(',');
    if (c1 < 0) continue;
    int c2 = ln.indexOf(',', c1 + 1);
    NoteEntry e;
    e.num = ln.substring(0, c1).toInt();
    e.hasText = (c2 >= 0 ? ln.substring(c2 + 1) : ln.substring(c1 + 1)).toInt() == 1;
    if (e.num > 0) noteIndex.push_back(e);
  }
  f.close();
}

void saveIndex() {
  const char* tmp = "/notes/index.tmp";
  if (SD_MMC.exists(tmp)) SD_MMC.remove(tmp);
  File f = SD_MMC.open(tmp, FILE_WRITE);
  if (!f) return;
  for (int i = 0; i < (int)noteIndex.size(); i++)
    f.printf("%d,%d\n", noteIndex[i].num, noteIndex[i].hasText ? 1 : 0);
  f.close();
  if (SD_MMC.exists(INDEX_FILE)) SD_MMC.remove(INDEX_FILE);
  SD_MMC.rename(tmp, INDEX_FILE);
}

void addToIndex(int num, bool hasText) {
  for (int i = 0; i < (int)noteIndex.size(); i++) {
    if (noteIndex[i].num == num) {
      noteIndex[i].hasText = hasText;
      saveIndex();
      writeNoteMeta(num);
      return;
    }
  }
  NoteEntry e;
  e.num = num;
  e.hasText = hasText;
  noteIndex.push_back(e);
  saveIndex();
  writeNoteMeta(num);
}

void updateIndexHasText(int num) {
  for (int i = 0; i < (int)noteIndex.size(); i++) {
    if (noteIndex[i].num == num) {
      noteIndex[i].hasText = true;
      break;
    }
  }
  saveIndex();
  writeNoteMeta(num);
}

void deleteNote(int num) {
  const char* exts[] = {"wav", "txt", "md", "meta", nullptr};
  char path[64];
  for (int e = 0; exts[e]; e++) {
    snprintf(path, sizeof(path), "%s/note_%03d.%s", NOTES_DIR, num, exts[e]);
    if (SD_MMC.exists(path)) SD_MMC.remove(path);
  }
  for (int i = 0; i < (int)noteIndex.size(); i++) {
    if (noteIndex[i].num == num) {
      noteIndex.erase(noteIndex.begin() + i);
      break;
    }
  }
  saveIndex();
}

int deleteAllNotes() {
  int erased = (int)noteIndex.size();
  std::vector<String> toRemove;
  File dir = SD_MMC.open(NOTES_DIR);
  if (dir && dir.isDirectory()) {
    for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
      String name = e.name();
      e.close();
      int slash = name.lastIndexOf('/');
      String base = slash >= 0 ? name.substring(slash + 1) : name;
      if (base.startsWith("note_") || base == "index.csv")
        toRemove.push_back(String(NOTES_DIR) + "/" + base);
    }
    dir.close();
  }
  for (size_t i = 0; i < toRemove.size(); i++) SD_MMC.remove(toRemove[i].c_str());
  if (SD_MMC.exists(INDEX_FILE)) SD_MMC.remove(INDEX_FILE);
  noteIndex.clear();
  return erased;
}

int nextNoteNumber() {
  int maxNum = 0;
  for (int i=0; i<(int)noteIndex.size(); i++) if (noteIndex[i].num > maxNum) maxNum = noteIndex[i].num;
  return maxNum + 1;
}

// ─── Meta ─────────────────────────────────────────────────────────────────

String noteMetaPath(int num) {
  char path[64];
  snprintf(path, sizeof(path), "%s/note_%03d.meta", NOTES_DIR, num);
  return String(path);
}

String readNoteMetaValue(int num, const char* key) {
  String path = noteMetaPath(num);
  File f = SD_MMC.open(path.c_str());
  if (!f) return "";
  String prefix = String(key) + "=";
  while (f.available()) {
    String line = f.readStringUntil('\n'); line.trim();
    if (line.startsWith(prefix)) { f.close(); return line.substring(prefix.length()); }
  }
  f.close();
  return "";
}

// Keep only the creation timestamp and transcription state in local metadata.
void writeNoteMeta(int num) {
  String created = readNoteMetaValue(num, "created_utc");
  if (!created.length()) created = currentUtcIso();
  bool hasText = false;
  for (int i = 0; i < (int)noteIndex.size(); i++)
    if (noteIndex[i].num == num) { hasText = noteIndex[i].hasText; break; }

  File f = SD_MMC.open(noteMetaPath(num).c_str(), FILE_WRITE);
  if (!f) return;
  f.print("created_utc="); f.println(created);
  f.print("synced="); f.println(hasText ? "1" : "0");
  f.close();
}

// ─── Time helpers ─────────────────────────────────────────────────────────

String noteCreatedUtc(int num) {
  return readNoteMetaValue(num, "created_utc");
}

String utcToLocalDeviceLabel(const String& utcIso) {
  if (utcIso.length() < 16) return "time not set";
  struct tm utc;
  memset(&utc, 0, sizeof(struct tm));
  utc.tm_year = utcIso.substring(0, 4).toInt() - 1900;
  utc.tm_mon  = utcIso.substring(5, 7).toInt() - 1;
  utc.tm_mday = utcIso.substring(8, 10).toInt();
  utc.tm_hour = utcIso.substring(11, 13).toInt();
  utc.tm_min  = utcIso.substring(14, 16).toInt();
  utc.tm_sec  = 0;
  time_t epoch = utcTmToEpoch(utc);
  epoch += (LOCAL_TIME_OFFSET_MIN * 60);
  struct tm localTm;
  gmtime_r(&epoch, &localTm);
  char buf[22];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &localTm);
  return String(buf);
}

String noteCreatedDeviceLabel(int num) {
  String utc = noteCreatedUtc(num);
  if (utc.length() < 16) return "time not set";
  return utcToLocalDeviceLabel(utc);
}

String currentUtcIso() {
  String fromRtc = rtcUtcIso();
  if (fromRtc.length() > 0) { timeReady = true; return fromRtc; }
  time_t now = time(nullptr);
  if (!timeReady || now < 1700000000) return "";
  struct tm utc;
  gmtime_r(&now, &utc);
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return String(buf);
}

// ─── Ticker + preview ─────────────────────────────────────────────────────

String notePreviewText(int num, size_t maxLen) {
  char txtPath[64];
  snprintf(txtPath, sizeof(txtPath), "%s/note_%03d.txt", NOTES_DIR, num);
  File f = SD_MMC.open(txtPath);
  if (!f) return "";
  String out = "";
  while (f.available() && out.length() < maxLen) {
    char c = (char)f.read();
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    out += c;
  }
  f.close();
  return normalizeForDisplay(out);
}

String noteTickerText(int idx) {
  if (idx < 0) return "";
  String dt      = noteCreatedDeviceLabel(noteIndex[idx].num);
  String preview = notePreviewText(noteIndex[idx].num, 130);
  if (preview.length() == 0)
    preview = noteIndex[idx].hasText ? "empty note" : "not synced";
  String ticker = dt;
  if (ticker == "time not set") ticker = preview;
  else { ticker += "  -  "; ticker += preview; }
  return normalizeForDisplay(ticker);
}

bool activeTickerNeedsScroll(int cursor) {
  int count = filteredCount();
  if (count <= 0) return false;
  int idx = noteAtFilteredIndex(cursor);
  if (idx < 0) return false;
  String ticker = noteTickerText(idx);
  return textW(ticker.c_str(), 1) > 145;
}

void drawTickerText(int x, int y, int maxW, const String& rawText, bool active, uint8_t color) {
  String text = normalizeForDisplay(rawText);
  if (textW(text.c_str(), 1) <= maxW || !active) {
    drawStrFit(x, y, maxW, text.c_str(), 1, color);
    return;
  }
  String spacer = "     ";
  String loopText = text + spacer + text;
  int cycleLen = text.length() + spacer.length();
  int start = tickerOffset % max(1, cycleLen);
  String window = "";
  for (int i = start; i < (int)loopText.length(); i++) {
    String candidate = window + loopText[i];
    if (textW(candidate.c_str(), 1) > maxW) break;
    window = candidate;
  }
  if (window.length() == 0) drawStrFit(x, y, maxW, text.c_str(), 1, color);
  else                      drawStr(x, y, window.c_str(), 1, color);
}

// ─── Filtering ────────────────────────────────────────────────────────────

int filteredCount() {
  return (int)noteIndex.size();
}

int noteAtFilteredIndex(int visIdx) {
  int rev = (int)noteIndex.size() - 1 - visIdx;
  if (rev < 0 || rev >= (int)noteIndex.size()) return -1;
  return rev;
}
