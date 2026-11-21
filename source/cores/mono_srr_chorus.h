/* mono_srr_chorus.h - the Monomachine track's SRR stage followed by SWAVE
 * ENS's chorus, fixed-point core.
 *
 * A native C port of these two parts of Monomachine OS 1.32B, written from
 * its DSP56300 code and checked sample for sample against that code running
 * in an emulator. It contains no firmware code or data: its tables are
 * generated from formulas at start-up.
 *
 * SRR (the kernel's P:$912-$978, after the AMP section). SRR k takes the
 * signal down to 44.1 kHz / (1 + 0.1172 k) and back, both times by linear
 * interpolation, not by holding samples: in each block it reads the input at
 * a fractional phase and every `step` samples after it, then reads those
 * down-rate samples at 1/step intervals to make 16 samples again. That puts
 * the output step + 1 samples behind the input (2 at SRR 0, where the stage
 * is a plain delay). Two firmware details matter and are kept: the phase
 * carried to the next block keeps only half its fraction, and at SRR 0 the
 * phase halves every block. After SRR drops, the phase can pass the new step
 * and the next block reads below its down-rate buffer, partly outside SRR's
 * memory, where the unit holds its filter's, EQ's and delay's working data.
 * This core keeps SRR's memory layout (X:$0..$fe) with the stores' timing and
 * reads those other stages' words as 0.
 *
 * Chorus (SWAVE ENS's hook P:$145c48, which runs right after SRR). A mono sum
 * goes into a delay of 1024 samples, read at 400 +- 340 samples by two LFOs a
 * quarter turn apart (0.2 Hz); left gets the first and right the second, both
 * at CHRL (smoothed) and panned at the track's PAN (held at its centre here),
 * each mixed into the other side by 1 - CHRW / 2; the dry part falls by
 * CHRL. The wet is muted for the first 64 blocks after the machine starts.
 *
 * The arithmetic follows the DSP: 24-bit words, 56-bit accumulators,
 * truncation, rounding and saturation where the original has them. Blocks
 * are 16 stereo frames at 44.1 kHz; controls are the 24-bit block words the
 * Monomachine's main CPU sends (knob value << 16).
 *
 * C99 and libm (for the tables). SPDX-License-Identifier: GPL-3.0-only */
#ifndef MONO_SRR_CHORUS_H
#define MONO_SRR_CHORUS_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#define SR_FRAMES 16
#define SR_ONE 0x7fffff
#define SR_GATE 64     /* blocks before the chorus's wet comes in */
#define SR_MEM 0xff    /* X:$0..$fe: SRR's buffers ($0..$45), positions ($80..$90) and step ($fe) */

enum { SR_SRR, SR_CHRL, SR_CHRW, SR_CONTROLS };   /* EFX SRR, ENS's CHRL and CHRW */

/* ---- DSP56300 arithmetic ------------------------------------------------- */

typedef int64_t sr_acc;           /* 56-bit accumulator, sign-extended; also 24.24 long words */

static inline sr_acc sr_s56(sr_acc v) { return (sr_acc)((uint64_t)v << 8) >> 8; }
static inline sr_acc sr_shl(sr_acc v, int n) { return sr_s56((sr_acc)((uint64_t)v << n)); }
static inline sr_acc sr_w(int32_t x) { return (sr_acc)x * 16777216; }
static inline sr_acc sr_mpy(int32_t x, int32_t y) { return (sr_acc)x * y * 2; }
static inline sr_acc sr_mpysu(int32_t s, uint32_t u) { return (sr_acc)s * (sr_acc)(u & 0xffffff) * 2; }
static inline int32_t sr_hi(sr_acc a) { return (int32_t)((uint32_t)(a >> 24) << 8) >> 8; }   /* a1 as stored */
static inline uint32_t sr_lo(sr_acc a) { return (uint32_t)a & 0xffffff; }                    /* a0 */
static inline sr_acc sr_long(int32_t hi, uint32_t lo) { return sr_w(hi) | (lo & 0xffffff); }
static inline int32_t sr_sat(sr_acc a) {
   const sr_acc v = a >> 24;
   return v > SR_ONE ? SR_ONE : v < -SR_ONE - 1 ? -SR_ONE - 1 : (int32_t)v;
}
static inline sr_acc sr_sat48(sr_acc a) {
   const sr_acc hi = ((sr_acc)1 << 47) - 1;
   return a > hi ? hi : a < -hi - 1 ? -hi - 1 : a;
}
/* x times a 48-bit long word, the upper 48 bits (MPYSU then DMAC) */
static inline sr_acc sr_dmul(int32_t x, sr_acc y) { return (sr_mpysu(x, sr_lo(y)) >> 24) + sr_mpy(x, sr_hi(y)); }
/* RND: convergent rounding of a at a1 */
static inline sr_acc sr_rnd(sr_acc a) {
   const uint32_t lo = sr_lo(a);
   sr_acc r = a - lo;
   if (lo > 0x800000 || (lo == 0x800000 && (sr_hi(a) & 1))) r += sr_w(1);
   return r;
}

/* ---- tables --------------------------------------------------------------- */

static int32_t sr_step[128];      /* SRR's step by knob, 16.16 samples (Y:$144946) */
static int32_t sr_gain[128];      /* 0.5 / step (Y:$1448c6) */
static int32_t sr_pan[2];         /* the chorus's right and left multipliers at PAN 64 (Y:$0, Y:$1) */

static void sr_tables(void) {
   for (int k = 0; k < 128; ++k) {
      sr_step[k] = 0x10000 + k * 0x1e00;
      sr_gain[k] = (int32_t)rint(0x400000 * 65536.0 / sr_step[k]);
   }
   /* PAN's quarter sine from the boot code's sine X:$14a000, a 48-bit
    * recurrence (as in Mono SWAVE); only the two entries PAN 64 reads */
   const int32_t c1 = 0x7ffffd, c0 = 0x885869;                 /* round(cos(pi/4096) x 2^47) */
   sr_acc prev = 0, cur = ((sr_acc)0x1921 << 24) | 0xfb2aed;   /* round(sin(pi/4096) x 2^47) */
   const int p = sr_sat(sr_shl(sr_mpy(0x408e05, 64 << 16), 1)) >> 12;
   for (int k = 2; k <= p + 0x801; ++k) {
      const int32_t s1 = (int32_t)(cur >> 24), s0 = (int32_t)(cur & 0xffffff);
      sr_acc a = (sr_acc)c0 * s0 * 2;
      a = (a >> 24) + (sr_acc)c1 * s0 * 2;
      a += (sr_acc)s1 * c0 * 2;
      a = (a >> 24) + (sr_acc)c1 * s1 * 2;
      const sr_acc next = sr_sat48(a * 2 - prev);
      if (k == p) sr_pan[0] = (int32_t)(next >> 24);
      if (k == p + 0x801) sr_pan[1] = (int32_t)(next >> 24);
      prev = cur;
      cur = next;
   }
}

/* ---- state ---------------------------------------------------------------- */

typedef struct {
   /* SRR */
   int32_t mem[SR_MEM];          /* X:$0..: input L (17) and R (17), the down-rate L (18) and R (18), ..., positions */
   uint8_t junk[SR_MEM];         /* 1: the firmware has another stage's data there (filter, EQ, delay), not held here */
   int junk_out;                 /* last block: output samples that read such data (test use) */
   int32_t last[2];              /* the previous block's last input sample */
   int32_t phase_hi;             /* where the next down-rate sample is taken, in samples (24.24) */
   uint32_t phase_lo;
   int32_t s1[2], s2[2];         /* the last two down-rate samples */
   /* chorus */
   int32_t count, gate, level;   /* blocks since the start (to 64), the wet's gate, CHRL smoothed */
   int32_t lfo[2];               /* its two LFOs, cosine and sine */
   int wp;                       /* the delay's write position */
   int32_t delay[1024];
} MonoSrrChorus;

/* The state after the ENS machine's assignment, from cleared memory. */
static void sr_reset(MonoSrrChorus *s) {
   memset(s, 0, sizeof *s);
   s->lfo[0] = -SR_ONE - 1;
   memset(s->junk + 0x46, 1, 0x80 - 0x46);
   memset(s->junk + 0x91, 1, 0xfe - 0x91);
}

/* ---- SRR -------------------------------------------------------------------- */

/* Reads and writes of the model of X memory; outside it, the firmware has
 * other kernel stages' data (filter, EQ), read here as 0 and marked junk. */
static int32_t sr_rd(const MonoSrrChorus *s, int32_t addr, uint8_t *junk) {
   if (addr >= 0 && addr < SR_MEM) { *junk |= s->junk[addr]; return s->mem[addr]; }
   *junk = 1;
   return 0;
}
static void sr_wr(MonoSrrChorus *s, int32_t addr, int32_t v, uint8_t junk) {
   if (addr >= 0 && addr < SR_MEM) { s->mem[addr] = v; s->junk[addr] = junk; }
}

/* P:$397: 16 left/right pairs by linear interpolation at pos + fr (fr a Q23
 * fraction): left from X:p and X:p+1, right from X:p+off and X:p+off+1. Its
 * stores lag one output behind: output n-1 (before output 0, the word first)
 * is stored between the two reads of output n, left and right each on its
 * own; at[c][0] is where first goes, at[c][n + 1] where output n goes. That
 * timing matters when a position reaches into the buffers being written.
 * Returns the outputs that read junk. */
static int sr_lerp(MonoSrrChorus *s, const int32_t *pos, const int32_t *fr, int off, const int32_t first[2],
                   uint8_t first_junk, const int at[2][17], int32_t out[2][SR_FRAMES]) {
   int32_t prev[2] = {first[0], first[1]};
   uint8_t pj[2] = {first_junk, first_junk};
   int junk = 0;
   for (int n = 0; n < 16; ++n)
      for (int c = 0; c < 2; ++c) {
         const int32_t p = pos[n] + (c ? off : 0), f = fr[n];
         uint8_t j = 0;
         const int32_t x0 = sr_rd(s, p, &j);
         sr_wr(s, at[c][n], prev[c], pj[c]);
         const int32_t x1 = sr_rd(s, p + 1, &j);
         prev[c] = out[c][n] = sr_sat(sr_rnd(sr_w(x0) - sr_mpy(x0, f) + sr_mpy(x1, f)));
         pj[c] = j;
         junk += j;
      }
   for (int c = 0; c < 2; ++c) sr_wr(s, at[c][16], prev[c], pj[c]);
   return junk;
}

/* Positions from a 24.24 start, every step: the integer part (also stored
 * at X:$80..), and the fraction kept beside it as a Q23 word that grows by df
 * and wraps (P:$929, P:$96e). */
static void sr_positions(MonoSrrChorus *s, sr_acc a, sr_acc step, uint32_t df, int count, int32_t *pos, int32_t *fr) {
   uint32_t f = sr_lo(a) >> 1;
   for (int n = 0; n < count; ++n) {
      pos[n] = sr_hi(a);
      fr[n] = (int32_t)f;
      sr_wr(s, 0x80 + n, pos[n], 0);
      a += step;
      f = (f + df) & 0x7fffff;
   }
}

/* One block of SRR, in place. io: the AMP section's left and right. */
static void sr_srr(MonoSrrChorus *s, int32_t word, int32_t io[2][SR_FRAMES]) {
   static const int down[2][17] = {
      {0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33},
      {0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45}};
   static const int up[2][17] = {
      {0x0f, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f},
      {0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f}};
   int k = sr_hi(sr_rnd(sr_mpy(word, 0x80)));
   /* shortcut: SRR words above 0x7f7fff would read past the firmware's tables;
    * the plug-in never sends them (its knob words end at 0x7f0000) */
   k = k < 0 ? 0 : k > 127 ? 127 : k;
   const int32_t g = sr_gain[k];
   const sr_acc step = sr_w(sr_step[k]) >> 16;      /* 24.24 samples */
   sr_wr(s, 0xfe, sr_step[k], 0);

   /* the AMP section's products, each channel led by its previous block's last sample */
   for (int c = 0; c < 2; ++c) {
      sr_wr(s, 0x11 * c, s->last[c], 0);
      for (int n = 0; n < SR_FRAMES; ++n) sr_wr(s, 0x11 * c + 1 + n, io[c][n], 0);
      s->last[c] = io[c][SR_FRAMES - 1];
   }

   /* the delay, after SRR, uses X:$20..$61 as its own working space, so by
    * now the down-rate buffers hold its data: unknown here, read as 0 */
   for (int a = 0x22; a < 0x46; ++a) sr_wr(s, a, 0, 1);

   sr_acc p = sr_long(s->phase_hi, s->phase_lo);
   if (g == 0x400000) p >>= 1;                      /* SRR 0 */

   /* down: 17 positions from the phase, every step; 16 are read, only those
    * inside this block are used later. The down-rate buffers hold the last
    * two samples, then the 16 new ones. */
   int32_t pos[17], fr[17], scratch[2][SR_FRAMES];
   sr_positions(s, p, step, sr_lo(step) >> 1, 17, pos, fr);
   sr_wr(s, 0x22, s->s1[0], 0);
   sr_wr(s, 0x34, s->s1[1], 0);
   sr_lerp(s, pos, fr, 0x11, s->s2, 0, down, scratch);

   /* how many down-rate samples this block made: about (16 - phase) / step,
    * rounded up; the last two and the phase carry over */
   const int at = sr_hi(sr_rnd(sr_shl(sr_dmul(g, sr_w(16) - p), 1) + sr_long(0x22, 0x7fffff)));
   int used = at - 0x22;
   used = used < 0 ? 0 : used > 16 ? 16 : used;      /* shortcut: 1..16 in every case seen */
   uint8_t j = 0;
   s->s1[0] = sr_rd(s, at, &j); s->s1[1] = sr_rd(s, at + 0x12, &j);
   s->s2[0] = sr_rd(s, at + 1, &j); s->s2[1] = sr_rd(s, at + 0x13, &j);
   sr_acc next = sr_long(pos[used], (uint32_t)fr[used]) - sr_w(16);   /* the fraction is the halved one */
   if (next > step) next = step;
   s->phase_hi = sr_hi(next);
   s->phase_lo = sr_lo(next);

   /* up: 16 positions at 1/step from 1 - phase/step in the down-rate buffer;
    * after SRR drops, the phase can pass the new step and the first positions
    * fall below it, into the input buffers (the first word stored, at X:$f,
    * is never read) */
   const sr_acc inv = sr_w(g) >> 22;                /* 1 / step, 24.24 */
   int32_t q[16], qf[16];
   static const int32_t none[2] = {0, 0};
   sr_positions(s, sr_w(0x23) - sr_shl(sr_dmul(g, p), 1), inv, sr_lo(inv >> 1), 16, q, qf);
   s->junk_out = sr_lerp(s, q, qf, 0x12, none, 1, up, io);
}

/* ---- ENS's chorus ------------------------------------------------------------ */

/* io: the SRR stage's left and right, changed in place (as Mono SWAVE's
 * ms_chorus, with the pan multipliers of PAN 64). */
static void sr_chorus(MonoSrrChorus *s, int32_t chrl, int32_t chrw, int32_t io[2][SR_FRAMES]) {
   if (s->count < SR_GATE) { s->count += 1; s->gate = 0; }
   else s->gate = SR_ONE;
   const int32_t sm = s->level;
   s->level = sr_sat(sr_mpy(chrl, 0x066666) + sr_mpy(sm, 0x79999a));
   const int32_t dry = sr_sat(sr_w(SR_ONE) - sr_w(sm));
   const int32_t across = sr_sat(sr_w(SR_ONE) + sr_mpy(chrw, -0x400000));   /* 1 - CHRW / 2 */
   const int32_t s1 = sr_sat(sr_mpy(SR_ONE, sm)), sw = sr_sat(sr_mpy(sm, across));
   const int32_t gr = sr_sat(sr_mpy(sr_pan[0], s->gate)), gl = sr_sat(sr_mpy(s->gate, sr_pan[1]));
   const int32_t c[4] = {sr_sat(sr_mpy(gl, s1)), sr_sat(sr_mpy(gr, sw)), sr_sat(sr_mpy(sw, gl)), sr_sat(sr_mpy(s1, gr))};
   /* the LFOs, stepped forward (forward Euler, saturating at 48 bits) */
   int32_t lfo[2][SR_FRAMES];
   sr_acc a = sr_w(s->lfo[0]), b = sr_w(s->lfo[1]);
   int32_t y1 = s->lfo[0];
   for (int i = 0; i < SR_FRAMES; ++i) {
      const int32_t y0 = sr_sat(b);
      lfo[1][i] = y0;
      b = sr_sat48(b - sr_mpy(0xef, y1));
      lfo[0][i] = y1 = sr_sat(a);
      a = sr_sat48(a + sr_mpy(y0, 0xef));
   }
   s->lfo[0] = sr_sat(a);
   s->lfo[1] = sr_sat(b);
   /* the two taps */
   int32_t tap[2][SR_FRAMES];
   const int base = s->wp - 400;
   for (int t = 0; t < 2; ++t)
      for (int i = 0; i < SR_FRAMES; ++i) {
         const sr_acc off = sr_mpy(lfo[t][i], 0x154);
         const int at = base + i + sr_hi(off);
         const uint32_t f = sr_lo(off);
         const int32_t d0 = s->delay[at & 1023], d1 = s->delay[(at + 1) & 1023];
         tap[t][i] = sr_sat(((sr_mpysu(d1, f) - sr_mpysu(d0, f)) >> 1) + sr_w(d0));
      }
   for (int i = 0; i < SR_FRAMES; ++i)
      s->delay[(s->wp + i) & 1023] = sr_sat(sr_mpy(0x400000, io[0][i]) + sr_mpy(0x400000, io[1][i]));
   s->wp = (s->wp + SR_FRAMES) & 1023;
   for (int i = 0; i < SR_FRAMES; ++i) {
      io[0][i] = sr_sat(sr_mpy(dry, io[0][i]) + sr_mpy(tap[0][i], c[0]) + sr_mpy(tap[1][i], c[2]));
      io[1][i] = sr_sat(sr_mpy(io[1][i], dry) + sr_mpy(tap[0][i], c[1]) + sr_mpy(tap[1][i], c[3]));
   }
}

/* ---- one block ---------------------------------------------------------------- */

/* ctl: the SRR, CHRL and CHRW words. io: 16 frames of left and right, 24-bit,
 * in place: SRR, then the chorus. */
static void sr_block(MonoSrrChorus *s, const int32_t ctl[SR_CONTROLS], int32_t io[2][SR_FRAMES]) {
   sr_srr(s, ctl[SR_SRR], io);
   sr_chorus(s, ctl[SR_CHRL], ctl[SR_CHRW], io);
}

#endif
