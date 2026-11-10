#pragma once
#include <Arduino.h>

void   loadIndex();
void   saveIndex();
void   addToIndex(int num, bool hasText);
void   updateIndexHasText(int num);
void   deleteNote(int num);
int    deleteAllNotes();
int    nextNoteNumber();

String noteMetaPath(int num);
String readNoteMetaValue(int num, const char* key);
void   writeNoteMeta(int num);
String noteCreatedUtc(int num);
String utcToLocalDeviceLabel(const String& utcIso);
String noteCreatedDeviceLabel(int num);
String currentUtcIso();

String notePreviewText(int num, size_t maxLen = 90);
String noteTickerText(int idx);
bool   activeTickerNeedsScroll(int cursor);
void   drawTickerText(int x, int y, int maxW, const String& rawText, bool active, uint8_t color);

int filteredCount();
int noteAtFilteredIndex(int visIdx);
