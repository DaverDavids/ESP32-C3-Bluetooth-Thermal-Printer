<img src="https://r2cdn.perplexity.ai/pplx-full-logo-primary-dark%402x.png" style="height:64px;margin-right:32px"/>

# yes lets try this. describe concisely the changes I need to make

Here are the changes needed:

## 1. `setup()` — remove LittleFS mount, just load font metadata

```cpp
// REMOVE these lines entirely:
if (!LittleFS.begin(true, "/littlefs", 4)) ...
logMsg("LittleFS OK. total=...")

// REPLACE with mount-read-unmount:
LittleFS.begin(true, "/littlefs", 4);
loadVlw(fontBasic, "/unifont_basic.vlw");
loadVlw(fontCJK,   "/unifont_cjk.vlw");
LittleFS.end();
```


## 2. `loadVlw()` — mount and unmount internally

```cpp
bool loadVlw(VlwFont& f, const char* path) {
  LittleFS.begin(true, "/littlefs", 4);
  File file = LittleFS.open(path, "r");
  // ... existing read logic unchanged ...
  file.close();
  LittleFS.end();  // ← add this
  // ... rest unchanged ...
}
```


## 3. `printToThermal()` — mount at start, unmount at end

```cpp
bool printToThermal(...) {
  LittleFS.begin(false, "/littlefs", 4);  // false = don't reformat
  // ... all existing logic unchanged ...
  if (fBasicHandle) fBasicHandle.close();
  if (fCJKHandle)   fCJKHandle.close();
  LittleFS.end();  // ← add before feedPaper/return
  if(feedLines > 0) feedPaper(feedLines);
  return true;
}
```


## 4. `handleUpload()` — already mounts/unmounts implicitly via LittleFS calls, just add `LittleFS.end()` after the `loadVlw()` reload calls at `UPLOAD_FILE_END`

```cpp
if (upload.filename == "unifont_basic.vlw") loadVlw(fontBasic, "/unifont_basic.vlw");
if (upload.filename == "unifont_cjk.vlw")   loadVlw(fontCJK,   "/unifont_cjk.vlw");
LittleFS.end();  // ← add this
```


## 5. `/fsinfo` and `/delete_file` routes — wrap with mount/unmount

```cpp
server.on("/fsinfo", []() {
  LittleFS.begin(false, "/littlefs", 4);
  // ... existing logic ...
  LittleFS.end();
  server.send(200, "text/html", out);
});
```

Same pattern for `/delete_file`.

The key insight is `LittleFS.begin()` is idempotent if already mounted, and `LittleFS.end()` fully releases the heap. Should recover the fragmentation that's blocking BLE+Twitch coexistence.

