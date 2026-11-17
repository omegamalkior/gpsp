#!/usr/bin/env python3
"""Reads and compares gpSP dynarec block dumps (build with -DGPSP_DRC_DUMP).

  drc_dump.py show  DUMP [--pc ADDR] [--limit N]
      Disassembles the translated blocks (all, or the one for guest PC ADDR).

  drc_dump.py compare A.bin B.bin [--hist A.hist B.hist] [--top N]
      Matches blocks by guest PC and mode and compares how many host
      instructions each build generated for them. With --hist (host execution
      counts from tools/armemu, env HOSTHIST=file) it also compares how many
      host instructions were actually executed, which is the number that
      matters for speed.

  drc_dump.py diff A.bin B.bin --pc ADDR
      Side-by-side disassembly of the same guest block in both builds.

Disassembly uses llvm-mc + llvm-objdump if available, otherwise GNU binutils
(arm-none-eabi-as/objdump, as shipped with devkitARM). Choose explicitly with
--tool llvm|gnu and --prefix (eg. --prefix arm-none-eabi-).

Notes:
 * Host instruction counts include the data words that follow memory handler
   calls (the PC of the guest instruction), since the CPU still has to step
   over them. ARM host code is fixed width, so count = size / 4.
 * A block can be translated more than once (cache flushes). "compare" uses
   the first translation of each block; the execution histogram is matched
   to the last block that occupied each host address.
"""
import argparse, os, shutil, struct, subprocess, sys, tempfile
from collections import OrderedDict

MAGIC = 0x42435244


def read_dump(path):
    """Returns a list of blocks: dict(pc, thumb, end, host, code)"""
    blocks = []
    data = open(path, "rb").read()
    off = 0
    while off + 24 <= len(data):
        magic, pc, end, lo, hi, size = struct.unpack_from("<6I", data, off)
        if magic != MAGIC:
            sys.exit(f"{path}: bad record at offset {off}")
        off += 24
        blocks.append(dict(pc=pc & ~1, thumb=bool(pc & 1), end=end,
                           host=(hi << 32) | lo, code=data[off:off + size]))
        off += size
    return blocks


def first_by_key(blocks):
    d = OrderedDict()
    for b in blocks:
        d.setdefault((b["pc"], b["thumb"]), b)
    return d


def read_hist(path):
    """Returns ({host address: count}, {summary key: value})"""
    h, info = {}, {}
    for line in open(path):
        if line.startswith("#"):
            f = line[1:].split()
            info.update({f[i]: int(f[i + 1]) for i in range(0, len(f) - 1, 2)})
            continue
        a, c = line.split()
        h[int(a, 16)] = int(c)
    return h, info


def exec_counts(blocks, hist):
    """Executed host instructions per (pc, thumb), attributing each host
    address to the last block dumped there."""
    owner = {}
    for b in blocks:
        for a in range(b["host"], b["host"] + len(b["code"]), 4):
            owner[a] = (b["pc"], b["thumb"])
    res = {}
    for a, c in hist.items():
        k = owner.get(a)
        if k is not None:
            res[k] = res.get(k, 0) + c
    return res


def guest_len(b):
    w = 2 if b["thumb"] else 4
    return max(0, (b["end"] - b["pc"]) // w)


# ---------------------------------------------------------------- disassembly
def find_tool(kind, prefix):
    if kind in (None, "llvm"):
        for mc, od in (("llvm-mc", "llvm-objdump"), ("llvm-mc-18", "llvm-objdump-18")):
            if shutil.which(mc) and shutil.which(od):
                return ("llvm", mc, od)
        if kind == "llvm":
            sys.exit("llvm-mc/llvm-objdump not found")
    for p in ([prefix] if prefix else ["arm-none-eabi-", "arm-linux-gnueabihf-", "arm-linux-gnueabi-"]):
        if shutil.which(p + "as") and shutil.which(p + "objdump"):
            return ("gnu", p + "as", p + "objdump")
    sys.exit("no ARM disassembler found (install LLVM or devkitARM)")


def disassemble(blocks, tool):
    """Returns {index: [text lines]} for the given blocks"""
    kind, asm, objdump = tool
    with tempfile.TemporaryDirectory() as td:
        src, obj = os.path.join(td, "b.s"), os.path.join(td, "b.o")
        with open(src, "w") as f:
            f.write(".arm\n.text\n")
            for i, b in enumerate(blocks):
                f.write(f"blk_{i}:\n")
                code = b["code"]
                for o in range(0, len(code) - len(code) % 4, 4):
                    f.write(".inst 0x%08x\n" % struct.unpack_from("<I", code, o))
        if kind == "llvm":
            subprocess.run([asm, "--triple=armv6k-none-eabi", "-filetype=obj", src, "-o", obj], check=True)
            out = subprocess.run([objdump, "-d", "--no-show-raw-insn", "--triple=armv6k-none-eabi", obj],
                                 capture_output=True, text=True, check=True).stdout
        else:
            subprocess.run([asm, "-march=armv6k", src, "-o", obj], check=True)
            out = subprocess.run([objdump, "-d", "--no-show-raw-insn", obj],
                                 capture_output=True, text=True, check=True).stdout
    res, cur = {}, None
    for line in out.splitlines():
        s = line.strip()
        if s.endswith(">:") and "<blk_" in s:
            cur = int(s.split("<blk_")[1].rstrip(">:"))
            res[cur] = []
        elif cur is not None and ":" in s and s.split(":")[0].strip().isalnum():
            addr, _, ins = s.partition(":")
            res[cur].append("%4x  %s" % (int(addr, 16) - 0, " ".join(ins.split())))
    # make offsets block-relative
    for i, lines in res.items():
        if lines:
            base = int(lines[0].split()[0], 16)
            res[i] = ["%4x  %s" % (int(l.split()[0], 16) - base, l.split(None, 1)[1] if len(l.split(None, 1)) > 1 else "")
                      for l in lines]
    return res


def fmt_key(k):
    return "%08x %s" % (k[0], "T" if k[1] else "A")


# ---------------------------------------------------------------- commands
def cmd_show(a):
    blocks = read_dump(a.dump)
    if a.pc is not None:
        blocks = [b for b in blocks if b["pc"] == a.pc]
    blocks = blocks[:a.limit]
    dis = disassemble(blocks, find_tool(a.tool, a.prefix))
    for i, b in enumerate(blocks):
        print(f"; guest {fmt_key((b['pc'], b['thumb']))}..{b['end']:08x} "
              f"({guest_len(b)} guest insts) -> host {b['host']:#x}, {len(b['code']) // 4} host words")
        for l in dis.get(i, []):
            print("  " + l)
        print()


def cmd_compare(a):
    A, B = read_dump(a.a), read_dump(a.b)
    fa, fb = first_by_key(A), first_by_key(B)
    common = [k for k in fa if k in fb]
    ea = eb = None
    if a.hist:
        (ha, ia), (hb, ib) = read_hist(a.hist[0]), read_hist(a.hist[1])
        ea, eb = exec_counts(A, ha), exec_counts(B, hb)
    rows = []
    for k in common:
        sa, sb = len(fa[k]["code"]) // 4, len(fb[k]["code"]) // 4
        xa = ea.get(k, 0) if ea else 0
        xb = eb.get(k, 0) if eb else 0
        rows.append((k, guest_len(fa[k]), sa, sb, xa, xb))
    only_a = len(fa) - len(common)
    only_b = len(fb) - len(common)
    tsa, tsb = sum(r[2] for r in rows), sum(r[3] for r in rows)
    print(f"blocks: {len(common)} in both, {only_a} only in A, {only_b} only in B")
    print(f"generated host instructions (blocks in both): A={tsa}  B={tsb}  "
          f"({(tsb - tsa) * 100.0 / max(tsa, 1):+.1f}%)")
    if ea:
        txa, txb = sum(ea.values()), sum(eb.values())
        def line(name, va, vb):
            print(f"  {name:<44} A={va:<12} B={vb:<12} ({(vb - va) * 100.0 / max(va, 1):+.1f}%)")
        print("executed host instructions:")
        line("in translated blocks (matched)", txa, txb)
        if "total_insns" in ia and "total_insns" in ib:
            line("whole emulator (incl. handlers, renderer)", ia["total_insns"], ib["total_insns"])
            line("outside translated code", ia["total_insns"] - ia["jit_insns"], ib["total_insns"] - ib["jit_insns"])
            line("estimated ARM11 cycles (armemu model)", ia["est_arm11_cycles"], ib["est_arm11_cycles"])
        print("  (inline fast paths add instructions to the blocks but remove calls")
        print("   into the shared memory handlers, so judge them by the whole total)")
        rows.sort(key=lambda r: -max(r[4], r[5]))
        print(f"\nhottest blocks (by executed instructions):")
    else:
        rows.sort(key=lambda r: -abs(r[3] - r[2]))
        print(f"\nblocks with the largest change in generated code:")
    print("  guest block   g.ins  gen A  gen B    exec A       exec B     change")
    for k, g, sa, sb, xa, xb in rows[:a.top]:
        ch = (f"{(xb - xa) * 100.0 / xa:+6.1f}%" if xa else "") if ea else f"{sb - sa:+d}"
        print(f"  {fmt_key(k)}  {g:5d}  {sa:5d}  {sb:5d}  {xa:10d}  {xb:10d}  {ch}")


def cmd_diff(a):
    fa, fb = first_by_key(read_dump(a.a)), first_by_key(read_dump(a.b))
    ks = [k for k in fa if k[0] == a.pc and k in fb]
    if not ks:
        sys.exit("block not found in both dumps")
    tool = find_tool(a.tool, a.prefix)
    for k in ks:
        da = disassemble([fa[k]], tool).get(0, [])
        db = disassemble([fb[k]], tool).get(0, [])
        print(f"; guest {fmt_key(k)}: A {len(da)} host insts, B {len(db)} host insts")
        w = max([len(l) for l in da] + [20]) + 2
        for i in range(max(len(da), len(db))):
            l = da[i] if i < len(da) else ""
            r = db[i] if i < len(db) else ""
            print(f"{l:<{w}}| {r}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tool", choices=["llvm", "gnu"])
    ap.add_argument("--prefix", help="GNU toolchain prefix, eg. arm-none-eabi-")
    sp = ap.add_subparsers(dest="cmd", required=True)
    s = sp.add_parser("show"); s.add_argument("dump")
    s.add_argument("--pc", type=lambda x: int(x, 16)); s.add_argument("--limit", type=int, default=50)
    c = sp.add_parser("compare"); c.add_argument("a"); c.add_argument("b")
    c.add_argument("--hist", nargs=2); c.add_argument("--top", type=int, default=25)
    d = sp.add_parser("diff"); d.add_argument("a"); d.add_argument("b")
    d.add_argument("--pc", type=lambda x: int(x, 16), required=True)
    a = ap.parse_args()
    {"show": cmd_show, "compare": cmd_compare, "diff": cmd_diff}[a.cmd](a)


if __name__ == "__main__":
    main()
