#!/bin/bash
# build.sh <gpsp_src_dir> <output_elf> [march] [extra cflags...]
#
# Builds gpSP (ARM dynarec) plus the headless harness as a static ARM Linux
# ELF that runs under armemu. Needs clang/lld 18+ and the sysroot made by
# build_armsys.py (set ARMSYS to its path, default: ./sysroot next to this
# script).  Example, with the block dump enabled:
#   ./build.sh ~/gpsp base.elf armv6k -DGPSP_DRC_DUMP
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$1" && pwd); OUT=$(realpath -m "$2"); MARCH=${3:-armv6k}; shift 3 || shift $#
EXTRA="$*"
SYS=${ARMSYS:-$HERE/sysroot}
CLANGINC=$(clang -print-resource-dir)/include
CFLAGS="--target=arm-linux-musleabi -march=$MARCH -marm -mfloat-abi=soft -O2 -nostdinc -isystem $SYS/include -isystem $CLANGINC -ffunction-sections -fno-strict-aliasing -w"
DEFS="-DARM_ARCH -DHAVE_DYNAREC -DMMAP_JIT_CACHE -D__LIBRETRO__ -DINLINE=inline -DFRONTEND_SUPPORTS_RGB565 -DNDEBUG -DHAVE_STDINT_H -DHAVE_INTTYPES_H -DHAVE_STRINGS_H $EXTRA"
INC="-I$SRC/libretro -I$SRC/libretro/libretro-common/include -I$SRC"
OBJ=$(mktemp -d)
CSRC="main.c gba_memory.c savestate.c input.c sound.c cheats.c memmap.c serial.c gbp.c rfu.c serial_proto.c libretro/libretro.c gba_cc_lut.c cpu_threaded.c
libretro/libretro-common/compat/compat_posix_string.c libretro/libretro-common/compat/compat_strl.c libretro/libretro-common/compat/fopen_utf8.c libretro/libretro-common/encodings/encoding_utf.c libretro/libretro-common/file/file_path.c libretro/libretro-common/file/file_path_io.c libretro/libretro-common/streams/file_stream.c libretro/libretro-common/string/stdstring.c libretro/libretro-common/time/rtime.c libretro/libretro-common/vfs/vfs_implementation.c"
for f in $CSRC; do
  [ -f "$SRC/$f" ] || continue
  clang $CFLAGS $DEFS $INC -c "$SRC/$f" -o "$OBJ/$(echo $f | tr / _).o" &
done
wait
for f in video.cc cpu.cc; do
  clang++ $CFLAGS $DEFS $INC -fno-rtti -fno-exceptions -std=c++11 -nostdinc++ -c "$SRC/$f" -o "$OBJ/$f.o"
done
for f in arm/arm_stub.S bios_data.S; do
  clang $CFLAGS $DEFS $INC -c "$SRC/$f" -o "$OBJ/$(echo $f | tr / _).o"
done
clang $CFLAGS -I"$SRC/libretro/libretro-common/include" -DSTATIC_CORE -c "$HERE/harness/run_static.c" -o "$OBJ/harness.o"
ld.lld -static -o "$OUT" "$SYS/lib/crt1.o" "$SYS/lib/crti.o" "$OBJ"/*.o \
  --start-group "$SYS/lib/libc.a" "$SYS/lib/libm.a" "$SYS/lib/libbuiltins.a" --end-group "$SYS/lib/crtn.o"
rm -rf "$OBJ"
echo "built $OUT"
