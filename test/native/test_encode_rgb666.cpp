// Correctness gate for JE_FMT_RGB666 input. Unlike JE_FMT_RGB565 (which
// needs real unpacking in tjpge_detail::get_rgb()), JE_FMT_RGB666 is
// handled byte-identically to JE_FMT_RGB888 - see JEPixelFormat's own
// comment in tjpge.h - so this test's job isn't to prove a new decode
// path works, it's to prove je_prepare()/encodeJpg() actually *accept*
// JE_FMT_RGB666 (not JER_PAR), derive ncomp==3 for it the same as
// JE_FMT_RGB888, and round-trip correctly - the same strategy
// test_encode_rgb565.cpp uses for its own format.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
#include "TinyJPEGDecoder.h"
#include "TinyJPEGEncoder.h"
#include "test_helpers.h"

using namespace tinyjpeg;

namespace {

// A gradient quantized to 6 significant bits per channel (low 2 bits
// zeroed, as JE_FMT_RGB666 documents - "don't-care", here simply 0),
// stored 3 bytes/pixel like RGB888. This buffer doubles as its own
// reference: get_rgb() reads JE_FMT_RGB666 exactly like JE_FMT_RGB888, no
// bit expansion, so a lossless round-trip should reproduce these same
// byte values.
std::vector<uint8_t> makeGradientRgb666(int w, int h) {
  std::vector<uint8_t> px((size_t)w * h * 3);
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      size_t i = ((size_t)y * w + x) * 3;
      px[i + 0] = (uint8_t)(x * 255 / (w - 1)) & 0xFC;
      px[i + 1] = (uint8_t)(y * 255 / (h - 1)) & 0xFC;
      px[i + 2] = (uint8_t)((x + y) * 255 / (w + h - 2)) & 0xFC;
    }
  }
  return px;
}

}  // namespace

int main() {
  const int w = 64, h = 48;
  auto rgb666 = makeGradientRgb666(w, h);

  TinyJPEGEncoder encoder;
  encoder.setQuality(90);
  encoder.setSubsample(false);

  std::vector<uint8_t> jpg(16384);
  size_t jpgSize = 0;
  JERESULT erc = encoder.encodeJpg(rgb666.data(), (uint16_t)w, (uint16_t)h, JE_FMT_RGB666, jpg.data(), jpg.size(), jpgSize);
  assert(erc == JER_OK);
  assert(jpgSize > 4);
  jpg.resize(jpgSize);
  assert(jpg[0] == 0xFF && jpg[1] == 0xD8);
  assert(jpg[jpg.size() - 2] == 0xFF && jpg[jpg.size() - 1] == 0xD9);

  TinyJPEGDecoder decoder;
  uint16_t dw, dh;
  assert(decoder.getJpgSize(&dw, &dh, jpg.data(), jpg.size()) == JDR_OK);
  assert(dw == w && dh == h);  // confirms SOF0 carries the true dimensions, i.e. ncomp/format were accepted, not silently rejected

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
    int rr = rgb666[i * 3 + 0], gg = rgb666[i * 3 + 1], bb = rgb666[i * 3 + 2];
    int d = std::abs(r8 - rr) + std::abs(g8 - gg) + std::abs(b8 - bb);
    sumAbs += d;
    if (d > maxDiff) maxDiff = d;
  }
  double meanDiff = sumAbs / (double)n;
  printf("test_encode_rgb666: mean=%.3f max=%d bytes=%zu\n", meanDiff, maxDiff, jpg.size());
  // Same budget shape as test_encode_roundtrip's 444/q90 case - JE_FMT_RGB666
  // contributes no error of its own here (no bit expansion, unlike RGB565),
  // so this should track that test's numbers closely.
  assert(meanDiff <= 14.0);
  assert(maxDiff <= 35);

  printf("test_encode_rgb666: OK\n");
  return 0;
}
