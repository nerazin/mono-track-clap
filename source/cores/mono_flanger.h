/* mono_flanger.h - Monomachine FX-FLANGER machine, fixed-point core.
 *
 * A native C port of the FLANGER machine of Monomachine OS 1.32B, written
 * from its DSP56300 code and checked sample for sample against that code
 * running in an emulator. It contains no firmware code or data: the sine
 * table is generated at start-up the way the firmware generates it.
 *
 * Signal path, per channel:
 *   input x INP^2 x 4 = dry
 *   ring (4096 words) <- dry + FB x (the ring at the delay, linear interpolation)
 *   wet = the ring at the delay + LFO x DEP x delay (4-point cubic interpolation)
 *   output = (1 - MIX) x dry + MIX x 0.7071 x (dry + wet)
 *
 * The delay is 2 to 1016 samples (DEL), glides 2% of the way per 16-sample
 * block and moves smoothly across each block. The sine LFO (SPD^2) sweeps the
 * read around it by up to +-delay at full DEP; WID shifts the left and right
 * sweeps by up to +-89 degrees in opposite directions. For the first 128
 * blocks after the machine starts, the feedback and the dry part of the mix
 * are off.
 *
 * The arithmetic follows the DSP: 24-bit words, 56-bit accumulators,
 * truncation and saturation where the original has them. Blocks are 16
 * stereo frames at 44.1 kHz; controls are the 24-bit block words the
 * Monomachine's main CPU sends (knob value << 16).
 *
 * This copy (Mono Track's) is renamed from mf_ to mfl_ and also keeps the words
 * the block leaves in Y:$ad..$af, which the track's AMP stage reads as the low
 * half of the filter's output.
 *
 * Plain C99, no dependencies. SPDX-License-Identifier: GPL-3.0-only */
#ifndef MONO_FLANGER_H
#define MONO_FLANGER_H

#include <stdint.h>
#include <string.h>

#define MFL_FRAMES 16
#define MFL_RING 0x1000            /* words per channel */
#define MFL_MASK (MFL_RING - 1)
#define MFL_GATE 128               /* blocks before feedback and dry come in */
#define MFL_ONE 0x7fffff

enum { MFL_DELAY, MFL_DEPTH, MFL_SPEED, MFL_MIX, MFL_FEEDBACK, MFL_WIDTH, MFL_INPUT, MFL_CONTROLS };   /* DEL DEP SPD MIX FB WID INP */

/* ---- DSP56300 arithmetic ------------------------------------------------- */

typedef int64_t mfl_acc;           /* 56-bit accumulator, sign-extended; also 24.24 long words */

static inline mfl_acc mfl_w(int32_t x) { return (mfl_acc)x * 16777216; }
static inline mfl_acc mfl_mpy(int32_t x, int32_t y) { return (mfl_acc)x * y * 2; }
static inline int32_t mfl_sat(mfl_acc a) {           /* accumulator -> 24-bit word (limiter) */
   const mfl_acc v = a >> 24;
   return v > MFL_ONE ? MFL_ONE : v < -MFL_ONE - 1 ? -MFL_ONE - 1 : (int32_t)v;
}
static inline mfl_acc mfl_sat48(mfl_acc a) {          /* saturation mode: clip the result to 48 bits */
   const mfl_acc hi = ((mfl_acc)1 << 47) - 1;
   return a > hi ? hi : a < -hi - 1 ? -hi - 1 : a;
}

/* ---- sine table ----------------------------------------------------------- */

/* sin(k pi / 4096), k = 0..4095, as the boot code builds it: a 48-bit
 * recurrence s[k+1] = 2 cos(pi/4096) s[k] - s[k-1] with a truncating
 * double-precision multiply, saturating at the peak. */
static int32_t mfl_sine[0x1000];

static void mfl_tables(void) {
   const int32_t c1 = 0x7ffffd, c0 = 0x885869;                 /* round(cos(pi/4096) x 2^47) */
   mfl_acc prev = 0, cur = ((mfl_acc)0x1921 << 24) | 0xfb2aed;   /* round(sin(pi/4096) x 2^47) */
   mfl_sine[0] = 0;
   mfl_sine[1] = 0x1921;
   for (int k = 2; k < 0x1000; ++k) {
      const int32_t s1 = (int32_t)(cur >> 24), s0 = (int32_t)(cur & 0xffffff);
      mfl_acc a = (mfl_acc)c0 * s0 * 2;
      a = (a >> 24) + (mfl_acc)c1 * s0 * 2;
      a += (mfl_acc)s1 * c0 * 2;
      a = (a >> 24) + (mfl_acc)c1 * s1 * 2;
      const mfl_acc next = mfl_sat48(a * 2 - prev);
      mfl_sine[k] = (int32_t)(next >> 24);
      prev = cur;
      cur = next;
   }
}

/* ---- the machine ---------------------------------------------------------- */

typedef struct {
   int32_t lfo_sin, lfo_cos;     /* LFO pair, ends of the last block */
   mfl_acc delay;                 /* the delay after the 2% glide: minus samples, 24.24 */
   mfl_acc depth;                 /* minus DEP after the glide (24.24) */
   int32_t span;                 /* DEP x delay: the sweep at the last block */
   int32_t width;                /* WID after the glide */
   int32_t rot_cos, rot_sin;     /* the WID rotation at the last block */
   int32_t blocks;               /* blocks run, up to MFL_GATE */
   int32_t pos;                  /* write position in the rings */
   int32_t ring[2][MFL_RING];
   int32_t yad[3];               /* Y:$ad..$af after the block: the left read's x^3 at frames 13..15 */
} MonoFlanger;

static int32_t mfl_target(int32_t del) { return mfl_sat(mfl_mpy(del, -1021) - mfl_w(3)); }   /* -(3 + 7.98 DEL) */

/* The machine's init routine: LFO at -1, the delay at DEL, the rest at 0. */
static void mfl_reset(MonoFlanger *f, int32_t del) {
   memset(f, 0, sizeof *f);
   f->lfo_sin = -MFL_ONE - 1;
   f->delay = mfl_w(mfl_target(del));
}

static const int32_t mfl_glide = 0x028f5c;          /* 0.02 */
static const int32_t mfl_sixteenth = 0x080000;      /* 1/16 */
static const int32_t mfl_half_root2 = 0x5a8241;     /* 0.7071 */

/* ramp[i] = from + i/16 of the way to `to`, as the firmware interpolates */
static void mfl_ramp(mfl_acc from, int32_t to, int32_t ramp[MFL_FRAMES]) {
   const mfl_acc step = mfl_mpy(mfl_sat(mfl_w(to) - from), mfl_sixteenth);
   for (int i = 0; i < MFL_FRAMES; ++i) ramp[i] = mfl_sat(from + i * step);
}

/* One 16-frame block. ctl: the block words of DEL DEP SPD MIX FB WID INP. */
static void mfl_block(MonoFlanger *f, const int32_t ctl[MFL_CONTROLS],
                     const int32_t in[2][MFL_FRAMES], int32_t out[2][MFL_FRAMES]) {
   int32_t dry[2][MFL_FRAMES], lfo[2][MFL_FRAMES], rc[MFL_FRAMES], rs[MFL_FRAMES], span[MFL_FRAMES], wet[2][MFL_FRAMES];

   const int32_t gate = f->blocks < MFL_GATE ? 0 : -MFL_ONE - 1;   /* -1 once open */
   if (f->blocks < MFL_GATE) ++f->blocks;

   const int32_t gain = mfl_sat(mfl_mpy(ctl[MFL_INPUT], ctl[MFL_INPUT]));
   for (int c = 0; c < 2; ++c)
      for (int i = 0; i < MFL_FRAMES; ++i) dry[c][i] = mfl_sat(mfl_mpy(in[c][i], gain) * 4);

   /* LFO: a coupled sine/cosine recurrence in saturation mode. The cosine
    * update reads the sine of one sample earlier, as the DSP's pipelined
    * register moves have it. */
   const int32_t rate = mfl_sat(mfl_mpy(mfl_sat(mfl_mpy(ctl[MFL_SPEED], ctl[MFL_SPEED])), 0x9566));
   mfl_acc s = mfl_w(f->lfo_sin), co = mfl_w(f->lfo_cos);
   int32_t s_late = mfl_sat(s), sn[MFL_FRAMES], cs[MFL_FRAMES];
   for (int i = 0; i < MFL_FRAMES; ++i) {
      cs[i] = mfl_sat(co);
      sn[i] = mfl_sat(s);
      co = mfl_sat48(co - mfl_mpy(s_late, rate));
      s_late = sn[i];
      s = mfl_sat48(s + mfl_mpy(cs[i], rate));
   }
   f->lfo_sin = mfl_sat(s);
   f->lfo_cos = mfl_sat(co);

   /* WID: rotate the LFO by +-w for the left and right sweeps */
   f->width = mfl_sat(mfl_w(f->width) + mfl_mpy(ctl[MFL_WIDTH], mfl_glide) - mfl_mpy(f->width, mfl_glide));
   const int k = f->width >> 12;
   const int32_t old_s = f->rot_sin, old_c = f->rot_cos;
   f->rot_sin = mfl_sine[k];
   f->rot_cos = mfl_sine[k + 0x800];
   mfl_ramp(mfl_w(old_s), f->rot_sin, rs);
   mfl_ramp(mfl_w(old_c), f->rot_cos, rc);
   for (int i = 0; i < MFL_FRAMES; ++i) {
      lfo[0][i] = mfl_sat(mfl_mpy(rc[i], cs[i]) + mfl_mpy(rs[i], sn[i]));
      lfo[1][i] = mfl_sat(mfl_mpy(rc[i], cs[i]) - mfl_mpy(rs[i], sn[i]));
   }

   /* delay and sweep glides; the sweep is DEP x (delay + 2) */
   const mfl_acc old_delay = f->delay;
   f->delay += mfl_mpy(mfl_target(ctl[MFL_DELAY]), mfl_glide) - mfl_mpy(mfl_sat(f->delay), mfl_glide);
   f->depth += -mfl_mpy(ctl[MFL_DEPTH], mfl_glide) - mfl_mpy(mfl_sat(f->depth), mfl_glide);
   const int32_t old_span = f->span;
   f->span = mfl_sat(mfl_mpy(mfl_sat(f->depth), mfl_sat((f->delay + mfl_w(2)) * 1024)));
   mfl_ramp(mfl_w(old_span), f->span, span);
   const mfl_acc step = (f->delay - old_delay + mfl_w(16)) >> 4;   /* per sample: 1 + the delay's change / 16 */

   /* feedback: the ring at the (unswept) delay, linearly interpolated */
   const int32_t t = (int32_t)((f->delay & 0xffffff) >> 1), back = (int32_t)(f->delay >> 24);
   int32_t fb = mfl_sat(mfl_w(ctl[MFL_FEEDBACK]) - mfl_w(0x400000));
   fb = mfl_sat(mfl_mpy(fb, 0x410410) * 4);            /* (FB - 64) / 63, clipped */
   fb = mfl_sat(mfl_mpy(fb, 0x7eb852));                /* x 0.99 */
   const int32_t near = mfl_sat(-mfl_mpy(mfl_sat(mfl_mpy(fb, t)), gate));
   const int32_t far = mfl_sat(-mfl_mpy(gate, mfl_sat(mfl_mpy(fb, mfl_sat(mfl_w(MFL_ONE) - mfl_w(t))))));
   const int32_t base = f->pos;
   for (int c = 0; c < 2; ++c) {
      int32_t *r = f->ring[c];
      for (int i = 0; i < MFL_FRAMES; ++i) {
         const int p = (base + back + i) & MFL_MASK;
         r[(base + i) & MFL_MASK] = mfl_sat(mfl_mpy(r[p], far) + mfl_mpy(r[(p + 1) & MFL_MASK], near) + mfl_mpy(MFL_ONE, dry[c][i]));
      }
   }
   f->pos = (base + MFL_FRAMES) & MFL_MASK;

   /* wet: the ring at the swept delay, 4-point Lagrange interpolation */
   for (int c = 0; c < 2; ++c) {
      const int32_t *r = f->ring[c];
      mfl_acc d = old_delay;
      for (int i = 0; i < MFL_FRAMES; ++i) {
         const mfl_acc at = ((mfl_mpy(lfo[c][i], span[i])) >> 10) + d;
         d += step;
         const int q = (int)(base + (int32_t)(at >> 24)) & MFL_MASK;
         const int32_t x = (int32_t)((at & 0xffffff) >> 1), x2 = mfl_sat(mfl_mpy(x, x)), x3 = mfl_sat(mfl_mpy(x, x2));
         if (c == 0 && i >= 13) f->yad[i - 13] = x3;
         const int32_t h0 = mfl_sat(mfl_mpy(x3, -0x155555) + mfl_mpy(x2, 0x400000) + mfl_mpy(x, -0x2aaaab));
         const int32_t h1 = mfl_sat(mfl_mpy(x3, 0x400000) - mfl_w(x2) + mfl_mpy(x, -0x400000) + mfl_w(0x800000));
         const int32_t h2 = mfl_sat(mfl_mpy(x3, -0x400000) + mfl_mpy(x2, 0x400000) + mfl_mpy(x, MFL_ONE));
         const int32_t h3 = mfl_sat(mfl_mpy(x3, 0x155555) + mfl_mpy(x, -0x155555));
         wet[c][i] = mfl_sat(mfl_mpy(r[q], h0) + mfl_mpy(r[(q + 1) & MFL_MASK], h1) +
                            mfl_mpy(r[(q + 2) & MFL_MASK], h2) + mfl_mpy(r[(q + 3) & MFL_MASK], h3));
      }
   }

   const int32_t through = mfl_sat(-mfl_mpy(gate, mfl_half_root2));
   const int32_t mix = ctl[MFL_MIX], keep = mfl_sat(mfl_w(MFL_ONE) - mfl_w(mix));
   for (int c = 0; c < 2; ++c)
      for (int i = 0; i < MFL_FRAMES; ++i) {
         const int32_t flanged = mfl_sat(mfl_mpy(dry[c][i], through) + mfl_mpy(mfl_half_root2, wet[c][i]));
         out[c][i] = mfl_sat(mfl_mpy(keep, dry[c][i]) + mfl_mpy(mix, flanged));
      }
}

#endif
