// The plain-function-pointer drawJpg(x, y, InputCallback, void*) overload
// (TinyJPEGDecoder.h) is the lowest-level input path - useful for sources
// that don't look like a file at all (a network socket, a ring buffer,
// ...). Note the contract this callback must honor (documented on
// TinyJPEGDecoder::InputCallback, inherited unchanged from tjpgd's own
// infunc): it must always return exactly `min(len, bytes actually
// available)` - never claim to have fulfilled more than it copied, but
// also never deliberately shortchange a request when more data *is*
// available. jd_prepare() relies on this for JPEG header segments
// (SOF0/DHT/DQT/DRI/SOS - see tjpgd.h): a call that returns less than the
// full `len` it asked for is treated as a hard stream error (JDR_INP),
// the same as a genuinely truncated file, even if more bytes actually
// remained. This is a "block until satisfied" contract (as a synchronous
// file read naturally is), not a general partial-read-tolerant streaming
// interface - a caller reading from a source prone to short reads (a raw
// non-blocking socket) needs to do its own buffering/retrying beneath
// this callback to satisfy it.
//
// This test's CountingReader always honors that contract (it's a plain
// memcpy from an in-memory buffer, so it always can); what it actually
// exercises is userData threading, the null-buffer "skip" contract (tjpgd
// uses that to discard segments it doesn't need, e.g. EXIF/comment data),
// and that decoding through many separate small calls - rather than one
// big buffer handed over up front - reaches the exact same result as the
// plain array path.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include "TinyJPEGDecoder.h"
#include "test_helpers.h"

using namespace tinyjpeg;

struct CountingReader {
  const uint8_t *data;
  size_t size;
  size_t pos = 0;
  int callCount = 0;
};

static size_t countingRead(void *userData, uint8_t *buf, size_t len) {
  auto *r = static_cast<CountingReader *>(userData);
  r->callCount++;
  size_t avail = r->size - r->pos;
  size_t n = std::min(len, avail);
  if (buf && n) memcpy(buf, r->data + r->pos, n);
  // A null buf means "skip", same contract as TJpg_Decoder's original
  // stream input function - advance regardless of whether data was copied.
  r->pos += n;
  return n;
}

int main() {
  auto jpg = readFile("assets/gradient_444.jpg");

  CountingReader sizeReader{jpg.data(), jpg.size()};
  TinyJPEGDecoder decoder;
  uint16_t w, h;
  assert(decoder.getJpgSize(&w, &h, &countingRead, &sizeReader) == JDR_OK);
  assert(w == 64 && h == 48);
  assert(sizeReader.callCount > 1);  // must issue several reads, not one big slurp

  // Must decode to the exact same pixels as the plain array overload.
  PixelSink arraySink;
  arraySink.width = w;
  arraySink.height = h;
  arraySink.pixels.assign((size_t)w * h, 0);
  TinyJPEGDecoder arrayDecoder;
  arrayDecoder.setUserData(&arraySink);
  arrayDecoder.setCallback(pixelSinkCallback);
  assert(arrayDecoder.drawJpg(0, 0, jpg.data(), jpg.size()) == JDR_OK);

  PixelSink cbSink;
  cbSink.width = w;
  cbSink.height = h;
  cbSink.pixels.assign((size_t)w * h, 0);
  CountingReader drawReader{jpg.data(), jpg.size()};
  TinyJPEGDecoder cbDecoder;
  cbDecoder.setUserData(&cbSink);
  cbDecoder.setCallback(pixelSinkCallback);
  assert(cbDecoder.drawJpg(0, 0, &countingRead, &drawReader) == JDR_OK);
  assert(drawReader.callCount > 1);

  assert(arraySink.pixels == cbSink.pixels);

  printf("test_callback_source: OK\n");
  return 0;
}
