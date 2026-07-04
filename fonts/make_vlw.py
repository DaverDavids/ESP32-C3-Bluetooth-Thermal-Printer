from fontTools import ttLib
from PIL import ImageFont, Image, ImageDraw
import struct, os

def make_vlw(ttf_paths, size, output_path, codepoint_ranges):
    # Load all fonts — first one is primary, rest are fallbacks
    pil_fonts = [ImageFont.truetype(p, size) for p in ttf_paths]
    
    # Build a cmap from each font — {codepoint: font_index}
    cmaps = []
    for p in ttf_paths:
        tt = ttLib.TTFont(p)
        cmaps.append(tt.getBestCmap() or {})
    
    def get_font_for_cp(cp):
        for i, cmap in enumerate(cmaps):
            if cp in cmap:
                return pil_fonts[i]
        return None

    # Collect codepoints
    codepoints = []
    for start, end in codepoint_ranges:
        for cp in range(start, end + 1):
            if get_font_for_cp(cp) is not None:
                codepoints.append(cp)

    print(f"Found {len(codepoints)} glyphs across {len(ttf_paths)} fonts...")

    glyphs = []
    for cp in codepoints:
        ch = chr(cp)
        font_pil = get_font_for_cp(cp)
        try:
            bbox = font_pil.getbbox(ch)
        except:
            continue
        if bbox is None:
            continue

        gw = bbox[2] - bbox[0]
        gh = bbox[3] - bbox[1]
        if gw <= 0 or gh <= 0:
            try:
                adv = int(font_pil.getlength(ch))
            except:
                adv = size // 2
            glyphs.append({'cp': cp, 'w': 0, 'h': 0,
                           'advance': adv, 'x_off': 0, 'y_off': 0, 'bitmap': b''})
            continue

        img = Image.new("L", (gw + 2, gh + 2), 0)
        draw = ImageDraw.Draw(img)
        draw.text((-bbox[0] + 1, -bbox[1] + 1), ch, font=font_pil, fill=255)

        row_bytes = (gw + 7) // 8
        bitmap = bytearray()
        for y in range(gh):
            row = 0
            for x in range(gw):
                px = img.getpixel((x + 1, y + 1))
                if px > 127:
                    row |= (1 << (7 - (x % 8)))
                if (x % 8 == 7) or (x == gw - 1):
                    bitmap.append(row)
                    row = 0

        try:
            adv = int(font_pil.getlength(ch))
        except:
            adv = gw

        glyphs.append({'cp': cp, 'w': gw, 'h': gh,
                       'advance': adv, 'x_off': bbox[0], 'y_off': bbox[1],
                       'bitmap': bytes(bitmap)})

    print(f"Writing {len(glyphs)} glyphs to {output_path}...")
    with open(output_path, 'wb') as f:
        f.write(struct.pack('>iiiiii', len(glyphs), 1, size, 0, 0, 0))
        for g in glyphs:
            f.write(struct.pack('>iiiiii',
                g['cp'], g['h'], g['w'],
                g['advance'], g['x_off'], g['y_off']))
        for g in glyphs:
            f.write(g['bitmap'])

    kb = os.path.getsize(output_path) / 1024
    print(f"Done! {output_path} = {kb:.1f} KB")


# Basic file — Unifont primary, NotoEmoji fallback for emoji ranges
make_vlw(
    ttf_paths=[
        "unifont-17.0.05.otf",
        "unifont_upper-17.0.05.otf",      # ← add this as fallback for emoji
    ],
    size=16,
    output_path="unifont_basic.vlw",
    codepoint_ranges=[
        (0x0000, 0x04FF),
        (0x0500, 0x06FF),
        (0x0900, 0x097F),
        (0x1E00, 0x1FFF),
        (0x2000, 0x28FF),
        (0x2600, 0x27BF),   # Symbols + Dingbats (covered by Unifont first)
        (0xFF00, 0xFFEF),
        (0x1F300, 0x1F9FF), # Emoji — falls through to OpenMoji-Black
    ]
)

make_vlw(
    ttf_paths=["unifont-17.0.05.otf"],
    size=16,
    output_path="unifont_cjk.vlw",
    codepoint_ranges=[
        (0x3040, 0x30FF),   # Hiragana + Katakana (unchanged)
        (0x4E00, 0x62FF),   # Most frequent ~5,600 CJK ideographs by block position
    ]
)