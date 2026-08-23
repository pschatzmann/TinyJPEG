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
  tjpgd_config.h      - JD_* compile-time configuration (was tjpgdcnf.h)
  tjpgd.h             - ChaN's TJpgDec engine (was tjpgd.h + tjpgd.c)
  TinyJPEGDecoder.h    - the public wrapper class (was TJpg_Decoder.h/.cpp)
test/native/           - desktop ctest suite - see testing.md
examples/              - Arduino .ino sketches
docs/                  - this file, decoding.md, testing.md
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
