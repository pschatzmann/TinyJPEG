#pragma once
#include "tjpge.h"
#include <cstdint>
#include <cstddef>
#include <cstring>

/*
 * TinyJPEGEncoder: a minimal, header-only baseline JPEG encoder for
 * microcontrollers, built on this project's own tjpge.h engine (see that
 * file for the encoder internals and docs/architecture.md for how it
 * relates to tjpgd.h, the decoder). This is the public-facing API, in the
 * `tinyjpeg` namespace alongside TinyJPEGDecoder (same house style as
 * that class - see its own file comment for the sibling-project
 * conventions this follows).
 *
 * Two ways to feed it pixels, in whichever JEPixelFormat you pass
 * (JE_FMT_RGB888, JE_FMT_RGB565, JE_FMT_RGB666, or JE_FMT_GRAY8 - see
 * that enum's own comment in tjpge.h for the exact byte layout of each):
 *
 *  - encodeJpg(): one full frame of pixels up front. Simplest, and fine
 *    when you already have (or don't mind holding) the whole frame in
 *    RAM - see the first two usage examples below.
 *  - beginEncode()/writeRows()/finishEncode(): rows fed incrementally (1
 *    row at a time, the whole image at once, or anything between), for a
 *    source that produces rows over time and can't or shouldn't be
 *    buffered whole first - a camera driver, a display back buffer read
 *    line-by-line. Peak pixel memory is one MCU row band (8 source rows,
 *    or 16 for 4:2:0 color) regardless of image height - see tjpge.h's
 *    own file comment (je_start()/je_write_rows()/je_finish(), which
 *    this is a thin wrapper over) and the third usage example below.
 *
 * Either way, the *compressed output* is always streamed the same way
 * the decoder streams its compressed input: handed to the caller
 * incrementally (JE_SZBUF, 512 bytes by default, at a time) via a fixed
 * array, a file-like object, or a callback, never buffered whole here.
 *
 * Usage (encode to a fixed byte array):
 *   using namespace tinyjpeg;
 *   TinyJPEGEncoder encoder;
 *   encoder.setQuality(80);
 *   uint8_t jpgBuf[8192];
 *   size_t jpgSize = 0;
 *   JERESULT r = encoder.encodeJpg(rgbPixels, width, height, JE_FMT_RGB888, jpgBuf, sizeof(jpgBuf), jpgSize);
 *   if (r != JER_OK) { ... }  // see JERESULT in tjpge.h for the error codes
 *
 * Usage (encode to a file - SD, LittleFS, SPIFFS, or any FileT with
 * write(const uint8_t*, size_t)) - straight from a typical TFT/camera
 * RGB565 back buffer, no manual conversion needed:
 *   File f = SD.open("/photo.jpg", FILE_WRITE);
 *   JERESULT r = encoder.encodeJpg(rgb565Pixels, width, height, JE_FMT_RGB565, f);
 *   f.close();
 *
 * Usage (streaming rows, output to a fixed array - a file/callback sink
 * works the same way, just with the matching beginEncode() overload):
 *   JERESULT r = encoder.beginEncode(width, height, JE_FMT_RGB888, jpgBuf, sizeof(jpgBuf));
 *   for (each row (or batch of rows) your source produces) {
 *     r = encoder.writeRows(rowPixels, numRows);
 *     if (r != JER_OK) break;
 *   }
 *   r = encoder.finishEncode();
 *   size_t jpgSize = encoder.bytesWritten();  // array sink only
 *
 * See tjpge_config.h for JE_QUALITY/JE_SUBSAMPLE (overridable per-call via
 * setQuality()/setSubsample() too), JE_SZBUF (output stream buffer
 * size), and JE_MAX_WIDTH (widest image this class's fixed workspace can
 * handle when streaming - see that macro's own comment) - all overridable
 * with a `-D` or a `#define` before the first `#include` of this library
 * anywhere in the build, the standard header-only override idiom.
 */

namespace tinyjpeg {

/**
 * A minimal, header-only baseline (non-progressive) JPEG encoder for
 * constrained devices. Workspace (quantization tables, Huffman encode
 * lookup tables, the output stream buffer, and - for the streaming row
 * API - a row-band buffer sized for images up to JE_MAX_WIDTH pixels
 * wide) is one fixed-size member array (TJPGE_WORKSPACE_SIZE bytes,
 * ~17.6KB with the default JE_SZBUF/JE_MAX_WIDTH - see tjpge_config.h),
 * so a TinyJPEGEncoder instance never allocates on the heap.
 */
class TinyJPEGEncoder {
 public:
  TinyJPEGEncoder() = default;

  /// Sets JPEG quality, 1 (smallest/lowest quality) to 100 (largest/highest quality). Defaults to JE_QUALITY.
  void setQuality(uint8_t quality) { quality_ = quality; }

  /// Enables (true) or disables (false) 4:2:0 chroma subsampling for color input - ignored for grayscale. Defaults to JE_SUBSAMPLE.
  void setSubsample(bool enable) { subsample_ = enable; }

  /**
   * Stashes an arbitrary caller-owned pointer on this instance - mirrors
   * TinyJPEGDecoder::setUserData()/getUserData(), though nothing in this
   * class reads it back itself (there is no per-block callback here to
   * hand it to); it's provided purely as a convenient place for a caller
   * to keep context alongside the encoder instance. Unset (nullptr) by default.
   */
  void setUserData(void *userData) { userData_ = userData; }

  /// Returns whatever setUserData() last set (nullptr if never called) - see setUserData().
  void *getUserData() const { return userData_; }

  /**
   * Encodes `pixels` (row-major, laid out per `format` - see
   * JEPixelFormat in tjpge.h for the exact byte layout of JE_FMT_RGB888/
   * JE_FMT_RGB565/JE_FMT_RGB666/JE_FMT_GRAY8) into `outBuf`, up to `outCapacity` bytes.
   * `outSize` receives the number of bytes actually written on success.
   * Returns JER_INTR (not JER_MEM1 - this isn't the workspace pool) if
   * the encoded JPEG doesn't fit in `outCapacity`; `outSize` will then
   * hold a truncated, unusable prefix, not a partial-but-valid JPEG.
   */
  JERESULT encodeJpg(const uint8_t *pixels, uint16_t width, uint16_t height, JEPixelFormat format, uint8_t *outBuf,
                      size_t outCapacity, size_t &outSize) {
    ArrayContext ctx(outBuf, outCapacity);
    JERESULT r = runEncode(ctx, pixels, width, height, format);
    outSize = ctx.written;
    return r;
  }

  /**
   * Encodes `pixels` to `file` - any object providing `size_t write(const
   * uint8_t*, size_t)` (Arduino's fs::File and SD's File both already
   * satisfy this). `file` is written from its current position and left
   * positioned wherever the encode happened to stop; callers that need it
   * closed do that themselves. See the array overload above for the
   * pixel buffer layout.
   */
  template <typename FileT>
  JERESULT encodeJpg(const uint8_t *pixels, uint16_t width, uint16_t height, JEPixelFormat format, FileT &file) {
    FileContext<FileT> ctx(file);
    return runEncode(ctx, pixels, width, height, format);
  }

  /**
   * Encodes `pixels`, streaming compressed output through a plain output
   * callback instead of a fixed buffer or a file-like object - useful for
   * network sockets or any other destination that doesn't naturally look
   * like a file. `write` receives `userData` unchanged, plus up to
   * JE_SZBUF bytes at a time; it must return the number of bytes actually
   * accepted (anything less than `len` aborts the encode with JER_INTR,
   * the same short-write contract tjpge.h's own JENC::outfunc has).
   */
  using OutputCallback = size_t (*)(void *userData, const uint8_t *buf, size_t len);

  JERESULT encodeJpg(const uint8_t *pixels, uint16_t width, uint16_t height, JEPixelFormat format,
                      OutputCallback write, void *userData) {
    CallbackContext ctx(write, userData);
    return runEncode(ctx, pixels, width, height, format);
  }

  /**
   * Streaming row API: an alternative to encodeJpg() for callers who
   * don't have (or don't want to hold) a whole frame in memory at once -
   * a camera driver or display back buffer producing rows incrementally,
   * for instance. beginEncode() writes the JPEG headers and readies the
   * encoder; writeRows() feeds any number of rows in any number of calls
   * (1 row at a time, the whole image at once, or anything between - see
   * je_write_rows() in tjpge.h); finishEncode() encodes the final
   * (possibly partial) row band and writes EOI. Exactly one
   * beginEncode()/writeRows()+/finishEncode() sequence may be in flight
   * on a given TinyJPEGEncoder instance at a time (it shares this
   * instance's workspace_ with encodeJpg(), so the two can't run
   * concurrently either) - start a new sequence only after the previous
   * one's finishEncode() has returned.
   *
   * This array-sink overload's `outSize` (via bytesWritten(), since
   * there's no single call to return it from) has the same truncation
   * contract as encodeJpg()'s array overload: JER_INTR if the encoded
   * JPEG doesn't fit `outCapacity`.
   */
  JERESULT beginEncode(uint16_t width, uint16_t height, JEPixelFormat format, uint8_t *outBuf, size_t outCapacity) {
    streamArrayCtx_ = ArrayContext(outBuf, outCapacity);
    return beginEncodeInto(width, height, format, &streamArrayCtx_);
  }

  /// Streaming row API, output via callback instead of a fixed array - see beginEncode(...,outBuf,outCapacity) and OutputCallback's own doc comment above.
  JERESULT beginEncode(uint16_t width, uint16_t height, JEPixelFormat format, OutputCallback write, void *userData) {
    streamCallbackCtx_ = CallbackContext(write, userData);
    return beginEncodeInto(width, height, format, &streamCallbackCtx_);
  }

  /// Feeds `numRows` more source rows (row-major, in the format beginEncode() was given) - see je_write_rows() in tjpge.h.
  JERESULT writeRows(const uint8_t *rows, uint16_t numRows) { return je_write_rows(&je_, rows, numRows); }

  /// Encodes the final row band and writes EOI - see je_finish() in tjpge.h. Ends the streaming sequence beginEncode() started.
  JERESULT finishEncode() { return je_finish(&je_); }

  /// Bytes written so far to the array `beginEncode(...,outBuf,outCapacity)` was given - meaningless (always 0) after the OutputCallback overload, since that sink doesn't buffer here at all.
  size_t bytesWritten() const { return streamArrayCtx_.written; }

 private:
  // Common base every output sink implements - je->device (see tjpge.h,
  // "Caller-owned I/O device identifier for the session") always points
  // at one of these while an encode is in progress, so jeOutputTrampoline()
  // below can reach the sink-specific write() override through a single
  // pointer, without needing to know which concrete context type is
  // actually in play - mirrors TinyJPEGDecoder::Context's own rationale.
  struct Context {
    virtual ~Context() = default;
    virtual size_t write(const uint8_t *buf, size_t len) = 0;
  };

  struct ArrayContext : Context {
    ArrayContext() = default; /* for streamArrayCtx_ - reassigned by beginEncode() before use */
    ArrayContext(uint8_t *b, size_t cap) : buf(b), capacity(cap) {}
    size_t write(const uint8_t *data, size_t len) override {
      if (written + len > capacity) len = capacity - written;
      if (len) memcpy(buf + written, data, len);
      written += len;
      return len;
    }
    uint8_t *buf = nullptr;
    size_t capacity = 0;
    size_t written = 0;
  };

  struct CallbackContext : Context {
    CallbackContext() = default; /* for streamCallbackCtx_ - reassigned by beginEncode() before use */
    CallbackContext(OutputCallback w, void *u) : writer(w), userData(u) {}
    size_t write(const uint8_t *data, size_t len) override { return writer(userData, data, len); }
    OutputCallback writer = nullptr;
    void *userData = nullptr;
  };

  template <typename FileT>
  struct FileContext : Context {
    explicit FileContext(FileT &f) : file(f) {}
    size_t write(const uint8_t *data, size_t len) override { return file.write(data, len); }
    FileT &file;
  };

  static size_t jeOutputTrampoline(JENC *je, const uint8_t *data, size_t len) {
    return static_cast<Context *>(je->device)->write(data, len);
  }

  // `ctx` is passed as `Context&` for the same reason TinyJPEGDecoder's
  // prepare() takes `Context&` rather than a concrete subtype - see that
  // function's own comment.
  JERESULT runEncode(Context &ctx, const uint8_t *pixels, uint16_t width, uint16_t height, JEPixelFormat format) {
    JENC je{};
    JERESULT r = je_prepare(&je, &jeOutputTrampoline, workspace_, sizeof(workspace_), &ctx, width, height, format);
    if (r != JER_OK) return r;
    je.quality = quality_;
    je.subsample = subsample_ ? 1 : 0;
    return je_encode(&je, pixels);
  }

  // Shared by both beginEncode() overloads: `ctx` is one of this
  // instance's own persistent streamArrayCtx_/streamCallbackCtx_ members
  // (not a local, unlike runEncode()'s `ctx` above) - it, and je_ itself,
  // must stay alive across the whole beginEncode()/writeRows()+/
  // finishEncode() sequence, which a per-call local can't do.
  JERESULT beginEncodeInto(uint16_t width, uint16_t height, JEPixelFormat format, Context *ctx) {
    je_ = JENC{};
    JERESULT r = je_prepare(&je_, &jeOutputTrampoline, workspace_, sizeof(workspace_), ctx, width, height, format);
    if (r != JER_OK) return r;
    je_.quality = quality_;
    je_.subsample = subsample_ ? 1 : 0;
    return je_start(&je_);
  }

  uint8_t quality_ = JE_QUALITY;
  bool subsample_ = (JE_SUBSAMPLE != 0);
  void *userData_ = nullptr;

  // Streaming-row API state (beginEncode()/writeRows()/finishEncode()) -
  // separate from encodeJpg()'s own local `JENC je{}`/Context so the two
  // APIs don't interfere with each other's per-call locals, though (see
  // beginEncode()'s own doc comment) they still share workspace_ and so
  // can't run concurrently on the same instance.
  JENC je_{};
  ArrayContext streamArrayCtx_;
  CallbackContext streamCallbackCtx_;

  // Must align workspace to a 32 bit boundary - same rationale as
  // TinyJPEGDecoder::workspace_ (some platforms fault on unaligned 32-bit
  // access from the tables tjpge.h pool-allocates out of it).
  alignas(4) uint8_t workspace_[TJPGE_WORKSPACE_SIZE];
};

}  // namespace tinyjpeg
