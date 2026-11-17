# Testing gpSP ARM dynarec changes

This branch only adds test tooling. Nothing here changes how gpSP behaves
unless it is built with `-DGPSP_DRC_DUMP`.

There are three separate questions to answer for a dynarec change. Each one
needs a different tool.

| Question | Tool | Where |
|---|---|---|
| Does the generated code do the same thing? | Block dump + side-by-side diff, then frame/audio hash comparison | PC |
| Is it fewer / cheaper instructions? | Block dump + execution histogram | PC |
| Is it actually faster? | Frame timing on a real 3DS | Real hardware |

Azahar (or any 3DS emulator) is not used for the last two. It runs the 3DS
CPU's code on a PC processor, so its timing says nothing about the ARM11. It
also does not model the CPU caches, so cache-coherency bugs (code written but
not flushed before it runs) can never show up there. It is only useful as
"does it boot".

## 1. Generated code: block dump

Build with `-DGPSP_DRC_DUMP`. Every translated block is appended to the file
named by the `GPSP_DRC_DUMP` environment variable (default
`gpsp_drc_dump.bin`): guest PC, mode, host address and the generated code,
before the block's exits are linked.

```
tools/drc/drc_dump.py show dump.bin --pc 080008d2
tools/drc/drc_dump.py diff base.bin new.bin --pc 080008d2
tools/drc/drc_dump.py compare base.bin new.bin [--hist base.hist new.hist]
```

* `diff` prints the same guest block from both builds side by side. This is
  how to review a change for correctness: every difference should be
  explainable by the change being tested and nothing else.
* `compare` lists, per guest block, how many host instructions each build
  generated and (with `--hist`) how many were executed.
* Disassembly uses LLVM (`llvm-mc`, `llvm-objdump`) or, with
  `--tool gnu --prefix arm-none-eabi-`, devkitARM's binutils.
* The word after a memory handler call (`blx`) is data (the guest PC), not an
  instruction, so its disassembly is meaningless (it often shows as `stmdaeq`).

For a 3DS build, add `-DGPSP_DRC_DUMP` to the core's CFLAGS and set the
environment variable or let it write `gpsp_drc_dump.bin` to the current
directory (the SD card root under the Homebrew Launcher).

## 2. Instruction counts and behaviour on a PC: armemu

`tools/armtest` builds gpSP plus a headless libretro harness as a static ARM
Linux program and runs it in `armemu`, a small user-mode ARM emulator.

```
# once: ARM sysroot (musl + compiler-rt builtins, compiled from source)
git clone --depth 1 https://github.com/kraj/musl
git clone --depth 1 --filter=blob:none --sparse https://github.com/llvm/llvm-project
(cd llvm-project && git sparse-checkout set compiler-rt/lib/builtins)
python3 tools/armtest/build_armsys.py musl llvm-project/compiler-rt/lib/builtins tools/armtest/sysroot

gcc -O2 -o armemu tools/armtest/armemu.c
tools/armtest/build.sh <gpsp tree> base.elf armv6k -DGPSP_DRC_DUMP
tools/armtest/build.sh <other tree> new.elf armv6k -DGPSP_DRC_DUMP

GPSP_DRC_DUMP=base.bin HOSTHIST=base.hist ./armemu base.elf game.gba 600 enabled
GPSP_DRC_DUMP=new.bin  HOSTHIST=new.hist  ./armemu new.elf  game.gba 600 enabled
tools/drc/drc_dump.py compare base.bin new.bin --hist base.hist new.hist
```

armemu prints `video=` and `audio=` hashes of all frames. **Two builds must
give identical hashes for the same ROM and frame count**; any difference is a
behaviour change. Compare dynarec builds with each other: gpSP's interpreter
counts cycles differently, so its hashes differ even for a trivial ROM and it
cannot be used as the reference. It also prints:

* `jit_errors`: code that ran after being written but before the instruction
  cache was flushed. On a real 3DS that runs stale bytes. Must be 0 (or no
  higher than the original).
* `est_arm11_cycles`: a rough ARM11 cost model (pipeline stalls, branch
  prediction, return stack). Good for comparing two builds, not an exact
  3DS number.

With `HOSTHIST` it writes how often each host address in the JIT caches ran,
plus whole-run totals. Inline fast paths add instructions to blocks but remove
calls into shared handlers, so compare the whole-emulator total, not only the
"in translated blocks" line.

Good test ROMs: jsmolka's gba-tests (arm, thumb, memory, bios), FuzzARM,
armwrestler, alyosha-tas's gba-tests, `gbabench/torture.c` (self-modifying
code, unaligned and mirrored accesses, stack-heavy code) and real games with
recorded input.

## 3. Speed: a real 3DS

Only hardware gives the real answer. Use an Old 3DS if possible (slowest,
most sensitive). With Luma3DS:

* The dynarec needs memory it can make executable
  (`svcControlProcessMemory`), which depends on how RetroArch was launched
  (CIA vs Homebrew Launcher). Make sure the dynarec is really active in both
  builds being compared (core option "Dynamic recompiler" on, and the core's
  log), otherwise the comparison measures the interpreter.
* Run the same ROM with the same input (a save state plus a recorded input
  replay, or an attract/demo sequence) with frame limiting off.
* Time `retro_run()` with `svcGetSystemTick()` (268 MHz ticks on Old 3DS) and
  average over a few thousand frames; compare original vs patched builds made
  with the same compiler and flags.
* RetroArch's own frame time counter (Settings > On-Screen Display) is a
  quick check but includes video output and vsync.

### Ready-made timing builds

The `gpsp-split-3ds-timing` branch of omegamalkior/RetroArch builds one
RetroArch 3DS per gpSP version (the original and each split branch) with
`GPSP_HW_TIMING=1`. Each build:

* boots `sdmc:/roms/gba/metroid_fusion.gba` (put your own dump there),
* plays it with the same scripted input as `tools/armtest/harness`,
* times `retro_run()` with `svcGetSystemTick()`, leaving out the video and
  audio callbacks, and after 6000 frames writes the averages to
  `sdmc:/retroarch/e2e/out_<name>/log.txt`, with whether the dynarec and
  svchax were active,
* saves frames 300, 900, 1500, 2100 and 2399 next to the log.

Run each build once (about two minutes each; "DONE" appears in the log when
it is finished), copy `sdmc:/retroarch/e2e` to a PC and run
`tools/3ds/compare_timing.py <that folder>`. It prints the core time per
frame against the original, and whether the saved frames are identical to
the original's (they must be).
