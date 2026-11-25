/* mono_phaser.h - Monomachine FX-PHASER machine, fixed-point core.
 *
 * A native C port of the PHASER machine of Monomachine OS 1.32B, written
 * from its DSP56300 code and checked sample for sample against that code
 * running in an emulator. It contains no firmware code or data: the sine
 * table is generated at start-up the way the firmware generates it.
 *
 * Signal path, per channel:
 *   input x INP^2 x 4 = dry
 *   dry + FB x (last stage output) -> six first-order allpass stages, all
 *   with the same coefficient -> wet = 0.7071 x (dry + allpass output)
 *   output = (1 - MIX) x dry + MIX x wet
 *
 * The coefficient sweeps around CNTR by DEP x (1 - |CNTR|), driven by a
 * sine LFO at a rate set by SPD^2. WID shifts the left and right sweeps by
 * up to +-89 degrees in opposite directions. CNTR, DEP and WID glide 2% of
 * the way to their setting per 16-sample block; FB, the sweep span and the
 * WID rotation are interpolated across each block.
 *
 * The arithmetic follows the DSP: 24-bit words, 56-bit accumulators,
 * truncation and saturation where the original has them. Blocks are 16
 * stereo frames at 44.1 kHz; controls are the 24-bit block words the
 * Monomachine's main CPU sends (knob value << 16).
 *
 * Plain C99, no dependencies. SPDX-License-Identifier: GPL-3.0-only */
#ifndef MONO_PHASER_H
#define MONO_PHASER_H

#include <stdint.h>
#include <string.h>

#define MP_FRAMES 16
#define MP_STAGES 6
#define MP_ONE 0x7fffff

enum { MP_CENTER, MP_DEPTH, MP_SPEED, MP_MIX, MP_FEEDBACK, MP_WIDTH, MP_INPUT, MP_CONTROLS };   /* CNTR DEP SPD MIX FB WID INP */

/* ---- DSP56300 arithmetic ------------------------------------------------- */

typedef int64_t mp_acc;           /* 56-bit accumulator, sign-extended; also 24.24 long words */

static inline mp_acc mp_w(int32_t x) { return (mp_acc)x * 16777216; }
static inline mp_acc mp_mpy(int32_t x, int32_t y) { return (mp_acc)x * y * 2; }
static inline int32_t mp_sat(mp_acc a) {           /* accumulator -> 24-bit word (limiter) */
   const mp_acc v = a >> 24;
   return v > MP_ONE ? MP_ONE : v < -MP_ONE - 1 ? -MP_ONE - 1 : (int32_t)v;
}
static inline mp_acc mp_sat48(mp_acc a) {          /* saturation mode: clip the result to 48 bits */
   const mp_acc hi = ((mp_acc)1 << 47) - 1;
   return a > hi ? hi : a < -hi - 1 ? -hi - 1 : a;
}

/* ---- sine table ----------------------------------------------------------- */

/* sin(k pi / 4096), k = 0..4095, as the boot code builds it: a 48-bit
 * recurrence s[k+1] = 2 cos(pi/4096) s[k] - s[k-1] with a truncating
 * double-precision multiply, saturating at the peak. */
static int32_t mp_sine[0x1000];

static void mp_tables(void) {
   const int32_t c1 = 0x7ffffd, c0 = 0x885869;                 /* round(cos(pi/4096) x 2^47) */
   mp_acc prev = 0, cur = ((mp_acc)0x1921 << 24) | 0xfb2aed;   /* round(sin(pi/4096) x 2^47) */
   mp_sine[0] = 0;
   mp_sine[1] = 0x1921;
   for (int k = 2; k < 0x1000; ++k) {
      const int32_t s1 = (int32_t)(cur >> 24), s0 = (int32_t)(cur & 0xffffff);
      mp_acc a = (mp_acc)c0 * s0 * 2;
      a = (a >> 24) + (mp_acc)c1 * s0 * 2;
      a += (mp_acc)s1 * c0 * 2;
      a = (a >> 24) + (mp_acc)c1 * s1 * 2;
      const mp_acc next = mp_sat48(a * 2 - prev);
      mp_sine[k] = (int32_t)(next >> 24);
      prev = cur;
      cur = next;
   }
}

/* ---- the machine ---------------------------------------------------------- */

typedef struct {
   int32_t lfo_sin, lfo_cos;     /* LFO pair, ends of the last block */
   mp_acc center, depth;         /* CNTR, DEP after the 2% glide (24.24) */
   int32_t span;                 /* DEP x (1 - |CNTR|) at the last block */
   int32_t width;                /* WID after the glide */
   int32_t rot_cos, rot_sin;     /* the WID rotation at the last block */
   int32_t feedback;             /* 2 x FB - 1 at the last block */
   int32_t ap[2][MP_STAGES + 1]; /* per channel: last allpass input, last output of each stage */
} MonoPhaser;

/* The machine's init routine: LFO at -1, glides and allpass memory at 0. */
static void mp_reset(MonoPhaser *p) {
   memset(p, 0, sizeof *p);
   p->lfo_sin = -MP_ONE - 1;
}

static const int32_t mp_glide = 0x028f5c;          /* 0.02 */
static const int32_t mp_sixteenth = 0x080000;      /* 1/16 */

/* ramp[i] = from + i/16 of the way to `to`, as the firmware interpolates */
static void mp_ramp(mp_acc from, int32_t to, int32_t ramp[MP_FRAMES]) {
   const mp_acc step = mp_mpy(mp_sat(mp_w(to) - from), mp_sixteenth);
   for (int i = 0; i < MP_FRAMES; ++i) ramp[i] = mp_sat(from + i * step);
}

/* One 16-frame block. ctl: the block words of CNTR DEP SPD MIX FB WID INP. */
static void mp_block(MonoPhaser *p, const int32_t ctl[MP_CONTROLS],
                     const int32_t in[2][MP_FRAMES], int32_t out[2][MP_FRAMES]) {
   int32_t dry[2][MP_FRAMES], lfo[2][MP_FRAMES], rc[MP_FRAMES], rs[MP_FRAMES], span[MP_FRAMES], fb[MP_FRAMES];

   const int32_t gain = mp_sat(mp_mpy(ctl[MP_INPUT], ctl[MP_INPUT]));
   for (int c = 0; c < 2; ++c)
      for (int i = 0; i < MP_FRAMES; ++i) dry[c][i] = mp_sat(mp_mpy(in[c][i], gain) * 4);

   /* LFO: a coupled sine/cosine recurrence in saturation mode. The cosine
    * update reads the sine of one sample earlier, as the DSP's pipelined
    * register moves have it. */
   const int32_t rate = mp_sat(mp_mpy(mp_sat(mp_mpy(ctl[MP_SPEED], ctl[MP_SPEED])), 0x9566));
   mp_acc s = mp_w(p->lfo_sin), co = mp_w(p->lfo_cos);
   int32_t s_late = mp_sat(s), sn[MP_FRAMES], cs[MP_FRAMES];
   for (int i = 0; i < MP_FRAMES; ++i) {
      cs[i] = mp_sat(co);
      sn[i] = mp_sat(s);
      co = mp_sat48(co - mp_mpy(s_late, rate));
      s_late = sn[i];
      s = mp_sat48(s + mp_mpy(cs[i], rate));
   }
   p->lfo_sin = mp_sat(s);
   p->lfo_cos = mp_sat(co);

   /* WID: rotate the LFO by +-w for the left and right sweeps */
   mp_acc a = mp_w(p->width) + mp_mpy(ctl[MP_WIDTH], mp_glide) - mp_mpy(p->width, mp_glide);
   p->width = mp_sat(a);
   const int k = p->width >> 12;
   const int32_t old_s = p->rot_sin, old_c = p->rot_cos;
   p->rot_sin = mp_sine[k];
   p->rot_cos = mp_sine[k + 0x800];
   mp_ramp(mp_w(old_s), p->rot_sin, rs);
   mp_ramp(mp_w(old_c), p->rot_cos, rc);
   for (int i = 0; i < MP_FRAMES; ++i) {
      lfo[0][i] = mp_sat(mp_mpy(rc[i], cs[i]) + mp_mpy(rs[i], sn[i]));
      lfo[1][i] = mp_sat(mp_mpy(rc[i], cs[i]) - mp_mpy(rs[i], sn[i]));
   }

   /* sweep = CNTR' + LFO x DEP' x (1 - |CNTR'|), CNTR' = 2 x CNTR - 1 */
   p->center += mp_mpy(ctl[MP_CENTER], mp_glide) - mp_mpy(mp_sat(p->center), mp_glide);
   p->depth += mp_mpy(ctl[MP_DEPTH], mp_glide) - mp_mpy(mp_sat(p->depth), mp_glide);
   const mp_acc c2 = (p->center - mp_w(0x400000)) * 2;
   const int32_t center = mp_sat(c2);
   const int32_t room = mp_sat(-((c2 < 0 ? -c2 : c2) + mp_w(-MP_ONE - 1)));
   const int32_t old_span = p->span;
   p->span = mp_sat(mp_mpy(mp_sat(p->depth), room));
   mp_ramp(mp_w(old_span), p->span, span);

   /* FB: 2 x FB - 1, interpolated across the block */
   const int32_t old_fb = p->feedback;
   p->feedback = mp_sat((mp_w(ctl[MP_FEEDBACK]) - mp_w(0x400000)) * 2);
   mp_ramp(mp_w(old_fb), p->feedback, fb);

   const int32_t mix = ctl[MP_MIX], keep = mp_sat(mp_w(MP_ONE) - mp_w(mix));
   for (int c = 0; c < 2; ++c) {
      int32_t *m = p->ap[c];
      for (int i = 0; i < MP_FRAMES; ++i) {
         const int32_t g = mp_sat(mp_mpy(lfo[c][i], span[i]) + mp_w(center));
         int32_t v = mp_sat(mp_w(dry[c][i]) + mp_mpy(fb[i], m[MP_STAGES]));
         for (int j = 0; j < MP_STAGES; ++j) {   /* y = g x + x[n-1] - g y[n-1] */
            const int32_t y = mp_sat(mp_w(m[j]) + mp_mpy(v, g) - mp_mpy(m[j + 1], g));
            m[j] = v;
            v = y;
         }
         m[MP_STAGES] = v;
         const int32_t wet = mp_sat(mp_mpy(0x5a8241, dry[c][i]) + mp_mpy(0x5a8241, v));
         out[c][i] = mp_sat(mp_mpy(keep, dry[c][i]) + mp_mpy(mix, wet));
      }
   }
}

#endif
