#pragma once
#include <Arduino.h>

bool transcribe(const String& wavPath, int noteNum);
void transcribeAll();

String htmlEscape(const String& s);
String readSmallFile(const char* path, size_t maxLen = 1600);
String urlDecodeSimple(String s);
String portalCss();

void handlePortalRoot();
void handlePortalJson();
void handleExportTxt();
void handleExportMd();
void sendFileByNum(const char* ext, const char* mime, bool attachment);
void handleNoteDelete();
void handleEraseAllNotes();

void setupTransferServer();
void startArduinoOta();
void handleArduinoOta();
void stopArduinoOta();
void stopTransferMode();
