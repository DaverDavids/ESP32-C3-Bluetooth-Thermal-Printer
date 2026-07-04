<img src="https://r2cdn.perplexity.ai/pplx-full-logo-primary-dark%402x.png" style="height:64px;margin-right:32px"/>

# [1016] Boot. Heap: 217592 MaxAlloc: 114676

[1019] LittleFS mounted OK
[1036] LittleFS total: 983040 used: 618496
[1036] After LittleFS. Heap: 215576 MaxAlloc: 114676
[1040] Config loaded
[1040] After loadConfig. Heap: 215576 MaxAlloc: 114676
[1248] VLW loaded: /unifont_cjk.vlw (5568 glyphs)
[1507] After VLW load. Heap: 100300 MaxAlloc: 77812
[5713]
WiFi OK: 192.168.7.150
[5714] After WiFi connect. Heap: 44508 MaxAlloc: 22516
[5716] mDNS: [http://c3printer.local](http://c3printer.local)
[5716] After mDNS. Heap: 38512 MaxAlloc: 18420
[5718] After ArduinoOTA. Heap: 35944 MaxAlloc: 16372
[5718] Twitch connect SKIPPED: heap too low (maxAlloc=16372)
[5718] After Twitch attempt. Heap: 35944 MaxAlloc: 16372
[5719] Ready!
[10003] Uptime 10s  Free heap: 33904 MaxAlloc: 14324  RSSI -59dBm
[10003] Twitch connect SKIPPED: heap too low (maxAlloc=14324)
[15008] BLE connect SKIPPED: heap too low (free=33196 maxAlloc=11252)
[20013] Uptime 20s  Free heap: 32500 MaxAlloc: 9204  RSSI -67dBm
[20013] Twitch connect SKIPPED: heap too low (maxAlloc=9204)
[30009] BLE connect SKIPPED: heap too low (free=33136 maxAlloc=9204)
[30020] Uptime 30s  Free heap: 33152 MaxAlloc: 9204  RSSI -64dBm
[30021] Twitch connect SKIPPED: heap too low (maxAlloc=9204)
[40022] Uptime 40s  Free heap: 33412 MaxAlloc: 9204  RSSI -65dBm
[40022] Twitch connect SKIPPED: heap too low (maxAlloc=9204)
[45015] BLE connect SKIPPED: heap too low (free=33348 maxAlloc=9204)
[50024] Uptime 50s  Free heap: 33412 MaxAlloc: 9204  RSSI -65dBm
[50024] Twitch connect SKIPPED: heap too low (maxAlloc=9204)

The two biggest single hits are the **VLW font load** (loses 115KB total heap, 37KB MaxAlloc) and **WiFi connect** (loses 56KB heap, 55KB MaxAlloc) — both are normal, expected, one-time costs of stacks you actually need. The real problem is different: **MaxAlloc keeps bleeding downward continuously afterward**, from 16372 → 14324 → 11252 → 9204, even while nothing is actively connecting.

## Two Separate Findings

**1. Font loading + WiFi + mDNS + ArduinoOTA together consume ~181KB of heap and ~98KB of MaxAlloc — this is expected and unavoidable overhead**, not a bug. Loading a 5,568-glyph font array, bringing up WiFi's internal buffers, and starting mDNS/OTA services are all legitimately heavy on a chip with only ~320KB total RAM. This isn't fragmentation — it's real memory being used by things you need running.

**2. The ongoing decay from 16372 → 9204 after boot completes, with nothing new connecting, is heap fragmentation from small allocations accumulating in `loop()`** — most likely from `String` concatenation in `logMsg()`, which is called every single loop iteration and constructs multiple temporary `String` objects (concatenation, `substring()`, etc.) that get allocated and freed constantly, progressively fragmenting the remaining free space into smaller pieces over time.

## Why This Matters for Your Original Problem

At 9204 bytes MaxAlloc, you're now **permanently below the ~20-30KB threshold BLE and TLS need**, meaning BLE and Twitch will never successfully connect again after roughly the 20-second mark, without a reboot. This isn't a temporary dip during uploads anymore — it's a slow, one-way ratchet down to a stuck floor.

## The Real Fix: Reduce String Churn in Hot Paths

**1. Replace `String` concatenation in `logMsg()` with a fixed-size char buffer:**

```cpp
void logMsg(const String &msg) {
  char line[160];
  snprintf(line, sizeof(line), "[%lu] %s\n", millis(), msg.c_str());
  size_t len = strlen(line);
  for (size_t i = 0; i < len; i++) {
    logBuffer[logHead] = line[i];
    logHead = (logHead + 1) % LOG_BUF_SIZE;
    if (logHead == 0) logWrapped = true;
  }
  Serial.print(line);
}
```

This avoids allocating a new `String` object with `String(millis()) + msg` every single call — currently every loop-cycle log line creates and destroys at least 2-3 temporary String heap allocations.

**2. Audit `loop()` for repeated String allocations in the heartbeat block:**

```cpp
logMsg("Uptime " + String(now/1000) + "s  Free heap: " + String(ESP.getFreeHeap()) +
       " MaxAlloc: " + String(ESP.getMaxAllocHeap()) + " RSSI " + String(rssi) + "dBm");
```

This single line creates roughly 5 temporary String objects every 10 seconds. Replace with:

```cpp
char buf[128];
snprintf(buf, sizeof(buf), "Uptime %lus  Free heap: %u MaxAlloc: %u RSSI %ddBm",
         now/1000, ESP.getFreeHeap(), ESP.getMaxAllocHeap(), rssi);
logMsg(buf);
```

**3. Call `heap_caps_check_integrity_all(true)` temporarily to confirm the fragmentation source**, or simpler: watch whether MaxAlloc still decays with the `logMsg` heartbeat commented out entirely for one test run — if it stabilizes, `String` churn in your logging is confirmed as the cause.

## Priority Order

Fix `logMsg()` and the heartbeat log line first — those run every loop cycle and every 10 seconds respectively, making them the most likely source of continuous fragmentation. Apply both changes, reflash, and watch whether MaxAlloc stabilizes instead of continuing to decay past the 20-second mark.

