// Headless libretro runner for gpSP: runs N frames, hashes video+audio,
// reports timing. Usage: run core.so rom.gba frames [drc=enabled|disabled] [hashfile]
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "libretro.h"

static const char *opt_drc = "enabled";
static const char *opt_frameskip = "disabled";
static uint64_t vhash = 1469598103934665603ULL, ahash = 1469598103934665603ULL;
static uint64_t frame_vhash;
static unsigned frame_no;
static FILE *hf;
static unsigned audio_frames;

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
  const uint8_t *b = p; for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; } return h;
}

static bool env(unsigned cmd, void *data) {
  switch (cmd) {
  case RETRO_ENVIRONMENT_GET_VARIABLE: {
    struct retro_variable *v = data;
    if (!strcmp(v->key, "gpsp_drc")) { v->value = opt_drc; return true; }
    if (!strcmp(v->key, "gpsp_bios")) { v->value = "builtin"; return true; }
    if (!strcmp(v->key, "gpsp_frameskip")) { v->value = opt_frameskip; return true; }
    v->value = NULL; return false;
  }
  case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: return true;
  case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
  case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    *(const char **)data = "/tmp/gpsp_sys"; return true;
  case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool *)data = true; return true;
  default: return false;
  }
}
static uint16_t lastfb[240*160]; static unsigned lw, lh;
static void video(const void *d, unsigned w, unsigned h, size_t pitch) {
  if (!d) return;
  lw = w; lh = h; for (unsigned y = 0; y < h && y < 160; y++) memcpy(lastfb + y*240, (const uint8_t*)d + y*pitch, (w>240?240:w)*2);
  uint64_t fh = 1469598103934665603ULL;
  for (unsigned y = 0; y < h; y++) fh = fnv(fh, (const uint8_t *)d + y * pitch, w * 2);
  frame_vhash = fh;
  vhash = fnv(vhash, &fh, 8);
}
static void audio(int16_t l, int16_t r) { int16_t s[2] = {l, r}; ahash = fnv(ahash, s, 4); audio_frames++; }
static size_t audio_batch(const int16_t *d, size_t f) { ahash = fnv(ahash, d, f * 4); audio_frames += f; return f; }
static void poll(void) {}
// Scripted input: tap START / A periodically so games get past title screens,
// and hold RIGHT for stretches to make gameplay happen.
static int16_t input(unsigned port, unsigned dev, unsigned idx, unsigned id) {
  if (port) return 0;
  unsigned f = frame_no;
  if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
    int16_t m = 0;
    if ((f % 90) < 4 && f > 60) m |= 1 << RETRO_DEVICE_ID_JOYPAD_START;
    if ((f % 45) < 3 && f > 120) m |= 1 << RETRO_DEVICE_ID_JOYPAD_A;
    if ((f % 600) > 300) m |= 1 << RETRO_DEVICE_ID_JOYPAD_RIGHT;
    if ((f % 70) < 10 && f > 400) m |= 1 << RETRO_DEVICE_ID_JOYPAD_B;
    return m;
  }
  if (id == RETRO_DEVICE_ID_JOYPAD_START) return (f % 90) < 4 && f > 60;
  if (id == RETRO_DEVICE_ID_JOYPAD_A) return (f % 45) < 3 && f > 120;
  if (id == RETRO_DEVICE_ID_JOYPAD_RIGHT) return (f % 600) > 300;
  if (id == RETRO_DEVICE_ID_JOYPAD_B) return (f % 70) < 10 && f > 400;
  return 0;
}

#define SYM(n) n = dlsym(h, #n); if (!n) { fprintf(stderr, "missing %s\n", #n); return 1; }
int main(int argc, char **argv) {
  if (argc < 4) { fprintf(stderr, "usage\n"); return 1; }
  void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
  if (argc > 4) opt_drc = argv[4];
  if (argc > 5) hf = fopen(argv[5], "w");
  if (getenv("FRAMESKIP")) opt_frameskip = getenv("FRAMESKIP");
  void (*retro_set_environment)(retro_environment_t); void (*retro_set_video_refresh)(retro_video_refresh_t);
  void (*retro_set_audio_sample)(retro_audio_sample_t); void (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
  void (*retro_set_input_poll)(retro_input_poll_t); void (*retro_set_input_state)(retro_input_state_t);
  void (*retro_init)(void); bool (*retro_load_game)(const struct retro_game_info *); void (*retro_run)(void);
  void (*retro_unload_game)(void); void (*retro_deinit)(void);
  SYM(retro_set_environment) SYM(retro_set_video_refresh) SYM(retro_set_audio_sample) SYM(retro_set_audio_sample_batch)
  SYM(retro_set_input_poll) SYM(retro_set_input_state) SYM(retro_init) SYM(retro_load_game) SYM(retro_run)
  SYM(retro_unload_game) SYM(retro_deinit)
  system("mkdir -p /tmp/gpsp_sys");
  retro_set_environment(env); retro_set_video_refresh(video); retro_set_audio_sample(audio);
  retro_set_audio_sample_batch(audio_batch); retro_set_input_poll(poll); retro_set_input_state(input);
  retro_init();
  FILE *f = fopen(argv[2], "rb"); if (!f) { perror("rom"); return 1; }
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  void *buf = malloc(sz); fread(buf, 1, sz, f); fclose(f);
  struct retro_game_info gi = { argv[2], buf, (size_t)sz, NULL };
  if (!retro_load_game(&gi)) { fprintf(stderr, "load failed\n"); return 1; }
  unsigned frames = atoi(argv[3]);
  struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
  for (frame_no = 0; frame_no < frames; frame_no++) {
    retro_run();
    if (hf) fprintf(hf, "%u %016llx\n", frame_no, (unsigned long long)frame_vhash);
  }
  clock_gettime(CLOCK_MONOTONIC, &t1);
  double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
  printf("frames=%u time=%.3fs fps=%.1f video=%016llx audio=%016llx asamples=%u\n", frames, s, frames / s,
         (unsigned long long)vhash, (unsigned long long)ahash, audio_frames);
  if (getenv("DUMP_PPM")) { FILE *p = fopen(getenv("DUMP_PPM"), "wb"); fprintf(p, "P6 240 160 255\n");
    for (int i = 0; i < 240*160; i++) { uint16_t c = lastfb[i]; uint8_t rgb[3] = {(c>>11)<<3, ((c>>5)&63)<<2, (c&31)<<3}; fwrite(rgb,1,3,p);} fclose(p); }
  if (hf) fclose(hf);
  retro_unload_game(); retro_deinit();
  return 0;
}
