// Correctness gate for JE_FMT_RGB565 input (tjpge.h's get_rgb() unpacking
// path) - the format most TFT/camera frame buffers are actually stored in,
// added alongside test_encode_roundtrip.cpp's RGB888/grayscale coverage.
// Same strategy as that test: encode, then decode the result back with
// this repo's own tjpgd.h (via TinyJPEGDecoder) and diff against a known-
// good reference, rather than just checking the encoder agrees with
// itself.
//
// The reference here is the *RGB565-quantized* source (each channel
// bit-replicated back up from 5/6/5 bits, the same expansion get_rgb()
// itself does), not the original full-precision RGB888 gradient - RGB565
// input is lossy before the encoder ever sees it, and that loss isn't a
// bug to budget tolerance for, it's the format. Comparing against the
// already-565-quantized reference isolates the JPEG-specific error (DCT/
// quantization/YCbCr rounding) from the format's own precision loss, the
// same separation test_decode_gradient.cpp's tolerance comment draws
// between RGB565 output quantization and decoder rounding.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
#include "TinyJPEGDecoder.h"
#include "TinyJPEGEncoder.h"
#include "test_helpers.h"

using namespace tinyjpeg;

namespace {

// Builds a synthetic RGB565 gradient (native-endian uint16_t packed into
// bytes, exactly the layout JE_FMT_RGB565 documents in tjpge.h) and, in
// parallel, the RGB888 reference obtained by expanding each pixel's 5/6/5
// bits back up to 8 - what a lossless decode of encoding *that* image
// should reproduce.
void makeGradientRgb565(int w, int h, std::vector<uint8_t>& rgb565, std::vector<uint8_t>& rgb888Ref) {
  rgb565.assign((size_t)w * h * 2, 0);
  rgb888Ref.assign((size_t)w * h * 3, 0);
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      uint8_t r8 = (uint8_t)(x * 255 / (w - 1));
      uint8_t g8 = (uint8_t)(y * 255 / (h - 1));
      uint8_t b8 = (uint8_t)((x + y) * 255 / (w + h - 2));
      uint8_t r5 = r8 >> 3, g6 = g8 >> 2, b5 = b8 >> 3;
      uint16_t px = (uint16_t)((r5 << 11) | (g6 << 5) | b5);

      size_t i = (size_t)y * w + x;
      rgb565[i * 2 + 0] = (uint8_t)(px & 0xFF);
      rgb565[i * 2 + 1] = (uint8_t)(px >> 8);

      rgb888Ref[i * 3 + 0] = (uint8_t)((r5 << 3) | (r5 >> 2));
      rgb888Ref[i * 3 + 1] = (uint8_t)((g6 << 2) | (g6 >> 4));
      rgb888Ref[i * 3 + 2] = (uint8_t)((b5 << 3) | (b5 >> 2));
    }
  }
}

}  // namespace

int main() {
  const int w = 64, h = 48;
  std::vector<uint8_t> rgb565, rgb888Ref;
  makeGradientRgb565(w, h, rgb565, rgb888Ref);

  TinyJPEGEncoder encoder;
  encoder.setQuality(90);
  encoder.setSubsample(false);  // isolate RGB565 unpacking error from chroma-subsampling error

  std::vector<uint8_t> jpg(16384);
  size_t jpgSize = 0;
  JERESULT erc = encoder.encodeJpg(rgb565.data(), (uint16_t)w, (uint16_t)h, JE_FMT_RGB565, jpg.data(), jpg.size(), jpgSize);
  assert(erc == JER_OK);
  assert(jpgSize > 4);
  jpg.resize(jpgSize);
  assert(jpg[0] == 0xFF && jpg[1] == 0xD8);
  assert(jpg[jpg.size() - 2] == 0xFF && jpg[jpg.size() - 1] == 0xD9);

  TinyJPEGDecoder decoder;
  uint16_t dw, dh;
  assert(decoder.getJpgSize(&dw, &dh, jpg.data(), jpg.size()) == JDR_OK);
  assert(dw == w && dh == h);

  PixelSink sink;
  sink.width = w;
  sink.height = h;
  sink.pixels.assign((size_t)w * h, 0);
  decoder.setUserData(&sink);
  decoder.setCallback(pixelSinkCallback);
  assert(decoder.drawJpg(0, 0, jpg.data(), jpg.size()) == JDR_OK);

  double sumAbs = 0;
  int maxDiff = 0;
  size_t n = (size_t)w * h;
  for (size_t i = 0; i < n; i++) {
    uint8_t r8, g8, b8;
    rgb565ToRgb888(sink.pixels[i], r8, g8, b8);
    int rr = rgb888Ref[i * 3 + 0], gg = rgb888Ref[i * 3 + 1], bb = rgb888Ref[i * 3 + 2];
    int d = std::abs(r8 - rr) + std::abs(g8 - gg) + std::abs(b8 - bb);
    sumAbs += d;
    if (d > maxDiff) maxDiff = d;
  }
  double meanDiff = sumAbs / (double)n;
  printf("test_encode_rgb565: mean=%.3f max=%d bytes=%zu\n", meanDiff, maxDiff, jpg.size());
  // Measured (not guessed) on this exact asset - same budget shape as
  // test_encode_roundtrip's 444/q90 case (RGB565 output quantization +
  // independent forward/inverse DCT and color-conversion rounding), since
  // the RGB565 *input* quantization was already factored out by comparing
  // against rgb888Ref above rather than the full-precision gradient.
  assert(meanDiff <= 14.0);
  assert(maxDiff <= 35);

  printf("test_encode_rgb565: OK\n");
  return 0;
}
