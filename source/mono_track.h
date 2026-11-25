/* mono_track.h - one whole Monomachine track, fixed-point core.
 *
 * A native C port of Monomachine OS 1.32B, written from its DSP56300 kernel
 * and its main-CPU code and checked sample for sample against that code in an
 * emulator. The signal flow is the kernel's, per 16-sample block:
 *
 *   pitch (glide, TUNE, master tune, word 30) -> machine -> EQ -> filter + DIST
 *   -> AMP (envelope, VOL, PAN) -> SRR -> ENS's chorus -> delay -> LEVEL -> out
 *
 * An FX machine (THRU, REVERB, CHORUS, DYNAMIX, RINGMOD, PHASER, FLANGER) takes
 * the track's input, `in`, as its INP A+B routing reads it; RINGMOD's EXT
 * carrier is `ext`.
 *
 * The filter envelope and the key tracking move the filter's two positions.
 * The machines, EQ, filter, AMP, SRR, chorus and delay are the cores in
 * cores/, each verified on its own; this file joins them and adds what only a
 * whole track has. mt_block runs the DSP side from the 52-word parameter block
 * the main CPU sends. The main CPU's side (knob slew, LFOs, notes) is mt_host.
 *
 * C99 and libm. SPDX-License-Identifier: GPL-3.0-only */
#ifndef MONO_TRACK_H
#define MONO_TRACK_H

#include <math.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include "cores/mono_core.h"
#include "cores/mono_delay.h"
#include "cores/mono_srr_chorus.h"
#include "cores/mono_fm.h"
#include "cores/mono_swave.h"
#include "cores/mono_gnd.h"
#include "cores/mono_phaser.h"
#include "cores/mono_flanger.h"
#include "cores/mono_dynamix.h"
#include "cores/mono_reverb.h"
#include "cores/mono_ringmod.h"
#include "cores/mono_chorus.h"

#define MT_FRAMES 16

enum { MT_SIN, MT_NOIS, MT_SAW, MT_PULS, MT_ENS, MT_STAT, MT_PAR, MT_DYN,
       MT_THRU, MT_REVERB, MT_CHORUS, MT_DYNAMIX, MT_RINGMOD, MT_PHASER, MT_FLANGER, MT_MACHINES };
#define MT_FX(m) ((m) >= MT_THRU)   /* an FX machine: it processes the track's input */

/* The block words a track uses (Monomachine main CPU -> DSP). */
enum {
   W_ATK = 0, W_HOLD, W_DEC, W_REL, W_DIST, W_VOL, W_PAN, W_PORT,
   W_BASE = 8, W_WDTH, W_HPQ, W_LPQ, W_FATK, W_FDEC, W_BOFS, W_WOFS,
   W_EQF = 16, W_EQG, W_SRR, W_DTIM, W_DSND, W_DFB, W_DBAS, W_DWID,
   W_PMOD = 30, W_LEVEL = 31, W_FTRIG = 32, W_RECIP = 35, W_ROUTE = 37, W_MTUNE = 38,
   W_NOTE = 40, W_PITCH = 41, W_TICK = 42, W_SYN = 44, MT_WORDS = 52
};
#define MT_LP_KEY (1 << 9)    /* word 37: the low-pass follows the pitch */
#define MT_HP_KEY (1 << 11)   /* word 37: the high-pass follows the pitch */
#define MT_INP_AB 0xd0        /* word 37: the input is ADC A and B, through the aux buffer (bits 4, 6, 7) */

/* What each machine's audio routine leaves for the filter: the level word
 * X:$50b (the headroom in front of the clipper) and whether it is mono (X:$50f
 * set: the right EQ and filter are skipped and AMP takes the left for both). */
static const int32_t mt_headroom[MT_MACHINES] = {0x200000, 0x200000, 0x0a05bc, 0x19999a, 0x280000, 0x200000, 0x200000, 0x200000,
   0x200000, 0x200000, 0x200000, 0x200000, 0x200000, 0x200000, 0x200000};
static const uint8_t mt_stereo[MT_MACHINES] = {0, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1};

typedef struct {
   int machine;
   MonoFm fm;
   MonoSwave sw;
   MonoGnd gnd;
   MonoPhaser phaser;
   MonoFlanger flanger;
   MonoDynamix dynamix;
   MonoReverb reverb;
   MonoRingmod ringmod;
   MonoChorus chorus;
   int init;                    /* an FX machine's init routine is still to run (it reads the SYN words) */
   int32_t in[2][MT_FRAMES];    /* an FX machine's input for the next block, and RINGMOD's EXT */
   int32_t ext[2][MT_FRAMES];
   MfPitch glide;
   int32_t fenv, fstage;        /* filter envelope: X:$5db, Y:$5db */
   MonoCore filter;
   MfAmp amp;
   MonoSrrChorus srr;
   MonoDelay delay;
   int32_t level;               /* LEVEL's gain at the end of the last block (Y:$5ff) */
} MonoTrack;

static int32_t mt_fdecay[129];  /* the filter envelope's fall per block (Y:$141a00) */

static void mt_tables(void) {
   mc_tables(); md_tables(); sr_tables(); mf_tables(); ms_tables(); mg_tables();
   mp_tables(); mfl_tables(); mx_tables(); mr_tables(); mq_tables(); mh_tables();
   for (int k = 0; k < 127; ++k) {   /* 1/64 x e^(-c k), to six decimals */
      const int64_t micro = (int64_t)floor(15625.0 * exp(-0.05647235686528125 * k) + 0.5);
      mt_fdecay[k] = (int32_t)((micro * 8388608 + 500000) / 1000000);
   }
   mt_fdecay[127] = 0;
   mt_fdecay[128] = 0x4000;
}

/* A machine just assigned, its note at `word`. */
static void mt_reset(MonoTrack *t, int machine, int32_t word) {
   memset(t, 0, sizeof *t);
   t->machine = machine;
   mono_core_reset(&t->filter);
   md_reset(&t->delay);
   sr_reset(&t->srr);
   mf_pitch_reset(&t->glide, word);
   if (MT_FX(machine)) t->init = 1;
   else if (machine >= MT_STAT) mf_init(&t->fm, machine - MT_STAT);
   else if (machine >= MT_SAW) ms_init(&t->sw, machine - MT_SAW);
   else mg_init(&t->gnd, machine);
   t->amp.stage = 3;   /* idle, as after a note's release */
}

/* An FX machine's init routine, with the SYN words of its first block. */
static void mt_machine_init(MonoTrack *t, const int32_t *syn) {
   if (!t->init) return;
   t->init = 0;
   switch (t->machine) {
   case MT_REVERB: mr_reset(&t->reverb); break;
   case MT_CHORUS: mh_reset(&t->chorus, syn[0]); break;
   case MT_DYNAMIX: mx_reset(&t->dynamix); break;
   case MT_RINGMOD: mq_reset(&t->ringmod); break;
   case MT_PHASER: mp_reset(&t->phaser); break;
   case MT_FLANGER: mfl_reset(&t->flanger, syn[0]); break;
   default: break;   /* THRU has no state */
   }
}

/* An FX machine's audio routine: its SYN knobs as its core takes them. */
static void mt_fx(MonoTrack *t, const int32_t *syn, int32_t pitch, int32_t x[2][MT_FRAMES]) {
   const int32_t (*in)[MT_FRAMES] = (const int32_t (*)[MT_FRAMES])t->in;
   const int32_t six[7] = {syn[0], syn[1], syn[2], syn[3], syn[4], syn[5], syn[7]};   /* the 7th knob unused */
   mt_machine_init(t, syn);
   switch (t->machine) {
   case MT_REVERB: mr_block(&t->reverb, six, in, x); break;
   case MT_CHORUS: mh_block(&t->chorus, syn, in, x); break;
   case MT_DYNAMIX: mx_block(&t->dynamix, syn, in, x); break;
   case MT_PHASER: mp_block(&t->phaser, six, in, x); break;
   case MT_FLANGER: mfl_block(&t->flanger, six, in, x); break;
   case MT_RINGMOD: {
      const int32_t ctl[MQ_CONTROLS] = {syn[0], syn[1], syn[3], syn[7]};
      mq_block(&t->ringmod, ctl, pitch, in, (const int32_t (*)[MT_FRAMES])t->ext, x);
      break;
   }
   default: {   /* THRU (P:$14764c): the input x INP^2 x 4 */
      const int32_t gain = mc_sat(mc_mpy(syn[7], syn[7]));
      for (int c = 0; c < 2; ++c)
         for (int i = 0; i < MT_FRAMES; ++i) x[c][i] = mc_sat(mc_shl(mc_mpy(in[c][i], gain), 2));
   }
   }
}

/* ---- filter envelope and key tracking (P:$4ff-$56c) -------------------------- */

static void mt_filter_env(MonoTrack *t, const int32_t *w) {
   if (w[W_FTRIG] == 1) { t->fstage = 0; t->fenv = 0; }
   if (t->fstage == 0) {
      const mc_acc b = mc_w(t->fenv) + mc_w(mf_attack[(w[W_FATK] >> 16) & 127]);
      if (b > mc_w(MC_ONE) + 0xffffff) t->fstage = 1;
      t->fenv = mc_sat(b);
      return;
   }
   /* the hold is always zero long here: straight on to the decay, in the same block */
   t->fstage = 2;
   const int k = mc_hi(mc_rnd((mc_w(w[W_FDEC]) + mc_w(0x7fff)) >> 16));
   const int32_t v = t->fenv - mt_fdecay[k < 0 ? 0 : k > 128 ? 128 : k];
   t->fenv = v < 0 ? 0 : v;
}

/* The signed square of (offset word - 1/2), x 4: the envelope depth of BOFS and WOFS. */
static int32_t mt_depth(int32_t ofs) {
   const mc_acc a = mc_w(ofs) + mc_w(-0x400000);
   const int32_t v = mc_sat(a), m = mc_sat(a < 0 ? -a : a);
   return mc_sat(mc_shl(mc_mpy(m, v), 2));
}

static void mt_positions(const int32_t *w, int32_t env, int32_t pitch, int32_t *hp, int32_t *lp) {
   const mc_acc base = mc_rnd(mc_mpy(w[W_BASE], 0x800) - mc_w(0x80));
   mc_acc a = mc_mpy(mc_sat(mc_mpy(env, mt_depth(w[W_BOFS]))), 0x700) + base;
   if (a >= mc_w(0x700)) a = mc_w(0x700);
   *hp = mc_hi(a);
   const mc_acc key = mc_mpy(pitch, 0x080000);   /* pitch / 16: 128 positions per octave, so 1:1 */
   a += key;
   if (w[W_ROUTE] & MT_HP_KEY) *hp = mc_hi(a);
   if (!(w[W_ROUTE] & MT_LP_KEY)) a -= key;
   a += mc_mpy(w[W_WDTH], 0x800);
   mc_acc b = a + mc_mpy(mc_sat(mc_mpy(env, mt_depth(w[W_WOFS]))), 0x700);
   if (b < 0) b = 0;
   *lp = mc_hi(b);
}

/* What an FX machine's block leaves in Y:$6d..$6f and Y:$ad..$af, for AMP's low
 * words; NULL where it leaves them alone. */
static void mt_fx_low(const MonoTrack *t, const int32_t **l, const int32_t **r) {
   *l = *r = NULL;
   switch (t->machine) {
   case MT_REVERB: *l = t->reverb.y6d; break;
   case MT_CHORUS: *l = t->chorus.y6d; break;
   case MT_DYNAMIX: *l = t->dynamix.y6d; break;
   case MT_RINGMOD: *l = t->ringmod.y6d; *r = t->ringmod.yad; break;
   case MT_FLANGER: *r = t->flanger.yad; break;
   default: break;
   }
}

/* ---- EQ, filter, DIST: as mono_core_block, at the given positions -------------
 * low[c]: what Y:$6d.. / Y:$ad.. hold when AMP reads them as the low half of
 * the filter's 48-bit output, which moves AMP's product by up to an LSB. The
 * first three are the last block's delay pass and zeros, or what the machine
 * left there (l6d, lad). */
static void mt_filter(MonoTrack *t, const int32_t *w, int32_t hp_target, int32_t lp_target,
                      int32_t io[2][MT_FRAMES], int32_t low[2][MT_FRAMES], const int32_t *l6d, const int32_t *lad) {
   MonoCore *m = &t->filter;
   McBlock k;
   mc_eq(w[W_EQF], w[W_EQG], k.eq);
   mc_acc a = mc_shl(mc_w(w[W_DIST]), 1);
   const int32_t drive = mc_drive[mc_sat(a) >> 15];
   a -= mc_w(0x800000);
   if (a < 0) a = 0;
   const int32_t over = mc_sat(a);
   const int32_t headroom = mt_headroom[t->machine];
   const int32_t post = mc_sat(mc_mpy(0x7deccd, mc_sat(mc_mpy(over, over))) + mc_w(0x021333));
   const int32_t comp = mc_sat(mc_mpy(mc_comp[over >> 15], headroom));
   k.clip_gain = mc_sat(mc_mpy(mc_div(mc_w(0x080000), headroom), post));

   mc_resonator(&m->lp, lp_target, w[W_LPQ], -22938, drive, &k.lp);
   mc_resonator(&m->hp, hp_target, w[W_HPQ], -9830, -1, &k.hp);

   const int32_t sixteenth = 0x080000;
   const int32_t f = mc_pole[lp_target > 0x6a3 ? 0x6a3 : lp_target];
   const mc_acc g = mc_mpy(comp, f);
   mc_ramp(k.smooth_f, 16, m->smooth_f, f - m->smooth_f, sixteenth);
   mc_ramp(k.smooth_g, 16, m->smooth_g, mc_sat(g - mc_w(m->smooth_g)), sixteenth);
   m->smooth_f = f;
   m->smooth_g = mc_sat(g);

   const int32_t tp = mc_pole[(hp_target > 0x63f ? 0x63f : hp_target < 0 ? 0 : hp_target) + 128];
   a = (2 * mc_w(MC_ONE) - mc_w(tp)) >> 1;
   mc_ramp(k.dc_c, 16, m->dc_c, mc_sat(a - mc_w(m->dc_c)), sixteenth);
   mc_ramp(k.dc_t, 16, m->dc_t, tp - m->dc_t, sixteenth);
   m->dc_c = mc_sat(a);
   m->dc_t = tp;

   const int32_t *y = l6d ? l6d : t->delay.y62 + 11;     /* Y:$6d..$6f: the machine's, or the last block's delay pass (Y:$62..) */
   int32_t u[12];
   mc_channel(&m->ch[0], &k, io[0], u);
   low[0][0] = y[0]; low[0][1] = y[1]; low[0][2] = y[2]; low[0][3] = comp;
   memcpy(&low[0][4], u, sizeof u);
   if (mt_stereo[t->machine]) {
      mc_channel(&m->ch[1], &k, io[1], u);
      for (int k = 0; k < 3; ++k) low[1][k] = lad ? lad[k] : 0;
      low[1][3] = k.clip_gain;
      memcpy(&low[1][4], u, sizeof u);
   } else {
      memcpy(io[1], io[0], sizeof io[0]);
      memcpy(low[1], low[0], sizeof low[0]);
   }
}

/* ---- one block ----------------------------------------------------------------- */

/* w: the 52 block words. out: the track after LEVEL, the words the kernel
 * stores at Y:$0.. (each limited to 24 bits). */
static void mt_block(MonoTrack *t, const int32_t *w, int32_t out[2][MT_FRAMES]) {
   const int trig = w[W_NOTE] & 0x80, m = t->machine;
   const int32_t *syn = w + W_SYN;
   /* X:$1, the pitch's +-2 semitones: the 8th SYN knob (TUNE), or 64 when the
    * track reads the aux buffer or a bus (P:$11b; an FX machine's 8th knob is INP) */
   const int32_t tune = w[W_ROUTE] & 0x18 ? 0x400000 : syn[7];
   const int32_t pitch = mf_pitch(&t->glide, w[W_PITCH], tune, w[W_PORT], w[W_TICK], w[W_PMOD], w[W_MTUNE]);

   /* the machine */
   int32_t x[2][MT_FRAMES];
   if (MT_FX(m)) mt_fx(t, syn, pitch, x);
   else if (m >= MT_STAT) mf_block(&t->fm, syn, pitch, trig, x[0]);
   else if (m >= MT_SAW) ms_block(&t->sw, syn, pitch, trig, x[0]);
   else mg_block(&t->gnd, syn, pitch, x);
   if (!mt_stereo[m] && m >= MT_SAW) memcpy(x[1], x[0], sizeof x[0]);

   /* filter envelope, key tracking, EQ, filter, DIST */
   mt_filter_env(t, w);
   int32_t hp, lp, low[2][MT_FRAMES];
   const int32_t *l6d = NULL, *lad = NULL;
   if (MT_FX(m)) mt_fx_low(t, &l6d, &lad);
   mt_positions(w, t->fenv, pitch, &hp, &lp);
   mt_filter(t, w, hp, lp, x, low, l6d, lad);

   /* AMP */
   int32_t g[2][MT_FRAMES], y[2][MT_FRAMES];
   mf_amp(&t->amp, w[W_NOTE] & 0x7f, w, w[W_RECIP], g);
   for (int c = 0; c < 2; ++c)
      for (int i = 0; i < MT_FRAMES; ++i) y[c][i] = mf_apply(g[c][i], mf_long(x[c][i], (uint32_t)low[c][i]));

   /* SRR, then ENS's chorus (its hook, P:$97c) */
   sr_srr(&t->srr, w[W_SRR], y);
   if (m == MT_ENS) {
      int32_t pan[2];
      ms_pan(w[W_PAN], pan);
      ms_chorus(&t->sw, syn, pan, y);
   }

   /* delay */
   const int32_t dl[MD_CONTROLS] = {w[W_DTIM], w[W_DFB], w[W_DBAS], w[W_DWID], w[W_DSND]};
   int32_t wet[2][MT_FRAMES];
   md_block(&t->delay, dl, (uint32_t)w[W_TICK], (const int32_t (*)[MT_FRAMES])y, wet);

   /* LEVEL (P:$b1e): LEVEL^2 x the global fade^2, ramped over the block; it
    * scales the echoes too. The fade is at full here (no kit change). */
   const int32_t lv = mc_sat(mc_mpy(w[W_LEVEL], w[W_LEVEL])), fade = mc_sat(mc_mpy(MC_ONE, MC_ONE));
   const mc_acc to = mc_mpy(fade, lv);
   const int32_t old = t->level, delta = mc_sat(to - mc_w(old));
   t->level = mc_sat(to);
   mc_acc ga = mc_w(old), gb = mc_w(old) + mc_mpy(0x080000, delta);
   int32_t gain[MT_FRAMES];
   for (int i = 0; i < 8; ++i) {
      gain[2 * i] = mc_sat(ga);
      gain[2 * i + 1] = mc_sat(gb);
      ga += mc_mpy(0x100000, delta);
      gb += mc_mpy(0x100000, delta);
   }
   for (int c = 0; c < 2; ++c)
      for (int i = 0; i < MT_FRAMES; ++i)
         out[c][i] = mc_sat(mc_mpy(y[c][i], gain[i]) + mc_mpy(gain[i], wet[c][i]));
}

#include "mono_track_host.h"

#endif
