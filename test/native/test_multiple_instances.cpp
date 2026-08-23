// TJpg_Decoder's original design relied on one process-wide global
// instance (`extern TJpg_Decoder TJpgDec`) plus a `thisPtr` self-pointer
// so its static C callback trampolines could reach instance state - see
// docs/architecture.md. TinyJPEGDecoder instead threads a per-call context
// through JDEC's own `device` field (tjpgd.h), so distinct instances carry
// fully independent state (scale, swap, callback, and now userData too)
// and don't interfere with each other. This test constructs two decoders
// configured differently - including two *separate* Bounds reached
// through the very same boundsCallback function via each decoder's own
// getUserData() - and checks that using one doesn't perturb the other:
// the property that wouldn't have held for the original singleton design,
// demonstrated here with the one callback function that wouldn't have been
// reusable like this without a per-instance userData to distinguish them.
#include <cassert>
#include <cstdio>
#include "TinyJPEGDecoder.h"
#include "test_helpers.h"

using namespace tinyjpeg;

struct Bounds {
  int maxX = 0;
  int maxY = 0;
};

static bool boundsCallback(TinyJPEGDecoder &decoder, int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *) {
  auto *bounds = static_cast<Bounds *>(decoder.getUserData());
  if (x + w > bounds->maxX) bounds->maxX = x + w;
  if (y + h > bounds->maxY) bounds->maxY = y + h;
  return true;
}

int main() {
  auto jpgGradient = readFile("assets/gradient_444.jpg");  // 64x48
  auto jpgChecker = readFile("assets/checker_420.jpg");    // 64x48

  Bounds boundsA, boundsB;

  TinyJPEGDecoder decoderA;
  decoderA.setJpgScale(1);
  decoderA.setUserData(&boundsA);
  decoderA.setCallback(boundsCallback);

  TinyJPEGDecoder decoderB;
  decoderB.setJpgScale(4);
  decoderB.setUserData(&boundsB);
  decoderB.setCallback(boundsCallback);

  // Interleave calls on the two instances - each must keep decoding
  // correctly with its own scale/userData, unaffected by the other's calls
  // in between (a shared-singleton implementation would have one instance
  // clobber the other's `jpgScale`/`tft_output`/userData here).
  boundsA = Bounds{};
  JRESULT ra1 = decoderA.drawJpg(0, 0, jpgGradient.data(), jpgGradient.size());
  assert(ra1 == JDR_OK);
  assert(boundsA.maxX == 64 && boundsA.maxY == 48);

  boundsB = Bounds{};
  JRESULT rb1 = decoderB.drawJpg(0, 0, jpgChecker.data(), jpgChecker.size());
  assert(rb1 == JDR_OK);
  assert(boundsB.maxX == 16 && boundsB.maxY == 12);  // scale 4 -> 64/4 x 48/4

  boundsA = Bounds{};
  JRESULT ra2 = decoderA.drawJpg(0, 0, jpgChecker.data(), jpgChecker.size());
  assert(ra2 == JDR_OK);
  assert(boundsA.maxX == 64 && boundsA.maxY == 48);  // decoderA's own scale (1) unaffected by decoderB's scale (4)

  boundsB = Bounds{};
  JRESULT rb2 = decoderB.drawJpg(0, 0, jpgGradient.data(), jpgGradient.size());
  assert(rb2 == JDR_OK);
  assert(boundsB.maxX == 16 && boundsB.maxY == 12);  // decoderB's own scale (4) unaffected by decoderA's scale (1)

  printf("test_multiple_instances: OK\n");
  return 0;
}
