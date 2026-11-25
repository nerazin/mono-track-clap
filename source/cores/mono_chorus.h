/* mono_chorus.h - Monomachine FX-CHORUS machine, fixed-point core.
 *
 * A native C port of the CHORUS machine of Monomachine OS 1.32B (audio routine
 * P:$14767b, init P:$147661), written from its DSP56300 code and checked
 * sample for sample against that code running in an emulator. It contains no
 * firmware code or data: the sine table is generated at start-up the way the
 * firmware generates it, and the feedback low-pass uses Mono Delay's one-pole
 * coefficients (md_coef, the same table as the firmware's X:$144ac7).
 *
 * Signal path, per channel:
 *   input x INP^2 x 4 = dry
 *   three taps of a 2048-word ring, linearly interpolated, each swept by one
 *   phase of a sine LFO (0, 120 and 240 degrees) -> wet = their sum / 3
 *   ring <- (1 + FB') / 2 x dry - FB' x (wet through a one-pole low-pass, LP),
 *   FB' = FB + 0.0077
 *   output = (1 - MIX) x dry + MIX x wet
 *
 * The delay is 16 to 1008 samples (DEL), glides 2% of the way per 16-sample
 * block and moves smoothly across each block; the LFO (SPD^2) sweeps each tap
 * around it by up to +-(delay - 16) at full DEP. WID turns the right side's
 * LFO back by up to 89 degrees. For the first 128 blocks after the machine
 * starts, the taps are off.
 *
 * The arithmetic follows the DSP: 24-bit words, 56-bit accumulators,
 * truncation and saturation where the original has them. Blocks are 16
 * stereo frames at 44.1 kHz; controls are the 24-bit block words the
 * Monomachine's main CPU sends (knob value << 16).
 *
 * Mono Track's AMP stage reads the words the block leaves in Y:$6d..$6f as the
 * low half of the filter's output; y6d keeps them.
 *
 * C99 and libm (for md_tables). SPDX-License-Identifier: GPL-3.0-only */
#ifndef MONO_CHORUS_H
#define MONO_CHORUS_H

#include <stdint.h>
#include <string.h>
#include "mono_delay.h"

#define MH_FRAMES 16
#define MH_RING 0x800             /* words per channel */
#define MH_MASK (MH_RING - 1)
#define MH_GATE 128               /* blocks before the taps come in */
#define MH_ONE 0x7fffff

enum { MH_DELAY, MH_DEPTH, MH_SPEED, MH_MIX, MH_FEEDBACK, MH_WIDTH, MH_LOWPASS, MH_INPUT, MH_CONTROLS };   /* DEL DEP SPD MIX FB WID LP INP */

/* ---- DSP56300 arithmetic ------------------------------------------------- */

typedef int64_t mh_acc;           /* 56-bit accumulator, sign-extended; also 24.24 long words */

static inline mh_acc mh_w(int32_t x) { return (mh_acc)x * 16777216; }
static inline mh_acc mh_mpy(int32_t x, int32_t y) { return (mh_acc)x * y * 2; }
static inline int32_t mh_sat(mh_acc a) {           /* accumulator -> 24-bit word (limiter) */
   const mh_acc v = a >> 24;
   return v > MH_ONE ? MH_ONE : v < -MH_ONE - 1 ? -MH_ONE - 1 : (int32_t)v;
}
static inline mh_acc mh_sat48(mh_acc a) {          /* saturation mode: clip the result to 48 bits */
   const mh_acc hi = ((mh_acc)1 << 47) - 1;
   return a > hi ? hi : a < -hi - 1 ? -hi - 1 : a;
}

/* ---- tables ----------------------------------------------------------------- */

/* sin(k pi / 4096), k = 0..4095, as the boot code builds it (see mono_flanger.h) */
static int32_t mh_sine[0x1000];

static void mh_tables(void) {
   md_tables();
   const int32_t c1 = 0x7ffffd, c0 = 0x885869;
   mh_acc prev = 0, cur = ((mh_acc)0x1921 << 24) | 0xfb2aed;
   mh_sine[0] = 0;
   mh_sine[1] = 0x1921;
   for (int k = 2; k < 0x1000; ++k) {
      const int32_t s1 = (int32_t)(cur >> 24), s0 = (int32_t)(cur & 0xffffff);
      mh_acc a = (mh_acc)c0 * s0 * 2;
      a = (a >> 24) + (mh_acc)c1 * s0 * 2;
      a += (mh_acc)s1 * c0 * 2;
      a = (a >> 24) + (mh_acc)c1 * s1 * 2;
      const mh_acc next = mh_sat48(a * 2 - prev);
      mh_sine[k] = (int32_t)(next >> 24);
      prev = cur;
      cur = next;
   }
}

/* ---- the machine ---------------------------------------------------------- */

typedef struct {
   int32_t lfo_sin, lfo_cos;     /* LFO pair, ends of the last block (X:$539, X:$53a) */
   mh_acc delay;                 /* the delay after the 2% glide: minus samples, 24.24 */
   mh_acc depth;                 /* minus DEP after the glide (24.24) */
   int32_t span;                 /* DEP x (delay - 16): the sweep at the last block */
   int32_t width;                /* WID after the glide */
   int32_t rot_cos, rot_sin;     /* the WID rotation at the last block */
   int32_t lp[2];                /* the feedback low-pass's states */
   int32_t blocks;               /* blocks run, up to MH_GATE */
   int32_t pos;                  /* write position in the rings */
   int32_t ring[2][MH_RING];
   int32_t y6d[3];               /* Y:$6d..$6f after the block: low-pass outputs, interleaved at Y:$60.. */
} MonoChorus;

/* The machine's init routine: LFO at -1, the delay at DEL (without the
 * block's -16), the rest at 0. */
static void mh_reset(MonoChorus *h, int32_t del) {
   memset(h, 0, sizeof *h);
   h->lfo_sin = -MH_ONE - 1;
   h->delay = mh_w(mh_sat(mh_mpy(del, -992)));
}

static const int32_t mh_glide = 0x028f5c;          /* 0.02 */
static const int32_t mh_sixteenth = 0x080000;      /* 1/16 */

/* ramp[i] = from + i/16 of the way to `to`, as the firmware interpolates */
static void mh_ramp(mh_acc from, int32_t to, int32_t ramp[MH_FRAMES]) {
   const mh_acc step = mh_mpy(mh_sat(mh_w(to) - from), mh_sixteenth);
   for (int i = 0; i < MH_FRAMES; ++i) ramp[i] = mh_sat(from + i * step);
}

/* One 16-frame block. ctl: the block words of DEL DEP SPD MIX FB WID LP INP. */
static void mh_block(MonoChorus *h, const int32_t ctl[MH_CONTROLS],
                     const int32_t in[2][MH_FRAMES], int32_t out[2][MH_FRAMES]) {
   int32_t dry[2][MH_FRAMES], cs[MH_FRAMES], sn[MH_FRAMES], rc[MH_FRAMES], rs[MH_FRAMES], span[MH_FRAMES];
   int32_t lfo[6][MH_FRAMES], wet[2][MH_FRAMES], lp[2][MH_FRAMES];

   const int32_t gate = h->blocks < MH_GATE ? 0 : MH_ONE;
   if (h->blocks < MH_GATE) ++h->blocks;

   const int32_t gain = mh_sat(mh_mpy(ctl[MH_INPUT], ctl[MH_INPUT]));
   for (int c = 0; c < 2; ++c)
      for (int i = 0; i < MH_FRAMES; ++i) dry[c][i] = mh_sat(mh_mpy(in[c][i], gain) * 4);

   /* LFO: a coupled sine/cosine recurrence in saturation mode, as the flanger's
    * (the cosine update reads the sine of one sample earlier), 10x slower */
   const int32_t rate = mh_sat(mh_mpy(mh_sat(mh_mpy(ctl[MH_SPEED], ctl[MH_SPEED])), 0x956));
   mh_acc s = mh_w(h->lfo_sin), co = mh_w(h->lfo_cos);
   int32_t s_late = mh_sat(s);
   for (int i = 0; i < MH_FRAMES; ++i) {
      cs[i] = mh_sat(co);
      sn[i] = mh_sat(s);
      co = mh_sat48(co - mh_mpy(s_late, rate));
      s_late = sn[i];
      s = mh_sat48(s + mh_mpy(cs[i], rate));
   }
   h->lfo_sin = mh_sat(s);
   h->lfo_cos = mh_sat(co);

   /* WID: the right side's LFO turned back by w */
   h->width = mh_sat(mh_w(h->width) + mh_mpy(ctl[MH_WIDTH], mh_glide) - mh_mpy(h->width, mh_glide));
   const int k = h->width >> 12;
   const int32_t old_s = h->rot_sin, old_c = h->rot_cos;
   h->rot_sin = mh_sine[k];
   h->rot_cos = mh_sine[k + 0x800];
   mh_ramp(mh_w(old_s), h->rot_sin, rs);
   mh_ramp(mh_w(old_c), h->rot_cos, rc);

   /* six sweeps: left at 0, 120 and 240 degrees, then right */
   for (int i = 0; i < MH_FRAMES; ++i) {
      const int32_t rcs = mh_sat(mh_mpy(rc[i], cs[i]) + mh_mpy(rs[i], sn[i]));
      const int32_t rsn = mh_sat(mh_mpy(sn[i], rc[i]) - mh_mpy(cs[i], rs[i]));
      lfo[0][i] = cs[i];
      lfo[1][i] = mh_sat(mh_mpy(-0x400000, cs[i]) + mh_mpy(0x6ed9ec, sn[i]));
      lfo[2][i] = mh_sat(mh_mpy(-0x400000, cs[i]) + mh_mpy(-0x6ed9ec, sn[i]));
      lfo[3][i] = rcs;
      lfo[4][i] = mh_sat(mh_mpy(-0x400000, rcs) + mh_mpy(0x6ed9ec, rsn));
      lfo[5][i] = mh_sat(mh_mpy(-0x400000, rcs) + mh_mpy(-0x6ed9ec, rsn));
   }

   /* delay and sweep glides; the sweep is DEP x (delay + 16) */
   const mh_acc old_delay = h->delay;
   const int32_t target = mh_sat(mh_mpy(ctl[MH_DELAY], -992) + mh_w(-16));
   h->delay += mh_mpy(target, mh_glide) - mh_mpy(mh_sat(h->delay), mh_glide);
   h->depth += -mh_mpy(ctl[MH_DEPTH], mh_glide) - mh_mpy(mh_sat(h->depth), mh_glide);
   const int32_t old_span = h->span;
   h->span = mh_sat(mh_mpy(mh_sat(h->depth), mh_sat((h->delay + mh_w(16)) * 256)));
   mh_ramp(mh_w(old_span), h->span, span);
   const mh_acc step = (h->delay - old_delay + mh_w(16)) >> 4;   /* per sample: 1 + the delay's change / 16 */

   /* the taps, linearly interpolated, summed and scaled by 1/3 once the gate is open */
   const int32_t third = mh_sat(mh_mpy(gate, 0x2aaaab)), base = h->pos;
   for (int c = 0; c < 2; ++c) {
      const int32_t *r = h->ring[c];
      mh_acc sum[MH_FRAMES] = {0};
      for (int l = 0; l < 3; ++l) {
         mh_acc d = old_delay;
         for (int i = 0; i < MH_FRAMES; ++i) {
            const mh_acc at = (mh_mpy(lfo[3 * c + l][i], span[i]) >> 8) + d;
            d += step;
            const int q = (int)(base + (int32_t)(at >> 24)) & MH_MASK;
            const int32_t x0 = r[q], x1 = r[(q + 1) & MH_MASK];
            sum[i] += mh_mpy(third, mh_sat(mh_w(x0) + (mh_acc)(x1 - x0) * (at & 0xffffff)));
         }
      }
      for (int i = 0; i < MH_FRAMES; ++i) wet[c][i] = mh_sat(sum[i]);
   }

   /* the feedback's low-pass: each output is its state before that sample */
   const int32_t coef = md_coef[(ctl[MH_LOWPASS] >> 16) & 127];
   for (int c = 0; c < 2; ++c) {
      mh_acc a = mh_w(h->lp[c]);
      for (int i = 0; i < MH_FRAMES; ++i) {
         lp[c][i] = mh_sat(a);
         a += mh_mpy(coef, wet[c][i]) - mh_mpy(lp[c][i], coef);
      }
      h->lp[c] = mh_sat(a);
   }
   h->y6d[0] = lp[1][6]; h->y6d[1] = lp[0][7]; h->y6d[2] = lp[1][7];

   /* the rings: dry x (1 + FB') / 2 - FB' x the low-passed wet */
   const mh_acc fb = mh_w(-ctl[MH_FEEDBACK]) - mh_w(0xfd71);
   const int32_t back = mh_sat(fb), through = mh_sat((mh_w(MH_ONE) - fb) >> 1);
   for (int c = 0; c < 2; ++c)
      for (int i = 0; i < MH_FRAMES; ++i)
         h->ring[c][(base + i) & MH_MASK] = mh_sat(mh_mpy(dry[c][i], through) + mh_mpy(back, lp[c][i]));
   h->pos = (base + MH_FRAMES) & MH_MASK;

   const int32_t mix = ctl[MH_MIX], keep = mh_sat(mh_w(MH_ONE) - mh_w(mix));
   for (int c = 0; c < 2; ++c)
      for (int i = 0; i < MH_FRAMES; ++i) out[c][i] = mh_sat(mh_mpy(keep, dry[c][i]) + mh_mpy(wet[c][i], mix));
}

#endif
