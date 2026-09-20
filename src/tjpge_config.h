#pragma once
/*----------------------------------------------------------------------------/
/ tjpge - TinyJPEG Encoder engine configuration.
/
/ Mirrors tjpgd_config.h's own pattern: every macro is #ifndef-guarded so a
/ consumer can override it with a `-D`, or a `#define` before the *first*
/ `#include` of this library anywhere in the build, instead of editing this
/ file - the standard header-only-library override idiom.
/----------------------------------------------------------------------------*/

#ifndef JE_QUALITY
#define JE_QUALITY 80
/* Default JPEG quality (1-100, libjpeg-style scale) used to scale the
/  standard Annex-K quantization tables when the caller doesn't pass an
/  explicit quality to the encoder. Higher = larger file, less loss.
*/
#endif

#ifndef JE_SUBSAMPLE
#define JE_SUBSAMPLE 1
/* Chroma subsampling mode.
/  0: 4:4:4 (no subsampling - one Cb/Cr sample per Y sample)
/  1: 4:2:0 (one Cb/Cr sample per 2x2 Y block - smaller files, standard default)
*/
#endif

#ifndef JE_SZBUF
#define JE_SZBUF 512
/* Size of the output stream buffer the encoder fills before handing it to
/  the caller's output function (mirrors JD_SZBUF on the decode side).
*/
#endif

#ifndef JE_FIXED_POINT_DCT
/* Which forward DCT (tjpge.h) encode_block() uses: 0 = forward_dct()
/  (float), 1 = forward_dct_fixed() (int64_t Q12 fixed-point). Same
/  output quality either way - this only picks which is faster, and that
/  depends on whether the target has a hardware FPU (float wins there,
/  fixed-point wins by ~2.2-2.6x without one - see docs/performance.md's
/  "Float vs. fixed-point" section for the measurements). Auto-detected
/  below per architecture (ARM/AVR/Xtensa/RISC-V); override with a `-D`
/  if it guesses wrong for your target.
*/
#if defined(__SOFTFP__) || defined(__ARM_ARCH_6M__) || defined(__AVR__) || \
    (defined(__XTENSA__) && defined(__XCHAL_HAVE_FP) && !__XCHAL_HAVE_FP) || defined(__riscv_float_abi_soft)
#define JE_FIXED_POINT_DCT 1  // no hardware FPU detected
#else
#define JE_FIXED_POINT_DCT 0  // assume a hardware FPU
#endif
#endif

#ifndef JE_MAX_WIDTH
#define JE_MAX_WIDTH 320
/* Widest image TinyJPEGEncoder's own fixed-size workspace array (see
/  TinyJPEGEncoder.h) is sized to handle - not a limit the encoder engine
/  itself (tjpge.h's je_prepare()/je_start()/etc.) enforces or even knows
/  about. je_start() pool-allocates a row-band buffer sized
/  `width * bytesPerPixel * mcuRows` (mcuRows is 8, or 16 for 4:2:0 color
/  - see tjpge.h's own header comment), and that's the only part of the
/  workspace that scales with image width at all; TJPGE_WORKSPACE_SIZE
/  below budgets for that at JE_MAX_WIDTH's worst case so
/  TinyJPEGEncoder's workspace_ member array can stay a fixed compile-time
/  size. A caller driving tjpge.h directly (not through TinyJPEGEncoder)
/  isn't bound by this at all - size your own pool to your actual runtime
/  width instead (`width * 3 * (subsample ? 16 : 8)` plus the fixed part
/  below covers any width, exactly, no ceiling needed). Raise this (a `-D`
/  or a `#define` before this library's first `#include`) if
/  TinyJPEGEncoder needs to handle wider images than the default; lower it
/  to shrink TinyJPEGEncoder's workspace_ if you know your images are
/  always narrower.
*/
#endif

// Do not change this, it is the minimum size in bytes of the workspace
// needed by the encoder for the worst case (3-component input, up to
// JE_MAX_WIDTH pixels wide): two quantization tables (2*64*4 = 512B), the
// DC/AC Huffman encode lookup tables for both luma and chroma
// (2*12*3 + 2*256*3 = 1608B, see build_huff_lut() in tjpge.h), the output
// stream buffer (JE_SZBUF), and the row-band buffer je_start() allocates
// (JE_MAX_WIDTH * 3 bytes/pixel * 16 rows worst case - see JE_MAX_WIDTH's
// own comment above). Per-block DCT/quantize scratch is stack-local
// (transient, not pool-allocated), unlike the decoder's persistent
// mcubuf/workbuf. Grayscale input (ncomp==1) and 4:4:4 (unsubsampled, 8
// rows not 16) both use less than this, so it's a safe upper bound to
// size a caller's pool buffer against, not a tight one.
#define TJPGE_WORKSPACE_SIZE (JE_SZBUF + 2176 + (JE_MAX_WIDTH) * 3 * 16)
