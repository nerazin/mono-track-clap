/* mono_fm.h - Monomachine FM+ STAT, FM+ PAR and FM+ DYN machines and the
 * track's AMP envelope, fixed-point core.
 *
 * A native C port of these parts of Monomachine OS 1.32B, written from its
 * DSP56300 code and checked sample for sample against that code running in
 * an emulator. Its tables are generated from formulas at start-up.
 *
 * The machines run at twice the rate, 32 samples per 16-frame block, and
 * share their building blocks: sine operators read from an 8192-point
 * table (interpolated, their frequency ramped across the block; or not
 * interpolated, with self-feedback), each operator's output differentiated,
 * low-passed (by TONE, or fixed) and fed back into itself by the part of
 * its level knob above 64; the operators summed, with decaying envelopes,
 * into a modulator that bends the carrier's phase increment (FM); the
 * carrier then halved in rate by two stages of allpass pairs. The output is
 * mono.
 *  - FM+ STAT: operator 2 (2FRQ, 2VOL) and operator 1 (1FRQ, 1FIN, 1FB, its
 *    depth decaying with 1ENV above 64) modulate the carrier.
 *  - FM+ PAR: three operators (xFRQ, xENV) in parallel.
 *  - FM+ DYN: operator 1 with a pitch envelope (1FEN) and a level envelope
 *    (1VEN) around 1VOL, and operator 2 with self-feedback (2FB) and 2ENV.
 *
 * The AMP section is the kernel's: an attack, a hold (in tempo time), a
 * decay and a release, then VOL and PAN, ramped across each block.
 *
 * The arithmetic follows the DSP: 24-bit words, 56-bit accumulators,
 * truncation, rounding and saturation where the original has them. Blocks
 * are 16 frames at 44.1 kHz; controls are the 24-bit block words the
 * Monomachine's main CPU sends (knob value << 16), and the pitch is the
 * track's pitch index (2048 per octave; MIDI note n is (n << 11) / 12 + 341
 * at TUNE 64 and 440 Hz once mf_pitch has glided to it).
 *
 * C99 and libm (for the tables). SPDX-License-Identifier: GPL-3.0-only */
#ifndef MONO_FM_H
#define MONO_FM_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#define MF_FRAMES 16
#define MF_ONE 0x7fffff
#define MF_PITCH_MAX 0x5800
#define MF_ADDR 0x14a000   /* the sine table's address; the operators' phases hold it */

enum { MF_STAT, MF_PAR, MF_DYN, MF_MACHINES };
enum { MF_ATK, MF_HOLD, MF_DEC, MF_REL, MF_DIST, MF_VOL, MF_PAN, MF_PORT };   /* the AMP page's block words */

/* ---- DSP56300 arithmetic ------------------------------------------------- */

typedef int64_t mf_acc;           /* 56-bit accumulator, sign-extended; also 24.24 long words */

static inline mf_acc mf_s56(mf_acc v) { return (mf_acc)((uint64_t)v << 8) >> 8; }
static inline mf_acc mf_shl(mf_acc v, int n) { return mf_s56((mf_acc)((uint64_t)v << n)); }
static inline mf_acc mf_w(int32_t x) { return (mf_acc)x * 16777216; }
static inline mf_acc mf_mpy(int32_t x, int32_t y) { return (mf_acc)x * y * 2; }
static inline mf_acc mf_mpysu(int32_t s, uint32_t u) { return (mf_acc)s * (mf_acc)(u & 0xffffff) * 2; }
static inline int32_t mf_hi(mf_acc a) { return (int32_t)((uint32_t)(a >> 24) << 8) >> 8; }   /* a1 as stored */
static inline uint32_t mf_lo(mf_acc a) { return (uint32_t)a & 0xffffff; }                    /* a0 */
static inline mf_acc mf_long(int32_t hi, uint32_t lo) { return mf_w(hi) | (lo & 0xffffff); }
static inline mf_acc mf_raw(mf_acc a) { return mf_long(mf_hi(a), mf_lo(a)); }                /* a1:a0 stored and reloaded */
static inline int32_t mf_sat(mf_acc a) {
   const mf_acc v = a >> 24;
   return v > MF_ONE ? MF_ONE : v < -MF_ONE - 1 ? -MF_ONE - 1 : (int32_t)v;
}
static inline mf_acc mf_sat48(mf_acc a) {
   const mf_acc hi = ((mf_acc)1 << 47) - 1;
   return a > hi ? hi : a < -hi - 1 ? -hi - 1 : a;
}
/* x times a 48-bit long word, the upper 48 bits (MPYSU then DMAC SS) */
static inline mf_acc mf_dmul(int32_t x, mf_acc y) { return (mf_mpysu(x, mf_lo(y)) >> 24) + mf_mpy(x, mf_hi(y)); }
/* RND: convergent rounding of a at a1 */
static inline mf_acc mf_rnd(mf_acc a) {
   const uint32_t lo = mf_lo(a);
   mf_acc r = a - lo;
   if (lo > 0x800000 || (lo == 0x800000 && (mf_hi(a) & 1))) r += mf_w(1);
   return r;
}

/* ---- tables --------------------------------------------------------------- */

static int32_t mf_sine[0x2000];   /* sin(k pi / 4096), the boot code's 48-bit recurrence */
static int32_t mf_exp[0x800];     /* 2^(22 + k/2048), the kernel's pitch-to-frequency table */
static int32_t mf_tone[128];      /* the one-pole coefficient by TONE, 10 Hz x 2000^(k/128) */
static int32_t mf_ratio[24];      /* the operators' frequency ratios to the carrier by xFRQ, x 2^20 (1/64 .. 4) */
static int32_t mf_attack[128];    /* the AMP envelope's rise per block by ATK */
static int32_t mf_decay[129];     /* minus its fall per block by DEC and REL (and the machines' envelopes) */
static int32_t mf_port[128];      /* the glide's speed by PORT, per tick */

/* Entries of mf_exp where the firmware's table is one LSB below the formula. */
static const uint32_t mf_exp_low[64] = {
   0x00000001, 0x00000000, 0x00000000, 0x00008040, 0x00000000, 0x00000000, 0x40012004, 0x00000080,
   0x80004000, 0x00000000, 0x28000408, 0x06100000, 0x00000000, 0x00000006, 0x00200000, 0x02003018,
   0x00000000, 0x9000460c, 0x00000090, 0x01410002, 0x01810480, 0x00004800, 0x82000500, 0x00868020,
   0x41080610, 0x4a142004, 0x18018004, 0x20002800, 0x30010000, 0x22062492, 0x00502844, 0x00042db1,
   0x00140804, 0x30440014, 0x40235104, 0x02002448, 0x24370104, 0x40506120, 0x88610a22, 0x801a00b0,
   0x0c0c209d, 0x0c21210c, 0x2a418020, 0xc1048884, 0xe974ba59, 0x11082820, 0x83416318, 0x5208260c,
   0x2c84208a, 0x42638174, 0x205e8410, 0x0228a101, 0x0828a040, 0xa1097d08, 0xd6c79050, 0xc429d423,
   0x444d8846, 0x220afd63, 0x24fd40d8, 0xa80880ac, 0x04983580, 0x0364f9ac, 0x1b6c0653, 0x980c94c7
};

static void mf_tables(void) {
   const int32_t c1 = 0x7ffffd, c0 = 0x885869;                 /* round(cos(pi/4096) x 2^47) */
   mf_acc prev = 0, cur = ((mf_acc)0x1921 << 24) | 0xfb2aed;   /* round(sin(pi/4096) x 2^47) */
   mf_sine[0] = 0;
   mf_sine[1] = 0x1921;
   for (int k = 2; k < 0x2000; ++k) {
      const int32_t s1 = (int32_t)(cur >> 24), s0 = (int32_t)(cur & 0xffffff);
      mf_acc a = (mf_acc)c0 * s0 * 2;
      a = (a >> 24) + (mf_acc)c1 * s0 * 2;
      a += (mf_acc)s1 * c0 * 2;
      a = (a >> 24) + (mf_acc)c1 * s1 * 2;
      const mf_acc next = mf_sat48(a * 2 - prev);
      mf_sine[k] = (int32_t)(next >> 24);
      prev = cur;
      cur = next;
   }
   for (int k = 0; k < 0x800; ++k)
      mf_exp[k] = (int32_t)floor(exp2(22 + k / 2048.0)) - (int32_t)(mf_exp_low[k >> 5] >> (k & 31) & 1);
   const double m = sqrt(2.0) - 1.0, pi = 3.14159265358979323846;
   for (int k = 0; k < 128; ++k) {   /* the shared one-pole formula, as in Mono Reverb */
      const double c = 1.0 - cos(2.0 * pi * 10.0 * pow(2000.0, k / 128.0) / 44100.0);
      mf_tone[k] = (int32_t)floor((sqrt(c * c + 2.0 * c * m) - c) / m * 8388608.0 + 0.5);
   }
   mf_tone[50] += 1; mf_tone[70] += 1; mf_tone[99] -= 1;   /* as in the original */
   /* ratios 1/4, then 1/2 to 3 in halves, then 4 5 6 7 in each octave up to 64 */
   for (int k = 0; k < 24; ++k)
      mf_ratio[k] = k == 0 ? 0x4000 : k < 7 ? k << 15 : (4 + (k - 7) % 4) << (16 + (k - 7) / 4);
   /* attack: 0.5 x e^(-c k) to six decimals, in 2^-23 */
   for (int k = 0; k < 128; ++k) {
      const int64_t micro = (int64_t)floor(5e5 * exp(-0.08354855518 * k) + 0.5);
      mf_attack[k] = (int32_t)((micro * 8388608 + 500000) / 1000000);
   }
   /* decay: 60 dB in 64 x e^(c k) blocks; 127 holds, and the word after the table is 1 */
   for (int k = 0; k < 127; ++k)
      mf_decay[k] = -(int32_t)floor(8388608.0 * pow(10.0, -3.0 / 64 * exp(-0.06730291836 * k)) + 0.5);
   mf_decay[127] = -MF_ONE - 1;
   mf_decay[128] = 1;
   /* portamento: 2^30 / (14400 x 2^(13k/128)); 0 is a negative mark, a glide of two blocks */
   mf_port[0] = -0x12345;
   for (int k = 1; k < 128; ++k) mf_port[k] = (int32_t)floor(1073741824.0 / 14400.0 / exp2(13.0 * k / 128.0) + 0.5);
}

/* ---- the kernel: pitch ------------------------------------------------------ */

/* The kernel's pitch glide, from the track's note word ((note << 11) / 12,
 * sent with every control frame) to the pitch index for each block, with
 * TUNE (the eighth SYN word: +-2 semitones), the pitch modulation pmod
 * (block word 30: 64 << 16 at rest, 352 units a step; the LFOs and KIT
 * ASSIGN controllers aimed at PTCH) and the master tune (word 38, 0 at
 * 440 Hz). The glide is linear, at a speed set by PORT (the AMP page's
 * eighth word) times the tick (block word 42, 24 x BPM), so its length is in
 * beats: about 0.0036 x 2^(13 PORT/128) beats for the whole way. With PORT
 * at 0 a new note is reached in two blocks: 127/128 of the way, then the
 * rest. The kernel's second glide (words 34 and 28, the MULTI TRIG
 * transpose) is left out: with word 34 at 0 it adds nothing. */
typedef struct { mf_acc now; int32_t start, target; } MfPitch;

static void mf_pitch_reset(MfPitch *g, int32_t word) { g->now = mf_w(word); g->start = g->target = word; }

static int32_t mf_pitch(MfPitch *g, int32_t word, int32_t tune, int32_t port, int32_t tick, int32_t pmod, int32_t mtune) {
   if (word != g->target) {   /* a glide still under way restarts from where it is */
      const int32_t now = mf_hi(g->now);
      if ((word < now && g->target >= now) || (word >= now && g->target < now)) g->start = g->target;
      g->target = word;
   }
   const mf_acc r = mf_mpy(mf_port[(port >> 16) & 0x7f], tick);
   const int32_t rate = r < 0 ? 0x7f0000 : (int32_t)((uint32_t)(r >> 8) << 8) >> 8;   /* a0 after ASR 8, as a signed word */
   const mf_acc dist = mf_w(g->target) - mf_w(g->start);
   mf_acc a = g->now + mf_mpy(mf_sat(dist), rate);
   if (dist >= 0 ? a > mf_w(g->target) : a < mf_w(g->target)) { g->start = g->target; a = mf_w(g->target); }
   g->now = mf_raw(a);
   const mf_acc p = a + mf_w(mtune) + mf_mpy(0x2ab, tune) + mf_mpy(0xb000, pmod) - mf_w(0x5800);   /* TUNE: 341.5 at 64 */
   return p < 0 ? 0 : p >= mf_w(MF_PITCH_MAX) ? MF_PITCH_MAX : mf_hi(p);
}

/* The main CPU's pitch words (main OS of OS 1.32B). Word 38 for MASTER TUNE
 * in tenths of a hertz, 4000..4999: its table at 0x251210 is
 * trunc(2048 log2(f / 440)). */
static int32_t mf_master_tune(int tenths) { return (int32_t)(2048.0 * log2(tenths / 4400.0)); }

/* The PTCH page's DEST ranges in semitones: 1/12 2/12 7/12 1OCT 2OCT 4OCT 8OCT 16OC. */
static const int32_t mf_range[8] = {1, 2, 7, 12, 24, 48, 96, 192};

/* What an LFO on page PTCH adds to word 30 with its wave at the peak (+-128 in
 * the high word) and DPTH d (negative for the inverted wave): (d x 128 >> 2) x
 * the range's step, 2 per semitone (the main OS's table at 0x2590d4). */
static int32_t mf_lfo_pitch(int d, int range) { return d * 64 * mf_range[range & 7]; }

/* What a pitch bend adds to word 30 through KIT ASSIGN JOY R/L with MIRR on,
 * PAGE PTCH, DEST range (0..7: 1/12 2/12 7/12 1OCT 2OCT 4OCT 8OCT 16OC) and
 * ADD +63: the bend's top 8 bits v (2 msb + (lsb >> 6)) are JOY R at v - 128
 * and JOY L at 127 - v, whose ADD the mirror negates; value x ADD x the
 * range's step, which is 4 x the range in semitones (the main OS's steps at
 * 0x251b30: 4 8 28 48 96 192 384 768). */
static int32_t mf_bend(int v, int range) {
   return (v >= 128 ? v - 128 : v - 127) * 63 * 4 * mf_range[range & 7];
}

/* A controller's value added to its word as 0x010003ba does: clamped on the side it moves to. */
static int32_t mf_add_mod(int32_t word, int32_t v) {
   const int32_t w = word + v;
   return v > 0 && w > 0x7effff ? 0x7effff : v < 0 && w < 0 ? 0 : w;
}

/* The kernel's frequency word for a pitch index. */
static mf_acc mf_freq(int32_t pitch) {
   const int32_t x = mf_sat(mf_shl(mf_w(mf_exp[pitch & 0x7ff]) >> 10, pitch >> 11));
   return mf_mpy(x, 0x1d22a) >> 3;
}

/* ---- the machines' building blocks ---------------------------------------- */

#define MF_N (2 * MF_FRAMES)

typedef struct { mf_acc phase, inc; } MfOsc;    /* a ramped operator: its phase and last block's increment */
typedef struct { mf_acc phase, fb; } MfFbOsc;   /* a feedback operator: its phase and fed-back term */
typedef struct { int32_t xe, xo, v0, v1, ya, yb, yc, yd; } MfDecim;

static inline mf_acc mf_wrap(mf_acc p) { return mf_w((mf_hi(p) & 0x1fff) + MF_ADDR) | mf_lo(p); }

/* The phases of a block whose increment ramps from the last block's to inc
 * (each phase taken before its step); mod, if given, is added to each step
 * >> 12 (the carrier's FM). */
static void mf_phases(MfOsc *o, mf_acc inc, const int32_t *mod, mf_acc ph[MF_N]) {
   const mf_acc old = o->inc, delta = mf_raw((inc - old) >> 5);
   o->inc = mf_raw(inc);
   mf_acc a = old, b = o->phase;
   for (int i = 0; i < MF_N; ++i) {
      a += delta;
      ph[i] = b;
      b += a;
      if (mod) b += mf_w(mod[i]) >> 12;
      b = mf_wrap(b);
   }
   o->phase = b;
}

/* The sine at a phase, interpolated, x 2 >> shift. */
static inline int32_t mf_lookup(mf_acc p, int shift) {
   const int k = mf_hi(p) - MF_ADDR;
   const uint32_t fr = mf_lo(p);
   const int32_t a = mf_sine[k], b = mf_sine[(k + 1) & 0x1fff];
   return mf_sat((-mf_mpysu(a, fr) + mf_w(a) + mf_w(a) + mf_mpysu(b, fr)) >> shift);
}

static void mf_osc(MfOsc *o, mf_acc inc, int32_t x[MF_N]) {
   mf_acc ph[MF_N];
   mf_phases(o, inc, NULL, ph);
   for (int i = 0; i < MF_N; ++i) x[i] = mf_lookup(ph[i], 1);
}

/* An operator fed back into its own phase by amount (not interpolated). */
static void mf_fb_osc(MfFbOsc *o, mf_acc inc, int32_t amount, int32_t x[MF_N]) {
   mf_acc b = o->phase, a = o->fb;
   for (int i = 0; i < MF_N; ++i) {
      b = mf_wrap(b + inc + a);
      x[i] = mf_sine[mf_hi(b) - MF_ADDR];
      b -= a;
      a = mf_mpy(x[i], amount);
   }
   o->phase = mf_raw(b);
   o->fb = mf_raw(a);
}

static void mf_diff(int32_t *state, int32_t x[MF_N]) {
   int32_t prev = *state;
   for (int i = 0; i < MF_N; ++i) {
      const int32_t s = x[i];
      x[i] = mf_sat(mf_w(s) - mf_w(prev));
      prev = s;
   }
   *state = prev;
}

static void mf_lowpass(int32_t *state, int32_t c, int32_t x[MF_N]) {
   mf_acc a = mf_w(*state);
   for (int i = 0; i < MF_N; ++i) {
      const int32_t y = mf_sat(a);
      a += mf_mpy(c, x[i]) - mf_mpy(y, c);
      x[i] = y;
   }
   *state = mf_sat(a);
}

/* k (the part of a level knob above 64) feeds the operator's last sample back in, x 32. */
static void mf_feedback(int32_t *state, int32_t k, int32_t x[MF_N]) {
   int32_t prev = *state;
   for (int i = 0; i < MF_N; ++i) {
      const int32_t s = x[i];
      x[i] = mf_sat(mf_shl(mf_mpy(prev, k), 4) + mf_w(s));
      prev = s;
   }
   *state = prev;
}

static inline int32_t mf_ratio_of(int32_t knob) { return mf_ratio[mf_hi(mf_mpy(0x18, mf_sat(mf_w(knob) + mf_w(0x8000))))]; }
static inline int32_t mf_above(int32_t knob) { return knob >= 0x400000 ? knob - 0x400000 : 0; }
static inline int32_t mf_square4(int32_t v) { return mf_sat(mf_shl(mf_mpy(v, v), 2)); }
/* An envelope's fall per block: from a knob above 64, 1 - 8 (knob - 64)^4 */
static inline int32_t mf_decay_env(int32_t env, int32_t knob) {
   if (knob < 0x400000) return env;
   const int32_t e = knob - 0x400000, e2 = mf_sat(mf_mpy(e, e));
   return mf_sat(mf_mpy(mf_sat(mf_w(MF_ONE) - mf_mpy(e2, e2)), env));
}
/* FM+ DYN's envelope rate: the decay table from the knob's distance to 64 */
static inline int32_t mf_dyn_rate(int32_t knob) {
   mf_acc b = (mf_w(knob) - mf_w(0x400000)) >> 15;
   if (b < 0) b = -b;
   return mf_decay[mf_hi(mf_w(0x80) - b)];
}

/* Two stages of allpass pairs, then the two branches averaged: 32 samples to 16. */
static void mf_decimate(MfDecim *d, const int32_t c[MF_N], int32_t out[MF_FRAMES]) {
   int32_t v[MF_N + 2];
   v[0] = d->v0; v[1] = d->v1;
   int32_t ya = d->ya, xe = d->xe, xo = d->xo;
   mf_acc yb = mf_w(d->yb);
   for (int k = 0; k < MF_FRAMES; ++k) {
      const int32_t ybs = mf_sat(yb);
      ya = mf_sat(mf_w(xe) + mf_mpy(0x2bc9ca, c[2 * k]) - mf_mpy(ya, 0x2bc9ca));
      yb = mf_w(xo) + mf_mpy(0x0cfce3, c[2 * k + 1]) - mf_mpy(ybs, 0x0cfce3);
      xe = c[2 * k]; xo = c[2 * k + 1];
      v[2 + 2 * k] = ya; v[3 + 2 * k] = mf_sat(yb);
   }
   d->ya = ya; d->yb = mf_sat(yb); d->xe = xe; d->xo = xo;
   int32_t yc = d->yc;
   mf_acc yd = mf_w(d->yd);
   for (int k = 0; k < MF_FRAMES; ++k) {
      const int32_t yds = mf_sat(yd);
      yc = mf_sat(mf_w(v[2 * k]) + mf_mpy(0x6f0f12, v[2 + 2 * k]) - mf_mpy(yc, 0x6f0f12));
      yd = mf_w(v[1 + 2 * k]) + mf_mpy(0x4e63df, v[3 + 2 * k]) - mf_mpy(yds, 0x4e63df);
      out[k] = mf_sat((mf_mpy(0x400000, yc) + mf_mpy(0x400000, mf_sat(yd))) >> 1);
   }
   d->yc = yc; d->yd = mf_sat(yd); d->v0 = v[MF_N]; d->v1 = v[MF_N + 1];
}

/* ---- the machines ----------------------------------------------------------- */

/* The state, by role; the comments give each machine's word in its memory. */
typedef struct {
   int machine;
   MfOsc op[3];      /* STAT: 2FRQ's operator; PAR: the three; DYN: 1FRQ's */
   MfFbOsc fbo;      /* STAT: 1FRQ's operator; DYN: 2FRQ's */
   MfOsc car;        /* the carrier */
   int32_t diff[3], lp[3], fb[3];   /* each operator's difference, low-pass and level-feedback states */
   int32_t mod;      /* the modulator's low-pass */
   int32_t env[3];   /* STAT: 1ENV's; PAR: 1ENV 2ENV 3ENV; DYN: 2ENV's, 1VEN's and 1FEN's amounts */
   MfDecim dec;
} MonoFm;

/* The machine's init routine (on assignment). */
static void mf_init(MonoFm *f, int machine) {
   memset(f, 0, sizeof *f);
   f->machine = machine;
   for (int k = 0; k < 3; ++k) f->op[k].phase = mf_w(MF_ADDR);
   f->fbo.phase = f->car.phase = mf_w(MF_ADDR);
}

/* The machine's trig routine (on a note): the envelopes restart. */
static void mf_trig(MonoFm *f, const int32_t syn[8]) {
   if (f->machine != MF_DYN) { f->env[0] = f->env[1] = f->env[2] = MF_ONE; return; }
   f->env[0] = MF_ONE;
   const mf_acc a = mf_w(syn[3]) - mf_w(0x400000);   /* 1VEN, scaled by 1VOL below 64 */
   const int32_t x = mf_sat(mf_mpy(mf_sat(a), syn[2]));
   f->env[1] = mf_sat(mf_shl(a < 0 ? mf_w(x) : a, 2));
   mf_acc b = mf_w(syn[1]) - mf_w(0x400000);         /* 1FEN, its square with its sign, halved below 64 */
   if (b < 0) b >>= 1;
   const int32_t s = mf_sat(b);
   f->env[2] = mf_sat(mf_mpy(mf_sat(b < 0 ? -b : b), s));
}

static void mf_stat(MonoFm *f, const int32_t syn[8], mf_acc base, int32_t mod[MF_N]) {
   int32_t o2[MF_N], o1[MF_N];
   const int32_t tone = mf_tone[(syn[6] >> 16) & 127];
   mf_osc(&f->op[0], mf_shl(mf_dmul(mf_ratio_of(syn[4]), base), 3), o2);
   mf_diff(&f->diff[0], o2);
   mf_lowpass(&f->lp[0], tone, o2);
   mf_feedback(&f->fb[0], mf_above(syn[5]), o2);
   const int32_t fine = mf_sat(((mf_w(syn[1]) - mf_w(0x400000)) >> 2) + mf_w(0x400000));
   const int32_t ratio = mf_sat(mf_shl(mf_mpy(fine, mf_ratio_of(syn[0])), 1));
   mf_fb_osc(&f->fbo, mf_raw(mf_shl(mf_dmul(ratio, base), 3)), mf_sat(mf_w(syn[3]) >> 11), o1);
   mf_diff(&f->diff[1], o1);
   mf_lowpass(&f->lp[1], tone, o1);
   const int32_t g2 = mf_square4(syn[5]);
   f->env[0] = mf_decay_env(f->env[0], syn[2]);
   const int32_t g1 = mf_sat(mf_mpy(mf_square4(syn[2]), f->env[0]));
   for (int i = 0; i < MF_N; ++i) mod[i] = mf_sat(mf_mpy(g2, o2[i]) + mf_mpy(g1, o1[i]));
   mf_lowpass(&f->mod, tone, mod);
}

static void mf_par(MonoFm *f, const int32_t syn[8], mf_acc base, int32_t mod[MF_N]) {
   int32_t o[3][MF_N];
   const int32_t tone = mf_tone[(syn[6] >> 16) & 127];
   for (int k = 0; k < 3; ++k) {
      mf_osc(&f->op[k], mf_shl(mf_dmul(mf_ratio_of(syn[2 * k]), base), 3), o[k]);
      mf_diff(&f->diff[k], o[k]);
      mf_lowpass(&f->lp[k], tone, o[k]);
      mf_feedback(&f->fb[k], mf_above(syn[2 * k + 1]), o[k]);
   }
   int32_t g[3];
   for (int k = 0; k < 3; ++k) {
      f->env[k] = mf_decay_env(f->env[k], syn[2 * k + 1]);
      g[k] = mf_sat(mf_mpy(mf_square4(syn[2 * k + 1]), f->env[k]));
   }
   for (int i = 0; i < MF_N; ++i)
      mod[i] = mf_sat(mf_w(mf_sat(mf_mpy(g[0], o[0][i]) + mf_mpy(g[1], o[1][i]))) + mf_mpy(o[2][i], g[2]));
   mf_lowpass(&f->mod, tone, mod);
}

static void mf_dyn(MonoFm *f, const int32_t syn[8], mf_acc base, int32_t mod[MF_N]) {
   int32_t o1[MF_N], o2[MF_N];
   /* operator 1: its pitch envelope (1FEN) added to the frequency, x 1FRQ x 2 */
   const mf_acc pe = mf_mpy(-mf_dyn_rate(syn[1]), f->env[2]);
   f->env[2] = mf_sat(pe);
   const int32_t r1 = syn[0] > 0x7effff ? MF_ONE : syn[0];
   mf_osc(&f->op[0], mf_shl(mf_dmul(r1, mf_raw(base + (pe >> 12))), 1), o1);
   mf_diff(&f->diff[0], o1);
   mf_lowpass(&f->lp[0], 0x400000, o1);
   /* its level envelope (1VEN) around 1VOL */
   const mf_acc ve = mf_mpy(-mf_dyn_rate(syn[3]), f->env[1]);
   f->env[1] = mf_sat(ve);
   const mf_acc above = mf_w(syn[2]) + ve - mf_w(0x400000);
   mf_feedback(&f->fb[0], above >= 0 ? mf_sat(above) : 0, o1);
   /* operator 2: 2FRQ squared x 4, fed back by 2FB */
   const int32_t r2 = mf_sat(mf_mpy(syn[4] > 0x7effff ? MF_ONE : syn[4], syn[4] > 0x7effff ? MF_ONE : syn[4]));
   mf_fb_osc(&f->fbo, mf_raw(mf_shl(mf_dmul(r2, base), 2)), mf_sat(mf_w(syn[6]) >> 11), o2);
   mf_diff(&f->diff[1], o2);
   mf_lowpass(&f->lp[1], 0x400000, o2);
   const int32_t g1 = mf_sat(mf_shl(mf_mpy(syn[2], syn[2]) + mf_w(f->env[1]), 2));
   f->env[0] = mf_decay_env(f->env[0], syn[5]);
   const int32_t g2 = mf_sat(mf_mpy(mf_square4(syn[5]), f->env[0]));
   for (int i = 0; i < MF_N; ++i) mod[i] = mf_sat(mf_mpy(g1, o1[i]) + mf_mpy(g2, o2[i]));
   mf_lowpass(&f->mod, 0x400000, mod);
}

/* One 16-frame block of the machine. syn: its eight SYN block words; pitch:
 * the pitch index (0..MF_PITCH_MAX); trig: the block carries a note's trig. */
static void mf_block(MonoFm *f, const int32_t syn[8], int32_t pitch, int trig, int32_t out[MF_FRAMES]) {
   if (trig) mf_trig(f, syn);
   const mf_acc base = mf_sat48(mf_dmul(0x0be37c, mf_freq(pitch)));
   int32_t mod[MF_N], car[MF_N];
   mf_acc ph[MF_N];
   if (f->machine == MF_STAT) mf_stat(f, syn, base, mod);
   else if (f->machine == MF_PAR) mf_par(f, syn, base, mod);
   else mf_dyn(f, syn, base, mod);
   mf_phases(&f->car, base, mod, ph);
   for (int i = 0; i < MF_N; ++i) car[i] = mf_lookup(ph[i], 3);
   mf_decimate(&f->dec, car, out);
}

/* ---- the kernel: the AMP section ---------------------------------------------- */

typedef struct {
   int stage;            /* 0 attack, 1 hold, 2 decay, 3 release, 4 quick release */
   int32_t level, held;  /* the envelope, and blocks held */
   int32_t gain[2];      /* left and right at the end of the last block */
} MfAmp;

/* One block of the envelope and the gains. note: the low bits of the trig
 * word (1 note on, 2 note off, 3 stop, 0 nothing new); amp: the AMP page's
 * block words; tick_recip: block word 35 (0x800000 / (24 x BPM)). gain: each
 * frame's left and right gain. */
static void mf_amp(MfAmp *e, int note, const int32_t amp[8], int32_t tick_recip, int32_t gain[2][MF_FRAMES]) {
   if (note == 1) e->stage = 0;
   else if (note >= 2) e->stage = note == 3 ? 4 : 3;
   if (e->stage == 0) {
      const mf_acc a = mf_w(e->level) + mf_w(mf_attack[(amp[MF_ATK] >> 16) & 127]);
      e->level = mf_sat(a);
      if (a > mf_w(MF_ONE) + 0xffffff) e->stage = e->held = 1;
   } else if (e->stage == 1) {
      const int32_t hold = amp[MF_HOLD] < 0x10000 ? 0 : amp[MF_HOLD];
      const mf_acc limit = mf_shl(mf_mpy(mf_sat(mf_mpy(tick_recip, hold)), 0x791fd0), 1), a = mf_w(e->held) + mf_w(1);
      if (a > limit) e->stage = 2;
      else e->held = mf_sat(a);
   } else {
      const int32_t knob = e->stage == 2 ? amp[MF_DEC] : e->stage == 3 ? amp[MF_REL] : 0x200000;
      const int k = mf_hi(mf_rnd((mf_w(knob) + mf_w(0x7fff)) >> 16));
      e->level = mf_sat(-mf_mpy(e->level, mf_decay[k < 0 ? 0 : k > 128 ? 128 : k]));
   }
   /* VOL squared, and PAN along a quarter sine */
   const int p = mf_sat(mf_shl(mf_mpy(0x408e05, amp[MF_PAN]), 1)) >> 12;
   const int32_t g = mf_sat(mf_mpy(e->level, mf_sat(mf_mpy(amp[MF_VOL], amp[MF_VOL]))));
   const mf_acc to[2] = {mf_mpy(g, mf_sine[(p + 0x801) & 0x1fff]), mf_mpy(mf_sine[p & 0x1fff], g)};
   for (int c = 0; c < 2; ++c) {
      const int32_t step = mf_sat(to[c] - mf_w(e->gain[c]));
      mf_acc a = mf_w(e->gain[c]);
      for (int i = 0; i < MF_FRAMES; ++i) {
         gain[c][i] = mf_sat(a);
         a += mf_mpy(step, 0x080000);
      }
      e->gain[c] = mf_sat(to[c]);
   }
}

/* The AMP section's product: x (a 48-bit word, the filter's output on the
 * unit) times the gain, x 16. */
static inline int32_t mf_apply(int32_t gain, mf_acc x) { return mf_sat(mf_shl(mf_dmul(gain, x), 4)); }

#endif
