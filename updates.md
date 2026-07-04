<img src="https://r2cdn.perplexity.ai/pplx-full-logo-primary-dark%402x.png" style="height:64px;margin-right:32px"/>

# yes, tell me how to implement the factory partition method... it should still be flashable over arduino IDE ota either way

This gives you SPIFFS space of ~2.19MB with your fonts using only 645KB — nearly 1.6MB of headroom, while still enabling real OTA. Here's the full implementation.

## Important Caveat First

This factory+OTA hybrid is **not** directly compatible with ArduinoOTA or a single `Update.h` call from your main sketch — that's the whole point of the pattern. Your **main application** (with BLE/Twitch/fonts) lives in `ota_0` and cannot safely rewrite itself while running, for the same XIP reason as before. Instead, the **factory partition** runs a small separate program whose only job is to receive the new binary and write it into `ota_0`. Flashing via Arduino IDE's network port still works, but it targets the factory app's web server, not your main app directly — the mechanism is a two-app-swap, not true self-update.

## 1. Partition Table

```csv
# Name,   Type, SubType, Offset,   Size
nvs,      data, nvs,     0x9000,   0x5000,
otadata,  data, ota,     0xe000,   0x2000,
factory,  app,  factory, 0x10000,  0x40000,
ota_0,    app,  ota_0,   0x50000,  0x180000,
spiffs,   data, spiffs,  0x1d0000, 0x230000,
```

Factory = 256KB (plenty for a minimal receiver app), ota_0 = 1.5MB (your full app), SPIFFS = ~2.19MB.

## 2. How the Flow Works

1. Device normally boots **ota_0** (your full thermal-printer app) — this is what runs day-to-day.
2. To update: your ota_0 app receives a signal (e.g., a specific HTTP endpoint hit) telling it to reboot into recovery mode.
3. On that signal, ota_0 calls `esp_ota_set_boot_partition()` pointing at **factory**, then `ESP.restart()`.
4. The **factory** app boots up — it's tiny, just runs a WebServer with the same `/ota_upload` handler you already built, and mDNS with the same hostname.
5. You upload the new `.bin` via the web form exactly as before — but now it's writing into `ota_0` (which is safe, since factory is the one currently executing).
6. On success, factory calls `esp_ota_set_boot_partition()` pointing back at **ota_0**, then reboots into your updated main app.

## 3. Main App Changes (ota_0)

Add a trigger route to reboot into factory/recovery mode:

```cpp
#include "esp_ota_ops.h"

server.on("/enter_update_mode", HTTP_POST, [](){
  server.send(200, "text/plain", "Rebooting into update mode...");
  delay(300);

  const esp_partition_t* factory_part = esp_partition_find_first(
    ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);

  if (factory_part) {
    esp_ota_set_boot_partition(factory_part);
    ESP.restart();
  } else {
    logMsg("ERROR: factory partition not found");
  }
});
```

Add a button in your web UI to hit this before uploading:

```html
<button class="save" onclick="enterUpdateMode()">Enter Update Mode</button>
```

```javascript
async function enterUpdateMode() {
  await fetch('/enter_update_mode', { method: 'POST' });
  document.getElementById('fwStatus').textContent = 'Rebooting into update mode... wait ~5s then upload';
}
```


## 4. Factory App (Separate, Minimal Sketch)

Create a second, small `.ino` project — this is what gets flashed once via USB into the `factory` slot and never needs updating again:

```cpp
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include "esp_ota_ops.h"

const char* ssid = "YOUR_SSID";
const char* password = "YOUR_PASSWORD";
const char* hostname = "c3printer";

WebServer server(80);

void handleFirmwareUpload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    const esp_partition_t* ota0 = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH, -1, LOW, ota0);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    Update.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    Update.end(true);
  }
}

void setup() {
  Serial.begin(115200);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
  MDNS.begin(hostname);
  MDNS.addService("http", "tcp", 80);

  server.on("/ota_upload", HTTP_POST,
    [](){
      const esp_partition_t* ota0 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
      if (!Update.hasError() && ota0) esp_ota_set_boot_partition(ota0);
      server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK, rebooting into main app");
      delay(300);
      ESP.restart();
    },
    handleFirmwareUpload
  );

  server.on("/", HTTP_GET, [](){
    server.send(200, "text/html",
      "<h2>Firmware Recovery Mode</h2>"
      "<form method='POST' action='/ota_upload' enctype='multipart/form-data'>"
      "<input type='file' name='firmware'><input type='submit' value='Upload'></form>");
  });

  server.begin();
  Serial.println("Recovery mode ready at http://c3printer.local/");
}

void loop() {
  server.handleClient();
}
```


## 5. Flash Process (One-Time Setup)

1. Set the partition table above, erase flash entirely (Tools → Erase Flash → All Flash Contents)
2. Flash the **factory app** via USB, targeting the `factory` partition offset (you'll need `esptool.py` directly for this, or a build script, since Arduino IDE by default flashes to `ota_0` — flag this if you want the exact esptool command)
3. Flash your **main app** via USB, into `ota_0`
4. From then on: hit "Enter Update Mode" in your main app's web UI → device reboots into factory → upload new `.bin` for the main app via the factory's web form → device reboots back into updated main app automatically

## Answering "Flashable via Arduino IDE OTA either way"

Yes — network-based uploads work throughout, since both factory and ota_0 run their own WebServer with mDNS at the same hostname, so `http://c3printer.local/` always resolves to whichever app is currently active. However, note this isn't literally "ArduinoOTA" (the mDNS-discovery IDE plugin) — it's the same web-form upload pattern you already built, just now hosted from two possible apps depending on mode. Want me to give you the exact `esptool.py` command to flash the factory app to its correct offset the first time?

