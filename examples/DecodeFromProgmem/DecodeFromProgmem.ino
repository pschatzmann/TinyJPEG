/*
 * TinyJPEGDecoder minimal example: decodes a tiny embedded baseline JPEG
 * (32x32, 4:2:0 - see jpeg_test_image.h) and prints per-block stats to
 * Serial. No SD card, LittleFS, or display needed, so this runs unmodified
 * as a smoke test that the library is working - the decoder core is plain
 * portable C++17 with no ESP32-specific dependencies, so this example
 * builds for any Arduino target with enough RAM for the ~3.5KB decode
 * workspace (JD_FASTDECODE=1, the default - see src/tjpgd_config.h).
 *
 * For a real application, feed drawJpg()/getJpgSize() with data read from
 * an SD card, LittleFS/SPIFFS, or a network stream instead of the embedded
 * test image - see docs/decoding.md and examples/DecodeFromSD.
 */

#include <TinyJPEGDecoder.h>
#include "jpeg_test_image.h"

using namespace tinyjpeg;

TinyJPEGDecoder decoder;
int blockCount = 0;
uint32_t lumaSum = 0;
uint32_t pixelCount = 0;

/*
 * Decode the image this many times back to back to get a stable timing
 * average instead of judging performance off a single decode.
 */
static const int kBenchmarkReps = 30;

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

// Called once per decoded MCU block, from inside drawJpg() below. A real
// application would push (x, y, w, h, data) straight to a display (e.g.
// tft.pushImage(x, y, w, h, data) for a TFT_eSPI display) instead of just
// accumulating a luma sum - see examples/DecodeToDisplay for that. This
// sketch has just the one global `decoder`, so plain globals (blockCount/
// lumaSum/pixelCount) are enough to hold the callback's running totals;
// setUserData()/getUserData() (see docs/decoding.md) is there for the
// case this simple example doesn't need - juggling more than one decoder
// instance, or avoiding globals for a value the callback needs.
bool onBlock(TinyJPEGDecoder &decoder, int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *data) {
  (void)decoder;
  (void)x;
  (void)y;
  blockCount++;
  for (uint16_t i = 0; i < (uint16_t)(w * h); i++) {
    uint16_t px = data[i];
    uint8_t r = (px >> 8) & 0xF8, g = (px >> 3) & 0xFC, b = (px << 3) & 0xF8;
    lumaSum += (uint32_t)r + g + b;
    pixelCount++;
  }
  return true;  // return false here to abort the decode early
}

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);
  Serial.println("TinyJPEGDecoder DecodeFromProgmem example");
  printFreeHeap("Free heap before decode");

  decoder.setCallback(onBlock);

  uint16_t w, h;
  JRESULT sizeResult = decoder.getJpgSize(&w, &h, kTestImage, kTestImageSize);
  Serial.print("Image size: ");
  Serial.print(w);
  Serial.print("x");
  Serial.print(h);
  Serial.print(" (getJpgSize result=");
  Serial.print((int)sizeResult);
  Serial.println(")");

  uint32_t startUs = micros();
  for (int rep = 0; rep < kBenchmarkReps; rep++) {
    JRESULT r = decoder.drawJpg(0, 0, kTestImage, kTestImageSize);
    if (r != JDR_OK) {
      Serial.print("Decode failed, JRESULT=");
      Serial.println((int)r);
      break;
    }
  }
  uint32_t totalUs = micros() - startUs;

  Serial.print("Decoded ");
  Serial.print(blockCount / kBenchmarkReps);
  Serial.print(" block(s)/rep over ");
  Serial.print(kBenchmarkReps);
  Serial.println(" repetition(s).");
  Serial.print("Average luma: ");
  Serial.println(pixelCount ? (uint32_t)(lumaSum / pixelCount) : 0);
  Serial.print("Total decode time: ");
  Serial.print(totalUs);
  Serial.print(" us (");
  Serial.print((float)totalUs / kBenchmarkReps);
  Serial.println(" us/rep)");

  printFreeHeap("Free heap after decode");
}

void loop() {
  delay(1000);
}
