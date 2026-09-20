// Correctness gate for TinyJPEGEncoder's streaming row API
// (beginEncode()/writeRows()/finishEncode() - TinyJPEGEncoder.h), the
// wrapper-level counterpart to test_encode_streaming.cpp's tjpge.h-level
// coverage. Proves: (1) streamed-in-chunks output is byte-identical to
// encodeJpg()'s full-buffer output on the same TinyJPEGEncoder instance
// (both share workspace_, so this also exercises that they don't corrupt
// each other's state when used sequentially on one instance), (2) the
// callback-sink beginEncode() overload works the same way, and (3)
// bytesWritten() reports the right final size for the array sink.
#include <cassert>
#include <cstdio>
#include <vector>
#include "TinyJPEGEncoder.h"

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

}  // namespace

int main() {
  const int w = 70, h = 45;  // neither dimension a multiple of 8 or 16
  auto rgb = makeGradientRgb(w, h);

  TinyJPEGEncoder encoder;
  encoder.setQuality(85);
  encoder.setSubsample(true);

  // Full-buffer reference, via encodeJpg() on the same instance.
  std::vector<uint8_t> fullBuf(16384);
  size_t fullSize = 0;
  assert(encoder.encodeJpg(rgb.data(), (uint16_t)w, (uint16_t)h, JE_FMT_RGB888, fullBuf.data(), fullBuf.size(),
                            fullSize) == JER_OK);
  fullBuf.resize(fullSize);

  // Streamed, 3 rows at a time (doesn't divide 8 or 16), on the SAME
  // instance right after the encodeJpg() call above - proves the two
  // APIs sharing workspace_ don't leave stale state behind.
  std::vector<uint8_t> streamBuf(16384);
  assert(encoder.beginEncode((uint16_t)w, (uint16_t)h, JE_FMT_RGB888, streamBuf.data(), streamBuf.size()) == JER_OK);
  int rowStride = w * 3;
  for (int y = 0; y < h;) {
    int n = (y + 3 <= h) ? 3 : (h - y);
    assert(encoder.writeRows(rgb.data() + (size_t)y * rowStride, (uint16_t)n) == JER_OK);
    y += n;
  }
  assert(encoder.finishEncode() == JER_OK);
  size_t streamSize = encoder.bytesWritten();
  streamBuf.resize(streamSize);

  assert(streamBuf == fullBuf);
  printf("test_encoder_wrapper_streaming: array sink byte-identical (%zu bytes)\n", streamSize);

  // Callback sink, same content.
  std::vector<uint8_t> cbBuf;
  auto writeCb = [](void *userData, const uint8_t *data, size_t len) -> size_t {
    auto *out = static_cast<std::vector<uint8_t> *>(userData);
    out->insert(out->end(), data, data + len);
    return len;
  };
  assert(encoder.beginEncode((uint16_t)w, (uint16_t)h, JE_FMT_RGB888, writeCb, &cbBuf) == JER_OK);
  for (int y = 0; y < h; y++) {
    assert(encoder.writeRows(rgb.data() + (size_t)y * rowStride, 1) == JER_OK);
  }
  assert(encoder.finishEncode() == JER_OK);
  assert(cbBuf == fullBuf);
  printf("test_encoder_wrapper_streaming: callback sink byte-identical (%zu bytes)\n", cbBuf.size());

  printf("test_encoder_wrapper_streaming: OK\n");
  return 0;
}
