#!/usr/bin/env python3
"""Compares the real-3DS timing runs of the gpSP builds.

usage: compare_timing.py <path to sdmc:/retroarch/e2e>

Each timing build (gpsp_timing_<name>.3dsx / .cia) boots Metroid Fusion from
sdmc:/roms/gba/metroid_fusion.gba, plays 6000 frames with scripted input and
writes out_<name>/log.txt plus a few raw frames. This prints, per build:
whether the dynarec was really on, the average core time per frame (after the
first 600 frames, in 268 MHz system ticks and in milliseconds) compared with
"original", and whether the saved frames are identical to the original's.
"""
import glob, hashlib, os, sys

TICKS_PER_MS = 268111.856

root = sys.argv[1] if len(sys.argv) > 1 else "."
runs = {}
for d in sorted(glob.glob(os.path.join(root, "out_*"))):
    name = os.path.basename(d)[4:]
    info = {"frames": {}}
    log = os.path.join(d, "log.txt")
    if os.path.exists(log):
        for line in open(log):
            f = line.split()
            if f[:1] == ["dynarec"]:
                info["dynarec"], info["svchax"] = f[1], f[3]
            if f[:1] == ["DONE"]:
                kv = dict(zip(f[1::2], f[2::2]))
                info["core"] = int(kv["avg_core_ticks_per_frame_after_600"])
                info["run"] = int(kv["avg_run_ticks_per_frame_after_600"])
    for raw in glob.glob(os.path.join(d, "f*.raw")):
        info["frames"][os.path.basename(raw)] = hashlib.md5(open(raw, "rb").read()).hexdigest()
    runs[name] = info

if not runs:
    sys.exit("no out_* folders found under " + root)
base = runs.get("original", {})
print(f"{'build':32} {'dynarec':>7} {'core ms/frame':>13} {'vs original':>11}  frames")
for name, r in runs.items():
    if "core" not in r:
        print(f"{name:32} {'?':>7} {'not finished':>13}")
        continue
    ms = r["core"] / TICKS_PER_MS
    rel = ""
    if "core" in base and name != "original":
        rel = f"{(r['core'] - base['core']) * 100.0 / base['core']:+.1f}%"
    same = ""
    if base.get("frames") and name != "original":
        common = set(base["frames"]) & set(r["frames"])
        diff = [k for k in common if base["frames"][k] != r["frames"][k]]
        same = "identical" if common and not diff else ("DIFFERENT: " + ", ".join(sorted(diff)) if diff else "no frames")
    dyn = "on" if r.get("dynarec") == "1" else "OFF"
    print(f"{name:32} {dyn:>7} {ms:13.3f} {rel:>11}  {same}")
if any(r.get("dynarec") != "1" for r in runs.values() if "core" in r):
    print("\nWARNING: the dynarec was off in some runs (no kernel access), so those"
          "\ntimed the interpreter. Install the .cia builds, or run the .3dsx with"
          "\nLuma3DS, and check that svchax is 1 in log.txt.")
