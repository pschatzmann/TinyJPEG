#pragma once
#include "tjpgd.h"
#include <cstdint>
#include <cstddef>
#include <cstring>

/*
 * TinyJPEGDecoder: a minimal, header-only baseline JPEG decoder for
 * microcontrollers, built on ChaN's TJpgDec engine (tjpgd.h - see that
 * file for the ported decompressor itself and docs/architecture.md for
 * the full conversion rationale). This is the public-facing API, in the
 * `tinyjpeg` namespace (mirrors TinyMPG's `tinympg` and TinyH264's
 * `tinyh264` namespaces - same author, same house style).
 *
 * This class is the header-only, platform-independent replacement for
 * Bodmer's TJpg_Decoder::TJpg_Decoder class - see docs/architecture.md's
 * "API changes from TJpg_Decoder" section for the exact mapping. The
 * changes from the original that most affect a migrating sketch:
 *
 *  1. No SD/SPIFFS/LittleFS-specific methods (drawSdJpg/drawFsJpg/etc.) -
 *     instead, drawJpg()/getJpgSize() take any object exposing
 *     available()/read(buf,len)/position()/seek(pos) (Arduino's fs::File
 *     and SD's File both do already), so the exact same template code
 *     path serves SD, LittleFS, SPIFFS, or a hand-written mock in a
 *     native unit test, with no Arduino.h dependency in this file at all.
 *     See docs/decoding.md for the exact FileT contract and a from-file
 *     usage example.
 *  2. No global singleton (the original's `extern TJpg_Decoder TJpgDec`
 *     plus a `thisPtr` self-pointer trick so its two static-by-necessity
 *     C callback trampolines could reach instance state) - this class
 *     instead threads a small stack-local, polymorphic context object
 *     through JDEC's own `device` field (a `void*` TJpgDec always carried
 *     for exactly this purpose, just unused by the original wrapper), so
 *     multiple TinyJPEGDecoder instances can safely coexist and this
 *     class has no mutable global state.
 *  3. SketchCallback's first parameter is a reference to the
 *     TinyJPEGDecoder instance drawJpg() was called on (TJpg_Decoder's
 *     own callback shape has no such parameter - it didn't need one,
 *     since it could only ever be called on the one global TJpgDec
 *     instance). Combined with setUserData()/getUserData() (an arbitrary
 *     caller-owned pointer carried on the instance, untouched by this
 *     class otherwise), a callback can reach whatever per-decode context
 *     it needs - a display object, an output buffer - through the
 *     instance itself instead of a global/static variable.
 *
 * Usage (array/PROGMEM source):
 *   using namespace tinyjpeg;
 *   bool onBlock(TinyJPEGDecoder& decoder, int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
 *     TFT_eSPI* tft = static_cast<TFT_eSPI*>(decoder.getUserData());
 *     tft->pushImage(x, y, w, h, bitmap);  // or however your display draws a block
 *     return true;  // false aborts the decode early
 *   }
 *   TinyJPEGDecoder decoder;
 *   decoder.setUserData(&tft);
 *   decoder.setCallback(onBlock);
 *   JRESULT r = decoder.drawJpg(0, 0, jpegData, jpegSize);
 *   if (r != JDR_OK) { ... }  // see JRESULT in tjpgd.h for the error codes
 *
 * Usage (file source - SD, LittleFS, SPIFFS, or any FileT with
 * available()/read()/position()/seek()):
 *   File f = SD.open("/image.jpg", FILE_READ);
 *   JRESULT r = decoder.drawJpg(0, 0, f);
 *   f.close();
 *
 * See tjpgd_config.h for JD_FORMAT (pixel format - RGB565 by default,
 * matching the uint16_t* the callback above receives; switching it to
 * RGB888 or grayscale changes the bitmap's actual byte layout, so
 * reinterpret the callback's pointer accordingly if you do), JD_SZBUF
 * (stream input buffer size) and JD_FASTDECODE (speed/workspace-size
 * tradeoff) - all overridable with a `-D` or a `#define` before the first
 * `#include` of this library anywhere in the build, the standard
 * header-only override idiom.
 */

namespace tinyjpeg {

class TinyJPEGDecoder;

/**
 * Per-decoded-block callback, invoked once per MCU (minimum coded unit,
 * up to 16x16 pixels) as the image streams out of the decoder. `decoder`
 * is the same instance drawJpg() was called on - dereference
 * `decoder.getUserData()` to reach whatever context setUserData() was
 * given (a display object, an output buffer, ...) instead of relying on a
 * global/static variable, the same reason InputCallback's `userData`
 * parameter exists on the input side. (x, y) is the block's top-left
 * corner in the destination image (already offset by the x/y passed to
 * drawJpg()); `data` points to `w * h` pixels in JD_FORMAT's configured
 * layout (uint16_t RGB565 by default). Return false to abort the decode
 * early (drawJpg() then returns JDR_INTR); true to continue.
 *
 * This is TJpg_Decoder's own SketchCallback shape plus the leading
 * `decoder` parameter - the one signature change needed to drop that
 * library's `thisPtr`/global-singleton workaround (see
 * docs/architecture.md's "No more singleton" section) without losing the
 * ability to reach per-decode context from inside the callback.
 */
using SketchCallback = bool (*)(TinyJPEGDecoder &decoder, int16_t x, int16_t y, uint16_t w, uint16_t h,
                                 uint16_t *data);

/**
 * A minimal, header-only baseline (non-progressive) JPEG decoder for
 * constrained devices - decodes directly into caller-supplied storage one
 * MCU block at a time via a callback (see SketchCallback above), rather
 * than requiring memory for a whole decoded frame at once, the same
 * memory strategy TJpg_Decoder itself uses. Workspace (the Huffman/
 * quantizer tables, MCU buffer, and IDCT scratch space TJpgDec needs) is
 * one fixed-size member array (TJPGD_WORKSPACE_SIZE bytes, ~3.5KB with
 * the default JD_FASTDECODE=1 - see tjpgd_config.h), so a TinyJPEGDecoder
 * instance never allocates on the heap.
 */
class TinyJPEGDecoder {
 public:
  TinyJPEGDecoder() = default;

  /// Registers the function invoked once per decoded MCU block - see SketchCallback.
  void setCallback(SketchCallback sketchCallback) { callback_ = sketchCallback; }

  /**
   * Sets the output reduction factor: 1 (full size), 2, 4, or 8 - any
   * other value is treated as 1. Applies to every drawJpg()/getJpgSize()
   * call until changed again.
   */
  void setJpgScale(uint8_t scaleFactor) {
    switch (scaleFactor) {
      case 2: scale_ = 1; break;
      case 4: scale_ = 2; break;
      case 8: scale_ = 3; break;
      default: scale_ = 0; break;
    }
  }

  /// Swaps the high/low bytes of each RGB565 output pixel - some displays
  /// expect big-endian pixel words; most TFT SPI displays do.
  void setSwapBytes(bool swap) { swapBytes_ = swap; }

  /**
   * Stashes an arbitrary caller-owned pointer on this instance - a display
   * object, an output pixel buffer, a struct bundling both, whatever the
   * block callback (SketchCallback) needs but isn't itself passed. Not
   * touched or interpreted by TinyJPEGDecoder in any way; retrieve it with
   * getUserData() from inside the callback via its `decoder` parameter.
   * Unset (nullptr) by default.
   */
  void setUserData(void *userData) { userData_ = userData; }

  /// Returns whatever setUserData() last set (nullptr if never called) - see setUserData().
  void *getUserData() const { return userData_; }

  /**
   * Decodes a JPEG held entirely in memory (RAM, or flash/PROGMEM already
   * copied to RAM - this decoder reads via plain pointer + memcpy, so on
   * AVR in particular `data` must not point directly into PROGMEM).
   * Invokes the registered callback once per decoded block, offsetting
   * every block by (x, y). Returns JDR_OK on success, JDR_INTR if the
   * callback returned false, or another JRESULT (tjpgd.h) on a bitstream
   * or memory error.
   */
  JRESULT drawJpg(int32_t x, int32_t y, const uint8_t *data, size_t size) {
    ArrayContext ctx(data, size);
    ctx.offsetX = x;
    ctx.offsetY = y;
    ctx.callback = callback_;
    return runDecode(ctx);
  }

  /// Reads just the width/height of an in-memory JPEG without decoding any pixel data.
  JRESULT getJpgSize(uint16_t *w, uint16_t *h, const uint8_t *data, size_t size) {
    ArrayContext ctx(data, size);
    return runGetSize(ctx, w, h);
  }

  /**
   * Decodes a JPEG read from `file` - any object providing
   * `size_t available()`, `size_t read(uint8_t*, size_t)`, `size_t
   * position()` and `bool seek(size_t)` (Arduino's fs::File and SD's File
   * both already satisfy this; see docs/decoding.md for a from-scratch
   * FileT for e.g. plain stdio FILE* on a native build). `file` is read
   * from its current position and is left positioned wherever the decode
   * happened to stop - callers that need it closed/rewound do that
   * themselves. See drawJpg(x, y, data, size) for the callback/offset
   * semantics.
   */
  template <typename FileT>
  JRESULT drawJpg(int32_t x, int32_t y, FileT &file) {
    FileContext<FileT> ctx(file);
    ctx.offsetX = x;
    ctx.offsetY = y;
    ctx.callback = callback_;
    return runDecode(ctx);
  }

  /// Reads just the width/height of a JPEG file without decoding any pixel data - see drawJpg(x, y, FileT&).
  template <typename FileT>
  JRESULT getJpgSize(uint16_t *w, uint16_t *h, FileT &file) {
    FileContext<FileT> ctx(file);
    return runGetSize(ctx, w, h);
  }

  /**
   * Decodes a JPEG streamed through a plain input callback instead of a
   * fixed buffer or a file-like object - useful for network sockets or
   * any other source that doesn't naturally look like a file. `read`
   * receives `userData` unchanged; it must copy up to `len` bytes into
   * `buf` and return the number of bytes actually placed there (0 means
   * end of stream/error), or, if `buf` is nullptr, skip `len` bytes of
   * input and return `len` (tjpgd uses a null buffer to discard segments
   * it doesn't need, e.g. EXIF/comment data).
   */
  using InputCallback = size_t (*)(void *userData, uint8_t *buf, size_t len);

  JRESULT drawJpg(int32_t x, int32_t y, InputCallback read, void *userData) {
    CallbackContext ctx(read, userData);
    ctx.offsetX = x;
    ctx.offsetY = y;
    ctx.callback = callback_;
    return runDecode(ctx);
  }

  /// Reads just the width/height of a streamed JPEG without decoding any pixel data - see drawJpg(x, y, InputCallback, void*).
  JRESULT getJpgSize(uint16_t *w, uint16_t *h, InputCallback read, void *userData) {
    CallbackContext ctx(read, userData);
    return runGetSize(ctx, w, h);
  }

 private:
  /*
   * Common base every input source implements - jd->device (see tjpgd.h,
   * "Pointer to I/O device identifiler for the session") always points at
   * one of these while a decode is in progress, so jdInputTrampoline()/
   * jdOutputTrampoline() below can reach both the source-specific read()
   * override and the source-independent offset/callback state through a
   * single polymorphic pointer, without needing to know which concrete
   * context type is actually in play. `read()` is virtual for exactly
   * that reason - it's called roughly once per JD_SZBUF (512) bytes of
   * compressed input, nowhere near a per-pixel hot path, so the indirect
   * call has no measurable cost here.
   */
  struct Context {
    virtual ~Context() = default;
    virtual size_t read(uint8_t *buf, size_t len) = 0;
    int32_t offsetX = 0;
    int32_t offsetY = 0;
    SketchCallback callback = nullptr;
    // Set by prepare() (always, for every context/overload) so
    // jdOutputTrampoline() can pass the owning instance to `callback` -
    // see SketchCallback's own doc comment above.
    TinyJPEGDecoder *owner = nullptr;
  };

  struct ArrayContext : Context {
    ArrayContext(const uint8_t *d, size_t n) : data(d), size(n) {}
    size_t read(uint8_t *buf, size_t len) override {
      if (index + len > size) len = size - index;
      if (buf && len) memcpy(buf, data + index, len);
      index += len;
      return len;
    }
    const uint8_t *data;
    size_t size;
    size_t index = 0;
  };

  struct CallbackContext : Context {
    CallbackContext(InputCallback r, void *u) : reader(r), userData(u) {}
    size_t read(uint8_t *buf, size_t len) override { return reader(userData, buf, len); }
    InputCallback reader;
    void *userData;
  };

  template <typename FileT>
  struct FileContext : Context {
    explicit FileContext(FileT &f) : file(f) {}
    size_t read(uint8_t *buf, size_t len) override {
      size_t avail = (size_t)file.available();
      if (len > avail) len = avail;
      if (buf) return file.read(buf, len);
      file.seek(file.position() + len);  // null buf: tjpgd wants these bytes skipped, not delivered
      return len;
    }
    FileT &file;
  };

  static size_t jdInputTrampoline(JDEC *jd, uint8_t *buf, size_t len) {
    return static_cast<Context *>(jd->device)->read(buf, len);
  }

  static int jdOutputTrampoline(JDEC *jd, void *bitmap, JRECT *rect) {
    auto *ctx = static_cast<Context *>(jd->device);
    int16_t x = (int16_t)(rect->left + ctx->offsetX);
    int16_t y = (int16_t)(rect->top + ctx->offsetY);
    uint16_t w = (uint16_t)(rect->right - rect->left + 1);
    uint16_t h = (uint16_t)(rect->bottom - rect->top + 1);
    if (!ctx->callback) return 1;  // no callback registered: keep decoding, just drop the blocks
    return ctx->callback(*ctx->owner, x, y, w, h, static_cast<uint16_t *>(bitmap)) ? 1 : 0;
  }

  // `ctx` is passed as `Context&` (not the concrete ArrayContext/FileContext<FileT>/
  // CallbackContext) so jd->device is built from a Context* to begin with -
  // jdInputTrampoline()/jdOutputTrampoline() then only ever round-trip a
  // `Context*` through jd->device's `void*`, never a derived pointer, which
  // keeps the void*<->pointer conversions on the well-defined side of
  // [expr.static.cast] instead of relying on "every real ABI places a
  // non-virtual first base at its derived object's own address" (true for
  // every context type here, but not something the standard promises).
  JRESULT prepare(JDEC &jd, Context &ctx) {
    jd.swap = swapBytes_ ? 1 : 0;
    ctx.owner = this;
    return jd_prepare(&jd, &jdInputTrampoline, workspace_, sizeof(workspace_), &ctx);
  }

  JRESULT runDecode(Context &ctx) {
    JDEC jd;
    JRESULT r = prepare(jd, ctx);
    if (r != JDR_OK) return r;
    return jd_decomp(&jd, &jdOutputTrampoline, scale_);
  }

  JRESULT runGetSize(Context &ctx, uint16_t *w, uint16_t *h) {
    *w = 0;
    *h = 0;
    JDEC jd;
    JRESULT r = prepare(jd, ctx);
    if (r == JDR_OK) {
      *w = jd.width;
      *h = jd.height;
    }
    return r;
  }

  SketchCallback callback_ = nullptr;
  void *userData_ = nullptr;
  uint8_t scale_ = 0;
  bool swapBytes_ = false;

  // Must align workspace to a 32 bit boundary, matching TJpg_Decoder's own
  // member (some platforms fault on unaligned 32-bit access from tjpgd's
  // internal tables).
  alignas(4) uint8_t workspace_[TJPGD_WORKSPACE_SIZE];
};

}  // namespace tinyjpeg
