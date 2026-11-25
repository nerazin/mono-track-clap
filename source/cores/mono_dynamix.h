/* mono_dynamix.h - Monomachine FX-DYNAMIX machine, fixed-point core.
 *
 * A native C port of the DYNAMIX machine (a compressor) of Monomachine OS
 * 1.32B, written from its DSP56300 code and checked sample for sample against
 * that code running in an emulator. It contains no firmware code: the tables
 * are generated from formulas at start-up (one of them with a bit mask of the
 * entries where the firmware's copy is one LSB lower).
 *
 * Signal path, per sample:
 *   input x INP^2 x 4 = dry
 *   level = the louder channel's power, smoothed (RMS sets the time)
 *   gain target = 2^-(reduction), reduction = RAT x (level - THRS) in log2
 *   gain = the target smoothed per sample: with ATK's time, or REL's on a
 *   sample where the target has risen
 *   output = (1 - MIX) x dry + MIX x (1 + 255 GAIN^2) x gain x dry
 *
 * The arithmetic follows the DSP: 24-bit words, 56-bit accumulators,
 * truncation, rounding and saturation where the original has them. Blocks
 * are 16 stereo frames at 44.1 kHz; controls are the 24-bit block words the
 * Monomachine's main CPU sends (knob value << 16).
 *
 * This copy (Mono Track's) also keeps the words the block leaves in Y:$6d..$6f,
 * which the track's AMP stage reads as the low half of the filter's output.
 *
 * C99 and libm (for the tables). SPDX-License-Identifier: GPL-3.0-only */
#ifndef MONO_DYNAMIX_H
#define MONO_DYNAMIX_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#define MX_FRAMES 16
#define MX_ONE 0x7fffff
#define MX_SETTLE 1024            /* blocks of silence that bring the gain from the machine's start (0) to rest, at any ATK */

enum { MX_ATTACK, MX_RELEASE, MX_THRESHOLD, MX_MIX, MX_RATIO, MX_GAIN, MX_RMS, MX_INPUT, MX_CONTROLS };   /* ATK REL THRS MIX RAT GAIN RMS INP */

/* ---- DSP56300 arithmetic ------------------------------------------------- */

typedef int64_t mx_acc;           /* 56-bit accumulator, sign-extended; also 24.24 long words */

static inline mx_acc mx_s56(mx_acc v) { return (mx_acc)((uint64_t)v << 8) >> 8; }   /* wrap to 56 bits */
static inline mx_acc mx_shl(mx_acc v, int n) { return mx_s56((mx_acc)((uint64_t)v << n)); }   /* ASL */
static inline mx_acc mx_w(int32_t x) { return (mx_acc)x * 16777216; }
static inline mx_acc mx_mpy(int32_t x, int32_t y) { return (mx_acc)x * y * 2; }
static inline int32_t mx_sat(mx_acc a) {           /* accumulator -> 24-bit word (limiter) */
   const mx_acc v = a >> 24;
   return v > MX_ONE ? MX_ONE : v < -MX_ONE - 1 ? -MX_ONE - 1 : (int32_t)v;
}
static inline mx_acc mx_rnd(mx_acc a) {           /* convergent rounding to 24 bits */
   a += 0x800000;
   if (!(a & 0xffffff)) a &= ~(mx_acc)0x1000000;
   return mx_s56(a & ~(mx_acc)0xffffff);
}

/* ---- tables --------------------------------------------------------------- */

/* One-pole coefficients that fall to -20 dB in t: 1 - 10^(-1 / (44100 t)).
 * Fast (attack and RMS): t = 0.25 ms x 200^(k/128). Slow (release): 2.5 ms x 60^(k/128). */
static int32_t mx_fast[128], mx_slow[128];
static int32_t mx_log[256];        /* log2(512 / (k + 257)), unsigned 0.24: the normalised level's mantissa */
static int32_t mx_exp[2048];       /* 2^(22 + k/2048) */

/* Entries of mx_exp where the firmware's table is one LSB below the formula
 * (2048 bits, bit k of word k/32). */
static const uint32_t mx_exp_low[64] = {
   0x00000001, 0x00000000, 0x00000000, 0x00008040, 0x00000000, 0x00000000, 0x40012004, 0x00000080,
   0x80004000, 0x00000000, 0x28000408, 0x06100000, 0x00000000, 0x00000006, 0x00200000, 0x02003018,
   0x00000000, 0x9000460c, 0x00000090, 0x01410002, 0x01810480, 0x00004800, 0x82000500, 0x00868020,
   0x41080610, 0x4a142004, 0x18018004, 0x20002800, 0x30010000, 0x22062492, 0x00502844, 0x00042db1,
   0x00140804, 0x30440014, 0x40235104, 0x02002448, 0x24370104, 0x40506120, 0x88610a22, 0x801a00b0,
   0x0c0c209d, 0x0c21210c, 0x2a418020, 0xc1048884, 0xe974ba59, 0x11082820, 0x83416318, 0x5208260c,
   0x2c84208a, 0x42638174, 0x205e8410, 0x0228a101, 0x0828a040, 0xa1097d08, 0xd6c79050, 0xc429d423,
   0x444d8846, 0x220afd63, 0x24fd40d8, 0xa80880ac, 0x04983580, 0x0364f9ac, 0x1b6c0653, 0x980c94c7
};

static void mx_tables(void) {
   for (int k = 0; k < 128; ++k) {
      mx_fast[k] = (int32_t)lround((1 - exp(-log(10.0) / (44100 * 0.00025 * pow(200, k / 128.0)))) * 8388608);
      mx_slow[k] = (int32_t)lround((1 - exp(-log(10.0) / (44100 * 0.0025 * pow(60, k / 128.0)))) * 8388608);
   }
   for (int k = 0; k < 256; ++k) mx_log[k] = (int32_t)floor(log2(512.0 / (k + 257)) * 16777216);
   for (int k = 0; k < 2048; ++k) mx_exp[k] = (int32_t)floor(exp2(22 + k / 2048.0)) - (int32_t)(mx_exp_low[k >> 5] >> (k & 31) & 1);
}

/* ---- the machine ---------------------------------------------------------- */

typedef struct {
   mx_acc power;                 /* the detector's smoothed power (24.24) */
   int32_t gain;                 /* the smoothed gain at the end of the last block */
   int32_t target;               /* the last sample's gain target */
   int32_t y6d[3];               /* Y:$6d..$6f after the block: the low words of the detector's levels 11..13 */
} MonoDynamix;

static void mx_reset(MonoDynamix *x) { memset(x, 0, sizeof *x); }

static int mx_index(int32_t ctl) { return mx_sat(mx_mpy(ctl, 0x80)); }   /* knob word -> 0..127 */

/* log2 of a non-negative level as the firmware has it: normalise (CLB, NORMF),
 * the mantissa's log from the table, the exponent as the integer part. */
static mx_acc mx_log2(mx_acc a) {
   a |= (mx_acc)1 << 24;
   int lead = 0;
   while (lead < 55 && !((a >> (55 - lead)) & 1)) ++lead;
   const int t = 9 - lead;                         /* CLB: -47..+8, here <= 0 */
   const mx_acc norm = t > 0 ? a >> t : mx_shl(a, -t);
   const int32_t mant = (int32_t)(norm >> 24);
   const int k = mx_sat(mx_mpy(mant, 0x200));      /* 256..511 */
   return mx_shl(mx_w(-t) | mx_log[k - 256], 17);
}

/* One 16-frame block. ctl: the block words of ATK REL THRS MIX RAT GAIN RMS INP. */
static void mx_block(MonoDynamix *x, const int32_t ctl[MX_CONTROLS],
                     const int32_t in[2][MX_FRAMES], int32_t out[2][MX_FRAMES]) {
   int32_t dry[2][MX_FRAMES], peak[MX_FRAMES], target[MX_FRAMES], gain[MX_FRAMES];
   mx_acc level[MX_FRAMES];

   const int32_t g_in = mx_sat(mx_mpy(ctl[MX_INPUT], ctl[MX_INPUT]));
   for (int c = 0; c < 2; ++c)
      for (int i = 0; i < MX_FRAMES; ++i) dry[c][i] = mx_sat(mx_mpy(in[c][i], g_in) * 4);

   /* detector: the louder channel's power, through a one-pole set by RMS */
   for (int i = 0; i < MX_FRAMES; ++i) {
      const mx_acc l = mx_mpy(dry[0][i], dry[0][i]), r = mx_mpy(dry[1][i], dry[1][i]);
      peak[i] = mx_sat(l >= r ? l : r);
   }
   const int32_t avg = mx_fast[mx_index(ctl[MX_RMS])];
   for (int i = 0; i < MX_FRAMES; ++i) {
      const int32_t prev = mx_sat(x->power);
      x->power += mx_mpy(avg, peak[i]) - mx_mpy(avg, prev);
      level[i] = x->power;
   }
   for (int k = 0; k < 3; ++k) x->y6d[k] = (int32_t)(level[11 + k] & 0xffffff);   /* the levels went to L:$62.. */

   /* gain computer, in log2 / 64 (the louder, the smaller) */
   const mx_acc thr = mx_mpy(ctl[MX_THRESHOLD], -0x300000) + mx_w(0x320000);
   const int32_t slope = mx_sat((mx_w(0x400000) + mx_mpy(ctl[MX_RATIO], -0x3fc000)) * 2);   /* 1 / ratio */
   for (int i = 0; i < MX_FRAMES; ++i) {
      const mx_acc l = mx_log2(level[i]);
      mx_acc cut = mx_mpy(slope, mx_sat(l - thr)) + mx_w(mx_sat(thr)) - mx_w(mx_sat(l));
      if (cut < 0) cut = 0;
      const int32_t r = (int32_t)(mx_rnd(cut >> 7) >> 24);       /* 11.11: octaves and 1/2048ths */
      target[i] = (int32_t)((mx_w(mx_exp[2047 - (r & 0x7ff)]) >> ((r >> 11) & 0x3f)) >> 24);
   }

   /* the gain follows the target with ATK's coefficient, or REL's on a sample where the target has risen */
   const int32_t atk = mx_fast[mx_index(ctl[MX_ATTACK])], rel = mx_slow[mx_index(ctl[MX_RELEASE])];
   mx_acc g = mx_w(x->gain);
   for (int i = 0; i < MX_FRAMES; ++i) {
      const int32_t c = x->target >= target[i] ? atk : rel, prev = mx_sat(g);
      x->target = target[i];
      g += mx_mpy(c, target[i]) - mx_mpy(c, prev);
      gain[i] = mx_sat(g);
   }
   x->gain = mx_sat(g);

   /* make-up gain (1 + 255 GAIN^2, up to +48 dB) and mix */
   const int32_t keep = mx_sat(mx_w(MX_ONE) - mx_w(ctl[MX_MIX]));
   const int32_t makeup = mx_sat(mx_mpy(mx_sat(mx_mpy(ctl[MX_GAIN], ctl[MX_GAIN])), 0x7f8000) + mx_w(0x8000));
   const int32_t wet = mx_sat(mx_mpy(ctl[MX_MIX], makeup));
   for (int c = 0; c < 2; ++c)
      for (int i = 0; i < MX_FRAMES; ++i) {
         const int32_t squeezed = mx_sat(mx_mpy(dry[c][i], gain[i]));
         out[c][i] = mx_sat(mx_s56(mx_shl(mx_mpy(squeezed, wet), 8) + mx_mpy(keep, dry[c][i])));
      }
}

#endif
