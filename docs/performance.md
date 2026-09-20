# Performance

Measured (not estimated) timings for both directions, on the same 64x48
test content across every platform below - same images, same encode
settings, same warm-up-then-repeat methodology everywhere, so the rows in
each table are directly comparable to each other, not just internally
consistent with themselves. One table per direction; add a platform by
adding a row, not a new section - see "How these numbers were produced"
at the bottom for the exact reproduction steps per platform.

## Decode

Decode has no float/fixed-point choice to make - `tjpgd_detail::block_idct()`
(the decoder's inverse DCT) has always been integer-only (the fast
AAN/Arai algorithm), so these numbers are unaffected by anything on this
page.

| Platform | Image | Time (avg / min / max) | fps (from avg) |
|---|---|---|---|
| Desktop x86 | `gradient_444.jpg` (4:4:4, smooth gradient) | 68.1 / 60.3 / 147.9 us | ~14,684 |
| Desktop x86 | `checker_420.jpg` (4:2:0, checkerboard) | 46.8 / 40.9 / 122.4 us | ~21,356 |
| ESP32-S3 @ 240MHz | `gradient_444.jpg` (4:4:4, smooth gradient) | 3820.7 / 3816 / 3822 us | ~261 |
| ESP32-S3 @ 240MHz | `checker_420.jpg` (4:2:0, checkerboard) | 2611.3 / 2608 / 2614 us | ~382 |
| RP2350 (Pico 2 W) @ 150MHz | `gradient_444.jpg` (4:4:4, smooth gradient) | 5708.9 / 5685 / 5928 us | ~175 |
| RP2350 (Pico 2 W) @ 150MHz | `checker_420.jpg` (4:2:0, checkerboard) | 3711.7 / 3705 / 3869 us | ~269 |
| RP2040 (Pico) @ 133MHz | `gradient_444.jpg` (4:4:4, smooth gradient) | 6809.6 / 6805 / 6831 us | ~147 |
| RP2040 (Pico) @ 133MHz | `checker_420.jpg` (4:2:0, checkerboard) | 4625.5 / 4623 / 4632 us | ~216 |

## Encode

Encoding a synthetic 64x48 RGB888 gradient (the same one
`test/native/test_encode_roundtrip.cpp` uses) at quality 85. Unlike
decode, the encoder's forward DCT has two implementations -
`forward_dct()` (float) and `forward_dct_fixed()` (Q12 fixed-point, same
algorithm - see `tjpge.h`) - selected by the `JE_FIXED_POINT_DCT` macro
(`tjpge_config.h`), which auto-detects "does this target have a hardware
FPU" per platform (see "Float vs. fixed-point" below); the **Default**
column marks which one each platform actually gets without any override.
Both produce the same encoded image quality (see
`test/native/test_forward_dct_fixed.cpp` - mean coefficient-level
difference 0.003, well under one unit of the DCT's own scale), so every
row below is a pure speed comparison, not a quality tradeoff.

| Platform | Mode | DCT | Default? | Time (avg / min / max) | fps (from avg) | Output size |
|---|---|---|---|---|---|---|
| Desktop x86 | 4:4:4 | float | **yes** | 316.5 / 275.0 / 989.1 us | ~3,159 | 1125 bytes |
| Desktop x86 | 4:2:0 | float | **yes** | 246.1 / 174.0 / 654.7 us | ~4,063 | 921 bytes |
| Desktop x86 | 4:4:4 | fixed | no | 316.6 / 306.6 / 663.8 us | ~3,159 | 1125 bytes |
| Desktop x86 | 4:2:0 | fixed | no | 190.5 / 187.3 / 295.3 us | ~5,249 | 921 bytes |
| ESP32-S3 @ 240MHz | 4:4:4 | float | **yes** | 19514.9 / 19511 / 19519 us | ~51 | 1125 bytes |
| ESP32-S3 @ 240MHz | 4:2:0 | float | **yes** | 12175.2 / 12171 / 12180 us | ~82 | 921 bytes |
| ESP32-S3 @ 240MHz | 4:4:4 | fixed | no | 24142.0 / 24138 / 24148 us | ~41 | 1125 bytes |
| ESP32-S3 @ 240MHz | 4:2:0 | fixed | no | 14481.6 / 14478 / 14485 us | ~69 | 921 bytes |
| RP2350 (Pico 2 W) @ 150MHz | 4:4:4 | float | **yes** | 27222.7 / 27194 / 27348 us | ~37 | 1125 bytes |
| RP2350 (Pico 2 W) @ 150MHz | 4:2:0 | float | **yes** | 17037.4 / 17017 / 17126 us | ~59 | 921 bytes |
| RP2350 (Pico 2 W) @ 150MHz | 4:4:4 | fixed | no | 29304.3 / 29275 / 29428 us | ~34 | 1125 bytes |
| RP2350 (Pico 2 W) @ 150MHz | 4:2:0 | fixed | no | 18072.7 / 18052 / 18176 us | ~55 | 921 bytes |
| RP2040 (Pico) @ 133MHz | 4:4:4 | float | no | 232558.4 / 232552 / 232585 us | ~4 | 1125 bytes |
| RP2040 (Pico) @ 133MHz | 4:2:0 | float | no | 130632.6 / 130627 / 130640 us | ~8 | 921 bytes |
| RP2040 (Pico) @ 133MHz | 4:4:4 | fixed | **yes** | 90630.3 / 90625 / 90662 us | ~11 | 1125 bytes |
| RP2040 (Pico) @ 133MHz | 4:2:0 | fixed | **yes** | 59887.9 / 59882 / 59900 us | ~16 | 921 bytes |

## Float vs. fixed-point: why there's a default per platform, not one answer

The instinct that "integer math is faster on microcontrollers" is true
often enough to be a reasonable prior, but it's wrong wherever the target
actually has a hardware FPU - and enough of this library's target
platforms do that defaulting to fixed-point everywhere would have made
encoding *slower* on most of them. The table above is the actual
evidence, not the prior:

- **Desktop x86, ESP32-S3, RP2350** all have a hardware FPU, and float is
  faster on all three - 8-11% faster on desktop, 19-24% faster on
  ESP32-S3, 6-8% faster on RP2350. The FPU does a float multiply-add
  about as cheaply as an integer one, so `forward_dct_fixed()`'s extra
  Q12 shift/round instructions per butterfly stage (see its own comment
  in `tjpge.h`) just add overhead the float path doesn't pay.
- **RP2040 has no hardware FPU** (Cortex-M0+), and fixed-point is
  dramatically faster there - **2.57x faster at 4:4:4, 2.18x faster at
  4:2:0** (232.6ms -> 90.6ms, 130.6ms -> 59.9ms). Every float multiply-add
  in `forward_dct()` was a software libgcc call with no FPU behind it;
  replacing those with int64 arithmetic removes that entirely. This also
  closes most of the disproportionate encode-vs-decode gap flagged in an
  earlier version of this page: RP2040 decode was already only ~1.2x
  slower than the FPU-equipped RP2350 (ordinary clock-speed difference),
  while RP2040 encode used to be ~8x slower than that same comparison -
  with fixed-point now the default there, RP2040 encode is
  "only" ~3.3-3.5x slower than RP2350's *float* encode, a gap much closer
  to what the two chips' clock speeds and decode numbers alone would
  predict.

**The auto-detection** (`JE_FIXED_POINT_DCT` in `tjpge_config.h`) picks
the right default per *architecture family*, not by enumerating chip
names - `__SOFTFP__`/`__ARM_ARCH_6M__` for ARM, `__AVR__` for AVR,
`__XTENSA__` + the compiler-builtin `__XCHAL_HAVE_FP == 0` for Xtensa,
and `__riscv_float_abi_soft` for RISC-V. This was verified directly
against each target's own predefined macros (`gcc -dM -E - < /dev/null`
for the Xtensa toolchains; `arduino-cli compile --warnings all` probing
`#if`/`#warning` output for everything else), not assumed from chip
names or family reputation - which is what caught a real subtlety in the
ESP32 family specifically: the original ESP32 (Xtensa LX6) and ESP32-S3
(Xtensa LX7 dual-core) both have a hardware FPU, but the **ESP32-S2**
(Xtensa LX7 single-core) does **not** - confirmed via its
`__XCHAL_HAVE_FP` value being 0, not 1, despite being the same core
family as the S3. The RISC-V ESP32 variants split the same way for a
different reason: ESP32-C3/C6 (and, by the same "no F extension" silicon,
almost certainly C2/H2) have no hardware FPU (`__riscv_float_abi_soft`),
while the newer **ESP32-P4** does (RV32IMAFC - the F extension - confirmed
via `__riscv_float_abi_single`, not `_soft`). None of these were
special-cased by chip name; they all fell out correctly from checking the
actual architectural capability each toolchain reports, which is also
why this should keep working correctly for chips this library has never
been tested against.

Override with a `-D` (or a `#define` before this library's first
`#include`) if the auto-detection guesses wrong for your specific target
- see `tjpge_config.h`'s own comment on `JE_FIXED_POINT_DCT` for the
full reasoning and exact macros.

## Cross-platform notes

**Decode scales the way you'd expect from clock speed and
microarchitecture alone**, on every microcontroller measured so far:
63-64x slower than desktop on the ESP32-S3 (240MHz Xtensa LX7), 91-94x on
the RP2350 (150MHz dual Cortex-M33), 113x on the RP2040 (133MHz dual
Cortex-M0+) - a steady progression tracking clock speed and core
capability, not a jump. None of that is evidence of a platform-specific
inefficiency; it's ordinary silicon getting slower as it gets simpler and
slower-clocked.

**Encode, with each platform's *default* DCT, now scales similarly.**
With float DCT on the FPU-equipped ESP32-S3/RP2350 and fixed-point on the
FPU-less RP2040 (see "Float vs. fixed-point" above), RP2040's default
encode time is ~3.3-3.5x RP2350's, much closer to the ~1.2x their decode
numbers alone would suggest than the ~8x gap a same-DCT (float-only)
comparison showed before `forward_dct_fixed()` existed - most of that old
gap really was float-emulation overhead on the FPU-less chip specifically,
not some other RP2040-specific inefficiency - which is exactly what
switching its default to fixed-point just fixed.

On every microcontroller, free heap (`ESP.getFreeHeap()`/
`rp2040.getFreeHeap()`) was identical before and after every run
(ESP32-S3: 310936 bytes; RP2350: 406008 bytes; RP2040: 205744 bytes, both
times on each, and unaffected by which DCT was active) - direct
on-device confirmation that neither direction, nor either DCT
implementation, allocates on the heap, matching the "fixed workspace, no
dynamic allocation" design both engines were built around (`tjpgd.h`'s
`TJPGD_WORKSPACE_SIZE`, `tjpge.h`'s `TJPGE_WORKSPACE_SIZE`).

## How these numbers were produced

**Desktop x86** (`test/native/bench_native.cpp`, `-O2`):

```sh
cmake --build build --target bench_native
cd test/native && ../build/test/native/bench_native
```

For the fixed-point DCT rows, reconfigure with
`-DCMAKE_CXX_FLAGS=-DJE_FIXED_POINT_DCT=1` before building
`bench_native` (a separate build directory keeps this from clobbering
your normal float build).

`min` is the more stable/representative number across runs on a shared
desktop machine (background processes, thermal/frequency scaling,
scheduler noise inflate `avg`/`max` run to run more than they do `min`);
`avg`/fps are still reported since that's the unit a real application
budget is usually expressed in.

**ESP32-S3** (`examples/PerformanceTest/PerformanceTest.ino`, on a
genuine ESP32-S3 rev2, dual-core Xtensa LX7 @ 240MHz, 8MB PSRAM):

```sh
arduino-cli compile --warnings all --fqbn esp32:esp32:esp32s3 examples/PerformanceTest
arduino-cli upload -p /dev/ttyACM0 --fqbn esp32:esp32:esp32s3 examples/PerformanceTest
# then open a serial monitor at 115200 baud
```

Add `--build-property "build.extra_flags=-DJE_FIXED_POINT_DCT=1"` to the
`compile` line for the fixed-point rows (upload always uses whatever was
last compiled - there's no separate flag for it).

(Swap the FQBN/port for your specific board - `esp32:esp32:esp32s3` is
the generic "ESP32S3 Dev Module" entry, which works for most ESP32-S3
dev boards regardless of exact silkscreen/vendor; run `arduino-cli board
listall esp32` for the full list of named variants, e.g. Adafruit's
Feather ESP32-S3.)

**RP2350 (Pico 2 W)** (same sketch, on a genuine Raspberry Pi Pico 2 W,
dual-core Cortex-M33 @ 150MHz):

```sh
arduino-cli compile --warnings all --fqbn rp2040:rp2040:rpipico2w examples/PerformanceTest
arduino-cli upload -p UF2_Board --fqbn rp2040:rp2040:rpipico2w examples/PerformanceTest
# then open a serial monitor at 115200 baud
```

`-p UF2_Board` is what `arduino-cli board list` shows while the Pico is
in its UF2 bootloader mode (freshly plugged in, or after holding BOOTSEL
on power-up) - it mounts as a mass-storage device and `arduino-cli`
copies the `.uf2` onto it directly, then it reboots into the sketch and
re-enumerates as a normal serial port (`/dev/ttyACM0` here) for
subsequent uploads/monitoring. `rp2040:rp2040:rpipico2w` is the Pico 2
W-specific entry (`rpipico2` for the non-wireless Pico 2, or
`generic_rp2350` for other RP2350 boards) - run `arduino-cli board
listall rp2040` for the full list. Add
`--build-property "build.extra_flags=-DJE_FIXED_POINT_DCT=1"` to the
`compile` line for the fixed-point rows, same as ESP32-S3 above (this
board defaults to float, so no flag is needed to reproduce those rows).

**RP2040 (original Pico)** (same sketch, on a genuine Raspberry Pi Pico,
dual-core Cortex-M0+ @ 133MHz, no hardware FPU):

```sh
arduino-cli compile --warnings all --fqbn rp2040:rp2040:rpipico examples/PerformanceTest
arduino-cli upload -p UF2_Board --fqbn rp2040:rp2040:rpipico examples/PerformanceTest
# then open a serial monitor at 115200 baud - the encode section alone
# takes ~35s (fixed-point DCT, the default here) to ~75s (float DCT) at
# 200 reps/mode on this board, so give it time before assuming the
# sketch has hung.
```

Same upload mechanics as the RP2350 above (`-p UF2_Board` while in
bootloader mode); `rp2040:rp2040:rpipico` is the plain-Pico-specific
entry (`rpipicow` for the Pico W, which adds wireless but is the same
RP2040 silicon and should perform identically for this sketch, which
doesn't touch WiFi/BT). This board now gets `JE_FIXED_POINT_DCT=1`
automatically (no `--build-property` needed) - add
`--build-property "build.extra_flags=-DJE_FIXED_POINT_DCT=0"` to the
`compile` line to force the (much slower here) float DCT instead, e.g.
to reproduce this page's "before" numbers.

All four environments use the same methodology: one untimed warm-up
call, then 200 (desktop: 2000) timed back-to-back repeats of the same
in-memory encode/decode call, reporting avg/min/max across the timed
repeats.
