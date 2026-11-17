// stm_ignore.gba: STM (Thumb and ARM) into address ranges whose writes are
// ignored (BIOS, 0x01, gamepak 0x09-0x0C, 0x0F, >= 0x10), followed by code
// whose results are shown in the palette. With the interpreter as reference,
// any difference in the frame hash means the dynarec misbehaved.
typedef unsigned int u32; typedef unsigned short u16;
#define REG16(a) (*(volatile u16 *)(a))
void *memcpy(void *d, const void *s, unsigned n) { char *a = d; const char *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, unsigned n) { char *a = d; while (n--) *a++ = c; return d; }
volatile u32 irq_count;
static u32 res[64];
static int nres;
static void put(u32 v) { res[nres++ & 63] = v; }

__attribute__((target("thumb"), noinline)) static u32 stm_thumb(u32 addr, u32 seed) {
  u32 c = 0;
  __asm__ volatile(
    ".syntax unified\n"
    "movs r4, %[addr]\n"
    "movs r5, %[s]\n"
    "adds r6, r5, #3\n"
    "stmia r4!, {r5, r6}\n"
    "adds %[c], %[c], #1\n"
    "adds %[c], %[c], #2\n"
    "adds %[c], %[c], r6\n"
    "movs %[addr], r4\n"
    : [addr] "+l"(addr), [c] "+l"(c)
    : [s] "l"(seed)
    : "r4", "r5", "r6", "memory", "cc");
  return addr ^ (c << 8);
}

__attribute__((target("arm"), noinline)) static u32 stm_arm(u32 addr, u32 seed) {
  u32 c = 0, d = 5;
  __asm__ volatile(
    "mov r4, %[addr]\n"
    "mov r5, %[s]\n"
    "add r6, %[s], #1\n"
    "mov r7, #3\n"
    "stmia r4!, {r5, r6, r7}\n"
    "add %[c], %[c], #1\n"
    "add %[c], %[c], #2\n"
    "add %[d], %[d], %[c]\n"
    "mov %[addr], r4\n"
    : [addr] "+r"(addr), [c] "+r"(c), [d] "+r"(d)
    : [s] "r"(seed)
    : "r4", "r5", "r6", "r7", "memory", "cc");
  return addr ^ (c << 8) ^ (d << 16);
}

/* STM that starts in an ignored area and ends in EWRAM (0x01FFFFF8 -> 0x02000000) */
__attribute__((target("thumb"), noinline)) static u32 stm_cross_thumb(u32 seed) {
  volatile u32 *ew = (volatile u32 *)0x02000000;
  ew[0] = 0; ew[1] = 0;
  __asm__ volatile(
    ".syntax unified\n"
    "ldr r3, =0x01FFFFF8\n"
    "movs r4, %[s]\n"
    "adds r5, r4, #1\n"
    "adds r6, r4, #2\n"
    "stmia r3!, {r4, r5, r6}\n"
    :: [s] "l"(seed) : "r3", "r4", "r5", "r6", "memory", "cc");
  return ew[0] ^ (ew[1] << 12);
}

__attribute__((target("arm"), noinline)) static u32 stm_cross_arm(u32 seed) {
  volatile u32 *ew = (volatile u32 *)0x02000010;
  ew[0] = 0; ew[1] = 0;
  __asm__ volatile(
    "ldr r4, =0x01FFFFF8 + 0x10\n"
    "mov r8, %[s]\n"
    "add r9, %[s], #1\n"
    "add r10, %[s], #2\n"
    "stmia r4, {r8, r9, r10}\n"
    :: [s] "r"(seed) : "r4", "r8", "r9", "r10", "memory", "cc");
  return ew[0] ^ (ew[1] << 12);
}

static void show(void) {
  volatile u16 *pal = (volatile u16 *)0x05000000;
  for (int i = 0; i < 64; i++) { pal[i * 2 + 1] = res[i] & 0x7FFF; pal[i * 2 + 2] = (res[i] >> 15) & 0x7FFF; }
}

int main(void) {
  static const u32 bases[] = { 0x00000100, 0x01000000, 0x09000000, 0x0A000000,
                               0x0B000000, 0x0C000000, 0x0F000000, 0x10000000 };
  REG16(0x04000000) = (1 << 8);
  REG16(0x04000008) = (1 << 7) | (30 << 8);
  volatile u32 *tiles = (volatile u32 *)0x06000000;
  for (int t = 0; t < 129; t++) for (int w = 0; w < 16; w++) tiles[t * 16 + w] = t * 0x01010101u;
  volatile u16 *map = (volatile u16 *)(0x06000000 + 30 * 0x800);
  for (int i = 0; i < 1024; i++) map[i] = 1 + (i % 128);
  REG16(0x04000200) = 1; REG16(0x04000004) = 1 << 3; REG16(0x04000208) = 1;
  u32 frame = 0;
  for (;;) {
    frame++; nres = 0;
#ifndef ONLY
    for (int i = 0; i < 8; i++) { put(stm_thumb(bases[i], frame)); put(stm_arm(bases[i], frame)); }
#elif ONLY < 8
    put(stm_thumb(bases[ONLY], frame)); put(stm_arm(bases[ONLY], frame));
#endif
#if !defined(ONLY) || ONLY == 8
    put(stm_cross_thumb(frame)); put(stm_cross_arm(frame));
#endif
    show();
    __asm__ volatile("swi 5" ::: "r0", "r1", "r2", "r3", "memory");
  }
}
