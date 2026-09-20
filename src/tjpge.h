#pragma once
/*----------------------------------------------------------------------------/
/ tjpge.h - TinyJPEG Encoder engine (baseline, non-progressive).
/
/ Clean-room counterpart to tjpgd.h: same shape (a plain-C-struct object +
/ free functions, internals in a `_detail` namespace, header-only/inline so
/ one definition survives across translation units), opposite direction
/ (RGB888/grayscale pixels in, a baseline JPEG byte stream out).
/
/ What's shared with the decoder, and what isn't, is explained in
/ jpeg_common.h's own header comment - short version: the zigzag table and
/ the byte clamp are shared (direction-agnostic); the Huffman tables, the
/ DCT and the color-conversion constants are not (the encoder needs the
/ *forward* transform and the *fixed* Annex-K Huffman tables below, not the
/ decoder's DHT-driven decode tables or inverse DCT).
/
/ Input contract: je_start()/je_write_rows()/je_finish() stream pixel rows
/ in (row-major, in whichever JEPixelFormat je_prepare() was given - RGB888,
/ RGB565, RGB666, or 8-bit grayscale, see that enum's own comment for the
/ exact byte layout of each), rather than requiring a whole frame in RAM
/ at once. Internally, only one MCU-row band (8 source rows unsubsampled,
/ 16 for 4:2:0 color - je->mcuRows) is ever buffered at a time, in
/ je->rowbuf (pool-allocated at je_start() once width/format/subsample are
/ all known): je_write_rows() copies rows into that band buffer and
/ encodes+discards it the moment it fills, so peak pixel memory is
/ `width * bytesPerPixel * mcuRows`, not `width * height * bytesPerPixel`.
/ A full frame's worth of *lookahead* was never avoidable - chroma
/ subsampling and the forward DCT both need to see more than one row at a
/ time - but a full *frame* always was; row-band buffering is the
/ smallest amount of lookahead that still works. je_encode(pixels) is a
/ thin convenience wrapper (je_start + one je_write_rows(pixels, height)
/ call + je_finish) for callers that already have - or don't mind
/ needing - the whole frame in memory; it changes nothing about how those
/ callers use this engine, it's just no longer the only option.
/
/ Performance note: the forward DCT is a direct (O(N^2) separable)
/ algorithm, not the fast AAN/Arai butterfly network the decoder's
/ block_idct() uses for the inverse transform - correctness and a
/ straightforward derivation from the JPEG spec's own FDCT definition
/ (Annex A) were prioritized over algorithmic speed. It exists in two
/ implementations, though: forward_dct() (float) and forward_dct_fixed()
/ (int64_t Q12 fixed-point, same algorithm and basis table, no float/FPU
/ instructions at all) - encode_block() picks between them via the
/ JE_FIXED_POINT_DCT macro (tjpge_config.h), which auto-detects "does
/ this target have a hardware FPU" per architecture family. Which one is
/ faster is genuinely platform-dependent, not a settled question with one
/ right answer: measured faster with float on every FPU-equipped target
/ tried (desktop x86, ESP32-S3, and by extension any other target with a
/ hardware FPU), but ~2.2-2.6x faster with fixed-point on the FPU-less
/ RP2040 - see docs/performance.md's "Float vs. fixed-point" section for
/ the full measurements and exactly how the auto-detection works.
/----------------------------------------------------------------------------*/

#include "tjpge_config.h"
#include "jpeg_common.h"
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cmath>

/* Error code (mirrors JRESULT's shape; distinct type since the failure
   modes differ - e.g. there is no "input" to malform, but there is an
   output function that can refuse a write). */
typedef enum {
  JER_OK = 0,   /* 0: Succeeded */
  JER_INTR,     /* 1: Interrupted by output function (refused/short write) */
  JER_PAR,      /* 2: Parameter error (bad width/height/quality/component count) */
  JER_MEM1,     /* 3: Insufficient memory pool for the encoder workspace */
} JERESULT;

/* Source pixel row layout je_write_rows()/je_encode()'s pixel data is read as -
   set at je_prepare() time, since it determines both the component count
   (JENC::ncomp) and how get_rgb()/get_gray() (tjpge.h) index into the
   buffer.
     JE_FMT_RGB888 - 3 bytes/pixel, interleaved R,G,B (0-255 each).
     JE_FMT_RGB565 - 2 bytes/pixel, packed 5-6-5 into a native-endian
       uint16_t (byte [0]=low, [1]=high - the same in-memory layout as a
       `uint16_t*` framebuffer array on a little-endian MCU, e.g. most TFT
       driver back buffers; NOT necessarily the wire/SPI byte order some
       displays want, which is a display-driver-side concern this encoder
       has no part in - see docs/encoding.md).
     JE_FMT_GRAY8 - 1 byte/pixel, 8-bit grayscale.
     JE_FMT_RGB666 - 3 bytes/pixel, interleaved R,G,B, same as
       JE_FMT_RGB888 but only the top 6 bits of each byte are meaningful
       (the low 2 bits are don't-care, whatever the source left there).
       Handled byte-identically to JE_FMT_RGB888 - get_rgb() reads all 8
       bits of each byte either way, so a don't-care low 2 bits costs
       nothing to support and needs no separate unpacking path; this is a
       distinct enum value purely so a caller's own intent (and the
       precision it's actually getting) is explicit at the call site
       rather than silently passing 18-bit data off as JE_FMT_RGB888. */
typedef enum {
  JE_FMT_RGB888 = 0,
  JE_FMT_RGB565 = 1,
  JE_FMT_GRAY8 = 2,
  JE_FMT_RGB666 = 3,
} JEPixelFormat;

/* Encoder object structure (mirrors JDEC's role: one struct carries all
   session state, pool-allocated from a caller-supplied workspace so the
   encoder never touches the heap). */
typedef struct JENC JENC;
struct JENC {
  uint16_t width, height;      /* Size of the source image (pixel) */
  uint8_t ncomp;                /* Number of color components: 1 grayscale, 3 color - derived from pixfmt at je_prepare() */
  uint8_t pixfmt;                /* JEPixelFormat: layout of je_write_rows()/je_encode()'s pixel rows */
  uint8_t subsample;             /* 0: 4:4:4, 1: 4:2:0 (ignored when ncomp==1) - see JE_SUBSAMPLE */
  uint8_t quality;               /* 1-100, libjpeg-style scale */
  int32_t* qttbl[2];             /* Quantization tables [0]=luma, [1]=chroma, raster-order, scaled by quality */

  /* Huffman encode tables: direct lookup by symbol value -> (code, code
     length in bits). [0]=luma, [1]=chroma. DC symbols are categories
     0..11 (size 12 each); AC symbols are RRRRSSSS bytes 0..255 (size 256
     each, though only the ~162 standard combinations are ever populated). */
  uint16_t* dcCode[2]; uint8_t* dcLen[2];
  uint16_t* acCode[2]; uint8_t* acLen[2];

  int16_t dcv[3];                /* Previous DC value per component (0=Y, 1=Cb, 2=Cr), for DC differential coding */

  uint8_t* outbuf;                /* Output stream buffer, JE_SZBUF bytes */
  size_t outpos;                   /* Bytes currently buffered in outbuf */
  uint32_t bitbuf;                 /* Bit-packing accumulator, left-justified */
  uint8_t bitcnt;                  /* Number of valid bits currently in bitbuf */

  /* Row-band streaming state (je_start()/je_write_rows()/je_finish()) -
     see this file's own header comment. rowbuf holds exactly mcuRows rows
     (8, or 16 for 4:2:0 color) of `width` pixels each, in je->pixfmt's
     byte layout; rowsBuffered is how many of those rows are currently
     valid (0..mcuRows) in the band being assembled; rowsWritten is the
     running total across the whole image, used to detect a caller
     supplying too many/too few rows. */
  uint8_t* rowbuf;
  uint16_t mcuRows;
  uint16_t rowsBuffered;
  uint32_t rowsWritten;
  uint8_t started;                /* je_start() has run */
  uint8_t finished;                /* je_finish() has run - further je_write_rows() calls are rejected */

  void* pool;                     /* Pointer to available memory pool */
  size_t sz_pool;                  /* Size of memory pool (bytes available) */
  size_t (*outfunc)(JENC*, const uint8_t*, size_t); /* Stream output function - returns bytes accepted; short write aborts the encode */
  void* device;                    /* Caller-owned I/O device identifier for the session (mirrors JDEC::device) */
};

/* TinyJPEG Encoder API functions */
JERESULT je_prepare(JENC* je, size_t (*outfunc)(JENC*, const uint8_t*, size_t), void* pool, size_t sz_pool, void* dev,
                     uint16_t width, uint16_t height, JEPixelFormat format);

/* Streaming API: writes JPEG headers, allocates the row-band buffer (from
   the same pool je_prepare() was given), and readies je for
   je_write_rows(). Call after je_prepare() and after setting je->quality/
   je->subsample if you're overriding their JE_QUALITY/JE_SUBSAMPLE
   defaults - the row-band buffer's size depends on je->subsample, so it
   must be finalized first. */
JERESULT je_start(JENC* je);

/* Streaming API: feeds `numRows` more source rows (row-major, in
   je->pixfmt's layout, `numRows * width * bytesPerPixel(pixfmt)` bytes)
   into the encoder. May be called any number of times with any number of
   rows each (1 at a time, the whole image at once, or anything between);
   internally slices the input into je->mcuRows-row bands and encodes+
   discards each band as soon as it's full, so peak buffered pixel memory
   never exceeds one band regardless of how `rows` is chunked. Returns
   JER_PAR if called before je_start(), after je_finish(), or with more
   rows total than je->height. */
JERESULT je_write_rows(JENC* je, const uint8_t* rows, uint16_t numRows);

/* Streaming API: encodes the final (possibly partial) row band if one is
   pending - padding it by replicating its last row, the same edge-
   replication je_write_rows()'s full bands get from get_rgb()/get_gray()
   at the right edge - then writes EOI and flushes. Returns JER_PAR if
   je_write_rows() hasn't yet been fed exactly je->height rows in total. */
JERESULT je_finish(JENC* je);

/* Convenience wrapper: je_start() + one je_write_rows(pixels, je->height)
   call + je_finish() - encodes one full in-memory frame. Behaves exactly
   as before the streaming API existed; use je_start()/je_write_rows()/
   je_finish() directly instead when the whole frame isn't (or shouldn't
   need to be) in memory at once - see this file's own header comment. */
JERESULT je_encode(JENC* je, const uint8_t* pixels);

namespace tjpge_detail {

/*-----------------------------------------------------------------------*/
/* Standard (Annex K) quantization tables, natural (row-major) order.    */
/* Scaled by quality and zigzagged into JENC::qttbl by create_qt_tbl().  */
/*-----------------------------------------------------------------------*/

inline constexpr uint8_t StdLumaQT[64] = {
  16, 11, 10, 16, 24, 40, 51, 61,
  12, 12, 14, 19, 26, 58, 60, 55,
  14, 13, 16, 24, 40, 57, 69, 56,
  14, 17, 22, 29, 51, 87, 80, 62,
  18, 22, 37, 56, 68, 109, 103, 77,
  24, 35, 55, 64, 81, 104, 113, 92,
  49, 64, 78, 87, 103, 121, 120, 101,
  72, 92, 95, 98, 112, 100, 103, 99
};

inline constexpr uint8_t StdChromaQT[64] = {
  17, 18, 24, 47, 99, 99, 99, 99,
  18, 21, 26, 66, 99, 99, 99, 99,
  24, 26, 56, 99, 99, 99, 99, 99,
  47, 66, 99, 99, 99, 99, 99, 99,
  99, 99, 99, 99, 99, 99, 99, 99,
  99, 99, 99, 99, 99, 99, 99, 99,
  99, 99, 99, 99, 99, 99, 99, 99,
  99, 99, 99, 99, 99, 99, 99, 99
};

/*-----------------------------------------------------------------------*/
/* Standard (Annex K) Huffman tables: bit-length distribution (16        */
/* entries: count of codes of length 1..16) + the symbols in code order. */
/* Fixed by the spec - unlike the decoder, which reads these from each   */
/* file's own DHT segment, the encoder always packs against these.      */
/*-----------------------------------------------------------------------*/

inline constexpr uint8_t StdDcLumaBits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
inline constexpr uint8_t StdDcLumaVals[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

inline constexpr uint8_t StdDcChromaBits[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
inline constexpr uint8_t StdDcChromaVals[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

inline constexpr uint8_t StdAcLumaBits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
inline constexpr uint8_t StdAcLumaVals[162] = {
  0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
  0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
  0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
  0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
  0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
  0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
  0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
  0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
  0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
  0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
  0xf9, 0xfa
};

inline constexpr uint8_t StdAcChromaBits[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
inline constexpr uint8_t StdAcChromaVals[162] = {
  0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
  0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
  0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
  0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
  0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
  0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
  0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
  0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
  0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
  0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
  0xf9, 0xfa
};

/*-----------------------------------------------------------------------*/
/* Forward DCT basis: CosTbl[x][u] = cos((2x+1)*u*pi/16), Cu[u] is the    */
/* JPEG FDCT definition's C(u) (1/sqrt(2) for u==0, else 1) - see Annex  */
/* A. Values are the well-known DCT-II basis constants, computed once    */
/* and stored as literals rather than calling cos() at runtime.         */
/*-----------------------------------------------------------------------*/

inline constexpr float CosTbl[8][8] = {
    {1.00000000f, 0.98078528f, 0.92387953f, 0.83146961f, 0.70710678f, 0.55557023f, 0.38268343f, 0.19509032f},
    {1.00000000f, 0.83146961f, 0.38268343f, -0.19509032f, -0.70710678f, -0.98078528f, -0.92387953f, -0.55557023f},
    {1.00000000f, 0.55557023f, -0.38268343f, -0.98078528f, -0.70710678f, 0.19509032f, 0.92387953f, 0.83146961f},
    {1.00000000f, 0.19509032f, -0.92387953f, -0.55557023f, 0.70710678f, 0.83146961f, -0.38268343f, -0.98078528f},
    {1.00000000f, -0.19509032f, -0.92387953f, 0.55557023f, 0.70710678f, -0.83146961f, -0.38268343f, 0.98078528f},
    {1.00000000f, -0.55557023f, -0.38268343f, 0.98078528f, -0.70710678f, -0.19509032f, 0.92387953f, -0.83146961f},
    {1.00000000f, -0.83146961f, 0.38268343f, 0.19509032f, -0.70710678f, 0.98078528f, -0.92387953f, 0.55557023f},
    {1.00000000f, -0.98078528f, 0.92387953f, -0.83146961f, 0.70710678f, -0.55557023f, 0.38268343f, -0.19509032f},
};

inline constexpr float Cu[8] = {0.70710678f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};

/*-----------------------------------------------------------------------*/
/* Same basis, Q12 fixed-point (values * 4096, rounded) - for            */
/* forward_dct_fixed() below. Not used by forward_dct() itself; kept     */
/* alongside it purely so both DCT implementations' basis tables sit     */
/* next to each other for comparison.                                    */
/*-----------------------------------------------------------------------*/

inline constexpr int32_t CosTblFixed[8][8] = {
    {4096, 4017, 3784, 3406, 2896, 2276, 1567, 799},
    {4096, 3406, 1567, -799, -2896, -4017, -3784, -2276},
    {4096, 2276, -1567, -4017, -2896, 799, 3784, 3406},
    {4096, 799, -3784, -2276, 2896, 3406, -1567, -4017},
    {4096, -799, -3784, 2276, 2896, -3406, -1567, 4017},
    {4096, -2276, -1567, 4017, -2896, -799, 3784, -3406},
    {4096, -3406, 1567, 799, -2896, 4017, -3784, 2276},
    {4096, -4017, 3784, -3406, 2896, -2276, 1567, -799},
};

inline constexpr int32_t CuFixed[8] = {2896, 4096, 4096, 4096, 4096, 4096, 4096, 4096};

/*-----------------------------------------------------------------------*/
/* Allocate a memory block from memory pool (mirrors tjpgd_detail's own) */
/*-----------------------------------------------------------------------*/

inline void* alloc_pool(JENC* je, size_t ndata) {
  char* rp = 0;
  ndata = (ndata + 3) & ~3; /* Align block size to the word boundary */
  if (je->sz_pool >= ndata) {
    je->sz_pool -= ndata;
    rp = (char*)je->pool;
    je->pool = (void*)(rp + ndata);
  }
  return (void*)rp;
}

/*-----------------------------------------------------------------------*/
/* Build a quality-scaled, zigzag-ordered quantization table from one of */
/* the Annex K base tables above (mirrors tjpgd_detail::create_qt_tbl's  */
/* zigzag re-ordering, opposite direction: no Arai prescale, since the   */
/* forward DCT this feeds isn't the decoder's Arai-scaled inverse one).  */
/*-----------------------------------------------------------------------*/

inline JERESULT create_qt_tbl(JENC* je, const uint8_t* base, unsigned int id) {
  /* Standard libjpeg quality->scale mapping. */
  unsigned int quality = je->quality < 1 ? 1 : (je->quality > 100 ? 100 : je->quality);
  unsigned int scale = quality < 50 ? (5000 / quality) : (200 - quality * 2);

  int32_t* pb = (int32_t*)alloc_pool(je, 64 * sizeof(int32_t));
  if (!pb) return JER_MEM1;
  je->qttbl[id] = pb;

  for (unsigned int i = 0; i < 64; i++) {
    unsigned int zi = jpeg_common::Zig[i];
    int32_t v = ((int32_t)base[i] * (int32_t)scale + 50) / 100;
    if (v < 1) v = 1;
    if (v > 255) v = 255;
    pb[zi] = v; /* raster-order slot zi holds the coefficient for zigzag position i */
  }
  return JER_OK;
}

/*-----------------------------------------------------------------------*/
/* Build a symbol -> (code, length) lookup table from a bit-length       */
/* distribution and a symbol list, per JPEG Annex C's code assignment    */
/* algorithm (same canonical-code construction the decoder's own         */
/* create_huffman_tbl uses to rebuild its code-word table - here the     */
/* result is indexed by symbol value instead, since the encoder looks up */
/* "what code does this symbol get" rather than "what symbol does this   */
/* code mean").                                                          */
/*-----------------------------------------------------------------------*/

inline JERESULT build_huff_lut(JENC* je, const uint8_t bits[16], const uint8_t* vals, size_t nvals, size_t lutsize,
                                uint16_t** codeOut, uint8_t** lenOut) {
  uint16_t* code = (uint16_t*)alloc_pool(je, lutsize * sizeof(uint16_t));
  uint8_t* len = (uint8_t*)alloc_pool(je, lutsize * sizeof(uint8_t));
  if (!code || !len) return JER_MEM1;
  memset(code, 0, lutsize * sizeof(uint16_t));
  memset(len, 0, lutsize * sizeof(uint8_t));

  uint16_t c = 0;
  size_t k = 0;
  for (unsigned int bl = 1; bl <= 16; bl++) {
    for (unsigned int n = bits[bl - 1]; n; n--) {
      uint8_t sym = vals[k++];
      code[sym] = c++;
      len[sym] = (uint8_t)bl;
    }
    c <<= 1;
  }
  (void)nvals; /* only used for the assertion-shaped comment above; k==nvals on exit */

  *codeOut = code;
  *lenOut = len;
  return JER_OK;
}

/*-----------------------------------------------------------------------*/
/* Output stream: buffer bytes into je->outbuf, flush to je->outfunc     */
/* when full or at end of encode.                                        */
/*-----------------------------------------------------------------------*/

inline JERESULT flush_outbuf(JENC* je) {
  if (je->outpos) {
    size_t n = je->outfunc(je, je->outbuf, je->outpos);
    if (n != je->outpos) return JER_INTR;
    je->outpos = 0;
  }
  return JER_OK;
}

inline JERESULT put_byte(JENC* je, uint8_t b) {
  je->outbuf[je->outpos++] = b;
  if (je->outpos >= JE_SZBUF) return flush_outbuf(je);
  return JER_OK;
}

inline JERESULT put_u16be(JENC* je, uint16_t v) {
  JERESULT rc = put_byte(je, (uint8_t)(v >> 8));
  if (rc != JER_OK) return rc;
  return put_byte(je, (uint8_t)(v & 0xFF));
}

/* A byte belonging to the entropy-coded (bit-packed) segment: 0xFF must
   be followed by a stuffed 0x00 so the decoder doesn't mistake it for a
   marker (see tjpgd.h's huffext()/bitext(), which undo exactly this). */
inline JERESULT put_byte_stuffed(JENC* je, uint8_t b) {
  JERESULT rc = put_byte(je, b);
  if (rc != JER_OK) return rc;
  if (b == 0xFF) return put_byte(je, 0x00);
  return JER_OK;
}

/* Pack `size` low bits of `code` into the bitstream, MSB-first, draining
   whole bytes (with stuffing) as they accumulate. `size` must be 0..16. */
inline JERESULT put_bits(JENC* je, uint32_t code, unsigned int size) {
  if (size == 0) return JER_OK;
  je->bitcnt += (uint8_t)size;
  je->bitbuf |= (code & ((1UL << size) - 1)) << (32 - je->bitcnt);
  while (je->bitcnt >= 8) {
    uint8_t byte = (uint8_t)(je->bitbuf >> 24);
    JERESULT rc = put_byte_stuffed(je, byte);
    if (rc != JER_OK) return rc;
    je->bitbuf <<= 8;
    je->bitcnt -= 8;
  }
  return JER_OK;
}

/* Pad the tail of the entropy-coded segment to a byte boundary with 1
   bits, per spec, so the following marker (RST/EOI) starts byte-aligned. */
inline JERESULT flush_bits(JENC* je) {
  unsigned int pad = (unsigned int)((8 - (je->bitcnt % 8)) % 8);
  if (pad) return put_bits(je, (1UL << pad) - 1, pad);
  return JER_OK;
}

/*-----------------------------------------------------------------------*/
/* Number of bits needed to represent |v| (0 for v==0) - the JPEG        */
/* "category"/"size" of a DC diff or AC coefficient.                     */
/*-----------------------------------------------------------------------*/

inline unsigned int bitsize(int v) {
  unsigned int a = (unsigned int)(v < 0 ? -v : v);
  unsigned int s = 0;
  while (a) {
    s++;
    a >>= 1;
  }
  return s;
}

/* The "additional bits" JPEG packs after a DC/AC size code: v itself if
   v>=0 (whose top bit is then 1), or v + (2^size - 1) if v<0 (whose top
   bit is then 0) - see tjpgd.h's mcu_load() for the matching decode-side
   `if (!(e & bc)) e -= (bc << 1) - 1;` this inverts. */
inline uint32_t magnitude_bits(int v, unsigned int size) {
  return (uint32_t)(v >= 0 ? v : v + (int)((1UL << size) - 1));
}

/*-----------------------------------------------------------------------*/
/* Forward DCT: separable, direct O(N^2) per dimension, per the FDCT     */
/* definition in JPEG spec Annex A. `src` is 64 raster-order 8-bit       */
/* samples; `dst` is 64 raster-order (not zigzag) coefficients.         */
/*-----------------------------------------------------------------------*/

inline void forward_dct(const uint8_t* src, int32_t* dst) {
  float tmp[64];
  for (unsigned int y = 0; y < 8; y++) {
    for (unsigned int u = 0; u < 8; u++) {
      float sum = 0;
      for (unsigned int x = 0; x < 8; x++) {
        sum += ((float)src[y * 8 + x] - 128.0f) * CosTbl[x][u];
      }
      tmp[y * 8 + u] = sum * Cu[u];
    }
  }
  for (unsigned int u = 0; u < 8; u++) {
    for (unsigned int v = 0; v < 8; v++) {
      float sum = 0;
      for (unsigned int y = 0; y < 8; y++) {
        sum += tmp[y * 8 + u] * CosTbl[y][v];
      }
      dst[v * 8 + u] = (int32_t)lroundf(sum * Cu[v] * 0.25f);
    }
  }
}

/* Round-to-nearest (ties away from zero, matching lroundf()'s convention
   in forward_dct() above) arithmetic right shift - the fixed-point
   equivalent of dividing by 2^shift with proper rounding instead of
   truncation. */
inline int64_t round_shift(int64_t v, unsigned int shift) {
  int64_t half = (int64_t)1 << (shift - 1);
  return (v >= 0) ? (v + half) >> shift : -(((-v) + half) >> shift);
}

/*-----------------------------------------------------------------------*/
/* Forward DCT: bit-for-bit the same separable, direct O(N^2) algorithm  */
/* as forward_dct() above - same two passes, same basis table values -   */
/* just computed with Q12 fixed-point integers (CosTblFixed/CuFixed)     */
/* instead of float, accumulated in int64_t to avoid needing to reason   */
/* about intermediate overflow. Deliberately NOT the fast AAN/Arai       */
/* butterfly algorithm tjpgd_detail::block_idct() uses for the decoder's */
/* inverse transform - keeping the algorithm identical to forward_dct()  */
/* isolates "does fixed-point vs float integer math matter here" from    */
/* "does a different, faster algorithm matter here", which are separate  */
/* questions (see docs/performance.md's "Cross-platform notes" for the   */
/* measurements this function exists to produce).                       */
/*                                                                        */
/* Fixed-point bookkeeping (Q12 = values scaled by 4096):                */
/*   pass 1: sum1 = Sigma diff(Q0) * CosTblFixed(Q12)           -> Q12   */
/*           tmp  = round_shift(sum1 * CuFixed[u](Q12), 12)     -> Q12   */
/*   pass 2: sum2 = Sigma tmp(Q12) * CosTblFixed(Q12)           -> Q24   */
/*           val  = round_shift(sum2 * CuFixed[v](Q12), 12)     -> Q24   */
/*           dst  = round_shift(val, 26)  -- the extra 2 bits fold in    */
/*                  the *0.25 factor forward_dct() applies explicitly    */
/*                  (0.25 == 2^-2, so removing Q24 *and* dividing by 4   */
/*                  is one combined shift of 24+2=26).                  */
/*-----------------------------------------------------------------------*/

inline void forward_dct_fixed(const uint8_t* src, int32_t* dst) {
  int64_t tmp[64];
  for (unsigned int y = 0; y < 8; y++) {
    for (unsigned int u = 0; u < 8; u++) {
      int64_t sum = 0;
      for (unsigned int x = 0; x < 8; x++) {
        sum += (int64_t)((int)src[y * 8 + x] - 128) * CosTblFixed[x][u];
      }
      tmp[y * 8 + u] = round_shift(sum * CuFixed[u], 12);
    }
  }
  for (unsigned int u = 0; u < 8; u++) {
    for (unsigned int v = 0; v < 8; v++) {
      int64_t sum = 0;
      for (unsigned int y = 0; y < 8; y++) {
        sum += tmp[y * 8 + u] * CosTblFixed[y][v];
      }
      int64_t val = round_shift(sum * CuFixed[v], 12);
      dst[v * 8 + u] = (int32_t)round_shift(val, 26);
    }
  }
}

/*-----------------------------------------------------------------------*/
/* Quantize a raster-order coefficient block against qttbl (also         */
/* raster-order), producing zigzag-ordered, clamped-to-category-range    */
/* output ready for entropy coding. AC magnitudes are clamped to the     */
/* standard Annex-K AC table's category range (1..10, i.e. |v|<=1023);   */
/* DC to the DC table's (1..11, i.e. |v|<=2047) - both are spec-legal    */
/* clamps for pathological high-quality/high-contrast blocks that would  */
/* otherwise need a category the fixed standard tables don't define.     */
/*-----------------------------------------------------------------------*/

inline void quantize_zigzag(const int32_t* coef, const int32_t* qt, int32_t zz[64]) {
  for (unsigned int k = 0; k < 64; k++) {
    unsigned int r = jpeg_common::Zig[k];
    int32_t v = (int32_t)lroundf((float)coef[r] / (float)qt[r]);
    int32_t limit = (k == 0) ? 2047 : 1023;
    if (v > limit) v = limit;
    if (v < -limit) v = -limit;
    zz[k] = v;
  }
}

/*-----------------------------------------------------------------------*/
/* Entropy-encode one already-quantized, zigzag-ordered 8x8 block: DC    */
/* differential vs je->dcv[cmp], then run-length/size AC symbols against */
/* tbl (0:luma, 1:chroma Huffman tables), per JPEG spec section F.1.2.   */
/*-----------------------------------------------------------------------*/

inline JERESULT encode_coefs(JENC* je, const int32_t zz[64], unsigned int cmp, unsigned int tbl) {
  JERESULT rc;

  int diff = zz[0] - je->dcv[cmp];
  je->dcv[cmp] = (int16_t)zz[0];
  unsigned int dsize = bitsize(diff);
  rc = put_bits(je, je->dcCode[tbl][dsize], je->dcLen[tbl][dsize]);
  if (rc != JER_OK) return rc;
  if (dsize) {
    rc = put_bits(je, magnitude_bits(diff, dsize), dsize);
    if (rc != JER_OK) return rc;
  }

  unsigned int run = 0;
  for (unsigned int k = 1; k < 64; k++) {
    int32_t v = zz[k];
    if (v == 0) {
      run++;
      continue;
    }
    while (run > 15) {
      rc = put_bits(je, je->acCode[tbl][0xF0], je->acLen[tbl][0xF0]); /* ZRL */
      if (rc != JER_OK) return rc;
      run -= 16;
    }
    unsigned int asize = bitsize(v);
    unsigned int sym = (run << 4) | asize;
    rc = put_bits(je, je->acCode[tbl][sym], je->acLen[tbl][sym]);
    if (rc != JER_OK) return rc;
    rc = put_bits(je, magnitude_bits((int)v, asize), asize);
    if (rc != JER_OK) return rc;
    run = 0;
  }
  if (run) {
    rc = put_bits(je, je->acCode[tbl][0x00], je->acLen[tbl][0x00]); /* EOB */
    if (rc != JER_OK) return rc;
  }
  return JER_OK;
}

inline JERESULT encode_block(JENC* je, const uint8_t src[64], unsigned int cmp, unsigned int tbl) {
  int32_t coef[64], zz[64];
#if JE_FIXED_POINT_DCT
  forward_dct_fixed(src, coef);
#else
  forward_dct(src, coef);
#endif
  quantize_zigzag(coef, je->qttbl[tbl], zz);
  return encode_coefs(je, zz, cmp, tbl);
}

/*-----------------------------------------------------------------------*/
/* Bytes/pixel for a JEPixelFormat - the row-band buffer's stride and    */
/* je_write_rows()'s expected input size are both `width * this`.        */
/*-----------------------------------------------------------------------*/

inline unsigned int bytes_per_pixel(JEPixelFormat fmt) { return fmt == JE_FMT_RGB565 ? 2 : fmt == JE_FMT_GRAY8 ? 1 : 3; }

/*-----------------------------------------------------------------------*/
/* Pixel access into the current row band (je->rowbuf - see this file's  */
/* header comment), with edge replication so the last (possibly partial) */
/* column of MCUs at the right edge of an image whose width isn't a      */
/* multiple of the MCU size still has full 8x8 blocks to DCT - the       */
/* padding is discarded on decode since SOF0 carries the true width, the */
/* same way tjpgd.h's own mcu_output() clips its output rectangle        */
/* against jd->width. `y` is a row *within the current band* (0..        */
/* je->mcuRows-1), not an absolute image row - je_finish() pads the      */
/* band's own trailing rows by replication for the same reason, so this  */
/* function never needs to know how many rows of the final band were     */
/* "real" versus padding.                                                */
/*-----------------------------------------------------------------------*/

inline void get_rgb(JENC* je, int x, int y, uint8_t& r, uint8_t& g, uint8_t& b) {
  if (x >= je->width) x = je->width - 1;
  size_t idx = (size_t)y * je->width + x;

  if (je->pixfmt == JE_FMT_RGB565) {
    const uint8_t* p = je->rowbuf + idx * 2;
    uint16_t px = (uint16_t)(p[0] | (p[1] << 8)); /* native-endian, see JEPixelFormat's own comment */
    uint8_t r5 = (uint8_t)((px >> 11) & 0x1F);
    uint8_t g6 = (uint8_t)((px >> 5) & 0x3F);
    uint8_t b5 = (uint8_t)(px & 0x1F);
    r = (uint8_t)((r5 << 3) | (r5 >> 2)); /* bit-replicate 5/6 bits up to 8, same trick test_helpers.h's rgb565ToRgb888() uses on the decode side */
    g = (uint8_t)((g6 << 2) | (g6 >> 4));
    b = (uint8_t)((b5 << 3) | (b5 >> 2));
  } else { /* JE_FMT_RGB888 or JE_FMT_RGB666 - byte-identical layout, see JEPixelFormat's own comment */
    const uint8_t* p = je->rowbuf + idx * 3;
    r = p[0];
    g = p[1];
    b = p[2];
  }
}

inline uint8_t get_gray(JENC* je, int x, int y) {
  if (x >= je->width) x = je->width - 1;
  return je->rowbuf[(size_t)y * je->width + x];
}

inline void rgb_to_ycbcr(uint8_t r, uint8_t g, uint8_t b, uint8_t& y, uint8_t& cb, uint8_t& cr) {
  y = jpeg_common::byteclip((int)lroundf(0.299f * r + 0.587f * g + 0.114f * b));
  cb = jpeg_common::byteclip((int)lroundf(-0.168736f * r - 0.331264f * g + 0.5f * b + 128.0f));
  cr = jpeg_common::byteclip((int)lroundf(0.5f * r - 0.418688f * g - 0.081312f * b + 128.0f));
}

/*-----------------------------------------------------------------------*/
/* Encode one MCU from the current row band (je->rowbuf - see this      */
/* file's header comment; `mx` is the MCU's column start, the band's own */
/* rows are always 0..je->mcuRows-1). 4:4:4 (or grayscale): a single 8x8 */
/* Y block, plus 8x8 Cb/Cr blocks for color. 4:2:0: four 8x8 Y blocks    */
/* covering a 16x16 area (order TL,TR,BL,BR - matching tjpgd.h's         */
/* mcu_output() block layout for msx=msy=2, see its `py += 64`/          */
/* `iy >= 8` offsets), plus one 8x8 Cb/Cr block each, box-downsampled by */
/* averaging every 2x2 group.                                            */
/*-----------------------------------------------------------------------*/

inline JERESULT encode_mcu_444(JENC* je, unsigned int mx) {
  uint8_t yblk[64], cbblk[64], crblk[64];
  JERESULT rc;

  for (unsigned int py = 0; py < 8; py++) {
    for (unsigned int px = 0; px < 8; px++) {
      unsigned int i = py * 8 + px;
      if (je->ncomp == 1) {
        yblk[i] = get_gray(je, mx + px, py);
      } else {
        uint8_t r, g, b;
        get_rgb(je, mx + px, py, r, g, b);
        rgb_to_ycbcr(r, g, b, yblk[i], cbblk[i], crblk[i]);
      }
    }
  }

  rc = encode_block(je, yblk, 0, 0);
  if (rc != JER_OK) return rc;
  if (je->ncomp == 3) {
    rc = encode_block(je, cbblk, 1, 1);
    if (rc != JER_OK) return rc;
    rc = encode_block(je, crblk, 2, 1);
    if (rc != JER_OK) return rc;
  }
  return JER_OK;
}

inline JERESULT encode_mcu_420(JENC* je, unsigned int mx) {
  uint8_t yblk[4][64];
  int cbSum[64] = {0}, crSum[64] = {0};

  for (unsigned int py = 0; py < 16; py++) {
    for (unsigned int px = 0; px < 16; px++) {
      uint8_t r, g, b, y, cb, cr;
      get_rgb(je, mx + px, py, r, g, b);
      rgb_to_ycbcr(r, g, b, y, cb, cr);

      unsigned int blockIdx = (py / 8) * 2 + (px / 8);
      yblk[blockIdx][(py % 8) * 8 + (px % 8)] = y;

      unsigned int ci = (py / 2) * 8 + (px / 2);
      cbSum[ci] += cb;
      crSum[ci] += cr;
    }
  }

  uint8_t cbblk[64], crblk[64];
  for (unsigned int i = 0; i < 64; i++) {
    cbblk[i] = (uint8_t)((cbSum[i] + 2) / 4);
    crblk[i] = (uint8_t)((crSum[i] + 2) / 4);
  }

  JERESULT rc;
  for (unsigned int b = 0; b < 4; b++) {
    rc = encode_block(je, yblk[b], 0, 0);
    if (rc != JER_OK) return rc;
  }
  rc = encode_block(je, cbblk, 1, 1);
  if (rc != JER_OK) return rc;
  return encode_block(je, crblk, 2, 1);
}

/*-----------------------------------------------------------------------*/
/* Encode every MCU across the current full row band (je->rowbuf holds   */
/* exactly je->mcuRows valid rows by the time this is called - either a  */
/* full band from je_write_rows(), or a replication-padded final one     */
/* from je_finish()).                                                    */
/*-----------------------------------------------------------------------*/

inline JERESULT encode_band(JENC* je) {
  for (unsigned int mx = 0; mx < je->width; mx += je->mcuRows) {
    JERESULT rc = (je->mcuRows == 16) ? encode_mcu_420(je, mx) : encode_mcu_444(je, mx);
    if (rc != JER_OK) return rc;
  }
  return JER_OK;
}

/*-----------------------------------------------------------------------*/
/* Marker/segment writer: SOI, DQT, SOF0, DHT, SOS. Header bytes go      */
/* through put_byte()/put_u16be() (never put_bits()/put_byte_stuffed()): */
/* 0xFF byte-stuffing only applies inside the entropy-coded scan data.   */
/*-----------------------------------------------------------------------*/

inline JERESULT write_dqt(JENC* je, unsigned int id) {
  JERESULT rc = put_u16be(je, 0xFFDB);
  if (rc != JER_OK) return rc;
  rc = put_u16be(je, 2 + 1 + 64);
  if (rc != JER_OK) return rc;
  rc = put_byte(je, (uint8_t)id);
  if (rc != JER_OK) return rc;
  for (unsigned int k = 0; k < 64; k++) {
    rc = put_byte(je, (uint8_t)je->qttbl[id][jpeg_common::Zig[k]]);
    if (rc != JER_OK) return rc;
  }
  return JER_OK;
}

inline JERESULT write_dht(JENC* je, unsigned int cls, unsigned int id, const uint8_t bits[16], const uint8_t* vals,
                           size_t nvals) {
  JERESULT rc = put_u16be(je, 0xFFC4);
  if (rc != JER_OK) return rc;
  rc = put_u16be(je, (uint16_t)(2 + 1 + 16 + nvals));
  if (rc != JER_OK) return rc;
  rc = put_byte(je, (uint8_t)((cls << 4) | id));
  if (rc != JER_OK) return rc;
  for (unsigned int i = 0; i < 16; i++) {
    rc = put_byte(je, bits[i]);
    if (rc != JER_OK) return rc;
  }
  for (size_t i = 0; i < nvals; i++) {
    rc = put_byte(je, vals[i]);
    if (rc != JER_OK) return rc;
  }
  return JER_OK;
}

inline JERESULT write_headers(JENC* je) {
  JERESULT rc = put_u16be(je, 0xFFD8); /* SOI */
  if (rc != JER_OK) return rc;

  rc = write_dqt(je, 0);
  if (rc != JER_OK) return rc;
  if (je->ncomp == 3) {
    rc = write_dqt(je, 1);
    if (rc != JER_OK) return rc;
  }

  /* SOF0 */
  rc = put_u16be(je, 0xFFC0);
  if (rc != JER_OK) return rc;
  rc = put_u16be(je, (uint16_t)(8 + 3 * je->ncomp));
  if (rc != JER_OK) return rc;
  rc = put_byte(je, 8); /* precision */
  if (rc != JER_OK) return rc;
  rc = put_u16be(je, je->height);
  if (rc != JER_OK) return rc;
  rc = put_u16be(je, je->width);
  if (rc != JER_OK) return rc;
  rc = put_byte(je, je->ncomp);
  if (rc != JER_OK) return rc;
  if (je->ncomp == 3) {
    uint8_t ySamp = je->subsample ? 0x22 : 0x11;
    rc = put_byte(je, 1);
    if (rc != JER_OK) return rc; /* Y id */
    rc = put_byte(je, ySamp);
    if (rc != JER_OK) return rc;
    rc = put_byte(je, 0); /* Y quant table id */
    if (rc != JER_OK) return rc;
    for (uint8_t id = 2; id <= 3; id++) { /* Cb, Cr */
      rc = put_byte(je, id);
      if (rc != JER_OK) return rc;
      rc = put_byte(je, 0x11);
      if (rc != JER_OK) return rc;
      rc = put_byte(je, 1); /* chroma quant table id */
      if (rc != JER_OK) return rc;
    }
  } else {
    rc = put_byte(je, 1); /* Y id */
    if (rc != JER_OK) return rc;
    rc = put_byte(je, 0x11);
    if (rc != JER_OK) return rc;
    rc = put_byte(je, 0);
    if (rc != JER_OK) return rc;
  }

  rc = write_dht(je, 0, 0, StdDcLumaBits, StdDcLumaVals, 12);
  if (rc != JER_OK) return rc;
  rc = write_dht(je, 1, 0, StdAcLumaBits, StdAcLumaVals, 162);
  if (rc != JER_OK) return rc;
  if (je->ncomp == 3) {
    rc = write_dht(je, 0, 1, StdDcChromaBits, StdDcChromaVals, 12);
    if (rc != JER_OK) return rc;
    rc = write_dht(je, 1, 1, StdAcChromaBits, StdAcChromaVals, 162);
    if (rc != JER_OK) return rc;
  }

  /* SOS */
  rc = put_u16be(je, 0xFFDA);
  if (rc != JER_OK) return rc;
  rc = put_u16be(je, (uint16_t)(6 + 2 * je->ncomp));
  if (rc != JER_OK) return rc;
  rc = put_byte(je, je->ncomp);
  if (rc != JER_OK) return rc;
  if (je->ncomp == 3) {
    rc = put_byte(je, 1);
    if (rc != JER_OK) return rc; /* Y */
    rc = put_byte(je, 0x00);
    if (rc != JER_OK) return rc;
    rc = put_byte(je, 2);
    if (rc != JER_OK) return rc; /* Cb */
    rc = put_byte(je, 0x11);
    if (rc != JER_OK) return rc;
    rc = put_byte(je, 3);
    if (rc != JER_OK) return rc; /* Cr */
    rc = put_byte(je, 0x11);
    if (rc != JER_OK) return rc;
  } else {
    rc = put_byte(je, 1);
    if (rc != JER_OK) return rc;
    rc = put_byte(je, 0x00);
    if (rc != JER_OK) return rc;
  }
  rc = put_byte(je, 0); /* Ss */
  if (rc != JER_OK) return rc;
  rc = put_byte(je, 63); /* Se */
  if (rc != JER_OK) return rc;
  return put_byte(je, 0); /* Ah/Al */
}

}  // namespace tjpge_detail

/*-----------------------------------------------------------------------*/
/* Initialize the encoder object: quantization tables and Huffman encode */
/* lookup tables (built once here against the fixed Annex-K tables) and  */
/* the output buffer, from the workspace pool. Does NOT allocate the     */
/* row-band buffer - that's je_start()'s job, since its size depends on  */
/* je->subsample, which a caller may still override (je.subsample = ...) */
/* between this call and je_start().                                    */
/*-----------------------------------------------------------------------*/

inline JERESULT je_prepare(JENC* je, size_t (*outfunc)(JENC*, const uint8_t*, size_t), void* pool, size_t sz_pool,
                            void* dev, uint16_t width, uint16_t height, JEPixelFormat format) {
  using namespace tjpge_detail;

  if (!width || !height) return JER_PAR;
  if (format != JE_FMT_RGB888 && format != JE_FMT_RGB565 && format != JE_FMT_GRAY8 && format != JE_FMT_RGB666)
    return JER_PAR;
  uint8_t ncomp = (format == JE_FMT_GRAY8) ? 1 : 3;

  memset(je, 0, sizeof(JENC));
  je->pool = pool;
  je->sz_pool = sz_pool;
  je->outfunc = outfunc;
  je->device = dev;
  je->width = width;
  je->height = height;
  je->ncomp = ncomp;
  je->pixfmt = (uint8_t)format;
  je->subsample = JE_SUBSAMPLE;
  je->quality = JE_QUALITY;

  JERESULT rc = create_qt_tbl(je, StdLumaQT, 0);
  if (rc != JER_OK) return rc;
  rc = build_huff_lut(je, StdDcLumaBits, StdDcLumaVals, 12, 12, &je->dcCode[0], &je->dcLen[0]);
  if (rc != JER_OK) return rc;
  rc = build_huff_lut(je, StdAcLumaBits, StdAcLumaVals, 162, 256, &je->acCode[0], &je->acLen[0]);
  if (rc != JER_OK) return rc;

  if (ncomp == 3) {
    rc = create_qt_tbl(je, StdChromaQT, 1);
    if (rc != JER_OK) return rc;
    rc = build_huff_lut(je, StdDcChromaBits, StdDcChromaVals, 12, 12, &je->dcCode[1], &je->dcLen[1]);
    if (rc != JER_OK) return rc;
    rc = build_huff_lut(je, StdAcChromaBits, StdAcChromaVals, 162, 256, &je->acCode[1], &je->acLen[1]);
    if (rc != JER_OK) return rc;
  }

  je->outbuf = (uint8_t*)alloc_pool(je, JE_SZBUF);
  if (!je->outbuf) return JER_MEM1;

  return JER_OK;
}

/*-----------------------------------------------------------------------*/
/* Write JPEG headers, allocate the row-band buffer, and ready je for    */
/* je_write_rows() - see this file's own header comment and je_start()'s */
/* declaration comment above.                                            */
/*-----------------------------------------------------------------------*/

inline JERESULT je_start(JENC* je) {
  using namespace tjpge_detail;

  if (!je->width || !je->height || !je->outbuf) return JER_PAR;
  if (je->started) return JER_PAR; /* already started - je is single-session, see JENC's own comment */

  JERESULT rc = write_headers(je);
  if (rc != JER_OK) return rc;

  je->dcv[0] = je->dcv[1] = je->dcv[2] = 0;
  je->bitbuf = 0;
  je->bitcnt = 0;

  je->mcuRows = (je->ncomp == 3 && je->subsample) ? 16 : 8;
  size_t rowStride = (size_t)je->width * bytes_per_pixel((JEPixelFormat)je->pixfmt);
  je->rowbuf = (uint8_t*)alloc_pool(je, rowStride * je->mcuRows);
  if (!je->rowbuf) return JER_MEM1;

  je->rowsBuffered = 0;
  je->rowsWritten = 0;
  je->started = 1;
  je->finished = 0;
  return JER_OK;
}

/*-----------------------------------------------------------------------*/
/* Feed more source rows into the encoder, encoding+discarding each      */
/* je->mcuRows-row band as soon as it fills - see this file's own header */
/* comment and je_write_rows()'s declaration comment above.              */
/*-----------------------------------------------------------------------*/

inline JERESULT je_write_rows(JENC* je, const uint8_t* rows, uint16_t numRows) {
  using namespace tjpge_detail;

  if (!je->started || je->finished || !rows) return JER_PAR;
  if ((uint32_t)je->rowsWritten + numRows > je->height) return JER_PAR;

  size_t rowStride = (size_t)je->width * bytes_per_pixel((JEPixelFormat)je->pixfmt);
  const uint8_t* src = rows;
  uint16_t remaining = numRows;

  while (remaining) {
    uint16_t toCopy = (uint16_t)(je->mcuRows - je->rowsBuffered);
    if (toCopy > remaining) toCopy = remaining;

    memcpy(je->rowbuf + (size_t)je->rowsBuffered * rowStride, src, (size_t)toCopy * rowStride);
    je->rowsBuffered = (uint16_t)(je->rowsBuffered + toCopy);
    je->rowsWritten += toCopy;
    src += (size_t)toCopy * rowStride;
    remaining = (uint16_t)(remaining - toCopy);

    if (je->rowsBuffered == je->mcuRows) {
      JERESULT rc = encode_band(je);
      if (rc != JER_OK) return rc;
      je->rowsBuffered = 0;
    }
  }
  return JER_OK;
}

/*-----------------------------------------------------------------------*/
/* Encode the final (possibly partial) row band, write EOI, and flush -  */
/* see this file's own header comment and je_finish()'s declaration      */
/* comment above.                                                        */
/*-----------------------------------------------------------------------*/

inline JERESULT je_finish(JENC* je) {
  using namespace tjpge_detail;

  if (!je->started || je->finished) return JER_PAR;
  if (je->rowsWritten != je->height) return JER_PAR; /* caller didn't feed exactly height rows in total */

  if (je->rowsBuffered) {
    size_t rowStride = (size_t)je->width * bytes_per_pixel((JEPixelFormat)je->pixfmt);
    const uint8_t* lastRow = je->rowbuf + (size_t)(je->rowsBuffered - 1) * rowStride;
    for (unsigned int r = je->rowsBuffered; r < je->mcuRows; r++) {
      memcpy(je->rowbuf + (size_t)r * rowStride, lastRow, rowStride); /* edge-replicate, see get_rgb()/get_gray()'s own comment */
    }
    JERESULT rc = encode_band(je);
    if (rc != JER_OK) return rc;
    je->rowsBuffered = 0;
  }

  JERESULT rc = flush_bits(je);
  if (rc != JER_OK) return rc;
  rc = put_u16be(je, 0xFFD9); /* EOI */
  if (rc != JER_OK) return rc;
  je->finished = 1;
  return flush_outbuf(je);
}

/*-----------------------------------------------------------------------*/
/* Convenience wrapper: encode one full in-memory frame of pixels        */
/* (row-major, in the JEPixelFormat je_prepare() was given) to a         */
/* baseline JPEG byte stream via je->outfunc - see je_encode()'s         */
/* declaration comment above and this file's own header comment.        */
/*-----------------------------------------------------------------------*/

inline JERESULT je_encode(JENC* je, const uint8_t* pixels) {
  if (!pixels) return JER_PAR;
  JERESULT rc = je_start(je);
  if (rc != JER_OK) return rc;
  rc = je_write_rows(je, pixels, je->height);
  if (rc != JER_OK) return rc;
  return je_finish(je);
}
