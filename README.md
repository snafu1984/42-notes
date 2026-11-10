# 42-note

A compact ESP32-S3 voice-note recorder. Hold the record button to record; release it to save a WAV file to the SD card. During sync, the device sends pending recordings to OpenAI Whisper for transcription and then uses GPT to turn each transcript into a Markdown note.

## Workflow

1. Hold **REC** to record audio.
2. Release **REC** to finish and save `/notes/note_NNN.wav`.
3. Start **Sync** from the device menu.
4. Pending WAV files are transcribed with OpenAI Whisper and saved as `/notes/note_NNN.txt`.
5. Transcripts without a Markdown counterpart are sent to GPT-4o-mini for cleanup and formatting. The result is saved as `/notes/note_NNN.md`.
6. Use the device web interface to preview, open, download, or delete WAV, TXT, and MD files.

The original WAV and Whisper TXT files are retained when Markdown is generated. Markdown conversion is local to the device and does not require GitHub, Obsidian, tags, or a separate vault.

## Spoken formatting commands

The GPT formatting prompt currently recognises these German commands:

- **Liste** — start a numbered list.
- **Listenpunkt** — start a numbered list item.
- **Aufzählung** — start a bulleted list.
- **Aufzählungspunkt** — start a bullet item.
- **Absatz** — start a new paragraph.
- **Überschrift** — start a level-two Markdown heading (`##`).

The spoken command words are omitted from the generated Markdown.

## Configuration

Configure Wi-Fi and an OpenAI API key through the device setup portal, or over USB serial using these line commands:

```text
SSID=<your Wi-Fi SSID>
PASS=<your Wi-Fi password>
KEY=<your OpenAI API key>
SHOW
```

Send `SSID=` before `PASS=`. Credentials are stored in the ESP32 NVS. `RESET` clears stored configuration; it does not erase notes on the SD card.

## Build and upload

The project uses PlatformIO. Build the `esp32s3` environment for USB firmware, or `esp32s3_ota` for OTA firmware:

```bash
pio run -e esp32s3
pio run -e esp32s3 -t upload
```

For OTA:

```bash
pio run -e esp32s3_ota
pio run -e esp32s3_ota -t upload
```

## SD-card layout

```text
/notes/
  index.csv
  note_001.wav
  note_001.txt
  note_001.md
  note_001.meta
```

The note index reader remains compatible with the previous tag-bearing CSV format. Legacy tag fields are ignored; the index is rewritten in the simplified `number,hasText` format when next saved.
