// Pixel-level correctness gate, 4:2:0 (subsampled chroma) path - this is
// the common real-world JPEG case (mx==16 or my==16 in tjpgd's
// mcu_output(), the "doubled block width/height" branch that 4:4:4 never
// exercises). The source image is a deliberately adversarial 8px
// checkerboard (high-contrast edges aligned to the 8x8 DCT block grid),
// which produces real JPEG ringing at every edge - see docs/testing.md for
// why this test's tolerance is measured, not guessed, and much looser than
// test_decode_gradient's: the content itself is a worst case for lossy
// compression, independent of anything tjpgd does. What this test actually
// gates is structural correctness (right dimensions, right decode result,
// no shifted/duplicated/garbled blocks) - visually confirmed against the
// libjpeg reference when this asset was added; the numeric bound here just
// keeps that confirmed-good state from silently regressing.
#include <cassert>
#include <cmath>
#include <cstdio>
#include "TinyJPEGDecoder.h"
#include "test_helpers.h"

using namespace tinyjpeg;

int main() {
  auto jpg = readFile("assets/checker_420.jpg");
  int refW, refH, refChannels;
  auto ref = readPNM("assets/checker_420_ref.ppm", refW, refH, refChannels);
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

  // Measured on this exact asset (see gen_assets.py) - much looser than
  // test_decode_gradient's, see the file comment above for why.
  const double kMaxMeanDiff = 70.0;
  const int kMaxSampleDiff = 200;

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
  printf("test_decode_checker420: mean=%.3f max=%d (bounds: mean<=%.1f max<=%d)\n", meanDiff, maxDiff,
         kMaxMeanDiff, kMaxSampleDiff);
  assert(meanDiff <= kMaxMeanDiff);
  assert(maxDiff <= kMaxSampleDiff);

  printf("test_decode_checker420: OK\n");
  return 0;
}
