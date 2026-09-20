// Desktop (x86) timing for docs/performance.md - not a correctness test
// (not added to ctest below), just real, measured numbers instead of
// guessed ones, the same rationale TinyMPG/TinyH264's own bench_native.cpp
// give for existing. Build/run with:
//   cmake --build build --target bench_native && cd test/native && ../build/test/native/bench_native
#include <chrono>
#include <cstdio>
#include <vector>
#include "TinyJPEGDecoder.h"
#include "TinyJPEGEncoder.h"
#include "test_helpers.h"

using namespace tinyjpeg;

static bool discardCallback(TinyJPEGDecoder &, int16_t, int16_t, uint16_t, uint16_t, uint16_t *) { return true; }

static void benchDecode(const char *label, const char *path, int reps) {
  auto jpg = readFile(path);
  TinyJPEGDecoder decoder;
  decoder.setCallback(discardCallback);

  // Warm up (first decode pays for e.g. any one-time page faults touching
  // the workspace array for the first time).
  decoder.drawJpg(0, 0, jpg.data(), jpg.size());

  uint64_t minNs = UINT64_MAX, maxNs = 0, totalNs = 0;
  for (int i = 0; i < reps; i++) {
    auto t0 = std::chrono::steady_clock::now();
    JRESULT r = decoder.drawJpg(0, 0, jpg.data(), jpg.size());
    auto t1 = std::chrono::steady_clock::now();
    if (r != JDR_OK) {
      fprintf(stderr, "%s: decode failed, JRESULT=%d\n", label, r);
      return;
    }
    uint64_t ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    if (ns < minNs) minNs = ns;
    if (ns > maxNs) maxNs = ns;
    totalNs += ns;
  }
  double avgUs = (double)totalNs / reps / 1000.0;
  printf("%-24s avg=%.1f us (%.0f fps)  min=%.1f us  max=%.1f us  (%d reps)\n", label, avgUs,
         1000000.0 / avgUs, minNs / 1000.0, maxNs / 1000.0, reps);
}

static std::vector<uint8_t> makeGradientRgb(int w, int h) {
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

static void benchEncode(const char *label, const std::vector<uint8_t> &rgb, int w, int h, bool subsample, int reps) {
  TinyJPEGEncoder encoder;
  encoder.setQuality(85);
  encoder.setSubsample(subsample);
  std::vector<uint8_t> outBuf(65536);

  // Warm up.
  size_t outSize = 0;
  JERESULT r = encoder.encodeJpg(rgb.data(), (uint16_t)w, (uint16_t)h, JE_FMT_RGB888, outBuf.data(), outBuf.size(),
                                  outSize);
  if (r != JER_OK) {
    fprintf(stderr, "%s: encode failed, JERESULT=%d\n", label, r);
    return;
  }

  uint64_t minNs = UINT64_MAX, maxNs = 0, totalNs = 0;
  for (int i = 0; i < reps; i++) {
    auto t0 = std::chrono::steady_clock::now();
    r = encoder.encodeJpg(rgb.data(), (uint16_t)w, (uint16_t)h, JE_FMT_RGB888, outBuf.data(), outBuf.size(), outSize);
    auto t1 = std::chrono::steady_clock::now();
    if (r != JER_OK) {
      fprintf(stderr, "%s: encode failed, JERESULT=%d\n", label, r);
      return;
    }
    uint64_t ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    if (ns < minNs) minNs = ns;
    if (ns > maxNs) maxNs = ns;
    totalNs += ns;
  }
  double avgUs = (double)totalNs / reps / 1000.0;
  printf("%-24s avg=%.1f us (%.0f fps)  min=%.1f us  max=%.1f us  (%d reps, %zu bytes)\n", label, avgUs,
         1000000.0 / avgUs, minNs / 1000.0, maxNs / 1000.0, reps, outSize);
}

int main() {
  const int kReps = 2000;

  printf("-- decode --\n");
  benchDecode("gradient_444.jpg (64x48)", "assets/gradient_444.jpg", kReps);
  benchDecode("checker_420.jpg (64x48)", "assets/checker_420.jpg", kReps);

  printf("-- encode (q85, 64x48 RGB888 gradient) --\n");
  auto rgb = makeGradientRgb(64, 48);
  benchEncode("encode 4:4:4", rgb, 64, 48, /*subsample=*/false, kReps);
  benchEncode("encode 4:2:0", rgb, 64, 48, /*subsample=*/true, kReps);

  return 0;
}
