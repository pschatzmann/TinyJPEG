// Desktop (x86) timing for docs/decoding.md-adjacent performance reporting
// - not a correctness test (not added to ctest below), just real,
// measured numbers instead of guessed ones, the same rationale
// TinyMPG/TinyH264's own bench_native.cpp give for existing. Build/run
// with:
//   cmake --build build --target bench_native && ./build/test/native/bench_native
#include <chrono>
#include <cstdio>
#include "TinyJPEGDecoder.h"
#include "test_helpers.h"

using namespace tinyjpeg;

static bool discardCallback(TinyJPEGDecoder &, int16_t, int16_t, uint16_t, uint16_t, uint16_t *) { return true; }

static void bench(const char *label, const char *path, int reps) {
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

int main() {
  const int kReps = 2000;
  bench("gradient_444.jpg (64x48)", "assets/gradient_444.jpg", kReps);
  bench("checker_420.jpg (64x48)", "assets/checker_420.jpg", kReps);
  return 0;
}
