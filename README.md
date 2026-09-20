# TinyJPEG

[![Arduino Library](https://img.shields.io/badge/Arduino-Library-blue.svg)](https://www.arduino.cc/reference/en/libraries/)
[![CMake](https://img.shields.io/badge/CMake-Supported-blue.svg)](https://cmake.org/)
[![IDF Component](https://img.shields.io/badge/IDF-Component-blue.svg)](https://github.com/pschatzmann/TinyJPEG)
[![License: FreeBSD](https://img.shields.io/badge/License-FreeBSD-green.svg)](https://www.freebsd.org/copyright/freebsd-license/)

A header-only C++ baseline (non-progressive) JPEG **decoder and encoder**
for microcontrollers such as the ESP32, with no external library
dependencies. Both directions are header-only C++17, allocate no heap
memory (each uses one fixed-size workspace member array), and work
identically from an Arduino sketch, a plain CMake project, or an ESP-IDF
component:

- **`TinyJPEGDecoder`** (`src/TinyJPEGDecoder.h`) decodes straight into
  caller-supplied storage one MCU block at a time via a callback
  (typically pushed straight to a TFT display), rather than requiring
  memory for a whole decoded frame at once. It's a **port**, not a
  rewrite: the actual decompressor (bitstream/Huffman/IDCT/YCbCr code) is
  [ChaN's proven TJpgDec](http://elm-chan.org/fsw/tjpgd/00index.html)
  engine, merged from its original two-file split into one header with no
  algorithmic changes, wrapped in an API in the spirit of [Bodmer's
  TJpg_Decoder](https://github.com/Bodmer/TJpg_Decoder).
- **`TinyJPEGEncoder`** (`src/TinyJPEGEncoder.h`) encodes one full
  RGB888/grayscale frame (a camera buffer, a display back buffer -
  whatever's already in RAM) to a baseline JPEG, streaming the compressed
  output to a fixed array, a file-like object, or a callback. It's a
  **clean-room implementation** written directly from the JPEG spec
  (ITU-T T.81) - there's no equivalent existing small-footprint encoder
  this project is porting from, the way `TinyJPEGDecoder` ports TJpgDec.

Both share `src/jpeg_common.h` (the zigzag table and byte-clamp function
that are direction-agnostic) but are otherwise independent - use either
one without the other, and neither drags in Arduino-specific code (SD,
SPIFFS, displays, ...) at the core level.

See [Architecture](docs/architecture.md) for the full design rationale
behind both directions, including an API-mapping table for the decoder if
you're migrating an existing TJpg_Decoder sketch, and the "Encoder"
section explaining what is and isn't shared between the two.

## Decoding

```cpp
#include <TinyJPEGDecoder.h>
using namespace tinyjpeg;

bool onBlock(TinyJPEGDecoder& decoder, int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  tft.pushImage(x, y, w, h, bitmap);  // or however your display draws a block
  return true;
}

TinyJPEGDecoder decoder;
decoder.setCallback(onBlock);
JRESULT r = decoder.drawJpg(0, 0, jpegData, jpegSize);
```

- **No Arduino dependency in the decoder itself.** The core reads via a
  generic callback (a memory buffer, any file-like object exposing
  `available()`/`read()`/`position()`/`seek()`, or a plain function
  pointer for anything else - see [Decoding](docs/decoding.md)), so the
  exact same code decodes on a desktop/CMake build, an ESP-IDF component,
  or an Arduino sketch, unlike TJpg_Decoder's SD/SPIFFS/LittleFS-specific
  methods.
- **No global singleton.** TJpg_Decoder relies on one process-wide
  instance; `TinyJPEGDecoder` is an ordinary object - construct as many as
  you like, each with independent state (see
  `test/native/test_multiple_instances.cpp`). The block callback carries a
  reference to the instance it's decoding for, plus a `setUserData()`/
  `getUserData()` pointer this class stores but never interprets, so a
  callback can reach whatever per-decode context it needs (a display
  object, an output buffer) without a global/static variable.
- **Fixed, small workspace** (`TJPGD_WORKSPACE_SIZE`, ~3.5KB with default
  settings) regardless of image size or resolution - no heap allocation.

Baseline JPEG only (as TJpg_Decoder itself is) - progressive JPEGs are
rejected with `JDR_FMT3`, not silently mis-decoded, the same restriction
ChaN's TJpgDec has always had (more memory would be needed to buffer a
progressive scan's multiple passes).

See [Decoding](docs/decoding.md) for the full API: in-memory buffers,
file-like sources (SD/LittleFS/SPIFFS or any conforming `FileT`), custom
streaming callbacks, and every compile-time configuration option
(`JD_FORMAT`, `JD_FASTDECODE`, ...).

## Encoding

```cpp
#include <TinyJPEGEncoder.h>
using namespace tinyjpeg;

TinyJPEGEncoder encoder;
encoder.setQuality(85);       // 1-100, default JE_QUALITY
encoder.setSubsample(true);   // 4:2:0 - smaller files, default JE_SUBSAMPLE

uint8_t jpgBuf[16384];
size_t jpgSize = 0;
JERESULT r = encoder.encodeJpg(rgbPixels, width, height, JE_FMT_RGB888, jpgBuf, sizeof(jpgBuf), jpgSize);
```

- **Full-frame or row-streamed input, always streamed output.**
  `encodeJpg()` takes one complete pixel buffer up front, for when you
  already have (or don't mind holding) the whole frame in RAM.
  `beginEncode()`/`writeRows()`/`finishEncode()` instead take rows
  incrementally - 1 at a time, the whole image at once, or anything
  between - for a camera driver or display back buffer that produces rows
  over time; peak pixel memory is one MCU row band (8 rows, or 16 for
  4:2:0 color) regardless of image height, not the whole frame. Either
  way, the *compressed output* is always streamed incrementally
  (`JE_SZBUF` bytes at a time) to a fixed array, a file-like object
  (SD/LittleFS/SPIFFS or any conforming `FileT`), or a plain callback -
  never buffered whole in memory.
- **RGB565/RGB666 input, no manual conversion.** `JEPixelFormat` covers
  `JE_FMT_RGB888`, `JE_FMT_RGB565` (2 bytes/pixel, packed 5-6-5 - the
  actual in-memory format of most TFT/camera frame buffers, not RGB888),
  `JE_FMT_RGB666` (3 bytes/pixel like RGB888, only the top 6 bits of each
  byte significant), and `JE_FMT_GRAY8` - pass a TFT back buffer or
  camera frame straight to `encodeJpg()` in whichever of these it's
  already in - see [Encoding](docs/encoding.md)'s "Pixel formats" section.
- **No Arduino dependency, no global singleton, fixed workspace** - the
  same three properties as the decoder above, for the same reasons; see
  [Architecture](docs/architecture.md).
- **Correctness checked by round-trip**, not just self-consistency:
  `test/native/test_encode_roundtrip.cpp` encodes with `tjpge.h`, decodes
  the result back with this same repo's own `tjpgd.h`, and diffs against
  the original source pixels - a pass means the encoder produces a
  spec-legal bitstream any conformant decoder (not just this one) can
  read back faithfully.

See [Encoding](docs/encoding.md) for the full API: encoding to a fixed
array, a file-like sink, or a custom streaming callback, and every
compile-time configuration option (`JE_QUALITY`, `JE_SUBSAMPLE`, ...).

Measured (not estimated) timings for both directions - desktop x86 and
real hardware (ESP32-S3, RP2350, RP2040) - are in
[Performance](docs/performance.md).

## Part of AudioTools

TinyJPEG is also used as a video frame decoder as part of the video
playback functionality provided by
[AudioTools](https://github.com/pschatzmann/arduino-audio-tools) - the
same author's broader library for audio (and video) I/O on Arduino.
If you're already using AudioTools for video playback, this is the
decoder behind the JPEG/MJPEG frames it plays; used standalone (as
everything above describes), it needs nothing from AudioTools at all.

## Documentation

- [Architecture](docs/architecture.md) - why this project exists, the
  exact file-by-file mapping from TJpg_Decoder/TJpgDec for the decoder,
  the API changes (no singleton, no SD/SPIFFS-specific methods) with a
  full before/after table, and the encoder's own design section (what's
  shared between encode/decode via `jpeg_common.h`, what isn't and why,
  and why the encoder takes a full frame instead of streaming MCUs).
- [Decoding](docs/decoding.md) - `TinyJPEGDecoder` usage: in-memory
  buffers, file-like sources (SD/LittleFS/SPIFFS or any conforming
  `FileT`), custom streaming callbacks, and every compile-time
  configuration option (`JD_FORMAT`, `JD_FASTDECODE`, ...).
- [Encoding](docs/encoding.md) - `TinyJPEGEncoder` usage: encoding to a
  fixed array, a file-like sink, or a custom streaming callback, and
  every compile-time configuration option (`JE_QUALITY`,
  `JE_SUBSAMPLE`, ...).
- [Testing](docs/testing.md) - running the native CMake/CTest suite, how
  the test assets are generated, and why the pixel-comparison tests (for
  both directions) are tolerance-based rather than byte-exact.
- [Performance](docs/performance.md) - measured desktop *and* real
  hardware (ESP32-S3, RP2350, RP2040) timings for both directions, and
  how to reproduce any of them.

## Installation

For Arduino, download this library as a zip and use Library ->
Include Library -> Add .ZIP Library. Or git clone this project into
your Arduino libraries folder, e.g.

```
cd ~/Documents/Arduino/libraries
git clone https://github.com/pschatzmann/TinyJPEG.git
```

No external Arduino library dependencies - the only include beyond the
C++ standard library is `Arduino.h` itself (transitively, via the Arduino
build, and only in the examples - neither the decoder nor the encoder
core in `src/` includes it). Use `#include <TinyJPEGDecoder.h>`,
`#include <TinyJPEGEncoder.h>`, or both, independently of one another.

For CMake or ESP-IDF projects instead, see [Testing](docs/testing.md) for
`add_subdirectory()`/`EXTRA_COMPONENT_DIRS` usage.
