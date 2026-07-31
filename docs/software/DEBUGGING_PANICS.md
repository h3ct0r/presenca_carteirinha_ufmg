# Decoding a Guru Meditation panic

When the firmware crashes, the ESP32-P4 prints a register dump and reboots.
Unlike the Xtensa chips, RISC-V targets print **no backtrace** — just registers
and a stack hexdump. `riscv32-esp-elf-addr2line` is what turns those hex numbers
back into `file.cpp:line`.

This is the tool used to find the two real crashes documented in
[FACE_DETECTION.md](FACE_DETECTION.md) §Status / caveats.

## The command

```sh
~/.platformio/packages/toolchain-riscv32-esp/bin/riscv32-esp-elf-addr2line \
  -pfiaC -e .pio/build/esp32p4/firmware.elf 0x40258570 0x401b4d6c
```

Worth an alias, since you will type it under pressure:

```sh
alias p4line='~/.platformio/packages/toolchain-riscv32-esp/bin/riscv32-esp-elf-addr2line -pfiaC -e .pio/build/esp32p4/firmware.elf'
```

The flags:

| Flag | What it does | Why you want it |
| --- | --- | --- |
| `-e <elf>` | the binary to look up addresses in | **must** be the exact build that crashed |
| `-a` | print the address before each answer | keeps answers matched to inputs when you pass several |
| `-f` | print the function name, not just the line | usually the fastest read |
| `-i` | expand **inlined** frames | at `-Os` most small functions are inlined; without this you get the outer one and a confusing line number |
| `-C` | demangle C++ names | `_ZN3fbs8FbsModel8load_mapEv` → `fbs::FbsModel::load_map()` |
| `-p` | one flat line per result | pastable into an issue |

Addresses are passed as `0x…`, as many at a time as you like.

## Rule zero: the ELF must be the one that crashed

`addr2line` looks up an address in a symbol table. Rebuild anything — even an
unrelated `.cpp` — and every address after that point shifts. Decoding an old
dump against a new ELF gives **confident, wrong answers**.

That is not hypothetical. The same address from a real dump, decoded against the
build that produced it and against a later build:

```
0x40297972: fbs::FbsModel::clear_map()   ← the build that crashed
0x40297972: fbs::FbsModel::load_map()    ← after a few unrelated edits
```

Both are plausible. Only one is true. So: **copy `firmware.elf` aside before you
flash**, or at minimum decode before you touch the tree again.

```sh
cp .pio/build/esp32p4/firmware.elf /tmp/firmware-$(git rev-parse --short HEAD).elf
```

### Detecting a stale ELF

Disassemble the faulting address and see whether the instruction can produce the
fault you got:

```sh
~/.platformio/packages/toolchain-riscv32-esp/bin/riscv32-esp-elf-objdump \
  -d --start-address=0x40297968 --stop-address=0x40297988 .pio/build/esp32p4/firmware.elf
```

If `MCAUSE` says *load access fault* but the instruction at `MEPC` is
`lw s5,212(sp)` — a plain stack restore that cannot fault — the ELF has moved on
and every symbol you just read is fiction.

## Reading the dump

```
Guru Meditation Error: Core  0 panic'ed (Load access fault). Exception was unhandled.

MEPC    : 0x40258570  RA      : 0x401b4d6c  SP      : 0x5010bf90  ...
A5      : 0x00000000  ...
MCAUSE  : 0x00000005  MTVAL   : 0x00000000
```

Read these five, in this order:

| Register | Meaning |
| --- | --- |
| `MCAUSE` | *why* it trapped (table below) |
| `MTVAL` | the address that faulted. `0x0` = null pointer. A small value like `0x2c` = a **member access off a null object** — the offset is the field |
| `MEPC` | the instruction that faulted → **feed to addr2line** |
| `RA` | the return address, i.e. who called it → **feed to addr2line** |
| `SP` | the stack pointer, which tells you which task (see the ranges below) |

`MCAUSE` values you will actually see:

| Value | Meaning | Usual cause |
| --- | --- | --- |
| 1 | Instruction access fault | jumped through a corrupt/null function pointer |
| 2 | Illegal instruction | executing data; a corrupted vtable |
| 5 | **Load access fault** | reading through a null/bad pointer |
| 7 | **Store access fault** | writing through a null/bad pointer |
| 4 / 6 | Load / store address misaligned | a bad cast or an unaligned struct |
| 3 | Breakpoint | a failed `assert` / `abort()` |

`A0`–`A7` hold the first eight arguments and `S0`–`S11` are callee-saved values.
They are useful corroboration — but at `-Os` the compiler reuses registers
freely, so treat them as hints, not evidence, unless the disassembly shows the
register is live.

## Which numbers are code?

Only addresses in the flash-mapped code window mean anything to `addr2line`.
For the ESP32-P4 (`soc.h`):

| Range | What lives there |
| --- | --- |
| `0x4000_0000`–`0x43FF_FFFF` | **code and rodata in flash** — the only decodable addresses. This build's `.flash.text` runs from `0x40000020` for `0x33d4b6` bytes |
| `0x4800_0000`–`0x4BFF_FFFF` | PSRAM — image buffers, big allocations |
| `0x4FF0_0000`–`0x4FFB_FFFF` | internal SRAM — statics, task stacks, DMA buffers |
| `0x5010_8000`–`0x5011_0000` | LP SRAM (32 KB) |

So `SP : 0x5010bf90` says that task's stack is in LP SRAM, and
`A1 : 0x48f73720` is a PSRAM pointer — a preview or camera buffer here. Feeding
either to `addr2line` produces nonsense.

## Rebuilding a call chain by hand

`MEPC` and `RA` give you two frames. For more, scrape every code-looking address
out of the `Stack memory` hexdump — return addresses pushed by earlier frames are
still lying there:

```sh
grep -oE '0x4[0-3][0-9a-f]{6}' panic.txt | sort -u | xargs p4line
```

Expect false positives (any stale word that happens to look like an address).
Read it as "these functions were on the stack recently", not as an ordered trace.

## Worked example: a null pointer, proven

The dump above, decoded:

```
$ p4line 0x40258570 0x401b4d6c
0x40258570: resize_nn_simd_helper_rgb565le2bgr888_qint8 at ??:?
0x401b4d6c: dl::image::ImageTransformer::resize_nn<...>(...) at lib/esp-dl/vision/image/dl_image_process.hpp:680
```

`??:?` on the first line just means that translation unit shipped without debug
line info (vendored esp-dl assembly); the **function name still resolves**, and
`RA` gives the caller with a real file and line.

Now the disassembly, to find *which* pointer was null:

```
$ riscv32-esp-elf-objdump -d --start-address=0x40258568 --stop-address=0x40258578 firmware.elf
40258568: esp.movi.32.a  q3,t4,2
4025856c: esp.movi.32.a  q3,t5,3
40258570: lh   a5,0(a5)      ← MEPC
```

`lh a5,0(a5)` loads a halfword from the address held in `a5`. The dump says
`A5 : 0x00000000`. That closes it: `MCAUSE 5` (load fault), `MTVAL 0x0`, and the
faulting instruction dereferences a register the dump proves was zero — a null
**source image** pointer in esp-dl's pixel conversion, not a wild address or a
stack overflow.

## Worked example: read the log before you decode

The second crash produced a near-identical dump, and decoding it was the *least*
useful thing to do, because the lines above it already said everything:

```
E sdmmc_cmd: sdmmc_read_sectors: not enough mem, err=0x101
E FbsLoader: Failed to open /sdcard/models/human_face_detect_mnp_s8_v1.espdl.
E dl::Model: Fail to load model
Guru Meditation Error: Core 0 panic'ed (Load access fault)
MCAUSE : 0x00000005   MTVAL : 0x0000002c
```

`err=0x101` is `ESP_ERR_NO_MEM`; a library announced it had failed to load a
model and then used it anyway; `MTVAL 0x2c` is a field 44 bytes into a null
object. The decode only confirmed *which* library function did it.

Two habits follow from this:

- **Capture the 20 lines before the dump**, always. A panic is where the program
  stopped, rarely where it went wrong.
- **Instrument before you re-flash.** The free-heap logging in
  `face_detection_start()` exists because the second dump could not tell us how
  much memory was left — the log line could.

## Let the monitor do it

PlatformIO ships a filter that decodes dumps as they arrive. It is not enabled in
`platformio.ini`; add it when you are chasing a crash:

```ini
monitor_filters = esp32_exception_decoder
```

It decodes against `.pio/build/esp32p4/firmware.elf`, so it is only correct while
that ELF is still the flashed one — rule zero applies to the automatic path too.
Manual `addr2line` remains the tool for a dump someone pasted to you hours later.

## Checklist

1. Save the whole serial log, including what came **before** the dump.
2. Make sure the ELF matches the flashed build; if unsure, cross-check `MEPC`
   with `objdump`.
3. Read `MCAUSE` and `MTVAL` first — they usually name the class of bug.
4. `addr2line` on `MEPC`, then `RA`.
5. `objdump` around `MEPC` to see which register held the bad value, then look it
   up in the dump.
6. Only then scrape the stack hexdump for more frames.
