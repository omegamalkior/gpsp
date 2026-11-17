// armemu: tiny user-mode ARM (A32, ARMv6K + common ARMv7 bits) Linux emulator
// for testing gpSP's ARM dynarec on an x86 host.
//  - runs static ARM EABI ELF binaries (soft-float, ARM mode only)
//  - flat 4GB guest address space mapped into host memory
//  - JIT-safety check: code written after the last icache flush (cacheflush
//    syscall) and then executed is reported as an error (a real-HW bug)
//  - rough ARM11 (3DS) cycle model + per-function profile
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <elf.h>

typedef uint32_t u32; typedef int32_t s32; typedef uint64_t u64; typedef int64_t s64;
typedef uint16_t u16; typedef uint8_t u8;

static u8 *M;                     // guest memory base (4GB)
#define G(a) (M + (u32)(a))
static inline u32 rd32(u32 a) { u32 v; memcpy(&v, G(a), 4); return v; }
static inline u16 rd16(u32 a) { u16 v; memcpy(&v, G(a), 2); return v; }
static inline u8 rd8(u32 a) { return *G(a); }

// ---- JIT coherency tracking: one bit per 4 bytes, for the mmap region ----
#define MMAP_BASE 0x40000000u
#define MMAP_LIMIT 0x80000000u
static u8 *dirty;  // bitmap over [MMAP_BASE, MMAP_LIMIT)
static u8 *codeseen; // bitmap: address executed at least once
static int check_jit = 1;
static u64 jit_errors;
static inline void mark_write(u32 a, u32 n) {
  if (a - MMAP_BASE >= MMAP_LIMIT - MMAP_BASE) return;
  for (u32 w = (a & ~3u); w < a + n; w += 4) {
    u32 i = (w - MMAP_BASE) >> 2;
    if (codeseen[i >> 3] & (1 << (i & 7))) dirty[i >> 3] |= 1 << (i & 7);
    else dirty[i >> 3] |= 1 << (i & 7);
  }
}
static inline void wr32(u32 a, u32 v) { memcpy(G(a), &v, 4); if (check_jit) mark_write(a, 4); }
static inline void wr16(u32 a, u16 v) { memcpy(G(a), &v, 2); if (check_jit) mark_write(a, 2); }
static inline void wr8(u32 a, u8 v) { *G(a) = v; if (check_jit) mark_write(a, 1); }

static u32 R[16];
static u32 N, Z, C, V, Q, GE;     // flags (0/1)
static u32 tls_value;
static u32 brk_cur, brk_base, mmap_next = MMAP_BASE;
static u64 icount, cycles;
static int exited, exit_code;
static u32 excl_addr = ~0u;
#define HH_SPAN (64u << 20)      // first 64MB of the mmap area (JIT caches)
static u32 *hosthist;

// ---- cost model (very rough ARM11/MPCore) ----
#define RS_SIZE 3
static u32 rstack[RS_SIZE]; static int rs_top;
static u8 *bht;  // 2-bit counters indexed by (pc>>2) & mask
#define BHT_BITS 7   // ARM11: 128-entry BHT
static u64 stat_branch_mispred, stat_indirect, stat_ret_ok, stat_ret_bad, stat_loads, stat_stores, stat_cond_br;
static int last_load_reg = -1;
#define PENALTY 6  // ~mispredict cost on ARM11 (pipeline refill)
static inline void rs_push(u32 a) { rs_top = (rs_top + 1) % RS_SIZE; rstack[rs_top] = a; }
static inline u32 rs_pop(void) { u32 v = rstack[rs_top]; rstack[rs_top] = 0xFFFFFFFF; rs_top = (rs_top + RS_SIZE - 1) % RS_SIZE; return v; }

// ---- profile ----
typedef struct { u32 addr, size; char *name; u64 cyc, ins; } sym_t;
static sym_t *syms; static int nsyms;
static u64 *prof_page; // cycles per 4KB page of guest pc (for unknown/jit code)
static int do_profile;
static u64 *pcprof;

static void die(const char *m, u32 pc, u32 op) {
  fprintf(stderr, "armemu: %s at pc=%08x op=%08x\n", m, pc, op);
  for (int i = 0; i < 16; i++) fprintf(stderr, "r%d=%08x%c", i, R[i], (i % 4 == 3) ? '\n' : ' ');
  exit(99);
}

static inline u32 ror32(u32 v, u32 s) { s &= 31; return s ? (v >> s) | (v << (32 - s)) : v; }
static inline bool cond_ok(u32 c) {
  switch (c) {
  case 0: return Z; case 1: return !Z; case 2: return C; case 3: return !C;
  case 4: return N; case 5: return !N; case 6: return V; case 7: return !V;
  case 8: return C && !Z; case 9: return !C || Z; case 10: return N == V; case 11: return N != V;
  case 12: return !Z && N == V; case 13: return Z || N != V; case 14: return true;
  default: return true;
  }
}
static inline u32 get_cpsr(void) { return (N << 31) | (Z << 30) | (C << 29) | (V << 28) | (Q << 27) | (GE << 16) | 0x10; }
static inline void set_cpsr_flags(u32 v, u32 mask_f, u32 mask_s) {
  if (mask_f) { N = v >> 31; Z = (v >> 30) & 1; C = (v >> 29) & 1; V = (v >> 28) & 1; Q = (v >> 27) & 1; }
  if (mask_s) GE = (v >> 16) & 0xF;
}

// shifter: returns value, sets *co carry out
static inline u32 shift_imm(u32 v, u32 type, u32 amt, u32 *co) {
  switch (type) {
  case 0: if (!amt) { *co = C; return v; } *co = (v >> (32 - amt)) & 1; return v << amt;
  case 1: if (!amt) { *co = v >> 31; return 0; } *co = (v >> (amt - 1)) & 1; return v >> amt;
  case 2: if (!amt) { *co = v >> 31; return (u32)((s32)v >> 31); } *co = (v >> (amt - 1)) & 1; return (u32)((s32)v >> amt);
  default: if (!amt) { *co = v & 1; return (C << 31) | (v >> 1); } *co = (v >> (amt - 1)) & 1; return ror32(v, amt);
  }
}
static inline u32 shift_reg(u32 v, u32 type, u32 amt, u32 *co) {
  amt &= 0xFF;
  if (!amt) { *co = C; return v; }
  switch (type) {
  case 0: if (amt < 32) { *co = (v >> (32 - amt)) & 1; return v << amt; } *co = (amt == 32) ? (v & 1) : 0; return 0;
  case 1: if (amt < 32) { *co = (v >> (amt - 1)) & 1; return v >> amt; } *co = (amt == 32) ? (v >> 31) : 0; return 0;
  case 2: if (amt < 32) { *co = (v >> (amt - 1)) & 1; return (u32)((s32)v >> amt); } *co = v >> 31; return (u32)((s32)v >> 31);
  default: amt &= 31; if (!amt) { *co = v >> 31; return v; } *co = (v >> (amt - 1)) & 1; return ror32(v, amt);
  }
}

static u32 cur_pc;
static u32 trace_lo, trace_hi;
static inline u32 reg_read(int r) { return r == 15 ? cur_pc + 8 : R[r]; }

// ---- syscalls ----
static u32 do_mmap(u32 addr, u32 len, u32 prot, u32 flags, s32 fd, u32 off) {
  len = (len + 0xFFF) & ~0xFFFu;
  u32 a;
  if ((flags & 0x10) && addr) a = addr; // MAP_FIXED
  else { a = mmap_next; mmap_next += len + 0x10000; if (mmap_next > MMAP_LIMIT) return (u32)-ENOMEM; }
  if (fd >= 0 && !(flags & 0x20)) { lseek(fd, (off_t)off << 12, SEEK_SET); if (read(fd, G(a), len) < 0) return (u32)-errno; }
  else memset(G(a), 0, len);
  return a;
}

static void fill_stat64(u32 p, struct stat *st) {
  memset(G(p), 0, 104);
  u64 dev = st->st_dev; memcpy(G(p), &dev, 8);
  u32 ino = st->st_ino; memcpy(G(p + 12), &ino, 4);
  u32 mode = st->st_mode; memcpy(G(p + 16), &mode, 4);
  u32 nl = st->st_nlink; memcpy(G(p + 20), &nl, 4);
  s64 size = st->st_size; memcpy(G(p + 48), &size, 8);
  u32 bs = 4096; memcpy(G(p + 56), &bs, 4);
  u64 blk = st->st_blocks; memcpy(G(p + 64), &blk, 8);
  u64 ino64 = st->st_ino; memcpy(G(p + 96), &ino64, 8);
}

static void icache_flush(u32 start, u32 end) {
  for (u32 w = start & ~3u; w < end; w += 4) {
    if (w - MMAP_BASE >= MMAP_LIMIT - MMAP_BASE) continue;
    u32 i = (w - MMAP_BASE) >> 2;
    dirty[i >> 3] &= ~(1 << (i & 7));
  }
}

static u32 syscall_(u32 nr) {
  u32 a0 = R[0], a1 = R[1], a2 = R[2], a3 = R[3], a4 = R[4], a5 = R[5];
  switch (nr) {
  case 1: case 248: exited = 1; exit_code = a0; return 0;
  case 3: { ssize_t r = read(a0, G(a1), a2); return r < 0 ? (u32)-errno : (u32)r; }
  case 4: { ssize_t r = write(a0, G(a1), a2); return r < 0 ? (u32)-errno : (u32)r; }
  case 146: { // writev
    u32 tot = 0; for (u32 i = 0; i < a2; i++) { u32 b = rd32(a1 + i * 8), l = rd32(a1 + i * 8 + 4); if (l) { ssize_t r = write(a0, G(b), l); if (r < 0) return (u32)-errno; tot += r; } } return tot; }
  case 145: { // readv
    u32 tot = 0; for (u32 i = 0; i < a2; i++) { u32 b = rd32(a1 + i * 8), l = rd32(a1 + i * 8 + 4); if (l) { ssize_t r = read(a0, G(b), l); if (r < 0) return (u32)-errno; tot += r; if ((u32)r < l) break; } } return tot; }
  case 5: { int r = open((char *)G(a0), a1 & 0x3FFFF ? ((a1 & 3) | ((a1 & 0x40) ? O_CREAT : 0) | ((a1 & 0x200) ? O_TRUNC : 0) | ((a1 & 0x400) ? O_APPEND : 0)) : (a1 & 3), a2); return r < 0 ? (u32)-errno : (u32)r; }
  case 322: { int fl = (a2 & 3) | ((a2 & 0x40) ? O_CREAT : 0) | ((a2 & 0x200) ? O_TRUNC : 0) | ((a2 & 0x400) ? O_APPEND : 0);
    int r = openat((s32)a0 == -100 ? AT_FDCWD : (int)a0, (char *)G(a1), fl, a3); return r < 0 ? (u32)-errno : (u32)r; }
  case 6: return close(a0) < 0 ? (u32)-errno : 0;
  case 19: { off_t r = lseek(a0, (s32)a1, a2); return r < 0 ? (u32)-errno : (u32)r; }
  case 140: { off_t r = lseek(a0, ((s64)a1 << 32) | a2, a4); if (r < 0) return (u32)-errno; u64 v = r; memcpy(G(a3), &v, 8); return 0; }
  case 45: if (a0 > brk_cur) { memset(G(brk_cur), 0, a0 - brk_cur); brk_cur = a0; } return brk_cur;
  case 192: { u32 r = do_mmap(a0, a1, a2, a3, (s32)a4, a5); if (getenv("TRACE_FLUSH")) fprintf(stderr, "mmap %08x len %x prot %x\n", r, a1, a2); return r; }
  case 91: case 125: case 220: return 0;  // munmap, mprotect, madvise
  case 163: { // mremap(old, oldsz, newsz, flags)
    u32 n = do_mmap(0, a2, 3, 0x22, -1, 0); memcpy(G(n), G(a0), a1 < a2 ? a1 : a2); return n; }
  case 54: return (u32)-ENOTTY;  // ioctl
  case 256: case 224: case 20: return 1000;
  case 174: case 175: return 0;
  case 197: { struct stat st; if (fstat(a0, &st) < 0) return (u32)-errno; fill_stat64(a1, &st); return 0; }
  case 195: case 196: { struct stat st; if (stat((char *)G(a0), &st) < 0) return (u32)-errno; fill_stat64(a1, &st); return 0; }
  case 327: { struct stat st; int r = (a1 && rd8(a1)) ? fstatat((s32)a0 == -100 ? AT_FDCWD : (int)a0, (char *)G(a1), &st, 0) : fstat(a0, &st);
    if (r < 0) return (u32)-errno; fill_stat64(a2, &st); return 0; }
  case 397: return (u32)-ENOSYS; // statx -> musl falls back
  case 263: { struct timespec ts; clock_gettime(a0, &ts); u32 s = ts.tv_sec, ns = ts.tv_nsec; memcpy(G(a1), &s, 4); memcpy(G(a1 + 4), &ns, 4); return 0; }
  case 403: { struct timespec ts; clock_gettime(a0, &ts); s64 s = ts.tv_sec; u32 ns = ts.tv_nsec; memcpy(G(a1), &s, 8); memcpy(G(a1 + 8), &ns, 4); memset(G(a1 + 12), 0, 4); return 0; }
  case 384: { for (u32 i = 0; i < a1; i++) wr8(a0 + i, rand()); return a1; }
  case 183: { if (!getcwd((char *)G(a0), a1)) return (u32)-errno; return strlen((char *)G(a0)) + 1; }
  case 33: return access((char *)G(a0), a1) < 0 ? (u32)-errno : 0;
  case 334: return faccessat(AT_FDCWD, (char *)G(a1), a2, 0) < 0 ? (u32)-errno : 0;
  case 39: return mkdir((char *)G(a0), a1) < 0 ? (u32)-errno : 0;
  case 323: return mkdir((char *)G(a1), a2) < 0 ? (u32)-errno : 0;
  case 85: case 332: return (u32)-EINVAL;
  case 0xf0002: if (getenv("TRACE_FLUSH")) fprintf(stderr, "flush %08x-%08x\n", a0, a1); icache_flush(a0, a1); return 0;  // ARM cacheflush
  case 0xf0005: tls_value = a0; return 0;        // set_tls
  default:
    fprintf(stderr, "armemu: unhandled syscall %u (0x%x)\n", nr, nr);
    return (u32)-ENOSYS;
  }
}

// ---- ELF loading ----
static u32 load_elf(const char *path) {
  int fd = open(path, O_RDONLY); if (fd < 0) { perror(path); exit(1); }
  struct stat st; fstat(fd, &st);
  u8 *f = mmap(0, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  Elf32_Ehdr *eh = (Elf32_Ehdr *)f;
  if (memcmp(eh->e_ident, ELFMAG, 4) || eh->e_machine != EM_ARM) { fprintf(stderr, "not an ARM ELF\n"); exit(1); }
  u32 maxa = 0;
  for (int i = 0; i < eh->e_phnum; i++) {
    Elf32_Phdr *ph = (Elf32_Phdr *)(f + eh->e_phoff + i * eh->e_phentsize);
    if (ph->p_type == PT_LOAD) {
      memcpy(G(ph->p_vaddr), f + ph->p_offset, ph->p_filesz);
      memset(G(ph->p_vaddr + ph->p_filesz), 0, ph->p_memsz - ph->p_filesz);
      if (ph->p_vaddr + ph->p_memsz > maxa) maxa = ph->p_vaddr + ph->p_memsz;
    } else if (ph->p_type == PT_TLS) { /* musl handles static TLS from phdrs */ }
  }
  brk_base = brk_cur = (maxa + 0xFFFF) & ~0xFFFFu;
  // symbols
  Elf32_Shdr *sh = (Elf32_Shdr *)(f + eh->e_shoff);
  for (int i = 0; i < eh->e_shnum; i++) if (sh[i].sh_type == SHT_SYMTAB) {
    Elf32_Sym *sy = (Elf32_Sym *)(f + sh[i].sh_offset); int n = sh[i].sh_size / sizeof(Elf32_Sym);
    char *str = (char *)f + sh[sh[i].sh_link].sh_offset;
    syms = calloc(n, sizeof(sym_t));
    for (int j = 0; j < n; j++) if (ELF32_ST_TYPE(sy[j].st_info) == STT_FUNC || ELF32_ST_TYPE(sy[j].st_info) == STT_OBJECT || (ELF32_ST_TYPE(sy[j].st_info) == STT_NOTYPE && sy[j].st_shndx && sy[j].st_value && str[sy[j].st_name] && str[sy[j].st_name] != '$' && str[sy[j].st_name] != '.')) {
      syms[nsyms].addr = sy[j].st_value & ~1u; syms[nsyms].size = sy[j].st_size; syms[nsyms].name = strdup(str + sy[j].st_name); nsyms++;
    }
  }
  // also need phdr info for musl (AT_PHDR)
  u32 entry = eh->e_entry;
  // place a copy of phdrs in memory: find the load segment containing e_phoff
  static u32 phdr_addr, phnum;
  phnum = eh->e_phnum; phdr_addr = 0;
  for (int i = 0; i < eh->e_phnum; i++) {
    Elf32_Phdr *ph = (Elf32_Phdr *)(f + eh->e_phoff + i * eh->e_phentsize);
    if (ph->p_type == PT_PHDR) phdr_addr = ph->p_vaddr;
    if (ph->p_type == PT_LOAD && eh->e_phoff >= ph->p_offset && eh->e_phoff < ph->p_offset + ph->p_filesz && !phdr_addr)
      phdr_addr = ph->p_vaddr + (eh->e_phoff - ph->p_offset);
  }
  if (!phdr_addr) { phdr_addr = brk_cur; memcpy(G(phdr_addr), f + eh->e_phoff, phnum * 32); brk_base = brk_cur = brk_cur + 0x1000; }
  R[0] = phdr_addr; R[1] = phnum; // stash
  return entry;
}

static int symcmp(const void *a, const void *b) { u32 x = ((sym_t *)a)->addr, y = ((sym_t *)b)->addr; return x < y ? -1 : x > y; }
static sym_t *find_sym(u32 pc) {
  int lo = 0, hi = nsyms - 1, best = -1;
  while (lo <= hi) { int m = (lo + hi) / 2; if (syms[m].addr <= pc) { best = m; lo = m + 1; } else hi = m - 1; }
  if (best < 0) return NULL;
  return &syms[best];
}

// ---- the interpreter ----
static u32 *sym_cache_pc; static sym_t **sym_cache;

static void step(void);

static inline u32 mul_cost(u32 op) { (void)op; return 2; }

#define IH 4096
static u32 ih_op[IH]; static u64 ih_cnt[IH]; static int ih_on;
static void ind_hist(u32 pc) { if (!ih_on) return; u32 op = rd32(pc); u32 h = (op * 2654435761u) >> 20; for (int k = 0; k < 8; k++) { u32 i = (h + k) & (IH - 1); if (ih_op[i] == op || !ih_cnt[i]) { ih_op[i] = op; ih_cnt[i]++; return; } } }
static inline void branch_to(u32 target, int kind, u32 pc) {
  // kind: 0 direct uncond, 1 direct cond (taken), 2 indirect, 3 return-type, 4 call direct, 5 call indirect
  switch (kind) {
  case 0: cycles += 0; break;
  case 4: rs_push(pc + 4); break;
  case 2: cycles += PENALTY; stat_indirect++; ind_hist(pc); break;
  case 5: cycles += PENALTY; stat_indirect++; ind_hist(pc); rs_push(pc + 4); break;
  case 3: if (rs_pop() == target) { stat_ret_ok++; cycles += 0; } else { stat_ret_bad++; cycles += PENALTY; } break;
  }
  R[15] = target;
}
static inline void cond_branch_predict(u32 pc, bool taken) {
  u32 i = (pc >> 2) & ((1 << BHT_BITS) - 1);
  u8 c = bht[i];
  bool pred = c >= 2;
  stat_cond_br++;
  if (pred != taken) { cycles += PENALTY; stat_branch_mispred++; }
  if (taken && c < 3) bht[i]++; else if (!taken && c > 0) bht[i]--;
}

static inline void write_pc_from_alu(u32 v, u32 pc, int rm_is_lr, int is_mov) {
  // mov pc, lr  -> return; add pc, pc, ... / mov pc, rX -> indirect
  if (v & 1) die("switch to Thumb not supported", pc, rd32(pc));
  branch_to(v & ~3u, (rm_is_lr && is_mov) ? 3 : 2, pc);
}

static void step(void) {
  u32 pc = R[15];
  cur_pc = pc;
  if (pc >= 0xffff0000u) {
    // kuser helpers
    if (pc == 0xffff0fe0) { R[0] = tls_value; R[15] = R[14]; return; }
    if (pc == 0xffff0fc0) { u32 old = R[0], nw = R[1], p = R[2]; if (rd32(p) == old) { wr32(p, nw); R[0] = 0; C = 1; } else { R[0] = 1; C = 0; } R[15] = R[14]; return; }
    if (pc == 0xffff0fa0) { R[15] = R[14]; return; }
    die("bad kuser call", pc, 0);
  }
  u32 op = rd32(pc);
  icount++; cycles++;
  if (trace_lo && pc >= trace_lo && pc < trace_hi) { fprintf(stderr, "T %08x %08x r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r12=%08x NZCV=%d%d%d%d\n", pc, op, R[0], R[1], R[2], R[3], R[4], R[12], N, Z, C, V); }
  if (check_jit && pc - MMAP_BASE < MMAP_LIMIT - MMAP_BASE) {
    u32 i = (pc - MMAP_BASE) >> 2;
    codeseen[i >> 3] |= 1 << (i & 7);
    if (dirty[i >> 3] & (1 << (i & 7))) {
      if (jit_errors++ < 20) fprintf(stderr, "armemu: JIT ERROR executing stale (unflushed) code at %08x\n", pc);
      dirty[i >> 3] &= ~(1 << (i & 7));
    }
  }
  // load-use interlock approximation
  int prev_load = last_load_reg; last_load_reg = -1;
  if (prev_load >= 0) {
    u32 rn = (op >> 16) & 15, rm = op & 15, rs = (op >> 8) & 15;
    if (rn == (u32)prev_load || rm == (u32)prev_load || rs == (u32)prev_load) cycles += 2;
  }
  u32 cond = op >> 28;
  R[15] = pc + 4;
  if (cond == 0xF) {
    // unconditional space
    if ((op & 0xFE000000) == 0xFA000000) { // BLX imm
      s32 off = ((s32)(op << 8) >> 6) | ((op >> 23) & 2);
      R[14] = pc + 4; die("BLX imm to Thumb unsupported", pc, op); (void)off; return;
    }
    if ((op & 0xFFFFFFF0) == 0xF57FF040 || (op & 0xFFFFFFF0) == 0xF57FF050 || (op & 0xFFFFFFF0) == 0xF57FF060) return; // dsb/dmb/isb
    if ((op & 0xFFFFFFFF) == 0xF57FF01F) { excl_addr = ~0u; return; } // clrex
    if ((op & 0xFD70F000) == 0xF550F000) return; // pld
    if ((op & 0xFD70F000) == 0xF450F000) return; // pli
    die("unhandled unconditional op", pc, op);
  }
  if (!cond_ok(cond)) {
    if ((op & 0x0E000000) == 0x0A000000) cond_branch_predict(pc, false);
    return;
  }
  u32 cls = (op >> 25) & 7;
  switch (cls) {
  case 0: case 1: {
    // misc / multiply / extra load-store / data processing
    if (cls == 0 && (op & 0x90) == 0x90 && (op & 0x60)) {
      // extra load/store: LDRH/STRH/LDRSB/LDRSH/LDRD/STRD
      u32 P = (op >> 24) & 1, U = (op >> 23) & 1, I = (op >> 22) & 1, W = (op >> 21) & 1, L = (op >> 20) & 1;
      u32 rn = (op >> 16) & 15, rd = (op >> 12) & 15, sh = (op >> 5) & 3;
      u32 off = I ? (((op >> 4) & 0xF0) | (op & 0xF)) : reg_read(op & 15);
      u32 base = reg_read(rn);
      u32 addr = P ? (U ? base + off : base - off) : base;
      u32 wb = U ? base + off : base - off;
      if (L) {
        stat_loads++;
        u32 v;
        if (sh == 1) v = rd16(addr);
        else if (sh == 2) v = (u32)(s32)(int8_t)rd8(addr);
        else v = (u32)(s32)(int16_t)rd16(addr);
        if (!P || W) R[rn] = wb;
        R[rd] = v; last_load_reg = rd;
        if (rd == 15) die("ldrh pc", pc, op);
      } else {
        if (sh == 1) { wr16(addr, reg_read(rd)); stat_stores++; if (!P || W) R[rn] = wb; }
        else if (sh == 2) { // LDRD
          stat_loads++; cycles += 1;
          u32 v0 = rd32(addr), v1 = rd32(addr + 4);
          if (!P || W) R[rn] = wb;
          R[rd] = v0; R[rd + 1] = v1; last_load_reg = rd + 1;
        } else { // STRD
          stat_stores++;
          wr32(addr, reg_read(rd)); wr32(addr + 4, reg_read(rd + 1));
          if (!P || W) R[rn] = wb;
        }
      }
      return;
    }
    if (cls == 0 && (op & 0xF0) == 0x90) {
      u32 hi = (op >> 20) & 0x1F;
      if (hi < 0x10) {
        // multiplies
        u32 rd = (op >> 16) & 15, rn = (op >> 12) & 15, rs = (op >> 8) & 15, rm = op & 15;
        u32 S = (op >> 20) & 1;
        cycles += 1;
        switch ((op >> 21) & 7) {
        case 0: { u32 r = R[rm] * R[rs]; R[rd] = r; if (S) { N = r >> 31; Z = !r; } break; }        // MUL
        case 1: { u32 r = R[rm] * R[rs] + R[rn]; R[rd] = r; if (S) { N = r >> 31; Z = !r; } break; } // MLA
        case 2: { u64 r = (u64)R[rm] * R[rs] + R[rn] + R[rd]; R[rn] = (u32)r; R[rd] = r >> 32; break; } // UMAAL
        case 3: { u32 r = R[rn] - R[rm] * R[rs]; R[rd] = r; break; } // MLS (v7)
        case 4: { u64 r = (u64)R[rm] * R[rs]; R[rn] = (u32)r; R[rd] = r >> 32; if (S) { N = R[rd] >> 31; Z = !r; } cycles++; break; }
        case 5: { u64 r = (u64)R[rm] * R[rs] + (((u64)R[rd] << 32) | R[rn]); R[rn] = (u32)r; R[rd] = r >> 32; if (S) { N = R[rd] >> 31; Z = !r; } cycles++; break; }
        case 6: { s64 r = (s64)(s32)R[rm] * (s32)R[rs]; R[rn] = (u32)r; R[rd] = (u64)r >> 32; if (S) { N = R[rd] >> 31; Z = !r; } cycles++; break; }
        case 7: { s64 r = (s64)(s32)R[rm] * (s32)R[rs] + (s64)(((u64)R[rd] << 32) | R[rn]); R[rn] = (u32)r; R[rd] = (u64)r >> 32; if (S) { N = R[rd] >> 31; Z = !r; } cycles++; break; }
        }
        return;
      }
      // swap / exclusives
      u32 rn = (op >> 16) & 15, rd = (op >> 12) & 15, rm = op & 15;
      if ((op & 0x0FB00FF0) == 0x01000090) { // SWP/SWPB
        u32 a = R[rn];
        if (op & 0x400000) { u32 t = rd8(a); wr8(a, R[rm]); R[rd] = t; } else { u32 t = rd32(a); wr32(a, R[rm]); R[rd] = t; }
        return;
      }
      u32 kind = (op >> 21) & 3, L = (op >> 20) & 1;
      if (L) { // LDREX{,D,B,H}
        u32 a = R[rn]; excl_addr = a;
        switch (kind) { case 0: R[rd] = rd32(a); break; case 1: R[rd] = rd32(a); R[rd + 1] = rd32(a + 4); break; case 2: R[rd] = rd8(a); break; case 3: R[rd] = rd16(a); break; }
        stat_loads++;
      } else { // STREX
        u32 a = R[rn];
        if (excl_addr == a) {
          switch (kind) { case 0: wr32(a, R[rm]); break; case 1: wr32(a, R[rm]); wr32(a + 4, R[rm + 1]); break; case 2: wr8(a, R[rm]); break; case 3: wr16(a, R[rm]); break; }
          R[rd] = 0;
        } else R[rd] = 1;
        excl_addr = ~0u;
      }
      return;
    }
    u32 opc = (op >> 21) & 15, S = (op >> 20) & 1;
    if (!S && opc >= 8 && opc <= 11) {
      // misc instructions (cls 0) / MSR imm, MOVW/MOVT (cls 1)
      if (cls == 1) {
        if (opc == 8) { R[(op >> 12) & 15] = ((op >> 4) & 0xF000) | (op & 0xFFF); return; } // MOVW
        if (opc == 10) { u32 rd = (op >> 12) & 15; R[rd] = (R[rd] & 0xFFFF) | ((((op >> 4) & 0xF000) | (op & 0xFFF)) << 16); return; } // MOVT
        if (opc == 9 && (op & 0x0FFFFF00) == 0x0320F000) return; // nop/yield/wfe hints
        if (opc == 9 || opc == 11) { // MSR imm
          u32 v = ror32(op & 0xFF, ((op >> 8) & 0xF) * 2);
          if (!(op & 0x400000)) set_cpsr_flags(v, op & 0x80000, op & 0x40000);
          return;
        }
        die("unhandled cls1 misc", pc, op);
      }
      u32 sub = (op >> 4) & 0xF;
      if (sub == 0) {
        if ((op & 0x0FBF0FFF) == 0x010F0000) { R[(op >> 12) & 15] = get_cpsr(); return; } // MRS
        if ((op & 0x0FB0FFF0) == 0x0120F000) { // MSR reg
          if (!(op & 0x400000)) { set_cpsr_flags(R[op & 15], op & 0x80000, op & 0x40000); }
          return;
        }
        die("unhandled misc0", pc, op);
      }
      if (sub == 1 && opc == 9) { // BX
        u32 t = R[op & 15];
        if (t & 1) die("BX to Thumb unsupported", pc, op);
        branch_to(t, (op & 15) == 14 ? 3 : 2, pc); return;
      }
      if (sub == 1 && opc == 11) { u32 v = R[op & 15]; R[(op >> 12) & 15] = v ? __builtin_clz(v) : 32; return; } // CLZ
      if (sub == 3 && opc == 9) { // BLX reg
        u32 t = R[op & 15]; R[14] = pc + 4;
        if (t & 1) die("BLX to Thumb unsupported", pc, op);
        branch_to(t, 5, pc); return;
      }
      if (sub == 5) { // QADD/QSUB/QDADD/QDSUB
        s64 a = (s32)R[op & 15], b = (s32)R[(op >> 16) & 15]; s64 r;
        #define SAT(x) ((x) > 0x7FFFFFFFLL ? (Q = 1, 0x7FFFFFFFLL) : (x) < -0x80000000LL ? (Q = 1, -0x80000000LL) : (x))
        switch (opc) { case 8: r = SAT(a + b); break; case 9: r = SAT(a - b); break; case 10: b = SAT(2 * b); r = SAT(a + b); break; default: b = SAT(2 * b); r = SAT(a - b); break; }
        R[(op >> 12) & 15] = (u32)r; return;
      }
      if (sub == 7 && opc == 9) die("BKPT", pc, op);
      if ((sub & 9) == 8) { // signed 16-bit multiplies (v5TE)
        u32 rd = (op >> 16) & 15, rn = (op >> 12) & 15, rs = (op >> 8) & 15, rm = op & 15;
        u32 x = (op >> 5) & 1, y = (op >> 6) & 1;
        s32 a16 = x ? (s32)R[rm] >> 16 : (int16_t)R[rm];
        s32 b16 = y ? (s32)R[rs] >> 16 : (int16_t)R[rs];
        switch (opc) {
        case 8: { s64 r = (s64)a16 * b16 + (s32)R[rn]; if (r != (s32)r) Q = 1; R[rd] = (u32)r; break; } // SMLAxy
        case 9: { s64 p = ((s64)(s32)R[rm] * b16) >> 16; if (!x) { s64 r = p + (s32)R[rn]; if (r != (s32)r) Q = 1; R[rd] = (u32)r; } else R[rd] = (u32)p; break; } // SMLAWy / SMULWy
        case 10: { s64 acc = ((u64)R[rd] << 32) | R[rn]; acc += (s64)a16 * b16; R[rn] = (u32)acc; R[rd] = (u64)acc >> 32; break; }
        case 11: R[rd] = (u32)(a16 * b16); break; // SMULxy
        }
        return;
      }
      die("unhandled misc", pc, op);
    }
    // data processing
    u32 rn = (op >> 16) & 15, rd = (op >> 12) & 15;
    u32 op2, co = C;
    if (cls == 1) { u32 rot = ((op >> 8) & 15) * 2; op2 = ror32(op & 0xFF, rot); if (rot) co = op2 >> 31; }
    else if (op & 0x10) { // reg shift reg
      cur_pc = pc + 4; // pc reads as +12
      op2 = shift_reg(reg_read(op & 15), (op >> 5) & 3, R[(op >> 8) & 15], &co);
      cycles += 1;
    } else op2 = shift_imm(reg_read(op & 15), (op >> 5) & 3, (op >> 7) & 31, &co);
    u32 a = reg_read(rn);
    cur_pc = pc;
    u32 r = 0; int logical = 1, wr = 1; u32 cf = co, vf = V;
    switch (opc) {
    case 0: r = a & op2; break;
    case 1: r = a ^ op2; break;
    case 2: r = a - op2; logical = 0; cf = a >= op2; vf = ((a ^ op2) & (a ^ r)) >> 31; break;
    case 3: r = op2 - a; logical = 0; cf = op2 >= a; vf = ((op2 ^ a) & (op2 ^ r)) >> 31; break;
    case 4: r = a + op2; logical = 0; cf = r < a; vf = (~(a ^ op2) & (a ^ r)) >> 31; break;
    case 5: { u64 t = (u64)a + op2 + C; r = (u32)t; logical = 0; cf = t >> 32; vf = (~(a ^ op2) & (a ^ r)) >> 31; break; }
    case 6: { u64 t = (u64)a + (u32)~op2 + C; r = (u32)t; logical = 0; cf = t >> 32; vf = ((a ^ op2) & (a ^ r)) >> 31; break; }
    case 7: { u64 t = (u64)op2 + (u32)~a + C; r = (u32)t; logical = 0; cf = t >> 32; vf = ((op2 ^ a) & (op2 ^ r)) >> 31; break; }
    case 8: r = a & op2; wr = 0; break;
    case 9: r = a ^ op2; wr = 0; break;
    case 10: r = a - op2; logical = 0; cf = a >= op2; vf = ((a ^ op2) & (a ^ r)) >> 31; wr = 0; break;
    case 11: r = a + op2; logical = 0; cf = r < a; vf = (~(a ^ op2) & (a ^ r)) >> 31; wr = 0; break;
    case 12: r = a | op2; break;
    case 13: r = op2; break;
    case 14: r = a & ~op2; break;
    case 15: r = ~op2; break;
    }
    if (S) {
      if (rd == 15 && wr) die("data-proc S with pc dest", pc, op);
      N = r >> 31; Z = !r; C = cf; if (!logical) V = vf;
    }
    if (wr) {
      if (rd == 15) write_pc_from_alu(r, pc, (op & 15) == 14 && !(op & 0x0FF0) && cls == 0, opc == 13);
      else R[rd] = r;
    }
    return;
  }
  case 2: case 3: {
    if (cls == 3 && (op & 0x10)) {
      // media instructions
      u32 rd = (op >> 12) & 15, rn = (op >> 16) & 15, rm = op & 15;
      u32 o1 = (op >> 20) & 0x1F, o2 = (op >> 5) & 7;
      if ((o1 & 0x1E) == 0x1E && (o2 & 3) == 2) { // UBFX
        u32 lsb = (op >> 7) & 31, w = ((op >> 16) & 31) + 1;
        R[rd] = (R[rm] >> lsb) & (w >= 32 ? 0xFFFFFFFF : ((1u << w) - 1)); return; }
      if ((o1 & 0x1E) == 0x1A && (o2 & 3) == 2) { // SBFX
        u32 lsb = (op >> 7) & 31, w = ((op >> 16) & 31) + 1;
        R[rd] = (u32)(((s32)(R[rm] << (32 - lsb - w))) >> (32 - w)); return; }
      if ((o1 & 0x1E) == 0x1C && (o2 & 3) == 0) { // BFI/BFC
        u32 lsb = (op >> 7) & 31, msb = (op >> 16) & 31;
        if (msb >= lsb) { u32 w = msb - lsb + 1; u32 mask = (w >= 32 ? 0xFFFFFFFF : ((1u << w) - 1)) << lsb;
          u32 src = (rm == 15) ? 0 : (R[rm] << lsb); R[rd] = (R[rd] & ~mask) | (src & mask); }
        return; }
      if ((op & 0x0FA00030) == 0x06A00010) { // SSAT/USAT
        u32 sat = (op >> 16) & 31; u32 sh = (op >> 7) & 31; u32 v = R[rm];
        s32 x = (op & 0x40) ? (sh ? (s32)v >> sh : (s32)v >> 31) : (s32)(v << sh);
        if (op & 0x400000) { // USAT
          s32 mx = (s32)((1u << sat) - 1); if (sat == 31) mx = 0x7FFFFFFF;
          if (x < 0) { x = 0; Q = 1; } else if (x > mx) { x = mx; Q = 1; }
        } else { s32 mx = (s32)((1u << sat) - 1), mn = -(s32)(1u << sat); if (x > mx) { x = mx; Q = 1; } else if (x < mn) { x = mn; Q = 1; } }
        R[rd] = (u32)x; return; }
      if ((op & 0x0F8000F0) == 0x06800070 && ((op >> 20) & 7) != 0) { // extend family (SXTAB etc.)
        u32 rot = ((op >> 10) & 3) * 8; u32 v = ror32(R[rm], rot); u32 add = (rn == 15) ? 0 : R[rn];
        switch ((op >> 20) & 7) {
        case 2: R[rd] = (((u32)(s32)(int8_t)v) & 0xFFFF) | (((u32)(s32)(int8_t)(v >> 16)) << 16); if (rn != 15) die("SXTAB16", pc, op); break;
        case 3: R[rd] = add + (u32)(s32)(int8_t)v; break;
        case 4: R[rd] = (v & 0x00FF00FF); if (rn != 15) die("UXTAB16", pc, op); break;
        case 6: R[rd] = add + (v & 0xFF); break;
        case 7: R[rd] = add + (v & 0xFFFF); break;
        default: die("ext", pc, op);
        }
        return; }
      if ((op & 0x0FF000F0) == 0x06B00070) { u32 rot = ((op >> 10) & 3) * 8; u32 v = ror32(R[rm], rot); R[rd] = ((rn == 15) ? 0 : R[rn]) + (u32)(s32)(int16_t)v; return; } // SXTH/SXTAH
      if ((op & 0x0FFF0FF0) == 0x06BF0F30) { R[rd] = __builtin_bswap32(R[rm]); return; } // REV
      if ((op & 0x0FFF0FF0) == 0x06BF0FB0) { u32 v = R[rm]; R[rd] = ((v & 0x00FF00FF) << 8) | ((v >> 8) & 0x00FF00FF); return; } // REV16
      if ((op & 0x0FFF0FF0) == 0x06FF0FB0) { u32 v = R[rm]; R[rd] = (u32)(s32)(int16_t)(((v & 0xFF) << 8) | ((v >> 8) & 0xFF)); return; } // REVSH
      if ((op & 0x0FFF0FF0) == 0x06FF0F30) { u32 v = R[rm], r = 0; for (int i = 0; i < 32; i++) if (v & (1u << i)) r |= 1u << (31 - i); R[rd] = r; return; } // RBIT
      if ((op & 0x0FF00030) == 0x06800010) { // PKHBT/PKHTB
        u32 sh = (op >> 7) & 31; u32 v = R[rm];
        if (op & 0x40) { v = sh ? (u32)((s32)v >> sh) : (u32)((s32)v >> 31); R[rd] = (R[rn] & 0xFFFF0000) | (v & 0xFFFF); }
        else { v <<= sh; R[rd] = (R[rn] & 0xFFFF) | (v & 0xFFFF0000); }
        return; }
      if ((op & 0x0FF00FF0) == 0x06800FB0) { // SEL
        u32 a = R[rn], b = R[rm], r = 0; for (int i = 0; i < 4; i++) r |= ((GE >> i) & 1 ? a : b) & (0xFFu << (i * 8)); R[rd] = r; return; }
      if ((op & 0x0F800010) == 0x06000010 && ((op >> 20) & 7) != 0 && ((op >> 20) & 7) != 4) { // parallel add/sub
        u32 kind = (op >> 20) & 7, o = (op >> 5) & 7; u32 a = R[rn], b = R[rm], r = 0, ge = 0;
        int lanes = (o >= 4) ? 4 : 2, bits = 32 / lanes;
        for (int i = 0; i < lanes; i++) {
          s32 x, y; u32 mask = (1u << bits) - 1;
          int sgn = (kind <= 3);
          u32 ua = (a >> (i * bits)) & mask, ub;
          int ib = i; if (o == 1 || o == 2) ib = i ^ 1; // ASX/SAX swap halves of b
          ub = (b >> (ib * bits)) & mask;
          if (sgn) { x = (s32)(ua << (32 - bits)) >> (32 - bits); y = (s32)(ub << (32 - bits)) >> (32 - bits); } else { x = ua; y = ub; }
          int add;
          switch (o) { case 0: case 4: add = 1; break; case 3: case 7: add = 0; break; case 1: add = (i == 1); break; case 2: add = (i == 0); break; default: die("bad parallel", pc, op); add = 0; }
          s32 res = add ? x + y : x - y;
          switch (kind) {
          case 1: case 5: // S / U : modular, set GE
            if (kind == 1) { if (res >= 0) ge |= (lanes == 2 ? 3u : 1u) << (i * (lanes == 2 ? 2 : 1)); }
            else { if (add ? (res >= (1 << bits)) : (res >= 0)) ge |= (lanes == 2 ? 3u : 1u) << (i * (lanes == 2 ? 2 : 1)); }
            break;
          case 2: { s32 mx = (1 << (bits - 1)) - 1, mn = -(1 << (bits - 1)); if (res > mx) res = mx; if (res < mn) res = mn; break; }
          case 6: { s32 mx = (1 << bits) - 1; if (res > mx) res = mx; if (res < 0) res = 0; break; }
          case 3: case 7: res >>= 1; break;
          }
          r |= ((u32)res & mask) << (i * bits);
        }
        if (kind == 1 || kind == 5) GE = ge;
        R[rd] = r; return; }
      if ((op & 0x0F8000F0) == 0x07000010 || (op & 0x0F8000F0) == 0x07000030 || (op & 0x0FF000D0) == 0x07500010) {
        // SMUAD/SMLAD/SMMUL/SMMLA
        u32 rdh = (op >> 16) & 15, ra = (op >> 12) & 15, rs = (op >> 8) & 15;
        if ((op & 0x0FF000D0) == 0x07500010) { s64 p = (s64)(s32)R[rm] * (s32)R[rs]; if (op & 0x20) p += 0x80000000LL; s32 hi = p >> 32; R[rdh] = (ra == 15) ? (u32)hi : (u32)(hi + (s32)R[ra]); return; }
        die("unhandled dual multiply", pc, op);
      }
      die("unhandled media", pc, op);
    }
    // LDR/STR
    u32 P = (op >> 24) & 1, U = (op >> 23) & 1, B = (op >> 22) & 1, W = (op >> 21) & 1, L = (op >> 20) & 1;
    u32 rn = (op >> 16) & 15, rd = (op >> 12) & 15, off, dummy;
    if (cls == 2) off = op & 0xFFF;
    else off = shift_imm(reg_read(op & 15), (op >> 5) & 3, (op >> 7) & 31, &dummy);
    u32 base = reg_read(rn);
    u32 addr = P ? (U ? base + off : base - off) : base;
    u32 wb = U ? base + off : base - off;
    if (L) {
      stat_loads++;
      u32 v = B ? rd8(addr) : rd32(addr);
      if (!P || W) R[rn] = wb;
      if (rd == 15) {
        if (v & 1) die("LDR pc to Thumb unsupported", pc, op);
        cycles += 2;
        // pop {pc} via ldr pc,[sp],#4 is a return
        branch_to(v & ~3u, (rn == 13 && !P) ? 3 : 2, pc);
      } else { R[rd] = v; last_load_reg = rd; }
    } else {
      stat_stores++;
      u32 v = rd == 15 ? pc + 8 : R[rd];
      if (B) wr8(addr, v); else wr32(addr, v);
      if (!P || W) R[rn] = wb;
    }
    return;
  }
  case 4: { // LDM/STM
    u32 P = (op >> 24) & 1, U = (op >> 23) & 1, S = (op >> 22) & 1, W = (op >> 21) & 1, L = (op >> 20) & 1;
    u32 rn = (op >> 16) & 15, list = op & 0xFFFF;
    (void)S;
    u32 n = __builtin_popcount(list);
    u32 base = R[rn];
    u32 start = U ? (P ? base + 4 : base) : (P ? base - n * 4 : base - n * 4 + 4);
    u32 wbv = U ? base + n * 4 : base - n * 4;
    cycles += (n + 1) / 2;
    u32 a = start;
    if (L) {
      stat_loads++;
      u32 newpc = 0; int haspc = 0;
      for (int i = 0; i < 16; i++) if (list & (1 << i)) { u32 v = rd32(a); a += 4; if (i == 15) { newpc = v; haspc = 1; } else R[i] = v; }
      if (W && !(list & (1 << rn))) R[rn] = wbv;
      if (haspc) { if (newpc & 1) die("LDM pc Thumb unsupported", pc, op); cycles += 1; branch_to(newpc & ~3u, 3, pc); }
    } else {
      stat_stores++;
      for (int i = 0; i < 16; i++) if (list & (1 << i)) { wr32(a, i == 15 ? pc + 8 : R[i]); a += 4; }
      if (W) R[rn] = wbv;
    }
    return;
  }
  case 5: { // B/BL
    s32 off = (s32)(op << 8) >> 6;
    u32 t = pc + 8 + off;
    if (cond != 14) cond_branch_predict(pc, true);
    if (op & 0x01000000) { R[14] = pc + 4; branch_to(t, 4, pc); }
    else branch_to(t, 0, pc);
    return;
  }
  case 6: die("coprocessor load/store (VFP?) unsupported", pc, op); return;
  case 7:
    if (op & 0x01000000) { R[0] = syscall_(R[7]); cycles += 20; return; }
    if ((op & 0x0F100F10) == 0x0E100F10) { // MRC p15
      u32 crn = (op >> 16) & 15, op1 = (op >> 21) & 7, crm = op & 15, op2 = (op >> 5) & 7, rd = (op >> 12) & 15;
      if (crn == 13 && crm == 0 && op2 == 3 && op1 == 0) { R[rd] = tls_value; return; }
      die("unhandled MRC", pc, op);
    }
    if ((op & 0x0F100F10) == 0x0E000F10) { // MCR p15 (barriers) - ignore
      u32 crn = (op >> 16) & 15; if (crn == 7) return;
      die("unhandled MCR", pc, op);
    }
    die("unhandled coprocessor op", pc, op);
  }
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: armemu [-p] prog args...\n"); return 1; }
  int ai = 1;
  while (ai < argc && argv[ai][0] == '-' && argv[ai][1] && !argv[ai][2]) {
    if (argv[ai][1] == 'p') do_profile = 1;
    else if (argv[ai][1] == 'n') check_jit = 0;
    ai++;
  }
  M = mmap(0, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (M == MAP_FAILED) { perror("mmap"); return 1; }
  dirty = calloc((MMAP_LIMIT - MMAP_BASE) >> 5, 1);
  codeseen = calloc((MMAP_LIMIT - MMAP_BASE) >> 5, 1);
  bht = calloc(1 << BHT_BITS, 1);
  memset(bht, 1, 1 << BHT_BITS);
  memset(rstack, 0xFF, sizeof rstack);
  if (do_profile) { pcprof = calloc(0x100000, 8); prof_page = calloc(1 << 20, 8); sym_cache_pc = calloc(65536, 4); memset(sym_cache_pc, 0xFF, 65536 * 4); sym_cache = calloc(65536, sizeof(void *)); }
  if (getenv("TRACE_LO")) { trace_lo = strtoul(getenv("TRACE_LO"), 0, 16); trace_hi = strtoul(getenv("TRACE_HI"), 0, 16); }
  ih_on = getenv("INDHIST") != NULL;
  if (getenv("HOSTHIST")) hosthist = calloc(HH_SPAN >> 2, sizeof(u32));
  u32 entry = load_elf(argv[ai]);
  u32 phdr = R[0], phnum = R[1];
  memset(R, 0, sizeof R);
  qsort(syms, nsyms, sizeof(sym_t), symcmp);
  // stack
  u32 sp = 0xbf000000;
  u32 strs[64]; int nargs = argc - ai;
  for (int i = 0; i < nargs; i++) { size_t l = strlen(argv[ai + i]) + 1; sp -= l; memcpy(G(sp), argv[ai + i], l); strs[i] = sp; }
  const char *envs[] = { "HOME=/tmp", NULL, NULL, NULL, NULL, NULL }; int nenv = 1;
  if (getenv("NOHASH")) envs[nenv++] = "NOHASH=1";
  if (getenv("FRAMESKIP")) { static char b[64]; snprintf(b, 64, "FRAMESKIP=%s", getenv("FRAMESKIP")); envs[nenv++] = b; }
  if (getenv("DUMP_PPM")) { static char b[512]; snprintf(b, 512, "DUMP_PPM=%s", getenv("DUMP_PPM")); envs[nenv++] = b; }
  if (getenv("GPSP_DRC_DUMP")) { static char b[512]; snprintf(b, 512, "GPSP_DRC_DUMP=%s", getenv("GPSP_DRC_DUMP")); envs[nenv++] = b; }
  u32 envp[6];
  for (int i = 0; i < nenv; i++) { size_t l = strlen(envs[i]) + 1; sp -= l; memcpy(G(sp), envs[i], l); envp[i] = sp; }
  sp -= 16; u32 rnd = sp; for (int i = 0; i < 16; i++) *G(rnd + i) = i * 7;
  sp &= ~15u;
  u32 auxv[][2] = { {3, phdr}, {4, 32}, {5, phnum}, {6, 4096}, {9, entry}, {16, (1 << 15) | (1 << 2) | (1 << 3) | (1 << 12)}, {25, rnd}, {23, 0}, {0, 0} };
  int naux = sizeof auxv / 8;
  u32 words = 1 + nargs + 1 + nenv + 1 + naux * 2;
  sp -= words * 4; sp &= ~7u;
  u32 p = sp;
  wr32(p, nargs); p += 4;
  for (int i = 0; i < nargs; i++) { wr32(p, strs[i]); p += 4; }
  wr32(p, 0); p += 4;
  for (int i = 0; i < nenv; i++) { wr32(p, envp[i]); p += 4; }
  wr32(p, 0); p += 4;
  for (int i = 0; i < naux; i++) { wr32(p, auxv[i][0]); wr32(p + 4, auxv[i][1]); p += 8; }
  R[13] = sp; R[15] = entry;
  struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
  if (do_profile) {
    while (!exited) {
      u32 pc = R[15]; u64 c0 = cycles; step(); u64 d = cycles - c0;
      u32 h = (pc >> 2) & 0xFFFF; sym_t *sy;
      if (sym_cache_pc[h] == pc) sy = sym_cache[h]; else { sy = (pc >= MMAP_BASE) ? NULL : find_sym(pc); if (sy && sy->size && pc >= sy->addr + sy->size) sy = NULL; sym_cache_pc[h] = pc; sym_cache[h] = sy; }
      if (sy) sy->ins += d; else prof_page[pc >> 12] += d;
      if (pc < 0x400000) pcprof[pc >> 2] += d;
    }
  } else if (hosthist) {
    while (!exited) { u32 pc = R[15]; if (pc - MMAP_BASE < HH_SPAN) hosthist[(pc - MMAP_BASE) >> 2]++; step(); }
  } else while (!exited) step();
  clock_gettime(CLOCK_MONOTONIC, &t1);
  double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
  fflush(stdout);
  fprintf(stderr, "armemu: exit=%d insns=%llu est_arm11_cycles=%llu (%.1f MIPS host) indirect=%llu ret_ok=%llu ret_bad=%llu condbr=%llu mispred=%llu loads=%llu stores=%llu jit_errors=%llu\n",
          exit_code, (unsigned long long)icount, (unsigned long long)cycles, icount / s / 1e6, (unsigned long long)stat_indirect,
          (unsigned long long)stat_ret_ok, (unsigned long long)stat_ret_bad, (unsigned long long)stat_cond_br, (unsigned long long)stat_branch_mispred,
          (unsigned long long)stat_loads, (unsigned long long)stat_stores, (unsigned long long)jit_errors);
  if (do_profile) {
    // sort syms by instructions
    u64 jit = 0; for (u32 i = (MMAP_BASE >> 12); i < (MMAP_LIMIT >> 12); i++) jit += prof_page[i];
    u64 other = 0; for (u32 i = 0; i < (1u << 20); i++) other += prof_page[i];
    fprintf(stderr, "PROFILE (est. cycles): total=%llu  jit-code=%.1f%%  unknown-other=%.1f%%\n", (unsigned long long)cycles, 100.0 * jit / cycles, 100.0 * (other - jit) / cycles);
    for (int k = 0; k < 30; k++) {
      int best = -1; for (int i = 0; i < nsyms; i++) if (syms[i].ins && (best < 0 || syms[i].ins > syms[best].ins)) best = i;
      if (best < 0) break;
      fprintf(stderr, "  %6.2f%%  %s\n", 100.0 * syms[best].ins / cycles, syms[best].name); syms[best].ins = 0;
    }
  }
  if (getenv("DUMPSYM")) { char nm[64]; unsigned off, len; sscanf(getenv("DUMPSYM"), "%63[^,],%x,%x", nm, &off, &len);
    for (int i = 0; i < nsyms; i++) if (!strcmp(syms[i].name, nm)) { for (u32 k = 0; k < len; k += 4) fprintf(stderr, "%s%08x", (k % 32) ? " " : "\nDUMP ", rd32(syms[i].addr + off + k)); fprintf(stderr, "\n"); break; } }
  if (hosthist) { FILE *f = fopen(getenv("HOSTHIST"), "w");
    if (f) { u64 jit = 0; for (u32 i = 0; i < (HH_SPAN >> 2); i++) jit += hosthist[i];
      fprintf(f, "# total_insns %llu jit_insns %llu est_arm11_cycles %llu\n", (unsigned long long)icount, (unsigned long long)jit, (unsigned long long)cycles);
      for (u32 i = 0; i < (HH_SPAN >> 2); i++) if (hosthist[i]) fprintf(f, "%08x %u\n", MMAP_BASE + i * 4, hosthist[i]); fclose(f); } }
  if (ih_on) { for (int k = 0; k < 12; k++) { int b = -1; for (int i = 0; i < IH; i++) if (ih_cnt[i] && (b < 0 || ih_cnt[i] > ih_cnt[b])) b = i; if (b < 0) break; fprintf(stderr, "  indirect op %08x : %llu\n", ih_op[b], (unsigned long long)ih_cnt[b]); ih_cnt[b] = 0; } }
  if (do_profile && getenv("PCPROF")) {
    const char *fn = getenv("PCPROF"); sym_t *t = NULL;
    for (int i = 0; i < nsyms; i++) if (!strcmp(syms[i].name, fn)) t = &syms[i];
    if (t) { FILE *f = fopen("/tmp/pcprof.txt", "w"); for (u32 a = t->addr; a < t->addr + t->size; a += 4) fprintf(f, "%08x %llu\n", a, (unsigned long long)pcprof[a >> 2]); fclose(f); }
  }
  return exit_code;
}
