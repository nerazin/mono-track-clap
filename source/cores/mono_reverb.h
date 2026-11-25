/* mono_reverb.h - Monomachine FX-REVERB machine, fixed-point core.
 *
 * A native C port of the REVERB machine of Monomachine OS 1.32B, written
 * from its DSP56300 code and checked sample for sample against that code
 * running in an emulator. It contains no firmware code: its tables are
 * generated from formulas at start-up (two entries are corrected).
 *
 * Signal path:
 *   (left + right) x INP^2 -> five allpass diffusers
 *   -> six delay lines (2663, 2447, 1811, 1621, 1399 and 613 samples; the
 *      last two read with a slow +-4 sample wobble), each read through a
 *      one-pole low-pass (DAMP) and fed back through a sum/difference matrix
 *      with gains for a decay time of 0.4 to 4 s (DEC)
 *   -> two outputs from the matrix -> gate (GATE) -> high-pass (HP)
 *   -> low-pass (LP) = wet
 *   output = MIX x wet + (1 - MIX) x INP^2 x 4 x input
 * For its first 256 blocks after the machine starts, the reverb is silent.
 *
 * The arithmetic follows the DSP: 24-bit words, 56-bit accumulators,
 * truncation and saturation where the original has them. Blocks are 16
 * stereo frames at 44.1 kHz; controls are the 24-bit block words the
 * Monomachine's main CPU sends (knob value << 16).
 *
 * This copy (Mono Track's) also keeps the words the block leaves in Y:$6d..$6f,
 * which the track's AMP stage reads as the low half of the filter's output.
 *
 * C99 and libm (for the tables). SPDX-License-Identifier: GPL-3.0-only */
#ifndef MONO_REVERB_H
#define MONO_REVERB_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#define MR_FRAMES 16
#define MR_START 256              /* blocks before the reverb comes in */
#define MR_ONE 0x7fffff
#define MR_LINES 6
#define MR_DIFFUSERS 5

enum { MR_DECAY, MR_DAMP, MR_GATE, MR_MIX, MR_HIGHPASS, MR_LOWPASS, MR_INPUT, MR_CONTROLS };   /* DEC DAMP GATE MIX HP LP INP */

/* ---- DSP56300 arithmetic ------------------------------------------------- */

typedef int64_t mr_acc;           /* 56-bit accumulator, sign-extended; also 24.24 long words */

static inline mr_acc mr_w(int32_t x) { return (mr_acc)x * 16777216; }
static inline mr_acc mr_mpy(int32_t x, int32_t y) { return (mr_acc)x * y * 2; }
static inline int32_t mr_sat(mr_acc a) {           /* accumulator -> 24-bit word (limiter) */
   const mr_acc v = a >> 24;
   return v > MR_ONE ? MR_ONE : v < -MR_ONE - 1 ? -MR_ONE - 1 : (int32_t)v;
}
static inline mr_acc mr_sat48(mr_acc a) {
   const mr_acc hi = ((mr_acc)1 << 47) - 1;
   return a > hi ? hi : a < -hi - 1 ? -hi - 1 : a;
}

/* ---- the machine's shape ------------------------------------------------- */

static const int mr_len[MR_LINES] = {2679, 2463, 1827, 1637, 2048, 1024};    /* delay line buffers */
static const int mr_delay[MR_LINES] = {2663, 2447, 1811, 1621, 1399, 613};   /* write ahead of read */
static const int mr_dlen[MR_DIFFUSERS] = {47, 67, 101, 149, 223};
static const int32_t mr_dgain[MR_DIFFUSERS] = {-0x7b4635, 0x79512f, -0x760f91, 0x719c8d, -0x6b1412};

/* ---- tables --------------------------------------------------------------- */

/* DEC (64 rows): each line's feedback, 0.7071 x 10^(-3 d / (44100 T)), T = 0.4 s x 10^(r/63).
 * DAMP (64 rows): each line's low-pass coefficient, p^d, p = (1 - 2^-23) - 0.000025 r.
 * Filters: the one-pole coefficient table the machines share (10 Hz x 2000^(k/128)).
 * LFO: every 16th entry of the boot code's sine table (sin(k pi / 4096), 48-bit recurrence). */
static int32_t mr_dec[64][MR_LINES], mr_damp[64][MR_LINES], mr_coef[128], mr_lfo[512];

static void mr_tables(void) {
   for (int r = 0; r < 64; ++r)
      for (int j = 0; j < MR_LINES; ++j) {
         mr_dec[r][j] = (int32_t)lround(sqrt(0.5) * pow(10, -3.0 * mr_delay[j] / (44100 * 0.4 * pow(10, r / 63.0))) * 8388608);
         mr_damp[r][j] = (int32_t)lround(pow(8388607 / 8388608.0 - 0.000025 * r, mr_delay[j]) * 8388608);
      }
   mr_dec[35][0] += 1;   /* the formula is one LSB off the original in these two */
   mr_dec[61][2] += 1;
   const double m = sqrt(2.0) - 1.0;
   for (int k = 0; k < 128; ++k) {
      const double c = 1.0 - cos(2.0 * 3.14159265358979323846 * 10.0 * pow(2000.0, k / 128.0) / 44100.0);
      mr_coef[k] = (int32_t)floor((sqrt(c * c + 2.0 * c * m) - c) / m * 8388608.0 + 0.5);
   }
   mr_coef[50] += 1; mr_coef[70] += 1; mr_coef[99] -= 1;   /* as in the original */
   const int32_t c1 = 0x7ffffd, c0 = 0x885869;                 /* round(cos(pi/4096) x 2^47) */
   mr_acc prev = 0, cur = ((mr_acc)0x1921 << 24) | 0xfb2aed;   /* round(sin(pi/4096) x 2^47) */
   mr_lfo[0] = 0;
   for (int k = 2; k < 0x2000; ++k) {
      const int32_t s1 = (int32_t)(cur >> 24), s0 = (int32_t)(cur & 0xffffff);
      mr_acc a = (mr_acc)c0 * s0 * 2;
      a = (a >> 24) + (mr_acc)c1 * s0 * 2;
      a += (mr_acc)s1 * c0 * 2;
      a = (a >> 24) + (mr_acc)c1 * s1 * 2;
      const mr_acc next = mr_sat48(a * 2 - prev);
      if (!(k & 15)) mr_lfo[k >> 4] = (int32_t)(next >> 24);
      prev = cur;
      cur = next;
   }
}

/* ---- the machine ---------------------------------------------------------- */

typedef struct {
   int32_t blocks;                          /* blocks run, up to MR_START */
   int32_t diff[MR_DIFFUSERS][223];         /* allpass diffusers: buffer, position, */
   int diff_pos[MR_DIFFUSERS];
   int32_t diff_last[MR_DIFFUSERS];         /* and the value still to be written */
   int32_t line[MR_LINES][2679];
   int rd[MR_LINES], wr[MR_LINES];
   mr_acc damp[MR_LINES];                   /* the lines' low-pass states (24.24) */
   int lfo;                                 /* 0..511 */
   int32_t env, peak, hold, gain;           /* gate: level, block peak, hold count, gain */
   mr_acc hp[2], lp[2];                     /* filter states (24.24) */
   int32_t hp_in[2];                        /* the high-pass's last input */
   int32_t y6d[3];                          /* Y:$6d..$6f after the block: the fifth line's last three reads */
} MonoReverb;

/* The machine's init routine: buffers and states at 0, reads at the start of
 * each line, writes the line's delay ahead. */
static void mr_reset(MonoReverb *v) {
   memset(v, 0, sizeof *v);
   for (int j = 0; j < MR_LINES; ++j) v->wr[j] = mr_delay[j];
}

static int mr_wrap(int p, int len) { return p >= len ? p - len : p < 0 ? p + len : p; }

/* One 16-frame block. ctl: the block words of DEC DAMP GATE MIX HP LP INP. */
static void mr_block(MonoReverb *v, const int32_t ctl[MR_CONTROLS],
                     const int32_t in[2][MR_FRAMES], int32_t out[2][MR_FRAMES]) {
   static const int32_t silent[MR_LINES] = {0};
   int32_t mono[MR_FRAMES], ln[MR_LINES][MR_FRAMES], fb[MR_LINES][MR_FRAMES], st[2][MR_FRAMES], gg[MR_FRAMES];

   const int32_t open = v->blocks < MR_START ? 0 : MR_ONE;
   if (v->blocks < MR_START) ++v->blocks;
   const int32_t *damp = open ? mr_damp[ctl[MR_DAMP] >> 17] : silent, *g = mr_dec[ctl[MR_DECAY] >> 17];
   const mr_acc hold2 = mr_mpy(ctl[MR_GATE], ctl[MR_GATE]) >> 11;
   const int32_t hold_time = hold2 > mr_w(0xfa0) ? 1000000 : mr_sat(hold2);   /* GATE^2 / 4 blocks; 127: none */

   /* mono input and the diffusers, whose gains grow with DEC */
   const int32_t gin = mr_sat(mr_mpy(ctl[MR_INPUT], ctl[MR_INPUT]));
   for (int i = 0; i < MR_FRAMES; ++i) mono[i] = mr_sat((mr_mpy(gin, in[0][i]) + mr_mpy(gin, in[1][i])) * 2);
   const int32_t s = mr_sat(mr_mpy(mr_sat(mr_mpy(ctl[MR_DECAY], 0x66666) + mr_w(0x59999a)), open));
   for (int j = 0; j < MR_DIFFUSERS; ++j) {
      const int32_t k = mr_sat(mr_mpy(s, mr_dgain[j]));
      int32_t *buf = v->diff[j], b = v->diff_last[j];
      int p = v->diff_pos[j];
      for (int i = 0; i < MR_FRAMES; ++i) {   /* each write lags its read by a sample, as the DSP's moves do */
         const int32_t old = buf[p];
         buf[p] = b;
         p = mr_wrap(p + 1, mr_dlen[j]);
         const int32_t y = mr_sat(mr_w(old) - mr_mpy(k, b));
         b = mr_sat(mr_w(mono[i]) + mr_mpy(k, buf[p]));
         mono[i] = y;
      }
      v->diff_pos[j] = p;
      v->diff_last[j] = b;
   }

   /* read the lines through their low-passes; the last two with the LFO's wobble */
   for (int j = 0; j < 4; ++j) {
      const int32_t c = damp[j];
      mr_acc a = v->damp[j];
      for (int i = 0; i < MR_FRAMES; ++i) {
         const int32_t y = mr_sat(a);
         ln[j][i] = y;
         a += mr_mpy(c, v->line[j][mr_wrap(v->rd[j] + i, mr_len[j])]) - mr_mpy(c, y);
      }
      v->damp[j] = a;
      v->rd[j] = mr_wrap(v->rd[j] + MR_FRAMES, mr_len[j]);
   }
   const mr_acc wob = mr_w(mr_lfo[v->lfo]) >> 21;
   v->lfo = (v->lfo + 1) & 511;
   const int off = (int)(wob >> 24);                        /* -4..3 samples */
   const int32_t t = (int32_t)((wob & 0xffffff) >> 1), c4 = damp[4];
   const int32_t far = mr_sat(mr_mpy(c4, mr_sat(mr_w(MR_ONE) - mr_w(t)))), near = mr_sat(mr_mpy(t, c4));
   for (int j = 4; j < MR_LINES; ++j) {   /* both use DAMP's fifth coefficient */
      mr_acc a = v->damp[j];
      int q = mr_wrap(v->rd[j] + off, mr_len[j]);
      int32_t x = v->line[j][q];
      for (int i = 0; i < MR_FRAMES; ++i) {
         const int32_t y = mr_sat(a);
         ln[j][i] = y;
         q = mr_wrap(q + 1, mr_len[j]);
         a += mr_mpy(far, x) + mr_mpy(near, v->line[j][q]) - mr_mpy(c4, y);
         x = v->line[j][q];
      }
      v->damp[j] = a;
      v->rd[j] = mr_wrap(v->rd[j] + MR_FRAMES, mr_len[j]);
   }
   memcpy(v->y6d, &ln[4][13], sizeof v->y6d);   /* the fifth line's reads went to Y:$60.. */

   /* the two outputs and the feedback, through sums and differences of the lines x DEC's gains */
   for (int i = 0; i < MR_FRAMES; ++i) {
      const mr_acc common = mr_mpy(g[2], ln[2][i]) - mr_mpy(g[5], ln[5][i]) - mr_mpy(g[3], ln[3][i]);
      st[0][i] = mr_sat((common + mr_mpy(g[1], ln[1][i])) * 2);
      st[1][i] = mr_sat((common + mr_mpy(g[0], ln[0][i])) * 2);
      fb[0][i] = mr_sat(-mr_mpy(g[4], ln[4][i]) + mr_mpy(g[5], ln[5][i]));
      fb[1][i] = mr_sat(mr_mpy(g[0], ln[0][i]) + mr_mpy(g[3], ln[3][i]));
      fb[2][i] = mr_sat(mr_mpy(g[0], ln[0][i]) - mr_mpy(g[3], ln[3][i]));
      fb[3][i] = mr_sat(-mr_mpy(g[4], ln[4][i]) - mr_mpy(g[5], ln[5][i]));
      fb[4][i] = mr_sat(mr_mpy(g[1], ln[1][i]) + mr_mpy(g[2], ln[2][i]));
      fb[5][i] = mr_sat(mr_mpy(g[1], ln[1][i]) - mr_mpy(g[2], ln[2][i]));
   }
   for (int j = 0; j < MR_LINES; ++j) {
      for (int i = 0; i < MR_FRAMES; ++i)
         v->line[j][mr_wrap(v->wr[j] + i, mr_len[j])] = mr_sat(mr_w(fb[j][i]) + mr_mpy(mono[i], 0x155556));   /* + input / 6 */
      v->wr[j] = mr_wrap(v->wr[j] + MR_FRAMES, mr_len[j]);
   }

   /* gate: a level follower on the diffused input holds the gain open for GATE's time after the last rise */
   mr_acc a = mr_w(v->env), pk = 0;
   for (int i = 0; i < MR_FRAMES; ++i) {
      const int32_t e = mr_sat(a), x = mr_sat(mono[i] < 0 ? -mr_w(mono[i]) : mr_w(mono[i]));
      a = mr_mpy(e, 0x7fef9e) + mr_mpy(x, 0x1062);
      if ((a < 0 ? -a : a) >= (pk < 0 ? -pk : pk)) pk = a;
   }
   v->env = mr_sat(a);
   const mr_acc rise = pk - mr_w(v->peak);
   v->peak = mr_sat(pk);
   v->hold = v->hold >= hold_time ? 0x7ffff0 : v->hold + 1;
   int closing = 0;
   if (hold_time != MR_ONE) {
      if (rise >= mr_w(0xa8)) v->hold = 0;
      else closing = v->hold >= hold_time;
   }
   if (!closing) {
      mr_acc gn = mr_w(v->gain);
      for (int i = 0; i < MR_FRAMES; ++i) { gg[i] = mr_sat(gn); gn += mr_w(0x147ae); }   /* +0.01 a sample */
      v->gain = mr_sat(gn);
   } else {   /* x 0.99, through the DSP's pipeline, which holds each step for two samples at first */
      int32_t x = v->gain;
      mr_acc gn = mr_w(v->gain);
      for (int i = 0; i < MR_FRAMES; ++i) {
         const mr_acc next = mr_mpy(x, 0x7eb852);
         x = gg[i] = mr_sat(gn);
         gn = next;
      }
      v->gain = mr_sat(gn);
   }
   for (int c = 0; c < 2; ++c)
      for (int i = 0; i < MR_FRAMES; ++i) st[c][i] = mr_sat(mr_mpy(gg[i], st[c][i]));

   /* high-pass, then low-pass; each output trails its input by a sample */
   const int32_t ch = mr_coef[ctl[MR_HIGHPASS] >> 16], cl = mr_coef[ctl[MR_LOWPASS] >> 16];
   const int32_t gh = mr_sat(-mr_mpy(0x400000, mr_sat(mr_w(ch) + mr_w(-MR_ONE - 1))));   /* (1 - c) / 2 */
   const int32_t mix = ctl[MR_MIX];
   const int32_t dry = mr_sat(mr_mpy(mr_sat(mr_w(MR_ONE) - mr_w(mix)), gin));
   for (int c = 0; c < 2; ++c) {
      mr_acc h = v->hp[c], l = v->lp[c];
      int32_t xp = v->hp_in[c];
      for (int i = 0; i < MR_FRAMES; ++i) {
         const int32_t yh = mr_sat(h), yl = mr_sat(l);
         h += -mr_mpy(xp, gh) - mr_mpy(ch, yh) + mr_mpy(st[c][i], gh);
         xp = st[c][i];
         l += mr_mpy(yh, cl) - mr_mpy(yl, cl);
         out[c][i] = mr_sat((mr_mpy(mix, yl) + mr_mpy(dry, in[c][i])) * 4);
      }
      v->hp[c] = h;
      v->lp[c] = l;
      v->hp_in[c] = xp;
   }
}

#endif
