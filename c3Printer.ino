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
#include <U8g2_for_Adafruit_GFX.h>
#include <Secrets.h>

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
unsigned long lastTwitchPing = 0;

String pointsRewardFilter = "";
bool shouldSaveConfig = false;

const int PRINTER_WIDTH       = 400;
const int PRINTER_WIDTH_BYTES = PRINTER_WIDTH / 8;

// ========== FONT SYSTEM ==========
// User picks a SIZE (0-4) and bold (true/false) separately.
// Each size has a normal and bold variant for Latin/extended.
// For any character outside Latin range, we automatically fall back
// to the Japanese Unifont which covers Katakana, CJK, Greek, Arabic, etc.

// Size IDs
#define FSIZE_SMALL   0   // ~7px  — tiny labels
#define FSIZE_MEDIUM  1   // ~13px — standard
#define FSIZE_LARGE   2   // ~15px — bigger
#define FSIZE_XLARGE  3   // ~20px — large text
#define FSIZE_HUGE    4   // ~28px — headlines

// Global print scale multiplier. Each rendered bitmap row/column is repeated this
// many times before sending to the printer, giving crisp pixel-scaled output.
// 2 = 2x size (recommended for small thermal printers where even Large feels tiny).
// Adjust to taste: 1 = native, 3 = very large.
#define PRINT_SCALE 2

struct SizeEntry {
  uint8_t     id;
  const char* label;
  const uint8_t* fontNormal;
  const uint8_t* fontBold;
  uint8_t     charH;
  const uint8_t* unicodeFallback;  // per-size Unicode font (Katakana, CJK, etc.)
  uint8_t     fallbackH;           // raw height of the Unicode fallback font
};

// ========== STRUCTS ==========

struct EventConfig {
  bool    enabled = true;
  String  msg[3];
  uint8_t font[3];   // stores FSIZE_* id
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

// charH = native height of the primary font (pre-PRINT_SCALE).
// unicodeFallback is the closest-height Unicode font available in U8g2 for each tier.
// All fonts render at charH * PRINT_SCALE on the actual paper.
const SizeEntry SIZE_TABLE[] = {
  { FSIZE_SMALL,  "Small",   u8g2_font_6x10_tf,       u8g2_font_7x13B_tf,
    10, u8g2_font_unifont_t_japanese1, 16 },
  { FSIZE_MEDIUM, "Medium",  u8g2_font_8x13_tf,        u8g2_font_8x13B_tf,
    13, u8g2_font_unifont_t_japanese1, 16 },
  { FSIZE_LARGE,  "Large",   u8g2_font_9x15_tf,        u8g2_font_9x15B_tf,
    15, u8g2_font_unifont_t_japanese1, 16 },
  { FSIZE_XLARGE, "X-Large", u8g2_font_10x20_tf,       u8g2_font_10x20_tf,
    20, u8g2_font_unifont_t_japanese2, 16 },
  { FSIZE_HUGE,   "Huge",    u8g2_font_logisoso28_tf,  u8g2_font_logisoso28_tf,
    28, u8g2_font_unifont_t_japanese2, 16 },
};
const int SIZE_TABLE_LEN = sizeof(SIZE_TABLE) / sizeof(SIZE_TABLE[0]);

// Global fallback kept for contexts without a SizeEntry (e.g. wordWrap default).
// Per-size fallback fonts are now in SizeEntry.unicodeFallback.
const uint8_t* UNICODE_FALLBACK_FONT = u8g2_font_unifont_t_japanese1;
const uint8_t  UNICODE_FALLBACK_H    = 16;

const SizeEntry* getSizeEntry(uint8_t id) {
  for (int i = 0; i < SIZE_TABLE_LEN; i++)
    if (SIZE_TABLE[i].id == id) return &SIZE_TABLE[i];
  return &SIZE_TABLE[1]; // default Medium
}

void initDefaults() {
  twitchCfg.subs.msg[0] = "NEW SUB:";
  twitchCfg.subs.msg[1] = "{user}!";
  twitchCfg.subs.msg[2] = "";
  for(int i=0;i<3;i++){
    twitchCfg.subs.font[i]=FSIZE_MEDIUM; twitchCfg.subs.align[i]=1;
    twitchCfg.subs.bold[i]=true; twitchCfg.subs.invert[i]=false;
  }
  twitchCfg.bits.msg[0] = "CHEER:";
  twitchCfg.bits.msg[1] = "{user}";
  twitchCfg.bits.msg[2] = "{amount} bits";
  for(int i=0;i<3;i++){
    twitchCfg.bits.font[i]=FSIZE_MEDIUM; twitchCfg.bits.align[i]=1;
    twitchCfg.bits.bold[i]=true; twitchCfg.bits.invert[i]=false;
  }
  twitchCfg.points.msg[0] = "REDEEM:";
  twitchCfg.points.msg[1] = "{user}";
  twitchCfg.points.msg[2] = "{reward}";
  for(int i=0;i<3;i++){
    twitchCfg.points.font[i]=FSIZE_MEDIUM; twitchCfg.points.align[i]=1;
    twitchCfg.points.bold[i]=true; twitchCfg.points.invert[i]=false;
  }
  twitchCfg.raids.msg[0] = "RAID!";
  twitchCfg.raids.msg[1] = "from";
  twitchCfg.raids.msg[2] = "{user}";
  for(int i=0;i<3;i++){
    twitchCfg.raids.font[i]=FSIZE_HUGE; twitchCfg.raids.align[i]=1;
    twitchCfg.raids.bold[i]=false; twitchCfg.raids.invert[i]=false;
  }
}

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

// ========== TEXT PROCESSING ==========

String processNewlines(String text) {
  text.replace("\\n", "\n");
  text.replace("{nl}", "\n");
  return text;
}

// Pass through valid UTF-8 sequences; strip bare control chars
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
      i++; // lone continuation byte, skip
    }
  }
  return result;
}

// Decode one UTF-8 codepoint, advance index
uint32_t nextCodepoint(const String& s, int& i) {
  unsigned char c = (unsigned char)s[i];
  if (c < 0x80)  { i++; return c; }
  if (c < 0xC0)  { i++; return 0xFFFD; } // lone continuation
  if (c < 0xE0)  { uint32_t cp = (c & 0x1F); i++; if(i<(int)s.length()) cp=(cp<<6)|((unsigned char)s[i++]&0x3F); return cp; }
  if (c < 0xF0)  { uint32_t cp = (c & 0x0F); i++; for(int j=0;j<2&&i<(int)s.length();j++) cp=(cp<<6)|((unsigned char)s[i++]&0x3F); return cp; }
  { uint32_t cp = (c & 0x07); i++; for(int j=0;j<3&&i<(int)s.length();j++) cp=(cp<<6)|((unsigned char)s[i++]&0x3F); return cp; }
}

// Returns true if this codepoint is covered by the standard Latin/extended fonts
// (i.e., ISO-8859 / basic Latin + extended Latin)
bool isLatinCodepoint(uint32_t cp) {
  return cp <= 0x024F; // Basic Latin + Latin-1 Supplement + Latin Extended-A/B
}

// Word-wrap using the wider of the two fonts for accurate measurement on mixed text
String wordWrap(const String& text, int maxWidth, const uint8_t* primaryFont, const SizeEntry* se = nullptr) {
  U8G2_FOR_ADAFRUIT_GFX u8m;
  PrintCanvas dummy(8, 8);
  u8m.begin(dummy);

  // Measure a line width accounting for font switching per segment
  auto measureLine = [&](const String& line) -> int {
    int total = 0, i = 0, len = line.length();
    while (i < len) {
      int segStart = i;
      int startPos = i;
      uint32_t cp = nextCodepoint(line, i);
      bool useFallback = !isLatinCodepoint(cp);
      // Collect run of same font type
      while (i < len) {
        int before = i;
        uint32_t cp2 = nextCodepoint(line, i);
        if (!isLatinCodepoint(cp2) != useFallback) { i = before; break; }
      }
      String seg = line.substring(segStart, i);
      u8m.setFont(useFallback ? (se ? se->unicodeFallback : UNICODE_FALLBACK_FONT) : primaryFont);
      total += u8m.getUTF8Width(seg.c_str());
    }
    return total;
  };

  String result = "";
  int lineStart = 0, textLen = (int)text.length();

  while (lineStart < textLen) {
    int lineEnd = text.indexOf('\n', lineStart);
    if (lineEnd < 0) lineEnd = textLen;
    String line = text.substring(lineStart, lineEnd);

    if (measureLine(line) <= maxWidth) {
      result += line;
      if (lineEnd < textLen) result += "\n";
    } else {
      while (line.length() > 0) {
        if (measureLine(line) <= maxWidth) { result += line; break; }
        int lo = 1, hi = line.length(), breakAt = 1;
        while (lo <= hi) {
          int mid = (lo + hi) / 2;
          String test = line.substring(0, mid);
          if (measureLine(test) <= maxWidth) { breakAt = mid; lo = mid + 1; }
          else hi = mid - 1;
        }
        int lastSpace = line.lastIndexOf(' ', breakAt);
        if (lastSpace > 0) breakAt = lastSpace;
        result += line.substring(0, breakAt);
        result += "\n";
        line = line.substring(breakAt);
        if (line.startsWith(" ")) line = line.substring(1);
      }
      if (lineEnd < textLen) result += "\n";
    }
    lineStart = lineEnd + 1;
  }
  return result;
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

// Draw one text line with automatic font fallback per Unicode segment.
// Segments of Latin chars use primaryFont; non-Latin uses se->unicodeFallback.
// PRINT_SCALE is applied via repeated-row rendering in printToThermal.
// Returns the width drawn (in canvas pixels, i.e. pre-scale).
int drawLineMixed(U8G2_FOR_ADAFRUIT_GFX& u8g2, const String& line,
                  int x, int y, int fgColor,
                  const uint8_t* primaryFont, bool bold,
                  const SizeEntry* se = nullptr) {
  int curX = x;
  int i = 0, len = line.length();
  const uint8_t* fallbackFont = se ? se->unicodeFallback : UNICODE_FALLBACK_FONT;
  while (i < len) {
    int segStart = i;
    uint32_t cp = nextCodepoint(line, i);
    bool useFallback = !isLatinCodepoint(cp);
    int segEnd = i;
    while (i < len) {
      int before = i;
      uint32_t cp2 = nextCodepoint(line, i);
      if (!isLatinCodepoint(cp2) != useFallback) { i = before; break; }
      segEnd = i;
    }
    String seg = line.substring(segStart, segEnd > segStart ? segEnd : i);
    const uint8_t* useFont = useFallback ? (se ? se->unicodeFallback : UNICODE_FALLBACK_FONT) : primaryFont;
    u8g2.setFont(useFont);
    u8g2.setForegroundColor(fgColor);
    u8g2.setCursor(curX, y);
    u8g2.print(seg);
    if (bold && !useFallback) { // bold = double-print shifted 1px (Latin only)
      u8g2.setCursor(curX + 1, y);
      u8g2.print(seg);
    }
    curX += u8g2.getUTF8Width(seg.c_str());
  }
  return curX - x;
}

bool printToThermal(String text, uint8_t sizeId, int align, bool bold, bool invert, int feedLines) {
  if(!printerConnected) return false;
  if(text.length() == 0) { if(feedLines > 0) feedPaper(feedLines); return true; }

  text = processNewlines(text);
  const SizeEntry* se = getSizeEntry(sizeId);
  const uint8_t* primaryFont = bold ? se->fontBold : se->fontNormal;

  // Render at 1/PRINT_SCALE width, then scale up by repeating pixels/rows.
  // This gives clean crisp scaling for bitmap fonts (avoids blurry stretching).
  int renderW       = PRINTER_WIDTH / PRINT_SCALE;
  int maxTextWidth  = renderW - 8;
  text = wordWrap(text, maxTextWidth, primaryFont, se);

  int totalLines = 1;
  for(int i = 0; i < (int)text.length(); i++) if(text[i] == '\n') totalLines++;

  // Line height uses the taller of primary vs unicode fallback (pre-scale)
  int charHeight  = max((int)se->charH, (int)se->fallbackH);
  int lineSpacing = max(2, charHeight / 6);
  int lineHeight  = charHeight + lineSpacing;
  int linesPerChunk = max(1, 200 / lineHeight);

  int currentLineIndex = 0, textIndex = 0;
  int fgColor = invert ? 0 : 1;

  while(currentLineIndex < totalLines) {
    int chunkLineCount = 0, chunkHeight = 0;
    if(currentLineIndex == 0) chunkHeight += lineSpacing * 2;
    for(int i = 0; i < linesPerChunk && (currentLineIndex + i) < totalLines; i++) {
      chunkLineCount++; chunkHeight += lineHeight;
    }
    if(currentLineIndex + chunkLineCount >= totalLines) chunkHeight += lineSpacing * 2;

    PrintCanvas canvas(renderW, chunkHeight);
    if(!canvas.buffer) { Serial.println("Chunk alloc failed!"); return false; }

    if(invert) canvas.fillRect(0, 0, renderW, chunkHeight, 1);

    U8G2_FOR_ADAFRUIT_GFX u8g2;
    u8g2.begin(canvas);
    u8g2.setFontMode(1);
    u8g2.setFontDirection(0);
    u8g2.setBackgroundColor(invert ? 1 : 0);

    int drawY = (currentLineIndex == 0) ? lineSpacing + charHeight : charHeight;

    for(int i = 0; i < chunkLineCount; i++) {
      int lineEnd = text.indexOf('\n', textIndex);
      if(lineEnd < 0) lineEnd = text.length();
      String line = text.substring(textIndex, lineEnd);

      if(line.length() > 0) {
        // Measure full line width for alignment (accounting for mixed fonts)
        U8G2_FOR_ADAFRUIT_GFX u8m;
        PrintCanvas dm(8, 8);
        u8m.begin(dm);
        int tw = 0, mi = 0, mlen = line.length();
        while (mi < mlen) {
          int ms = mi;
          uint32_t cp = nextCodepoint(line, mi);
          bool fb = !isLatinCodepoint(cp);
          int me = mi;
          while (mi < mlen) {
            int mb = mi; uint32_t cp2 = nextCodepoint(line, mi);
            if (!isLatinCodepoint(cp2) != fb) { mi = mb; break; }
            me = mi;
          }
          String seg = line.substring(ms, me > ms ? me : mi);
          u8m.setFont(fb ? se->unicodeFallback : primaryFont);
          tw += u8m.getUTF8Width(seg.c_str());
        }

        tw = min(tw, renderW - 4);

        int x = 2;
        if     (align == 1) x = max(2, (renderW - tw) / 2);
        else if(align == 2) x = max(2, renderW - tw - 2);

        drawLineMixed(u8g2, line, x, drawY, fgColor, primaryFont, bold, se);
      }
      drawY    += lineHeight;
      textIndex = lineEnd + 1;
    }

    // Scale up: repeat each row and column PRINT_SCALE times for crisp 2x output
    {
      int scaledH = chunkHeight * PRINT_SCALE;
      int scaledWBytes = PRINTER_WIDTH / 8;
      int renderWBytes = renderW / 8;
      uint8_t* scaled = (uint8_t*)malloc(scaledWBytes * scaledH);
      if (scaled) {
        memset(scaled, 0, scaledWBytes * scaledH);
        for (int row = 0; row < chunkHeight; row++) {
          const uint8_t* srcRow = canvas.buffer + row * renderWBytes;
          for (int rep = 0; rep < PRINT_SCALE; rep++) {
            uint8_t* dstRow = scaled + (row * PRINT_SCALE + rep) * scaledWBytes;
            for (int dstBit = 0; dstBit < PRINTER_WIDTH; dstBit++) {
              int srcBit = dstBit / PRINT_SCALE;
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

  if(feedLines > 0) feedPaper(feedLines);
  return true;
}

void printEvent(EventConfig& cfg, String username, String val1, String val2) {
  if(!cfg.enabled) return;
  Serial.println("Printing Event...");
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
  Serial.println("Connecting to Twitch IRC...");
  twitchClient.setInsecure();
  if(twitchClient.connect("irc.chat.twitch.tv", 6697)) {
    twitchClient.println("PASS " TWITCH_OAUTH_SECRET);
    twitchClient.println("NICK " TWITCH_OAUTH_NICK);
    twitchClient.println("CAP REQ :twitch.tv/tags twitch.tv/commands");
    twitchClient.println("JOIN #" TWITCH_CHANNEL);
    twitchConnected = true;
    lastTwitchPing  = millis();
    Serial.println("Twitch OK");
  } else {
    twitchConnected = false;
    Serial.println("Twitch failed");
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
    Serial.println("Twitch connection lost");
  }
}

// ========== BLE CONNECTION ==========

class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* p)    { Serial.println("BLE Connected"); }
  void onDisconnect(BLEClient* p) { printerConnected = false; Serial.println("BLE Disconnected"); }
};

bool connectPrinter() {
  Serial.println("Connecting: " + printerMAC);
  BLEDevice::init("ESP32-C3-Printer");
  if(pClient) delete pClient;
  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());
  if(!pClient->connect(BLEAddress(printerMAC.c_str()))) return false;
  BLERemoteService* svc = pClient->getService(serviceUUID);
  if(!svc) return false;
  pWriteCharacteristic = svc->getCharacteristic(charWriteUUID);
  if(!pWriteCharacteristic) return false;
  printerConnected = true;
  delay(500);
  uint8_t wake[] = {0x00,0x00,0x00,0x00,0x00};
  pWriteCharacteristic->writeValue(wake, 5); delay(100);
  uint8_t init[] = {0x1B, 0x40};
  pWriteCharacteristic->writeValue(init, 2); delay(100);
  Serial.println("Printer Ready");
  return true;
}

void disconnectPrinter() {
  if(pClient && printerConnected) pClient->disconnect();
  printerConnected = false;
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
  Serial.println("Config loaded");
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
  Serial.println("Config saved");
}

// ========== WEB SERVER ==========

const char* htmlPage = R"rawliteral(
<!DOCTYPE html>
<html lang="en"><head><title>C3 Printer</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:sans-serif;background:#1a1a2e;color:#e0e0e0;padding:10px;max-width:640px;margin:0 auto}
h1{font-size:18px;color:#a78bfa;padding:10px 0 6px;text-align:center;letter-spacing:1px}
.card{background:#16213e;border:1px solid #2d2d5a;border-radius:8px;padding:10px;margin-bottom:10px}
h2{font-size:13px;color:#7c86c9;border-bottom:1px solid #2d2d5a;padding-bottom:5px;margin-bottom:8px;text-transform:uppercase;letter-spacing:.5px}
.stat{padding:3px 10px;border-radius:12px;font-size:11px;font-weight:bold;display:inline-block;margin-right:6px}
.ok{background:#1a3a2a;color:#4ade80;border:1px solid #166534}
.err{background:#3a1a1a;color:#f87171;border:1px solid #7f1d1d}
input[type=text],input[type=number],select,textarea{
  background:#0f0f23;color:#e0e0e0;border:1px solid #3d3d6b;border-radius:4px;padding:4px 6px;font-size:12px;width:100%}
input[type=checkbox]{width:16px;height:16px;cursor:pointer;accent-color:#a78bfa}
.line-row{display:flex;gap:4px;align-items:center;margin-bottom:5px;background:#0f0f23;padding:5px;border-radius:5px;border:1px solid #2d2d5a}
.line-row input[type=text]{flex:1;min-width:0}
.ctl{display:flex;flex-direction:column;align-items:center;gap:2px;flex-shrink:0}
.ctl.size-ctl{width:80px}
.ctl.align-ctl{width:60px}
.ctl.check-ctl{width:32px}
.tiny-lbl{font-size:9px;color:#7c86c9;white-space:nowrap;text-align:center}
.line-num{font-size:10px;color:#555;width:12px;text-align:center;flex-shrink:0}
button{padding:8px 14px;border:none;border-radius:5px;cursor:pointer;font-size:13px;font-weight:bold;color:#fff;width:100%;margin-top:6px}
button.save{background:#6d28d9}
button.test{background:#0e7490;width:auto;padding:4px 10px;font-size:11px;margin-top:0;float:right}
button.connect{background:#065f46}
button.feed-btn{background:#374151}
button.print-btn{background:#1d4ed8}
.feed-row{display:flex;gap:6px;margin-top:5px}
.feed-row button{flex:1}
.section-footer{display:flex;align-items:center;justify-content:space-between;margin-top:6px;font-size:12px;color:#9ca3af}
.section-footer input[type=number]{width:44px;text-align:center}
label.en-lbl{display:flex;align-items:center;gap:4px;font-size:12px;cursor:pointer;color:#a78bfa;float:right}
.pts-filter{margin-top:8px;font-size:12px;color:#9ca3af}
textarea{height:56px;resize:vertical;font-family:monospace}
</style></head>
<body>
<h1>&#127381; C3 Printer</h1>

<div class="card">
  <h2>Status</h2>
  <div id="ps" class="stat err">Printer: --</div>
  <div id="ts" class="stat err">Twitch: --</div>
  <button class="connect" onclick="doConnect()" style="margin-top:8px">Connect Printer</button>
</div>

<div id="cfg"></div>

<div class="card">
  <button class="save" onclick="save()">&#128190; Save All Configuration</button>
</div>

<div class="card">
  <h2>Manual Test Print</h2>
  <textarea id="t_txt">Hello! Caf&#233; resum&#233; na&#239;ve &#1055;&#1088;&#1080;&#1074;&#1077;&#1090; &#12484;</textarea>
  <div class="line-row" style="margin-top:6px">
    <div class="ctl size-ctl">
      <select id="t_s"></select>
      <span class="tiny-lbl">Size</span>
    </div>
    <div class="ctl align-ctl">
      <select id="t_al"><option value="0">Left</option><option value="1" selected>Center</option><option value="2">Right</option></select>
      <span class="tiny-lbl">Align</span>
    </div>
    <div class="ctl check-ctl"><input type="checkbox" id="t_b" checked><span class="tiny-lbl">Bold</span></div>
    <div class="ctl check-ctl"><input type="checkbox" id="t_i"><span class="tiny-lbl">Invert</span></div>
  </div>
  <div class="feed-row">
    <button class="print-btn" onclick="testPrint()">&#128424; Print</button>
    <button class="feed-btn" onclick="feed()">&#128196; Feed 3</button>
  </div>
</div>

<script>
// Size IDs must match FSIZE_* defines in firmware
const SIZES = [
  [0,"Small"],[1,"Medium"],[2,"Large"],[3,"X-Large"],[4,"Huge"]
];

const evts   = ['sub','bit','pts','raid'];
const titles = ['Subscriptions','Bits','Points','Raids'];

function sizeOpts(sel) {
  return SIZES.map(([id,lbl])=>`<option value="${id}"${id==sel?' selected':''}>${lbl}</option>`).join('');
}
function alignOpts(sel) {
  return ['Left','Center','Right'].map((t,i)=>
    `<option value="${i}"${i==sel?' selected':''}>${t}</option>`).join('');
}

function render() {
  let h = '';
  evts.forEach((k,i) => {
    h += `<div class="card">
      <h2>${titles[i]}
        <label class="en-lbl"><input type="checkbox" id="${k}_e"> Enabled</label>
      </h2>`;
    for(let l=0;l<3;l++) {
      h += `<div class="line-row">
        <span class="line-num">${l+1}</span>
        <input type="text" id="${k}${l}_m" placeholder="Line ${l+1} ({user} {amount} {reward})">
        <div class="ctl size-ctl">
          <select id="${k}${l}_s">${sizeOpts(1)}</select>
          <span class="tiny-lbl">Size</span>
        </div>
        <div class="ctl align-ctl">
          <select id="${k}${l}_a">${alignOpts(1)}</select>
          <span class="tiny-lbl">Align</span>
        </div>
        <div class="ctl check-ctl"><input type="checkbox" id="${k}${l}_b"><span class="tiny-lbl">Bold</span></div>
        <div class="ctl check-ctl"><input type="checkbox" id="${k}${l}_i"><span class="tiny-lbl">Inv</span></div>
      </div>`;
    }
    h += `<div class="section-footer">
      <span>Feed lines: <input type="number" id="${k}_f" value="3" min="0" max="20"></span>
      <button class="test" onclick="testEvt('${k}')">&#129514; Test ${titles[i]}</button>
    </div>`;
    if(k==='pts') {
      h += `<div class="pts-filter"><label>Custom Reward ID filter (blank = all):<br>
        <input type="text" id="pts_filter" placeholder="a1b2c3d4-e5f6-..."></label></div>`;
    }
    h += `</div>`;
  });
  document.getElementById('cfg').innerHTML = h;
  document.getElementById('t_s').innerHTML = sizeOpts(1);
}

function load() {
  render();
  fetch('/gcfg').then(r=>r.json()).then(d=>{
    evts.forEach(k=>{
      let el;
      el=document.getElementById(`${k}_e`); if(el) el.checked=d[`${k}_e`];
      el=document.getElementById(`${k}_f`); if(el) el.value=d[`${k}_f`];
      for(let l=0;l<3;l++){
        el=document.getElementById(`${k}${l}_m`); if(el) el.value=d[`${k}${l}_m`]||'';
        el=document.getElementById(`${k}${l}_s`); if(el) el.value=d[`${k}${l}_s`];
        el=document.getElementById(`${k}${l}_a`); if(el) el.value=d[`${k}${l}_a`];
        el=document.getElementById(`${k}${l}_b`); if(el) el.checked=d[`${k}${l}_b`];
        el=document.getElementById(`${k}${l}_i`); if(el) el.checked=d[`${k}${l}_i`];
      }
    });
    el=document.getElementById('pts_filter'); if(el) el.value=d.pts_filter||'';
  }).catch(()=>{});
}

function save() {
  let p = new URLSearchParams();
  evts.forEach(k=>{
    p.append(`${k}_e`,document.getElementById(`${k}_e`).checked?1:0);
    p.append(`${k}_f`,document.getElementById(`${k}_f`).value);
    for(let l=0;l<3;l++){
      p.append(`${k}${l}_m`,document.getElementById(`${k}${l}_m`).value);
      p.append(`${k}${l}_s`,document.getElementById(`${k}${l}_s`).value);
      p.append(`${k}${l}_a`,document.getElementById(`${k}${l}_a`).value);
      p.append(`${k}${l}_b`,document.getElementById(`${k}${l}_b`).checked?1:0);
      p.append(`${k}${l}_i`,document.getElementById(`${k}${l}_i`).checked?1:0);
    }
  });
  p.append('pts_filter',document.getElementById('pts_filter').value.trim());
  fetch('/tcfg',{method:'POST',body:p}).then(r=>r.text()).then(alert).catch(e=>alert(e));
}

function testEvt(k) {
  let p = new URLSearchParams();
  p.append('type',k);
  p.append('f',document.getElementById(`${k}_f`).value);
  for(let l=0;l<3;l++){
    p.append(`m${l}`,document.getElementById(`${k}${l}_m`).value);
    p.append(`s${l}`,document.getElementById(`${k}${l}_s`).value);
    p.append(`a${l}`,document.getElementById(`${k}${l}_a`).value);
    p.append(`b${l}`,document.getElementById(`${k}${l}_b`).checked?1:0);
    p.append(`i${l}`,document.getElementById(`${k}${l}_i`).checked?1:0);
  }
  fetch('/test_evt',{method:'POST',body:p}).then(r=>r.text()).then(alert).catch(e=>alert(e));
}

function testPrint() {
  let p = new URLSearchParams();
  p.append('txt',document.getElementById('t_txt').value);
  p.append('sz', document.getElementById('t_s').value);
  p.append('al', document.getElementById('t_al').value);
  p.append('b',  document.getElementById('t_b').checked?1:0);
  p.append('inv',document.getElementById('t_i').checked?1:0);
  fetch('/p',{method:'POST',body:p}).then(r=>r.text()).then(alert).catch(e=>alert(e));
}

function feed()      { fetch('/f?lines=3'); }
function doConnect() { fetch('/c').then(r=>r.text()).then(alert); }

setInterval(()=>{
  fetch('/s').then(r=>r.json()).then(d=>{
    let ps=document.getElementById('ps'),ts=document.getElementById('ts');
    ps.className='stat '+(d.printer?'ok':'err');
    ps.textContent='Printer: '+(d.printer?'Connected':'Offline');
    ts.className='stat '+(d.twitch?'ok':'err');
    ts.textContent='Twitch: '+(d.twitch?'Connected':'Offline');
  }).catch(()=>{});
},2000);

load();
</script></body></html>
)rawliteral";

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

void handleConnect()    { if(connectPrinter()) server.send(200,"text/plain","Connected!"); else server.send(500,"text/plain","Failed"); }
void handleDisconnect() { disconnectPrinter(); server.send(200,"text/plain","Disconnected"); }

void handlePrint() {
  if(!printerConnected) { server.send(400,"text/plain","Not connected"); return; }
  String text = sanitizeText(server.arg("txt"));
  uint8_t sizeId = server.hasArg("sz") ? (uint8_t)server.arg("sz").toInt() : FSIZE_MEDIUM;
  int  al  = server.hasArg("al")  ? server.arg("al").toInt()   : 1;
  bool b   = server.hasArg("b")   ? (server.arg("b")  =="1")   : true;
  bool inv = server.hasArg("inv") ? (server.arg("inv")=="1")   : false;
  printToThermal(text, sizeId, al, b, inv, 3);
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
  else if(type=="pts")  printEvent(tCfg,"TestUser","","Hydrate :> ツ");
  else if(type=="raid") printEvent(tCfg,"TestUser","","");
  server.send(200,"text/plain","Test Sent");
}

// ========== SETUP & LOOP ==========

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\nESP32-C3 Thermal Printer (Unicode Fallback Mode)");
  loadConfig();
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostname);
  WiFi.begin(MYSSID, MYPSK);
  while(WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nWiFi OK: " + WiFi.localIP().toString());
  if(MDNS.begin(hostname)) { MDNS.addService("http","tcp",80); Serial.println("mDNS: http://c3printer.local"); }
  ArduinoOTA.setHostname(hostname);
  ArduinoOTA.begin();
  connectTwitch();
  server.on("/",         handleRoot);
  server.on("/s",        handleStatus);
  server.on("/gcfg",     handleGetConfig);
  server.on("/c",        handleConnect);
  server.on("/d",        handleDisconnect);
  server.on("/p",        HTTP_POST, handlePrint);
  server.on("/f",        handleFeed);
  server.on("/tcfg",     HTTP_POST, handleTwitchConfig);
  server.on("/test_evt", HTTP_POST, handleTestEvent);
  server.begin();
  Serial.println("Ready!");
}

void loop() {
  ArduinoOTA.handle();
  if(shouldSaveConfig) { saveConfig(); shouldSaveConfig = false; }
  server.handleClient();
  static unsigned long lastTwitchRetry = 0, lastPrinterRetry = 0;
  unsigned long now = millis();
  if(twitchConnected)   handleTwitchIRC();
  else if(now - lastTwitchRetry  > 10000) { lastTwitchRetry  = now; connectTwitch();   }
  if(!printerConnected && now - lastPrinterRetry > 15000) { lastPrinterRetry = now; connectPrinter(); }
  delay(10);
}
