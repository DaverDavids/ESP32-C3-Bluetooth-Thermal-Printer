#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <Update.h>
#include <ArduinoOTA.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <LittleFS.h>
#include <Secrets.h>
#include "html.h"

struct VlwFont;
struct VlwGlyph;
class PrintCanvas;
struct EventConfig;

const char* hostname = "c3printer";
#define TWITCH_CHANNEL "daverdavid"

WebServer server(80);
WiFiClientSecure twitchClient;
Preferences preferences;

static BLEUUID serviceUUID("49535343-fe7d-4ae5-8fa9-9fafd205e455");
static BLEUUID charWriteUUID("49535343-8841-43f4-a8d4-ecbe34729bb3");
static BLEUUID charNotifyUUID("49535343-1e4d-4bd9-ba61-23c647249616");

String printerMAC = "56:17:a1:30:0d:dc";
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteCharacteristic = nullptr;
bool printerConnected = false;
bool twitchConnected  = false;

// twitchEverConnected: set true once Twitch first connects.
// After that, Twitch reconnects independently every 10s.
// bleDeadlineMs: if BLE has not reached BLE_READY by this time,
// Twitch connects anyway so a live stream with no printer still works.
bool twitchEverConnected = false;
unsigned long bleDeadlineMs = 0;  // set in setup() to millis() + 45000

enum BLEConnState { BLE_IDLE, BLE_CONNECTING, BLE_DISCOVERING, BLE_INITING, BLE_READY, BLE_FAILED };
volatile BLEConnState bleState = BLE_IDLE;
unsigned long lastTwitchPing = 0;

String pointsRewardFilter = "";
bool shouldSaveConfig = false;
File uploadFile;
size_t uploadExpectedSize = 0;
size_t uploadWrittenBytes = 0;
bool uploadFailed = false;
bool uploadInProgress = false;
unsigned long lastUploadFinish = 0;

#define LOG_BUF_SIZE 8192
char logBuffer[LOG_BUF_SIZE];
size_t logHead = 0;
bool logWrapped = false;

void logMsg(const char* msg) {
  char line[160];
  int len = snprintf(line, sizeof(line), "[%lu] %s\n", millis(), msg);
  if (len < 0) return;
  if ((size_t)len >= sizeof(line)) len = sizeof(line) - 1;
  for (int i = 0; i < len; i++) {
    logBuffer[logHead] = line[i];
    logHead = (logHead + 1) % LOG_BUF_SIZE;
    if (logHead == 0) logWrapped = true;
  }
  Serial.print(line);
}

void logMsg(const String& msg) { logMsg(msg.c_str()); }

const int PRINTER_WIDTH       = 400;
const int PRINTER_WIDTH_BYTES = PRINTER_WIDTH / 8;

#define PRINT_SCALE  2
#define SCALE_SMALL  1
#define SCALE_MEDIUM 2
#define SCALE_LARGE  3

#define MAX_GLYPH_BYTES 512
static uint8_t glyphBuf[MAX_GLYPH_BYTES];

// ========== VLW FONT STRUCTS ==========
// bitmapOffsets[] has been REMOVED — it was malloc()ing 16-40KB of heap
// permanently at boot, consuming the contiguous block that NimBLE and TLS
// both need. Offsets are now computed on-the-fly via findGlyphOffset().

struct VlwGlyph {
  uint32_t cp;
  int16_t  w, h, advance, x_off, y_off;
  uint32_t bitmapOffset;
};

struct VlwFont {
  int      count;
  int      size;          // point size from VLW header
  uint32_t indexStart;    // file offset where glyph index begins
  char*    path;
  bool     loaded;
  // NOTE: no bitmapOffsets[] array — zero heap cost at load time.
};

VlwFont fontBasic = {0};
VlwFont fontCJK   = {0};

// ========== BITMAP CANVAS ==========

class PrintCanvas : public Adafruit_GFX {
public:
  uint8_t *buffer;
  int bufferSize;

  PrintCanvas(int16_t w, int16_t h) : Adafruit_GFX(w, h) {
    bufferSize = (w / 8) * h;
    buffer = (uint8_t*)malloc(bufferSize);
    if(buffer) memset(buffer, 0, bufferSize);
  }
  ~PrintCanvas() { if(buffer) free(buffer); }

  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    if(!buffer || x < 0 || x >= _width || y < 0 || y >= _height) return;
    int byteIndex = (y * (_width / 8)) + (x / 8);
    int bitIndex  = 7 - (x % 8);
    if(color) buffer[byteIndex] |=  (1 << bitIndex);
    else       buffer[byteIndex] &= ~(1 << bitIndex);
  }
  void clear() { if(buffer) memset(buffer, 0, bufferSize); }
};

// ========== STRUCTS ==========

struct EventConfig {
  bool    enabled = true;
  String  msg[3];
  uint8_t font[3];
  int     align[3];
  bool    bold[3];
  bool    invert[3];
  int     feed = 3;
};

struct TwitchConfig {
  EventConfig subs;
  EventConfig bits;
  EventConfig points;
  EventConfig raids;
} twitchCfg;

void initDefaults() {
  twitchCfg.subs.msg[0] = "NEW SUB:";
  twitchCfg.subs.msg[1] = "{user}!";
  twitchCfg.subs.msg[2] = "";
  for(int i=0;i<3;i++){
    twitchCfg.subs.font[i]=SCALE_MEDIUM; twitchCfg.subs.align[i]=1;
    twitchCfg.subs.bold[i]=true; twitchCfg.subs.invert[i]=false;
  }
  twitchCfg.bits.msg[0] = "CHEER:";
  twitchCfg.bits.msg[1] = "{user}";
  twitchCfg.bits.msg[2] = "{amount} bits";
  for(int i=0;i<3;i++){
    twitchCfg.bits.font[i]=SCALE_MEDIUM; twitchCfg.bits.align[i]=1;
    twitchCfg.bits.bold[i]=true; twitchCfg.bits.invert[i]=false;
  }
  twitchCfg.points.msg[0] = "REDEEM:";
  twitchCfg.points.msg[1] = "{user}";
  twitchCfg.points.msg[2] = "{reward}";
  for(int i=0;i<3;i++){
    twitchCfg.points.font[i]=SCALE_MEDIUM; twitchCfg.points.align[i]=1;
    twitchCfg.points.bold[i]=true; twitchCfg.points.invert[i]=false;
  }
  twitchCfg.raids.msg[0] = "RAID!";
  twitchCfg.raids.msg[1] = "from";
  twitchCfg.raids.msg[2] = "{user}";
  for(int i=0;i<3;i++){
    twitchCfg.raids.font[i]=SCALE_LARGE; twitchCfg.raids.align[i]=1;
    twitchCfg.raids.bold[i]=false; twitchCfg.raids.invert[i]=false;
  }
}

// ========== VLW LOADER ==========
// Reads only the header and records indexStart. No heap allocation.

bool loadVlw(VlwFont& f, const char* path) {
  File file = LittleFS.open(path, "r");
  if (!file) { logMsg("VLW missing: " + String(path)); return false; }

  auto read32 = [&]() -> int32_t {
    uint8_t b[4]; file.read(b, 4);
    return (int32_t)((b[0]<<24)|(b[1]<<16)|(b[2]<<8)|b[3]);
  };

  f.count = read32();
  /* ver */ read32();
  f.size  = read32();
  read32(); read32(); read32();   // reserved fields

  f.indexStart = file.position(); // byte offset of first index entry
  file.close();

  if (f.path) free(f.path);
  f.path   = strdup(path);
  f.loaded = true;
  logMsg("VLW loaded: " + String(path) + " (" + String(f.count) + " glyphs, 0 bytes heap)");
  return true;
}

// ========== GLYPH OFFSET CALCULATOR ==========
// Walks the VLW index once to compute the bitmap offset for a given
// index position. Each entry is 24 bytes; bitmap data follows all entries.
// This replaces the old bitmapOffsets[] array that malloc'd 4 bytes per glyph.

uint32_t findGlyphOffset(const VlwFont& f, File& file, int idx) {
  // Scan entries 0..idx-1 accumulating bitmap bytes to find where entry
  // idx's bitmap starts.
  uint32_t bmpOff = f.indexStart + (uint32_t)f.count * 24;
  for (int i = 0; i < idx; i++) {
    file.seek(f.indexStart + i * 24 + 4); // skip cp(4), read h(4) w(4)
    uint8_t b[8]; file.read(b, 8);
    int16_t h = (int16_t)((b[0]<<8)|b[1]);
    int16_t w = (int16_t)((b[4]<<8)|b[5]);
    int rowBytes = (w + 7) / 8;
    bmpOff += (uint32_t)rowBytes * (uint32_t)h;
  }
  return bmpOff;
}

// ========== GLYPH LOOKUP ==========
// Binary-search the index for codepoint cp in an already-open file.
// Calls findGlyphOffset() only for the matching entry — O(log n) seeks
// for the binary search + O(n/2) linear scan for the offset.
// For a 4000-glyph font that's ~12 seeks + ~2000 for the offset scan.
// Acceptable for print-time; zero heap cost.

bool findGlyphInOpenFile(const VlwFont& f, File& file, uint32_t cp, VlwGlyph* out) {
  if (!f.loaded || f.count == 0) return false;

  auto read32at = [&](uint32_t pos) -> int32_t {
    file.seek(pos);
    uint8_t b[4]; file.read(b, 4);
    return (int32_t)((b[0]<<24)|(b[1]<<16)|(b[2]<<8)|b[3]);
  };

  int lo = 0, hi = f.count - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    uint32_t midCp = (uint32_t)read32at(f.indexStart + mid * 24);
    if (midCp == cp) {
      // Read remaining fields for this entry
      file.seek(f.indexStart + mid * 24 + 4);
      uint8_t b[20]; file.read(b, 20);
      out->cp      = cp;
      out->h       = (int16_t)((b[0]<<8)|b[1]);
      out->w       = (int16_t)((b[4]<<8)|b[5]);
      out->advance = (int16_t)((b[8]<<8)|b[9]);
      out->x_off   = (int16_t)((b[12]<<8)|b[13]);
      out->y_off   = (int16_t)((b[16]<<8)|b[17]);
      out->bitmapOffset = findGlyphOffset(f, file, mid);
      return true;
    } else if (midCp < cp) lo = mid + 1;
    else                   hi = mid - 1;
  }
  return false;
}

// ========== TEXT PROCESSING ==========

String processNewlines(String text) {
  text.replace("\\n", "\n");
  text.replace("{nl}", "\n");
  return text;
}

String sanitizeText(String text) {
  String result = "";
  int i = 0, len = (int)text.length();
  while (i < len) {
    unsigned char c = (unsigned char)text[i];
    if (c == '\n') {
      result += '\n'; i++;
    } else if (c < 0x20 || c == 0x7F) {
      i++;
    } else if (c <= 0x7E) {
      result += (char)c; i++;
    } else if (c >= 0xF0 && c <= 0xF7) {
      if (i+3 < len &&
          ((unsigned char)text[i+1]&0xC0)==0x80 &&
          ((unsigned char)text[i+2]&0xC0)==0x80 &&
          ((unsigned char)text[i+3]&0xC0)==0x80) {
        result+=text[i]; result+=text[i+1]; result+=text[i+2]; result+=text[i+3]; i+=4;
      } else { i++; }
    } else if (c >= 0xE0 && c <= 0xEF) {
      if (i+2 < len &&
          ((unsigned char)text[i+1]&0xC0)==0x80 &&
          ((unsigned char)text[i+2]&0xC0)==0x80) {
        result+=text[i]; result+=text[i+1]; result+=text[i+2]; i+=3;
      } else { i++; }
    } else if (c >= 0xC0 && c <= 0xDF) {
      if (i+1 < len && ((unsigned char)text[i+1]&0xC0)==0x80) {
        result+=text[i]; result+=text[i+1]; i+=2;
      } else { i++; }
    } else {
      i++;
    }
  }
  return result;
}

uint32_t nextCodepoint(const String& s, int& i) {
  unsigned char c = (unsigned char)s[i];
  if (c < 0x80)  { i++; return c; }
  if (c < 0xC0)  { i++; return 0xFFFD; }
  if (c < 0xE0)  { uint32_t cp=(c&0x1F); i++; if(i<(int)s.length()) cp=(cp<<6)|((unsigned char)s[i++]&0x3F); return cp; }
  if (c < 0xF0)  { uint32_t cp=(c&0x0F); i++; for(int j=0;j<2&&i<(int)s.length();j++) cp=(cp<<6)|((unsigned char)s[i++]&0x3F); return cp; }
  { uint32_t cp=(c&0x07); i++; for(int j=0;j<3&&i<(int)s.length();j++) cp=(cp<<6)|((unsigned char)s[i++]&0x3F); return cp; }
}

// ========== VLW MEASURE / WORD-WRAP ==========
// All variants accept pre-opened File& handles so callers that already
// have the files open pay zero open/close overhead per character.
// The no-handle overloads open/close once internally for convenience.

int measureTextVlwF(const String& text, File& fBasic, File& fCJK) {
  int total = 0, i = 0, len = (int)text.length();
  VlwGlyph g;
  while (i < len) {
    uint32_t cp = nextCodepoint(text, i);
    if (fontBasic.loaded && findGlyphInOpenFile(fontBasic, fBasic, cp, &g)) {
      total += g.advance;
    } else if (fontCJK.loaded && findGlyphInOpenFile(fontCJK, fCJK, cp, &g)) {
      total += g.advance;
    } else {
      total += fontBasic.size / 2;
    }
  }
  return total;
}

int measureTextVlw(const String& text) {
  File fB, fC;
  if (fontBasic.loaded && fontBasic.path) fB = LittleFS.open(fontBasic.path, "r");
  if (fontCJK.loaded   && fontCJK.path)   fC = LittleFS.open(fontCJK.path,   "r");
  int w = measureTextVlwF(text, fB, fC);
  if (fB) fB.close();
  if (fC) fC.close();
  return w;
}

String wordWrapF(const String& text, int maxWidth, File& fBasic, File& fCJK) {
  String result = "";
  int lineStart = 0, textLen = (int)text.length();
  VlwGlyph g;

  while (lineStart < textLen) {
    int lineEnd = text.indexOf('\n', lineStart);
    if (lineEnd < 0) lineEnd = textLen;

    int i = lineStart, lineWidth = 0, lastSpaceI = -1;
    while (i < lineEnd) {
      if (text[i] == ' ') { lastSpaceI = i; }
      int before = i;
      uint32_t cp = nextCodepoint(text, i);
      int adv;
      if (fontBasic.loaded && findGlyphInOpenFile(fontBasic, fBasic, cp, &g))      adv = g.advance;
      else if (fontCJK.loaded && findGlyphInOpenFile(fontCJK, fCJK, cp, &g))       adv = g.advance;
      else                                                                           adv = fontBasic.size / 2;

      if (lineWidth + adv > maxWidth && lineWidth > 0) {
        int breakAt = (lastSpaceI > lineStart) ? lastSpaceI : before;
        result += text.substring(lineStart, breakAt);
        result += '\n';
        lineStart = breakAt;
        if (lineStart < textLen && text[lineStart] == ' ') lineStart++;
        lineWidth = 0; lastSpaceI = -1;
        i = lineStart;
        lineEnd = text.indexOf('\n', lineStart);
        if (lineEnd < 0) lineEnd = textLen;
        continue;
      }
      lineWidth += adv;
    }
    result += text.substring(lineStart, lineEnd);
    if (lineEnd < textLen) result += '\n';
    lineStart = lineEnd + 1;
  }
  return result;
}

String wordWrap(const String& text, int maxWidth) {
  File fB, fC;
  if (fontBasic.loaded && fontBasic.path) fB = LittleFS.open(fontBasic.path, "r");
  if (fontCJK.loaded   && fontCJK.path)   fC = LittleFS.open(fontCJK.path,   "r");
  String r = wordWrapF(text, maxWidth, fB, fC);
  if (fB) fB.close();
  if (fC) fC.close();
  return r;
}

// ========== BITMAP PRINTING ==========

void sendCmd(const uint8_t* cmd, size_t len) {
  if(!printerConnected || !pWriteCharacteristic) return;
  pWriteCharacteristic->writeValue((uint8_t*)cmd, len);
  delay(10);
}

void printBitmap(uint8_t *bitmap, int width, int height) {
  if(!printerConnected || !bitmap) return;
  int widthBytes = width / 8;
  uint8_t cmd[] = {
    0x1D, 0x76, 0x30, 0x00,
    (uint8_t)(widthBytes & 0xFF), (uint8_t)(widthBytes >> 8),
    (uint8_t)(height     & 0xFF), (uint8_t)(height     >> 8)
  };
  pWriteCharacteristic->writeValue(cmd, 8); delay(10);
  int totalBytes = widthBytes * height, chunkSize = 200;
  for(int i = 0; i < totalBytes; i += chunkSize) {
    int sendSize = min(chunkSize, totalBytes - i);
    pWriteCharacteristic->writeValue(&bitmap[i], sendSize);
    delay(10);
  }
}

void feedPaper(int lines) {
  if(lines > 0 && lines < 256) {
    uint8_t cmd[] = {0x1B, 0x64, (uint8_t)lines};
    sendCmd(cmd, 3);
  }
}

int drawGlyph(PrintCanvas& canvas, File& fontFile,
              const VlwGlyph* g, int x, int baseline_y, bool invert) {
  if (!g || g->w == 0 || g->h == 0) return g ? g->advance : 0;
  int rowBytes = (g->w + 7) / 8;
  int bmpBytes = rowBytes * g->h;
  if (bmpBytes > MAX_GLYPH_BYTES) {
    logMsg("WARN: glyph too large for glyphBuf, skipping");
    return g->advance;
  }
  fontFile.seek(g->bitmapOffset);
  fontFile.read(glyphBuf, bmpBytes);
  for (int row = 0; row < g->h; row++) {
    int py = baseline_y + g->y_off + row;
    for (int col = 0; col < g->w; col++) {
      int px = x + g->x_off + col;
      uint8_t byte = glyphBuf[row * rowBytes + col / 8];
      bool set = (byte >> (7 - (col % 8))) & 1;
      if (set) canvas.drawPixel(px, py, invert ? 0 : 1);
    }
  }
  return g->advance;
}

// ========== THERMAL PRINT ==========

bool printToThermal(String text, uint8_t printScale, int align, bool bold, bool invert, int feedLines) {
  if(!printerConnected) return false;
  if(text.length() == 0) { if(feedLines > 0) feedPaper(feedLines); return true; }
  unsigned long tStart = millis();

  text = processNewlines(text);
  if (printScale < 1) printScale = 1;
  if (printScale > 3) printScale = 3;

  const int vlwSize    = (fontBasic.loaded ? fontBasic.size : 16);
  const int renderW    = PRINTER_WIDTH / printScale;
  const int maxTextW   = renderW - 8;
  const int lineSpacing = 3;
  const int lineHeight  = vlwSize + lineSpacing;
  const int baseline    = vlwSize;

  // Open font files once for the entire render — shared by wordwrap,
  // measure, and drawGlyph. Zero per-character open/close overhead.
  File fBasicHandle, fCJKHandle;
  if (fontBasic.loaded && fontBasic.path) fBasicHandle = LittleFS.open(fontBasic.path, "r");
  if (fontCJK.loaded   && fontCJK.path)   fCJKHandle   = LittleFS.open(fontCJK.path,   "r");

  text = wordWrapF(text, maxTextW, fBasicHandle, fCJKHandle);

  int totalLines = 1;
  for(int i = 0; i < (int)text.length(); i++) if(text[i] == '\n') totalLines++;

  int linesPerChunk = max(1, 200 / lineHeight);
  int currentLineIndex = 0, textIndex = 0;

  while(currentLineIndex < totalLines) {
    int chunkLineCount = 0, chunkHeight = 0;
    if(currentLineIndex == 0) chunkHeight += lineSpacing * 2;
    for(int i = 0; i < linesPerChunk && (currentLineIndex + i) < totalLines; i++) {
      chunkLineCount++; chunkHeight += lineHeight;
    }
    if(currentLineIndex + chunkLineCount >= totalLines) chunkHeight += lineSpacing * 2;

    PrintCanvas canvas(renderW, chunkHeight);
    if(!canvas.buffer) { logMsg("Chunk alloc failed!"); break; }

    if(invert) canvas.fillRect(0, 0, renderW, chunkHeight, 1);

    int drawY = (currentLineIndex == 0) ? lineSpacing + baseline : baseline;

    for(int i = 0; i < chunkLineCount; i++) {
      int lineEnd = text.indexOf('\n', textIndex);
      if(lineEnd < 0) lineEnd = (int)text.length();
      String line = text.substring(textIndex, lineEnd);

      if(line.length() > 0) {
        int tw = min(measureTextVlwF(line, fBasicHandle, fCJKHandle), renderW - 4);

        int x = 2;
        if     (align == 1) x = max(2, (renderW - tw) / 2);
        else if(align == 2) x = max(2, renderW - tw - 2);

        int ci = 0, clen = (int)line.length();
        while (ci < clen) {
          uint32_t cp = nextCodepoint(line, ci);
          VlwGlyph glyphOut;

          if (fBasicHandle && fontBasic.loaded &&
              findGlyphInOpenFile(fontBasic, fBasicHandle, cp, &glyphOut)) {
            x += drawGlyph(canvas, fBasicHandle, &glyphOut, x, drawY, invert);
          } else if (fCJKHandle && fontCJK.loaded &&
              findGlyphInOpenFile(fontCJK, fCJKHandle, cp, &glyphOut)) {
            x += drawGlyph(canvas, fCJKHandle, &glyphOut, x, drawY, invert);
          } else {
            x += vlwSize / 2;
          }
          if (x >= renderW - 2) break;
        }
      }

      drawY    += lineHeight;
      textIndex = lineEnd + 1;
    }

    {
      int scaledH      = chunkHeight * printScale;
      int scaledWBytes = PRINTER_WIDTH / 8;
      int renderWBytes = renderW / 8;
      uint8_t* scaled  = (uint8_t*)malloc(scaledWBytes * scaledH);
      if (scaled) {
        memset(scaled, 0, scaledWBytes * scaledH);
        for (int row = 0; row < chunkHeight; row++) {
          const uint8_t* srcRow = canvas.buffer + row * renderWBytes;
          for (int rep = 0; rep < printScale; rep++) {
            uint8_t* dstRow = scaled + (row * printScale + rep) * scaledWBytes;
            for (int dstBit = 0; dstBit < PRINTER_WIDTH; dstBit++) {
              int srcBit = dstBit / printScale;
              if (srcBit < renderW) {
                int sb = srcBit / 8, sbit = 7 - (srcBit % 8);
                if (srcRow[sb] & (1 << sbit)) {
                  dstRow[dstBit / 8] |= (1 << (7 - (dstBit % 8)));
                }
              }
            }
          }
        }
        printBitmap(scaled, PRINTER_WIDTH, scaledH);
        free(scaled);
      }
    }
    currentLineIndex += chunkLineCount;
    delay(20);
  }

  if (fBasicHandle) fBasicHandle.close();
  if (fCJKHandle)   fCJKHandle.close();

  if(feedLines > 0) feedPaper(feedLines);
  unsigned long dt = millis() - tStart;
  if (dt > 1000) logMsg("printToThermal took " + String(dt) + " ms (" + String(text.length()) + " chars)");
  return true;
}

void printEvent(EventConfig& cfg, String username, String val1, String val2) {
  if(!cfg.enabled) return;
  logMsg("Printing Event...");
  for(int i = 0; i < 3; i++) {
    if(cfg.msg[i].length() == 0) continue;
    String p = cfg.msg[i];
    p.replace("{user}",   username);
    p.replace("{amount}", val1);
    p.replace("{reward}", val2);
    p = sanitizeText(p);
    int feed = (i == 2 || (i < 2 && cfg.msg[i+1].length() == 0)) ? cfg.feed : 0;
    printToThermal(p, cfg.font[i], cfg.align[i], cfg.bold[i], cfg.invert[i], feed);
  }
}

// ========== TWITCH IRC PARSING ==========

String extractIRCMessage(const String& line) {
  int cmdPos = line.indexOf(" PRIVMSG ");
  if (cmdPos < 0) cmdPos = line.indexOf(" USERNOTICE ");
  if (cmdPos < 0) return "";
  int hashPos = line.indexOf('#', cmdPos);
  if (hashPos < 0) return "";
  int spaceAfterChan = line.indexOf(' ', hashPos);
  if (spaceAfterChan < 0) return "";
  if (spaceAfterChan + 1 >= (int)line.length() || line[spaceAfterChan + 1] != ':') return "";
  String payload = line.substring(spaceAfterChan + 2);
  payload.replace("\r", ""); payload.trim();
  return payload;
}

String extractTag(const String& line, const String& tagName) {
  String search = tagName + "=";
  int start = line.indexOf(search);
  if (start < 0) return "";
  start += search.length();
  int end = line.indexOf(';', start);
  int spaceEnd = line.indexOf(' ', start);
  if (end < 0 || (spaceEnd >= 0 && spaceEnd < end)) end = spaceEnd;
  if (end < 0) end = line.length();
  return line.substring(start, end);
}

void parseTwitchMessage(String msg) {
  if      (msg.indexOf("msg-id=sub") > 0)
    printEvent(twitchCfg.subs,   extractTag(msg,"display-name"), "", "");
  else if (msg.indexOf("bits=") > 0)
    printEvent(twitchCfg.bits,   extractTag(msg,"display-name"), extractTag(msg,"bits"), "");
  else if (msg.indexOf("custom-reward-id=") > 0) {
    String rewardId = extractTag(msg, "custom-reward-id");
    if (pointsRewardFilter.length() > 0 && rewardId != pointsRewardFilter) return;
    printEvent(twitchCfg.points, extractTag(msg,"display-name"), "", extractIRCMessage(msg));
  } else if (msg.indexOf("msg-id=raid") > 0)
    printEvent(twitchCfg.raids,  extractTag(msg,"display-name"), "", "");
}

// connectTwitch() — called from loop() only, never from setup().
void connectTwitch() {
  if (ESP.getMaxAllocHeap() < 20000) {
    logMsg("Twitch connect SKIPPED: heap too low (maxAlloc=" + String(ESP.getMaxAllocHeap()) + ")");
    twitchConnected = false;
    return;
  }
  logMsg("Connecting to Twitch IRC... heap free=" + String(ESP.getFreeHeap()) + " maxAlloc=" + String(ESP.getMaxAllocHeap()));
  twitchClient.setInsecure();
  if(twitchClient.connect("irc.chat.twitch.tv", 6697)) {
    twitchClient.println("PASS " TWITCH_OAUTH_SECRET);
    twitchClient.println("NICK " TWITCH_OAUTH_NICK);
    twitchClient.println("CAP REQ :twitch.tv/tags twitch.tv/commands");
    twitchClient.println("JOIN #" TWITCH_CHANNEL);
    twitchConnected      = true;
    twitchEverConnected  = true;
    lastTwitchPing       = millis();
    logMsg("Twitch OK");
  } else {
    twitchConnected = false;
    logMsg("Twitch failed");
  }
}

void handleTwitchIRC() {
  if(!twitchConnected) return;
  while(twitchClient.available()) {
    String line = twitchClient.readStringUntil('\n');
    line.trim();
    if(line.startsWith("PING")) {
      twitchClient.println("PONG :tmi.twitch.tv");
      lastTwitchPing = millis();
    } else if(line.indexOf("PRIVMSG") > 0 || line.indexOf("USERNOTICE") > 0) {
      parseTwitchMessage(line);
    }
  }
  if(millis() - lastTwitchPing > 240000) {
    twitchClient.println("PING :tmi.twitch.tv");
    lastTwitchPing = millis();
  }
  if(!twitchClient.connected()) {
    twitchConnected = false;
    logMsg("Twitch connection lost — will retry in 10s");
  }
}

// ========== BLE CONNECTION ==========

static void bleDeinit() {
  pClient = nullptr;
  pWriteCharacteristic = nullptr;
  BLEDevice::deinit(true);
  logMsg("BLE stack deinit. Heap: " + String(ESP.getFreeHeap()) + " MaxAlloc: " + String(ESP.getMaxAllocHeap()));
}

class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* p) {
    logMsg("BLE onConnect fired");
  }
  void onDisconnect(BLEClient* p) {
    printerConnected = false;
    pWriteCharacteristic = nullptr;
    bleState = BLE_IDLE;
    logMsg("BLE Disconnected — will retry in 15s");
  }
};

void connectPrinter() {
  if (ESP.getFreeHeap() < 35000 || ESP.getMaxAllocHeap() < 35000) {
    logMsg("BLE connect SKIPPED: heap too low (free=" + String(ESP.getFreeHeap()) +
           " maxAlloc=" + String(ESP.getMaxAllocHeap()) + ")");
    return;
  }
  logMsg("Connecting BLE: " + printerMAC + " heap free=" + String(ESP.getFreeHeap()) + " maxAlloc=" + String(ESP.getMaxAllocHeap()));
  BLEDevice::init("ESP32-C3-Printer");
  if(pClient) { delete pClient; pClient = nullptr; }
  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());
  bleState = BLE_CONNECTING;
  pClient->connect(BLEAddress(printerMAC.c_str()), false);  // non-blocking
}

void disconnectPrinter() {
  if(pClient && printerConnected) pClient->disconnect();
  printerConnected = false;
  bleState = BLE_IDLE;
}

// ========== CONFIGURATION STORAGE ==========

void loadConfig() {
  initDefaults();
  preferences.begin("twitch", false);
  auto loadEvent = [&](const char* prefix, EventConfig& evt) {
    evt.enabled = preferences.getBool((String(prefix)+"_e").c_str(), true);
    evt.feed    = preferences.getInt( (String(prefix)+"_f").c_str(), 3);
    for(int i = 0; i < 3; i++) {
      String p = String(prefix) + String(i);
      if(preferences.isKey((p+"_m").c_str())) evt.msg[i] = preferences.getString((p+"_m").c_str());
      evt.font[i]   = (uint8_t)preferences.getInt((p+"_s").c_str(), (int)evt.font[i]);
      evt.align[i]  = preferences.getInt( (p+"_a").c_str(), evt.align[i]);
      evt.bold[i]   = preferences.getBool((p+"_b").c_str(), evt.bold[i]);
      evt.invert[i] = preferences.getBool((p+"_i").c_str(), evt.invert[i]);
    }
  };
  loadEvent("sub",  twitchCfg.subs);
  loadEvent("bit",  twitchCfg.bits);
  loadEvent("pts",  twitchCfg.points);
  loadEvent("raid", twitchCfg.raids);
  pointsRewardFilter = preferences.getString("pts_filter", "");
  preferences.end();
  logMsg("Config loaded");
}

void saveConfig() {
  preferences.end();
  if(!preferences.begin("twitch", false)) return;
  auto saveEvent = [&](const char* prefix, EventConfig& evt) {
    preferences.putBool((String(prefix)+"_e").c_str(), evt.enabled);
    preferences.putInt( (String(prefix)+"_f").c_str(), evt.feed);
    for(int i = 0; i < 3; i++) {
      String p = String(prefix) + String(i);
      preferences.putString((p+"_m").c_str(), evt.msg[i]);
      preferences.putInt(   (p+"_s").c_str(), (int)evt.font[i]);
      preferences.putInt(   (p+"_a").c_str(), evt.align[i]);
      preferences.putBool(  (p+"_b").c_str(), evt.bold[i]);
      preferences.putBool(  (p+"_i").c_str(), evt.invert[i]);
      delay(2); yield();
    }
  };
  saveEvent("sub",  twitchCfg.subs);
  saveEvent("bit",  twitchCfg.bits);
  saveEvent("pts",  twitchCfg.points);
  saveEvent("raid", twitchCfg.raids);
  preferences.putString("pts_filter", pointsRewardFilter);
  preferences.end();
  logMsg("Config saved");
}

// ========== WEB SERVER ==========

void handleRoot()   { server.send(200, "text/html; charset=UTF-8", htmlPage); }

void handleStatus() {
  String json = "{\"printer\":" + String(printerConnected?"true":"false") +
                ",\"twitch\":"  + String(twitchConnected ?"true":"false") + "}";
  server.send(200, "application/json", json);
}

void handleGetConfig() {
  String json = "{";
  auto addEvt = [&](const char* p, EventConfig& e) {
    json += "\""+String(p)+"_e\":"+(e.enabled?"true":"false")+",";
    json += "\""+String(p)+"_f\":"+String(e.feed)+",";
    for(int i=0;i<3;i++){
      String k = String(p)+String(i);
      json += "\""+k+"_m\":\""+e.msg[i]+"\",";
      json += "\""+k+"_s\":"+String((int)e.font[i])+",";
      json += "\""+k+"_a\":"+String(e.align[i])+",";
      json += "\""+k+"_b\":"+(e.bold[i]?"true":"false")+",";
      json += "\""+k+"_i\":"+(e.invert[i]?"true":"false")+",";
    }
  };
  addEvt("sub",  twitchCfg.subs);
  addEvt("bit",  twitchCfg.bits);
  addEvt("pts",  twitchCfg.points);
  addEvt("raid", twitchCfg.raids);
  json += "\"pts_filter\":\""+pointsRewardFilter+"\"";
  json += "}";
  server.send(200, "application/json", json);
}

void handleConnect()    { connectPrinter(); server.send(200,"text/plain","Connecting..."); }
void handleDisconnect() { disconnectPrinter(); server.send(200,"text/plain","Disconnected"); }

void handlePrint() {
  if(!printerConnected) { server.send(400,"text/plain","Not connected"); return; }
  String text = sanitizeText(server.arg("txt"));
  uint8_t printScale = server.hasArg("sz") ? (uint8_t)server.arg("sz").toInt() : SCALE_MEDIUM;
  int  al  = server.hasArg("al")  ? server.arg("al").toInt()   : 1;
  bool b   = server.hasArg("b")   ? (server.arg("b")  =="1")   : true;
  bool inv = server.hasArg("inv") ? (server.arg("inv")=="1")   : false;
  printToThermal(text, printScale, al, b, inv, 3);
  server.send(200,"text/plain","Printed!");
}

void handleFeed() {
  if(!printerConnected) { server.send(400,"text/plain","Not connected"); return; }
  int lines = server.arg("lines").toInt();
  if(lines < 1) lines = 3;
  feedPaper(lines);
  server.send(200,"text/plain","Fed "+String(lines)+" lines");
}

void handleTwitchConfig() {
  auto updEvt = [&](const char* p, EventConfig& e) {
    if(server.hasArg(String(p)+"_e")) e.enabled = server.arg(String(p)+"_e")=="1";
    if(server.hasArg(String(p)+"_f")) e.feed    = server.arg(String(p)+"_f").toInt();
    for(int i=0;i<3;i++){
      String k = String(p)+String(i);
      if(server.hasArg(k+"_m")) e.msg[i]    = server.arg(k+"_m");
      if(server.hasArg(k+"_s")) e.font[i]   = (uint8_t)server.arg(k+"_s").toInt();
      if(server.hasArg(k+"_a")) e.align[i]  = server.arg(k+"_a").toInt();
      if(server.hasArg(k+"_b")) e.bold[i]   = server.arg(k+"_b")=="1";
      if(server.hasArg(k+"_i")) e.invert[i] = server.arg(k+"_i")=="1";
    }
  };
  updEvt("sub",twitchCfg.subs); updEvt("bit",twitchCfg.bits);
  updEvt("pts",twitchCfg.points); updEvt("raid",twitchCfg.raids);
  if(server.hasArg("pts_filter")){ pointsRewardFilter=server.arg("pts_filter"); pointsRewardFilter.trim(); }
  shouldSaveConfig = true;
  server.send(200,"text/plain","Config Saved!");
}

void handleTestEvent() {
  if(!printerConnected) { server.send(400,"text/plain","No Printer"); return; }
  EventConfig tCfg;
  String type = server.arg("type");
  tCfg.enabled = true;
  tCfg.feed    = server.arg("f").toInt();
  for(int i=0;i<3;i++){
    tCfg.msg[i]    = server.arg("m"+String(i));
    tCfg.font[i]   = (uint8_t)server.arg("s"+String(i)).toInt();
    tCfg.align[i]  = server.arg("a"+String(i)).toInt();
    tCfg.bold[i]   = server.arg("b"+String(i))=="1";
    tCfg.invert[i] = server.arg("i"+String(i))=="1";
  }
  if     (type=="sub")  printEvent(tCfg,"TestUser","","");
  else if(type=="bit")  printEvent(tCfg,"TestUser","1000","");
  else if(type=="pts")  printEvent(tCfg,"TestUser","","Hydrate :> \u30C4");
  else if(type=="raid") printEvent(tCfg,"TestUser","","");
  server.send(200,"text/plain","Test Sent");
}

// ========== FIRMWARE UPDATE ==========

void handleFirmwareUpload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    if (printerConnected || bleState == BLE_CONNECTING || bleState == BLE_DISCOVERING) {
      logMsg("Disconnecting BLE before firmware upload");
      disconnectPrinter();
      delay(200);
    }
    logMsg("Firmware upload start: " + upload.filename + " free heap " + String(ESP.getFreeHeap()));
    if (ESP.getMaxAllocHeap() < 20000) {
      logMsg("REJECTED firmware upload: MaxAlloc too low (" + String(ESP.getMaxAllocHeap()) + ")");
      return;
    }
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH))
      logMsg("Update.begin failed: " + String(Update.errorString()));
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
      logMsg("Update.write failed: " + String(Update.errorString()));
  } else if (upload.status == UPLOAD_FILE_END) {
    lastUploadFinish = millis();
    if (Update.end(true)) logMsg("Firmware update SUCCESS, " + String(upload.totalSize) + " bytes, rebooting");
    else                  logMsg("Update.end failed: " + String(Update.errorString()));
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    logMsg("Firmware upload ABORTED");
  }
}

// ========== FILE UPLOAD ==========

void handleUpload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    if (printerConnected || bleState == BLE_CONNECTING || bleState == BLE_DISCOVERING) {
      logMsg("Disconnecting BLE before upload to free heap");
      disconnectPrinter();
      delay(200);
    }
    logMsg("Free heap before upload: " + String(ESP.getFreeHeap()) + " MaxAlloc: " + String(ESP.getMaxAllocHeap()));
    if (ESP.getMaxAllocHeap() < 20000) {
      logMsg("REJECTED upload: MaxAlloc too low — reboot device first");
      uploadFailed = true;
      return;
    }
    String filename = "/" + upload.filename;
    logMsg("Upload start: " + filename);
    size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
    size_t expected  = server.header("Content-Length").toInt();
    if (expected > 0 && expected > freeBytes) {
      logMsg("REJECTED: need " + String(expected) + " bytes, only " + String(freeBytes) + " free");
      uploadFailed = true;
      return;
    }
    if (LittleFS.exists(filename)) LittleFS.remove(filename);
    uploadFile = LittleFS.open(filename, "w");
    uploadWrittenBytes = 0;
    uploadFailed       = false;
    uploadInProgress   = true;
    if (!uploadFile) { logMsg("Failed to open file for writing"); uploadFailed = true; }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile && !uploadFailed) {
      size_t written = uploadFile.write(upload.buf, upload.currentSize);
      uploadWrittenBytes += written;
      if (written != upload.currentSize) {
        logMsg("WRITE MISMATCH: expected " + String(upload.currentSize) + ", wrote " + String(written));
        uploadFailed = true;
      }
    }
    yield();
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) uploadFile.close();
    logMsg("Upload end: " + String(uploadWrittenBytes) + " bytes written");
    uploadInProgress = false;
    lastUploadFinish = millis();
    if (uploadWrittenBytes == 0) { uploadFailed = true; logMsg("Upload wrote zero bytes"); }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    logMsg("Upload ABORTED by client");
    uploadFailed     = true;
    uploadInProgress = false;
    if (uploadFile) uploadFile.close();
  }
}

void handleUploadComplete() {
  if (uploadFailed)
    server.send(500, "text/plain", "Upload FAILED — wrote " + String(uploadWrittenBytes) + " bytes");
  else
    server.send(200, "text/plain", "Upload OK: " + String(uploadWrittenBytes) + " bytes written.");
}

// ========== SETUP & LOOP ==========

void setup() {
  Serial.begin(115200);
  delay(1000);
  logMsg("Boot. Heap: " + String(ESP.getFreeHeap()) + " MaxAlloc: " + String(ESP.getMaxAllocHeap()));
  if (!LittleFS.begin(true))
    logMsg("LittleFS mount failed even after format attempt");
  else
    logMsg("LittleFS mounted OK");
  logMsg("LittleFS total: " + String(LittleFS.totalBytes()) + " used: " + String(LittleFS.usedBytes()));
  loadConfig();
  // loadVlw now allocates 0 heap — safe to call before WiFi/BLE init
  loadVlw(fontBasic, "/unifont_basic.vlw");
  loadVlw(fontCJK,   "/unifont_cjk.vlw");
  logMsg("After VLW load. Heap: " + String(ESP.getFreeHeap()) + " MaxAlloc: " + String(ESP.getMaxAllocHeap()));
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostname);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.begin(MYSSID, MYPSK);
  while(WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  logMsg("\nWiFi OK: " + WiFi.localIP().toString());
  if(MDNS.begin(hostname)) { MDNS.addService("http","tcp",80); logMsg("mDNS: http://c3printer.local"); }
  ArduinoOTA.setHostname(hostname);
  ArduinoOTA.begin();
  server.on("/",         handleRoot);
  server.on("/s",        handleStatus);
  server.on("/gcfg",     handleGetConfig);
  server.on("/c",        handleConnect);
  server.on("/d",        handleDisconnect);
  server.on("/p",        HTTP_POST, handlePrint);
  server.on("/f",        handleFeed);
  server.on("/tcfg",     HTTP_POST, handleTwitchConfig);
  server.on("/test_evt", HTTP_POST, handleTestEvent);
  server.on("/upload",   HTTP_POST, handleUploadComplete, handleUpload);
  server.on("/ota_upload", HTTP_POST,
    [](){
      server.sendHeader("Connection", "close");
      server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK");
      delay(300);
      ESP.restart();
    },
    handleFirmwareUpload
  );
  server.on("/fsinfo", []() {
    size_t total = LittleFS.totalBytes(), used = LittleFS.usedBytes();
    String out = "<h2>LittleFS Info</h2>";
    out += "<p>Total: "+String(total)+" bytes<br>Used: "+String(used)+" bytes<br>Free: "+String(total-used)+" bytes</p>";
    out += "<h3>Files</h3><ul>";
    File root = LittleFS.open("/"); File f = root.openNextFile(); bool any = false;
    while (f) {
      any = true;
      String name = String(f.name());
      out += "<li>"+name+" - "+String(f.size())+" bytes <a href=\"/delete_file?name="+name+"\" onclick=\"return confirm('Delete "+name+"?')\" style=\"color:#f87171\">[delete]</a></li>";
      f = root.openNextFile();
    }
    if (!any) out += "<li>No files</li>";
    out += "</ul><p><a href=\"/\" style=\"color:#a78bfa\">&larr; Back</a></p>";
    server.send(200, "text/html", out);
  });
  server.on("/delete_file", []() {
    String name = server.arg("name");
    if (name.length() == 0) { server.send(400,"text/plain","Missing name"); return; }
    if (!name.startsWith("/")) name = "/"+name;
    if (!LittleFS.exists(name)) { server.send(404,"text/plain","Not found: "+name); return; }
    LittleFS.remove(name);
    server.send(200,"text/html","<p>Deleted: "+name+"</p><p><a href=\"/fsinfo\" style=\"color:#a78bfa\">&larr; Back</a></p>");
  });
  server.on("/console", []() {
    String out;
    if (logWrapped) { out += String(logBuffer).substring(logHead); out += String(logBuffer).substring(0, logHead); }
    else            { out  = String(logBuffer).substring(0, logHead); }
    server.send(200, "text/plain", out);
  });
  server.on("/ping", []() { server.send(200,"text/plain","pong"); });
  server.on("/log", []() {
    server.send(200, "text/html", R"rawliteral(
<!DOCTYPE html><html><head><title>Console</title>
<style>body{background:#0f0f23;color:#4ade80;font-family:monospace;font-size:12px;padding:10px;white-space:pre-wrap}</style>
</head><body><div id="log">Loading...</div>
<script>
async function tick(){const r=await fetch('/console');document.getElementById('log').textContent=await r.text();window.scrollTo(0,document.body.scrollHeight);}
setInterval(tick,1000);tick();
</script></body></html>)rawliteral");
  });
  server.begin();

  // bleDeadlineMs: Twitch will connect unconditionally at this time
  // even if BLE has not reached BLE_READY yet. Prevents Twitch being
  // permanently blocked by a slow or absent printer.
  bleDeadlineMs = millis() + 45000;
  logMsg("Ready! BLE will connect first; Twitch fallback in 45s if BLE stalls.");
}

void loop() {
  static unsigned long lastHeapLog      = 0;
  static unsigned long lastPrinterRetry = 0;
  static unsigned long lastTwitchRetry  = 0;
  static unsigned long connectStart     = 0;
  static unsigned long discoverStart    = 0;
  unsigned long now = millis();

  if (now - lastHeapLog > 10000) {
    lastHeapLog = now;
    char hb[128];
    snprintf(hb, sizeof(hb), "Uptime %lus  Free heap: %u MaxAlloc: %u RSSI %ddBm bleState: %d twitch: %d",
             now/1000, ESP.getFreeHeap(), ESP.getMaxAllocHeap(), WiFi.RSSI(), (int)bleState, (int)twitchConnected);
    logMsg(hb);
  }

  wl_status_t wifiStatus = WiFi.status();
  static wl_status_t lastWifiStatus = WL_CONNECTED;
  if (wifiStatus != lastWifiStatus) {
    lastWifiStatus = wifiStatus;
    logMsg("WiFi status changed: " + String(wifiStatus));
    if (wifiStatus == WL_CONNECTED) logMsg("WiFi reconnected: " + WiFi.localIP().toString());
  }

  ArduinoOTA.handle();
  if(shouldSaveConfig) { saveConfig(); shouldSaveConfig = false; }
  server.handleClient();

  if (!uploadInProgress) {

    // ---- Twitch IRC keep-alive / connect ----
    if (twitchConnected) {
      unsigned long t0 = micros();
      handleTwitchIRC();
      unsigned long dt = micros() - t0;
      if (dt > 50000) logMsg("WARN: handleTwitchIRC took " + String(dt) + " us");
    } else if (twitchEverConnected && now - lastTwitchRetry > 10000) {
      // Normal reconnect path after first successful connect
      lastTwitchRetry = now;
      connectTwitch();
    } else if (!twitchEverConnected && now >= bleDeadlineMs && now - lastTwitchRetry > 10000) {
      // BLE deadline expired — connect Twitch regardless of BLE state
      logMsg("BLE deadline reached — connecting Twitch unconditionally");
      lastTwitchRetry = now;
      connectTwitch();
    }

    // ---- BLE printer connect / reconnect ----
    if (bleState == BLE_IDLE && !printerConnected &&
        now - lastPrinterRetry > 15000 &&
        now - lastUploadFinish > 10000) {
      lastPrinterRetry = now;
      connectStart = 0;
      connectPrinter();
    }

    // ---- BLE_CONNECTING: poll isConnected() with 15s timeout ----
    if (bleState == BLE_CONNECTING && pClient) {
      if (connectStart == 0) connectStart = now;
      if (pClient->isConnected()) {
        logMsg("BLE connection established");
        connectStart = 0;
        bleState = BLE_DISCOVERING;
        discoverStart = 0;
      } else if (now - connectStart > 15000) {
        logMsg("BLE connect timeout — deiniting stack");
        bleDeinit();
        connectStart = 0;
        bleState = BLE_FAILED;
      }
    }

    // ---- BLE_DISCOVERING: service/char discovery with 10s timeout ----
    if (bleState == BLE_DISCOVERING && pClient) {
      if (discoverStart == 0) discoverStart = now;
      BLERemoteService* svc = pClient->getService(serviceUUID);
      if (svc) {
        pWriteCharacteristic = svc->getCharacteristic(charWriteUUID);
        if (pWriteCharacteristic) {
          discoverStart = 0;
          bleState = BLE_INITING;
        } else {
          logMsg("BLE char not found — deiniting stack");
          bleDeinit(); discoverStart = 0; bleState = BLE_FAILED;
        }
      } else if (now - discoverStart > 10000) {
        logMsg("BLE discover timeout — deiniting stack");
        bleDeinit(); discoverStart = 0; bleState = BLE_FAILED;
      }
    }

    // ---- BLE_INITING: send wake + init bytes, mark ready ----
    if (bleState == BLE_INITING) {
      printerConnected = true;
      uint8_t wake[] = {0x00,0x00,0x00,0x00,0x00};
      pWriteCharacteristic->writeValue(wake, 5);
      uint8_t init[] = {0x1B, 0x40};
      pWriteCharacteristic->writeValue(init, 2);
      logMsg("Printer Ready. Heap: " + String(ESP.getFreeHeap()) + " MaxAlloc: " + String(ESP.getMaxAllocHeap()));
      bleState = BLE_READY;

      // First Twitch connect on BLE success (before deadline)
      if (!twitchEverConnected) {
        logMsg("BLE ready — initiating first Twitch connect");
        connectTwitch();
        lastTwitchRetry = now;
      }
    }

    // ---- BLE_FAILED: Twitch fallback + back to IDLE ----
    if (bleState == BLE_FAILED) {
      if (!twitchEverConnected) {
        logMsg("BLE failed — connecting Twitch as fallback");
        connectTwitch();
        lastTwitchRetry = now;
      }
      bleState = BLE_IDLE;
    }
  }
  delay(10);
}
