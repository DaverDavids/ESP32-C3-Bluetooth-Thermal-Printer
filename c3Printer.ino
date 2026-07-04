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
#include <SPIFFS.h>
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
File uploadFile;
size_t uploadExpectedSize = 0;
size_t uploadWrittenBytes = 0;
bool uploadFailed = false;

const int PRINTER_WIDTH       = 400;
const int PRINTER_WIDTH_BYTES = PRINTER_WIDTH / 8;

// PRINT_SCALE: base multiplier (default 2). EventConfig.font[] stores the
// per-line override (1=Small/16px, 2=Medium/32px, 3=Large/48px on paper).
#define PRINT_SCALE 2

#define SCALE_SMALL  1
#define SCALE_MEDIUM 2
#define SCALE_LARGE  3

// ========== VLW FONT STRUCTS ==========
// Defined first — referenced by loadVlw/drawGlyph which come after PrintCanvas.

struct VlwGlyph {
  uint32_t cp;
  int16_t  w, h, advance, x_off, y_off;
  uint32_t bitmapOffset;
};

struct VlwFont {
  int       count;
  int       size;
  VlwGlyph* glyphs;
  uint8_t*  bitmaps;
  size_t    bitmapBytes;
  bool      loaded;
};

VlwFont fontBasic = {0};
VlwFont fontCJK   = {0};

// ========== BITMAP CANVAS ==========
// Must be defined before drawGlyph.

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
// Defined before initDefaults() and printEvent().

struct EventConfig {
  bool    enabled = true;
  String  msg[3];
  uint8_t font[3];   // SCALE_SMALL/MEDIUM/LARGE
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
  File file = SPIFFS.open(path, "r");
  if (!file) { Serial.printf("VLW missing: %s\n", path); return false; }

  auto read32 = [&]() -> int32_t {
    uint8_t b[4]; file.read(b, 4);
    return (b[0]<<24)|(b[1]<<16)|(b[2]<<8)|b[3];
  };

  f.count  = read32();
  int ver  = read32();
  f.size   = read32();
  read32(); read32(); read32(); // reserved

  f.glyphs = (VlwGlyph*)malloc(f.count * sizeof(VlwGlyph));
  if (!f.glyphs) { file.close(); return false; }

  for (int i = 0; i < f.count; i++) {
    f.glyphs[i].cp      = (uint32_t)read32();
    f.glyphs[i].h       = (int16_t)read32();
    f.glyphs[i].w       = (int16_t)read32();
    f.glyphs[i].advance = (int16_t)read32();
    f.glyphs[i].x_off   = (int16_t)read32();
    f.glyphs[i].y_off   = (int16_t)read32();
  }

  f.bitmapBytes = file.size() - file.position();
  f.bitmaps = (uint8_t*)malloc(f.bitmapBytes);
  if (!f.bitmaps) { free(f.glyphs); file.close(); return false; }
  file.read(f.bitmaps, f.bitmapBytes);
  file.close();

  size_t offset = 0;
  for (int i = 0; i < f.count; i++) {
    f.glyphs[i].bitmapOffset = offset;
    int rowBytes = (f.glyphs[i].w + 7) / 8;
    offset += rowBytes * f.glyphs[i].h;
  }

  f.loaded = true;
  Serial.printf("VLW loaded: %s (%d glyphs)\n", path, f.count);
  return true;
}

// ========== GLYPH LOOKUP + DRAW ==========

const VlwGlyph* findGlyph(const VlwFont& f, uint32_t cp) {
  if (!f.loaded) return nullptr;
  int lo = 0, hi = f.count - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if      (f.glyphs[mid].cp == cp) return &f.glyphs[mid];
    else if (f.glyphs[mid].cp  < cp) lo = mid + 1;
    else                              hi = mid - 1;
  }
  return nullptr;
}

// Returns glyph + owning font pointer; basic first, then CJK.
const VlwGlyph* getGlyph(uint32_t cp, const VlwFont** outFont) {
  const VlwGlyph* g = findGlyph(fontBasic, cp);
  if (g) { if (outFont) *outFont = &fontBasic; return g; }
  g = findGlyph(fontCJK, cp);
  if (g) { if (outFont) *outFont = &fontCJK;   return g; }
  if (outFont) *outFont = nullptr;
  return nullptr;
}

// Draw one glyph into PrintCanvas at (x, baseline_y). Returns advance width.
int drawGlyph(PrintCanvas& canvas, const VlwFont& font, const VlwGlyph* g,
              int x, int baseline_y, bool invert) {
  if (!g || g->w == 0 || g->h == 0) return g ? g->advance : 0;
  int rowBytes = (g->w + 7) / 8;
  const uint8_t* bmp = font.bitmaps + g->bitmapOffset;
  for (int row = 0; row < g->h; row++) {
    int py = baseline_y + g->y_off + row;
    for (int col = 0; col < g->w; col++) {
      int px = x + g->x_off + col;
      uint8_t byte = bmp[row * rowBytes + col / 8];
      bool set = (byte >> (7 - (col % 8))) & 1;
      if (set) canvas.drawPixel(px, py, invert ? 0 : 1);
    }
  }
  return g->advance;
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
  if (c < 0xE0)  { uint32_t cp = (c & 0x1F); i++; if(i<(int)s.length()) cp=(cp<<6)|((unsigned char)s[i++]&0x3F); return cp; }
  if (c < 0xF0)  { uint32_t cp = (c & 0x0F); i++; for(int j=0;j<2&&i<(int)s.length();j++) cp=(cp<<6)|((unsigned char)s[i++]&0x3F); return cp; }
  { uint32_t cp = (c & 0x07); i++; for(int j=0;j<3&&i<(int)s.length();j++) cp=(cp<<6)|((unsigned char)s[i++]&0x3F); return cp; }
}

// ========== VLW WORD WRAP ==========

int measureTextVlw(const String& text) {
  int total = 0, i = 0, len = (int)text.length();
  while (i < len) {
    uint32_t cp = nextCodepoint(text, i);
    const VlwFont* srcFont = nullptr;
    const VlwGlyph* g = getGlyph(cp, &srcFont);
    if (g) total += g->advance;
    else   total += fontBasic.size / 2;
  }
  return total;
}

String wordWrap(const String& text, int maxWidth) {
  String result = "";
  int lineStart = 0, textLen = (int)text.length();

  while (lineStart < textLen) {
    int lineEnd = text.indexOf('\n', lineStart);
    if (lineEnd < 0) lineEnd = textLen;
    String line = text.substring(lineStart, lineEnd);

    if (measureTextVlw(line) <= maxWidth) {
      result += line;
      if (lineEnd < textLen) result += "\n";
    } else {
      while (line.length() > 0) {
        if (measureTextVlw(line) <= maxWidth) { result += line; break; }
        int lo = 1, hi = (int)line.length(), breakAt = 1;
        while (lo <= hi) {
          int mid = (lo + hi) / 2;
          String test = line.substring(0, mid);
          if (measureTextVlw(test) <= maxWidth) { breakAt = mid; lo = mid + 1; }
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

// ========== THERMAL PRINT ==========

bool printToThermal(String text, uint8_t printScale, int align, bool bold, bool invert, int feedLines) {
  if(!printerConnected) return false;
  if(text.length() == 0) { if(feedLines > 0) feedPaper(feedLines); return true; }

  text = processNewlines(text);

  if (printScale < 1) printScale = 1;
  if (printScale > 3) printScale = 3;

  const int vlwSize    = (fontBasic.loaded ? fontBasic.size : 16);
  const int renderW    = PRINTER_WIDTH / printScale;
  const int maxTextW   = renderW - 8;
  const int lineSpacing = 3;
  const int lineHeight  = vlwSize + lineSpacing;
  const int baseline    = vlwSize;

  text = wordWrap(text, maxTextW);

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
    if(!canvas.buffer) { Serial.println("Chunk alloc failed!"); return false; }

    if(invert) canvas.fillRect(0, 0, renderW, chunkHeight, 1);

    int drawY = (currentLineIndex == 0) ? lineSpacing + baseline : baseline;

    for(int i = 0; i < chunkLineCount; i++) {
      int lineEnd = text.indexOf('\n', textIndex);
      if(lineEnd < 0) lineEnd = (int)text.length();
      String line = text.substring(textIndex, lineEnd);

      if(line.length() > 0) {
        int tw = min(measureTextVlw(line), renderW - 4);

        int x = 2;
        if     (align == 1) x = max(2, (renderW - tw) / 2);
        else if(align == 2) x = max(2, renderW - tw - 2);

        int ci = 0, clen = (int)line.length();
        while (ci < clen) {
          uint32_t cp = nextCodepoint(line, ci);
          const VlwFont* srcFont = nullptr;
          const VlwGlyph* g = getGlyph(cp, &srcFont);
          if (g && srcFont) {
            x += drawGlyph(canvas, *srcFont, g, x, drawY, invert);
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

#include "html.h"

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

// ========== FILE UPLOAD ==========

void handleUpload() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    String filename = "/" + upload.filename;
    Serial.printf("Upload start: %s\n", filename.c_str());

    size_t freeBytes = SPIFFS.totalBytes() - SPIFFS.usedBytes();
    Serial.printf("SPIFFS free: %u bytes\n", freeBytes);

    if (SPIFFS.exists(filename)) SPIFFS.remove(filename);
    uploadFile = SPIFFS.open(filename, "w");
    uploadWrittenBytes = 0;
    uploadFailed = false;

    if (!uploadFile) {
      Serial.println("Failed to open file for writing");
      uploadFailed = true;
    }
  }
  else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile && !uploadFailed) {
      size_t written = uploadFile.write(upload.buf, upload.currentSize);
      uploadWrittenBytes += written;
      if (written != upload.currentSize) {
        Serial.printf("WRITE MISMATCH: expected %u, wrote %u\n",
                      upload.currentSize, written);
        uploadFailed = true;
      }
    }
    yield();
  }
  else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) uploadFile.close();
    Serial.printf("Upload end. Total written: %u / expected %u\n",
                  uploadWrittenBytes, upload.totalSize);
    if (uploadWrittenBytes != upload.totalSize) {
      uploadFailed = true;
      Serial.println("SIZE MISMATCH — upload incomplete!");
    }
  }
  else if (upload.status == UPLOAD_FILE_ABORTED) {
    Serial.println("Upload ABORTED by client");
    uploadFailed = true;
    if (uploadFile) uploadFile.close();
  }
}

void handleUploadComplete() {
  if (uploadFailed) {
    server.send(500, "text/plain", "Upload FAILED — file incomplete or write error. Check Serial log.");
  } else {
    server.send(200, "text/plain", "Upload OK: " + String(uploadWrittenBytes) + " bytes written.");
  }
}

// ========== SETUP & LOOP ==========

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\nESP32-C3 Thermal Printer (VLW SPIFFS Fonts)");
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS mount failed");
  }
  loadConfig();
  loadVlw(fontBasic, "/unifont_basic.vlw");
  loadVlw(fontCJK,   "/unifont_cjk.vlw");
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostname);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
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
  server.on("/upload", HTTP_POST, handleUploadComplete, handleUpload);
  server.on("/spiffs_check", []() {
    String out = "<h2>SPIFFS Files</h2><ul>";
    File root = SPIFFS.open("/");
    File f = root.openNextFile();
    bool any = false;
    while (f) {
      any = true;
      String name = String(f.name());
      out += "<li>" + name + " - " + String(f.size()) + " bytes <a href=\"/delete_file?name=" + name + "\" onclick=\"return confirm('Delete " + name + "?')\" style=\"color:#f87171\">[delete]</a></li>";
      f = root.openNextFile();
    }
    if (!any) out += "<li>No files on SPIFFS</li>";
    out += "</ul><p><a href=\"/\" style=\"color:#a78bfa\">&larr; Back</a></p>";
    server.send(200, "text/html", out);
  });
  server.on("/delete_file", []() {
    String name = server.arg("name");
    if (name.length() == 0) { server.send(400, "text/plain", "Missing name"); return; }
    if (!name.startsWith("/")) name = "/" + name;
    if (!SPIFFS.exists(name)) { server.send(404, "text/plain", "Not found: " + name); return; }
    SPIFFS.remove(name);
    server.send(200, "text/html", "<p>Deleted: " + name + "</p><p><a href=\"/spiffs_check\" style=\"color:#a78bfa\">&larr; Back</a></p>");
  });
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
