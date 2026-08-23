#pragma once
/*----------------------------------------------------------------------------/
/ TJpgDec system configuration - header-only port.
/
/ Originally "tjpgdcnf.h" (TJpgDec R0.03, (C)ChaN, 2021). Every value below
/ is unchanged from ChaN's own defaults; the only difference from the
/ original is that each macro is now guarded with #ifndef so a consumer can
/ override any of them with a `-D` (or a `#define` before the *first*
/ `#include` of this library anywhere in the build) instead of editing this
/ file in place - the standard header-only-library override idiom, since
/ there is no longer a per-project copy of this file to hand-edit the way a
/ traditional Arduino library (TJpg_Decoder itself included) expects.
/----------------------------------------------------------------------------*/

#ifndef JD_SZBUF
#define JD_SZBUF 512
/* Specifies size of stream input buffer */
#endif

#ifndef JD_FORMAT
#define JD_FORMAT 1
/* Specifies output pixel format.
/  0: RGB888 (24-bit/pix)
/  1: RGB565 (16-bit/pix)
/  2: Grayscale (8-bit/pix)
*/
#endif

#ifndef JD_USE_SCALE
#define JD_USE_SCALE 1
/* Switches output descaling feature.
/  0: Disable
/  1: Enable
*/
#endif

#ifndef JD_TBLCLIP
#define JD_TBLCLIP 0
/* Use table conversion for saturation arithmetic. A bit faster, but increases 1 KB of code size.
/  0: Disable
/  1: Enable
*/
#endif

#ifndef JD_FASTDECODE
#define JD_FASTDECODE 1
/* Optimization level
/  0: Basic optimization. Suitable for 8/16-bit MCUs.
/     Workspace of 3100 bytes needed.
/  1: + 32-bit barrel shifter. Suitable for 32-bit MCUs.
/     Workspace of 3480 bytes needed.
/  2: + Table conversion for huffman decoding (wants 6 << HUFF_BIT bytes of RAM).
/     Workspace of 9644 bytes needed.
*/
#endif

// Do not change this, it is the minimum size in bytes of the workspace needed by the decoder
#if JD_FASTDECODE == 0
#define TJPGD_WORKSPACE_SIZE 3100
#elif JD_FASTDECODE == 1
#define TJPGD_WORKSPACE_SIZE 3500
#elif JD_FASTDECODE == 2
#define TJPGD_WORKSPACE_SIZE (3500 + 6144)
#endif
