// getJpgSize() must report correct dimensions from just the SOF0 header -
// jd_prepare() alone, without ever reaching jd_decomp() - and must not
// decode any pixel data (no callback is even registered here).
#include <cassert>
#include <cstdio>
#include "TinyJPEGDecoder.h"
#include "test_helpers.h"

using namespace tinyjpeg;

static void checkSize(const char *path, uint16_t expectW, uint16_t expectH) {
  auto jpg = readFile(path);
  TinyJPEGDecoder decoder;
  uint16_t w = 0xFFFF, h = 0xFFFF;
  JRESULT r = decoder.getJpgSize(&w, &h, jpg.data(), jpg.size());
  assert(r == JDR_OK);
  assert(w == expectW);
  assert(h == expectH);
}

int main() {
  checkSize("assets/gradient_444.jpg", 64, 48);
  checkSize("assets/checker_420.jpg", 64, 48);
  checkSize("assets/tiny_32.jpg", 32, 32);
  checkSize("assets/gray_32x24.jpg", 32, 24);

  // w/h must be zeroed out on failure, not left at a caller's prior value.
  TinyJPEGDecoder decoder;
  uint8_t garbage[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  uint16_t w = 999, h = 999;
  JRESULT r = decoder.getJpgSize(&w, &h, garbage, sizeof(garbage));
  assert(r != JDR_OK);
  assert(w == 0);
  assert(h == 0);

  printf("test_get_size: OK\n");
  return 0;
}
