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

// Measure pixel width of a mixed-font string (Latin + Unicode fallback segments)
static int measureMixed(U8G2_FOR_ADAFRUIT_GFX& u8m, const String& s,
                        const uint8_t* primaryFont, const SizeEntry* se) {
  int total = 0, i = 0, len = s.length();
  while (i < len) {
    int segStart = i;
    uint32_t cp = nextCodepoint(s, i);
    bool fb = !isLatinCodepoint(cp);
    int segEnd = i;
    while (i < len) {
      int before = i;
      uint32_t cp2 = nextCodepoint(s, i);
      if (!isLatinCodepoint(cp2) != fb) { i = before; break; }
      segEnd = i;
    }
    String seg = s.substring(segStart, segEnd > segStart ? segEnd : i);
    u8m.setFont(fb ? (se ? se->unicodeFallback : UNICODE_FALLBACK_FONT) : primaryFont);
    total += u8m.getUTF8Width(seg.c_str());
  }
  return total;
}

// Hard-wrap a single word that is wider than maxWidth, breaking at the pixel boundary
static String hardWrapWord(U8G2_FOR_ADAFRUIT_GFX& u8m, const String& word,
                           int maxWidth, const uint8_t* primaryFont, const SizeEntry* se) {
  String result = "";
  String current = "";
  int i = 0, len = word.length();
  while (i < len) {
    int cpStart = i;
    uint32_t cp = nextCodepoint(word, i);
    String cpStr = word.substring(cpStart, i);
    String test = current + cpStr;
    if (measureMixed(u8m, test, primaryFont, se) <= maxWidth) {
      current = test;
    } else {
      if (current.length() > 0) { result += current; result += "\n"; }
      current = cpStr;
    }
  }
  result += current;
  return result;
}

String wordWrap(const String& text, int maxWidth, const uint8_t* primaryFont, const SizeEntry* se = nullptr) {
  U8G2_FOR_ADAFRUIT_GFX u8m;
  PrintCanvas dummy(8, 8);
  u8m.begin(dummy);

  String result = "";
  int lineStart = 0, textLen = (int)text.length();

  while (lineStart < textLen) {
    // Find end of this hard-wrapped paragraph
    int lineEnd = text.indexOf('\n', lineStart);
    if (lineEnd < 0) lineEnd = textLen;
    String para = text.substring(lineStart, lineEnd);

    // Word-by-word wrapping within this paragraph
    String currentLine = "";
    int wi = 0, plen = para.length();

    while (wi <= plen) {
      // Find next word boundary (space or end of string)
      int wordStart = wi;
      while (wi < plen && para[wi] != ' ') wi++;
      String word = para.substring(wordStart, wi);
      if (wi < plen) wi++; // skip the space

      if (word.length() == 0) continue;

      if (currentLine.length() == 0) {
        // First word on this line
        int wordW = measureMixed(u8m, word, primaryFont, se);
        if (wordW > maxWidth) {
          // Word wider than page — hard wrap it
          String wrapped = hardWrapWord(u8m, word, maxWidth, primaryFont, se);
          // Last segment of wrapped word becomes the new currentLine
          int lastNL = wrapped.lastIndexOf('\n');
          if (lastNL >= 0) {
            result += wrapped.substring(0, lastNL + 1);
            currentLine = wrapped.substring(lastNL + 1);
          } else {
            currentLine = wrapped;
          }
        } else {
          currentLine = word;
        }
      } else {
        // Try appending to current line
        String test = currentLine + " " + word;
        if (measureMixed(u8m, test, primaryFont, se) <= maxWidth) {
          currentLine = test;
        } else {
          // Flush current line, start new
          result += currentLine + "\n";
          // Now handle the word on a fresh line
          int wordW = measureMixed(u8m, word, primaryFont, se);
          if (wordW > maxWidth) {
            String wrapped = hardWrapWord(u8m, word, maxWidth, primaryFont, se);
            int lastNL = wrapped.lastIndexOf('\n');
            if (lastNL >= 0) {
              result += wrapped.substring(0, lastNL + 1);
              currentLine = wrapped.substring(lastNL + 1);
            } else {
              currentLine = wrapped;
            }
          } else {
            currentLine = word;
          }
        }
      }
    }

    // Flush remaining text in this paragraph
    if (currentLine.length() > 0) result += currentLine;
    if (lineEnd < textLen) result += "\n";
    lineStart = lineEnd + 1;
  }
  return result;
}