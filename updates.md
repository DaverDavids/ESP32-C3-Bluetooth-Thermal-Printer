<img src="https://r2cdn.perplexity.ai/pplx-full-logo-primary-dark%402x.png" style="height:64px;margin-right:32px"/>

# describe the exact changes we need to make to the code

Here are the exact changes needed. Only 3 areas to touch in [c3Printer.ino](https://github.com/DaverDavids/c3Printer/blob/main/c3Printer.ino):

## 1. Add a codepoint range detector (new function)

Add this right after `isLatinCodepoint()`:

```cpp
bool isBrailleOrBlock(uint32_t cp) {
  return (cp >= 0x2500 && cp <= 0x257F) ||  // Box Drawing
         (cp >= 0x2580 && cp <= 0x259F) ||  // Block Elements
         (cp >= 0x2800 && cp <= 0x28FF);    // Braille Patterns
}
```


## 2. Add braille fallback font to `SizeEntry` and `SIZE_TABLE`

In the `SizeEntry` struct, add one field:

```cpp
struct SizeEntry {
  // ... existing fields ...
  const uint8_t* brailleFallback;  // ADD THIS
};
```

Then update every row in `SIZE_TABLE` to include `u8g2_font_unifont_t_75` as the last value:

```cpp
const SizeEntry SIZE_TABLE[] = {
  { FSIZE_SMALL,  "Small",   u8g2_font_6x10_tf,      u8g2_font_7x13B_tf,
    10, u8g2_font_unifont_t_japanese1, 16, u8g2_font_unifont_t_75 },
  { FSIZE_MEDIUM, "Medium",  u8g2_font_8x13_tf,       u8g2_font_8x13B_tf,
    13, u8g2_font_unifont_t_japanese1, 16, u8g2_font_unifont_t_75 },
  { FSIZE_LARGE,  "Large",   u8g2_font_9x15_tf,       u8g2_font_9x15B_tf,
    15, u8g2_font_unifont_t_japanese1, 16, u8g2_font_unifont_t_75 },
  { FSIZE_XLARGE, "X-Large", u8g2_font_10x20_tf,      u8g2_font_10x20_tf,
    20, u8g2_font_unifont_t_japanese2, 16, u8g2_font_unifont_t_75 },
  { FSIZE_HUGE,   "Huge",    u8g2_font_logisoso28_tf, u8g2_font_logisoso28_tf,
    28, u8g2_font_unifont_t_japanese2, 16, u8g2_font_unifont_t_75 },
};
```


## 3. Add a font-picker helper (new function)

Add this after `getSizeEntry()` — it replaces the scattered inline ternaries for font selection:

```cpp
const uint8_t* pickFont(uint32_t cp, const uint8_t* primaryFont, const SizeEntry* se) {
  if (isLatinCodepoint(cp))  return primaryFont;
  if (isBrailleOrBlock(cp))  return se ? se->brailleFallback : u8g2_font_unifont_t_75;
  return se ? se->unicodeFallback : UNICODE_FALLBACK_FONT;
}
```


## 4. Update `drawLineMixed` to use `pickFont`

Replace the font-selection logic inside the `while (i < len)` loop. The current code only checks `isLatinCodepoint` — change it to use `pickFont` so runs are grouped by **which font they use**:

```cpp
int drawLineMixed(U8G2_FOR_ADAFRUIT_GFX& u8g2, const String& line,
                  int x, int y, int fgColor,
                  const uint8_t* primaryFont, bool bold,
                  const SizeEntry* se = nullptr) {
  int curX = x;
  int i = 0, len = line.length();
  while (i < len) {
    int segStart = i;
    uint32_t cp = nextCodepoint(line, i);
    const uint8_t* useFont = pickFont(cp, primaryFont, se);
    // collect run of chars using the same font
    while (i < len) {
      int before = i;
      uint32_t cp2 = nextCodepoint(line, i);
      if (pickFont(cp2, primaryFont, se) != useFont) { i = before; break; }
    }
    String seg = line.substring(segStart, i);
    u8g2.setFont(useFont);
    u8g2.setForegroundColor(fgColor);
    u8g2.setCursor(curX, y);
    u8g2.print(seg);
    if (bold && useFont == primaryFont) {
      u8g2.setCursor(curX + 1, y);
      u8g2.print(seg);
    }
    curX += u8g2.getUTF8Width(seg.c_str());
  }
  return curX - x;
}
```


## 5. Update `wordWrap`'s `measureLine` lambda the same way

Replace the inner lambda's font-selection ternary with `pickFont`:

```cpp
auto measureLine = [&](const String& line) -> int {
  int total = 0, i = 0, len = line.length();
  while (i < len) {
    int segStart = i;
    uint32_t cp = nextCodepoint(line, i);
    const uint8_t* useFont = pickFont(cp, primaryFont, se);
    while (i < len) {
      int before = i;
      uint32_t cp2 = nextCodepoint(line, i);
      if (pickFont(cp2, primaryFont, se) != useFont) { i = before; break; }
    }
    String seg = line.substring(segStart, i);
    u8m.setFont(useFont);
    total += u8m.getUTF8Width(seg.c_str());
  }
  return total;
};
```


***

That's all of it. The `pickFont` helper is the key unification — it means both rendering and measurement use identical font-routing logic, so word wrap measurements stay accurate for Braille art too. Want me to commit these changes directly to the repo?

