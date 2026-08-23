// setJpgScale(1|2|4|8) must produce output dimensions divided by that
// factor (JD_USE_SCALE path in tjpgd's mcu_output()), and an unrecognized
// factor must fall back to 1 (full size) rather than doing anything
// undefined - see TinyJPEGDecoder::setJpgScale()'s default case.
#include <cassert>
#include <cstdio>
#include "TinyJPEGDecoder.h"
#include "test_helpers.h"

using namespace tinyjpeg;

struct Bounds {
  int maxX = 0;
  int maxY = 0;
};

// Reaches its Bounds via TinyJPEGDecoder::getUserData() rather than a
// global variable - see test_helpers.h's PixelSink comment for why that's
// what the `decoder` parameter is for.
static bool boundsCallback(TinyJPEGDecoder &decoder, int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *) {
  auto *bounds = static_cast<Bounds *>(decoder.getUserData());
  if (x + w > bounds->maxX) bounds->maxX = x + w;
  if (y + h > bounds->maxY) bounds->maxY = y + h;
  return true;
}

static void checkScale(const std::vector<uint8_t> &jpg, uint8_t scaleFactor, int expectW, int expectH) {
  Bounds bounds;
  TinyJPEGDecoder decoder;
  decoder.setJpgScale(scaleFactor);
  decoder.setUserData(&bounds);
  decoder.setCallback(boundsCallback);
  JRESULT r = decoder.drawJpg(0, 0, jpg.data(), jpg.size());
  assert(r == JDR_OK);
  assert(bounds.maxX == expectW);
  assert(bounds.maxY == expectH);
}

int main() {
  auto jpg = readFile("assets/gradient_444.jpg");  // 64x48

  checkScale(jpg, 1, 64, 48);
  checkScale(jpg, 2, 32, 24);
  checkScale(jpg, 4, 16, 12);
  checkScale(jpg, 8, 8, 6);
  checkScale(jpg, 3, 64, 48);   // unrecognized factor -> falls back to scale 1 (full size)
  checkScale(jpg, 0, 64, 48);   // unrecognized factor -> falls back to scale 1 (full size)

  // x/y offset must be added on top of the (already scaled) block coordinates.
  Bounds bounds;
  TinyJPEGDecoder decoder;
  decoder.setUserData(&bounds);
  decoder.setCallback(boundsCallback);
  JRESULT r = decoder.drawJpg(100, 200, jpg.data(), jpg.size());
  assert(r == JDR_OK);
  assert(bounds.maxX == 100 + 64);
  assert(bounds.maxY == 200 + 48);

  printf("test_scale: OK\n");
  return 0;
}
