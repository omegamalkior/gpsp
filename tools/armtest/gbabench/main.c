// gbabench: synthetic but "commercial-game-shaped" GBA workload for gpSP testing.
// Thumb game logic in ROM, ARM mixer in IWRAM, sprites + 2 BG layers,
// VBlank IRQ + VBlankIntrWait, BIOS Div, DMA, function pointers, jump tables.
typedef unsigned int u32; typedef int s32; typedef unsigned short u16; typedef short s16;
typedef unsigned char u8; typedef signed char s8;
#define REG16(a) (*(volatile u16 *)(a))
#define REG32(a) (*(volatile u32 *)(a))
#define IWRAM_CODE __attribute__((section(".iwram"), target("arm"), noinline))
#define EWRAM_BSS __attribute__((section(".ewram")))

void *memcpy(void *d, const void *s, unsigned n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, unsigned n) { u8 *a = d; while (n--) *a++ = c; return d; }

volatile u32 irq_count;

struct ent { s32 x, y, vx, vy; u16 type, hp, tile, flags; s32 timer; };
#define NENT 96
EWRAM_BSS static struct ent ents[NENT];
static u16 oam_shadow[128 * 4];
EWRAM_BSS static s8 mixbuf[2][608];
EWRAM_BSS static u8 decomp[4096];
static u32 rng = 12345;
static u32 frame;
static s32 cam_x, cam_y;
static u32 checksum;

static u32 rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 16; }

static int bios_div(int n, int d) {
  register int r0 __asm__("r0") = n; register int r1 __asm__("r1") = d;
  __asm__ volatile("swi 6" : "+r"(r0), "+r"(r1) : : "r2", "r3", "memory");
  return r0;
}
static void vblank_wait(void) { __asm__ volatile("swi 5" ::: "r0", "r1", "r2", "r3", "memory"); }

// ---- entity behaviours (called through a function pointer table) ----
static void upd_walker(struct ent *e) {
  e->x += e->vx; e->y += e->vy;
  if (e->x < 0 || e->x > (480 << 8)) e->vx = -e->vx;
  if (e->y < 0 || e->y > (320 << 8)) e->vy = -e->vy;
}
static void upd_chaser(struct ent *e) {
  s32 dx = (ents[0].x - e->x) >> 6, dy = (ents[0].y - e->y) >> 6;
  e->vx = (e->vx * 7 + dx) >> 3; e->vy = (e->vy * 7 + dy) >> 3;
  e->x += e->vx; e->y += e->vy;
}
static void upd_bouncer(struct ent *e) {
  e->vy += 24; e->x += e->vx; e->y += e->vy;
  if (e->y > (300 << 8)) { e->y = 300 << 8; e->vy = -(e->vy * 3) / 4 - 300; }
  if (e->x < 0 || e->x > (480 << 8)) e->vx = -e->vx;
}
static void upd_spinner(struct ent *e) {
  e->timer++;
  switch (e->timer & 7) {
  case 0: e->x += 256; break; case 1: e->y += 256; break; case 2: e->x -= 128; break;
  case 3: e->y -= 128; break; case 4: e->x += e->hp; break; case 5: e->y += e->hp & 15; break;
  case 6: e->x -= e->hp & 31; break; default: e->y -= 300; break;
  }
  if (e->x > (480 << 8)) e->x = 0; if (e->y > (320 << 8)) e->y = 0;
  if (e->x < 0) e->x = 480 << 8; if (e->y < 0) e->y = 320 << 8;
}
typedef void (*upd_fn)(struct ent *);
static const upd_fn behaviours[4] = { upd_walker, upd_chaser, upd_bouncer, upd_spinner };

static void collide(void) {
  for (int i = 0; i < 48; i++) {
    struct ent *a = &ents[i];
    for (int j = i + 1; j < 48; j++) {
      struct ent *b = &ents[j];
      s32 dx = (a->x - b->x) >> 8, dy = (a->y - b->y) >> 8;
      if (dx < 0) dx = -dx; if (dy < 0) dy = -dy;
      if (dx < 12 && dy < 12) {
        s32 t = a->vx; a->vx = b->vx; b->vx = t;
        t = a->vy; a->vy = b->vy; b->vy = t;
        if (a->hp) a->hp--; if (b->hp) b->hp--;
        checksum += i * 31 + j;
      }
    }
  }
}

// ---- ARM mixer in IWRAM (like the m4a/Sappy engine) ----
struct chan { const s8 *wave; u32 pos, step, len; s32 vol; };
static struct chan chans[4];
extern const s8 waves[4][256];
IWRAM_CODE void mix_frame(s8 *out, int n, struct chan *ch) {
  static s16 acc[608];
  for (int i = 0; i < n; i++) acc[i] = 0;
  for (int c = 0; c < 4; c++) {
    const s8 *w = ch[c].wave; u32 pos = ch[c].pos, step = ch[c].step, len = ch[c].len << 12; s32 vol = ch[c].vol;
    for (int i = 0; i < n; i++) {
      acc[i] += (w[pos >> 12] * vol) >> 4;
      pos += step; if (pos >= len) pos -= len;
    }
    ch[c].pos = pos;
  }
  for (int i = 0; i < n; i++) { s32 v = acc[i] >> 2; if (v > 127) v = 127; if (v < -128) v = -128; out[i] = v; }
}
static void (*const mix_ptr)(s8 *, int, struct chan *) = mix_frame;

// ---- simple LZ-style decompressor (Thumb, byte loads from ROM) ----
extern const u8 lzdata[];
extern const u32 lzdata_len;
static u32 decompress(u8 *dst, const u8 *src, u32 srclen) {
  u32 o = 0, i = 0;
  while (i < srclen && o < sizeof decomp) {
    u8 flags = src[i++];
    for (int b = 0; b < 8 && i < srclen; b++) {
      if (flags & (0x80 >> b)) {
        u32 d = ((src[i] & 0xF) << 8) | src[i + 1]; u32 l = (src[i] >> 4) + 3; i += 2;
        for (u32 k = 0; k < l && o < sizeof decomp; k++, o++) dst[o] = dst[o - d - 1];
      } else if (o < sizeof decomp) dst[o++] = src[i++];
    }
  }
  return o;
}


// ---- call-heavy code: comparator sort, small recursive calls, bytecode VM ----
static struct ent *order[NENT];
static __attribute__((noinline)) int cmp_y(const struct ent *a, const struct ent *b) { return a->y - b->y; }
static __attribute__((noinline)) int cmp_x(const struct ent *a, const struct ent *b) { return a->x - b->x; }
static __attribute__((noinline)) void isort(struct ent **arr, int n, int (*cmp)(const struct ent *, const struct ent *)) {
  for (int i = 1; i < n; i++) { struct ent *k = arr[i]; int j = i - 1;
    while (j >= 0 && cmp(arr[j], k) > 0) { arr[j + 1] = arr[j]; j--; } arr[j + 1] = k; }
}
static __attribute__((noinline)) int f_small(int x) { return x * 3 + 1; }
static __attribute__((noinline)) int f_rec(int n) { return n < 2 ? n : f_rec(n - 1) + f_small(n - 2); }
static const u8 script[] = { 1,5, 2,3, 3, 4,7, 5, 6,2, 1,9, 7, 2,1, 3, 8, 4,3, 5, 6,1, 7, 0 };
static __attribute__((noinline)) s32 run_vm(s32 seed) {
  s32 acc = seed, reg = 1; u32 pc = 0;
  for (int steps = 0; steps < 400; steps++) {
    u8 op = script[pc++];
    switch (op) {
    case 0: pc = 0; reg++; break;
    case 1: acc += script[pc++]; break;
    case 2: acc *= script[pc++]; break;
    case 3: acc ^= reg; break;
    case 4: acc -= script[pc++] * reg; break;
    case 5: acc = (acc << 1) | (acc >> 31); break;
    case 6: reg += script[pc++]; break;
    case 7: acc = f_small(acc); break;
    default: acc = ~acc; break;
    }
  }
  return acc;
}

static void setup_gfx(void) {
  REG16(0x04000000) = 0x0000 | (1 << 8) | (1 << 9) | (1 << 12) | (1 << 6); // mode0, BG0, BG1, OBJ, 1D
  REG16(0x04000008) = (0 << 2) | (30 << 8) | 1;  // BG0: char 0, screen 30, prio 1
  REG16(0x0400000A) = (0 << 2) | (31 << 8) | 2;  // BG1
  volatile u16 *pal = (volatile u16 *)0x05000000;
  for (int i = 0; i < 256; i++) { pal[i] = (i * 0x0421 + (i << 3)) & 0x7FFF; pal[256 + i] = (0x7FFF - i * 0x111) & 0x7FFF; }
  volatile u32 *tiles = (volatile u32 *)0x06000000;
  for (int t = 0; t < 64; t++) for (int r = 0; r < 8; r++) tiles[t * 8 + r] = 0x11111111u * ((t + r) & 15) ^ (r * 0x01020304u);
  volatile u16 *map0 = (volatile u16 *)(0x06000000 + 30 * 0x800), *map1 = (volatile u16 *)(0x06000000 + 31 * 0x800);
  for (int i = 0; i < 1024; i++) { map0[i] = (i * 7) & 63; map1[i] = ((i * 13) & 63) | ((i & 3) << 12) | ((i & 0x40) ? 0x400 : 0); }
  volatile u32 *otiles = (volatile u32 *)0x06010000;
  for (int t = 0; t < 32; t++) for (int r = 0; r < 8; r++) otiles[t * 8 + r] = (r == 0 || r == 7) ? 0x22222222 : 0x23333332u + t;
  REG16(0x04000200) = 1;          // IE = vblank
  REG16(0x04000004) = 1 << 3;     // DISPSTAT vblank irq
  REG16(0x04000208) = 1;          // IME
}

int main(void) {
  setup_gfx();
  for (int i = 0; i < NENT; i++) {
    ents[i].x = ((rnd() * 480u) >> 16) << 8; ents[i].y = ((rnd() * 320u) >> 16) << 8;
    ents[i].vx = (s32)(rnd() & 511) - 256; ents[i].vy = (s32)(rnd() & 511) - 256;
    ents[i].type = i & 3; ents[i].hp = 100 + (i & 31); ents[i].tile = i & 31; ents[i].timer = i;
  }
  for (int c = 0; c < 4; c++) { chans[c].wave = waves[c]; chans[c].step = 0x800 + c * 0x3A1; chans[c].len = 256; chans[c].vol = 3 + c * 4; }
  for (;;) {
    frame++;
    for (int i = 0; i < NENT; i++) behaviours[ents[i].type](&ents[i]);
    collide();
    for (int i = 0; i < NENT; i++) order[i] = &ents[i];
    isort(order, 64, (frame & 1) ? cmp_x : cmp_y);
    checksum += order[0]->hp + f_rec(12) + run_vm(frame);
    mix_ptr(mixbuf[frame & 1], 304, chans);
    u32 dlen = decompress(decomp, lzdata, lzdata_len);
    checksum += dlen + decomp[frame & 1023];
    // divisions through the BIOS (like many games' fixed point code)
    for (int i = 0; i < 40; i++) checksum += bios_div((s32)(rnd() * 1000) + 1, (s32)(i + 3));
    // camera follows entity 0
    cam_x = (ents[0].x >> 8) - 120; cam_y = (ents[0].y >> 8) - 80;
    // build OAM shadow
    for (int i = 0; i < 128; i++) {
      u16 *o = &oam_shadow[i * 4];
      if (i < NENT) {
        s32 sx = (ents[i].x >> 8) - cam_x, sy = (ents[i].y >> 8) - cam_y;
        if (sx < -16 || sx > 240 || sy < -16 || sy > 160) { o[0] = 0x0200; continue; }
        o[0] = (sy & 0xFF) | ((ents[i].hp & 1) ? 0x0400 : 0);  // some semi-transparent
        o[1] = (sx & 0x1FF) | (1 << 14);                       // 16x16
        o[2] = (ents[i].tile * 4) | ((i & 1) << 10);
      } else o[0] = 0x0200;
    }
    vblank_wait();
    // DMA3 OAM upload
    REG32(0x040000D4) = (u32)oam_shadow; REG32(0x040000D8) = 0x07000000; REG32(0x040000DC) = (128 * 4 / 2) | 0x80000000u;
    REG16(0x04000010) = cam_x & 0x1FF; REG16(0x04000012) = cam_y & 0x1FF;
    REG16(0x04000014) = (cam_x >> 1) & 0x1FF; REG16(0x04000016) = (cam_y >> 1) & 0x1FF;
    REG16(0x04000050) = (1 << 1) | (1 << 6) | (1 << 8); // blend BG1 over BG0
    REG16(0x04000052) = 0x0808;
  }
}
