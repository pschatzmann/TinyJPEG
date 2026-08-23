// The templated drawJpg(x, y, FileT&) / getJpgSize(w, h, FileT&) overloads
// (TinyJPEGDecoder.h) are the header-only, Arduino-independent replacement
// for TJpg_Decoder's drawSdJpg()/drawFsJpg() - see docs/decoding.md for the
// exact FileT contract (available()/read()/position()/seek()) that lets
// the same code serve SD, LittleFS, SPIFFS, or (as here) a native stdio
// FILE* wrapper. This test's NativeFile below is that from-scratch FileT,
// standing in for what would be Arduino's fs::File/SD's File on-device -
// proving the template compiles and behaves correctly against any
// conforming type, not just Arduino's own.
#include <cassert>
#include <cstdio>
#include "TinyJPEGDecoder.h"
#include "test_helpers.h"

using namespace tinyjpeg;

// Minimal FileT: wraps a plain stdio FILE* behind the four methods
// TinyJPEGDecoder::FileContext<FileT> calls - see TinyJPEGDecoder.h.
class NativeFile {
 public:
  explicit NativeFile(const char *path) { f_ = fopen(path, "rb"); }
  ~NativeFile() {
    if (f_) fclose(f_);
  }

  size_t available() {
    long cur = ftell(f_);
    fseek(f_, 0, SEEK_END);
    long end = ftell(f_);
    fseek(f_, cur, SEEK_SET);
    return (size_t)(end - cur);
  }
  size_t read(uint8_t *buf, size_t len) { return fread(buf, 1, len, f_); }
  size_t position() { return (size_t)ftell(f_); }
  bool seek(size_t pos) { return fseek(f_, (long)pos, SEEK_SET) == 0; }

 private:
  FILE *f_ = nullptr;
};

int main() {
  // getJpgSize() via the file template must match the array overload.
  NativeFile sizeFile("assets/gradient_444.jpg");
  TinyJPEGDecoder decoder;
  uint16_t fw, fh;
  assert(decoder.getJpgSize(&fw, &fh, sizeFile) == JDR_OK);
  assert(fw == 64 && fh == 48);

  // drawJpg() via the file template must decode to the exact same pixels
  // as the array overload on the same bytes (both exercise the identical
  // tjpgd core - this test isn't re-checking decode correctness, just that
  // the FileT plumbing (read()/seek() with a null buffer for skips, EOF
  // handling via available()) doesn't alter the result).
  auto jpg = readFile("assets/gradient_444.jpg");
  PixelSink arraySink;
  arraySink.width = fw;
  arraySink.height = fh;
  arraySink.pixels.assign((size_t)fw * fh, 0);
  TinyJPEGDecoder arrayDecoder;
  arrayDecoder.setUserData(&arraySink);
  arrayDecoder.setCallback(pixelSinkCallback);
  assert(arrayDecoder.drawJpg(0, 0, jpg.data(), jpg.size()) == JDR_OK);

  PixelSink fileSink;
  fileSink.width = fw;
  fileSink.height = fh;
  fileSink.pixels.assign((size_t)fw * fh, 0);
  NativeFile drawFile("assets/gradient_444.jpg");
  TinyJPEGDecoder fileDecoder;
  fileDecoder.setUserData(&fileSink);
  fileDecoder.setCallback(pixelSinkCallback);
  assert(fileDecoder.drawJpg(0, 0, drawFile) == JDR_OK);

  assert(arraySink.pixels == fileSink.pixels);

  printf("test_file_source: OK\n");
  return 0;
}
