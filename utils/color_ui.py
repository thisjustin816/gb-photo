"""Compile the original artwork into exact CGB background and sprite colors."""
import argparse
from collections import Counter, deque
from itertools import combinations
from pathlib import Path
import struct
import subprocess
import tempfile

from PIL import Image

PAPER = (248, 248, 248)
INK = (24, 24, 24)
LIGHT = (208, 216, 216)
SILVER = (128, 144, 152)
BLUE = (64, 128, 192)
GLYPHS = ('11110/10001/10001/10001/10001/10001/11110',
          '10001/10001/01010/00100/01010/10001/10001')


def indexed(name):
    image = Image.open(f'res/gfx/gb/backgrounds/{name}.png').convert('RGB')
    shades = sorted(set(image.convert('RGB').get_flattened_data()), key=sum, reverse=True)
    return [[shades.index(image.getpixel((x, y))) for x in range(image.width)]
            for y in range(image.height)]


def badge(canvas, x0, y0, scale):
    fill = {(x0 + i * 7 * scale + x * scale + dx, y0 + y * scale + dy)
            for i, glyph in enumerate(GLYPHS) for y, row in enumerate(glyph.split('/'))
            for x, bit in enumerate(row) if bit == '1'
            for dx in range(scale) for dy in range(scale)}
    if scale == 2:
        for x, y in fill:
            for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1)):
                if (x + dx, y + dy) not in fill:
                    canvas[y + dy][x + dx] = INK
    for x, y in fill:
        canvas[y][x] = BLUE


def title():
    src = indexed('logo')
    outside = set()
    queue = deque((x, y) for y in range(36, 98) for x in range(6, 156)
                  if x in (6, 155) or y in (36, 97))
    while queue:
        x, y = queue.popleft()
        if not (6 <= x < 156 and 36 <= y < 98) or (x, y) in outside or src[y][x] not in (0, 1):
            continue
        outside.add((x, y))
        queue.extend(((x-1, y), (x+1, y), (x, y-1), (x, y+1)))
    canvas = [[PAPER for _ in range(160)] for _ in range(128)]
    for y in range(36, 98):
        for x in range(6, 156):
            if (x, y) not in outside:
                canvas[y][x] = (PAPER, LIGHT, SILVER, INK)[src[y][x]]
    sheet = Image.open('assets/art/Logo/Logo_variants_Photo!_clear.png').convert('RGBA')
    # The monochrome variant matches the ROM at (+14, -12); its color variant
    # is 48 pixels above it. Alpha preserves the Game Boy behind the letters.
    for sy in range(64, 104):
        for sx in range(16, 144):
            if sheet.getpixel((sx, sy))[3]:
                canvas[sy-12][sx+14] = sheet.getpixel((sx, sy-48))[:3]
    badge(canvas, 122, 96, 2)
    return canvas


def hub():
    canvas = [[PAPER for _ in range(144)] for _ in range(48)]
    logo = indexed('logo_about')
    for y, row in enumerate(logo):
        for x, shade in enumerate(row):
            canvas[y][x+40] = PAPER if not shade else INK
    badge(canvas, 106, 1, 1)
    for y, row in enumerate(indexed('main_menu')):
        for x, shade in enumerate(row):
            material = (192, 128, 48) if 28 <= x < 57 else BLUE if x >= 119 else SILVER
            canvas[y+16][x] = (PAPER, LIGHT, material, INK)[shade]
    return canvas


def color15(rgb):
    return sum((value >> 3) << (i * 5) for i, value in enumerate(rgb))


def encode(tile):
    return bytes(value for row in tile for value in
                 (sum((p & 1) << (7-i) for i, p in enumerate(row)),
                  sum((p >> 1) << (7-i) for i, p in enumerate(row))))


def pack(canvas, x0, y0):
    data = [[color15(c) for c in row] for row in canvas]
    width, height = len(data[0]) // 8, len(data) // 8
    requirements = [set(data[y][x] for y in range(ty*8, ty*8+8)
                        for x in range(tx*8, tx*8+8))
                    for ty in range(height) for tx in range(width)]
    candidates = set()
    for colors in requirements:
        candidates.update([frozenset(colors)] if len(colors) <= 4 else
                          map(frozenset, combinations(sorted(colors), 4)))
    chosen, costs = [], [10] * len(requirements)
    for _ in range(min(7, len(candidates))):
        palette = max(sorted(candidates, key=lambda p: tuple(sorted(p))), key=lambda p:
                      sum(max(0, old-(len(colors-p)+2)//3) for old, colors in zip(costs, requirements)))
        chosen.append(palette)
        costs = [min(old, (len(colors-palette)+2)//3) for old, colors in zip(costs, requirements)]
        candidates.remove(palette)
    palettes = [sorted(p) + [color15(PAPER)] * (4-len(p)) for p in chosen]
    tiles, attrs, overlays = [], [], []
    for ty in range(height):
        for tx in range(width):
            colors = Counter(data[y][x] for y in range(ty*8, ty*8+8) for x in range(tx*8, tx*8+8))
            p = min(range(len(palettes)), key=lambda i: (len(set(colors)-set(palettes[i])),
                    sum(n for c, n in colors.items() if c not in palettes[i])))
            missing = sorted(set(colors)-set(palettes[p]))
            attrs.append(p+1)
            tiles.append(encode([[palettes[p].index(data[y][x]) if data[y][x] in palettes[p] else 0
                                  for x in range(tx*8, tx*8+8)] for y in range(ty*8, ty*8+8)]))
            for start in range(0, len(missing), 3):
                overlays.append((tx, ty, set(missing[start:start+3])))
    sprite_palettes = []
    for colors in sorted({frozenset(s) for _, _, s in overlays}, key=lambda s: (-len(s), sorted(s))):
        if any(colors <= s for s in sprite_palettes):
            continue
        fit = [i for i, s in enumerate(sprite_palettes) if len(s | colors) <= 3]
        if fit:
            sprite_palettes[min(fit, key=lambda i: len(sprite_palettes[i] | colors))] |= colors
        else:
            sprite_palettes.append(set(colors))
    assert len(overlays) <= 36 and len(sprite_palettes) <= 6
    assert max(Counter(y for _, y, _ in overlays).values(), default=0) <= (10 if y0 == 0 else 6)
    sprite_palettes = [sorted(s) for s in sprite_palettes]
    sprites, patterns = [], []
    for tx, ty, colors in overlays:
        p = next(i for i, s in enumerate(sprite_palettes) if colors <= set(s))
        patterns.append(encode([[sprite_palettes[p].index(data[y][x])+1 if data[y][x] in colors else 0
                                  for x in range(tx*8, tx*8+8)] for y in range(ty*8, ty*8+8)]))
        sprites.extend((y0+ty*8+16, x0+tx*8+8, len(patterns)-1, (p+2) | 8))
    print(f'{width}x{height}: {len(overlays)} sprites, {len(sprite_palettes)} sprite palettes')
    return (b''.join(tiles), bytes(attrs), b''.join(struct.pack('<4H', *p) for p in palettes).ljust(56, b'\0'),
            bytes(sprites), b''.join(patterns),
            b''.join(struct.pack('<4H', 0, *(s+[0]*(3-len(s)))) for s in sprite_palettes).ljust(48, b'\0'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compressor', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    arrays = []
    with tempfile.TemporaryDirectory() as directory:
        raw, compressed = Path(directory)/'raw.bin', Path(directory)/'compressed.bin'
        def emit(name, data, compress=True):
            if not data:
                return
            if compress:
                raw.write_bytes(data)
                subprocess.run([args.compressor, str(raw), str(compressed)], check=True, capture_output=True)
                data = compressed.read_bytes()
            arrays.append('static const uint8_t '+name+'[] = {\n'+
                          ','.join(f'0x{v:02x}' for v in data)+'\n};\n')
        for name, canvas, x0, y0 in [('title', title(), 0, 0), ('hub', hub(), 8, 24)]:
            tiles, attrs, palettes, sprites, patterns, sprite_palettes = pack(canvas, x0, y0)
            width = len(canvas[0])//8
            for start in range(0, len(canvas)//8, 6):
                emit(f'{name}_tiles_{start}', tiles[start*width*16:(start+6)*width*16])
            for suffix, data in [('attrs', attrs), ('palettes', palettes), ('patterns', patterns), ('sprite_palettes', sprite_palettes)]:
                emit(f'{name}_{suffix}', data)
            emit(f'{name}_sprites', sprites, False)
            expected = Image.new('RGB', (len(canvas[0]), len(canvas)))
            expected.putdata([tuple(((v >> 3) << 3) | ((v >> 3) >> 2) for v in pixel)
                              for row in canvas for pixel in row])
            expected.save(args.output.parent / f'{name}-expected.png')
            (args.output.parent / f'{name}-expected.rgb').write_bytes(expected.tobytes())
    args.output.write_text('/* Generated by utils/color_ui.py. */\n' + '\n'.join(arrays))


if __name__ == '__main__':
    main()
