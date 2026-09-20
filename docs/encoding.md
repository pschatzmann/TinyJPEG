# Encoding

`TinyJPEGEncoder` (`src/TinyJPEGEncoder.h`, namespace `tinyjpeg`) encodes
pixels into a baseline (non-progressive) JPEG, streaming the *compressed
output* incrementally (`JE_SZBUF`, 512 bytes by default, at a time)
rather than requiring memory for the whole encoded file at once. It never
allocates on the heap: the quantization tables, Huffman encode lookup
tables, the output buffer, and (for the streaming row API) a small
row-band buffer that `tjpge.h` needs all live in one fixed-size member
array (`TJPGE_WORKSPACE_SIZE` bytes - see `src/tjpge_config.h`).

Two ways to feed it pixels, row-major, in whichever `JEPixelFormat`
(`tjpge.h`) you pass:

- **`encodeJpg()`** - one full frame up front. Simplest; fine when you
  already have (or don't mind holding) the whole frame in RAM.
- **`beginEncode()`/`writeRows()`/`finishEncode()`** - rows fed
  incrementally (1 row at a time, the whole image at once, or anything in
  between), for a source that produces rows over time and shouldn't need
  to be buffered whole first - a camera driver, a display back buffer
  read line-by-line. Peak pixel memory is one MCU row band (8 source
  rows, or 16 for 4:2:0 color) regardless of image height, not the whole
  frame - see "Streaming rows" below and `docs/architecture.md`'s
  "Encoder" section for the full design rationale (chroma subsampling and
  the forward DCT need to see more than one row at a time, but never
  needed to see a whole *frame* at a time - that was just the simplest
  first version).

Either way, the source pixels are read via plain pointer arithmetic, not
`PROGMEM` - copy flash data to RAM first if your platform needs that, the
same as `TinyJPEGDecoder`'s array input path.

## Pixel formats (`JEPixelFormat`)

| Format | Bytes/pixel | Layout |
|---|---|---|
| `JE_FMT_RGB888` | 3 | Interleaved R, G, B, 0-255 each. |
| `JE_FMT_RGB565` | 2 | Packed 5-6-5 into a native-endian `uint16_t` (byte 0 = low, byte 1 = high) - the same in-memory layout as a `uint16_t*` framebuffer array on a little-endian MCU, e.g. most TFT driver back buffers. |
| `JE_FMT_RGB666` | 3 | Interleaved R, G, B, same layout as `JE_FMT_RGB888` - only the top 6 bits of each byte are meaningful (the low 2 bits are don't-care). |
| `JE_FMT_GRAY8` | 1 | 8-bit grayscale. |

`JE_FMT_RGB565` exists because that's what most embedded frame buffers
actually are, not RGB888 - a TFT back buffer, an ESP32 camera in RGB565
mode, or an off-screen `GFXcanvas16` can all be passed to `encodeJpg()`
directly, with no manual unpacking. Each pixel is expanded to 8 bits per
channel via bit replication (`(v5 << 3) | (v5 >> 2)` for 5-bit channels,
`(v6 << 2) | (v6 >> 4)` for the 6-bit green channel - the same technique
`test_helpers.h`'s `rgb565ToRgb888()` uses on the decode side) before
color conversion and the forward DCT, both of which only ever see 8-bit
RGB regardless of the source format.

`JE_FMT_RGB666` (18-bit color) is handled byte-identically to
`JE_FMT_RGB888` - `get_rgb()` reads all 8 bits of each byte either way, so
a don't-care low 2 bits costs nothing to support and needs no separate
unpacking path. It's still a distinct enum value rather than just telling
callers to pass `JE_FMT_RGB888`, purely so a caller's own intent (and the
precision their source is actually providing) is explicit at the call
site. This covers the byte-per-channel RGB666 this project has seen from
embedded sources; the genuinely tight-packed 18-bit variant (2.25
bytes/pixel, no unused bits) isn't supported - open an issue if you have
a real source that produces it.

`JE_FMT_RGB565`'s "native-endian" byte order is the in-RAM layout of a
`uint16_t` array on the MCU, *not* necessarily the wire/SPI byte order
some displays expect for transmission (that's `setSwapBytes()`'s job on
`TinyJPEGDecoder`'s output side - a display-driver concern this encoder
has no part in, since it only ever reads pixels, never sends them
anywhere).

## Basic usage (encode to a fixed byte array)

```cpp
#include <TinyJPEGEncoder.h>
using namespace tinyjpeg;

TinyJPEGEncoder encoder;
encoder.setQuality(85);       // 1-100, libjpeg-style scale (default JE_QUALITY)
encoder.setSubsample(true);   // 4:2:0 (default JE_SUBSAMPLE) - false for 4:4:4

uint8_t jpgBuf[16384];
size_t jpgSize = 0;
JERESULT result = encoder.encodeJpg(rgbPixels, width, height, JE_FMT_RGB888, jpgBuf, sizeof(jpgBuf), jpgSize);
if (result != JER_OK) {
  // see JERESULT in tjpge.h for the full error list (JER_INTR, JER_PAR, JER_MEM1)
}
// jpgBuf[0..jpgSize) now holds a complete JPEG file.
```

If the encoded JPEG doesn't fit in `outCapacity`, `encodeJpg()` returns
`JER_INTR` (not `JER_MEM1` - that's reserved for the fixed workspace pool
running out, a configuration problem, not a data-size one) and `jpgSize`
holds a truncated, unusable prefix - check the return value, don't just
trust `jpgSize` alone. There's no way to know the encoded size ahead of
time without encoding it, since it depends on image content, not just
dimensions.

## Encoding to a file (SD, LittleFS, SPIFFS, or anything file-shaped)

`encodeJpg` also takes a template `FileT&` - any type exposing:

```cpp
size_t write(const uint8_t* buf, size_t len);   // write len bytes, return how many were accepted
```

Arduino's `fs::File` (SPIFFS/LittleFS) and SD's `File` both already
satisfy this - open the file yourself and pass the handle:

```cpp
File f = SD.open("/photo.jpg", FILE_WRITE);
JERESULT result = encoder.encodeJpg(rgbPixels, width, height, JE_FMT_RGB888, f);
f.close();
```

This is a template, not a hardwired call into `SD.h`/`FS.h` - this header
never `#include`s either, so it works identically for any conforming
type, Arduino or not (see `test/native/test_encoder_wrapper.cpp` for a
plain stdio `FILE*` wrapper used exactly this way in the desktop test
suite, and `examples/EncodeToSD/EncodeToSD.ino` for a complete sketch).

`file` is written from its current position and left wherever the encode
happened to stop; this class never opens, closes, or truncates it -
`SD.open(path, FILE_WRITE)` itself appends rather than truncates, so a
real sketch writing the same path repeatedly should `SD.remove()` (or
equivalent) first, as the example does.

## Encoding to a custom destination (network, ring buffer, ...)

For a destination that isn't file-shaped, use the plain-callback overload
instead:

```cpp
size_t writeToSocket(void* userData, const uint8_t* buf, size_t len) {
  MySocket* sock = static_cast<MySocket*>(userData);
  return sock->writeExactly(buf, len);  // must accept all len bytes, or fewer only on a genuine failure
}

JERESULT result = encoder.encodeJpg(rgbPixels, width, height, JE_FMT_RGB888, &writeToSocket, &mySocket);
```

**Contract**: `write` must return exactly `len` on success; anything less
aborts the encode with `JER_INTR` (mirrors `tjpge.h`'s own
`JENC::outfunc` short-write contract, and the too-small-buffer case of the
array overload above). This is a "block until accepted" contract, the
same one a synchronous file write or a blocking socket send naturally
satisfies - a destination prone to genuine partial writes needs to retry
underneath this callback until the full `len` is accepted, or treat a
partial accept as fatal.

A `FileT` sink can use this same callback overload instead of the
template `encodeJpg(...,FileT&)` overload - wrap it in a one-line
captureless lambda (which converts to a plain function pointer, same as
`OutputCallback` itself):

```cpp
File f = SD.open("/photo.jpg", FILE_WRITE);
JERESULT result = encoder.encodeJpg(rgbPixels, width, height, JE_FMT_RGB888,
    [](void* userData, const uint8_t* buf, size_t len) -> size_t {
      return static_cast<File*>(userData)->write(buf, len);
    }, &f);
f.close();
```

This is exactly how the streaming row API's array/callback sinks avoid
needing a template session type per `FileT` - see "Streaming rows" below.

## Streaming rows

For a source that produces pixel rows over time (a camera driver, a
display back buffer read line-by-line) rather than handing over a
complete frame, `beginEncode()`/`writeRows()`/`finishEncode()` replace a
single `encodeJpg()` call - same three output sinks (array, file,
callback via the lambda pattern above), same options, but pixels arrive
incrementally instead of all at once:

```cpp
JERESULT result = encoder.beginEncode(width, height, JE_FMT_RGB565, jpgBuf, sizeof(jpgBuf));
if (result != JER_OK) { /* ... */ }

while (/* your source has more rows */) {
  result = encoder.writeRows(nextRowOrRows, numRows);  // 1 row at a time, or however many you have
  if (result != JER_OK) break;
}

result = encoder.finishEncode();
size_t jpgSize = encoder.bytesWritten();  // array sink only - see below
```

See `examples/EncodeStreamFromSD/EncodeStreamFromSD.ino` for a complete
sketch that reads a raw RGB888 image from an SD card one row at a time
and streams it straight into an SD-file-destined encode - contrast with
`examples/EncodeToSD/EncodeToSD.ino`'s single `encodeJpg()` call on a
frame already fully in RAM: the streaming version's peak pixel memory is
one row (a few hundred bytes) regardless of image size, not the whole
frame.

`beginEncode()` has two overloads, mirroring `encodeJpg()`'s array and
callback overloads (there's no dedicated file overload - use the lambda
pattern above for a `FileT` sink):

```cpp
JERESULT beginEncode(uint16_t width, uint16_t height, JEPixelFormat format, uint8_t* outBuf, size_t outCapacity);
JERESULT beginEncode(uint16_t width, uint16_t height, JEPixelFormat format, OutputCallback write, void* userData);
```

`writeRows(rows, numRows)` accepts `numRows` more rows (row-major, in
`beginEncode()`'s format, `numRows * width * bytesPerPixel` bytes) and
may be called any number of times with any number of rows each - it
doesn't need to line up with the 8-row (or 16-row, for 4:2:0 color) MCU
band size internally, `tjpge.h`'s `je_write_rows()` buffers across calls
correctly regardless of how the input is chunked (see
`test/native/test_encode_streaming.cpp`, which proves feeding 1 row at a
time produces byte-identical output to feeding the whole image in one
call). Peak pixel memory is exactly one row band - `width *
bytesPerPixel * (subsample ? 16 : 8)` bytes - never the whole frame.

`finishEncode()` encodes the final (possibly partial) row band - padded
by replicating its last row, the same right-edge replication `encodeJpg()`
already does for images whose width isn't a multiple of the MCU size,
just applied to the bottom edge instead - and writes EOI.
`bytesWritten()` then reports the final size for the array sink (it's
meaningless, always 0, for the callback sink, since that sink never
buffers here at all - track your own total from the callback if you need
it there).

**Sizing note**: unlike the raw `tjpge.h` engine (which pool-allocates
its row-band buffer to the exact `width` passed to `je_prepare()`, no
matter how large), `TinyJPEGEncoder`'s workspace is a fixed compile-time
array, so it budgets the row-band buffer for widths up to `JE_MAX_WIDTH`
(default 320) - `beginEncode()` returns `JER_MEM1` if the actual `width`
exceeds what fits. Raise `JE_MAX_WIDTH` (a `-D` or `#define` before this
library's first `#include`) if you need wider streaming images, or call
`tjpge.h`'s `je_prepare()`/`je_start()`/`je_write_rows()`/`je_finish()`
directly with your own precisely-sized pool instead of going through
`TinyJPEGEncoder` at all - see `src/tjpge_config.h`'s own comment on
`JE_MAX_WIDTH` for the exact accounting.

**One session at a time**: a `beginEncode()`/`writeRows()`/`finishEncode()`
sequence (one `beginEncode()`, then one or more `writeRows()` calls, then
one `finishEncode()`) shares this instance's `workspace_` with
`encodeJpg()` - don't
start a new sequence (of either kind) until the previous one has finished
(`test/native/test_encoder_wrapper_streaming.cpp` exercises calling
`encodeJpg()` immediately followed by a full `beginEncode()`/`writeRows()`/
`finishEncode()` sequence on the same instance, proving they don't leave
stale state behind for each other - but the two still can't be
*interleaved*).

## Options

- `setQuality(1-100)` - libjpeg-style JPEG quality scale. Higher means a
  larger file and less loss (default `JE_QUALITY`, 80).
- `setSubsample(bool)` - `true` for 4:2:0 chroma subsampling (smaller
  files, standard default for photographic content), `false` for 4:4:4
  (no subsampling). Ignored for grayscale input (`JE_FMT_GRAY8`), which has
  no chroma to subsample. Default `JE_SUBSAMPLE`.
- `setUserData(ptr)` / `getUserData()` - an arbitrary caller-owned pointer
  carried on the instance, untouched by this class otherwise. Unlike
  `TinyJPEGDecoder`'s version, nothing in this class reads it back itself
  (there's no per-block callback here to hand it to) - it's provided
  purely as a convenient place to keep context alongside the encoder
  instance. `nullptr` until set.

## Compile-time configuration (`src/tjpge_config.h`)

Override any of these with `-D` or a `#define` before the *first*
`#include` of this library anywhere in the build (the standard
header-only-library override idiom):

- `JE_QUALITY` (default `80`) - default quality when `setQuality()` is
  never called.
- `JE_SUBSAMPLE` (default `1`, 4:2:0) - default subsampling mode when
  `setSubsample()` is never called; `0` for 4:4:4.
- `JE_SZBUF` (default `512`) - output stream buffer size.
- `JE_FIXED_POINT_DCT` (default: auto-detected per target - `1` on
  FPU-less architectures, `0` elsewhere) - `0` uses the float forward
  DCT, `1` the fixed-point one. Both produce the same encoded image
  quality; this is purely a speed choice, and which one is actually
  faster is platform-dependent (float wins on every FPU-equipped target
  measured so far; fixed-point wins by ~2.2-2.6x on the FPU-less RP2040)
  - see [Performance](performance.md)'s "Float vs. fixed-point" section
  for the measurements and exactly how the auto-detection works, and
  override this if it guesses wrong for your target.
- `JE_MAX_WIDTH` (default `320`) - widest image `TinyJPEGEncoder`'s fixed
  workspace budgets for - see "Streaming rows"' own "Sizing note" above
  for what happens when the actual image is wider, and when this doesn't
  apply at all (calling `tjpge.h` directly with your own pool).

`TJPGE_WORKSPACE_SIZE` is derived from `JE_SZBUF`/`JE_MAX_WIDTH` and is
not meant to be overridden directly - see `src/tjpge_config.h`'s comment
for exactly what it accounts for (quantization tables, Huffman lookup
tables, the output buffer, and the row-band buffer).

## What's shared with the decoder, and what isn't

`src/jpeg_common.h` holds the two pieces genuinely common to both
directions - the zigzag/raster permutation table and the byte-saturation
clamp. Everything else the encoder needs (the fixed Annex-K Huffman
tables it packs against, the forward DCT, the RGB→YCbCr color-conversion
constants) is direction-specific and lives only in `tjpge.h` - see that
file's own header comment, and `docs/architecture.md`'s "Encoder" section,
for the reasoning behind what was and wasn't shared.
