#!/usr/bin/env python3
"""Generate Shengsheng's native LVGL8 2-bit avatar; Python stdlib only.

Run from any directory. The generated include is checked in; firmware builds
do not run this script or require an image package. --check verifies it is
current; --preview writes an optional contact sheet (idle, listening, thinking,
speaking, sleepy, happy, angry, sad, in row order).
"""

import argparse
import hashlib
from pathlib import Path
import struct
import zlib

WIDTH = HEIGHT = 144
SCALE = 4
# Same original adult sprite as miniapps/tamagotchi.lua; '.' is transparent.
SPRITE = (
    ".....#..........#.....",
    "....###........###....",
    "...#####......#####...",
    "...#####oooooo#####...",
    "..#oooooooooooooooo#..",
    ".#oooooooooooooooooo#.",
    ".#oooooooooooooooooo#.",
    "#ooooo##oooooo##ooooo#",
    "#ooooo##oooooo##ooooo#",
    "#oooooooooooooooooooo#",
    "#ooooooooo##ooooooooo#",
    "#oooooooooooooooooooo#",
    ".#oooooooooooooooooo#.",
    ".#oooooooooooooooooo#.",
    "..#oooooooooooooooo#..",
    "..#oooooooooooooooo#..",
    "...##oooooooooooo##...",
    ".....############.....",
    "....###........###....",
    "...####........####...",
)
# RGBA in Python; LVGL8's lv_color32_t palette bytes are BGRA.
PALETTE = ((0, 0, 0, 0), (0x28, 0x38, 0x2D, 255),
           (0xD0, 0xD7, 0xB2, 255), (0xB8, 0xC1, 0x9B, 255))


def rect(image, x, y, w, h, color):
    assert 0 <= color < 4 and 0 <= x <= x + w <= WIDTH
    assert 0 <= y <= y + h <= HEIGHT
    for row in range(y, y + h):
        image[row][x:x + w] = [color] * w


def frame(expression="idle", phase=0):
    image = [[0] * WIDTH for _ in range(HEIGHT)]
    # Stepped corners preserve the handheld LCD silhouette at pixel scale.
    rect(image, 8, 4, 128, 136, 1)
    rect(image, 4, 8, 136, 128, 1)
    rect(image, 8, 8, 128, 128, 3)
    rect(image, 14, 14, 116, 116, 1)
    rect(image, 16, 16, 112, 112, 2)
    rect(image, 22, 122, 100, 2, 3)
    sprite = [list(row) for row in SPRITE]
    # Replace face pixels in sprite coordinates, preserving ears and outline.
    for y in (7, 8):
        for x in (6, 7, 14, 15):
            sprite[y][x] = "o"
    for x in (10, 11):
        sprite[10][x] = "o"

    def dot(x, y):
        sprite[y][x] = "#"

    def eye(x, mode):
        if mode == "closed":
            for dx in range(4):
                dot(x - 1 + dx, 8)
        elif mode == "happy":
            for dx, dy in ((-1, 1), (0, 0), (1, 0), (2, 1)):
                dot(x + dx, 7 + dy)
        elif mode == "angry":
            dot(x, 7)
            dot(x + 1, 8)
            dot(x, 8)
        elif mode == "sad":
            dot(x, 8)
            dot(x + 1, 8)
            dot(x - 1, 7)
        else:
            for dy in range(2):
                for dx in range(2):
                    dot(x + dx, 7 + dy)

    mode = expression if expression in ("happy", "angry", "sad") else "open"
    if expression in ("blink", "sleepy"):
        mode = "closed"
    eye(6, mode)
    eye(14, mode)
    if expression == "speaking" and phase:
        for y in range(10, 10 + phase + 1):
            for x in range(9, 13):
                dot(x, y)
    elif expression == "happy":
        for x, y in ((9, 10), (10, 11), (11, 11), (12, 10)):
            dot(x, y)
    elif expression == "sad":
        for x, y in ((9, 11), (10, 10), (11, 10), (12, 11)):
            dot(x, y)
    else:
        dot(10, 10)
        dot(11, 10)
    bob = -2 if phase % 2 and expression not in ("speaking", "thinking") else 0
    for y, row in enumerate(sprite):
        for x, value in enumerate(row):
            if value != ".":
                rect(image, 28 + x * SCALE, 36 + bob + y * SCALE,
                     SCALE, SCALE, 1 if value == "#" else 2)
    if expression == "listening":
        for side in (20, 120):
            rect(image, side, 48, 4, 16 if phase == 0 else 8, 1)
        rect(image, 66, 22, 12, 4, 1)
        rect(image, 70, 18, 4, 4, 1)
    if expression == "thinking":
        for index in range(3):
            rect(image, 92 + index * 8, 22 - (4 if index == phase else 0),
                 4, 4, 1)
    if expression == "sleepy":
        for x, y in ((106, 25), (112, 19)):
            rect(image, x, y, 8, 2, 1)
            rect(image, x + 3, y + 2, 2, 2, 1)
            rect(image, x, y + 4, 8, 2, 1)
    if expression == "happy":
        for x in (20, 116):
            rect(image, x, 29 - 2 * phase, 4, 12, 1)
            rect(image, x - 4, 33 - 2 * phase, 12, 4, 1)
    if expression == "angry":
        rect(image, 108, 24, 4, 12, 1)
        rect(image, 104, 28, 12, 4, 1)
    if expression == "sad":
        rect(image, 92, 74 + phase * 8, 4, 8, 1)
    return image


def pack(image):
    data = bytearray()
    for r, g, b, a in PALETTE:
        data.extend((b, g, r, a))
    for row in image:
        for x in range(0, WIDTH, 4):
            data.append(sum(row[x + k] << (6 - 2 * k) for k in range(4)))
    assert len(data) == 16 + ((WIDTH + 3) // 4) * HEIGHT == 5200
    # Inverse matches lv_img_decoder_built_in_line_indexed(), including rows
    # beginning at each sub-byte x offset; palette bytes match lv_color32_t.
    for y in range(HEIGHT):
        for x in range(WIDTH):
            decoded = (data[16 + y * 36 + (x >> 2)] >> (6 - (x & 3) * 2)) & 3
            assert decoded == image[y][x]
    return bytes(data)


def generate():
    definitions = {
        "idle": [("idle", 0, 1050), ("idle", 1, 550),
                 ("blink", 0, 100), ("idle", 0, 350)],
        "listening": [("listening", 0, 320), ("listening", 1, 320)],
        "thinking": [("thinking", phase, 250) for phase in range(3)],
        "speaking": [("speaking", phase, 140) for phase in (0, 1, 2, 1)],
        "sleepy": [("sleepy", 0, 900), ("sleepy", 1, 900)],
        "happy": [("happy", 0, 240), ("happy", 1, 240)],
        "angry": [("angry", 0, 350), ("angry", 1, 350)],
        "sad": [("sad", 0, 500), ("sad", 1, 500)],
    }
    lines = ["/* Generated by generate_avatar.py; do not edit. */", ""]
    seen = {}
    previews = []
    for name, phases in definitions.items():
        refs = []
        for expression, phase, delay in phases:
            image = frame(expression, phase)
            packed = pack(image)
            if packed not in seen:
                data_name = "shengsheng_pixels_%02d" % len(seen)
                seen[packed] = data_name
                lines.append("static const uint8_t %s[] = {" % data_name)
                for start in range(0, len(packed), 24):
                    lines.append("    " + ", ".join("0x%02x" % byte
                                 for byte in packed[start:start + 24]) + ",")
                lines.extend(("};", ""))
            refs.append(seen[packed])
        previews.append(frame(phases[0][0], phases[0][1]))
        lines.append("static const lv_img_dsc_t shengsheng_%s_frames[] = {" % name)
        for ref in refs:
            lines.append("    {.header = {.cf = LV_IMG_CF_INDEXED_2BIT, .w = 144, .h = 144},")
            lines.append("     .data_size = sizeof(%s), .data = %s}," % (ref, ref))
        lines.extend(("};", "static const uint32_t shengsheng_%s_delays[] = {%s};" %
                      (name, ", ".join(str(entry[2]) for entry in phases)), ""))
        lines.extend(("static const lisa_ui_anim_ext_config_t shengsheng_%s = {" % name,
                      "    .loop = {.frames = shengsheng_%s_frames," % name,
                      "             .delays = shengsheng_%s_delays," % name,
                      "             .frame_count = %d, .default_delay = 250," % len(refs),
                      "             .loop = -1, .pause_frame_idx = 0},", "};", ""))
    source = "\n".join(lines)
    return source, previews, len(seen)


def write_preview(path, previews):
    width, height = WIDTH * 4, HEIGHT * 2
    pixels = bytearray()
    for y in range(height):
        pixels.append(0)  # PNG filter: none.
        for x in range(width):
            image = previews[(y // HEIGHT) * 4 + x // WIDTH]
            color = PALETTE[image[y % HEIGHT][x % WIDTH]]
            pixels.extend(color if color[3] else (0, 0, 0, 255))

    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff))

    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">2I5B", width, height, 8, 6, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(bytes(pixels), 9)) + chunk(b"IEND", b""))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--preview", type=Path)
    args = parser.parse_args()
    assert len(SPRITE) == 20 and all(len(row) == 22 for row in SPRITE)
    output, previews, count = generate()
    target = Path(__file__).with_name("shengsheng_avatar_data.inc")
    if args.check:
        if not target.exists() or target.read_text() != output:
            raise SystemExit("Avatar data is stale; run generate_avatar.py")
    else:
        target.write_text(output)
    if args.preview:
        write_preview(args.preview, previews)
    print("%d unique frames, %d bytes of pixels; generated SHA256 %s" %
          (count, count * 5200, hashlib.sha256(output.encode()).hexdigest()))


if __name__ == "__main__":
    main()
