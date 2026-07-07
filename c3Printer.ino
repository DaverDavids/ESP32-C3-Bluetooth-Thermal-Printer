#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
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

bool twitchEverConnected = false;
unsigned long bleDeadlineMs = 0;

enum BLEConnState { BLE_IDLE, BLE_CONNECTING, BLE_DISCOVERING, BLE_INITING, BLE_READY, BLE_FAILED };
volatile BLEConnState bleState = BLE_IDLE;
unsigned long lastTwitchPing = 0;

String pointsRewardFilter = "";
bool shouldSaveConfig = false;
File uploadFile;
size_t uploadWrittenBytes = 0;
bool uploadFailed = false;
bool uploadInProgress = false;
unsigned long lastUploadFinish = 0;

int wifiReconnectCount = 0;
uint8_t lastDisconnectReason = 0;
unsigned long lastDisconnectTime = 0;
unsigned long minFreeHeapSeen = 999999;

// LOG_BUF_SIZE reduced from 8192 to 2048 — saves 6KB of static RAM.
// The ring buffer is only consumed via /console; 2KB holds ~15-20 recent
// log lines which is sufficient for debugging.
#define LOG_BUF_SIZE 2048
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

#define SCALE_SMALL  1
#define SCALE_MEDIUM 2
#define SCALE_LARGE  3

#define MAX_GLYPH_BYTES 512
static uint8_t glyphBuf[MAX_GLYPH_BYTES];

// ========== VLW FONT STRUCTS ==========

struct VlwGlyph {
  uint32_t cp;
  int16_t  w, h, advance, x_off, y_off;
  uint32_t bitmapOffset;
};

struct VlwFont {
  int      count;
  int      size;
  uint32_t indexStart;
  char*    path;
  bool     loaded;
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
  read32(); read32(); read32();
  f.indexStart = file.position();
  file.close();
  if (f.path) free(f.path);
  f.path   = strdup(path);
  f.loaded = true;
  logMsg("VLW loaded: " + String(path) + " (" + String(f.count) + " glyphs, 0 bytes heap)");
  return true;
}

// ========== GLYPH OFFSET CALCULATOR ==========

uint32_t findGlyphOffset(const VlwFont& f, File& file, int idx) {
  uint32_t bmpOff = f.indexStart + (uint32_t)f.count * 24;
  for (int i = 0; i < idx; i++) {
    file.seek(f.indexStart + i * 24 + 4);
    uint8_t b[8]; file.read(b, 8);
    int16_t h = (int16_t)((b[0]<<8)|b[1]);
    int16_t w = (int16_t)((b[4]<<8)|b[5]);
    bmpOff += (uint32_t)((w + 7) / 8) * (uint32_t)h;
  }
  return bmpOff;
}

// ========== GLYPH LOOKUP ==========

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
      file.seek(f.indexStart + mid * 24 + 4);
      auto read32 = [&]() -> int32_t {
        uint8_t b[4]; file.read(b, 4);
        return (int32_t)((b[0]<<24)|(b[1]<<16)|(b[2]<<8)|b[3]);
      };
      out->cp      = cp;
      out->h       = (int16_t)read32();
      out->w       = (int16_t)read32();
      out->advance = (int16_t)read32();
      out->x_off   = (int16_t)read32();
      out->y_off   = (int16_t)read32();
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
    } else { i++; }
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

int measureTextVlwF(const String& text, File& fBasic, File& fCJK) {
  int total = 0, i = 0, len = (int)text.length();
  VlwGlyph g;
  while (i < len) {
    uint32_t cp = nextCodepoint(text, i);
    if (fontBasic.loaded && findGlyphInOpenFile(fontBasic, fBasic, cp, &g))
      total += g.advance;
    else if (fontCJK.loaded && findGlyphInOpenFile(fontCJK, fCJK, cp, &g))
      total += g.advance;
    else
      total += fontBasic.size / 2;
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
      if (text[i] == ' ') lastSpaceI = i;
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

void printBitmap(uint8_t *bitmap, int width, int height,
                 int chunkSize = 200, int delayMs = 10) {
  if(!printerConnected || !bitmap) return;
  int widthBytes = width / 8;
  int totalData  = widthBytes * height;
  uint8_t* packet = (uint8_t*)malloc(8 + totalData);
  if (!packet) { logMsg("printBitmap malloc failed"); return; }
  packet[0] = 0x1D; packet[1] = 0x76; packet[2] = 0x30; packet[3] = 0x00;
  packet[4] = (uint8_t)(widthBytes & 0xFF);
  packet[5] = (uint8_t)(widthBytes >> 8);
  packet[6] = (uint8_t)(height & 0xFF);
  packet[7] = (uint8_t)(height >> 8);
  memcpy(packet + 8, bitmap, totalData);
  for(int i = 0; i < 8 + totalData; i += chunkSize) {
    int sz = min(chunkSize, 8 + totalData - i);
    pWriteCharacteristic->writeValue(&packet[i], sz);
    delay(delayMs);
  }
  free(packet);
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
  logMsg("drawGlyph cp=" + String(g->cp) + " x=" + String(x) +
         " baseline=" + String(baseline_y) + " yoff=" + String(g->y_off) +
         " py0=" + String(baseline_y + g->y_off));
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

  const int vlwSize     = (fontBasic.loaded ? fontBasic.size : 16);
  const int renderW     = PRINTER_WIDTH / printScale;
  const int maxTextW    = renderW - 8;
  const int lineSpacing = 3;
  const int descender   = 8;
  const int lineHeight  = vlwSize + lineSpacing + descender;
  const int baseline    = vlwSize;

  File fBasicHandle, fCJKHandle;
  if (fontBasic.loaded && fontBasic.path) {
    logMsg("pre-open free=" + String(ESP.getFreeHeap()));
    fBasicHandle = LittleFS.open(fontBasic.path, "r");
    logMsg("post-open basic free=" + String(ESP.getFreeHeap()) + " handle=" + (fBasicHandle ? "ok" : "fail"));
  }
  if (fontCJK.loaded   && fontCJK.path) {
    fCJKHandle = LittleFS.open(fontCJK.path, "r");
    logMsg("post-open cjk free=" + String(ESP.getFreeHeap()) + " handle=" + (fCJKHandle ? "ok" : "fail"));
  }

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
      if (ESP.getMaxAllocHeap() < 12000) {
        logMsg("SKIP chunk: heap too low maxAlloc=" + String(ESP.getMaxAllocHeap()));
        currentLineIndex += chunkLineCount;
        continue;
      }
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
                if (srcRow[sb] & (1 << sbit))
                  dstRow[dstBit / 8] |= (1 << (7 - (dstBit % 8)));
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
  if (dt > 1000) logMsg("printToThermal took " + String(dt) + " ms");
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

void connectTwitch() {
  if (ESP.getMaxAllocHeap() < 20000) {
    logMsg("Twitch connect SKIPPED: heap too low (maxAlloc=" + String(ESP.getMaxAllocHeap()) + ")");
    twitchConnected = false;
    return;
  }
  logMsg("Connecting Twitch... free=" + String(ESP.getFreeHeap()) + " maxAlloc=" + String(ESP.getMaxAllocHeap()));
  twitchClient.setInsecure();
  if(twitchClient.connect("irc.chat.twitch.tv", 6697)) {
    twitchClient.println("PASS " TWITCH_OAUTH_SECRET);
    twitchClient.println("NICK " TWITCH_OAUTH_NICK);
    twitchClient.println("CAP REQ :twitch.tv/tags twitch.tv/commands");
    twitchClient.println("JOIN #" TWITCH_CHANNEL);
    twitchConnected     = true;
    twitchEverConnected = true;
    lastTwitchPing      = millis();
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
    logMsg("Twitch lost — retry in 10s");
  }
}

// ========== BLE ==========

static void bleDeinit() {
  pClient = nullptr;
  pWriteCharacteristic = nullptr;
  BLEDevice::deinit(true);
  logMsg("BLE deinit. free=" + String(ESP.getFreeHeap()) + " maxAlloc=" + String(ESP.getMaxAllocHeap()));
}

class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* p)    { logMsg("BLE onConnect fired"); }
  void onDisconnect(BLEClient* p) {
    printerConnected = false;
    pWriteCharacteristic = nullptr;
    bleState = BLE_IDLE;
    logMsg("BLE Disconnected — retry in 15s");
  }
};

void connectPrinter() {
  if (ESP.getFreeHeap() < 35000 || ESP.getMaxAllocHeap() < 35000) {
    logMsg("BLE SKIPPED: heap too low (free=" + String(ESP.getFreeHeap()) +
           " maxAlloc=" + String(ESP.getMaxAllocHeap()) + ")");
    return;
  }
  logMsg("BLE connecting: " + printerMAC);
  BLEDevice::init("ESP32-C3-Printer");
  if(pClient) { delete pClient; pClient = nullptr; }
  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());
  bleState = BLE_CONNECTING;
  pClient->connect(BLEAddress(printerMAC.c_str()), false);
}

void disconnectPrinter() {
  if(pClient && printerConnected) pClient->disconnect();
  printerConnected = false;
  bleState = BLE_IDLE;
}

// ========== CONFIG ==========

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

// handleRoot uses send_P — reads htmlPage directly from flash (PROGMEM),
// never allocates the 11KB string into heap.
void handleRoot() {
  server.send_P(200, "text/html; charset=UTF-8", htmlPage);
}

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
  uint8_t ps  = server.hasArg("sz")  ? (uint8_t)server.arg("sz").toInt()  : SCALE_MEDIUM;
  int  al     = server.hasArg("al")  ? server.arg("al").toInt()            : 1;
  bool b      = server.hasArg("b")   ? (server.arg("b")  =="1")            : true;
  bool inv    = server.hasArg("inv") ? (server.arg("inv")=="1")            : false;
  printToThermal(text, ps, al, b, inv, 3);
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
  tCfg.enabled = true;
  tCfg.feed    = server.arg("f").toInt();
  String type  = server.arg("type");
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

// ========== DEBUG HANDLERS ==========

void handleDbgRaw() {
  if(!printerConnected) { server.send(400,"text/plain","Not connected"); return; }
  String pattern = server.arg("pattern");
  int w = server.arg("w").toInt();
  int h = server.arg("h").toInt();
  if (w < 1 || h < 1) { server.send(400,"text/plain","Invalid w/h"); return; }
  int wb = (w + 7) / 8;
  uint8_t* bmp = (uint8_t*)malloc(wb * h);
  if(!bmp) { server.send(500,"text/plain","malloc failed"); return; }
  if (pattern == "checker") {
    for (int y=0; y<h; y++) for (int x=0; x<w; x++)
      if ((x/8 + y/8) % 2 == 0) bmp[y*wb + x/8] |= (1 << (7 - x%8));
  } else if (pattern == "solid") {
    memset(bmp, 0xFF, wb * h);
  } else {
    for (int i=0; i<wb*h; i++) bmp[i] = 0xAA;
  }
  printBitmap(bmp, w, h);
  free(bmp);
  feedPaper(3);
  logMsg("dbg_raw: pattern=" + pattern + " w=" + String(w) + " h=" + String(h));
  server.send(200,"text/plain","OK");
}

void handleDbgGlyph() {
  if(!printerConnected) { server.send(400,"text/plain","Not connected"); return; }
  uint32_t cp = (uint32_t)server.arg("cp").toInt();
  if (cp == 0) cp = 65;
  File f;
  VlwFont* fp = nullptr;
  if (fontBasic.loaded && fontBasic.path) { f = LittleFS.open(fontBasic.path, "r"); fp = &fontBasic; }
  if (!f && fontCJK.loaded && fontCJK.path) { f = LittleFS.open(fontCJK.path, "r"); fp = &fontCJK; }
  if (!f) { server.send(400,"text/plain","No font file"); return; }
  VlwGlyph g;
  String res;
  if (findGlyphInOpenFile(*fp, f, cp, &g)) {
    int w = 20 + g.w + g.x_off;
    int h = 5 + fp->size + 8;
    PrintCanvas canvas(w, h);
    if(!canvas.buffer) { res = "Canvas alloc failed"; }
    else {
      int bl = 5 + fp->size;
      drawGlyph(canvas, f, &g, 10, bl, false);
      int wb = (w + 7) / 8;
      printBitmap(canvas.buffer, w, h);
      res = "cp=" + String(g.cp) + " w=" + String(g.w) + " h=" + String(g.h) +
            " adv=" + String(g.advance) + " x_off=" + String(g.x_off) +
            " y_off=" + String(g.y_off) + " bitmapOffset=" + String(g.bitmapOffset);
    }
  } else {
    res = "Glyph not found for cp=" + String(cp);
  }
  f.close();
  feedPaper(3);
  logMsg("dbg_glyph: " + res);
  server.send(200,"text/plain", res);
}

void handleDbgLine() {
  if(!printerConnected) { server.send(400,"text/plain","Not connected"); return; }
  String text = server.arg("text");
  int scale = server.arg("scale").toInt();
  if (scale < 1) scale = 1;
  if (scale > 3) scale = 3;
  unsigned long t0 = millis();
  text = processNewlines(text);
  const int vlwSize = (fontBasic.loaded ? fontBasic.size : 16);
  const int renderW = PRINTER_WIDTH / scale;
  const int maxTextW = renderW - 8;
  const int lineSpacing = 3;
  const int descender = 8;
  const int lineHeight = vlwSize + lineSpacing + descender;
  const int baseline = vlwSize;
  File fBasicHandle, fCJKHandle;
  if (fontBasic.loaded && fontBasic.path) fBasicHandle = LittleFS.open(fontBasic.path, "r");
  if (fontCJK.loaded && fontCJK.path) fCJKHandle = LittleFS.open(fontCJK.path, "r");
  String wrapped = wordWrapF(text, maxTextW, fBasicHandle, fCJKHandle);
  unsigned long t1 = millis();
  int totalLines = 1;
  for(int i=0;i<(int)wrapped.length();i++) if(wrapped[i]=='\n') totalLines++;
  int chunkHeight = lineHeight * totalLines + lineSpacing * 4;
  PrintCanvas canvas(renderW, chunkHeight);
  unsigned long t2 = millis();
  if(!canvas.buffer) { server.send(500,"text/plain","Canvas alloc failed"); return; }
  int drawY = lineSpacing + baseline;
  int textIndex = 0;
  for(int li=0; li<totalLines; li++) {
    int lineEnd = wrapped.indexOf('\n', textIndex);
    if(lineEnd < 0) lineEnd = (int)wrapped.length();
    String lineStr = wrapped.substring(textIndex, lineEnd);
    if(lineStr.length() > 0) {
      int tw = min(measureTextVlwF(lineStr, fBasicHandle, fCJKHandle), renderW - 4);
      int x = max(2, (renderW - tw) / 2);
      int ci = 0, clen = (int)lineStr.length();
      while (ci < clen) {
        uint32_t cp = nextCodepoint(lineStr, ci);
        VlwGlyph glyphOut;
        if (fBasicHandle && fontBasic.loaded &&
            findGlyphInOpenFile(fontBasic, fBasicHandle, cp, &glyphOut))
          x += drawGlyph(canvas, fBasicHandle, &glyphOut, x, drawY, false);
        else if (fCJKHandle && fontCJK.loaded &&
            findGlyphInOpenFile(fontCJK, fCJKHandle, cp, &glyphOut))
          x += drawGlyph(canvas, fCJKHandle, &glyphOut, x, drawY, false);
        else
          x += vlwSize / 2;
        if (x >= renderW - 2) break;
      }
    }
    drawY += lineHeight;
    textIndex = lineEnd + 1;
  }
  unsigned long t3 = millis();
  int scaledH = chunkHeight * scale;
  int scaledWBytes = PRINTER_WIDTH / 8;
  int renderWBytes = renderW / 8;
  uint8_t* scaled = (uint8_t*)malloc(scaledWBytes * scaledH);
  unsigned long t4 = millis();
  if (scaled) {
    memset(scaled, 0, scaledWBytes * scaledH);
    for (int row=0; row<chunkHeight; row++) {
      const uint8_t* srcRow = canvas.buffer + row * renderWBytes;
      for (int rep=0; rep<scale; rep++) {
        uint8_t* dstRow = scaled + (row*scale + rep) * scaledWBytes;
        for (int dstBit=0; dstBit<PRINTER_WIDTH; dstBit++) {
          int srcBit = dstBit / scale;
          if (srcBit < renderW) {
            int sb = srcBit / 8, sbit = 7 - (srcBit % 8);
            if (srcRow[sb] & (1 << sbit))
              dstRow[dstBit / 8] |= (1 << (7 - (dstBit % 8)));
          }
        }
      }
    }
    printBitmap(scaled, PRINTER_WIDTH, scaledH);
    free(scaled);
  }
  unsigned long t5 = millis();
  if (fBasicHandle) fBasicHandle.close();
  if (fCJKHandle) fCJKHandle.close();
  feedPaper(3);
  char logline[200];
  snprintf(logline, sizeof(logline),
    "dbg_line: wrap=%lums canvas=%lums render=%lums scale=%lums print=%lums heap=%u maxAlloc=%u",
    t1-t0, t2-t1, t3-t2, t4-t3, t5-t4, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  logMsg(logline);
  server.send(200,"text/plain",logline);
}

void handleDbgChunk() {
  if(!printerConnected) { server.send(400,"text/plain","Not connected"); return; }
  int bytes = server.arg("bytes").toInt();
  int delayMs = server.arg("delayms").toInt();
  if (bytes < 32) bytes = 200;
  if (delayMs < 0) delayMs = 10;
  int h = 64;
  int wb = PRINTER_WIDTH / 8;
  uint8_t* bmp = (uint8_t*)malloc(wb * h);
  if(!bmp) { server.send(500,"text/plain","malloc failed"); return; }
  for (int y=0; y<h; y++) for (int x=0; x<wb; x++)
    bmp[y*wb + x] = (y < h/2) ? 0xAA : 0x55;
  unsigned long t0 = millis();
  printBitmap(bmp, PRINTER_WIDTH, h, bytes, delayMs);
  unsigned long dt = millis() - t0;
  free(bmp);
  feedPaper(3);
  char logline[128];
  snprintf(logline, sizeof(logline), "dbg_chunk: chunkSize=%d delay=%dms %lums", bytes, delayMs, dt);
  logMsg(logline);
  server.send(200,"text/plain",logline);
}

void handleDbgCmd() {
  if(!printerConnected) { server.send(400,"text/plain","Not connected"); return; }
  String hex = server.arg("hex");
  hex.replace(" ", ""); hex.replace("\n", ""); hex.replace("\r", "");
  if (hex.length() % 2 != 0) { server.send(400,"text/plain","Hex must have even length"); return; }
  int len = hex.length() / 2;
  uint8_t* buf = (uint8_t*)malloc(len);
  if(!buf) { server.send(500,"text/plain","malloc failed"); return; }
  for (int i=0; i<len; i++) {
    char hc = hex[i*2], lc = hex[i*2+1];
    auto h2d = [](char c) -> uint8_t {
      if (c>='0'&&c<='9') return c-'0';
      if (c>='A'&&c<='F') return c-'A'+10;
      if (c>='a'&&c<='f') return c-'a'+10;
      return 0;
    };
    buf[i] = (h2d(hc) << 4) | h2d(lc);
  }
  for(int i=0; i<len; i+=200) {
    int sz = min(200, len-i);
    pWriteCharacteristic->writeValue(&buf[i], sz);
    delay(10);
  }
  free(buf);
  logMsg("dbg_cmd: " + String(len) + " bytes");
  server.send(200,"text/plain","Sent " + String(len) + " bytes");
}

void handleDbgHeap() {
  String json = "{";
  json += "\"free\":" + String(ESP.getFreeHeap()) + ",";
  json += "\"maxAlloc\":" + String(ESP.getMaxAllocHeap()) + ",";
  json += "\"minFree\":" + String(minFreeHeapSeen) + ",";
  json += "\"bleState\":" + String((int)bleState) + ",";
  json += "\"printer\":" + String(printerConnected?"true":"false") + ",";
  json += "\"twitch\":" + String(twitchConnected?"true":"false") + ",";
  json += "\"uptime\":" + String(millis()/1000);
  json += "}";
  server.send(200, "application/json", json);
}

void handleDbgWifi() {
  String json = "{";
  json += "\"status\":" + String(WiFi.status()) + ",";
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"reconnectCount\":" + String(wifiReconnectCount) + ",";
  json += "\"lastDisconnectReason\":" + String(lastDisconnectReason) + ",";
  json += "\"lastDisconnectTime\":" + String(lastDisconnectTime) + ",";
  json += "\"ssid\":\"" + WiFi.SSID() + "\",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

const char debugPage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en"><head><title>Debug — C3 Printer</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:sans-serif;background:#0f0f23;color:#e0e0e0;padding:10px;max-width:800px;margin:0 auto}
h1{font-size:18px;color:#f87171;padding:10px 0 6px;text-align:center;letter-spacing:1px}
h2{font-size:13px;color:#fbbf24;border-bottom:1px solid #3d3d6b;padding-bottom:5px;margin-bottom:8px;text-transform:uppercase}
.card{background:#1a1a2e;border:1px solid #3d3d6b;border-radius:8px;padding:10px;margin-bottom:10px}
.stat{padding:2px 8px;border-radius:10px;font-size:11px;font-weight:bold;display:inline-block;margin-right:4px}
.ok{background:#1a3a2a;color:#4ade80;border:1px solid #166534}
.err{background:#3a1a1a;color:#f87171;border:1px solid #7f1d1d}
.warn{background:#3a3a1a;color:#facc15;border:1px solid #7f7f1d}
#status{display:flex;gap:6px;flex-wrap:wrap;margin-bottom:6px;font-size:11px}
#status span{background:#0f0f23;padding:3px 8px;border-radius:4px;border:1px solid #2d2d5a}
#console{border:1px solid #2d2d5a;background:#0a0a1a;color:#4ade80;font-family:monospace;font-size:11px;padding:6px;height:200px;overflow-y:auto;white-space:pre-wrap;border-radius:4px;margin-bottom:6px}
label{display:block;font-size:11px;color:#9ca3af;margin-top:4px}
input[type=text],input[type=number],select{background:#0f0f23;color:#e0e0e0;border:1px solid #3d3d6b;border-radius:3px;padding:3px 5px;font-size:12px}
input[type=number]{width:60px}
.row{display:flex;gap:6px;align-items:end;flex-wrap:wrap;margin-bottom:4px}
.row label{font-size:10px;margin-top:0}
button{padding:6px 12px;border:none;border-radius:4px;cursor:pointer;font-size:12px;font-weight:bold;color:#fff}
button.go{background:#6d28d9}
button.go2{background:#0e7490}
.result{background:#0f0f23;border:1px solid #2d2d5a;border-radius:3px;padding:4px 6px;font-family:monospace;font-size:11px;margin-top:4px;white-space:pre-wrap;word-break:break-all}
a{color:#a78bfa;font-size:11px;text-decoration:none}
a:hover{text-decoration:underline}
</style></head>
<body>
<h1>&#128295; Debug Console</h1>

<div class="card">
  <h2>Live Status</h2>
  <div id="status">Loading...</div>
</div>

<div class="card">
  <h2>Console Log</h2>
  <div id="console">Loading...</div>
</div>

<div class="card">
  <h2>Test Raw Bitmap</h2>
  <div class="row">
    <label>Pattern: <select id="raw_pat"><option value="stripe">Stripe</option><option value="checker">Checker</option><option value="solid">Solid</option></select></label>
    <label>Width: <input type="number" id="raw_w" value="400" min="1" max="576"></label>
    <label>Height: <input type="number" id="raw_h" value="16" min="1" max="128"></label>
    <button class="go" onclick="dbgRaw()">Send</button>
  </div>
  <div id="raw_res" class="result"></div>
</div>

<div class="card">
  <h2>Test Single Glyph</h2>
  <div class="row">
    <label>Codepoint: <input type="number" id="gly_cp" value="65" min="32" max="65535"></label>
    <button class="go" onclick="dbgGlyph()">Render</button>
  </div>
  <div id="gly_res" class="result"></div>
</div>

<div class="card">
  <h2>Test One Line</h2>
  <div class="row">
    <label>Text: <input type="text" id="line_txt" value="Hello World!" style="width:200px"></label>
    <label>Scale: <select id="line_sc"><option value="1">1</option><option value="2" selected>2</option><option value="3">3</option></select></label>
    <button class="go" onclick="dbgLine()">Print</button>
  </div>
  <div id="line_res" class="result"></div>
</div>

<div class="card">
  <h2>BLE Chunk Test</h2>
  <div class="row">
    <label>Chunk bytes: <input type="number" id="chk_bytes" value="200" min="32" max="512"></label>
    <label>Delay ms: <input type="number" id="chk_delay" value="10" min="0" max="200"></label>
    <button class="go" onclick="dbgChunk()">Send</button>
  </div>
  <div id="chk_res" class="result"></div>
</div>

<div class="card">
  <h2>Raw ESC/POS Command</h2>
  <div class="row">
    <label>Hex bytes: <input type="text" id="cmd_hex" value="1B40" style="width:300px;font-family:monospace" placeholder="e.g. 1B40 or 1D76300002000800FF"></label>
    <button class="go" onclick="dbgCmd()">Send</button>
  </div>
  <div id="cmd_res" class="result"></div>
</div>

<div class="card">
  <h2>Heap &amp; WiFi</h2>
  <div class="row">
    <button class="go2" onclick="dbgHeap()">Refresh Heap</button>
    <button class="go2" onclick="dbgWifi()">Refresh WiFi</button>
  </div>
  <div id="heap_res" class="result"></div>
  <div id="wifi_res" class="result"></div>
</div>

<div style="text-align:center;margin-top:10px">
  <a href="/">&larr; Main Page</a>
</div>

<script>
async function q(url) {
  try { return await (await fetch(url)).text(); } catch(e) { return 'Error: '+e; }
}
function statusHtml(d) {
  let h = '<span>Printer: <b class="stat '+(d.printer?'ok':'err')+'">'+(d.printer?'Connected':'Offline')+'</b></span>';
  h += '<span>Twitch: <b class="stat '+(d.twitch?'ok':'err')+'">'+(d.twitch?'Connected':'Offline')+'</b></span>';
  if(d.free !== undefined) {
    h += '<span>Heap free: '+d.free+'</span>';
    h += '<span>MaxAlloc: '+d.maxAlloc+'</span>';
    h += '<span>MinFree: '+d.minFree+'</span>';
    h += '<span>Uptime: '+d.uptime+'s</span>';
  }
  return h;
}
async function refreshStatus() {
  try {
    let s = await (await fetch('/s')).json();
    let h = await (await fetch('/dbg_heap')).json();
    Object.assign(s, h);
    document.getElementById('status').innerHTML = statusHtml(s);
  } catch(e) {}
}
async function refreshConsole() {
  let c = document.getElementById('console');
  try {
    let t = await (await fetch('/console')).text();
    c.textContent = t;
    c.scrollTop = c.scrollHeight;
  } catch(e) {}
}
async function dbgRaw() {
  let p = document.getElementById('raw_pat').value;
  let w = document.getElementById('raw_w').value;
  let h = document.getElementById('raw_h').value;
  document.getElementById('raw_res').textContent = await q('/dbg_raw?pattern='+p+'&w='+w+'&h='+h);
}
async function dbgGlyph() {
  let cp = document.getElementById('gly_cp').value;
  document.getElementById('gly_res').textContent = await q('/dbg_glyph?cp='+cp);
}
async function dbgLine() {
  let txt = document.getElementById('line_txt').value;
  let sc = document.getElementById('line_sc').value;
  document.getElementById('line_res').textContent = await q('/dbg_line?text='+encodeURIComponent(txt)+'&scale='+sc);
}
async function dbgChunk() {
  let b = document.getElementById('chk_bytes').value;
  let d = document.getElementById('chk_delay').value;
  document.getElementById('chk_res').textContent = await q('/dbg_chunk?bytes='+b+'&delayms='+d);
}
async function dbgCmd() {
  let h = document.getElementById('cmd_hex').value.replace(/[^0-9a-fA-F]/g,'');
  document.getElementById('cmd_res').textContent = await q('/dbg_cmd?hex='+h);
}
async function dbgHeap() {
  document.getElementById('heap_res').textContent = await q('/dbg_heap');
}
async function dbgWifi() {
  document.getElementById('wifi_res').textContent = await q('/dbg_wifi');
}
setInterval(refreshStatus, 2000);
setInterval(refreshConsole, 1000);
refreshStatus(); refreshConsole();
</script></body></html>
)rawliteral";

// ========== FILE UPLOAD ==========

void handleUpload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    // Tear down Twitch TLS — frees ~20KB of heap needed for LittleFS writes
    if (twitchConnected || twitchClient.connected()) {
      logMsg("Disconnecting Twitch before upload  pre-stop free=" + String(ESP.getFreeHeap()));
      twitchClient.stop();
      delay(10);
      logMsg("post-stop free=" + String(ESP.getFreeHeap()));
      twitchConnected = false;
      delay(100);
    }
    if (printerConnected || bleState == BLE_CONNECTING || bleState == BLE_DISCOVERING) {
      logMsg("Disconnecting BLE before upload");
      disconnectPrinter(); delay(200);
    }
    logMsg("Upload start: " + upload.filename +
           " free=" + String(ESP.getFreeHeap()) +
           " maxAlloc=" + String(ESP.getMaxAllocHeap()));
    if (ESP.getMaxAllocHeap() < 20000) {
      logMsg("REJECTED: maxAlloc too low — reboot first");
      uploadFailed = true; return;
    }
    String filename = "/" + upload.filename;
    size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
    size_t expected  = server.header("Content-Length").toInt();
    if (expected > 0 && expected > freeBytes) {
      logMsg("REJECTED: need " + String(expected) + " bytes, only " + String(freeBytes) + " free");
      uploadFailed = true; return;
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
        logMsg("WRITE MISMATCH: " + String(upload.currentSize) + " vs " + String(written));
        uploadFailed = true;
      }
    }
    yield();
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) uploadFile.close();
    uploadInProgress = false;
    lastUploadFinish = millis();
    logMsg("Upload end: " + String(uploadWrittenBytes) + " bytes");
    if (uploadWrittenBytes == 0) { uploadFailed = true; logMsg("Upload wrote 0 bytes"); }
    // Reload font metadata if a VLW was just uploaded
    if (upload.filename == "unifont_basic.vlw") loadVlw(fontBasic, "/unifont_basic.vlw");
    if (upload.filename == "unifont_cjk.vlw")   loadVlw(fontCJK,   "/unifont_cjk.vlw");
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    logMsg("Upload ABORTED");
    uploadFailed = true; uploadInProgress = false;
    if (uploadFile) uploadFile.close();
  }
}

void handleUploadComplete() {
  if (uploadFailed)
    server.send(500, "text/plain", "Upload FAILED — wrote " + String(uploadWrittenBytes) + " bytes");
  else
    server.send(200, "text/plain", "OK: " + String(uploadWrittenBytes) + " bytes");
}

// ========== SETUP & LOOP ==========

void setup() {
  Serial.begin(115200);
  delay(1000);
  logMsg("Boot. free=" + String(ESP.getFreeHeap()) + " maxAlloc=" + String(ESP.getMaxAllocHeap()));

  // maxOpenFiles=4: reduces LittleFS internal heap cost by ~2.4KB vs
  // the default of 10. We need at most 3 simultaneous file handles
  // (2 font files during render + 1 upload file), so 4 is safe.
  LittleFS.begin(true, "/littlefs", 4);
  loadVlw(fontBasic, "/unifont_basic.vlw");
  loadVlw(fontCJK,   "/unifont_cjk.vlw");
  LittleFS.end();
  // Diagnostic: verify glyph 'A' can be read from the font file
  LittleFS.begin(false, "/littlefs", 4);
  {
    File df = LittleFS.open(fontBasic.path, "r");
    VlwGlyph dg;
    if (df && findGlyphInOpenFile(fontBasic, df, 'A', &dg))
      logMsg("GlyphA w=" + String(dg.w) + " h=" + String(dg.h) +
             " adv=" + String(dg.advance) + " off=" + String(dg.bitmapOffset));
    else
      logMsg("GlyphA NOT FOUND");
    if (df) df.close();
  }
  logMsg("After VLW. free=" + String(ESP.getFreeHeap()) + " maxAlloc=" + String(ESP.getMaxAllocHeap()));

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostname);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.begin(MYSSID, MYPSK);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      lastDisconnectReason = info.wifi_sta_disconnected.reason;
      lastDisconnectTime = millis();
      wifiReconnectCount++;
    }
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  while(WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  logMsg("WiFi OK. free=" + String(ESP.getFreeHeap()) + " maxAlloc=" + String(ESP.getMaxAllocHeap()));
  if(MDNS.begin(hostname)) { MDNS.addService("http","tcp",80); logMsg("mDNS OK. free=" + String(ESP.getFreeHeap()) + " maxAlloc=" + String(ESP.getMaxAllocHeap())); }
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
  server.on("/fsinfo", []() {
    size_t total = LittleFS.totalBytes(), used = LittleFS.usedBytes();
    String out = "<h2>LittleFS Info</h2>";
    out += "<p>Total: "+String(total)+" used: "+String(used)+" free: "+String(total-used)+"</p><ul>";
    File root = LittleFS.open("/"); File f = root.openNextFile(); bool any=false;
    while (f) {
      any = true;
      String name = String(f.name());
      out += "<li>"+name+" "+String(f.size())+"B "
             "<a href=\"/delete_file?name="+name+"\" onclick=\"return confirm('Delete?')\" style=\"color:#f87171\">[x]</a></li>";
      f = root.openNextFile();
    }
    if (!any) out += "<li>No files</li>";
    out += "</ul><p><a href=\"/\">&larr; Back</a></p>";
    server.send(200, "text/html", out);
  });
  server.on("/delete_file", []() {
    String name = server.arg("name");
    if (!name.startsWith("/")) name = "/"+name;
    if (!LittleFS.exists(name)) { server.send(404,"text/plain","Not found"); return; }
    LittleFS.remove(name);
    server.send(200,"text/html","<p>Deleted: "+name+"</p><a href=\"/fsinfo\">&larr; Back</a>");
  });
  server.on("/console", []() {
    String out;
    if (logWrapped) { out = String(logBuffer + logHead) + String(logBuffer, logHead); }
    else            { out = String(logBuffer, logHead); }
    server.send(200, "text/plain", out);
  });
  server.on("/debug", []() { server.send_P(200, "text/html; charset=UTF-8", debugPage); });
  server.on("/dbg_raw",   handleDbgRaw);
  server.on("/dbg_glyph", handleDbgGlyph);
  server.on("/dbg_line",  handleDbgLine);
  server.on("/dbg_chunk", handleDbgChunk);
  server.on("/dbg_cmd",   handleDbgCmd);
  server.on("/dbg_heap",  handleDbgHeap);
  server.on("/dbg_wifi",  handleDbgWifi);
  server.on("/ping", []() { server.send(200,"text/plain","pong"); });
  server.on("/log", []() {
    server.send_P(200, "text/html", R"rawliteral(
<!DOCTYPE html><html><head><title>Console</title>
<style>body{background:#0f0f23;color:#4ade80;font-family:monospace;font-size:12px;padding:10px;white-space:pre-wrap}</style>
</head><body><div id="l">Loading...</div>
<script>async function t(){const r=await fetch('/console');document.getElementById('l').textContent=await r.text();scrollTo(0,document.body.scrollHeight);}setInterval(t,1000);t();</script>
</body></html>)rawliteral");
  });
  server.begin();

  bleDeadlineMs = millis() + 45000;
  logMsg("Ready. BLE first; Twitch fallback at 45s.");
}

void loop() {
  static unsigned long lastHeapLog      = 0;
  static unsigned long lastPrinterRetry = 0;
  static unsigned long lastTwitchRetry  = 0;
  static unsigned long connectStart     = 0;
  static unsigned long discoverStart    = 0;
  unsigned long now = millis();

  {
    unsigned long f = ESP.getFreeHeap();
    if (f < minFreeHeapSeen) minFreeHeapSeen = f;
  }

  if (now - lastHeapLog > 10000) {
    lastHeapLog = now;
    char hb[128];
    snprintf(hb, sizeof(hb), "Uptime %lus free=%u maxAlloc=%u minFree=%u RSSI=%d bleState=%d twitch=%d",
             now/1000, ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
             (unsigned int)minFreeHeapSeen, WiFi.RSSI(), (int)bleState, (int)twitchConnected);
    logMsg(hb);
  }

  static wl_status_t lastWifi = WL_CONNECTED;
  wl_status_t wifiNow = WiFi.status();
  if (wifiNow != lastWifi) {
    lastWifi = wifiNow;
    if (wifiNow == WL_CONNECTED) logMsg("WiFi reconnected: " + WiFi.localIP().toString());
    else logMsg("WiFi lost: " + String(wifiNow));
  }

  ArduinoOTA.handle();
  if(shouldSaveConfig) { saveConfig(); shouldSaveConfig = false; }
  server.handleClient();

  if (!uploadInProgress) {

    // ---- Twitch ----
    if (twitchConnected) {
      handleTwitchIRC();
    } else if (twitchEverConnected && now - lastTwitchRetry > 10000) {
      lastTwitchRetry = now;
      connectTwitch();
    } else if (!twitchEverConnected && now >= bleDeadlineMs && now - lastTwitchRetry > 10000) {
      logMsg("BLE deadline — connecting Twitch unconditionally");
      lastTwitchRetry = now;
      connectTwitch();
    }

    // ---- BLE idle retry ----
    if (bleState == BLE_IDLE && !printerConnected &&
        now - lastPrinterRetry > 15000 &&
        now - lastUploadFinish > 10000) {
      lastPrinterRetry = now; connectStart = 0;
      connectPrinter();
    }

    // ---- BLE_CONNECTING ----
    if (bleState == BLE_CONNECTING && pClient) {
      if (connectStart == 0) connectStart = now;
      if (pClient->isConnected()) {
        logMsg("BLE link up");
        connectStart = 0; bleState = BLE_DISCOVERING; discoverStart = 0;
      } else if (now - connectStart > 15000) {
        logMsg("BLE connect timeout");
        bleDeinit(); connectStart = 0; bleState = BLE_FAILED;
      }
    }

    // ---- BLE_DISCOVERING ----
    if (bleState == BLE_DISCOVERING && pClient) {
      if (discoverStart == 0) discoverStart = now;
      BLERemoteService* svc = pClient->getService(serviceUUID);
      if (svc) {
        pWriteCharacteristic = svc->getCharacteristic(charWriteUUID);
        if (pWriteCharacteristic) {
          discoverStart = 0; bleState = BLE_INITING;
        } else {
          logMsg("BLE char not found");
          bleDeinit(); discoverStart = 0; bleState = BLE_FAILED;
        }
      } else if (now - discoverStart > 10000) {
        logMsg("BLE discover timeout");
        bleDeinit(); discoverStart = 0; bleState = BLE_FAILED;
      }
    }

    // ---- BLE_INITING ----
    if (bleState == BLE_INITING) {
      printerConnected = true;
      uint8_t wake[] = {0x00,0x00,0x00,0x00,0x00};
      pWriteCharacteristic->writeValue(wake, 5);
      uint8_t init[] = {0x1B, 0x40};
      pWriteCharacteristic->writeValue(init, 2);
      bleState = BLE_READY;
      logMsg("Printer ready. free=" + String(ESP.getFreeHeap()) + " maxAlloc=" + String(ESP.getMaxAllocHeap()));
      if (!twitchEverConnected) {
        logMsg("BLE ready — first Twitch connect");
        connectTwitch(); lastTwitchRetry = now;
      }
    }

    // ---- BLE_FAILED ----
    if (bleState == BLE_FAILED) {
      if (!twitchEverConnected) {
        logMsg("BLE failed — Twitch fallback");
        connectTwitch(); lastTwitchRetry = now;
      }
      bleState = BLE_IDLE;
    }
  }
  delay(10);
}
