<img src="https://r2cdn.perplexity.ai/pplx-full-logo-primary-dark%402x.png" style="height:64px;margin-right:32px"/>

# still cuts off. I printed this: "Hello! Café resumé naïve Привет ツ"

in medium font, centered. the beginning of it starts about 5 or 6 mm from the left. and then it cuts off the 'e' in naive about 3mm from the left. the word after naive doesn't get printed. describe all changes for me to make in code, be very concise

The problem is the **size normalization** — the font heights differ but their widths weren't normalized. Let me look at exactly what's happening.

With `PRINT_SCALE 2`, `renderW = 200px`. Medium font (`8x13`) is 8px wide per char. The unicode fallback (`unifont_t_japanese1`) is 16px wide per char — **twice as wide** as the Latin font.

When `wordWrap` measures the line, it correctly accounts for the mixed fonts. But the issue is the **size normalization introduced for the fallback font**: the fallback renders at its native 16px width in the canvas, but the Latin font only renders at 8px — so the fallback glyphs are **2× wider relative to Latin** in the pre-scale canvas. After PRINT_SCALE×2 upscale, the fallback chars are effectively **4× the printer width** of Latin chars.

Here's the specific issue: the wrapping correctly measures `tw`, but when the line is drawn, the fallback-font segment (ツ) takes 16px in the canvas, which after 2× scale = 32 printer pixels — that's fine. But `Привет` uses the fallback too, and those 6 chars × 16px = 96px in canvas → 192px on paper. Combined with the Latin part, `tw` from `measureLine` matches the draw width, so wrapping is correct.

**The real bug:** `maxTextWidth = renderW - 4 = 196`, but `renderW = 200`. When center-aligned: `x = max(2, (200 - tw) / 2)`. If `tw = 196`, `x = 2`. Drawing goes from 2 to 198 — fine. But if `tw` slightly *exceeds* `renderW - 4` due to measurement rounding (U8g2 `getUTF8Width` can round differently than actual draw), the line is not wrapped and overflows right, AND center-align pushes `x` below 2, so left side clips too.

## Changes to make

**1. Add a safety margin to `maxTextWidth` — change in `printToThermal`:**

```cpp
// was:
int maxTextWidth  = renderW - 4;  // 2px margin each side matches drawing margins

// change to:
int maxTextWidth  = renderW - 8;  // 4px margin each side; guards against measurement rounding
```

**2. Clamp `tw` in the alignment block so a line that slipped through wrapping can't overflow — add one line before the `x` calculation:**

```cpp
        tw = min(tw, renderW - 4);  // clamp: never let a line exceed drawable width

        int x = 2;
        if     (align == 1) x = max(2, (renderW - tw) / 2);
        else if(align == 2) x = max(2, renderW - tw - 2);
```

That `tw = min(tw, renderW - 4)` ensures center/right alignment math can never produce a negative `x`, and left-align at `x=2` won't draw past `renderW - 2` for any line that fit within the clamp.

**3. In `wordWrap`, the `measureLine` lambda has a bug where single-char segments don't accumulate the full run correctly** — the `segEnd` stays at `i` (after first char) when there's only one char before a font switch. This causes under-measurement. Fix the segment collection:

```cpp
// In the measureLine lambda, replace the inner while loop and seg assignment:
      seg = line.substring(segStart, i);  // start with first char already consumed
      while (i < len) {
        int before = i;
        uint32_t cp2 = nextCodepoint(line, i);
        if (!isLatinCodepoint(cp2) != useFallback) { i = before; break; }
      }
      seg = line.substring(segStart, i);  // now seg covers full run including first char
      u8m.setFont(useFallback ? (se ? se->unicodeFallback : UNICODE_FALLBACK_FONT) : primaryFont);
      total += u8m.getUTF8Width(seg.c_str());
```

The `segEnd` variable and the ternary `segEnd > segStart ? segEnd : i` are the culprit — they exist to handle the single-char case but introduce a subtle off-by-one. Removing them and just using `i` directly after the inner loop is cleaner and correct.

