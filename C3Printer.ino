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

// ========== U8G2 FONT TABLE ==========
// _tf  = transparent, full character set (ISO 8859 Latin extended)
// _tr  = transparent, ASCII 32-127 only
// Unifont: u8g2_font_unifont_tf  (the _t_unifont suffix does not exist)

struct FontEntry {
  uint8_t     id;
  const char* label;
  const uint8_t* font;
  uint8_t     charH;
};

#define FONT_5X7         0
#define FONT_6X10        1
#define FONT_7X13        2
#define FONT_7X13B       3
#define FONT_8X13        4
#define FONT_8X13B       5
#define FONT_9X15        6
#define FONT_9X15B       7
#define FONT_10X20       8
#define FONT_NCENR12     9
#define FONT_NCENR14     10
#define FONT_NCENR18     11
#define FONT_HELVR12     12
#define FONT_HELVR14     13
#define FONT_HELVR18     14
#define FONT_HELVB12     15
#define FONT_HELVB14     16
#define FONT_HELVB18     17
#define FONT_LOGISOSO28  18
#define FONT_LOGISOSO32  19
#define FONT_LOGISOSO42  20
#define FONT_UNIFONT     21
#define FONT_CYRILLIC_S  22
#define FONT_CYRILLIC_L  23

const FontEntry FONT_TABLE[] = {
  { FONT_5X7,        "5x7",               u8g2_font_5x7_tf,        7  },
  { FONT_6X10,       "6x10",              u8g2_font_6x10_tf,       10 },
  { FONT_7X13,       "7x13",              u8g2_font_7x13_tf,       13 },
  { FONT_7X13B,      "7x13 Bold",         u8g2_font_7x13B_tf,      13 },
  { FONT_8X13,       "8x13",              u8g2_font_8x13_tf,       13 },
  { FONT_8X13B,      "8x13 Bold",         u8g2_font_8x13B_tf,      13 },
  { FONT_9X15,       "9x15",              u8g2_font_9x15_tf,       15 },
  { FONT_9X15B,      "9x15 Bold",         u8g2_font_9x15B_tf,      15 },
  { FONT_10X20,      "10x20",             u8g2_font_10x20_tf,      20 },
  { FONT_NCENR12,    "Serif 14",          u8g2_font_ncenR14_tf,    14 },
  { FONT_NCENR14,    "Serif 14",          u8g2_font_ncenR14_tf,    14 },
  { FONT_NCENR18,    "Serif 24",          u8g2_font_ncenR24_tf,    24 },
  { FONT_HELVR12,    "Sans 14",           u8g2_font_helvR14_tf,    14 },
  { FONT_HELVR14,    "Sans 14",           u8g2_font_helvR14_tf,    14 },
  { FONT_HELVR18,    "Sans 24",           u8g2_font_helvR24_tf,    24 },
  { FONT_HELVB12,    "Sans Bold 12",      u8g2_font_helvB12_tf,    12 },
  { FONT_HELVB14,    "Sans Bold 14",      u8g2_font_helvB14_tf,    14 },
  { FONT_HELVB18,    "Sans Bold 18",      u8g2_font_helvB18_tf,    18 },
  { FONT_LOGISOSO28, "Logisoso 28",       u8g2_font_logisoso28_tf, 28 },
  { FONT_LOGISOSO32, "Logisoso 32",       u8g2_font_logisoso32_tf, 32 },
  { FONT_LOGISOSO42, "Logisoso 42",       u8g2_font_logisoso42_tf, 42 },
  { FONT_UNIFONT,    "Unifont (Unicode)",  u8g2_font_unifont_tf,    16 },
  { FONT_CYRILLIC_S, "5x7 Cyrillic",      u8g2_font_5x7_t_cyrillic,  7 },
  { FONT_CYRILLIC_L, "9x15 Cyrillic",     u8g2_font_9x15_t_cyrillic, 15 },
};
const int FONT_TABLE_SIZE = sizeof(FONT_TABLE) / sizeof(FONT_TABLE[0]);

const FontEntry* getFontEntry(uint8_t id) {
  for (int i = 0; i < FONT_TABLE_SIZE; i++)
    if (FONT_TABLE[i].id == id) return &FONT_TABLE[i];
  return &FONT_TABLE[2];
}

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
    twitchCfg.subs.font[i]=FONT_7X13; twitchCfg.subs.align[i]=1;
    twitchCfg.subs.bold[i]=true; twitchCfg.subs.invert[i]=false;
  }
  twitchCfg.bits.msg[0] = "CHEER:";
  twitchCfg.bits.msg[1] = "{user}";
  twitchCfg.bits.msg[2] = "{amount} bits";
  for(int i=0;i<3;i++){
    twitchCfg.bits.font[i]=FONT_7X13; twitchCfg.bits.align[i]=1;
    twitchCfg.bits.bold[i]=true; twitchCfg.bits.invert[i]=false;
  }
  twitchCfg.points.msg[0] = "REDEEM:";
  twitchCfg.points.msg[1] = "{user}";
  twitchCfg.points.msg[2] = "{reward}";
  for(int i=0;i<3;i++){
    twitchCfg.points.font[i]=FONT_7X13; twitchCfg.points.align[i]=1;
    twitchCfg.points.bold[i]=true; twitchCfg.points.invert[i]=false;
  }
  twitchCfg.raids.msg[0] = "RAID!";
  twitchCfg.raids.msg[1] = "from";
  twitchCfg.raids.msg[2] = "{user}";
  for(int i=0;i<3;i++){
    twitchCfg.raids.font[i]=FONT_LOGISOSO28; twitchCfg.raids.align[i]=1;
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

String wordWrap(const String& text, int maxWidth, const uint8_t* font) {
  U8G2_FOR_ADAFRUIT_GFX u8measure;
  PrintCanvas dummy(8, 8);
  u8measure.begin(dummy);
  u8measure.setFont(font);

  String result = "";
  int lineStart = 0, textLen = (int)text.length();

  while (lineStart < textLen) {
    int lineEnd = text.indexOf('\n', lineStart);
    if (lineEnd < 0) lineEnd = textLen;
    String line = text.substring(lineStart, lineEnd);

    if (u8measure.getUTF8Width(line.c_str()) <= maxWidth) {
      result += line;
      if (lineEnd < textLen) result += "\n";
    } else {
      while (line.length() > 0) {
        if (u8measure.getUTF8Width(line.c_str()) <= maxWidth) {
          result += line;
          break;
        }
        int lo = 1, hi = line.length(), breakAt = 1;
        while (lo <= hi) {
          int mid = (lo + hi) / 2;
          String test = line.substring(0, mid);
          if (u8measure.getUTF8Width(test.c_str()) <= maxWidth) {
            breakAt = mid; lo = mid + 1;
          } else {
            hi = mid - 1;
          }
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

bool printToThermal(String text, uint8_t fontId, int align, bool bold, bool invert, int feedLines) {
  if(!printerConnected) return false;
  if(text.length() == 0) { if(feedLines > 0) feedPaper(feedLines); return true; }

  text = processNewlines(text);
  const FontEntry* fe = getFontEntry(fontId);

  int maxTextWidth = PRINTER_WIDTH - 6;
  text = wordWrap(text, maxTextWidth, fe->font);

  int totalLines = 1;
  for(int i = 0; i < (int)text.length(); i++) if(text[i] == '\n') totalLines++;

  int charHeight    = fe->charH;
  int lineSpacing   = max(2, charHeight / 6);
  int lineHeight    = charHeight + lineSpacing;
  int linesPerChunk = max(1, 200 / lineHeight);

  int currentLineIndex = 0, textIndex = 0;

  while(currentLineIndex < totalLines) {
    int chunkLineCount = 0, chunkHeight = 0;
    if(currentLineIndex == 0) chunkHeight += lineSpacing * 2;
    for(int i = 0; i < linesPerChunk && (currentLineIndex + i) < totalLines; i++) {
      chunkLineCount++; chunkHeight += lineHeight;
    }
    if(currentLineIndex + chunkLineCount >= totalLines) chunkHeight += lineSpacing * 2;

    PrintCanvas canvas(PRINTER_WIDTH, chunkHeight);
    if(!canvas.buffer) { Serial.println("Chunk alloc failed!"); return false; }

    if(invert) canvas.fillRect(0, 0, PRINTER_WIDTH, chunkHeight, 1);

    U8G2_FOR_ADAFRUIT_GFX u8g2;
    u8g2.begin(canvas);
    u8g2.setFont(fe->font);
    u8g2.setFontMode(1);
    u8g2.setFontDirection(0);
    u8g2.setForegroundColor(invert ? 0 : 1);
    u8g2.setBackgroundColor(invert ? 1 : 0);

    int drawY = (currentLineIndex == 0) ? lineSpacing + charHeight : charHeight;

    for(int i = 0; i < chunkLineCount; i++) {
      int lineEnd = text.indexOf('\n', textIndex);
      if(lineEnd < 0) lineEnd = text.length();
      String line = text.substring(textIndex, lineEnd);

      if(line.length() > 0) {
        int16_t tw = u8g2.getUTF8Width(line.c_str());
        int x = 2;
        if     (align == 1) x = max(0, (PRINTER_WIDTH - tw) / 2);
        else if(align == 2) x = max(2, PRINTER_WIDTH - tw - 2);
        u8g2.setCursor(x, drawY);
        u8g2.print(line);
        if(bold) {
          u8g2.setCursor(x + 1, drawY);
          u8g2.print(line);
        }
      }
      drawY    += lineHeight;
      textIndex = lineEnd + 1;
    }

    printBitmap(canvas.buffer, PRINTER_WIDTH, chunkHeight);
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
.ctl.font-ctl{width:130px}
.ctl.align-ctl{width:56px}
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
  <textarea id="t_txt">Hello! Cafe resume naive Privet</textarea>
  <div class="line-row" style="margin-top:6px">
    <div class="ctl font-ctl">
      <select id="t_s"></select>
      <span class="tiny-lbl">Font</span>
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
const FONTS = [
  [0,"5x7"],[1,"6x10"],[2,"7x13"],[3,"7x13 Bold"],
  [4,"8x13"],[5,"8x13 Bold"],[6,"9x15"],[7,"9x15 Bold"],[8,"10x20"],
  [9,"Serif 14"],[10,"Serif 14"],[11,"Serif 24"],
  [12,"Sans 14"],[13,"Sans 14"],[14,"Sans 24"],
  [15,"Sans Bold 12"],[16,"Sans Bold 14"],[17,"Sans Bold 18"],
  [18,"Logisoso 28"],[19,"Logisoso 32"],[20,"Logisoso 42"],
  [21,"Unifont (Unicode)"],[22,"5x7 Cyrillic"],[23,"9x15 Cyrillic"]
];

const evts   = ['sub','bit','pts','raid'];
const titles = ['Subscriptions','Bits','Points','Raids'];

function fontOpts(sel) {
  return FONTS.map(([id,lbl])=>`<option value="${id}"${id==sel?' selected':''}>${lbl}</option>`).join('');
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
        <div class="ctl font-ctl">
          <select id="${k}${l}_s">${fontOpts(2)}</select>
          <span class="tiny-lbl">Font</span>
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
  document.getElementById('t_s').innerHTML = fontOpts(2);
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
  uint8_t fontId = server.hasArg("sz") ? (uint8_t)server.arg("sz").toInt() : FONT_7X13;
  int  al  = server.hasArg("al")  ? server.arg("al").toInt()   : 1;
  bool b   = server.hasArg("b")   ? (server.arg("b")  =="1")   : true;
  bool inv = server.hasArg("inv") ? (server.arg("inv")=="1")   : false;
  printToThermal(text, fontId, al, b, inv, 3);
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
  else if(type=="pts")  printEvent(tCfg,"TestUser","","Hydrate :>");
  else if(type=="raid") printEvent(tCfg,"TestUser","","");
  server.send(200,"text/plain","Test Sent");
}

// ========== SETUP & LOOP ==========

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\nESP32-C3 Thermal Printer (U8g2 Font Mode)");
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
