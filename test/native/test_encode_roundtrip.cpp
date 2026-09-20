// Round-trip correctness gate for the encoder (tjpge.h): encode a synthetic
// RGB888 gradient with je_prepare()/je_encode(), then decode the result back
// with this repo's own decoder (tjpgd.h, via TinyJPEGDecoder) and diff
// against the original source pixels. This is the main correctness check
// for the encoder described in docs/architecture.md - it validates against
// the decoder already proven correct against libjpeg by the
// test_decode_*.cpp tests, so a passing round-trip means the encoder
// produces a spec-legal bitstream a conformant decoder can read back
// faithfully, not just "a decoder we also wrote agrees with itself".
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
#include "TinyJPEGDecoder.h"
#include "tjpge.h"
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
      px[i + 2] = (uint8_t)(((x + y) * 255 / (w + h - 2)));
    }
  }
  return px;
}

// Encodes `rgb` (w*h*3 bytes) at the given quality/subsampling into `jpg`.
// Returns the JERESULT from je_encode().
JERESULT encodeToVector(const std::vector<uint8_t>& rgb, int w, int h, uint8_t quality, uint8_t subsample,
                         std::vector<uint8_t>& jpg) {
  struct Ctx {
    std::vector<uint8_t>* out;
  } ctx{&jpg};

  auto outfunc = [](JENC* je, const uint8_t* data, size_t n) -> size_t {
    auto* c = static_cast<Ctx*>(je->device);
    c->out->insert(c->out->end(), data, data + n);
    return n;
  };

  static uint8_t pool[TJPGE_WORKSPACE_SIZE];
  JENC je{};
  JERESULT rc = je_prepare(&je, outfunc, pool, sizeof(pool), &ctx, (uint16_t)w, (uint16_t)h, JE_FMT_RGB888);
  if (rc != JER_OK) return rc;
  je.quality = quality;
  je.subsample = subsample;
  return je_encode(&je, rgb.data());
}

double meanAbsDiff(const std::vector<uint8_t>& rgbRef, const PixelSink& sink, int& maxDiff) {
  double sumAbs = 0;
  maxDiff = 0;
  size_t n = (size_t)sink.width * sink.height;
  for (size_t i = 0; i < n; i++) {
    uint8_t r8, g8, b8;
    rgb565ToRgb888(sink.pixels[i], r8, g8, b8);
    int rr = rgbRef[i * 3 + 0], gg = rgbRef[i * 3 + 1], bb = rgbRef[i * 3 + 2];
    int d = std::abs(r8 - rr) + std::abs(g8 - gg) + std::abs(b8 - bb);
    sumAbs += d;
    if (d > maxDiff) maxDiff = d;
  }
  return sumAbs / (double)n;
}

}  // namespace

int main() {
  const int w = 64, h = 48;
  auto rgb = makeGradientRgb(w, h);

  // 4:4:4, high quality: tightest tolerance, since neither subsampling nor
  // heavy quantization should be contributing error here.
  {
    std::vector<uint8_t> jpg;
    JERESULT erc = encodeToVector(rgb, w, h, /*quality=*/90, /*subsample=*/0, jpg);
    assert(erc == JER_OK);
    assert(jpg.size() > 4);
    assert(jpg[0] == 0xFF && jpg[1] == 0xD8);              // SOI
    assert(jpg[jpg.size() - 2] == 0xFF && jpg[jpg.size() - 1] == 0xD9);  // EOI

    TinyJPEGDecoder decoder;
    uint16_t dw, dh;
    assert(decoder.getJpgSize(&dw, &dh, jpg.data(), jpg.size()) == JDR_OK);
    assert(dw == w && dh == h);

    PixelSink sink;
    sink.width = w;
    sink.height = h;
    sink.pixels.assign((size_t)w * h, 0);
    decoder.setUserData(&sink);
    decoder.setCallback(pixelSinkCallback);
    JRESULT drc = decoder.drawJpg(0, 0, jpg.data(), jpg.size());
    assert(drc == JDR_OK);

    int maxDiff;
    double meanDiff = meanAbsDiff(rgb, sink, maxDiff);
    printf("test_encode_roundtrip: 444 q90 mean=%.3f max=%d bytes=%zu\n", meanDiff, maxDiff, jpg.size());
    // Measured (not guessed) on this exact synthetic gradient: mean~11.0,
    // max~25 - dominated by RGB565's own 5/6/5-bit quantization (~5 of
    // this budget) plus independent forward/inverse DCT and color-
    // conversion rounding between tjpge and tjpgd, same policy as the
    // decode tests' own tolerance comments.
    assert(meanDiff <= 14.0);
    assert(maxDiff <= 35);
  }

  // 4:2:0, default quality: chroma subsampling adds more error, mostly on
  // the diagonal Cr gradient's sharp edges - wider but still bounded budget.
  {
    std::vector<uint8_t> jpg;
    JERESULT erc = encodeToVector(rgb, w, h, /*quality=*/80, /*subsample=*/1, jpg);
    assert(erc == JER_OK);

    TinyJPEGDecoder decoder;
    uint16_t dw, dh;
    assert(decoder.getJpgSize(&dw, &dh, jpg.data(), jpg.size()) == JDR_OK);
    assert(dw == w && dh == h);

    PixelSink sink;
    sink.width = w;
    sink.height = h;
    sink.pixels.assign((size_t)w * h, 0);
    decoder.setUserData(&sink);
    decoder.setCallback(pixelSinkCallback);
    JRESULT drc = decoder.drawJpg(0, 0, jpg.data(), jpg.size());
    assert(drc == JDR_OK);

    int maxDiff;
    double meanDiff = meanAbsDiff(rgb, sink, maxDiff);
    printf("test_encode_roundtrip: 420 q80 mean=%.3f max=%d bytes=%zu\n", meanDiff, maxDiff, jpg.size());
    // Measured: mean~11.3, max~30 - chroma subsampling adds a bit more
    // error than the 444/q90 case above, mostly on the diagonal Cr
    // gradient's sharp edges.
    assert(meanDiff <= 15.0);
    assert(maxDiff <= 40);
  }

  // Grayscale input, 4:4:4 forced regardless of je.subsample (ncomp==1).
  {
    std::vector<uint8_t> gray((size_t)w * h);
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++) gray[(size_t)y * w + x] = (uint8_t)((x * 255 / (w - 1) + y * 255 / (h - 1)) / 2);

    std::vector<uint8_t> jpg;
    struct Ctx {
      std::vector<uint8_t>* out;
    } ctx{&jpg};
    auto outfunc = [](JENC* je, const uint8_t* data, size_t n) -> size_t {
      auto* c = static_cast<Ctx*>(je->device);
      c->out->insert(c->out->end(), data, data + n);
      return n;
    };
    static uint8_t pool[TJPGE_WORKSPACE_SIZE];
    JENC je{};
    JERESULT rc = je_prepare(&je, outfunc, pool, sizeof(pool), &ctx, (uint16_t)w, (uint16_t)h, JE_FMT_GRAY8);
    assert(rc == JER_OK);
    rc = je_encode(&je, gray.data());
    assert(rc == JER_OK);
    assert(jpg.size() > 4);

    TinyJPEGDecoder decoder;
    uint16_t dw, dh;
    assert(decoder.getJpgSize(&dw, &dh, jpg.data(), jpg.size()) == JDR_OK);
    assert(dw == w && dh == h);

    PixelSink sink;
    sink.width = w;
    sink.height = h;
    sink.pixels.assign((size_t)w * h, 0);
    decoder.setUserData(&sink);
    decoder.setCallback(pixelSinkCallback);
    JRESULT drc = decoder.drawJpg(0, 0, jpg.data(), jpg.size());
    assert(drc == JDR_OK);

    double sumAbs = 0;
    int maxDiff = 0;
    for (size_t i = 0; i < (size_t)w * h; i++) {
      uint8_t r8, g8, b8;
      rgb565ToRgb888(sink.pixels[i], r8, g8, b8);
      int d = std::abs((int)r8 - (int)gray[i]);  // R==G==B for a decoded grayscale JPEG
      sumAbs += d;
      if (d > maxDiff) maxDiff = d;
    }
    double meanDiff = sumAbs / (double)((size_t)w * h);
    printf("test_encode_roundtrip: gray mean=%.3f max=%d bytes=%zu\n", meanDiff, maxDiff, jpg.size());
    // Measured: mean~4.1, max~9 - grayscale has no chroma/YCbCr rounding
    // to contribute, so this is the tightest of the three budgets.
    assert(meanDiff <= 6.0);
    assert(maxDiff <= 15);
  }

  printf("test_encode_roundtrip: OK\n");
  return 0;
}
