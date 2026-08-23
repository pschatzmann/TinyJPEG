// Pixel-level correctness gate, 4:4:4 (unsubsampled chroma) path: decode a
// real libjpeg-encoded image and diff the result against libjpeg's own
// decode of the same file (via Pillow - this project's correctness oracle,
// the same role ffmpeg plays for TinyMPG/TinyH264's own decode tests - see
// docs/testing.md). Comparison is tolerance-based, not byte-exact - see
// docs/testing.md for why (RGB565 quantization plus the two decoders'
// independent, but both spec-legal, IDCT/YCbCr rounding).
#include <cassert>
#include <cmath>
#include <cstdio>
#include "TinyJPEGDecoder.h"
#include "test_helpers.h"

using namespace tinyjpeg;

int main() {
  auto jpg = readFile("assets/gradient_444.jpg");
  int refW, refH, refChannels;
  auto ref = readPNM("assets/gradient_444_ref.ppm", refW, refH, refChannels);
  assert(refChannels == 3);

  TinyJPEGDecoder decoder;
  uint16_t w, h;
  assert(decoder.getJpgSize(&w, &h, jpg.data(), jpg.size()) == JDR_OK);
  assert(w == refW && h == refH);

  PixelSink sink;
  sink.width = w;
  sink.height = h;
  sink.pixels.assign((size_t)w * h, 0);
  decoder.setUserData(&sink);
  decoder.setCallback(pixelSinkCallback);
  JRESULT r = decoder.drawJpg(0, 0, jpg.data(), jpg.size());
  assert(r == JDR_OK);

  // Measured (not guessed) on this exact asset - see gen_assets.py. Bound
  // is sum-of-|R diff|+|G diff|+|B diff| per pixel, dominated by RGB565's
  // own 5/6/5-bit quantization (~5 of this budget) plus a few units of
  // independent IDCT/color-conversion rounding between tjpgd and libjpeg.
  const double kMaxMeanDiff = 20.0;
  const int kMaxSampleDiff = 40;

  double sumAbs = 0;
  int maxDiff = 0;
  size_t n = (size_t)w * h;
  for (size_t i = 0; i < n; i++) {
    uint8_t r8, g8, b8;
    rgb565ToRgb888(sink.pixels[i], r8, g8, b8);
    int rr = ref[i * 3 + 0], gg = ref[i * 3 + 1], bb = ref[i * 3 + 2];
    int d = std::abs(r8 - rr) + std::abs(g8 - gg) + std::abs(b8 - bb);
    sumAbs += d;
    if (d > maxDiff) maxDiff = d;
  }
  double meanDiff = sumAbs / (double)n;
  printf("test_decode_gradient: mean=%.3f max=%d (bounds: mean<=%.1f max<=%d)\n", meanDiff, maxDiff,
         kMaxMeanDiff, kMaxSampleDiff);
  assert(meanDiff <= kMaxMeanDiff);
  assert(maxDiff <= kMaxSampleDiff);

  printf("test_decode_gradient: OK\n");
  return 0;
}
