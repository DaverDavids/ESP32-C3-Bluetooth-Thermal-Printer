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
bool twitchConnected = false;
unsigned long lastTwitchPing = 0;

String pointsRewardFilter = "";  // Empty = trigger on ALL redemptions

bool shouldSaveConfig = false;
const int PRINTER_WIDTH = 400; // dots
const int PRINTER_WIDTH_BYTES = PRINTER_WIDTH / 8; // 48 bytes

struct EventConfig {
  bool enabled = true;
  String msg[3];
  int size[3];
  int align[3];
  bool bold[3];
  bool invert[3];
  int feed = 3;
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
  for(int i=0; i<3; i++) {
    twitchCfg.subs.size[i] = 3; twitchCfg.subs.align[i] = 1;
    twitchCfg.subs.bold[i] = true; twitchCfg.subs.invert[i] = false;
  }

  twitchCfg.bits.msg[0] = "CHEER:";
  twitchCfg.bits.msg[1] = "{user}";
  twitchCfg.bits.msg[2] = "{amount} bits";
  for(int i=0; i<3; i++) {
    twitchCfg.bits.size[i] = 3; twitchCfg.bits.align[i] = 1;
    twitchCfg.bits.bold[i] = true; twitchCfg.bits.invert[i] = false;
  }

  twitchCfg.points.msg[0] = "REDEEM:";
  twitchCfg.points.msg[1] = "{user}";
  twitchCfg.points.msg[2] = "{reward}";
  for(int i=0; i<3; i++) {
    twitchCfg.points.size[i] = 3; twitchCfg.points.align[i] = 1;
    twitchCfg.points.bold[i] = true; twitchCfg.points.invert[i] = false;
  }

  twitchCfg.raids.msg[0] = "\xF0\x9F\x9A\xA8 RAID \xF0\x9F\x9A\xA8";
  twitchCfg.raids.msg[1] = "from";
  twitchCfg.raids.msg[2] = "{user}";
  for(int i=0; i<3; i++) {
    twitchCfg.raids.size[i] = 4; twitchCfg.raids.align[i] = 1;
    twitchCfg.raids.bold[i] = true; twitchCfg.raids.invert[i] = false;
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

  ~PrintCanvas() {
    if(buffer) free(buffer);
  }

  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    if(!buffer || x < 0 || x >= _width || y < 0 || y >= _height) return;
    int byteIndex = (y * (_width / 8)) + (x / 8);
    int bitIndex = 7 - (x % 8);
    if(color) buffer[byteIndex] |=  (1 << bitIndex);
    else       buffer[byteIndex] &= ~(1 << bitIndex);
  }

  void clear() {
    if(buffer) memset(buffer, 0, bufferSize);
  }
};

// ========== TEXT PROCESSING ==========

String processNewlines(String text) {
  text.replace("\\n", "\n");
  text.replace("{nl}", "\n");
  return text;
}

// Clean text for printing - keep printable ASCII only.
// Multi-byte UTF-8 sequences and control characters are replaced with '?'.
String sanitizeText(String text) {
  String result = "";
  int i = 0;
  while (i < (int)text.length()) {
    unsigned char c = (unsigned char)text[i];

    if (c == '\n') {
      result += '\n';
      i++;
    } else if (c >= 32 && c <= 126) {
      // Plain printable ASCII - keep as-is
      result += (char)c;
      i++;
    } else if (c < 32 || c == 127) {
      // Control character - skip silently
      i++;
    } else if (c >= 0xF0) {
      // 4-byte UTF-8 (emoji etc.) - replace whole sequence with '?'
      result += '?';
      i += 4;
    } else if (c >= 0xE0) {
      // 3-byte UTF-8 - replace with '?'
      result += '?';
      i += 3;
    } else if (c >= 0xC0) {
      // 2-byte UTF-8 - replace with '?'
      result += '?';
      i += 2;
    } else {
      // Stray continuation byte - skip
      i++;
    }
  }
  return result;
}

String wordWrap(String text, int maxWidth, int fontSize) {
  String result = "";
  int charWidth = 6 * fontSize;
  int maxCharsPerLine = maxWidth / charWidth;
  if(maxCharsPerLine < 5) maxCharsPerLine = 5;

  int lineStart = 0;
  while(lineStart < (int)text.length()) {
    int lineEnd = text.indexOf('\n', lineStart);
    if(lineEnd < 0) lineEnd = text.length();
    String line = text.substring(lineStart, lineEnd);

    if((int)line.length() <= maxCharsPerLine) {
      result += line;
      if(lineEnd < (int)text.length()) result += "\n";
    } else {
      while(line.length() > 0) {
        if((int)line.length() <= maxCharsPerLine) {
          result += line;
          break;
        }
        int breakPoint = maxCharsPerLine;
        int lastSpace = line.lastIndexOf(' ', breakPoint);
        if(lastSpace > 0) breakPoint = lastSpace;
        result += line.substring(0, breakPoint);
        result += "\n";
        line = line.substring(breakPoint);
        if(line.startsWith(" ")) line = line.substring(1);
      }
      if(lineEnd < (int)text.length()) result += "\n";
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

void initPrinter() {
  uint8_t init[] = {0x1B, 0x40};
  sendCmd(init, 2);
  delay(100);
  uint8_t utf8[] = {0x1B, 0x74, 0x10};
  sendCmd(utf8, 3);
  delay(50);
}

void printBitmap(uint8_t *bitmap, int width, int height) {
  if(!printerConnected || !bitmap) return;
  int widthBytes = width / 8;
  uint8_t cmd[] = {
    0x1D, 0x76, 0x30, 0x00,
    (uint8_t)(widthBytes & 0xFF), (uint8_t)(widthBytes >> 8),
    (uint8_t)(height  & 0xFF),   (uint8_t)(height  >> 8)
  };
  pWriteCharacteristic->writeValue(cmd, 8);
  delay(10);
  int totalBytes = widthBytes * height;
  int chunkSize = 200;
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

bool printToThermal(String text, int textSize, int align, bool bold, bool invert, int feedLines) {
  if(!printerConnected) return false;
  if(text.length() == 0) { if(feedLines > 0) feedPaper(feedLines); return true; }

  text = processNewlines(text);
  int fontSize = constrain(textSize, 1, 8);
  int maxTextWidth = PRINTER_WIDTH - 6;
  text = wordWrap(text, maxTextWidth, fontSize);

  int totalLines = 1;
  for(int i = 0; i < (int)text.length(); i++) if(text[i] == '\n') totalLines++;

  int charHeight   = 8 * fontSize;
  int lineSpacing  = 2 * fontSize;
  int lineHeight   = charHeight + lineSpacing;
  int linesPerChunk = 200 / lineHeight;
  if(linesPerChunk < 1) linesPerChunk = 1;

  int currentLineIndex = 0;
  int textIndex = 0;

  while(currentLineIndex < totalLines) {
    int chunkLineCount = 0;
    int chunkHeight = 0;
    if(currentLineIndex == 0) chunkHeight += fontSize * 2;

    for(int i = 0; i < linesPerChunk && (currentLineIndex + i) < totalLines; i++) {
      chunkLineCount++;
      chunkHeight += lineHeight;
    }
    if(currentLineIndex + chunkLineCount >= totalLines) chunkHeight += fontSize * 2;

    PrintCanvas canvas(PRINTER_WIDTH, chunkHeight);
    if(!canvas.buffer) { Serial.println("Chunk alloc failed!"); return false; }

    canvas.setTextSize(fontSize);
    canvas.setTextWrap(false);
    if(invert) {
      canvas.fillRect(0, 0, PRINTER_WIDTH, chunkHeight, 1);
      canvas.setTextColor(0);
    } else {
      canvas.setTextColor(1);
    }

    int drawY = (currentLineIndex == 0) ? fontSize : 0;

    for(int i = 0; i < chunkLineCount; i++) {
      int lineEnd = text.indexOf('\n', textIndex);
      if(lineEnd < 0) lineEnd = text.length();
      String line = text.substring(textIndex, lineEnd);

      if(line.length() > 0) {
        int16_t x1, y1; uint16_t w, h;
        canvas.getTextBounds(line, 0, 0, &x1, &y1, &w, &h);
        int x = 2;
        if(align == 1) x = (PRINTER_WIDTH - w) / 2;
        else if(align == 2) x = PRINTER_WIDTH - w - 2;
        if(x < 2) x = 2;
        canvas.setCursor(x, drawY);
        canvas.print(line);
        if(bold) {
          canvas.setCursor(x+1, drawY); canvas.print(line);
          if(!invert) { canvas.setCursor(x, drawY+1); canvas.print(line); }
        }
      }
      drawY   += lineHeight;
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
    int feed = (i == 2 || cfg.msg[i+1 < 3 ? i+1 : 2].length() == 0) ? cfg.feed : 0;
    printToThermal(p, cfg.size[i], cfg.align[i], cfg.bold[i], cfg.invert[i], feed);
  }
}

// ========== TWITCH IRC PARSING ==========

/**
 * Robustly extracts the chat message payload from a raw Twitch IRC line.
 *
 * Twitch IRC lines look like:
 *   @tags :nick!user@host PRIVMSG #channel :the actual message here :> :-)
 *
 * Strategy:
 *   1. Find the PRIVMSG or USERNOTICE command token.
 *   2. From there, find the '#' that starts the channel name.
 *   3. Find the ' ' space after the channel name.
 *   4. The very next character must be ':', then the message begins.
 *
 * This is the only unambiguous boundary: no matter how many colons appear
 * inside the message text (e.g. ":>", ":-)", "http://..."), they cannot
 * be mistaken for the message-start delimiter.
 */
String extractIRCMessage(const String& line) {
  // Step 1: find the command
  int cmdPos = line.indexOf(" PRIVMSG ");
  if (cmdPos < 0) cmdPos = line.indexOf(" USERNOTICE ");
  if (cmdPos < 0) return "";

  // Step 2: find '#channel' after the command token
  int hashPos = line.indexOf('#', cmdPos);
  if (hashPos < 0) return "";

  // Step 3: find the space after the channel name
  int spaceAfterChan = line.indexOf(' ', hashPos);
  if (spaceAfterChan < 0) return "";

  // Step 4: the message payload begins right after that space+colon
  // Confirm we have ' :' at this position
  if (spaceAfterChan + 1 >= (int)line.length() || line[spaceAfterChan + 1] != ':') return "";

  String payload = line.substring(spaceAfterChan + 2); // skip ' :'
  payload.replace("\r", "");  // strip any trailing carriage return
  payload.trim();
  return payload;
}

/**
 * Extracts a tag value from the Twitch IRC tag string.
 * e.g. extractTag(line, "display-name") returns "DaverDavid"
 */
String extractTag(const String& line, const String& tagName) {
  String search = tagName + "=";
  int start = line.indexOf(search);
  if (start < 0) return "";
  start += search.length();
  // Tags are semicolon-delimited; value ends at ';' or ' ' (start of non-tag section)
  int end = line.indexOf(';', start);
  int spaceEnd = line.indexOf(' ', start);
  if (end < 0 || (spaceEnd >= 0 && spaceEnd < end)) end = spaceEnd;
  if (end < 0) end = line.length();
  return line.substring(start, end);
}

void parseTwitchMessage(String msg) {
  if (msg.indexOf("msg-id=sub") > 0) {
    String user = extractTag(msg, "display-name");
    printEvent(twitchCfg.subs, user, "", "");
  }
  else if (msg.indexOf("bits=") > 0) {
    String bits = extractTag(msg, "bits");
    String user = extractTag(msg, "display-name");
    printEvent(twitchCfg.bits, user, bits, "");
  }
  else if (msg.indexOf("custom-reward-id=") > 0) {
    String rewardId = extractTag(msg, "custom-reward-id");
    if (pointsRewardFilter.length() > 0 && rewardId != pointsRewardFilter) return;
    String user   = extractTag(msg, "display-name");
    String reward = extractIRCMessage(msg);
    printEvent(twitchCfg.points, user, "", reward);
  }
  else if (msg.indexOf("msg-id=raid") > 0) {
    String user = extractTag(msg, "display-name");
    printEvent(twitchCfg.raids, user, "", "");
  }
}

void connectTwitch() {
  Serial.println("Connecting to Twitch IRC...");
  twitchClient.setInsecure();
  if(twitchClient.connect("irc.chat.twitch.tv", 6697)) {
    Serial.println("Connected to Twitch");
    twitchClient.println("PASS " TWITCH_OAUTH_SECRET);
    twitchClient.println("NICK " TWITCH_OAUTH_NICK);
    twitchClient.println("CAP REQ :twitch.tv/tags twitch.tv/commands");
    twitchClient.println("JOIN #" TWITCH_CHANNEL);
    twitchConnected = true;
    lastTwitchPing  = millis();
    Serial.println("Joined #" TWITCH_CHANNEL);
  } else {
    Serial.println("Twitch connection failed");
    twitchConnected = false;
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
  void onConnect(BLEClient* pclient)    { Serial.println("BLE Connected"); }
  void onDisconnect(BLEClient* pclient) { printerConnected = false; Serial.println("BLE Disconnected"); }
};

bool connectPrinter() {
  Serial.println("Connecting: " + printerMAC);
  BLEDevice::init("ESP32-C3-Printer");
  if(pClient) delete pClient;
  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());
  if(!pClient->connect(BLEAddress(printerMAC.c_str()))) return false;
  BLERemoteService* pRemoteService = pClient->getService(serviceUUID);
  if(!pRemoteService) return false;
  pWriteCharacteristic = pRemoteService->getCharacteristic(charWriteUUID);
  if(!pWriteCharacteristic) return false;
  printerConnected = true;
  delay(500);
  uint8_t wake[] = {0x00, 0x00, 0x00, 0x00, 0x00};
  pWriteCharacteristic->writeValue(wake, 5);
  delay(100);
  uint8_t init[] = {0x1B, 0x40};
  pWriteCharacteristic->writeValue(init, 2);
  delay(100);
  Serial.println("Printer Ready");
  return true;
}

void disconnectPrinter() {
  if(pClient != nullptr && printerConnected) pClient->disconnect();
  printerConnected = false;
  Serial.println("Printer disconnected");
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
      evt.size[i]   = preferences.getInt( (p+"_s").c_str(), evt.size[i]);
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
  Serial.println("Configuration loaded");
}

void saveConfig() {
  Serial.println("Saving config...");
  preferences.end();
  if(!preferences.begin("twitch", false)) {
    Serial.println("Failed to open preferences!");
    return;
  }
  auto saveEvent = [&](const char* prefix, EventConfig& evt) {
    preferences.putBool((String(prefix)+"_e").c_str(), evt.enabled);
    preferences.putInt( (String(prefix)+"_f").c_str(), evt.feed);
    for(int i = 0; i < 3; i++) {
      String p = String(prefix) + String(i);
      preferences.putString((p+"_m").c_str(), evt.msg[i]);
      preferences.putInt(   (p+"_s").c_str(), evt.size[i]);
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
  Serial.println("Configuration saved successfully");
}

// ========== WEB SERVER ==========

const char* htmlPage = R"rawliteral(
<!DOCTYPE html>
<html><head><title>ESP32 Printer</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
body{font-family:sans-serif;max-width:600px;margin:0 auto;padding:10px;background:#f4f4f4}
.card{background:#fff;padding:10px;margin-bottom:10px;border-radius:6px;box-shadow:0 1px 3px rgba(0,0,0,0.1)}
h2{margin:0 0 10px;font-size:16px;border-bottom:2px solid #eee;padding-bottom:5px;color:#333}
.stat{padding:4px 8px;border-radius:4px;font-size:12px;font-weight:bold;margin-right:5px;display:inline-block}
.ok{background:#d4edda;color:#155724}.err{background:#f8d7da;color:#721c24}
input,select{box-sizing:border-box;border:1px solid #ccc;border-radius:3px}
input[type=text]{padding:4px;font-size:13px;width:100%}
.line-row{display:flex;gap:4px;align-items:center;margin-bottom:4px;background:#f9f9f9;padding:4px;border-radius:3px}
.line-row input[type=text]{flex:1;min-width:0}
.ctl{display:flex;flex-direction:column;align-items:center;justify-content:center;width:40px}
.ctl select{width:100%;font-size:11px}
.ctl input[type=checkbox]{margin:0}
.tiny-lbl{font-size:9px;color:#666;margin-top:1px}
.sec{margin-bottom:15px}
button{width:100%;padding:10px;background:#007bff;color:#fff;border:none;border-radius:4px;margin-top:5px;cursor:pointer;font-weight:bold}
button:active{opacity:0.8}
button.save{background:#28a745}
button.test{background:#17a2b8;width:auto;float:right;padding:5px 10px;margin:0;font-size:12px}
textarea{width:100%;box-sizing:border-box;border:1px solid #ccc;border-radius:3px;padding:5px;font-family:monospace}
</style></head>
<body>
<div class="card">
  <h2>Status</h2>
  <div id="ps" class="stat err">Printer</div> <div id="ts" class="stat err">Twitch</div>
</div>

<div id="cfg"></div>

<div class="card">
  <button class="save" onclick="save()">&#128190; Save Configuration</button>
</div>

<div class="card">
  <h2>Manual Test Print</h2>
  <textarea id="t_txt" rows="2">Test Print</textarea>
  <div class="line-row">
    <div class="ctl" style="width:auto;flex:1"><span class="tiny-lbl">Settings applied to full text</span></div>
    <div class="ctl"><select id="t_sz"></select><span class="tiny-lbl">Size</span></div>
    <div class="ctl">
      <select id="t_al"><option value="0">L</option><option value="1" selected>C</option><option value="2">R</option></select>
      <span class="tiny-lbl">Align</span>
    </div>
    <div class="ctl"><input type="checkbox" id="t_b" checked><span class="tiny-lbl">Bold</span></div>
    <div class="ctl"><input type="checkbox" id="t_i"><span class="tiny-lbl">Inv</span></div>
  </div>
  <button onclick="print()">&#128424;&#65039; Print Test</button>
  <button onclick="feed()" style="background:#6c757d">&#128196; Feed 3 Lines</button>
</div>

<script>
const evts = ['sub','bit','pts','raid'];
const titles = ['Subscriptions','Bits','Points','Raids'];

function mkOpt(sel) {
  let h=''; for(let i=1;i<=8;i++) h+=`<option value="${i}" ${i==sel?'selected':''}>${i}</option>`; return h;
}

function render() {
  let h='';
  evts.forEach((k,i) => {
    h+=`<div class="card sec"><h2>${titles[i]} <label style="float:right"><input type="checkbox" id="${k}_e"> En</label></h2>`;
    for(let l=0; l<3; l++) {
      h+=`<div class="line-row">
        <span style="font-size:10px;color:#888;width:10px">${l+1}</span>
        <input type="text" id="${k}${l}_m" placeholder="Line ${l+1}...">
        <div class="ctl"><select id="${k}${l}_s" data-def="3"></select><span class="tiny-lbl">Size</span></div>
        <div class="ctl"><select id="${k}${l}_a">
          <option value="0">L</option><option value="1">C</option><option value="2">R</option>
        </select><span class="tiny-lbl">Align</span></div>
        <div class="ctl"><input type="checkbox" id="${k}${l}_b"><span class="tiny-lbl">Bold</span></div>
        <div class="ctl"><input type="checkbox" id="${k}${l}_i"><span class="tiny-lbl">Inv</span></div>
      </div>`;
    }
    h += `<div style="margin-top:5px;font-size:12px;display:flex;align-items:center;justify-content:space-between">
      <span>Feed lines: <input type="number" id="${k}_f" style="width:40px" value="3"></span>
      <button class="test" onclick="test('${k}')">&#129514; Test ${titles[i]}</button>
    </div>`;
    if(k === 'pts') {
      h += `<div style="margin-top:6px;font-size:12px;"><label>Custom Reward ID (blank = all):<br><input type="text" id="pts_filter" style="width:100%" placeholder="Reward UUID (e.g. a1b2c3d4-e5f6-...)"></label></div>`;
    }
    h += `</div>`;
  });

  document.getElementById('cfg').innerHTML = h;
  document.getElementById('t_sz').innerHTML = mkOpt(3);
  evts.forEach(k => {
    for(let l=0;l<3;l++) document.getElementById(`${k}${l}_s`).innerHTML = mkOpt(3);
  });
}

function load() {
  render();
  fetch('/gcfg').then(r=>r.json()).then(d => {
    evts.forEach(k => {
      let el = document.getElementById(`${k}_e`); if(el) el.checked = d[`${k}_e`];
      el = document.getElementById(`${k}_f`); if(el) el.value = d[`${k}_f`];
      for(let l=0; l<3; l++) {
        el = document.getElementById(`${k}${l}_m`); if(el) el.value = d[`${k}${l}_m`] || '';
        el = document.getElementById(`${k}${l}_s`); if(el) el.value = d[`${k}${l}_s`];
        el = document.getElementById(`${k}${l}_a`); if(el) el.value = d[`${k}${l}_a`];
        el = document.getElementById(`${k}${l}_b`); if(el) el.checked = d[`${k}${l}_b`];
        el = document.getElementById(`${k}${l}_i`); if(el) el.checked = d[`${k}${l}_i`];
      }
    });
    el = document.getElementById('pts_filter');
    if(el) el.value = d.pts_filter || '';
  }).catch(e=>{});
}

function save() {
  try {
    let params = new URLSearchParams();
    evts.forEach(k => {
      params.append(`${k}_e`, document.getElementById(`${k}_e`).checked?1:0);
      params.append(`${k}_f`, document.getElementById(`${k}_f`).value);
      for(let l=0; l<3; l++) {
        params.append(`${k}${l}_m`, document.getElementById(`${k}${l}_m`).value);
        params.append(`${k}${l}_s`, document.getElementById(`${k}${l}_s`).value);
        params.append(`${k}${l}_a`, document.getElementById(`${k}${l}_a`).value);
        params.append(`${k}${l}_b`, document.getElementById(`${k}${l}_b`).checked?1:0);
        params.append(`${k}${l}_i`, document.getElementById(`${k}${l}_i`).checked?1:0);
      }
    });
    params.append('pts_filter', document.getElementById('pts_filter').value.trim());
    fetch('/tcfg', {method:'POST', body:params})
      .then(r=>r.text()).then(alert).catch(e=>alert("Error: "+e));
  } catch(err) { alert(err); }
}

function test(k) {
  let params = new URLSearchParams();
  params.append('type', k);
  params.append('f', document.getElementById(`${k}_f`).value);
  for(let l=0; l<3; l++) {
    params.append(`m${l}`, document.getElementById(`${k}${l}_m`).value);
    params.append(`s${l}`, document.getElementById(`${k}${l}_s`).value);
    params.append(`a${l}`, document.getElementById(`${k}${l}_a`).value);
    params.append(`b${l}`, document.getElementById(`${k}${l}_b`).checked?1:0);
    params.append(`i${l}`, document.getElementById(`${k}${l}_i`).checked?1:0);
  }
  fetch('/test_evt', {method:'POST', body:params})
    .then(r=>r.text()).then(alert).catch(e=>alert("Error: "+e));
}

function print() {
  let params = new URLSearchParams();
  params.append('txt', document.getElementById('t_txt').value);
  params.append('sz',  document.getElementById('t_sz').value);
  params.append('al',  document.getElementById('t_al').value);
  params.append('b',   document.getElementById('t_b').checked?1:0);
  params.append('inv', document.getElementById('t_i').checked?1:0);
  fetch('/p', {method:'POST', body:params})
    .then(r=>r.text()).then(alert).catch(e=>alert("Error: "+e));
}

function feed() { fetch('/f?lines=3'); }

setInterval(() => {
  fetch('/s').then(r=>r.json()).then(d => {
    document.getElementById('ps').className = 'stat ' + (d.printer ? 'ok':'err');
    document.getElementById('ts').className = 'stat ' + (d.twitch  ? 'ok':'err');
  }).catch(e=>{});
}, 2000);

load();
</script></body></html>
)rawliteral";

void handleRoot()   { server.send(200, "text/html; charset=UTF-8", htmlPage); }

void handleStatus() {
  String json = "{\"printer\":" + String(printerConnected ? "true" : "false") +
                ",\"twitch\":"+  String(twitchConnected  ? "true" : "false") + "}";
  server.send(200, "application/json", json);
}

void handleGetConfig() {
  String json = "{";
  auto addEvt = [&](const char* p, EventConfig& e) {
    json += "\"" + String(p) + "_e\":" + (e.enabled ? "true" : "false") + ",";
    json += "\"" + String(p) + "_f\":" + String(e.feed) + ",";
    for(int i = 0; i < 3; i++) {
      String k = String(p) + String(i);
      json += "\"" + k + "_m\":\"" + e.msg[i]              + "\",";
      json += "\"" + k + "_s\":"  + String(e.size[i])       + ",";
      json += "\"" + k + "_a\":"  + String(e.align[i])      + ",";
      json += "\"" + k + "_b\":"  + (e.bold[i]   ? "true" : "false") + ",";
      json += "\"" + k + "_i\":"  + (e.invert[i] ? "true" : "false") + ",";
    }
  };
  addEvt("sub",  twitchCfg.subs);
  addEvt("bit",  twitchCfg.bits);
  addEvt("pts",  twitchCfg.points);
  addEvt("raid", twitchCfg.raids);
  json += "\"pts_filter\":\"" + pointsRewardFilter + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void handleConnect() {
  if(connectPrinter()) server.send(200, "text/plain", "Connected!");
  else                  server.send(500, "text/plain", "Connection failed");
}

void handleDisconnect() {
  disconnectPrinter();
  server.send(200, "text/plain", "Disconnected");
}

void handlePrint() {
  if(!printerConnected) { server.send(400, "text/plain", "Printer not connected"); return; }
  String text = server.arg("txt");
  int  sz  = server.hasArg("sz")  ? server.arg("sz").toInt()        : 3;
  int  al  = server.hasArg("al")  ? server.arg("al").toInt()        : 1;
  bool b   = server.hasArg("b")   ? (server.arg("b")   == "1")      : true;
  bool inv = server.hasArg("inv") ? (server.arg("inv") == "1")      : false;
  printToThermal(text, sz, al, b, inv, 3);
  server.send(200, "text/plain", "Printed!");
}

void handleFeed() {
  if(!printerConnected) { server.send(400, "text/plain", "Not connected"); return; }
  int lines = server.arg("lines").toInt();
  if(lines < 1) lines = 3;
  feedPaper(lines);
  server.send(200, "text/plain", "Fed " + String(lines) + " lines");
}

void handleTwitchConfig() {
  Serial.println("Receiving new config...");
  auto updEvt = [&](const char* p, EventConfig& e) {
    if(server.hasArg(String(p)+"_e")) e.enabled = server.arg(String(p)+"_e") == "1";
    if(server.hasArg(String(p)+"_f")) e.feed    = server.arg(String(p)+"_f").toInt();
    for(int i = 0; i < 3; i++) {
      String k = String(p) + String(i);
      if(server.hasArg(k+"_m")) e.msg[i]    = server.arg(k+"_m");
      if(server.hasArg(k+"_s")) e.size[i]   = server.arg(k+"_s").toInt();
      if(server.hasArg(k+"_a")) e.align[i]  = server.arg(k+"_a").toInt();
      if(server.hasArg(k+"_b")) e.bold[i]   = server.arg(k+"_b") == "1";
      if(server.hasArg(k+"_i")) e.invert[i] = server.arg(k+"_i") == "1";
    }
  };
  updEvt("sub",  twitchCfg.subs);
  updEvt("bit",  twitchCfg.bits);
  updEvt("pts",  twitchCfg.points);
  updEvt("raid", twitchCfg.raids);
  if(server.hasArg("pts_filter")) {
    pointsRewardFilter = server.arg("pts_filter");
    pointsRewardFilter.trim();
  }
  shouldSaveConfig = true;
  server.send(200, "text/plain", "Config Saved!");
}

void handleTestEvent() {
  if(!printerConnected) { server.send(400, "text/plain", "No Printer"); return; }
  EventConfig tCfg;
  String type  = server.arg("type");
  tCfg.enabled = true;
  tCfg.feed    = server.arg("f").toInt();
  for(int i = 0; i < 3; i++) {
    tCfg.msg[i]    = server.arg("m"+String(i));
    tCfg.size[i]   = server.arg("s"+String(i)).toInt();
    tCfg.align[i]  = server.arg("a"+String(i)).toInt();
    tCfg.bold[i]   = server.arg("b"+String(i)) == "1";
    tCfg.invert[i] = server.arg("i"+String(i)) == "1";
  }
  if     (type == "sub")  printEvent(tCfg, "TestUser", "",     "");
  else if(type == "bit")  printEvent(tCfg, "TestUser", "1000", "");
  else if(type == "pts")  printEvent(tCfg, "TestUser", "",     "Hydrate :>");
  else if(type == "raid") printEvent(tCfg, "TestUser", "",     "");
  server.send(200, "text/plain", "Test Sent");
}

// ========== SETUP & LOOP ==========

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\nESP32-C3 Thermal Printer (Bitmap Mode)");
  loadConfig();
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostname);
  WiFi.begin(MYSSID, MYPSK);
  while(WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nWiFi OK");
  Serial.print("IP: "); Serial.println(WiFi.localIP());
  if(MDNS.begin(hostname)) {
    Serial.println(String("mDNS: http://") + hostname + ".local");
    MDNS.addService("http", "tcp", 80);
  }
  ArduinoOTA.setHostname(hostname);
  ArduinoOTA.begin();
  Serial.println("OTA Ready");
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
  if(shouldSaveConfig) {
    saveConfig();
    shouldSaveConfig = false;
  }
  server.handleClient();
  static unsigned long lastTwitchRetry  = 0;
  static unsigned long lastPrinterRetry = 0;
  unsigned long now = millis();
  if(twitchConnected) {
    handleTwitchIRC();
  } else {
    if(now - lastTwitchRetry > 10000) { lastTwitchRetry = now; connectTwitch(); }
  }
  if(!printerConnected) {
    if(now - lastPrinterRetry > 15000) { lastPrinterRetry = now; connectPrinter(); }
  }
  delay(10);
}
