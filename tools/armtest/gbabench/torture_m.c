// torture_m.gba: memory accesses to every region, from Thumb/ARM code in ROM
// and ARM code in IWRAM: single loads of all widths, and block transfers
// (LDM/STM) to/from VRAM, ROM, palette, OAM, I/O and unmapped areas. Every
// result is hashed into the palette, so any behaviour change shows up in the
// frame hash.
typedef unsigned int u32; typedef int s32; typedef unsigned short u16; typedef short s16;
typedef unsigned char u8; typedef signed char s8;
#define REG16(a) (*(volatile u16 *)(a))
#define REG32(a) (*(volatile u32 *)(a))
#define ARM_ROM __attribute__((target("arm"), noinline))
#define ARM_IW  __attribute__((section(".iwram"), target("arm"), noinline))
#define NOINL   __attribute__((noinline))

void *memcpy(void *d, const void *s, unsigned n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, unsigned n) { u8 *a = d; while (n--) *a++ = c; return d; }
volatile u32 irq_count;

static u32 res[64];
static u32 nres;
static NOINL void put(u32 v) { u32 i = nres++ & 63; res[i] = res[i] * 31 + v + nres; }

const u32 romtab[64] = {
  0x80FF7F01, 0x12345678, 0x9ABCDEF0, 0x0F1E2D3C, 0x4B5A6978, 0x8796A5B4, 0xC3D2E1F0, 0xFFFFFFFF,
  0x00000000, 0x7FFF8000, 0x80007FFF, 0x01020304, 0x05060708, 0x090A0B0C, 0x0D0E0F10, 0x11121314,
  0xDEADBEEF, 0xCAFEBABE, 0x0BADF00D, 0xFEEDFACE, 0x8BADF00D, 0xDEFEC8ED, 0xD15EA5E5, 0xE011CFD0,
  0x13579BDF, 0x2468ACE0, 0xFDB97531, 0x0ECA8642, 0x55AA55AA, 0xAA55AA55, 0x00FF00FF, 0xFF00FF00,
  1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32 };

// Single loads of every width. The same source is built three times.
#define LOADS_BODY \
  u32 h = 0; \
  h = h * 33 + *(volatile u8 *)a; \
  h = h * 33 + (u32)*(volatile s8 *)a; \
  h = h * 33 + *(volatile u16 *)a; \
  h = h * 33 + (u32)*(volatile s16 *)a; \
  h = h * 33 + *(volatile u32 *)a; \
  h = h * 33 + *(volatile u8 *)(a + 3); \
  h = h * 33 + (u32)*(volatile s8 *)(a + 1); \
  h = h * 33 + *(volatile u16 *)(a + 2); \
  h = h * 33 + (u32)*(volatile s16 *)(a + 6); \
  h = h * 33 + *(volatile u32 *)(a + 4); \
  return h;
static NOINL u32 loads_thumb(u32 a) { LOADS_BODY }
static ARM_ROM u32 loads_arm(u32 a) { LOADS_BODY }
static ARM_IW u32 loads_iw(u32 a) { LOADS_BODY }

// Polls VCOUNT/DISPSTAT a few times, like wait loops do
#define POLL_BODY \
  u32 h = 0; \
  for (u32 i = 0; i < n; i++) { h += REG16(0x04000006); h ^= REG16(0x04000004) << 8; h += *(volatile u8 *)0x04000006; h += REG32(0x04000004); } \
  return h;
static NOINL u32 poll_thumb(u32 n) { POLL_BODY }
static ARM_ROM u32 poll_arm(u32 n) { POLL_BODY }
static ARM_IW u32 poll_iw(u32 n) { POLL_BODY }

// Block copies: src -> dst, 4 words (Thumb) or 8 words (ARM), like CpuFastSet
u32 copy4_thumb(u32 src, u32 dst);
u32 copy8_arm(u32 src, u32 dst);
u32 copy8_iw(u32 src, u32 dst);
u32 copy3_arm_db(u32 src, u32 dst);
__asm__(
"  .text\n  .thumb\n  .thumb_func\n  .global copy4_thumb\ncopy4_thumb:\n"
"  push {r4-r5}\n  ldmia r0!, {r2-r5}\n  stmia r1!, {r2-r5}\n  eors r0, r1\n  pop {r4-r5}\n  bx lr\n"
"  .arm\n  .align 2\n  .global copy8_arm\ncopy8_arm:\n"
"  stmfd sp!, {r4-r9}\n  ldmia r0!, {r2-r9}\n  stmia r1!, {r2-r9}\n  eor r0, r0, r1\n  ldmfd sp!, {r4-r9}\n  bx lr\n"
"  .global copy3_arm_db\ncopy3_arm_db:\n"
"  stmfd sp!, {r4}\n  ldmdb r0, {r2-r4}\n  stmdb r1!, {r2-r4}\n  mov r0, r1\n  ldmfd sp!, {r4}\n  bx lr\n"
"  .section .iwram, \"ax\"\n  .arm\n  .align 2\n  .global copy8_iw\ncopy8_iw:\n"
"  stmfd sp!, {r4-r9}\n  ldmia r0!, {r2-r9}\n  stmia r1!, {r2-r9}\n  eor r0, r0, r1\n  ldmfd sp!, {r4-r9}\n  bx lr\n"
"  .text\n  .thumb\n");

static u32 buf[16];

static NOINL u32 sum(volatile u32 *p, u32 n) { u32 h = 0; while (n--) h = h * 7 + *p++; return h; }

static NOINL void block_loads(u32 src) {
  for (int i = 0; i < 16; i++) buf[i] = 0xA5A50000 + i;
  put(copy4_thumb(src, (u32)buf)); put(sum(buf, 4));
  put(copy8_arm(src, (u32)buf)); put(sum(buf, 8));
  put(copy8_iw(src, (u32)(buf + 4))); put(sum(buf, 12));
  put(copy3_arm_db(src + 12, (u32)(buf + 16))); put(sum(buf, 16));
}

static NOINL void block_stores(u32 dst, u32 seed) {
  for (int i = 0; i < 16; i++) buf[i] = seed * 0x9E3779B1u + i * 0x01000193u;
  put(copy4_thumb((u32)buf, dst)); put(sum((volatile u32 *)(dst & ~3u), 4));
  put(copy8_arm((u32)(buf + 2), dst)); put(sum((volatile u32 *)(dst & ~3u), 8));
  put(copy8_iw((u32)(buf + 5), dst + 4)); put(sum((volatile u32 *)(dst & ~3u), 9));
  put(copy3_arm_db((u32)(buf + 12), dst + 12)); put(sum((volatile u32 *)(dst & ~3u), 9));
}

static void show(void) {
  volatile u16 *pal = (volatile u16 *)0x05000000;
  for (int i = 0; i < 64; i++) { pal[i * 2 + 1] = res[i] & 0x7FFF; pal[i * 2 + 2] = (res[i] >> 15) & 0x7FFF; }
}

static const u32 load_addrs[] = {
  0x08000000, (u32)romtab, (u32)romtab + 0x02000000, (u32)romtab + 0x04000000, 0x09FFFFF0, 0x0BFFFFF8,
  0x0C000010, 0x0CFFFFF8, 0x0D000000, 0x0E000000, 0x08007FF8, 0x08007FFC, 0x0800FFF0,
  0x00000000, 0x00003FF8, 0x01000000, 0x10000000, 0xFFFFFFF0,
  0x04000000, 0x04000004, 0x04000008, 0x0400000C, 0x04000010, 0x04000100, 0x04000130, 0x04000200, 0x04000400, 0x04FFFFF0,
  0x05000000, 0x050003F8, 0x05000400, 0x06000000, 0x06017FF8, 0x06018000, 0x0601FFF8, 0x06020000, 0x06A14000,
  0x07000000, 0x070003F8, 0x02000000, 0x0203FFF8, 0x02040000, 0x03000000, 0x03007FF8, 0x03FFFFF0 };

static const u32 store_addrs[] = {
  0x06004000, 0x06017FE0, 0x06017FF0, 0x06017FF8, 0x06018000, 0x0601FFE0, 0x0601FFF8, 0x06020000, 0x06A14010, 0x06FFFFF0,
  0x05000180, 0x050003F0, 0x07000100, 0x070003F0, 0x04000010, 0x04000020,
  0x02001000, 0x0203FFF0, 0x03006800, 0x03007FE0 & 0x03006FE0, 0x0E000000, 0x10000000 };

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
    for (u32 i = 0; i < sizeof(load_addrs) / 4; i++) {
      u32 a = load_addrs[i];
      for (u32 o = 0; o < 4; o++) { put(loads_thumb(a + o)); put(loads_arm(a + o)); put(loads_iw(a + o)); }
      block_loads(a); block_loads(a + 1); block_loads(a + 2);
    }
    put(poll_thumb(40 + (frame & 7))); put(poll_arm(33 + (frame & 3))); put(poll_iw(21 + (frame & 15)));
    for (u32 i = 0; i < sizeof(store_addrs) / 4; i++) { block_stores(store_addrs[i], frame + i); block_stores(store_addrs[i] + 2, frame * 3 + i); }
    // restore what the VRAM/OAM/IO stores trashed, then show
    REG16(0x04000010) = 0; REG16(0x04000012) = 0; REG32(0x04000020) = 0x100; REG32(0x04000024) = 0x1000000; REG32(0x04000028) = 0; REG32(0x0400002C) = 0;
    for (int t = 0; t < 129; t++) for (int w = 0; w < 16; w++) tiles[t * 16 + w] = t * 0x01010101u;
    for (int i = 0; i < 1024; i++) map[i] = 1 + (i % 128);
    put(poll_thumb(9)); put(poll_arm(11)); put(poll_iw(13));
    show();
    __asm__ volatile("swi 5" ::: "r0", "r1", "r2", "r3", "memory");
  }
}
