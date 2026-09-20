/*
 * TinyJPEGEncoder + SD card example: encodes a raw RGB888 image read
 * incrementally from SD, one row at a time, into a JPEG also streamed
 * straight to SD - unlike examples/EncodeToSD (which holds the whole
 * source frame in one RAM array), this sketch never has more than a
 * single pixel row in memory at once, regardless of image size.
 *
 * This demonstrates the streaming row API - beginEncode()/writeRows()/
 * finishEncode() - which exists for exactly this: a source that produces
 * (or, as here, is read from) pixel rows over time instead of already
 * being a complete in-memory frame. See docs/encoding.md's "Streaming
 * rows" section and docs/architecture.md's "Row-band streaming" section
 * for the full design rationale.
 *
 * Peak pixel memory here is `kWidth * 3` bytes (one RGB888 row - 960
 * bytes at the 320x240 default below), plus the encoder's own internal
 * row-band buffer (a further `kWidth * 3 * 16` bytes worst case, pool-
 * allocated inside TinyJPEGEncoder's fixed workspace_ - see
 * TJPGE_WORKSPACE_SIZE in tjpge_config.h). Encoding the same 320x240
 * image via examples/EncodeToSD's encodeJpg(pixels, ...) instead would
 * need a `kWidth * kHeight * 3` = 230400-byte frame buffer up front -
 * over 240x more pixel RAM than this sketch ever holds.
 *
 * Input file contract: "/photo.rgb" on the SD card must be exactly
 * kWidth * kHeight * 3 bytes - raw, headerless, row-major, interleaved
 * RGB888 (no PNG/JPEG/BMP container, this sketch doesn't parse one). You
 * can produce a test file with ImageMagick or ffmpeg, e.g.:
 *   ffmpeg -i photo.jpg -vf scale=320:240 -pix_fmt rgb24 -f rawvideo photo.rgb
 * Copy the result to the SD card's root as "/photo.rgb" before running
 * this sketch.
 *
 * Requires Arduino's SD library in addition to TinyJPEG.
 */

#include <TinyJPEGEncoder.h>
#include <SD.h>
#include "SPI.h"

using namespace tinyjpeg;

#define SD_CS 4

const uint16_t kWidth = 320;
const uint16_t kHeight = 240;

TinyJPEGEncoder encoder;

// Adapts an SD File to TinyJPEGEncoder's OutputCallback - see
// docs/encoding.md's "Encoding to a custom destination" section for why
// this pattern (rather than a dedicated FileT overload) covers the file
// case too: any type with write(data, len) works this way, including a
// FileT's own.
size_t writeToFile(void *userData, const uint8_t *data, size_t len) {
  return static_cast<File *>(userData)->write(data, len);
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n\nTinyJPEGEncoder EncodeStreamFromSD example");

  if (!SD.begin(SD_CS)) {
    Serial.println("SD.begin failed!");
    while (1) delay(0);
  }

  encoder.setQuality(85);
  encoder.setSubsample(true);  // 4:2:0 - smaller files, negligible visual cost for photos
}

void loop() {
  File in = SD.open("/photo.rgb", FILE_READ);
  if (!in) {
    Serial.println("Could not open /photo.rgb - copy a raw RGB888 file with that name to the SD card root (see this sketch's file comment).");
    delay(5000);
    return;
  }
  if (in.size() != (uint32_t)kWidth * kHeight * 3) {
    Serial.print("/photo.rgb is ");
    Serial.print(in.size());
    Serial.print(" bytes, expected ");
    Serial.print((uint32_t)kWidth * kHeight * 3);
    Serial.println(" (kWidth * kHeight * 3) - wrong dimensions or not raw RGB888?");
    in.close();
    delay(5000);
    return;
  }

  SD.remove("/photo.jpg");  // FILE_WRITE appends to an existing file rather than truncating it
  File out = SD.open("/photo.jpg", FILE_WRITE);
  if (!out) {
    Serial.println("Could not open /photo.jpg for writing.");
    in.close();
    delay(2000);
    return;
  }

  uint32_t t = millis();

  JERESULT r = encoder.beginEncode(kWidth, kHeight, JE_FMT_RGB888, &writeToFile, &out);

  static uint8_t rowBuf[kWidth * 3];  // exactly one source row - the only pixel memory this sketch holds
  for (uint16_t y = 0; y < kHeight && r == JER_OK; y++) {
    size_t n = in.read(rowBuf, sizeof(rowBuf));
    if (n != sizeof(rowBuf)) {
      Serial.println("Short read from /photo.rgb.");
      r = JER_PAR;
      break;
    }
    r = encoder.writeRows(rowBuf, 1);
  }
  if (r == JER_OK) r = encoder.finishEncode();

  in.close();
  out.close();

  t = millis() - t;
  if (r != JER_OK) {
    Serial.print("Encode failed, JERESULT=");
    Serial.println((int)r);
  } else {
    Serial.print("Wrote /photo.jpg (streamed row-by-row) in ");
    Serial.print(t);
    Serial.println(" ms");
  }

  delay(5000);
}
