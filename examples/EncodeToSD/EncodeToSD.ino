/*
 * TinyJPEGEncoder + SD card example: encodes an in-memory RGB888 frame
 * (a small synthetic gradient, standing in for a camera frame buffer or
 * anything else already sitting in RAM) and writes it to an SD card as
 * "/photo.jpg" - a baseline (non-progressive) JPEG any conformant decoder,
 * including this library's own TinyJPEGDecoder, can read back.
 *
 * This demonstrates the templated encodeJpg(pixels, w, h, format, FileT&)
 * overload - see docs/encoding.md and docs/architecture.md. The same
 * template works unchanged for LittleFS/SPIFFS - just open the file with
 * LittleFS.open(...)/SPIFFS.open(...) instead of SD.open(...) below and
 * pass that handle instead.
 *
 * Requires Arduino's SD library in addition to TinyJPEG. Swap
 * makeGradientFrame() below for a real camera driver's frame buffer (e.g.
 * an ESP32 camera's fb->buf, converted to RGB888 if it isn't already) to
 * encode and save an actual photo instead.
 */

#include <TinyJPEGEncoder.h>
#include <SD.h>
#include "SPI.h"

using namespace tinyjpeg;

#define SD_CS 4

const uint16_t kWidth = 160;
const uint16_t kHeight = 120;

TinyJPEGEncoder encoder;
uint8_t frame[kWidth * kHeight * 3];  // RGB888, row-major

void makeGradientFrame() {
  for (uint16_t y = 0; y < kHeight; y++) {
    for (uint16_t x = 0; x < kWidth; x++) {
      uint8_t *p = &frame[((uint32_t)y * kWidth + x) * 3];
      p[0] = (uint8_t)(x * 255 / (kWidth - 1));   // R
      p[1] = (uint8_t)(y * 255 / (kHeight - 1));  // G
      p[2] = (uint8_t)((x + y) * 255 / (kWidth + kHeight - 2));  // B
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n\nTinyJPEGEncoder EncodeToSD example");

  if (!SD.begin(SD_CS)) {
    Serial.println("SD.begin failed!");
    while (1) delay(0);
  }

  encoder.setQuality(85);
  encoder.setSubsample(true);  // 4:2:0 - smaller files, negligible visual cost for photos

  makeGradientFrame();
}

void loop() {
  uint32_t t = millis();

  SD.remove("/photo.jpg");  // FILE_WRITE appends to an existing file rather than truncating it
  File f = SD.open("/photo.jpg", FILE_WRITE);
  if (!f) {
    Serial.println("Could not open /photo.jpg for writing.");
    delay(2000);
    return;
  }

  JERESULT r = encoder.encodeJpg(frame, kWidth, kHeight, JE_FMT_RGB888, f);
  f.close();

  t = millis() - t;
  if (r != JER_OK) {
    Serial.print("Encode failed, JERESULT=");
    Serial.println((int)r);
  } else {
    Serial.print("Wrote /photo.jpg in ");
    Serial.print(t);
    Serial.println(" ms");
  }

  delay(5000);
}
