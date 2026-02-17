#!/usr/bin/env python3
"""
raw_convert.py — PNG/JPEG → XRGB8888 raw 변환

사용법:
    python3 raw_convert.py input.png output.raw [width height]

출력 형식: XRGB8888 (4바이트/픽셀, 알파 = 0x00)
컴포지터가 /usr/share/wallpaper.raw로 로드.

크기 미지정 시 원본 크기 그대로 변환.
크기 지정 시 리사이즈 (Pillow LANCZOS).
"""

import sys
import struct

try:
    from PIL import Image
except ImportError:
    print("ERROR: Pillow 필요 — pip install Pillow", file=sys.stderr)
    sys.exit(1)


def convert(inpath, outpath, width=None, height=None):
    img = Image.open(inpath).convert("RGB")

    if width and height:
        img = img.resize((width, height), Image.LANCZOS)

    w, h = img.size
    pixels = img.load()

    with open(outpath, "wb") as f:
        for y in range(h):
            for x in range(w):
                r, g, b = pixels[x, y]
                # XRGB8888: 0x00RRGGBB (little-endian uint32)
                f.write(struct.pack("<I", (r << 16) | (g << 8) | b))

    print(f"변환 완료: {w}x{h} → {outpath} ({w*h*4} bytes)")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(f"사용법: {sys.argv[0]} input.png output.raw [width height]")
        sys.exit(1)

    w = int(sys.argv[3]) if len(sys.argv) > 4 else None
    h = int(sys.argv[4]) if len(sys.argv) > 4 else None

    convert(sys.argv[1], sys.argv[2], w, h)
