/* Regression test for mono_track.h that needs no firmware.
 *
 * The expected hashes come from Monomachine OS 1.32B itself. For each machine
 * the stimulus in stimulus.h (notes, bends, tempo, and moves of every knob on
 * every page and every LFO parameter) went through the firmware's whole track
 * in an emulator, and the 32 output words of each block were hashed. The
 * main-CPU hash comes from running the main OS's own LFO, apply and slew code
 * in a 68020 emulator (lab golden_host.py). */
#include <stdio.h>
#include <stdlib.h>
#include "../source/mono_track.h"
#include "stimulus.h"

static const uint32_t expect_track[MT_MACHINES] = {0xa26f567au, 0x8180bae7u, 0xb5c3f419u, 0xcafa0803u, 0x4ce5591du, 0xb9032731u, 0x6fc356dau, 0x99005988u};
#define EXPECT_TABLE 0x00e321fau
#define EXPECT_HOST 0xe67ee683u

static uint32_t host_hash(void) {
   static MtHost h;
   memset(&h, 0, sizeof h);
   static const int d[8] = {0, 64, 0, 0, 0, 64, 0, 0};
   for (int l = 0; l < 3; ++l) for (int k = 0; k < 8; ++k) h.raw[l][k] = d[k];
   h.level = 100 << 16;
   mt_host_reset(&h);
   h.word[W_TICK] = 2880;
   mt_host_rebuild(&h, NULL);
   for (int l = 0; l < 3; ++l) mt_lfo_evaluate(&h, l, 0, 0);
   st_seed = 77;
   uint32_t x = 2166136261u;
   for (int f = 0; f < 6000; ++f) {
      if (st_rnd(6) == 0) { const int l = (int)st_rnd(3), k = (int)st_rnd(8); h.raw[l][k] = (int32_t)st_rnd(128); }
      if (st_rnd(4) == 0) { const int k = (int)st_rnd(32); h.knob[k] = (int32_t)st_rnd(128) << 16; }
      if (st_rnd(90) == 0) h.word[W_TICK] = 720 + (int32_t)st_rnd(6481);
      if (st_rnd(25) == 0) h.lfo_trig = 1;
      mt_host_frame(&h, NULL);
      for (int w = 0; w < 24; ++w) x = st_fnv(x, h.block[w]);
      x = st_fnv(x, h.block[W_PMOD]);
      x = st_fnv(x, h.block[W_LEVEL]);
      for (int w = 0; w < 8; ++w) x = st_fnv(x, h.block[W_SYN + w]);
      for (int l = 0; l < 3; ++l) { x = st_fnv(x, h.lfo[l].pos); x = st_fnv(x, h.lfo[l].acc); }
   }
   return x;
}

int main(void) {
   mt_tables();
   int bad = 0;
   uint32_t t = 2166136261u;
   for (int k = 0; k < 127; ++k) t = st_fnv(t, mt_fdecay[k]);
   if (t != EXPECT_TABLE) { printf("FAIL filter decay table: 0x%08x\n", t); bad = 1; }
   const uint32_t hh = host_hash();
   if (hh != EXPECT_HOST) { printf("FAIL main CPU (LFOs, slew): 0x%08x\n", hh); bad = 1; }
   static MtHost h;
   static MonoTrack track;
   for (int m = 0; m < MT_MACHINES; ++m) {
      st_seed = 1000u + (uint32_t)m;
      st_kit(&h, m);
      mt_reset(&track, m, h.word[W_PITCH]);
      uint32_t x = 2166136261u;
      for (long b = 0; b < 12000; ++b) {
         if (b % 3 == 0) st_frame(&h, b / 3);
         int32_t out[2][MT_FRAMES];
         mt_block(&track, h.block, out);
         for (int i = 0; i < MT_FRAMES; ++i) { x = st_fnv(x, out[0][i]); x = st_fnv(x, out[1][i]); }
         h.block[W_NOTE] = h.block[W_FTRIG] = 0;
      }
      if (x != expect_track[m]) { printf("FAIL machine %d: 0x%08x\n", m, x); bad = 1; }
   }
   puts(bad ? "FAILED" : "PASS: tables, main CPU and all eight tracks match the firmware's hashes");
   return bad;
}
