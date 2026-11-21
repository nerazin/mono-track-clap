/* The test stimulus, shared with the firmware harness that produces the
 * expected hashes: notes, bends, tempo changes and moves of every page's
 * knobs and every LFO parameter, run through the track's main-CPU model. */
#ifndef STIMULUS_H
#define STIMULUS_H

static uint32_t st_seed;
static uint32_t st_rnd(uint32_t n) { st_seed = st_seed * 1664525u + 1013904223u; return (st_seed >> 8) % n; }
static uint32_t st_fnv(uint32_t h, int32_t v) {
   for (int b = 0; b < 3; ++b) h = (h ^ (((uint32_t)v >> (8 * b)) & 0xff)) * 16777619u;
   return h;
}

/* A kit at the unit's defaults for machine m. */
static void st_kit(MtHost *h, int m) {
   static const unsigned char syn[MT_MACHINES][8] = {
      {0, 0, 0, 0, 0, 0, 0, 64}, {0, 0, 0, 0, 0, 0, 0, 64}, {0, 0, 0, 0, 0, 0, 0, 64}, {0, 0, 0, 0, 64, 0, 0, 64},
      {63, 63, 63, 0, 64, 0, 127, 64}, {60, 64, 80, 30, 80, 64, 98, 64}, {60, 64, 80, 64, 102, 80, 98, 64}, {64, 64, 64, 64, 74, 80, 30, 64}};
   static const unsigned char page[24] = {0, 0, 64, 64, 64, 64, 64, 0, 0, 127, 0, 0, 0, 32, 64, 64, 64, 64, 0, 64, 64, 28, 0, 127};
   static const unsigned char lfo[8] = {0, 64, 0, 0, 0, 64, 0, 0};
   memset(h, 0, sizeof *h);
   for (int k = 0; k < 8; ++k) h->knob[k] = syn[m][k] << 16;
   for (int k = 0; k < 24; ++k) h->knob[8 + k] = page[k] << 16;
   for (int l = 0; l < 3; ++l) for (int k = 0; k < 8; ++k) h->raw[l][k] = lfo[k];
   h->level = 100 << 16;
   mt_host_reset(h);
   h->word[W_TICK] = 2880;
   h->word[W_RECIP] = 0x800000 / 2880;
   h->word[W_PITCH] = (60 << 11) / 12;
   h->word[W_ROUTE] = 1 | MT_LP_KEY | MT_HP_KEY;
}

/* What happens before frame f: edits, then the frame's block in h->block. */
static void st_frame(MtHost *h, long f) {
   h->word[W_NOTE] = h->word[W_FTRIG] = 0;
   if (f == 0) h->word[W_NOTE] = 0x80;
   else {
      if (st_rnd(4) == 0) { const int k = (int)st_rnd(32); h->knob[k] = (int32_t)st_rnd(128) << 16; }
      if (st_rnd(6) == 0) { const int l = (int)st_rnd(3), k = (int)st_rnd(8); h->raw[l][k] = (int32_t)st_rnd(128); }
      if (st_rnd(60) == 0) h->level = (int32_t)st_rnd(128) << 16;
      if (st_rnd(90) == 0) { h->word[W_TICK] = 720 + (int32_t)st_rnd(6481); h->word[W_RECIP] = 0x800000 / h->word[W_TICK]; }
      if (st_rnd(40) == 0) h->bend = mf_bend((int)st_rnd(256), (int)st_rnd(8));
      if (st_rnd(120) == 0) h->word[W_ROUTE] = 1 | (st_rnd(2) ? MT_LP_KEY : 0) | (st_rnd(2) ? MT_HP_KEY : 0);
      if (st_rnd(120) == 0) h->word[W_MTUNE] = mf_master_tune(4000 + (int)st_rnd(1000));
      const uint32_t n = st_rnd(16);
      if (n == 0) { h->word[W_NOTE] = 0x81; h->word[W_FTRIG] = 1; h->word[W_PITCH] = (int32_t)((12 + st_rnd(97)) << 11) / 12; h->lfo_trig = 1; }
      else if (n == 1) h->word[W_NOTE] = 2;
   }
   mt_host_frame(h, NULL);
}

#endif
