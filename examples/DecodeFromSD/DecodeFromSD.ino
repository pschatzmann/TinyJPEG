/*
 * TinyJPEGDecoder + SD card example: decodes a JPEG file from an SD card
 * and pushes it to a TFT SPI display. Save a baseline (non-progressive)
 * JPEG named "/image.jpg" to the root of an SD card (formatted FAT/FAT32)
 * before running this.
 *
 * This demonstrates the templated drawJpg(x, y, FileT&)/
 * getJpgSize(w, h, FileT&) overloads that replace TJpg_Decoder's
 * drawSdJpg()/getSdJpgSize() - see docs/decoding.md and
 * docs/architecture.md's "API changes from TJpg_Decoder" table. The same
 * template works unchanged for LittleFS/SPIFFS - just open the file with
 * LittleFS.open(...)/SPIFFS.open(...) instead of SD.open(...) below and
 * pass that handle instead.
 *
 * Requires the TFT_eSPI library (https://github.com/Bodmer/TFT_eSPI,
 * configured for your specific board/display) and Arduino's SD library in
 * addition to TinyJPEG.
 */

#include <TinyJPEGDecoder.h>
#include <SD.h>
#include "SPI.h"
#include <TFT_eSPI.h>

using namespace tinyjpeg;

#define SD_CS 4

TFT_eSPI tft = TFT_eSPI();
TinyJPEGDecoder decoder;

bool tft_output(TinyJPEGDecoder &decoder, int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  TFT_eSPI *tft = static_cast<TFT_eSPI *>(decoder.getUserData());
  if (y >= tft->height()) return false;
  tft->pushImage(x, y, w, h, bitmap);
  return true;
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n\nTinyJPEGDecoder DecodeFromSD example");

  if (!SD.begin(SD_CS)) {
    Serial.println("SD.begin failed!");
    while (1) delay(0);
  }

  tft.begin();
  tft.fillScreen(TFT_BLACK);

  decoder.setSwapBytes(true);
  decoder.setJpgScale(1);
  decoder.setUserData(&tft);
  decoder.setCallback(tft_output);
}

void loop() {
  tft.fillScreen(TFT_BLACK);
  uint32_t t = millis();

  // getJpgSize()/drawJpg() each open their own File handle here so the
  // file is read from the start both times - a real application decoding
  // just once per file would only need to open() once, right before
  // drawJpg().
  File sizeFile = SD.open("/image.jpg", FILE_READ);
  uint16_t w = 0, h = 0;
  if (sizeFile) {
    decoder.getJpgSize(&w, &h, sizeFile);
    sizeFile.close();
  }
  Serial.print("Width = ");
  Serial.print(w);
  Serial.print(", height = ");
  Serial.println(h);

  File drawFile = SD.open("/image.jpg", FILE_READ);
  if (drawFile) {
    JRESULT r = decoder.drawJpg(0, 0, drawFile);
    drawFile.close();
    if (r != JDR_OK) {
      Serial.print("Decode failed, JRESULT=");
      Serial.println((int)r);
    }
  } else {
    Serial.println("Could not open /image.jpg - copy a baseline JPEG to the SD card root with that name.");
  }

  t = millis() - t;
  Serial.print(t);
  Serial.println(" ms");

  delay(2000);
}
