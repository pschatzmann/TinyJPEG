#pragma once
// Shared, tiny helpers for the native test suite - reading a whole file into
// memory and parsing a binary PNM (P6/P5, the format PIL writes when asked
// to save a decoded reference image with no extra options) into a raw
// sample buffer. Not part of the public library - test-only.
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cstdint>
#include "TinyJPEGDecoder.h"

inline std::vector<uint8_t> readFile(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "cannot open %s (run from test/native/, see docs/testing.md)\n", path);
    exit(1);
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> data((size_t)size);
  size_t n = fread(data.data(), 1, (size_t)size, f);
  (void)n;
  fclose(f);
  return data;
}

// Parses a binary PPM (P6, 3 channels) or PGM (P5, 1 channel) file with a
// single-line header (no embedded comments) - exactly what
// test/native/gen_assets.py produces via PIL's Image.save().
inline std::vector<uint8_t> readPNM(const char *path, int &w, int &h, int &channels) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "cannot open %s (run from test/native/, see docs/testing.md)\n", path);
    exit(1);
  }
  char magic[3] = {0};
  if (fscanf(f, "%2s", magic) != 1) exit(1);
  channels = (magic[1] == '6') ? 3 : 1;
  int maxval;
  if (fscanf(f, "%d %d %d", &w, &h, &maxval) != 3) exit(1);
  fgetc(f);  // single whitespace byte separating the header from pixel data
  std::vector<uint8_t> data((size_t)w * h * channels);
  size_t n = fread(data.data(), 1, data.size(), f);
  (void)n;
  fclose(f);
  return data;
}

inline void rgb565ToRgb888(uint16_t px, uint8_t &r, uint8_t &g, uint8_t &b) {
  r = (uint8_t)((px >> 8) & 0xF8);
  g = (uint8_t)((px >> 3) & 0xFC);
  b = (uint8_t)((px << 3) & 0xF8);
}

/*
 * Accumulates decoded blocks into a plain RGB565 buffer for pixel-level
 * comparison against an oracle - reached from pixelSinkCallback() below
 * via TinyJPEGDecoder::getUserData(), not a global/static variable (see
 * TinyJPEGDecoder.h's SketchCallback doc comment for why the callback
 * carries a `decoder` reference specifically to make this possible).
 * Usage: `PixelSink sink; sink.width = w; sink.height = h;
 * sink.pixels.assign((size_t)w*h, 0); decoder.setUserData(&sink);
 * decoder.setCallback(pixelSinkCallback);`.
 */
struct PixelSink {
  std::vector<uint16_t> pixels;
  int width = 0;
  int height = 0;
};

using tinyjpeg::TinyJPEGDecoder;

inline bool pixelSinkCallback(TinyJPEGDecoder &decoder, int16_t x, int16_t y, uint16_t w, uint16_t h,
                               uint16_t *data) {
  auto *sink = static_cast<PixelSink *>(decoder.getUserData());
  for (uint16_t row = 0; row < h; row++) {
    for (uint16_t col = 0; col < w; col++) {
      int px = x + col, py = y + row;
      if (px < 0 || py < 0 || px >= sink->width || py >= sink->height) continue;
      sink->pixels[(size_t)py * sink->width + px] = data[row * w + col];
    }
  }
  return true;
}
