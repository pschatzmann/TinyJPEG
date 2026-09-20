// Exercises TinyJPEGEncoder's three output sinks (fixed array, file-like
// object, plain callback - TinyJPEGEncoder.h) against each other and
// against a direct decode, mirroring how test_file_source.cpp/
// test_callback_source.cpp prove TinyJPEGDecoder's own multiple-source
// plumbing without re-testing decode correctness itself. The actual
// encoded-bitstream correctness is test_encode_roundtrip.cpp's job (it
// calls tjpge.h directly); this test's job is just "does the wrapper class
// deliver the identical bytes regardless of which sink you hand it".
#include <cassert>
#include <cstdio>
#include <vector>
#include "TinyJPEGDecoder.h"
#include "TinyJPEGEncoder.h"
#include "test_helpers.h"

using namespace tinyjpeg;

namespace {

std::vector<uint8_t> makeGradientRgb(int w, int h) {
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

// Minimal FileT: wraps a plain stdio FILE* behind the one method
// TinyJPEGEncoder::FileContext<FileT> calls - see test_file_source.cpp's
// own NativeFile for the decoder-side equivalent.
class NativeFile {
 public:
  explicit NativeFile(const char *path) { f_ = fopen(path, "wb"); }
  ~NativeFile() {
    if (f_) fclose(f_);
  }
  size_t write(const uint8_t *buf, size_t len) { return fwrite(buf, 1, len, f_); }

 private:
  FILE *f_ = nullptr;
};

}  // namespace

int main() {
  const int w = 64, h = 48;
  auto rgb = makeGradientRgb(w, h);

  // Array sink.
  TinyJPEGEncoder arrayEncoder;
  arrayEncoder.setQuality(85);
  arrayEncoder.setSubsample(false);
  std::vector<uint8_t> arrayBuf(16384);
  size_t arraySize = 0;
  JERESULT r1 = arrayEncoder.encodeJpg(rgb.data(), w, h, JE_FMT_RGB888, arrayBuf.data(), arrayBuf.size(), arraySize);
  assert(r1 == JER_OK);
  assert(arraySize > 4);
  arrayBuf.resize(arraySize);

  // File sink, same settings - must produce byte-identical output.
  {
    TinyJPEGEncoder fileEncoder;
    fileEncoder.setQuality(85);
    fileEncoder.setSubsample(false);
    NativeFile f("test_encoder_wrapper_out.jpg");
    JERESULT r2 = fileEncoder.encodeJpg(rgb.data(), w, h, JE_FMT_RGB888, f);
    assert(r2 == JER_OK);
  }
  auto fileBuf = readFile("test_encoder_wrapper_out.jpg");
  assert(fileBuf == arrayBuf);

  // Callback sink, same settings - must also produce byte-identical output.
  TinyJPEGEncoder callbackEncoder;
  callbackEncoder.setQuality(85);
  callbackEncoder.setSubsample(false);
  std::vector<uint8_t> callbackBuf;
  auto writeCb = [](void *userData, const uint8_t *data, size_t len) -> size_t {
    auto *out = static_cast<std::vector<uint8_t> *>(userData);
    out->insert(out->end(), data, data + len);
    return len;
  };
  JERESULT r3 = callbackEncoder.encodeJpg(rgb.data(), w, h, JE_FMT_RGB888, writeCb, &callbackBuf);
  assert(r3 == JER_OK);
  assert(callbackBuf == arrayBuf);

  // The array sink's output must itself be a decodable JPEG matching the
  // source dimensions (bitstream-level correctness is test_encode_roundtrip's
  // job; this is just a sanity check that the wrapper didn't corrupt anything).
  TinyJPEGDecoder decoder;
  uint16_t dw, dh;
  assert(decoder.getJpgSize(&dw, &dh, arrayBuf.data(), arrayBuf.size()) == JDR_OK);
  assert(dw == w && dh == h);

  // Array sink too small: must fail with JER_INTR, not silently truncate-and-succeed.
  TinyJPEGEncoder tooSmall;
  uint8_t tinyBuf[8];
  size_t tinySize = 0;
  JERESULT r4 = tooSmall.encodeJpg(rgb.data(), w, h, JE_FMT_RGB888, tinyBuf, sizeof(tinyBuf), tinySize);
  assert(r4 == JER_INTR);

  // setUserData()/getUserData() round-trip (mirrors TinyJPEGDecoder's own).
  int marker = 42;
  arrayEncoder.setUserData(&marker);
  assert(arrayEncoder.getUserData() == &marker);

  printf("test_encoder_wrapper: OK (encoded %zu bytes)\n", arraySize);
  return 0;
}
