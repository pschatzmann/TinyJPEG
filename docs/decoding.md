# Decoding

`TinyJPEGDecoder` (`src/TinyJPEGDecoder.h`, namespace `tinyjpeg`) decodes a
baseline (non-progressive) JPEG one MCU block at a time via a callback,
rather than requiring memory for a whole decoded frame at once - the same
strategy [TJpg_Decoder](https://github.com/Bodmer/TJpg_Decoder) itself
uses. It never allocates on the heap: the Huffman/quantizer tables, MCU
buffer, and IDCT scratch space TJpgDec needs all live in one fixed-size
member array (`TJPGD_WORKSPACE_SIZE` bytes, ~3.5KB with the default
`JD_FASTDECODE=1` - see `src/tjpgd_config.h`).

## Basic usage (in-memory buffer)

```cpp
#include <TinyJPEGDecoder.h>
using namespace tinyjpeg;

bool onBlock(TinyJPEGDecoder& decoder, int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  TFT_eSPI* tft = static_cast<TFT_eSPI*>(decoder.getUserData());
  tft->pushImage(x, y, w, h, bitmap);  // or however your display draws a block
  return true;  // false aborts the decode early (drawJpg() then returns JDR_INTR)
}

TinyJPEGDecoder decoder;
decoder.setUserData(&tft);  // reachable from onBlock() above via decoder.getUserData()
decoder.setCallback(onBlock);
JRESULT result = decoder.drawJpg(0, 0, jpegData, jpegSize);
if (result != JDR_OK) {
  // see JRESULT in tjpgd.h for the full error list (JDR_INTR, JDR_INP,
  // JDR_MEM1/2, JDR_PAR, JDR_FMT1/2/3)
}
```

`decoder` is always the same instance drawJpg()/getJpgSize() was called
on - useful once more than one `TinyJPEGDecoder` exists (e.g. one per
display), so a single callback function can still tell which decode it's
being called for. `setUserData()`/`getUserData()` are a plain, untouched
`void*` this class carries purely as a convenience for exactly that -
store whatever the callback needs (a display object, an output buffer, a
struct bundling several) and retrieve it with `decoder.getUserData()`
instead of a global/static variable.

`jpegData` must be plain RAM-addressable memory, not `PROGMEM` - this
decoder reads via ordinary pointer + `memcpy`, exactly like TJpg_Decoder's
own array path (which uses `memcpy_P` specifically so it *can* read
`PROGMEM` on AVR; TinyJPEGDecoder's core is platform-independent C++, so
if you're on a platform where flash data isn't plain-pointer-readable,
copy it to a RAM buffer first, the same as any other non-AVR-specific
code would).

Just the dimensions, without decoding any pixels:

```cpp
uint16_t w, h;
JRESULT result = decoder.getJpgSize(&w, &h, jpegData, jpegSize);
```

## Decoding from a file (SD, LittleFS, SPIFFS, or anything file-shaped)

`drawJpg`/`getJpgSize` also take a template `FileT&` - any type exposing:

```cpp
size_t available();               // bytes remaining to read
size_t read(uint8_t* buf, size_t len);   // read up to len bytes, return how many
size_t position();                 // current read offset
bool   seek(size_t pos);            // absolute seek
```

Arduino's `fs::File` (SPIFFS/LittleFS) and SD's `File` both already
satisfy this - open the file yourself and pass the handle:

```cpp
File f = SD.open("/image.jpg", FILE_READ);
JRESULT result = decoder.drawJpg(0, 0, f);
f.close();
```

```cpp
fs::File f = LittleFS.open("/image.jpg", "r");
JRESULT result = decoder.drawJpg(0, 0, f);
f.close();
```

This is a template, not a hardwired call into `SD.h`/`FS.h` - this header
never `#include`s either, so it works identically for any conforming type,
Arduino or not (see `test/native/test_file_source.cpp` for a plain stdio
`FILE*` wrapper used exactly this way in the desktop test suite).
`getJpgSize(w, h, file)` reads just the header, the same way.

`file` is read from its current position and left wherever the decode
happened to stop; this class never opens, closes, or rewinds it.

## Decoding from a custom source (network, ring buffer, ...)

For a source that isn't file-shaped, use the plain-callback overload
instead:

```cpp
size_t readFromSocket(void* userData, uint8_t* buf, size_t len) {
  MySocket* sock = static_cast<MySocket*>(userData);
  if (!buf) { sock->skip(len); return len; }  // null buf: discard len bytes
  return sock->readExactly(buf, len);
}

JRESULT result = decoder.drawJpg(0, 0, &readFromSocket, &mySocket);
```

**Contract**: `read` must always return exactly `min(len, bytes actually
available)` - never claim to have supplied more than it copied, but also
never deliberately supply less than `len` while more data remains
available. This matters because tjpgd itself relies on it for JPEG header
segments (SOF0/DHT/DQT/DRI/SOS): a call that returns less than the full
`len` it asked for there is treated as a hard stream error (`JDR_INP`),
identically to a genuinely truncated file - it does *not* mean "try again
for the rest." (The one place a short answer is tolerated is refilling the
internal entropy-coded-scan buffer, `JD_SZBUF` - 512 - bytes at a time;
that's an internal implementation detail of `tjpgd.h`, not something a
caller's `read` needs to special-case.) In short: this is a "block until
satisfied" contract, the same one a synchronous file read naturally
satisfies - a source prone to genuine short reads (a raw non-blocking
socket) needs to do its own buffering/retrying underneath this callback.
See `test/native/test_callback_source.cpp` for a working example driven
through many small reads.

`buf == nullptr` means "skip `len` bytes without delivering them" - tjpgd
uses this to discard JPEG segments it doesn't need (EXIF, comments, ...)
without allocating anywhere to put them.

`getJpgSize(w, h, read, userData)` uses the same callback to read just the
header.

## Options

- `setJpgScale(n)` - output reduction factor: 1 (full size), 2, 4, or 8.
  Any other value is treated as 1.
- `setSwapBytes(swap)` - swaps the high/low byte of each RGB565 output
  pixel. Most SPI TFT displays expect big-endian pixel words; set this to
  `true` if colors come out with red/blue swapped.
- `setCallback(cb)` - registers the per-block callback (`SketchCallback`:
  `bool(TinyJPEGDecoder& decoder, int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* data)`).
  `data` points to `w * h` pixels in whatever `JD_FORMAT` is configured to
  (RGB565 `uint16_t` by default - see `src/tjpgd_config.h`; switching to
  RGB888 or grayscale changes the bitmap's actual byte layout, so
  reinterpret the pointer accordingly if you do).
- `setUserData(ptr)` / `getUserData()` - an arbitrary caller-owned pointer
  carried on the instance, untouched by this class otherwise; retrieve it
  from inside the callback via its `decoder` parameter. `nullptr` until set.

## Compile-time configuration (`src/tjpgd_config.h`)

Override any of these with `-D` or a `#define` before the *first*
`#include` of this library anywhere in the build (the standard
header-only-library override idiom):

- `JD_FORMAT` (default `1`, RGB565) - `0` for RGB888, `2` for 8-bit
  grayscale.
- `JD_SZBUF` (default `512`) - stream input buffer size.
- `JD_USE_SCALE` (default `1`) - enables `setJpgScale()`'s 1/2/4/8
  reduction; `0` disables it (and its small runtime cost) entirely.
- `JD_FASTDECODE` (default `1`) - `0` for the smallest workspace (~3.1KB)
  on 8/16-bit MCUs, `1` (default) for a 32-bit barrel shifter (~3.5KB),
  `2` for an additional Huffman lookup table (~9.6KB) trading RAM for
  speed.
- `JD_TBLCLIP` (default `0`) - `1` swaps a branchy clamp for a 1KB lookup
  table.

All five are unchanged from ChaN's own TJpgDec defaults/semantics - see
`src/tjpgd_config.h`'s comments for the full description of each.
