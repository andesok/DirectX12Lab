#!/usr/bin/env python3
"""
Нарезка карты высот (и цветовой карты) из генератора на тайлы для ДЗ Terrain.

Нужен, если генератор не умеет тайловую сборку (Tiled Build) или она платная.
Соседние тайлы делят общую кромку в 1 пиксель — как Tiled Build в World Machine/Gaea,
поэтому на стыках нет щелей. Программа сама это распознаёт.

Пример:
    pip install pillow numpy
    python slice_heightmap.py height.png --tiles 4 --out ../Terrain
    python slice_heightmap.py height.r16 --tiles 4 --color color.png --out ../Terrain

Результат: Terrain/height_x0_y0.png ... height_x3_y3.png (16 бит),
           Terrain/color_x0_y0.png ... (если указан --color)
"""
import argparse
import math
import os
import sys

import numpy as np
from PIL import Image


def load_height(path: str) -> np.ndarray:
    ext = os.path.splitext(path)[1].lower()
    if ext in (".r16", ".raw"):
        data = np.fromfile(path, dtype="<u2")
        n = int(round(math.sqrt(data.size)))
        if n * n != data.size:
            sys.exit(f"{path}: RAW не квадратный ({data.size} значений)")
        return data.reshape(n, n)

    img = Image.open(path)
    arr = np.array(img)
    if arr.ndim == 3:                      # RGB(A) — берём первый канал
        arr = arr[..., 0]
    if arr.dtype == np.uint8:
        arr = arr.astype(np.uint16) * 257  # 8 бит -> 16 бит
    elif arr.dtype != np.uint16:           # 'I' (int32) у некоторых PNG / float у TIFF
        a = arr.astype(np.float64)
        a = (a - a.min()) / max(a.max() - a.min(), 1e-12)
        arr = np.round(a * 65535)
    return arr.astype(np.uint16)


def pad_to(arr: np.ndarray, size_y: int, size_x: int) -> np.ndarray:
    """Дополнить повтором края до нужного размера (если не делится на тайлы)."""
    py, px = size_y - arr.shape[0], size_x - arr.shape[1]
    if py < 0 or px < 0:
        return arr[:size_y, :size_x]
    widths = [(0, py), (0, px)] + [(0, 0)] * (arr.ndim - 2)
    return np.pad(arr, widths, mode="edge")


def slice_shared(arr: np.ndarray, tiles: int):
    """Тайлы с общей кромкой: tile = step + 1 пикселей, шаг = step."""
    h, w = arr.shape[:2]
    step = math.ceil((max(h, w) - 1) / tiles)
    full = step * tiles + 1
    if (h, w) != (full, full):
        print(f"  размер {w}x{h} -> {full}x{full} (дополнен краем)")
        arr = pad_to(arr, full, full)
    for ty in range(tiles):
        for tx in range(tiles):
            yield tx, ty, arr[ty * step: ty * step + step + 1, tx * step: tx * step + step + 1]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("height", help="карта высот: PNG (8/16 бит) или RAW16 (.r16/.raw)")
    ap.add_argument("--tiles", type=int, default=4, help="тайлов на сторону (по умолчанию 4 => 4x4)")
    ap.add_argument("--color", help="цветовая карта (PNG/JPG), режется так же")
    ap.add_argument("--out", default="Terrain", help="папка для тайлов")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)

    h = load_height(args.height)
    print(f"Высоты: {h.shape[1]}x{h.shape[0]}, min={h.min()} max={h.max()}")
    for tx, ty, tile in slice_shared(h, args.tiles):
        name = os.path.join(args.out, f"height_x{tx}_y{ty}.png")
        Image.fromarray(np.ascontiguousarray(tile)).save(name)   # uint16 -> 16-битный PNG
    print(f"  -> {args.tiles * args.tiles} тайлов высот, каждый {tile.shape[1]}x{tile.shape[0]}")

    if args.color:
        c = np.array(Image.open(args.color).convert("RGBA"))
        if c.shape[:2] != h.shape[:2]:
            print(f"  цвет {c.shape[1]}x{c.shape[0]} масштабируется под высоты {h.shape[1]}x{h.shape[0]}")
            c = np.array(Image.fromarray(c).resize((h.shape[1], h.shape[0]), Image.BICUBIC))
        for tx, ty, tile in slice_shared(c, args.tiles):
            Image.fromarray(np.ascontiguousarray(tile), "RGBA").save(
                os.path.join(args.out, f"color_x{tx}_y{ty}.png"))
        print(f"  -> {args.tiles * args.tiles} цветовых тайлов")

    print("Готово:", os.path.abspath(args.out))


if __name__ == "__main__":
    main()
