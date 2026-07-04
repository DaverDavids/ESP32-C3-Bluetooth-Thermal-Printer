<img src="https://r2cdn.perplexity.ai/pplx-full-logo-primary-dark%402x.png" style="height:64px;margin-right:32px"/>

# .........[6056]

WiFi OK: 192.168.7.150
[6058] mDNS: [http://c3printer.local](http://c3printer.local)
[6059] Connecting to Twitch IRC...
[6225] Twitch failed
[6226] Ready!
[10004] Uptime 10s  Free heap: 32500 MaxAlloc: 10228  RSSI -64dBm
[10004] Connecting to Twitch IRC...
[10182] Twitch failed
[13104] Upload start: /unifont_cjk.vlw
[13117] LittleFS free: 675840 bytes
[15988] Upload end: 307720 bytes written
[16015] Connecting: 56:17:a1:30:0d:dc
E (16515) BLE_INIT: Malloc failed
E (16515) BLE_INIT: esp_bt_controller_init -4

E (16516) BLE_INIT: controller init failed

Guru Meditation Error: Core  0 panic'ed (Store access fault). Exception was unhandled.

Core  0 register dump:
MEPC    : 0x42013ce8  RA      : 0x42002b68  SP      : 0x3fcac380  GP      : 0x3fc95c00
TP      : 0x3fcac4c0  T0      : 0x4209ebec  T1      : 0x10000000  T2      : 0x420a39f6
S0/FP   : 0x3fc9b000  S1      : 0x3fca2000  A0      : 0x00000000  A1      : 0x3fcb9f08
A2      : 0x00000004  A3      : 0x00000003  A4      : 0x00000000  A5      : 0x3c14eddc
A6      : 0xa0000000  A7      : 0x0000000a  S2      : 0x00000000  S3      : 0x3fca2000
S4      : 0x00000000  S5      : 0x00000000  S6      : 0x00000000  S7      : 0x00000000
S8      : 0x00000000  S9      : 0x00000000  S10     : 0x00000000  S11     : 0x00000000
T3      : 0x00000000  T4      : 0x420a3864  T5      : 0x00002033  T6      : 0x420a3924
MSTATUS : 0x00001881  MTVEC   : 0x40380001  MCAUSE  : 0x00000007  MTVAL   : 0x00000010
MHARTID : 0x00000000

Stack memory:
3fcac380: 0x3fcbffbc 0x00000400 0x00000000 0x00000000 0x00000008 0xdb1c9dc6 0x3fca2000 0x00000000
3fcac3a0: 0x00000000 0x00000000 0x0c000000 0xcf4e0494 0x00000000 0x00000000 0x00000000 0x3fc9b000
3fcac3c0: 0x3fca2000 0x3fca2000 0x00003e85 0x42005974 0x00000000 0x00000000 0x00000000 0x00000000
3fcac3e0: 0x00000000 0x00000000 0x040008b4 0x00000000 0x4038d1bc 0x4038d1ac 0x3fcac470 0x3fc95c00
3fcac400: 0x3fcac4c0 0xa8c09607 0x3fc9c000 0x5000c906 0x3fca2000 0x42022000 0x00000001 0x00000001
3fcac420: 0x3fca2000 0x3fca2000 0x00000001 0x600c0028 0x00000001 0x3fc9c000 0x00000000 0x00000000
3fcac440: 0x00000000 0x00000000 0x00000000 0x00000000 0x00000000 0x00000000 0x00000000 0xcf4e0494
3fcac460: 0x00000014 0x00000000 0x00000000 0x00000000 0x00000000 0x42022000 0x3fca2000 0x42021474
3fcac480: 0x00000000 0x00000000 0x00000000 0x4038cfc8 0x00000000 0x00000000 0x00000000 0x00000000
3fcac4a0: 0x00000000 0x00000000 0x00000000 0x00000000 0x00000000 0xa5a5a5a5 0xa5a5a5a5 0xa5a5a5a5
3fcac4c0: 0xa5a5a5a5 0xa5a5a5a5 0xa5a5a5a5 0xbaad5678 0x158b4fb6 0x3e06545b 0x9df7e4e6 0x63f4509e
3fcac4e0: 0x954deaa0 0xc5c14c87 0xb33f0ea6 0x4dcf4595 0x8f2ba7d9 0xe6faf225 0x165819a1 0x0cc68a43
3fcac500: 0x349e72e7 0x7d3be058 0xa56e6bb9 0x2fe6667a 0xf636ea52 0xfbdb549d 0x413e49f6 0xb8a9233d
3fcac520: 0xd7a60011 0x52f0a8ab 0x18d31bef 0x766ea26e 0x599aa0e6 0x3c125658 0x65d37abb 0x1c6e6ffa
3fcac540: 0x3d4f52e4 0xc2c9e963 0xc19081c1 0xb85c4eeb 0xc2b3829a 0x85bf8319 0x90ee1098 0x6a101a46
3fcac560: 0x5243040c 0xac99f5e9 0x5d2e8bcc 0xaf2bbe3f 0xcc774a58 0xf657fed5 0x1bbd377d 0x39dfbc46
3fcac580: 0x47ec7cb3 0x1ddbe6f8 0x861fc710 0xd9d0a99c 0x7b8e5c7c 0x11e584b2 0x4ce33213 0x835cef5d
3fcac5a0: 0x07cd0841 0xd9e10ac2 0x4642cd06 0xeca856d7 0xbbd557d6 0x2a389a3c 0x6f501c8c 0x73c75957
3fcac5c0: 0x46142c62 0x6dda99f8 0x5a32f278 0x3b45bb22 0x33e0546c 0x7c93dc7d 0x64fbaa2c 0xddbd1c61
3fcac5e0: 0xecda1964 0x171bf4ac 0x5609b45a 0xeb3cf005 0xf536ab99 0xc2741a32 0x001fb2d7 0x7ed655da
3fcac600: 0xbbe79be0 0xf75cfdd5 0xcdd11319 0x64b9440b 0xdf174546 0x9bff8462 0x948f1b69 0xb537f2ef
3fcac620: 0x9400b80c 0xb291fb5b 0x68cf7fde 0x9278a4fa 0xfbf2dbf6 0xb7eab2d9 0x9ca33a4b 0x039e3861
3fcac640: 0x4123c4c2 0xa370947a 0x44ac6df3 0x78dfe698 0x176adc3f 0x26194ee1 0x3e8e3290 0xd558074d
3fcac660: 0xcd353af0 0xcf85a31b 0x5393b8c1 0xfdcb7ac6 0x4a5bd06c 0xa0297087 0x231e4f52 0x507d2181
3fcac680: 0xdbc7ffec 0x77ff8718 0x4f665067 0xf84c8082 0xe6ae2c78 0xe7ac12b7 0x9ae0bf3a 0xe59a605e
3fcac6a0: 0xc26c3019 0x3bae9b40 0x3a946f23 0x412f676f 0x66d7d0ca 0x29ff604a 0x4ff2be27 0xc2b068c1
3fcac6c0: 0x3fcaa4bc 0x00000170 0xabba1234 0x0000015c 0x3fcabc60 0x00003e83 0x3fc9c5e4 0x3fc9c5e4
3fcac6e0: 0x3fcac6d0 0x3fc9c5dc 0x00000018 0x3fcbba84 0x3fcbba84 0x3fcac6d0 0x00000000 0x00000001
3fcac700: 0x3fcaa4cc 0x706f6f6c 0x6b736154 0x00000000 0x00000000 0x3fcac4c0 0x00000005 0x00000000
3fcac720: 0x00000001 0x00000000 0x3fcb1298 0x4202b3d4 0x0023d81b 0x0000000b 0x3fca2fc0 0x3fca3028
3fcac740: 0x3fca3090 0x00000000 0x00000000 0x00000001 0x00000000 0x00000000 0x00000000 0x4202af5a
3fcac760: 0x00000000 0x00000000 0x00000000 0x00000000 0x00000000 0x00000000 0x00000000 0x00000000

ELF file SHA256: ce7cf10ab

E (4973) esp_core_dump_flash: Core dump flash config is corrupted! CRC=0x7bd5c66f instead of 0x0
E (4981) esp_core_dump_elf: Elf write init failed!
E (4986) esp_core_dump_common: Core dump write failed with error=-1
Rebooting...
ESP-ROM:esp32c3-api1-20210207
Build:Feb  7 2021
rst:0xc (RTC_SW_CPU_RST),boot:0xd (SPI_FAST_FLASH_BOOT)
Saved PC:0x4038bbac
SPIWP:0xee
mode:DIO, clock div:1
load:0x3fcd5820,len:0x110c
load:0x403cbf10,len:0xb54
load:0x403ce710,len:0x2f8c
entry 0x403cbf10
E (258) esp_core_dump_flash: No core dump partition found!
E (258) esp_core_dump_flash: No core dump partition found!
[1010]

ESP32-C3 Thermal Printer (VLW LittleFS Fonts)
[1013] LittleFS mounted OK
[1030] LittleFS total: 983040 used: 618496
[1034] Config loaded
[1242] VLW loaded: /unifont_cjk.vlw (5568 glyphs)
.......[5074]
WiFi OK: 192.168.7.150
[5076] mDNS: [http://c3printer.local](http://c3printer.local)
[5077] Connecting to Twitch IRC...
[5250] Twitch failed
[5251] Ready!
[10008] Uptime 10s  Free heap: 33868 MaxAlloc: 12788  RSSI -64dBm
[10008] Connecting to Twitch IRC...
[10136] Twitch failed

This is worse than the earlier symptom — it's now a **hard crash**, not just an aborted upload. The sequence is clear: upload finishes at `[15988]`, then `connectPrinter()` fires at `[16016]` while heap is still depleted from the file write, `esp_bt_controller_init` fails to allocate its required block (`Malloc failed`, error `-4`), and critically, **`BLEDevice::init()` doesn't check that return code** — it proceeds to dereference an invalid pointer, causing the "Store access fault" panic and a full reboot.

## Root Cause, Precisely

BLE controller init needs a large contiguous heap allocation (typically 50KB+) to set up its internal buffers. Right after a ~300KB LittleFS write, your heap is temporarily depleted/fragmented enough that this allocation fails. Arduino-ESP32's `BLEDevice::init()` has a known defect: it doesn't gracefully handle `esp_bt_controller_init()` returning an error — it just carries on and crashes.

## Fix 1 — Block BLE Connect Attempts Until Heap Recovers

In `c3Printer.ino`, modify `connectPrinter()` to refuse to run if heap is too low:

```cpp
void connectPrinter() {
  if (ESP.getFreeHeap() < 40000 || ESP.getMaxAllocHeap() < 30000) {
    logMsg("BLE connect SKIPPED: heap too low (free=" + String(ESP.getFreeHeap()) +
           " maxAlloc=" + String(ESP.getMaxAllocHeap()) + ")");
    return;
  }
  logMsg("Connecting: " + printerMAC);
  BLEDevice::init("ESP32-C3-Printer");
  if (pClient) delete pClient;
  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());
  bleState = BLECONNECTING;
  pClient->connect(BLEAddress(printerMAC.c_str()), true);
}
```

This is the critical fix — it prevents the crash entirely by refusing the risky operation instead of letting the SDK crash on a failed malloc.

## Fix 2 — Delay BLE Reconnect Right After Any Upload

In `loop()`, extend the retry cooldown specifically after an upload just finished, giving heap time to settle:

```cpp
static unsigned long lastUploadFinish = 0;
```

Set this inside `UPLOAD_FILE_END` in `handleUpload()`:

```cpp
else if (upload.status == UPLOAD_FILE_END) {
  if (uploadFile) uploadFile.close();
  logMsg("Upload end. Total written: " + String(uploadWrittenBytes) + " expected: " + String(upload.totalSize));
  uploadInProgress = false;
  lastUploadFinish = millis();
  if (uploadWrittenBytes != upload.totalSize) {
    uploadFailed = true;
    logMsg("SIZE MISMATCH — upload incomplete!");
  }
}
```

Then in `loop()`'s BLE retry condition, add a cooldown window after uploads:

```cpp
if (bleState == BLEIDLE && !printerConnected &&
    now - lastPrinterRetry > 15000 &&
    now - lastUploadFinish > 10000) {
  lastPrinterRetry = now;
  connectPrinter();
}
```

This ensures BLE won't attempt reconnection for at least 10 seconds after any file finishes uploading, giving the heap time to defragment/recover.

## Fix 3 — Same Heap Guard for Twitch's TLS Connection

Your log also shows `Twitch failed` repeatedly under low heap — `WiFiClientSecure` TLS handshakes need a large contiguous buffer too. Add the same guard to `connectTwitch()`:

```cpp
void connectTwitch() {
  if (ESP.getMaxAllocHeap() < 30000) {
    logMsg("Twitch connect SKIPPED: heap too low (maxAlloc=" + String(ESP.getMaxAllocHeap()) + ")");
    twitchConnected = false;
    return;
  }
  logMsg("Connecting to Twitch IRC...");
  ...
```


## Why This Set of Fixes Works

Each of these guards stops the device from attempting a large, heap-hungry operation (BLE init, TLS handshake) at the exact moments your logs show MaxAlloc collapsing into the 6-12KB range — instead of crashing on a failed malloc, the device now logs a skip message and simply retries later once heap has recovered. This trades a few extra seconds of delay in reconnecting BLE/Twitch after an upload for eliminating the panic/reboot cycle entirely.

Apply all three, reflash, and repeat the `unifont_cjk.vlw` upload test — you should now see "BLE connect SKIPPED" in the log instead of a crash, with BLE successfully connecting once heap recovers a few seconds later.

