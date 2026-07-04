

## Issue 1: Blank Web Page

The HTML was moved out to `#include "html.h"` , but `handleRoot` still does:

```cpp
server.send(200, "text/html; charset=UTF-8", htmlPage);
```

`htmlPage` is whatever `html.h` defines. The blank page means one of:

- **`html.h` doesn't exist in the sketch folder** — it compiled fine (the include resolved) but `htmlPage` is an empty string or a null pointer being cast to String. Check that `html.h` is actually sitting next to `c3Printer.ino` in your local sketch directory.
- **`html.h` defines `htmlPage` as a `const char*` pointing to a string that starts with a null byte or is empty.** Open the file and verify the first character of the string literal isn't accidentally `\0`.
- **Encoding issue** — if you saved `html.h` on Windows and it has a UTF-8 BOM, the browser may refuse to render it. Make sure the file is saved as UTF-8 *without* BOM.

Quick diagnostic: hit `http://c3printer.local/` and open DevTools → Network. Check the response size. If it's 0 bytes or just `\r\n`, the string is empty. If it's a reasonable size but renders blank, it's a content/encoding issue.

Also note: **the `#include <SPIFFS.h>` is still there but you said you switched to LittleFS** — that means `SPIFFS.begin()`, `SPIFFS.open()`, `SPIFFS.exists()` etc. are all calling the wrong filesystem. If LittleFS is mounted but SPIFFS isn't, `loadVlw` will fail silently (it logs "VLW missing"), but the web server itself doesn't depend on the filesystem so this wouldn't cause a blank page on its own.

***

## Issue 2: Slower Than Before the Font Changes

This is `measureTextVlw` being called **quadratically** in `wordWrap` . Look at the wrapping loop:

```cpp
while (line.length() > 0) {
    if (measureTextVlw(line) <= maxWidth) { ... break; }   // ← full scan
    // binary search: calls measureTextVlw on substrings O(log N) times
    while (lo <= hi) {
        String test = line.substring(0, mid);
        if (measureTextVlw(test) <= maxWidth) { ... }      // ← full scan each
    }
}
```

`measureTextVlw` iterates every codepoint in the substring doing a binary search per glyph. For a long line this runs O(N log N) times inside an outer binary search — easily 50–100× slower than the old U8g2 `getUTF8Width` which was a single tight C loop over precomputed width tables in flash.

**The fix** is a single linear-scan accumulator instead of repeated full remeasures. Replace `wordWrap` with a codepoint-advance version that walks the string once, tracking cumulative width and last-space position:

```cpp
String wordWrap(const String& text, int maxWidth) {
  String result = "";
  int lineStart = 0, textLen = (int)text.length();

  while (lineStart < textLen) {
    int lineEnd = text.indexOf('\n', lineStart);
    if (lineEnd < 0) lineEnd = textLen;

    // Single linear pass: accumulate width, track last space
    int i = lineStart, lineWidth = 0, lastSpaceI = -1, lastSpaceW = 0;
    while (i < lineEnd) {
      if (text[i] == ' ') { lastSpaceI = i; lastSpaceW = lineWidth; }
      int before = i;
      uint32_t cp = nextCodepoint(text, i);
      const VlwFont* sf = nullptr;
      const VlwGlyph* g = getGlyph(cp, &sf);
      int adv = g ? g->advance : (fontBasic.size / 2);
      if (lineWidth + adv > maxWidth && lineWidth > 0) {
        // break here
        int breakAt = (lastSpaceI > lineStart) ? lastSpaceI : before;
        result += text.substring(lineStart, breakAt);
        result += '\n';
        lineStart = breakAt;
        if (lineStart < textLen && text[lineStart] == ' ') lineStart++;
        lineWidth = 0; lastSpaceI = -1;
        i = lineStart;
        lineEnd = text.indexOf('\n', lineStart);
        if (lineEnd < 0) lineEnd = textLen;
        continue;
      }
      lineWidth += adv;
    }
    result += text.substring(lineStart, lineEnd);
    if (lineEnd < textLen) result += '\n';
    lineStart = lineEnd + 1;
  }
  return result;
}
```

This is O(N) per line regardless of how many wraps occur. The old binary-search approach also had a subtle bug where `measureTextVlw` was being called on Arduino `String` substrings that each allocate heap — on the ESP32-C3 with its small heap that's a lot of malloc/free churn mid-render, which adds to the slowness.

