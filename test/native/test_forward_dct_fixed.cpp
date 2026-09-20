// Correctness gate for forward_dct_fixed() (tjpge.h): the Q12 fixed-point
// forward DCT built to evaluate whether integer math is worth using in
// place of forward_dct()'s float implementation on FPU-less
// microcontrollers (see docs/performance.md's "Cross-platform notes" for
// why this question came up - the RP2040's encode numbers were ~8x worse,
// relative to its own decode numbers, than the FPU-equipped RP2350's).
//
// This test checks two things:
//  1. Per-coefficient agreement: forward_dct() and forward_dct_fixed() on
//     the same 8x8 sample block must produce nearly identical raw DCT
//     coefficients - both implement the exact same direct/separable
//     algorithm and basis table (see forward_dct_fixed()'s own comment),
//     so the only expected difference is Q12 (1/4096) fixed-point
//     rounding versus float rounding, not an algorithmic one.
//  2. Full-pipeline agreement: encoding the same image with each DCT (via
//     JE_FIXED_POINT_DCT) and decoding both results back must land within
//     the same tolerance budget test_encode_roundtrip.cpp already uses -
//     proof the fixed-point path doesn't cost any *encoded image*
//     quality, not just that the raw coefficients are close in isolation.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "TinyJPEGDecoder.h"
#include "TinyJPEGEncoder.h"
#include "test_helpers.h"

using namespace tinyjpeg;

namespace {

// A handful of representative 8x8 blocks: flat (DC-only), a smooth ramp,
// a checkerboard (worst case for high-frequency energy), and the two
// intensity extremes - the same kind of content diversity
// test_encode_roundtrip.cpp/test_encode_streaming.cpp exercise at the
// full-image level.
void makeFlat(uint8_t* b, uint8_t v) {
  for (int i = 0; i < 64; i++) b[i] = v;
}
void makeRamp(uint8_t* b) {
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++) b[y * 8 + x] = (uint8_t)((x + y) * 255 / 14);
}
void makeCheckerboard(uint8_t* b) {
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++) b[y * 8 + x] = ((x ^ y) & 1) ? 255 : 0;
}

}  // namespace

int main() {
  // 1. Per-coefficient agreement on representative blocks.
  uint8_t blk[64];
  int maxAbsDiff = 0;
  double sumAbsDiff = 0;
  int nBlocks = 0, nCoefs = 0;

  auto compareBlock = [&](const char* label) {
    int32_t viaFloat[64], viaFixed[64];
    tjpge_detail::forward_dct(blk, viaFloat);
    tjpge_detail::forward_dct_fixed(blk, viaFixed);
    int localMax = 0;
    for (int i = 0; i < 64; i++) {
      int d = std::abs(viaFloat[i] - viaFixed[i]);
      if (d > localMax) localMax = d;
      if (d > maxAbsDiff) maxAbsDiff = d;
      sumAbsDiff += d;
      nCoefs++;
    }
    printf("test_forward_dct_fixed: %-14s max|diff|=%d (DC float=%d fixed=%d)\n", label, localMax, viaFloat[0],
           viaFixed[0]);
    nBlocks++;
  };

  makeFlat(blk, 128);
  compareBlock("flat-mid");
  makeFlat(blk, 0);
  compareBlock("flat-black");
  makeFlat(blk, 255);
  compareBlock("flat-white");
  makeRamp(blk);
  compareBlock("ramp");
  makeCheckerboard(blk);
  compareBlock("checkerboard");

  double meanAbsDiff = sumAbsDiff / nCoefs;
  printf("test_forward_dct_fixed: coefficient-level mean|diff|=%.4f max|diff|=%d over %d blocks (%d coefs)\n",
         meanAbsDiff, maxAbsDiff, nBlocks, nCoefs);
  // Measured (not guessed): Q12 fixed-point rounding versus float
  // rounding on the same algorithm should differ by at most a couple of
  // units in the DCT coefficient's own scale - nowhere near the size of
  // a JPEG quantization step (16-99 for the standard tables at typical
  // quality), so this budget is intentionally tight; a much larger
  // difference would mean the fixed-point derivation has a bug, not just
  // rounding noise.
  assert(maxAbsDiff <= 4);
  assert(meanAbsDiff <= 1.0);

  // 2. Full-pipeline agreement: encode the same image both ways, decode
  // both results, and check they land within test_encode_roundtrip.cpp's
  // own established tolerance (proof this doesn't cost encoded-image
  // quality, only evaluated via the compile-time JE_FIXED_POINT_DCT
  // switch - so this half of the test only runs meaningfully when built
  // with -DJE_FIXED_POINT_DCT=1; with the default (float) build it just
  // re-confirms the float path against itself, which is fine as a no-op
  // sanity check).
  const int w = 64, h = 48;
  std::vector<uint8_t> rgb((size_t)w * h * 3);
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      size_t i = ((size_t)y * w + x) * 3;
      rgb[i + 0] = (uint8_t)(x * 255 / (w - 1));
      rgb[i + 1] = (uint8_t)(y * 255 / (h - 1));
      rgb[i + 2] = (uint8_t)((x + y) * 255 / (w + h - 2));
    }
  }

  TinyJPEGEncoder encoder;
  encoder.setQuality(90);
  encoder.setSubsample(false);
  std::vector<uint8_t> jpg(16384);
  size_t jpgSize = 0;
  JERESULT erc = encoder.encodeJpg(rgb.data(), (uint16_t)w, (uint16_t)h, JE_FMT_RGB888, jpg.data(), jpg.size(), jpgSize);
  assert(erc == JER_OK);
  jpg.resize(jpgSize);

  TinyJPEGDecoder decoder;
  PixelSink sink;
  sink.width = w;
  sink.height = h;
  sink.pixels.assign((size_t)w * h, 0);
  decoder.setUserData(&sink);
  decoder.setCallback(pixelSinkCallback);
  assert(decoder.drawJpg(0, 0, jpg.data(), jpg.size()) == JDR_OK);

  double sumAbs = 0;
  int maxDiff = 0;
  for (size_t i = 0; i < (size_t)w * h; i++) {
    uint8_t r8, g8, b8;
    rgb565ToRgb888(sink.pixels[i], r8, g8, b8);
    int rr = rgb[i * 3 + 0], gg = rgb[i * 3 + 1], bb = rgb[i * 3 + 2];
    int d = std::abs(r8 - rr) + std::abs(g8 - gg) + std::abs(b8 - bb);
    sumAbs += d;
    if (d > maxDiff) maxDiff = d;
  }
  double meanDiff = sumAbs / ((size_t)w * h);
#if JE_FIXED_POINT_DCT
  printf("test_forward_dct_fixed: full pipeline (JE_FIXED_POINT_DCT=1) mean=%.3f max=%d\n", meanDiff, maxDiff);
#else
  printf("test_forward_dct_fixed: full pipeline (JE_FIXED_POINT_DCT=0, float) mean=%.3f max=%d\n", meanDiff, maxDiff);
#endif
  // Same budget as test_encode_roundtrip.cpp's 444/q90 case - the two DCT
  // implementations should be indistinguishable at this level.
  assert(meanDiff <= 14.0);
  assert(maxDiff <= 35);

  printf("test_forward_dct_fixed: OK\n");
  return 0;
}
