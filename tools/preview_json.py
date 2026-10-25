"""Render the first (or a chosen) frame of an ASCII Player JSON scene to PNG."""
import argparse
import json
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


def xterm_rgb(index: int):
    base = [
        (0, 0, 0), (128, 0, 0), (0, 128, 0), (128, 128, 0),
        (0, 0, 128), (128, 0, 128), (0, 128, 128), (192, 192, 192),
        (128, 128, 128), (255, 0, 0), (0, 255, 0), (255, 255, 0),
        (0, 0, 255), (255, 0, 255), (0, 255, 255), (255, 255, 255),
    ]
    levels = [0, 95, 135, 175, 215, 255]
    if index < 16:
        return base[index]
    if index < 232:
        n = index - 16
        return levels[n // 36], levels[(n % 36) // 6], levels[n % 6]
    gray = 8 + (index - 232) * 10
    return gray, gray, gray


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("scene")
    parser.add_argument("-o", "--out", default="preview.png")
    parser.add_argument("--frame", type=int, default=0)
    parser.add_argument("--scale", type=int, default=2)
    args = parser.parse_args()
    data = json.loads(Path(args.scene).read_text(encoding="utf-8"))
    frame = data["frames"][min(args.frame, len(data["frames"]) - 1)]
    cols, rows = data["width"], data["height"]
    font = ImageFont.truetype(r"C:\Windows\Fonts\consolab.ttf", 18 * args.scale)
    char_width = font.getlength("M")
    bbox = font.getbbox("M")
    cell_h = int((bbox[3] - bbox[1]) * 1.45)
    margin = 18 * args.scale
    img = Image.new("RGB", (int(cols * char_width + margin * 2), int(rows * cell_h + margin * 2)),
                    (4, 6, 9))
    draw = ImageDraw.Draw(img)
    colors = frame.get("p")
    for y, line in enumerate(frame["c"]):
        for x, ch in enumerate(line):
            if ch == " ":
                continue
            color = (235, 240, 245)
            if colors:
                pair = colors[y][x * 2:x * 2 + 2]
                color = xterm_rgb(int(pair, 16))
            draw.text((margin + x * char_width, margin + y * cell_h - bbox[1]),
                      ch, font=font, fill=color)
    img.save(args.out)
    print(f"wrote {args.out} ({img.width}x{img.height})")


if __name__ == "__main__":
    main()
