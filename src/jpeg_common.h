#pragma once
/*----------------------------------------------------------------------------/
/ jpeg_common.h - pieces of the JPEG baseline codec that are direction-
/ agnostic (equally correct for decoding and encoding), factored out so
/ tjpgd.h (decoder, ported from ChaN's TJpgDec) and tjpge.h (encoder) share
/ one definition instead of carrying two copies that could drift apart.
/
/ Everything here is `inline constexpr`/`inline` so, same as tjpgd.h's own
/ tables, there is exactly one copy in the final binary no matter how many
/ translation units include this header.
/-----------------------------------------------------------------------------/
/ What lives here and why:
/  - Zig[64]: the zigzag-order <-> raster-order permutation for an 8x8
/    block. The decoder applies it once per de-quantized coefficient
/    (raster write of a zigzag-ordered stream); the encoder applies the
/    same table the other way (zigzag read of a raster-ordered block)
/    before Huffman-coding the coefficients. Same table, opposite walk
/    direction - not two different tables.
/  - BYTECLIP()/Clip8[]: saturate a value to [0, 255]. The decoder clips
/    IDCT/color-conversion output; the encoder clips RGB/YCbCr
/    intermediates before the forward DCT. Same clamp either way.
/
/ What does NOT live here, on purpose (see docs/architecture.md):
/  - Huffman tables: the decoder builds a code->symbol lookup from each
/    file's own DHT segment; the encoder instead packs against the fixed
/    Annex-K symbol->code tables. Different data shape, not just a
/    direction flip, so there is nothing to share.
/  - The DCT: tjpgd.h's block_idct() is the *inverse* transform; the
/    encoder needs a forward transform. Related math, distinct routines.
/  - Color conversion: decode goes YCbCr->RGB, encode goes RGB->YCbCr -
/    different constants, not a shared function.
/----------------------------------------------------------------------------*/

#include <cstdint>

namespace jpeg_common {

/*-----------------------------------------------*/
/* Zigzag-order to raster-order conversion table */
/*-----------------------------------------------*/

inline constexpr uint8_t Zig[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

/*---------------------------------------------*/
/* Saturating clamp to [0, 255]                 */
/*---------------------------------------------*/

inline uint8_t byteclip(int val) {
  if (val < 0) return 0;
  if (val > 255) return 255;
  return (uint8_t)val;
}

}  // namespace jpeg_common
