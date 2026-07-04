<img src="https://r2cdn.perplexity.ai/pplx-full-logo-primary-dark%402x.png" style="height:64px;margin-right:32px"/>

# tell me what to change to implement the web form upload version, and remove ota. I still want mdns

Here are the exact changes to make. This removes ArduinoOTA entirely, keeps mDNS, and adds a web-based firmware upload using `Update.h`.

## 1. Remove ArduinoOTA

Delete this include:

```cpp
#include <ArduinoOTA.h>
```

Add this instead (mDNS is unaffected, this is separate):

```cpp
#include <Update.h>
```


## 2. Remove ArduinoOTA Setup Calls

In `setup()`, delete these lines:

```cpp
ArduinoOTA.setHostname(hostname);
ArduinoOTA.begin();
```

Keep everything else in `setup()` unchanged, including:

```cpp
if(MDNS.begin(hostname)) { MDNS.addService("http","tcp",80); Serial.println("mDNS: http://c3printer.local"); }
```


## 3. Remove ArduinoOTA from loop()

Delete this line from `loop()`:

```cpp
ArduinoOTA.handle();
```


## 4. Add the Firmware Upload Handler

Add this function anywhere near your other handler functions (e.g., near `handleUpload`):

```cpp
void handleFirmwareUploadStream() {
  WiFiClient client = server.client();

  String line;
  size_t contentLength = 0;
  while (client.connected()) {
    line = client.readStringUntil('\n');
    line.trim();
    if (line.startsWith("Content-Length:")) {
      contentLength = line.substring(16).toInt();
    }
    if (line.length() == 0) break;
  }

  logMsg("Firmware upload start, expecting " + String(contentLength) + " bytes, free heap " + String(ESP.getFreeHeap()));

  if (contentLength == 0) {
    client.println("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n");
    logMsg("Firmware upload ERROR: no Content-Length");
    return;
  }

  if (!Update.begin(contentLength, U_FLASH)) {
    logMsg("Update.begin failed: " + String(Update.errorString()));
    client.println("HTTP/1.1 500 Fail\r\nContent-Length: 0\r\n\r\n");
    return;
  }

  size_t written = 0;
  uint8_t buf[512];
  while (written < contentLength && client.connected()) {
    size_t avail = client.available();
    if (avail) {
      size_t toRead = min(avail, sizeof(buf));
      size_t got = client.read(buf, toRead);
      Update.write(buf, got);
      written += got;
    }
    yield();
  }

  bool ok = (written == contentLength) && Update.end(true);
  logMsg(ok ? "Firmware update SUCCESS, rebooting"
            : ("Firmware update FAILED: " + String(Update.errorString()) + " written " + String(written) + "/" + String(contentLength)));

  if (ok) {
    client.print("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK");
    client.stop();
    delay(300);
    ESP.restart();
  } else {
    Update.abort();
    client.print("HTTP/1.1 500 Fail\r\nContent-Length: 0\r\n\r\n");
    client.stop();
  }
}
```


## 5. Register the Route

In `setup()`, add alongside your other `server.on(...)` calls, before `server.begin()`:

```cpp
server.on("/ota_upload", HTTP_POST, [](){}, handleFirmwareUploadStream);
```

Note the empty lambda as the first handler — this prevents `WebServer`'s default body buffering from interfering, since `handleFirmwareUploadStream` takes over the raw socket itself.

## 6. Add the Upload Form to Your Web UI

In your `htmlPage` HTML, add a new card (place it near the existing "Manual Test Print" card):

```html
<div class="card">
  <h2>Firmware Update</h2>
  <input type="file" id="fwFile" accept=".bin">
  <button class="save" onclick="uploadFirmware()">Flash Firmware</button>
  <div id="fwStatus" style="font-size:11px;margin-top:6px;color:#9ca3af"></div>
</div>
```

Add this JS in your `<script>` block:

```javascript
async function uploadFirmware() {
  const file = document.getElementById('fwFile').files[0];
  const status = document.getElementById('fwStatus');
  if (!file) { status.textContent = 'Select a .bin file first'; return; }
  status.textContent = `Uploading ${file.name} (${file.size} bytes)...`;
  try {
    const res = await fetch('/ota_upload', { method: 'POST', body: file });
    const text = await res.text();
    status.textContent = res.ok ? 'Update OK — rebooting...' : ('Failed: ' + text);
  } catch (e) {
    status.textContent = 'Upload error: ' + e;
  }
}
```


## 7. How to Use It

1. In Arduino IDE: **Sketch → Export Compiled Binary** — produces `c3Printer.ino.bin` next to your sketch
2. Open `http://c3printer.local/`, select that `.bin` file in the new Firmware Update card, click Flash
3. Device writes it via `Update.h`, verifies, and reboots automatically

## Summary of What Changed

| Removed | Added |
| :-- | :-- |
| `#include <ArduinoOTA.h>` | `#include <Update.h>` |
| `ArduinoOTA.setHostname/begin()` | — |
| `ArduinoOTA.handle()` in loop | — |
| — | `/ota_upload` route + streaming handler |
| — | Firmware upload card in web UI |

mDNS (`MDNS.begin`, `MDNS.addService`) stays completely untouched — you'll still resolve `c3printer.local` exactly as before, this only removes the network-discovery OTA protocol, not mDNS itself.

