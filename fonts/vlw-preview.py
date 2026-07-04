from PIL import Image, ImageDraw, ImageFont

def preview_chars(ttf_path, texts, sizes=[10, 16, 20]):
    for size in sizes:
        # Load font explicitly with encoding
        font = ImageFont.truetype(ttf_path, size, encoding="unic")
        
        for label, text in texts.items():
            # Use getbbox on the font object directly — more reliable than textbbox
            lines = text.split('\n')
            line_height = size + 4
            total_h = line_height * len(lines) + 8
            
            # Measure max width across lines
            max_w = 0
            for line in lines:
                try:
                    left, top, right, bottom = font.getbbox(line)
                    max_w = max(max_w, right - left)
                except:
                    max_w = max(max_w, len(line) * size)
            
            w = max(max_w + 8, 100)
            h = total_h
            
            # Render grayscale first (antialiased), then threshold to 1-bit
            img = Image.new("RGB", (w, h), (255, 255, 255))
            draw = ImageDraw.Draw(img)
            
            y = 4
            for line in lines:
                draw.text((4, y), line, font=font, fill=(0, 0, 0))
                y += line_height
            
            # Convert to grayscale then threshold — simulates 1-bit thermal
            gray = img.convert("L")
            bw = gray.point(lambda x: 0 if x < 180 else 255)
            
            # 2x nearest-neighbor scale — simulates PRINT_SCALE=2
            scaled = bw.resize((w * 2, h * 2), Image.NEAREST)
            
            fname = f"preview_{label}_{size}px.png"
            scaled.save(fname)
            print(f"Saved {fname} ({w}x{h} -> {w*2}x{h*2})")

texts = {
    "preview":  "⠀⠀⣰⡀⠀\n⣸⣿⣷⠀\n⣿⣿⣿⣧ 😀 🔥 ⚡ 💀 🎉  Привет мир  日本語テスト  Hello café naïve    مرحبا بالعالم   ¯\_(ツ)_/¯"}

preview_chars("unifont-17.0.05.otf", texts, sizes=[10, 16, 20])