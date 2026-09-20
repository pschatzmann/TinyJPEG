// Correctness gate for the row-band streaming API (je_start()/
// je_write_rows()/je_finish() in tjpge.h) - the feature that removes
// je_encode()'s "whole frame must already be in RAM" requirement, see
// docs/architecture.md's "Streaming input" section.
//
// Two things are checked:
//  1. Byte-identical output: encoding the same image via je_encode()
//     (full buffer) versus via je_start()+repeated je_write_rows() calls
//     in irregular chunk sizes (not aligned to the 8/16-row band size)
//     +je_finish() must produce the exact same compressed bytes - proof
//     the streaming path is a real refactor of the same encode logic,
//     not a second, independently-verified implementation that happens
//     to also produce valid JPEGs.
//  2. The image is 70x45 - both dimensions deliberately NOT a multiple
//     of 8 or 16, so both the existing right-edge column padding
//     (get_rgb()'s x clamp) AND the new bottom-edge row padding
//     (je_finish()'s replication of the final partial band) are actually
//     exercised, for both subsample settings (mcuRows=8 and mcuRows=16).
//     Round-tripped through this repo's own decoder afterward, same
//     strategy as test_encode_roundtrip.cpp.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
#include "TinyJPEGDecoder.h"
#include "tjpge.h"
#include "test_helpers.h"

using namespace tinyjpeg;

namespace {

std::vector<uint8_t> makeGradientRgb(int w, int h) {
  std::vector<uint8_t> px((size_t)w * h * 3);
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      size_t i = ((size_t)y * w + x) * 3;
      px[i + 0] = (uint8_t)(x * 255 / (w - 1));
      px[i + 1] = (uint8_t)(y * 255 / (h - 1));
      px[i + 2] = (uint8_t)((x + y) * 255 / (w + h - 2));
    }
  }
  return px;
}

struct Sink {
  std::vector<uint8_t>* out;
};

size_t appendToVector(JENC* je, const uint8_t* data, size_t n) {
  static_cast<Sink*>(je->device)->out->insert(static_cast<Sink*>(je->device)->out->end(), data, data + n);
  return n;
}

std::vector<uint8_t> encodeFullBuffer(const std::vector<uint8_t>& rgb, int w, int h, bool subsample) {
  std::vector<uint8_t> jpg;
  Sink sink{&jpg};
  static uint8_t pool[TJPGE_WORKSPACE_SIZE];
  JENC je{};
  JERESULT rc = je_prepare(&je, appendToVector, pool, sizeof(pool), &sink, (uint16_t)w, (uint16_t)h, JE_FMT_RGB888);
  assert(rc == JER_OK);
  je.quality = 85;
  je.subsample = subsample ? 1 : 0;
  rc = je_encode(&je, rgb.data());
  assert(rc == JER_OK);
  return jpg;
}

// Feeds `rgb` through je_start()/je_write_rows()/je_finish() in chunks of
// `chunkRows` rows (the last chunk naturally shorter) - deliberately not a
// divisor of 8 or 16, so band boundaries and write() call boundaries never
// line up, proving je_write_rows() correctly buffers across calls rather
// than assuming one call per band.
std::vector<uint8_t> encodeStreamed(const std::vector<uint8_t>& rgb, int w, int h, bool subsample, int chunkRows) {
  std::vector<uint8_t> jpg;
  Sink sink{&jpg};
  static uint8_t pool[TJPGE_WORKSPACE_SIZE];
  JENC je{};
  JERESULT rc = je_prepare(&je, appendToVector, pool, sizeof(pool), &sink, (uint16_t)w, (uint16_t)h, JE_FMT_RGB888);
  assert(rc == JER_OK);
  je.quality = 85;
  je.subsample = subsample ? 1 : 0;

  rc = je_start(&je);
  assert(rc == JER_OK);

  int rowStride = w * 3;
  int rowsSent = 0;
  while (rowsSent < h) {
    int n = chunkRows;
    if (rowsSent + n > h) n = h - rowsSent;
    rc = je_write_rows(&je, rgb.data() + (size_t)rowsSent * rowStride, (uint16_t)n);
    assert(rc == JER_OK);
    rowsSent += n;
  }

  rc = je_finish(&je);
  assert(rc == JER_OK);
  return jpg;
}

void checkRoundTrip(const char* label, const std::vector<uint8_t>& jpg, const std::vector<uint8_t>& rgbRef, int w,
                     int h, double maxMean, int maxSample) {
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
    int rr = rgbRef[i * 3 + 0], gg = rgbRef[i * 3 + 1], bb = rgbRef[i * 3 + 2];
    int d = std::abs(r8 - rr) + std::abs(g8 - gg) + std::abs(b8 - bb);
    sumAbs += d;
    if (d > maxDiff) maxDiff = d;
  }
  double meanDiff = sumAbs / (double)n;
  printf("test_encode_streaming: %s mean=%.3f max=%d\n", label, meanDiff, maxDiff);
  assert(meanDiff <= maxMean);
  assert(maxDiff <= maxSample);
}

}  // namespace

int main() {
  const int w = 70, h = 45;  // neither dimension a multiple of 8 or 16
  auto rgb = makeGradientRgb(w, h);

  for (bool subsample : {false, true}) {
    auto full = encodeFullBuffer(rgb, w, h, subsample);
    auto streamed1 = encodeStreamed(rgb, w, h, subsample, /*chunkRows=*/1);   // worst case: one row per call
    auto streamed7 = encodeStreamed(rgb, w, h, subsample, /*chunkRows=*/7);   // divides neither 8 nor 16
    auto streamedAll = encodeStreamed(rgb, w, h, subsample, /*chunkRows=*/h); // one call, whole image

    assert(full == streamed1);
    assert(full == streamed7);
    assert(full == streamedAll);
    printf("test_encode_streaming: subsample=%d full/streamed byte-identical (%zu bytes)\n", (int)subsample,
           full.size());

    checkRoundTrip(subsample ? "420" : "444", full, rgb, w, h, 16.0, 45);
  }

  // Error contract: wrong call order/counts must fail cleanly, not corrupt
  // state or silently produce a truncated JPEG.
  {
    static uint8_t pool[TJPGE_WORKSPACE_SIZE];
    JENC je{};
    Sink sink{nullptr};
    std::vector<uint8_t> jpg;
    sink.out = &jpg;
    JERESULT rc = je_prepare(&je, appendToVector, pool, sizeof(pool), &sink, (uint16_t)w, (uint16_t)h, JE_FMT_RGB888);
    assert(rc == JER_OK);

    assert(je_write_rows(&je, rgb.data(), 1) == JER_PAR);  // before je_start()
    assert(je_finish(&je) == JER_PAR);                     // before je_start()

    assert(je_start(&je) == JER_OK);
    assert(je_start(&je) == JER_PAR);                                    // double start
    assert(je_finish(&je) == JER_PAR);                                   // no rows written yet
    assert(je_write_rows(&je, rgb.data(), (uint16_t)(h + 1)) == JER_PAR); // more rows than height
    assert(je_write_rows(&je, rgb.data(), (uint16_t)h) == JER_OK);
    assert(je_write_rows(&je, rgb.data(), 1) == JER_PAR);  // already have all rows
    assert(je_finish(&je) == JER_OK);
    assert(je_write_rows(&je, rgb.data(), 1) == JER_PAR);  // after finish
    assert(je_finish(&je) == JER_PAR);                     // double finish
  }

  printf("test_encode_streaming: OK\n");
  return 0;
}
