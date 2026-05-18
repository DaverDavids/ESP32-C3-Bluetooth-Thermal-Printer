
## The Two Line Fixes

**Line ~374** — fix wrap budget to exactly match margins:

```cpp
// BEFORE:
int maxTextWidth  = renderW - 6;

// AFTER:
int maxTextWidth  = renderW - 4;  // 2px margin each side matches drawing margins
```

**Line ~437** — fix center-align x to respect left margin:

```cpp
// BEFORE:
if     (align == 1) x = max(0, (renderW - tw) / 2);

// AFTER:
if     (align == 1) x = max(2, (renderW - tw) / 2);
```

The second fix ensures that even if a very wide word can't be broken (no spaces), the text starts at `x=2` minimum rather than `x=0`, keeping it consistent with left and right alignment which both clamp to `2`.

