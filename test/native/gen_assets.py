#!/usr/bin/env python3
"""Generates test/native/assets/*.jpg and their Pillow-decoded *.ppm/*.pgm
oracle references - see docs/testing.md. Run from the repository root:

    python3 test/native/gen_assets.py

Requires Pillow (`pip install pillow`). Re-run this if the test images
ever need to change, rather than hand-editing the binary assets - keeps
the JPEG bytes and their oracle decode in sync by construction.
"""
import os
from PIL import Image

ASSETS_DIR = os.path.join(os.path.dirname(__file__), "assets")


def make_pattern(w, h):
    """Smooth RGB gradient - low-frequency content, a gentle case for JPEG."""
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            r = (x * 255) // (w - 1)
            g = (y * 255) // (h - 1)
            b = ((x ^ y) * 255) // (max(w, h) - 1)
            px[x, y] = (r, g, b)
    return img


def make_checker(w, h, cell=8):
    """High-contrast checkerboard aligned to the 8x8 DCT block grid - a
    deliberately adversarial (worst-case ringing) pattern for JPEG - see
    docs/testing.md's note on test_decode_checker420's looser tolerance."""
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            on = ((x // cell) + (y // cell)) % 2 == 0
            px[x, y] = (230, 40, 40) if on else (20, 60, 200)
    return img


def save_with_oracle(img, jpg_name, ref_name, **save_kwargs):
    jpg_path = os.path.join(ASSETS_DIR, jpg_name)
    ref_path = os.path.join(ASSETS_DIR, ref_name)
    img.save(jpg_path, "JPEG", **save_kwargs)
    # Decode back through Pillow (libjpeg) - this, not the pre-encode
    # source image, is the correctness oracle the native tests diff
    # against, since it went through the same lossy round-trip tjpgd does.
    decoded = Image.open(jpg_path).convert(img.mode if img.mode == "L" else "RGB")
    decoded.save(ref_path)
    print(f"{jpg_name}: {img.size}, oracle -> {ref_name}")


def main():
    os.makedirs(ASSETS_DIR, exist_ok=True)

    # 64x48 gradient, 4:4:4 (unsubsampled chroma) - test_decode_gradient.cpp
    save_with_oracle(make_pattern(64, 48), "gradient_444.jpg", "gradient_444_ref.ppm",
                      quality=92, subsampling=0)

    # 64x48 checkerboard, 4:2:0 (subsampled chroma) - test_decode_checker420.cpp
    save_with_oracle(make_checker(64, 48, cell=8), "checker_420.jpg", "checker_420_ref.ppm",
                      quality=90, subsampling=2)

    # 32x32, 4:2:0 - small embedded-in-flash asset for examples/DecodeFromProgmem
    make_pattern(32, 32).save(os.path.join(ASSETS_DIR, "tiny_32.jpg"), "JPEG",
                              quality=90, subsampling=2)
    print("tiny_32.jpg: (32, 32) - no oracle needed, used by examples/ only")

    # 32x24 grayscale (single-component) - test_decode_grayscale.cpp
    save_with_oracle(make_pattern(32, 24).convert("L"), "gray_32x24.jpg", "gray_32x24_ref.pgm",
                      quality=90)


if __name__ == "__main__":
    main()
