# TinyJPEG

[![Arduino Library](https://img.shields.io/badge/Arduino-Library-blue.svg)](https://www.arduino.cc/reference/en/libraries/)
[![CMake](https://img.shields.io/badge/CMake-Supported-blue.svg)](https://cmake.org/)
[![IDF Component](https://img.shields.io/badge/IDF-Component-blue.svg)](https://github.com/pschatzmann/TinyJPEG)
[![License: FreeBSD](https://img.shields.io/badge/License-FreeBSD-green.svg)](https://www.freebsd.org/copyright/freebsd-license/)

A header-only C++ port of [Bodmer's
TJpg_Decoder](https://github.com/Bodmer/TJpg_Decoder) - a baseline
(non-progressive) JPEG decoder built on [ChaN's
TJpgDec](http://elm-chan.org/fsw/tjpgd/00index.html), for microcontrollers
such as the ESP32 with no external library dependencies. Decodes straight
into caller-supplied storage one MCU block at a time via a callback
(typically pushed straight to a TFT display), rather than requiring memory
for a whole decoded frame at once - so decode workspace is a small, fixed
~3.5KB regardless of image size.

This is a **conversion**, not a rewrite: the actual decompressor
(bitstream/Huffman/IDCT/YCbCr code) is ChaN's proven TJpgDec engine,
merged from its original two-file split into one header-only file with no
algorithmic changes. What changed is everything *around* it - see
[Architecture](docs/architecture.md) for the full rationale and an
API-mapping table if you're migrating an existing TJpg_Decoder sketch:

- **Header-only.** No `.cpp` to compile or add to a build - `#include
  <TinyJPEGDecoder.h>` and go.
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
- **CMake/ESP-IDF support** alongside the Arduino Library Manager, via an
  `INTERFACE` target - see [Testing](docs/testing.md).

Baseline JPEG only (as TJpg_Decoder itself is) - progressive JPEGs are
rejected with `JDR_FMT3`, not silently mis-decoded, the same restriction
ChaN's TJpgDec has always had (more memory would be needed to buffer a
progressive scan's multiple passes).

## Performance

Measured (not estimated) on a 64x48 test image, x86 desktop (`-O2`, see
`test/native/bench_native.cpp`):

| Image | Decode time (avg / min / max) | fps |
|---|---|---|
| `gradient_444.jpg` (4:4:4, smooth gradient) | 85.8 / 60.0 / 700.3 us | ~11,655 |
| `checker_420.jpg` (4:2:0, checkerboard) | 47.6 / 40.7 / 182.6 us | ~20,987 |

Build/run it yourself: `cmake --build build --target bench_native &&
cd test/native && ../build/test/native/bench_native` (see
[Testing](docs/testing.md)). Real embedded-hardware numbers (ESP32,
STM32, ...) are TBD until measured - no fabricated benchmark figures here,
same rule [TinyH264](https://github.com/pschatzmann/TinyH264) and
[TinyMPG](https://github.com/pschatzmann/TinyMPG) follow for their own
performance sections.

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
  exact file-by-file mapping from TJpg_Decoder/TJpgDec, and the API
  changes (no singleton, no SD/SPIFFS-specific methods) with a full
  before/after table.
- [Decoding](docs/decoding.md) - `TinyJPEGDecoder` usage: in-memory
  buffers, file-like sources (SD/LittleFS/SPIFFS or any conforming
  `FileT`), custom streaming callbacks, and every compile-time
  configuration option (`JD_FORMAT`, `JD_FASTDECODE`, ...).
- [Testing](docs/testing.md) - running the native CMake/CTest suite, how
  the test assets are generated, and why the pixel-comparison tests are
  tolerance-based rather than byte-exact.

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
build, and only in the examples - the decoder core in `src/` never
includes it).

For CMake or ESP-IDF projects instead, see [Testing](docs/testing.md) for
`add_subdirectory()`/`EXTRA_COMPONENT_DIRS` usage.

## License

Three licenses apply, one per layer of authorship - ChaN's own permissive
TJpgDec license for `src/tjpgd.h`, and the FreeBSD License for both
Bodmer's original TJpg_Decoder API design and everything new in this port
(`src/TinyJPEGDecoder.h`, the build files, tests, examples, and docs) -
see [LICENSE](LICENSE) for the complete text of each.
