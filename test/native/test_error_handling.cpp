// Malformed/absent input must fail cleanly with a JRESULT != JDR_OK - never
// crash, hang, or silently fire the block callback with garbage - across
// jd_prepare()'s own error paths (empty input, no SOI marker, truncated
// mid-stream) and drawJpg()'s JDR_INTR path (callback aborts the decode).
#include <cassert>
#include <cstdio>
#include "TinyJPEGDecoder.h"
#include "test_helpers.h"

using namespace tinyjpeg;

static bool neverCalled(TinyJPEGDecoder &, int16_t, int16_t, uint16_t, uint16_t, uint16_t *) {
  assert(false && "callback must not fire for a decode that fails during jd_prepare()");
  return true;
}

int main() {
  TinyJPEGDecoder decoder;
  decoder.setCallback(neverCalled);

  // Empty input - no SOI marker possible at all.
  assert(decoder.drawJpg(0, 0, static_cast<const uint8_t *>(nullptr), (size_t)0) != JDR_OK);

  // Non-JPEG garbage.
  uint8_t garbage[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  assert(decoder.drawJpg(0, 0, garbage, sizeof(garbage)) != JDR_OK);

  // A real JPEG's header, truncated well before the scan data.
  auto jpg = readFile("assets/gradient_444.jpg");
  assert(jpg.size() > 50);
  std::vector<uint8_t> truncated(jpg.begin(), jpg.begin() + 50);
  assert(decoder.drawJpg(0, 0, truncated.data(), truncated.size()) != JDR_OK);

  // A real, otherwise-valid JPEG whose callback aborts on the very first
  // block: drawJpg() must surface that as JDR_INTR, not JDR_OK.
  struct AbortOnce {
    static bool cb(TinyJPEGDecoder &, int16_t, int16_t, uint16_t, uint16_t, uint16_t *) { return false; }
  };
  TinyJPEGDecoder decoder2;
  decoder2.setCallback(AbortOnce::cb);
  JRESULT r = decoder2.drawJpg(0, 0, jpg.data(), jpg.size());
  assert(r == JDR_INTR);

  printf("test_error_handling: OK\n");
  return 0;
}
