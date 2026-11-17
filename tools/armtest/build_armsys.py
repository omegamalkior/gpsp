#!/usr/bin/env python3
"""Builds a static ARM (soft-float, ARMv6K) sysroot from musl + compiler-rt
builtins sources by compiling them directly with clang. Nothing from the
downloaded trees is executed; only compiled.

usage: build_armsys.py <musl_src> <compiler_rt_builtins_dir> <out_sysroot>
"""
import glob, os, re, shutil, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

musl, crt_dir, out = map(os.path.abspath, sys.argv[1:4])
TARGET = ["--target=arm-linux-musleabi", "-march=armv6k", "-marm", "-mfloat-abi=soft"]
obj = os.path.join(out, "obj")
inc = os.path.join(out, "include")
lib = os.path.join(out, "lib")
for d in (obj, inc, lib, os.path.join(obj, "include/bits"), os.path.join(obj, "src/internal")):
    os.makedirs(d, exist_ok=True)

# --- generated headers (same transformations as musl's Makefile) ---
def mkalltypes(text):
    o = []
    for line in text.splitlines():
        m = re.match(r"^TYPEDEF (.*) ([^ ]*);$", line)
        if m:
            t, n = m.groups()
            o += [f"#if defined(__NEED_{n}) && !defined(__DEFINED_{n})", f"typedef {t} {n};", f"#define __DEFINED_{n}", "#endif", ""]
            continue
        m = re.match(r"^(STRUCT|UNION) *([^ ]*) (.*);$", line)
        if m:
            kind, n, body = m.groups(); k = kind.lower()
            o += [f"#if defined(__NEED_{k}_{n}) && !defined(__DEFINED_{k}_{n})", f"{k} {n} {body};", f"#define __DEFINED_{k}_{n}", "#endif", ""]
            continue
        o.append(line)
    return "\n".join(o) + "\n"

src_at = open(f"{musl}/arch/arm/bits/alltypes.h.in").read() + open(f"{musl}/include/alltypes.h.in").read()
open(f"{obj}/include/bits/alltypes.h", "w").write(mkalltypes(src_at))
sc = open(f"{musl}/arch/arm/bits/syscall.h.in").read()
extra = [l.replace("__NR_", "SYS_") for l in sc.splitlines() if "__NR_" in l]
open(f"{obj}/include/bits/syscall.h", "w").write(sc + "\n".join(extra) + "\n")
open(f"{obj}/src/internal/version.h", "w").write('#define VERSION "git"\n')

# --- installed headers ---
shutil.copytree(f"{musl}/include", inc, dirs_exist_ok=True)
os.makedirs(f"{inc}/bits", exist_ok=True)
for d in (f"{musl}/arch/generic/bits", f"{musl}/arch/arm/bits", f"{obj}/include/bits"):
    for f in glob.glob(d + "/*.h"):
        shutil.copy(f, f"{inc}/bits/")

# --- musl sources (static libc only, no ldso) ---
src_dirs = sorted(glob.glob(f"{musl}/src/*")) + [f"{musl}/src/malloc/mallocng"]
base = sorted(f for d in src_dirs for f in glob.glob(d + "/*.c"))
arch = sorted(f for d in src_dirs for f in glob.glob(d + "/arm/*.[csS]"))
replaced = {os.path.splitext(f.replace("/arm/", "/"))[0] for f in arch}
srcs = [f for f in base if os.path.splitext(f)[0] not in replaced] + arch

cflags = TARGET + ["-std=c99", "-ffreestanding", "-nostdinc", "-Os", "-D_XOPEN_SOURCE=700",
          "-fno-stack-protector", "-ffunction-sections", "-fdata-sections", "-w",
          f"-I{musl}/arch/arm", f"-I{musl}/arch/generic", f"-I{obj}/src/internal",
          f"-I{musl}/src/include", f"-I{musl}/src/internal", f"-I{obj}/include", f"-I{musl}/include"]

def cc(args):
    src, o, flags = args
    r = subprocess.run(["clang"] + flags + ["-c", src, "-o", o], capture_output=True, text=True)
    return src, r.returncode, r.stderr[-400:]

jobs = []
for s in srcs:
    rel = os.path.relpath(s, musl).replace("/", "_")
    jobs.append((s, f"{obj}/{rel}.o", cflags))
with ThreadPoolExecutor(os.cpu_count() or 4) as ex:
    res = list(ex.map(cc, jobs))
bad = [r for r in res if r[1]]
print(f"musl: {len(res)-len(bad)} ok, {len(bad)} failed")
for r in bad[:10]:
    print("  FAIL", r[0], r[2])
objs = [j[1] for j in jobs if os.path.exists(j[1])]
if os.path.exists(f"{lib}/libc.a"): os.remove(f"{lib}/libc.a")
subprocess.run(["llvm-ar-18", "rcs", f"{lib}/libc.a"] + objs, check=True)
open(f"{lib}/libm.a", "wb").write(b"!<arch>\n")  # math lives in libc.a in musl

for s, o in ((f"{musl}/crt/crt1.c", "crt1.o"), (f"{musl}/crt/arm/crti.s", "crti.o"), (f"{musl}/crt/arm/crtn.s", "crtn.o")):
    r = cc((s, f"{lib}/{o}", cflags + ["-DCRT"]))
    print("crt", o, "ok" if r[1] == 0 else r[2])

# --- compiler-rt builtins (soft-float ARM) ---
bdir = os.path.join(obj, "builtins"); os.makedirs(bdir, exist_ok=True)
armsrc = glob.glob(f"{crt_dir}/arm/*.S") + glob.glob(f"{crt_dir}/arm/*.c")
# skip VFP / Thumb-only / sync helpers that don't apply to soft-float ARM mode
armsrc = [f for f in armsrc if not re.search(r"(vfp|sync_|switch|chkstk|restore_vfp|save_vfp|fp_mode)", os.path.basename(f))]
bflags0 = TARGET + ["-O2", "-fno-builtin", "-ffreestanding", "-w", f"-I{inc}", f"-I{crt_dir}"]
with ThreadPoolExecutor(os.cpu_count() or 4) as ex:
    ares = list(ex.map(cc, [(f, f"{bdir}/{os.path.basename(f)}.o", bflags0) for f in armsrc]))
armsrc = [r[0] for r in ares if r[1] == 0]   # ARM versions that build for this target
armnames = {os.path.splitext(os.path.basename(f))[0] for f in armsrc}
gen = [f for f in glob.glob(f"{crt_dir}/*.c")
       if os.path.splitext(os.path.basename(f))[0] not in armnames
       and not re.search(r"(atomic|emutls|enable_execute_stack|gcc_personality|cpu_model|os_version|trampoline|apple|eprintf|_tf|tf[0-9]|tf\.|xf|bf|hf|fp16|extendhf|truncsfhf|trunc.*hf|truncdfhf|truncdfbf)", os.path.basename(f))]
bflags = TARGET + ["-O2", "-fno-builtin", "-ffreestanding", "-w", f"-I{inc}", f"-I{crt_dir}"]
jobs = [(f, f"{bdir}/{os.path.basename(f)}.o", bflags) for f in gen + armsrc]
with ThreadPoolExecutor(os.cpu_count() or 4) as ex:
    res = list(ex.map(cc, jobs))
bad = [r for r in res if r[1]]
print(f"builtins: {len(res)-len(bad)} ok, {len(bad)} failed: {[os.path.basename(r[0]) for r in bad][:15]}")
bobjs = [j[1] for j in jobs if os.path.exists(j[1])]
if os.path.exists(f"{lib}/libbuiltins.a"): os.remove(f"{lib}/libbuiltins.a")
subprocess.run(["llvm-ar-18", "rcs", f"{lib}/libbuiltins.a"] + bobjs, check=True)
print("sysroot ready:", out)
