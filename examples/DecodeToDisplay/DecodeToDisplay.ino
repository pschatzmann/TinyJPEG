/*
 * TinyJPEGDecoder + TFT_eSPI display example: decodes a small embedded
 * baseline JPEG (32x32, see jpeg_test_image.h) and pushes it to a TFT SPI
 * display, repeating with a scale-factor sweep (1, 2, 4) so the effect of
 * setJpgScale() is visible. Requires the TFT_eSPI library
 * (https://github.com/Bodmer/TFT_eSPI, configured for your specific
 * board/display - see that library's own setup instructions) in addition
 * to TinyJPEG.
 *
 * This mirrors TJpg_Decoder's own FLash_array/Flash_Jpg_GFX.ino example
 * as closely as the API change allows - see docs/architecture.md's "API
 * changes from TJpg_Decoder" table for the exact mapping if you're
 * migrating an existing sketch. In particular, tft_output() below reaches
 * `tft` via decoder.getUserData() rather than as a global, to show the
 * pattern that matters once a sketch has more than one TinyJPEGDecoder
 * (each pointed at a different display, say) and a single callback
 * function needs to tell them apart - see docs/decoding.md.
 */

#include <TinyJPEGDecoder.h>
#include "jpeg_test_image.h"

#include "SPI.h"
#include <TFT_eSPI.h>

using namespace tinyjpeg;

TFT_eSPI tft = TFT_eSPI();
TinyJPEGDecoder decoder;

// Called once per decoded MCU block, from inside decoder.drawJpg() below.
// If you use a different display library, adapt this function to suit -
// e.g. Adafruit_GFX's tft.drawRGBBitmap(x, y, bitmap, w, h) instead of
// TFT_eSPI's pushImage().
bool tft_output(TinyJPEGDecoder &decoder, int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  TFT_eSPI *tft = static_cast<TFT_eSPI *>(decoder.getUserData());
  if (y >= tft->height()) return false;  // stop decoding once past the bottom of the screen
  tft->pushImage(x, y, w, h, bitmap);     // clips automatically at the TFT boundaries
  return true;                           // true: keep decoding
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n\nTinyJPEGDecoder DecodeToDisplay example");

  tft.begin();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  decoder.setSwapBytes(true);  // TFT_eSPI expects big-endian RGB565 pixel words
  decoder.setUserData(&tft);
  decoder.setCallback(tft_output);
}

void loop() {
  for (uint8_t scale : {1, 2, 4}) {
    tft.fillScreen(TFT_BLACK);
    decoder.setJpgScale(scale);

    uint32_t t = millis();

    uint16_t w = 0, h = 0;
    decoder.getJpgSize(&w, &h, kTestImage, kTestImageSize);

    JRESULT r = decoder.drawJpg(10, 10, kTestImage, kTestImageSize);

    t = millis() - t;
    Serial.print("scale=");
    Serial.print(scale);
    Serial.print(": ");
    Serial.print(w / scale);
    Serial.print("x");
    Serial.print(h / scale);
    Serial.print(", result=");
    Serial.print((int)r);
    Serial.print(", ");
    Serial.print(t);
    Serial.println(" ms");

    delay(1500);
  }
}
