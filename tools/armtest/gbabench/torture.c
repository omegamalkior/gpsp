// torture.gba: exercises dynarec memory edge cases. Every result goes into the
// palette and is displayed, so any behaviour change shows up in the frame hash.
typedef unsigned int u32; typedef int s32; typedef unsigned short u16; typedef short s16;
typedef unsigned char u8; typedef signed char s8;
#define REG16(a) (*(volatile u16 *)(a))
#define REG32(a) (*(volatile u32 *)(a))
#define IWRAM_CODE __attribute__((section(".iwram"), target("arm"), noinline))

void *memcpy(void *d, const void *s, unsigned n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, unsigned n) { u8 *a = d; while (n--) *a++ = c; return d; }
volatile u32 irq_count;

static u32 res[64];
static int nres;
static void put(u32 v) { res[nres++ & 63] = v; }

typedef u32 (*fn0)(void);
#define IW_CODE ((volatile u16 *)0x03004000)
#define EW_CODE ((volatile u16 *)0x02020000)
#define IW_ARMCODE ((volatile u32 *)0x03004100)
#define EW_ARMCODE ((volatile u32 *)0x02020100)

// --- SMC: thumb code in RAM, modified with various store widths ---
static void smc_thumb(volatile u16 *code) {
  code[0] = 0x2000 | 11;   // movs r0, #11
  code[1] = 0x4770;        // bx lr
  fn0 f = (fn0)((u32)code | 1);
  put(f());
  code[0] = 0x2000 | 22; put(f());                         // strh
  *(volatile u8 *)code = 33; put(f());                      // strb
  *(volatile u32 *)code = (0x4770u << 16) | 0x2000 | 44; put(f()); // str
  // modify via stmia (block store)
  register u32 r1 __asm__("r1") = (0x4770u << 16) | 0x2000 | 55;
  register u32 r2 __asm__("r2") = 0x46C046C0;   // nop; nop
  register volatile u16 *r0 __asm__("r0") = code;
  __asm__ volatile("stmia %0!, {%1, %2}" : "+r"(r0) : "r"(r1), "r"(r2) : "memory");
  put(f());
}
// --- SMC: ARM code in RAM ---
static void smc_arm(volatile u32 *code) {
  code[0] = 0xE3A00000 | 7;  // mov r0, #7
  code[1] = 0xE12FFF1E;      // bx lr
  fn0 f = (fn0)(u32)code;
  put(f());
  code[0] = 0xE3A00000 | 9; put(f());
  ((volatile u16 *)code)[0] = 13; put(f());
  ((volatile u8 *)code)[0] = 17; put(f());
}

// --- unaligned and mirrored accesses (from Thumb) ---
static void unaligned(u32 base) {
  volatile u8 *b = (volatile u8 *)base;
  for (int i = 0; i < 16; i++) b[i] = 0x10 + i * 0x11;
  for (int o = 0; o < 4; o++) {
    put(*(volatile u32 *)(base + o));
    put(*(volatile u16 *)(base + o));
    put((u32)*(volatile s16 *)(base + o));
    put((u32)*(volatile s8 *)(base + o + 7));
  }
  *(volatile u16 *)(base + 5) = 0xBEEF;      // forced alignment
  *(volatile u32 *)(base + 9) = 0xCAFEBABE;  // forced alignment
  put(*(volatile u32 *)(base + 4)); put(*(volatile u32 *)(base + 8));
}
static void mirrors(void) {
  *(volatile u32 *)0x03006000 = 0x12345678;
  put(*(volatile u32 *)0x03FFE000);            // IWRAM mirror (0x3FFE000 & 0x7FFF = 0x6000)
  *(volatile u32 *)0x03FFE004 = 0x9ABCDEF0;
  put(*(volatile u32 *)0x03006004);
  *(volatile u16 *)0x02030000 = 0x5A5A;
  put(*(volatile u16 *)0x02F30000);            // EWRAM mirror
  *(volatile u8 *)0x02F30003 = 0x77;
  put(*(volatile u32 *)0x02030000);
  put(*(volatile u32 *)0x01000000);            // unmapped region (open bus)
  put(*(volatile u32 *)0x00000100);            // BIOS read from ROM code (protected)
  *(volatile u32 *)0x08000100 = 0xFFFFFFFF;   // write to ROM (ignored)
  put(*(volatile u32 *)0x08000100);
}

// --- block transfers crossing the mirror boundary ---
static void block_cross_inner(u32 addr) {
  register u32 a __asm__("r0") = addr;
  register u32 v1 __asm__("r1") = 0x11111111 ^ addr, v2 __asm__("r2") = 0x22222222, v3 __asm__("r3") = 0x33333333, v4 __asm__("r4") = 0x44444444 ^ addr;
  __asm__ volatile("stmia %0!, {%1, %2, %3, %4}" : "+r"(a) : "r"(v1), "r"(v2), "r"(v3), "r"(v4) : "memory");
  put(a);
  a = addr;
  __asm__ volatile("ldmia %0!, {%1, %2, %3, %4}" : "+r"(a), "=r"(v1), "=r"(v2), "=r"(v3), "=r"(v4) : : "memory");
  put(a); put(v1); put(v2); put(v3); put(v4);
}

static u32 wrap_addr(u32 a) {
  if ((a >> 24) == 3) return 0x03000000 | (a & 0x7FFF);
  if ((a >> 24) == 2) return 0x02000000 | (a & 0x3FFFF);
  return a;
}
static void block_cross(u32 addr) {
  u32 save[4];
  REG16(0x04000208) = 0;   // no IRQs while the IRQ vector/handler may be clobbered
  for (int i = 0; i < 4; i++) save[i] = *(volatile u32 *)wrap_addr(addr + i * 4);
  block_cross_inner(addr);
  for (int i = 0; i < 4; i++) *(volatile u32 *)wrap_addr(addr + i * 4) = save[i];
  REG16(0x04000208) = 1;
}

// --- flags must survive memory accesses (Thumb) ---
static u32 flags_mem(u32 x, u32 y, volatile u32 *p) {
  u32 out;
  __asm__ volatile(
    "  movs r5, #0\n"
    "  cmp %1, %2\n"
    "  ldr r6, [%3]\n"       // load between cmp and branch
    "  str r6, [%3, #4]\n"   // store too
    "  push {r6}\n"
    "  pop {r6}\n"
    "  bne 1f\n"
    "  adds r5, #1\n"
    "1:"
    "  bcc 2f\n"
    "  adds r5, #2\n"
    "2:"
    "  adds r6, %1, %2\n"    // may set V and C
    "  ldrh r7, [%3]\n"
    "  strb r7, [%3, #9]\n"
    "  bvc 3f\n"
    "  adds r5, #4\n"
    "3:"
    "  bmi 4f\n"
    "  adds r5, #8\n"
    "4:"
    "  bcc 5f\n"
    "  adds r5, #16\n"
    "5:"
    "  adds %0, r5, #0\n"
    : "=r"(out) : "r"(x), "r"(y), "r"(p) : "r5", "r6", "r7", "cc", "memory");
  return out;
}

// --- ARM mode equivalents (IWRAM) ---
IWRAM_CODE u32 arm_mem_tests(volatile u32 *p, u32 x, u32 y) {
  u32 acc = 0, t;
  for (int i = 0; i < 8; i++) p[i] = 0x01010101u * (i + 1);
  // flags live across loads/stores and LDM/STM
  __asm__ volatile(
    "  cmp %2, %3\n"
    "  ldr %1, [%4]\n"
    "  stmdb sp!, {%1}\n"
    "  ldmia sp!, {%1}\n"
    "  str %1, [%4, #8]\n"
    "  addeq %0, %0, #1\n"
    "  addcs %0, %0, #2\n"
    "  addmi %0, %0, #4\n"
    "  addvs %0, %0, #8\n"
    "  ldrh %1, [%4, #2]\n"
    "  addhi %0, %0, #16\n"
    : "+r"(acc), "=&r"(t) : "r"(x), "r"(y), "r"(p) : "cc", "memory");
  // unaligned ARM loads
  for (int o = 0; o < 4; o++) acc += *(volatile u32 *)((u32)p + o) + *(volatile u16 *)((u32)p + o) + *(volatile s16 *)((u32)p + o);
  // LDM/STM blocks
  u32 a = (u32)p;
  __asm__ volatile("ldmia %0, {r4-r7}\n add %1, %1, r4\n add %1, %1, r7\n stmib %0, {r4-r7}" : : "r"(a), "r"(acc) : "r4", "r5", "r6", "r7", "memory");
  acc += p[1] + p[4];
  return acc;
}
static u32 (*const arm_mem_ptr)(volatile u32 *, u32, u32) = arm_mem_tests;

static void io_stores(int frame) {
  // stores to IO/palette/VRAM/OAM regions from thumb (slow paths)
  volatile u16 *vram = (volatile u16 *)0x06008000;
  for (int i = 0; i < 64; i++) vram[i] = i * frame;
  volatile u8 *vb = (volatile u8 *)0x06008100;
  for (int i = 0; i < 16; i++) vb[i] = i;  // byte writes to VRAM (duplicated)
  put(*(volatile u32 *)0x06008100);
  volatile u32 *oam = (volatile u32 *)0x07000200;
  oam[0] = 0x200; put(oam[0]);
  REG16(0x04000010) = frame & 0x1FF; put(REG16(0x04000006) < 228);
}

static void dma_smc(void) {
  static const u16 newcode[2] = { 0x2000 | 99, 0x4770 };
  volatile u16 *code = (volatile u16 *)0x03004040;
  code[0] = 0x2000 | 1; code[1] = 0x4770;
  fn0 f = (fn0)((u32)code | 1);
  put(f());
  REG32(0x040000D4) = (u32)newcode; REG32(0x040000D8) = (u32)code; REG32(0x040000DC) = 2 | 0x80000000u;
  put(f());
}

static void show(void) {
  volatile u16 *pal = (volatile u16 *)0x05000000;
  for (int i = 0; i < 64; i++) { pal[i * 2 + 1] = res[i] & 0x7FFF; pal[i * 2 + 2] = (res[i] >> 15) & 0x7FFF; }
}

int main(void) {
  REG16(0x04000000) = (1 << 8);                 // mode 0, BG0
  REG16(0x04000008) = (1 << 7) | (30 << 8);     // 8bpp, screen 30
  volatile u32 *tiles = (volatile u32 *)0x06000000;
  for (int t = 0; t < 129; t++) for (int w = 0; w < 16; w++) tiles[t * 16 + w] = t * 0x01010101u;
  volatile u16 *map = (volatile u16 *)(0x06000000 + 30 * 0x800);
  for (int i = 0; i < 1024; i++) map[i] = 1 + (i % 128);
  REG16(0x04000200) = 1; REG16(0x04000004) = 1 << 3; REG16(0x04000208) = 1;
  u32 frame = 0;
  for (;;) {
    frame++; nres = 0;
    smc_thumb(IW_CODE); smc_thumb(EW_CODE);
    smc_arm(IW_ARMCODE); smc_arm(EW_ARMCODE);
    unaligned(0x03006100); unaligned(0x02010001 & ~3u); unaligned(0x02010003);
    mirrors();
    block_cross(0x03007FF8); block_cross(0x03007FF0); block_cross(0x0203FFF8); block_cross(0x02FFFFF8); block_cross(0x03006200);
    block_cross(0x05000100); block_cross(0x06009000);
    for (u32 x = 0; x < 4; x++) for (u32 y = 0; y < 4; y++) put(flags_mem(x * 0x40000000u, y * 0x3FFFFFFFu, (volatile u32 *)0x03006300));
    put(arm_mem_ptr((volatile u32 *)0x03006400, frame, 3));
    put(arm_mem_ptr((volatile u32 *)0x02012001, 0x80000000u, frame));
    io_stores(frame);
    dma_smc();
    show();
    __asm__ volatile("swi 5" ::: "r0", "r1", "r2", "r3", "memory");
  }
}
