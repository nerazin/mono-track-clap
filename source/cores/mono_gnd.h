/* mono_gnd.h - Monomachine GND SIN and GND NOIS machines and the track's AMP
 * envelope, fixed-point core.
 *
 * A native C port of these parts of Monomachine OS 1.32B, written from its
 * DSP56300 code and checked sample for sample against that code running in
 * an emulator. Its tables are generated from formulas at start-up.
 *
 *  - SIN: one sine from the 8192-word sine table, read with linear
 *    interpolation, at a quarter of full scale. Its step follows the pitch,
 *    ramped across each block; its phase runs on, a note does not reset it.
 *  - NOIS: two noise words a sample (left and right), each the sum of the two
 *    before it (mod 2^24); a third-order pink filter and a leaky integrator
 *    (red) on them. RED fades from white (at a quarter of full scale) to pink
 *    at 64 and on to red; ST sets the width, STON (64 and up) the stereo:
 *    below 64 both sides get the left one's mix. The pitch is not used.
 * (GND GND itself writes zeros.)
 *
 * The AMP section is the kernel's: an attack, a hold (in tempo time), a
 * decay and a release, then VOL and PAN, ramped across each block.
 *
 * The arithmetic follows the DSP: 24-bit words, 56-bit accumulators,
 * truncation, rounding and saturation where the original has them. Controls
 * are the 24-bit block words the Monomachine's main CPU sends (knob value
 * << 16), and the pitch is the track's pitch index (2048 per octave; MIDI
 * note n is (n << 11) / 12 + 341 at TUNE 64 and 440 Hz once mg_pitch has
 * glided to it).
 *
 * C99 and libm (for the tables). SPDX-License-Identifier: GPL-3.0-only */
#ifndef MONO_GND_H
#define MONO_GND_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#define MG_FRAMES 16
#define MG_ONE 0x7fffff
#define MG_PITCH_MAX 0x5800
#define MG_ADDR 0x14a000   /* the sine table's address; SIN's phase holds it */

enum { MG_SIN, MG_NOIS, MG_MACHINES };
enum { MG_ATK, MG_HOLD, MG_DEC, MG_REL, MG_DIST, MG_VOL, MG_PAN, MG_PORT };   /* the AMP page's block words */

/* ---- DSP56300 arithmetic ------------------------------------------------- */

typedef int64_t mg_acc;           /* 56-bit accumulator, sign-extended; also 24.24 long words */

static inline mg_acc mg_s56(mg_acc v) { return (mg_acc)((uint64_t)v << 8) >> 8; }
static inline mg_acc mg_shl(mg_acc v, int n) { return mg_s56((mg_acc)((uint64_t)v << n)); }
static inline mg_acc mg_w(int32_t x) { return (mg_acc)x * 16777216; }
static inline int32_t mg_sx(uint32_t v) { return (int32_t)(v << 8) >> 8; }
static inline mg_acc mg_mpy(int32_t x, int32_t y) { return (mg_acc)x * y * 2; }
static inline mg_acc mg_mpysu(int32_t s, uint32_t u) { return (mg_acc)s * (mg_acc)(u & 0xffffff) * 2; }
static inline int32_t mg_hi(mg_acc a) { return (int32_t)((uint32_t)(a >> 24) << 8) >> 8; }   /* a1 as stored */
static inline uint32_t mg_lo(mg_acc a) { return (uint32_t)a & 0xffffff; }                    /* a0 */
static inline mg_acc mg_long(int32_t hi, uint32_t lo) { return mg_w(hi) | (lo & 0xffffff); }
static inline mg_acc mg_raw(mg_acc a) { return mg_long(mg_hi(a), mg_lo(a)); }                /* a1:a0 stored and reloaded */
static inline int32_t mg_sat(mg_acc a) {
   const mg_acc v = a >> 24;
   return v > MG_ONE ? MG_ONE : v < -MG_ONE - 1 ? -MG_ONE - 1 : (int32_t)v;
}
static inline mg_acc mg_sat48(mg_acc a) {
   const mg_acc hi = ((mg_acc)1 << 47) - 1;
   return a > hi ? hi : a < -hi - 1 ? -hi - 1 : a;
}
/* x times a 48-bit long word, the upper 48 bits (MPYSU then DMAC) */
static inline mg_acc mg_dmul(int32_t x, mg_acc y) { return (mg_mpysu(x, mg_lo(y)) >> 24) + mg_mpy(x, mg_hi(y)); }
/* RND: convergent rounding of a at a1 */
static inline mg_acc mg_rnd(mg_acc a) {
   const uint32_t lo = mg_lo(a);
   mg_acc r = a - lo;
   if (lo > 0x800000 || (lo == 0x800000 && (mg_hi(a) & 1))) r += mg_w(1);
   return r;
}

/* ---- tables --------------------------------------------------------------- */

static int32_t mg_sine[0x2000];   /* sin(k pi / 4096), the boot code's 48-bit recurrence */
static int32_t mg_exp[0x800];     /* 2^(22 + k/2048), the kernel's pitch-to-frequency table */
static int32_t mg_attack[128];    /* the AMP envelope's rise per block by ATK */
static int32_t mg_decay[129];     /* minus its fall per block by DEC and REL */
static int32_t mg_port[128];      /* the glide's speed by PORT, per tick */

/* Entries of mg_exp where the firmware's table is one LSB below the formula. */
static const uint32_t mg_exp_low[64] = {
   0x00000001, 0x00000000, 0x00000000, 0x00008040, 0x00000000, 0x00000000, 0x40012004, 0x00000080,
   0x80004000, 0x00000000, 0x28000408, 0x06100000, 0x00000000, 0x00000006, 0x00200000, 0x02003018,
   0x00000000, 0x9000460c, 0x00000090, 0x01410002, 0x01810480, 0x00004800, 0x82000500, 0x00868020,
   0x41080610, 0x4a142004, 0x18018004, 0x20002800, 0x30010000, 0x22062492, 0x00502844, 0x00042db1,
   0x00140804, 0x30440014, 0x40235104, 0x02002448, 0x24370104, 0x40506120, 0x88610a22, 0x801a00b0,
   0x0c0c209d, 0x0c21210c, 0x2a418020, 0xc1048884, 0xe974ba59, 0x11082820, 0x83416318, 0x5208260c,
   0x2c84208a, 0x42638174, 0x205e8410, 0x0228a101, 0x0828a040, 0xa1097d08, 0xd6c79050, 0xc429d423,
   0x444d8846, 0x220afd63, 0x24fd40d8, 0xa80880ac, 0x04983580, 0x0364f9ac, 0x1b6c0653, 0x980c94c7
};

static void mg_tables(void) {
   const int32_t c1 = 0x7ffffd, c0 = 0x885869;                 /* round(cos(pi/4096) x 2^47) */
   mg_acc prev = 0, cur = ((mg_acc)0x1921 << 24) | 0xfb2aed;   /* round(sin(pi/4096) x 2^47) */
   mg_sine[0] = 0;
   mg_sine[1] = 0x1921;
   for (int k = 2; k < 0x2000; ++k) {
      const int32_t s1 = (int32_t)(cur >> 24), s0 = (int32_t)(cur & 0xffffff);
      mg_acc a = (mg_acc)c0 * s0 * 2;
      a = (a >> 24) + (mg_acc)c1 * s0 * 2;
      a += (mg_acc)s1 * c0 * 2;
      a = (a >> 24) + (mg_acc)c1 * s1 * 2;
      const mg_acc next = mg_sat48(a * 2 - prev);
      mg_sine[k] = (int32_t)(next >> 24);
      prev = cur;
      cur = next;
   }
   for (int k = 0; k < 0x800; ++k)
      mg_exp[k] = (int32_t)floor(exp2(22 + k / 2048.0)) - (int32_t)(mg_exp_low[k >> 5] >> (k & 31) & 1);
   /* attack: 0.5 x e^(-c k) to six decimals, in 2^-23 */
   for (int k = 0; k < 128; ++k) {
      const int64_t micro = (int64_t)floor(5e5 * exp(-0.08354855518 * k) + 0.5);
      mg_attack[k] = (int32_t)((micro * 8388608 + 500000) / 1000000);
   }
   /* decay: 60 dB in 64 x e^(c k) blocks; 127 holds, and the word after the table is 1 */
   for (int k = 0; k < 127; ++k)
      mg_decay[k] = -(int32_t)floor(8388608.0 * pow(10.0, -3.0 / 64 * exp(-0.06730291836 * k)) + 0.5);
   mg_decay[127] = -MG_ONE - 1;
   mg_decay[128] = 1;
   /* portamento: 2^30 / (14400 x 2^(13k/128)); 0 is a negative mark, a glide of two blocks */
   mg_port[0] = -0x12345;
   for (int k = 1; k < 128; ++k) mg_port[k] = (int32_t)floor(1073741824.0 / 14400.0 / exp2(13.0 * k / 128.0) + 0.5);
}

/* ---- the kernel: pitch ------------------------------------------------------ */

/* The kernel's pitch glide, from the track's note word ((note << 11) / 12,
 * sent with every control frame) to the pitch index for each block, with
 * TUNE (the eighth SYN word: +-2 semitones), the pitch modulation pmod
 * (block word 30: 64 << 16 at rest, 352 units a step; the LFOs and KIT
 * ASSIGN controllers aimed at PTCH) and the master tune (word 38, 0 at
 * 440 Hz). The glide is linear, at a speed set by PORT (the AMP page's
 * eighth word) times the tick (block word 42, 24 x BPM), so its length is in
 * beats. With PORT at 0 a new note is reached in two blocks: 127/128 of the
 * way, then the rest. The kernel's second glide (words 34 and 28, the MULTI
 * TRIG transpose) is left out: with word 34 at 0 it adds nothing. (As in Mono
 * FM+.) */
typedef struct { mg_acc now; int32_t start, target; } MgPitch;

static void mg_pitch_reset(MgPitch *g, int32_t word) { g->now = mg_w(word); g->start = g->target = word; }

static int32_t mg_pitch(MgPitch *g, int32_t word, int32_t tune, int32_t port, int32_t tick, int32_t pmod, int32_t mtune) {
   if (word != g->target) {   /* a glide still under way restarts from where it is */
      const int32_t now = mg_hi(g->now);
      if ((word < now && g->target >= now) || (word >= now && g->target < now)) g->start = g->target;
      g->target = word;
   }
   const mg_acc r = mg_mpy(mg_port[(port >> 16) & 0x7f], tick);
   const int32_t rate = r < 0 ? 0x7f0000 : (int32_t)((uint32_t)(r >> 8) << 8) >> 8;   /* a0 after ASR 8, as a signed word */
   const mg_acc dist = mg_w(g->target) - mg_w(g->start);
   mg_acc a = g->now + mg_mpy(mg_sat(dist), rate);
   if (dist >= 0 ? a > mg_w(g->target) : a < mg_w(g->target)) { g->start = g->target; a = mg_w(g->target); }
   g->now = mg_raw(a);
   const mg_acc p = a + mg_w(mtune) + mg_mpy(0x2ab, tune) + mg_mpy(0xb000, pmod) - mg_w(0x5800);   /* TUNE: 341.5 at 64 */
   return p < 0 ? 0 : p >= mg_w(MG_PITCH_MAX) ? MG_PITCH_MAX : mg_hi(p);
}

/* The main CPU's pitch words (main OS of OS 1.32B). Word 38 for MASTER TUNE
 * in tenths of a hertz, 4000..4999: its table at 0x251210 is
 * trunc(2048 log2(f / 440)). */
static int32_t mg_master_tune(int tenths) { return (int32_t)(2048.0 * log2(tenths / 4400.0)); }

/* The PTCH page's DEST ranges in semitones: 1/12 2/12 7/12 1OCT 2OCT 4OCT 8OCT 16OC. */
static const int32_t mg_range[8] = {1, 2, 7, 12, 24, 48, 96, 192};

/* What an LFO on page PTCH adds to word 30 with its wave at the peak (+-128 in
 * the high word) and DPTH d (negative for the inverted wave): (d x 128 >> 2) x
 * the range's step, 2 per semitone (the main OS's table at 0x2590d4). */
static int32_t mg_lfo_pitch(int d, int range) { return d * 64 * mg_range[range & 7]; }

/* What a pitch bend adds to word 30 through KIT ASSIGN JOY R/L with MIRR on,
 * PAGE PTCH, DEST range (0..7: 1/12 2/12 7/12 1OCT 2OCT 4OCT 8OCT 16OC) and
 * ADD +63: the bend's top 8 bits v (2 msb + (lsb >> 6)) are JOY R at v - 128
 * and JOY L at 127 - v, whose ADD the mirror negates; value x ADD x the
 * range's step, which is 4 x the range in semitones (the main OS's steps at
 * 0x251b30: 4 8 28 48 96 192 384 768). */
static int32_t mg_bend(int v, int range) {
   return (v >= 128 ? v - 128 : v - 127) * 63 * 4 * mg_range[range & 7];
}

/* A controller's value added to its word as 0x010003ba does: clamped on the side it moves to. */
static int32_t mg_add_mod(int32_t word, int32_t v) {
   const int32_t w = word + v;
   return v > 0 && w > 0x7effff ? 0x7effff : v < 0 && w < 0 ? 0 : w;
}

/* The kernel's frequency word for a pitch index (register a on the machine's entry). */
static mg_acc mg_freq(int32_t pitch) {
   const int32_t x = mg_sat(mg_shl(mg_w(mg_exp[pitch & 0x7ff]) >> 10, pitch >> 11));
   return mg_sat48(mg_mpy(x, 0x1d22a) >> 3);
}

/* ---- the machines -------------------------------------------------------------- */

typedef struct {
   int machine;
   mg_acc phase, step;       /* SIN: the table address and its fraction (X/Y:(r6+$10)); last block's step (X/Y:(r6+$12)) */
   uint32_t noise[2];        /* NOIS: the generator's two words (Y:(r6+$10), Y:(r6+$11)) */
   int32_t in[6], pink[6];   /* NOIS: the pink filter's last three inputs and outputs, oldest first, left then right */
   int32_t red[2];           /* NOIS: the integrators, left and right (X/Y:(r6+$18)) */
} MonoGnd;

/* The machine's init routine (on assignment), from cleared memory. Neither
 * machine's trig routine does anything. */
static void mg_init(MonoGnd *s, int machine) {
   memset(s, 0, sizeof *s);
   s->machine = machine;
   s->phase = mg_w(MG_ADDR);
   s->noise[0] = 10;
   s->noise[1] = 1;
}

/* SIN (P:$144cd0): the step is the frequency word x 0.1858 (table words per
 * sample, 24.24), ramped from last block's in 16 equal parts. */
static void mg_sin(MonoGnd *s, mg_acc freq, int32_t out[2][MG_FRAMES]) {
   const mg_acc step = mg_dmul(0x17c6f9, freq), ramp = mg_raw((step - s->step) >> 4);
   mg_acc a = s->step, b = s->phase;
   s->step = mg_raw(step);
   for (int i = 0; i < MG_FRAMES; ++i) {
      const int k = mg_hi(b) & 0x1fff;
      const uint32_t f = mg_lo(b);
      const int32_t s0 = mg_sine[k], s1 = mg_sine[(k + 1) & 0x1fff];
      out[0][i] = out[1][i] = mg_sat((-mg_mpysu(s0, f) + mg_w(s0) + mg_w(s0) + mg_mpysu(s1, f)) >> 3);
      a += ramp;
      b += a;
      b = (mg_acc)((uint64_t)b & ~((uint64_t)0xffe000 << 24)) + mg_w(MG_ADDR);   /* AND $1fff on b1, then the base */
   }
   s->phase = mg_raw(b);
}

/* The pink filter's coefficients (X:$144e7a): inputs n-3..n, then outputs n-3..n-1; its sum is x 4. */
static const int32_t mg_pink_c[7] = {-0x008371, 0x05e4f1, -0x0b2de8, 0x05d05a, 0x10b5c7, -0x408d71, 0x4fd6ae};

/* NOIS (P:$144da2). syn: ST, RED, STON. */
static void mg_nois(MonoGnd *s, const int32_t syn[8], int32_t out[2][MG_FRAMES]) {
   enum { N = 2 * MG_FRAMES };
   int32_t x[6 + N], y[6 + N], red[N], mix[N];
   /* the white noise, left and right interleaved: (b, q) -> (b + q, b + 2q), both kept */
   uint32_t b = s->noise[0], q = s->noise[1];
   for (int i = 0; i < N; i += 2) {
      b = (b + q) & 0xffffff;
      q = (q + b) & 0xffffff;
      x[6 + i] = mg_sx(b);
      x[7 + i] = mg_sx(q);
   }
   s->noise[0] = q;   /* stored the other way round */
   s->noise[1] = b;
   /* pink: x 4, saturated */
   memcpy(x, s->in, sizeof s->in);
   memcpy(y, s->pink, sizeof s->pink);
   for (int i = 0; i < N; ++i) {
      mg_acc a = 0;
      for (int t = 0; t < 4; ++t) a += mg_mpy(mg_pink_c[t], x[i + 2 * t]);
      for (int t = 0; t < 3; ++t) a += mg_mpy(mg_pink_c[4 + t], y[i + 2 * t]);
      y[6 + i] = mg_sat(mg_shl(a, 2));
   }
   memcpy(s->in, x + N, sizeof s->in);
   memcpy(s->pink, y + N, sizeof s->pink);
   /* red: each sample's value before it adds the noise / 64 and leaks by 839 / 2^23 */
   for (int c = 0; c < 2; ++c) {
      mg_acc a = mg_w(s->red[c]);
      for (int i = c; i < N; i += 2) {
         const int32_t was = red[i] = mg_sat(a);
         a += mg_mpy(0x020000, x[6 + i]) + mg_mpy(-0x347, was);
      }
      s->red[c] = mg_sat(a);
   }
   /* RED: white x 1/4 to pink below 0.5, pink to red from there */
   const int32_t r = syn[1];
   int32_t gw = 0, gp, gr = 0;
   const mg_acc d = mg_w(MG_ONE) - mg_w(r) - mg_w(r);
   if (d >= 0) {
      gw = mg_sat(d >> 2);
      gp = mg_sat(mg_shl(mg_w(r), 1));
   } else {
      const mg_acc e = mg_shl(mg_w(r) - mg_w(0x400000), 1);
      gr = mg_sat(e);
      gp = mg_sat(mg_w(MG_ONE) - e);
   }
   for (int i = 0; i < N; ++i) mix[i] = mg_sat(mg_mpy(gw, x[6 + i]) + mg_mpy(gp, y[6 + i]) + mg_mpy(red[i], gr));
   /* ST and STON */
   const int32_t near = mg_sat(mg_w(0x400000) + mg_mpy(syn[0], 0x400000)), far = mg_sat(mg_w(0x400000) - mg_mpy(syn[0], 0x400000));
   for (int i = 0; i < MG_FRAMES; ++i) {
      const int32_t l = mix[2 * i], rr = mix[2 * i + 1];
      out[0][i] = mg_sat(mg_mpy(near, l) + mg_mpy(far, rr));
      out[1][i] = syn[2] >= 0x400000 ? mg_sat(mg_mpy(l, far) + mg_mpy(rr, near)) : out[0][i];
   }
}

/* One 16-frame block of the machine, left and right. syn: its eight SYN
 * block words; pitch: the pitch index (0..MG_PITCH_MAX). */
static void mg_block(MonoGnd *s, const int32_t syn[8], int32_t pitch, int32_t out[2][MG_FRAMES]) {
   if (s->machine == MG_SIN) mg_sin(s, mg_freq(pitch), out);
   else mg_nois(s, syn, out);
}

/* ---- the kernel: the AMP section ---------------------------------------------- */

typedef struct {
   int stage;            /* 0 attack, 1 hold, 2 decay, 3 release, 4 quick release */
   int32_t level, held;  /* the envelope, and blocks held */
   int32_t gain[2];      /* left and right at the end of the last block */
} MgAmp;

/* PAN's quarter sine: the right and left multipliers (Y:$0, Y:$1). */
static void mg_pan(int32_t pan_word, int32_t pan[2]) {
   const int p = mg_sat(mg_shl(mg_mpy(0x408e05, pan_word), 1)) >> 12;
   pan[0] = mg_sine[p & 0x1fff];
   pan[1] = mg_sine[(p + 0x801) & 0x1fff];
}

/* One block of the envelope and the gains. note: the low bits of the trig
 * word (1 note on, 2 note off, 3 stop, 0 nothing new); amp: the AMP page's
 * block words; tick_recip: block word 35 (0x800000 / (24 x BPM)). gain: each
 * frame's left and right gain. (As in Mono FM+.) */
static void mg_amp(MgAmp *e, int note, const int32_t amp[8], int32_t tick_recip, int32_t gain[2][MG_FRAMES]) {
   if (note == 1) e->stage = 0;
   else if (note >= 2) e->stage = note == 3 ? 4 : 3;
   if (e->stage == 0) {
      const mg_acc a = mg_w(e->level) + mg_w(mg_attack[(amp[MG_ATK] >> 16) & 127]);
      e->level = mg_sat(a);
      if (a > mg_w(MG_ONE) + 0xffffff) e->stage = e->held = 1;
   } else if (e->stage == 1) {
      const int32_t hold = amp[MG_HOLD] < 0x10000 ? 0 : amp[MG_HOLD];
      const mg_acc limit = mg_shl(mg_mpy(mg_sat(mg_mpy(tick_recip, hold)), 0x791fd0), 1), a = mg_w(e->held) + mg_w(1);
      if (a > limit) e->stage = 2;
      else e->held = mg_sat(a);
   } else {
      const int32_t knob = e->stage == 2 ? amp[MG_DEC] : e->stage == 3 ? amp[MG_REL] : 0x200000;
      const int k = mg_hi(mg_rnd((mg_w(knob) + mg_w(0x7fff)) >> 16));
      e->level = mg_sat(-mg_mpy(e->level, mg_decay[k < 0 ? 0 : k > 128 ? 128 : k]));
   }
   /* VOL squared, and PAN along a quarter sine */
   int32_t pan[2];
   mg_pan(amp[MG_PAN], pan);
   const int32_t g = mg_sat(mg_mpy(e->level, mg_sat(mg_mpy(amp[MG_VOL], amp[MG_VOL]))));
   const mg_acc to[2] = {mg_mpy(g, pan[1]), mg_mpy(pan[0], g)};
   for (int c = 0; c < 2; ++c) {
      const int32_t step = mg_sat(to[c] - mg_w(e->gain[c]));
      mg_acc a = mg_w(e->gain[c]);
      for (int i = 0; i < MG_FRAMES; ++i) {
         gain[c][i] = mg_sat(a);
         a += mg_mpy(step, 0x080000);
      }
      e->gain[c] = mg_sat(to[c]);
   }
}

/* The AMP section's product: x (a 48-bit word, the filter's output on the
 * unit) times the gain, x 16. */
static inline int32_t mg_apply(int32_t gain, mg_acc x) { return mg_sat(mg_shl(mg_dmul(gain, x), 4)); }

#endif
