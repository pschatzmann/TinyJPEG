// Correctness gate for a single-component (grayscale/monochrome, ncomp==1)
// source image - a separate code path in tjpgd's mcu_load()/mcu_output()
// from the 3-component (Y/Cb/Cr) case (chroma blocks are synthesized as a
// flat 128 rather than decoded), even though the *output* format here is
// still RGB565 (JD_FORMAT defaults to 1 - see tjpgd_config.h; a grayscale
// source's R==G==B in the output either way). See docs/testing.md for why
// pixel comparisons are tolerance-based, not byte-exact.
#include <cassert>
#include <cmath>
#include <cstdio>
#include "TinyJPEGDecoder.h"
#include "test_helpers.h"

using namespace tinyjpeg;

int main() {
  auto jpg = readFile("assets/gray_32x24.jpg");
  int refW, refH, refChannels;
  auto ref = readPNM("assets/gray_32x24_ref.pgm", refW, refH, refChannels);
  assert(refChannels == 1);

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

  const double kMaxMeanDiff = 20.0;  // measured on this asset - see gen_assets.py
  const int kMaxSampleDiff = 40;

  double sumAbs = 0;
  int maxDiff = 0;
  size_t n = (size_t)w * h;
  for (size_t i = 0; i < n; i++) {
    uint8_t r8, g8, b8;
    rgb565ToRgb888(sink.pixels[i], r8, g8, b8);
    // A grayscale source decodes with R==G==B==yy *before* RGB565 packing
    // (cb==cr==0 zeroes out mcu_output()'s whole chroma contribution - see
    // tjpgd.h's mcu_output()), but RGB565 itself then quantizes R/G/B to
    // 5/6/5 bits independently, so e.g. yy=6 packs as R=0,G=4,B=0 - not
    // exactly equal. The two channels sharing 5-bit precision (R, B) must
    // still match each other exactly; G can differ by up to one 5-bit
    // quantization step (8).
    assert(r8 == b8);
    assert(std::abs(r8 - g8) <= 8);
    int yy = ref[i];
    int d = std::abs(r8 - yy) * 3;  // scaled to match the 3-channel sum used by the color tests
    sumAbs += d;
    if (d > maxDiff) maxDiff = d;
  }
  double meanDiff = sumAbs / (double)n;
  printf("test_decode_grayscale: mean=%.3f max=%d (bounds: mean<=%.1f max<=%d)\n", meanDiff, maxDiff,
         kMaxMeanDiff, kMaxSampleDiff);
  assert(meanDiff <= kMaxMeanDiff);
  assert(maxDiff <= kMaxSampleDiff);

  printf("test_decode_grayscale: OK\n");
  return 0;
}
