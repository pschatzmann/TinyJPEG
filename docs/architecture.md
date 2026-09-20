# Architecture

## Why this exists

[TJpg_Decoder](https://github.com/Bodmer/TJpg_Decoder) (Bodmer) is a
well-established Arduino wrapper around [ChaN's
TJpgDec](http://elm-chan.org/fsw/tjpgd/00index.html) - a small, proven
baseline JPEG decoder built for tiny embedded systems. It works well, but
it's an Arduino library in the traditional sense: a `.cpp`/`.h` pair meant
to be compiled once per sketch, built around a process-wide singleton
(`extern TJpg_Decoder TJpgDec`), and its file-based API is hardwired to
Arduino's `SD`/`SPIFFS`/`LittleFS` types. That makes it awkward to:

- use from a plain CMake or ESP-IDF project (no Arduino build system in
  the loop at all);
- unit-test on a desktop with a real correctness oracle (no `SD.h` on a
  desktop machine);
- decode from anything other than a file or a single flash array (no
  network stream, no ring buffer, no in-memory buffer that isn't
  `PROGMEM`);
- run two independent decodes at once (the singleton has exactly one set
  of instance state for the whole process).

TinyJPEG is a **conversion**, not a rewrite: `src/tjpgd.h` is ChaN's
TJpgDec R0.03 decompressor engine (the actual bitstream/Huffman/IDCT/
YCbCr code), merged from its original two-file split (`tjpgd.h` +
`tjpgd.c`) into one header and given `inline` linkage so it can be
`#include`d from as many translation units as needed with no separate
compilation step - see that file's own header comment for the complete,
itemized list of mechanical changes (there are no algorithmic ones).
`src/TinyJPEGDecoder.h` is a new wrapper class, in the same spirit as
Bodmer's `TJpg_Decoder` (same block-callback decode model, same
`drawJpg`/`getJpgSize`/`setJpgScale`/`setSwapBytes`/`setCallback` naming),
but with the Arduino/SD/SPIFFS coupling replaced by a small, generic,
header-only input abstraction - see "API changes from TJpg_Decoder" below.

This mirrors the conversion this author has already done for other
Arduino codec libraries -
[TinyH264](https://github.com/pschatzmann/TinyH264) and
[TinyMPG](https://github.com/pschatzmann/TinyMPG) - though those two are
from-scratch reimplementations of their respective standards (no existing
small-footprint C/C++ H.264 or MPEG-1 decoder to port from under a
compatible license), where this project instead converts existing, proven
code. The house style (header-only, `src/`+`test/native/`+`examples/`+
`docs/` layout, CMake `INTERFACE` target with an `ESP_PLATFORM` branch,
explicit-list `ctest` suite) is intentionally the same across all three -
see [Testing](testing.md).

## File layout

```
src/
  jpeg_common.h        - pieces shared by both directions (zigzag table, byte clamp)
  tjpgd_config.h        - JD_* compile-time configuration (was tjpgdcnf.h)
  tjpgd.h               - ChaN's TJpgDec engine (was tjpgd.h + tjpgd.c)
  TinyJPEGDecoder.h      - the public decoder wrapper class (was TJpg_Decoder.h/.cpp)
  tjpge_config.h         - JE_* compile-time configuration (encoder)
  tjpge.h                - the encoder engine (clean-room, no upstream counterpart)
  TinyJPEGEncoder.h       - the public encoder wrapper class
test/native/             - desktop ctest suite - see testing.md
examples/                - Arduino .ino sketches
docs/                    - this file, decoding.md, encoding.md, testing.md
```

There is no `User_Config.h` equivalent - TJpg_Decoder's own copy of that
file only ever toggled `TJPGD_LOAD_FFS`/`TJPGD_LOAD_SD_LIBRARY` to decide
which Arduino filesystem headers to `#include` and which
`drawSdJpg`/`drawFsJpg` overloads to compile in. TinyJPEGDecoder has
nothing analogous to toggle: the file-source overload is a template (see
below), so it never `#include`s any filesystem header itself, Arduino or
otherwise.

## API changes from TJpg_Decoder

| TJpg_Decoder | TinyJPEGDecoder | Why |
|---|---|---|
| `extern TJpg_Decoder TJpgDec;` (global singleton) | `tinyjpeg::TinyJPEGDecoder decoder;` (ordinary object, any number of them) | See "No more singleton" below. |
| `TJpgDec.drawJpg(x, y, array, size)` | `decoder.drawJpg(x, y, array, size)` | Unchanged signature. |
| `TJpgDec.getJpgSize(&w, &h, array, size)` | `decoder.getJpgSize(&w, &h, array, size)` | Unchanged signature. |
| `TJpgDec.drawSdJpg(x, y, path)` / `drawFsJpg(x, y, path, fs)` | `decoder.drawJpg(x, y, file)` (template over any `available()`/`read()`/`position()`/`seek()` type) | One template code path instead of separate SD/SPIFFS/LittleFS overloads - see [Decoding](decoding.md). Open the file yourself (`SD.open(...)`, `LittleFS.open(...)`, ...) and pass the handle; TinyJPEGDecoder never touches a filesystem API directly. |
| `TJpgDec.getSdJpgSize(...)` / `getFsJpgSize(...)` | `decoder.getJpgSize(&w, &h, file)` | Same template, no decode. |
| (none) | `decoder.drawJpg(x, y, InputCallback, void* userData)` / `decoder.getJpgSize(&w, &h, InputCallback, void* userData)` | New: a plain-function-pointer input source for anything that isn't file-shaped (a network socket, a ring buffer) - see [Decoding](decoding.md). |
| `TJpgDec.setJpgScale(n)` | `decoder.setJpgScale(n)` | Unchanged. |
| `TJpgDec.setSwapBytes(b)` | `decoder.setSwapBytes(b)` | Unchanged. |
| `TJpgDec.setCallback(cb)` | `decoder.setCallback(cb)` | Almost unchanged - `SketchCallback` gains one leading parameter, a reference to the `TinyJPEGDecoder` instance (`bool(TinyJPEGDecoder& decoder, int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* data)`) - see "The callback's `decoder` parameter and setUserData()" below for why. |
| (none) | `decoder.setUserData(void*)` / `decoder.getUserData()` | New: an arbitrary caller-owned pointer carried on the instance, reachable from inside the callback via its `decoder` parameter - see below. |
| `JDEC`, `JRESULT`, `JRECT`, `JDR_OK`/etc. | Same, unqualified | These are ChaN's own public API (tjpgd.h) and are reused as-is everywhere, including in TinyJPEGDecoder's own return types - not renamed or renamespaced, so error-handling code written against TJpg_Decoder (`if (result != JDR_OK)`) needs no changes. |

### No more singleton

TJpg_Decoder's static C callback trampolines (`jd_input`/`jd_output`, the
functions `jd_prepare`/`jd_decomp` actually call into) need to reach
instance state (the current scale, the sketch's callback, the input
source) but can't capture anything - they're plain function pointers.
TJpg_Decoder solves this with a `thisPtr` self-pointer on the one global
`TJpgDec` instance: every static trampoline reads `TJpgDec.thisPtr` to
find "the" instance, which only works because there's assumed to be
exactly one.

`JDEC` already carries a `void* device` field for exactly this kind of
problem ("Pointer to I/O device identifiler for the session" - tjpgd.h's
own comment), just unused by TJpg_Decoder. TinyJPEGDecoder uses it: each
`drawJpg()`/`getJpgSize()` call builds a small stack-local context object
(holding the offset, the registered callback, and the input source) and
passes its address through as `jd_prepare()`'s `dev` parameter; the
trampolines recover it from `jd->device`. That means instance state lives
on the instance (and the call stack), not in a global - so distinct
`TinyJPEGDecoder` objects never interfere with each other (see
`test/native/test_multiple_instances.cpp`), and there is no
must-be-defined-exactly-once global to trip over when linking multiple
translation units.

### The callback's `decoder` parameter and setUserData()

TJpg_Decoder's `SketchCallback` never needed a way to reach per-instance
context because there was only ever one instance to reach - a sketch's
callback function just closed over whatever globals it needed (`tft`,
typically). `TinyJPEGDecoder` has no such guarantee once more than one
instance exists (see "No more singleton" above), so `SketchCallback`
carries a reference to the instance drawJpg() was called on as its first
parameter, and the class adds `setUserData(void*)`/`getUserData()` - an
arbitrary pointer this class stores but never interprets - so a callback
can recover whatever context it needs (a display object, an output
buffer, a struct bundling several) through that reference instead of a
global/static variable. `examples/DecodeToDisplay/DecodeToDisplay.ino`
reaches its `TFT_eSPI` object this way; `test/native/test_helpers.h`'s
`PixelSink`/`pixelSinkCallback` and
`test/native/test_multiple_instances.cpp` reach two independent decode
targets through the *same* callback function this way, which is exactly
the scenario a plain global variable can't handle cleanly (see that
test's own file comment).

### The FileT / InputCallback input abstraction

TJpg_Decoder's `drawSdJpg`/`drawFsJpg` split exists because `SD::File` and
`fs::File` are two distinct Arduino types with no common base - the
library has to overload separately for each, `#include`ing `<SD.h>` and
`<FS.h>`/`<LittleFS.h>` respectively to do it, gated behind
`TJPGD_LOAD_SD_LIBRARY`/`TJPGD_LOAD_FFS` macros users had to set correctly.

TinyJPEGDecoder's `drawJpg(x, y, FileT& file)` is a template instead: any
type with `available()`, `read(uint8_t*, size_t)`, `position()`, and
`seek(size_t)` methods works, whether that's Arduino's `fs::File`, SD's
`File`, or (as in `test/native/test_file_source.cpp`) a hand-written
wrapper around plain stdio `FILE*` for a native/desktop build. This file
never `#include`s any filesystem header, Arduino or otherwise - it only
needs the method names to exist on whatever `FileT` the *caller* actually
instantiates, resolved at the caller's own compile time. See
[Decoding](decoding.md) for the exact contract and a usage example.

The lower-level `drawJpg(x, y, InputCallback read, void* userData)`
overload exists for sources that aren't file-shaped at all - see
[Decoding](decoding.md) for the read-contract details (in particular: a
call must fulfill the *entire* requested length whenever that much data
is actually available - a partial "short read" is only tolerated for the
one case tjpgd itself tolerates it, an entropy-coded-scan-data refill, not
for header segments).

## Encoder

`src/tjpge.h` + `src/TinyJPEGEncoder.h` are a baseline JPEG *encoder*,
the opposite direction from everything above - not a port of an existing
project (there's no upstream "TJpgEnc" being converted the way `tjpgd.h`
converts ChaN's TJpgDec), but a clean-room implementation written
directly from the JPEG spec (ITU-T T.81) to sit alongside the decoder,
sharing its house style (`_config.h` + engine header + wrapper class,
`inline`/header-only, no heap allocation, pool-allocated fixed workspace)
and its public-API conventions (`TinyJPEGEncoder` mirrors
`TinyJPEGDecoder`'s `setUserData`/`getUserData`, its `Context`+trampoline
pattern for multiple output sinks, and its error-enum shape - `JERESULT`
next to `JRESULT`, `JENC` next to `JDEC`).

### What's shared with the decoder, and what isn't

`src/jpeg_common.h` holds exactly the two pieces that are genuinely
direction-agnostic:

- **`Zig[64]`**, the zigzag↔raster permutation for an 8x8 block. The
  decoder applies it once per de-quantized coefficient (raster write of a
  zigzag-ordered stream); the encoder applies the same table the other
  way (zigzag read of a raster-ordered block) before Huffman-coding the
  coefficients. Same table, opposite walk direction.
- **`byteclip()`**, a saturating clamp to `[0, 255]`. The decoder clips
  IDCT/color-conversion output; the encoder clips RGB/YCbCr intermediates
  before the forward DCT. Same clamp either way.

`tjpgd.h` was refactored to pull both from `jpeg_common.h` instead of
defining its own copies, rather than duplicating them for the encoder -
see that header's own file comment for the full rationale, and its git
history for the (behavior-preserving) refactor itself.

Everything else was deliberately *not* shared, because it isn't actually
the same code wearing a direction flip - it's genuinely different:

- **Huffman tables.** The decoder builds a code→symbol lookup from each
  file's own `DHT` segment (`tjpgd_detail::create_huffman_tbl`); the
  encoder instead always packs against the fixed Annex-K symbol→code
  tables (`tjpge_detail::Std*Bits`/`Std*Vals`, built once into a
  symbol-indexed lookup by `tjpge_detail::build_huff_lut`). Different data
  shape and different construction algorithm, not just opposite lookup
  direction on the same table.
- **The DCT.** `tjpgd_detail::block_idct()` is the Arai/AAN fast
  *inverse* transform, prescaled to fold in de-quantization. The encoder
  needs the *forward* transform, and has two implementations of it -
  `tjpge_detail::forward_dct()` (float) and `forward_dct_fixed()`
  (int64_t Q12 fixed-point) - a direct, un-fused separable algorithm
  either way, not a port of AAN's forward butterfly network (see
  `tjpge.h`'s own file comment for why correctness/simplicity was chosen
  over algorithmic speed here, and `docs/performance.md`'s "Float vs.
  fixed-point" section for why *neither* implementation is universally
  faster - it's genuinely platform-dependent, and `encode_block()` picks
  between them per target via `JE_FIXED_POINT_DCT`, tjpge_config.h).
  Related math to the decoder's IDCT, not a shared routine either way.
- **Color conversion.** Decode goes YCbCr→RGB
  (`tjpgd_detail::mcu_output()`); encode goes RGB→YCbCr
  (`tjpge_detail::rgb_to_ycbcr()`). Different constants, different
  direction, not a shared function.

### Input pixel formats (`JEPixelFormat`)

The decoder's output format (`JD_FORMAT`) is a compile-time choice - one
binary decodes to RGB565, RGB888, or grayscale, whichever
`tjpgd_config.h` was built with. The encoder's input format
(`JEPixelFormat` - `JE_FMT_RGB888`/`JE_FMT_RGB565`/`JE_FMT_RGB666`/
`JE_FMT_GRAY8`) is a runtime choice instead, passed to
`je_prepare()`/`encodeJpg()` per call.
That asymmetry is deliberate, not an oversight: the decoder's output
format is a single, library-wide decision a project makes once (it's
what feeds the rest of that project's rendering pipeline), while an
encoder is far more likely to be handed frames from more than one source
format in the same program - a camera driver producing RGB565 and a
synthetic overlay assembled as RGB888, for instance - so hardcoding one
format at compile time would be the wrong trade here.

`JEPixelFormat` only changes `tjpge_detail::get_rgb()`/`get_gray()`
(tjpge.h) - the byte-unpacking step that runs once per source pixel and
produces plain 8-bit R/G/B (or gray) values. Everything downstream
(`rgb_to_ycbcr()`, `forward_dct()`, quantization, entropy coding) only
ever sees those 8-bit values and has no idea what the source format was;
adding a format is purely a `get_rgb()`/`get_gray()` concern, never a
pipeline-wide one. `JE_FMT_RGB565` (2 bytes/pixel, packed 5-6-5) exists
because that's the actual in-memory format of most TFT/camera frame
buffers, not RGB888 - see [Encoding](encoding.md)'s "Pixel formats"
section for the exact byte layout and the bit-replication expansion used
to bring each channel up to 8 bits. `JE_FMT_RGB666` needs no expansion at
all - it's handled byte-identically to `JE_FMT_RGB888` in `get_rgb()`
(only the top 6 bits of each byte are meaningful, the rest don't-care)
and exists as its own enum value purely so a caller's intent is explicit
at the call site, not because the encoder needs to treat it differently.

### Row-band streaming, not a full frame

`TinyJPEGDecoder` never needs a whole decoded image in memory at once -
it hands each MCU to the caller's callback as soon as that block is
ready, because nothing about decoding one MCU depends on any other MCU
(other than sequential Huffman/DC-prediction state, which is small and
carried on `JDEC` itself). Encoding doesn't have quite that property:
4:2:0 chroma subsampling needs to look at a 16x16 source-pixel
neighborhood to produce one downsampled 8x8 chroma block, so *something*
wider than one MCU's pixels has to be available at a time. But "wider
than one MCU" and "the whole frame" are very different asks - the first
version of this encoder conflated them (`je_encode()` originally required
`width * height` pixels up front), when the actual lookahead need is
only ever one MCU-row band: 8 source rows unsubsampled, 16 for 4:2:0
color (`JENC::mcuRows`).

`je_start()`/`je_write_rows()`/`je_finish()` (`tjpge.h`) are built around
that realization: `je_start()` pool-allocates exactly one band's worth of
buffer (`JENC::rowbuf`, sized `width * bytesPerPixel * mcuRows` - the
only part of the encoder's workspace that scales with image size at
all), and `je_write_rows()` copies caller-supplied rows into it,
encoding-and-discarding the band the moment it fills - regardless of how
the caller chunks their input (1 row per call, the whole image in one
call, or anything between; `tjpge_detail::get_rgb()`/`get_gray()` read
from this band via a row index that's always 0..`mcuRows`-1, never an
absolute image row, so they don't need to know or care how many
`je_write_rows()` calls it took to fill it). `je_finish()` handles the
one remaining wrinkle: if `height` isn't a multiple of `mcuRows`, the
final band is padded by replicating its last real row before encoding -
the same right-edge replication `get_rgb()`/`get_gray()` already did for
a width that isn't a multiple of the MCU size, just applied to the
bottom edge of the *buffer* instead of read-time to a pixel index.
`je_encode(pixels)` still exists, now as a three-line convenience
wrapper (`je_start()` + one `je_write_rows(pixels, height)` call +
`je_finish()`) for callers who do have, or don't mind needing, the whole
frame - it changes nothing about how those callers already used it, it's
simply no longer the *only* option, which is the actual complaint this
design addresses (see [Encoding](encoding.md)'s "Streaming rows" section
for the usage-level version, and `TinyJPEGEncoder::beginEncode()`/
`writeRows()`/`finishEncode()` for the wrapper-level one).

A caller wanting genuinely full-image-lookahead rate-distortion decisions
(optimal Huffman tables, adaptive quantization) still wouldn't fit this
row-band model - but this library doesn't do that either way (see "What's
shared with the decoder, and what isn't" above: fixed Annex-K Huffman
tables, not optimized per-image ones), so there was no such requirement
pulling against row-band buffering in the first place.

What's still streamed the same way the decoder streams its compressed
input, unaffected by any of this, is the encoder's *output*:
`JENC::outfunc`/`TinyJPEGEncoder`'s sinks all receive the encoded bytes
incrementally (`JE_SZBUF` at a time), never as one buffered blob,
regardless of which input mode (`je_encode()` or
`je_start()`/`je_write_rows()`/`je_finish()`) is in use.

See `test/native/test_encode_roundtrip.cpp` for the correctness check
this design enables: encode with `tjpge.h`, decode the result back with
this same repo's `tjpgd.h`, and diff against the original source pixels -
a passing round-trip means the encoder produces a spec-legal bitstream a
conformant decoder reads back faithfully, not just "a decoder we also
wrote agrees with itself" (`test_decode_gradient.cpp` and its siblings
play the equivalent role against libjpeg/Pillow for the decoder alone).
`test/native/test_encode_streaming.cpp` additionally proves the streaming
path isn't a second, independently-verified implementation: it encodes
the same image both ways (one `je_encode()` call vs. many irregularly-
chunked `je_write_rows()` calls, on an image whose dimensions are
deliberately not a multiple of 8 or 16 so both edge-replication paths -
right column and bottom row - are exercised) and asserts the compressed
bytes are byte-for-byte identical.

### `TinyJPEGEncoder`'s workspace vs. `JE_MAX_WIDTH`

`tjpge.h`'s own functions are pool-based and width-agnostic - a caller
driving `je_prepare()`/`je_start()` directly sizes their own pool to
their actual runtime `width`, exactly, no matter how large.
`TinyJPEGEncoder`, though, promises a fixed compile-time `workspace_`
member array (like `TinyJPEGDecoder`'s own `workspace_`) - and since
`je_start()`'s row-band buffer scales with `width`, that promise now
needs a ceiling to stay compile-time-fixed. `JE_MAX_WIDTH` (default 320 -
`tjpge_config.h`) is that ceiling: `TJPGE_WORKSPACE_SIZE` budgets for the
worst case (3 bytes/pixel, 16-row band) at that width, and
`beginEncode()`/`encodeJpg()` return `JER_MEM1` if the actual image is
wider. This is purely a `TinyJPEGEncoder` concession, not an engine
limitation - raise `JE_MAX_WIDTH` for wider images through the wrapper,
or bypass it and call `tjpge.h` directly with a pool sized to the actual
width for no ceiling at all.
