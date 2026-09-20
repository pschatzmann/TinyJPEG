/*
 * TinyJPEG on-device performance test: times both TinyJPEGDecoder and
 * TinyJPEGEncoder on real hardware, using the exact same 64x48 test
 * content test/native/bench_native.cpp uses for its own desktop
 * measurements (see docs/performance.md) - gradient_444.jpg/
 * checker_420.jpg (embedded via PerformanceTestAssets.h) for decode, and
 * a synthetically-generated RGB888 gradient (same formula
 * bench_native.cpp's makeGradientRgb() uses) for encode - so the numbers
 * this prints are directly comparable to the desktop ones already in
 * docs/performance.md, not just internally consistent with themselves.
 *
 * No SD card, LittleFS, or display needed - everything is embedded or
 * synthesized in RAM, so this runs unmodified as a drop-in benchmark on
 * any Arduino target with enough RAM for both workspaces (~3.5KB decode +
 * ~17.6KB encode with default settings - see docs/decoding.md/
 * docs/encoding.md). Written and measured against an ESP32-S3; see that
 * file's own comment for why PROGMEM works here without pgm_read_byte()
 * (ESP32 flash is memory-mapped) - a strict-Harvard-architecture target
 * (classic AVR) would need pgm_read_byte()-based copies instead before
 * handing the decoder a RAM buffer, since TinyJPEGDecoder itself reads
 * via plain pointer + memcpy, not PROGMEM-aware (see docs/decoding.md).
 */

#include <TinyJPEGDecoder.h>
#include <TinyJPEGEncoder.h>
#include "PerformanceTestAssets.h"

using namespace tinyjpeg;

static const int kDecodeReps = 200;
static const int kEncodeReps = 200;
static const int kWidth = 64;
static const int kHeight = 48;

TinyJPEGDecoder decoder;
TinyJPEGEncoder encoder;

static bool discardCallback(TinyJPEGDecoder &, int16_t, int16_t, uint16_t, uint16_t, uint16_t *) { return true; }

static void printFreeHeap(const char *label) {
  Serial.print(label);
  Serial.print(": ");
#if defined(ARDUINO_ARCH_ESP32)
  Serial.print(ESP.getFreeHeap());
  Serial.println(" bytes");
#elif defined(ARDUINO_ARCH_RP2040)
  Serial.print(rp2040.getFreeHeap());
  Serial.println(" bytes");
#else
  Serial.println("(not available on this core)");
#endif
}

// Mirrors bench_native.cpp's bench(): warms up once, then times `reps`
// back-to-back decodes of the same in-memory JPEG, reporting avg/min/max.
static void benchDecode(const char *label, const uint8_t *jpg, size_t jpgSize, int reps) {
  decoder.setCallback(discardCallback);
  decoder.drawJpg(0, 0, jpg, jpgSize);  // warm-up

  uint32_t minUs = UINT32_MAX, maxUs = 0;
  uint64_t totalUs = 0;
  for (int i = 0; i < reps; i++) {
    uint32_t t0 = micros();
    JRESULT r = decoder.drawJpg(0, 0, jpg, jpgSize);
    uint32_t us = micros() - t0;
    if (r != JDR_OK) {
      Serial.print(label);
      Serial.print(": decode failed, JRESULT=");
      Serial.println((int)r);
      return;
    }
    if (us < minUs) minUs = us;
    if (us > maxUs) maxUs = us;
    totalUs += us;
  }
  float avgUs = (float)totalUs / reps;
  Serial.print(label);
  Serial.print(" avg=");
  Serial.print(avgUs, 1);
  Serial.print(" us (");
  Serial.print((int)(1000000.0f / avgUs));
  Serial.print(" fps)  min=");
  Serial.print(minUs);
  Serial.print(" us  max=");
  Serial.print(maxUs);
  Serial.print(" us  (");
  Serial.print(reps);
  Serial.println(" reps)");
}

// Mirrors bench_native.cpp's makeGradientRgb(): same formula, so the
// encoder is fed byte-identical content on-device as it is on desktop.
static void makeGradientRgb(uint8_t *px, int w, int h) {
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      size_t i = ((size_t)y * w + x) * 3;
      px[i + 0] = (uint8_t)(x * 255 / (w - 1));
      px[i + 1] = (uint8_t)(y * 255 / (h - 1));
      px[i + 2] = (uint8_t)((x + y) * 255 / (w + h - 2));
    }
  }
}

static uint8_t gRgb[kWidth * kHeight * 3];
static uint8_t gOutBuf[16384];

// Mirrors bench_native.cpp's benchEncode(): warms up once, then times
// `reps` back-to-back encodes of the same in-memory RGB888 frame.
static void benchEncode(const char *label, bool subsample, int reps) {
  encoder.setQuality(85);
  encoder.setSubsample(subsample);

  size_t outSize = 0;
  JERESULT r = encoder.encodeJpg(gRgb, kWidth, kHeight, JE_FMT_RGB888, gOutBuf, sizeof(gOutBuf), outSize);  // warm-up
  if (r != JER_OK) {
    Serial.print(label);
    Serial.print(": encode failed, JERESULT=");
    Serial.println((int)r);
    return;
  }

  uint32_t minUs = UINT32_MAX, maxUs = 0;
  uint64_t totalUs = 0;
  for (int i = 0; i < reps; i++) {
    uint32_t t0 = micros();
    r = encoder.encodeJpg(gRgb, kWidth, kHeight, JE_FMT_RGB888, gOutBuf, sizeof(gOutBuf), outSize);
    uint32_t us = micros() - t0;
    if (r != JER_OK) {
      Serial.print(label);
      Serial.print(": encode failed, JERESULT=");
      Serial.println((int)r);
      return;
    }
    if (us < minUs) minUs = us;
    if (us > maxUs) maxUs = us;
    totalUs += us;
  }
  float avgUs = (float)totalUs / reps;
  Serial.print(label);
  Serial.print(" avg=");
  Serial.print(avgUs, 1);
  Serial.print(" us (");
  Serial.print((int)(1000000.0f / avgUs));
  Serial.print(" fps)  min=");
  Serial.print(minUs);
  Serial.print(" us  max=");
  Serial.print(maxUs);
  Serial.print(" us  (");
  Serial.print(reps);
  Serial.print(" reps, ");
  Serial.print(outSize);
  Serial.println(" bytes)");
}

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);
  Serial.println();
  Serial.println("TinyJPEG PerformanceTest");
#if defined(ARDUINO_ARCH_ESP32)
  Serial.print("Chip: ");
  Serial.print(ESP.getChipModel());
  Serial.print(" rev");
  Serial.print(ESP.getChipRevision());
  Serial.print(", ");
  Serial.print(ESP.getCpuFreqMHz());
  Serial.println(" MHz");
#endif
  printFreeHeap("Free heap at start");

  Serial.println();
  Serial.println("-- decode --");
  benchDecode("gradient_444.jpg (64x48)", kGradient444Jpg, kGradient444JpgSize, kDecodeReps);
  benchDecode("checker_420.jpg (64x48) ", kChecker420Jpg, kChecker420JpgSize, kDecodeReps);

  Serial.println();
  Serial.println("-- encode (q85, 64x48 RGB888 gradient) --");
  makeGradientRgb(gRgb, kWidth, kHeight);
  benchEncode("encode 4:4:4", /*subsample=*/false, kEncodeReps);
  benchEncode("encode 4:2:0", /*subsample=*/true, kEncodeReps);

  Serial.println();
  printFreeHeap("Free heap at end");
  Serial.println();
  Serial.println("Done.");
}

void loop() {
  delay(1000);
}
