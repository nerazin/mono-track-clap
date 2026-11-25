/* mono_ringmod.h - Monomachine FX-RINGMOD machine, fixed-point core.
 *
 * A native C port of the RINGMOD machine of Monomachine OS 1.32B, written
 * from its DSP56300 code and checked sample for sample against that code
 * running in an emulator. Its tables are generated from formulas at
 * start-up, except the oscillator's band-limited impulse, which is copied
 * from the firmware (mq_impulse below).
 *
 * The carrier: a sine and a band-limited square at the track's pitch
 * (the square built from band-limited impulses, integrated), the square
 * shaped towards a triangle by WAVE above 64, crossfaded with the sine by
 * WAVE below 64, and with the external input by EXT.
 * The modulation, per channel: the input, through an 8-sample delay, times
 * the carrier (averaged over two samples); and the input through an
 * 8th-order allpass, times the other channel's carrier, through the allpass
 * again; then the two halves summed. MIX sets how much of the carrier and
 * how much of a plain 1 goes into both products.
 *
 * The arithmetic follows the DSP: 24-bit words, 56-bit accumulators,
 * truncation, rounding and saturation where the original has them. Blocks
 * are 16 stereo frames at 44.1 kHz; controls are the 24-bit block words the
 * Monomachine's main CPU sends (knob value << 16), and the pitch is the
 * track's pitch index (2048 per octave; MIDI note n is (n << 11) / 12 + 340
 * once mq_pitch has glided to it).
 *
 * This copy (Mono Track's) also keeps the words the block leaves in Y:$6d..$6f
 * and Y:$ad..$af, which the track's AMP stage reads as the low half of the
 * filter's output.
 *
 * C99 and libm (for the tables). SPDX-License-Identifier: GPL-3.0-only */
#ifndef MONO_RINGMOD_H
#define MONO_RINGMOD_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#define MQ_FRAMES 16
#define MQ_ONE 0x7fffff
#define MQ_PITCH_MAX 0x5800

enum { MQ_WAVE, MQ_EXT, MQ_MIX, MQ_INPUT, MQ_CONTROLS };   /* the machine's WAVE EXT MIX INP */

/* ---- DSP56300 arithmetic ------------------------------------------------- */

typedef int64_t mq_acc;           /* 56-bit accumulator, sign-extended; also 24.24 long words */

static inline mq_acc mq_s56(mq_acc v) { return (mq_acc)((uint64_t)v << 8) >> 8; }
static inline mq_acc mq_shl(mq_acc v, int n) { return mq_s56((mq_acc)((uint64_t)v << n)); }
static inline mq_acc mq_w(int32_t x) { return (mq_acc)x * 16777216; }
static inline mq_acc mq_mpy(int32_t x, int32_t y) { return (mq_acc)x * y * 2; }
static inline mq_acc mq_mpysu(int32_t s, uint32_t u) { return (mq_acc)s * (mq_acc)(u & 0xffffff) * 2; }
static inline int32_t mq_hi(mq_acc a) { return (int32_t)((uint32_t)(a >> 24) << 8) >> 8; }   /* a1 as stored */
static inline uint32_t mq_lo(mq_acc a) { return (uint32_t)a & 0xffffff; }                    /* a0 */
static inline mq_acc mq_long(int32_t hi, uint32_t lo) { return mq_w(hi) | (lo & 0xffffff); }
static inline mq_acc mq_raw(mq_acc a) { return mq_long(mq_hi(a), mq_lo(a)); }                /* a1:a0 stored and reloaded */
static inline int32_t mq_sat(mq_acc a) {
   const mq_acc v = a >> 24;
   return v > MQ_ONE ? MQ_ONE : v < -MQ_ONE - 1 ? -MQ_ONE - 1 : (int32_t)v;
}
static inline mq_acc mq_sat48(mq_acc a) {
   const mq_acc hi = ((mq_acc)1 << 47) - 1;
   return a > hi ? hi : a < -hi - 1 ? -hi - 1 : a;
}
/* x times a 48-bit long word, the upper 48 bits (MPYSU then DMAC SS) */
static inline mq_acc mq_dmul(int32_t x, mq_acc y) { return (mq_mpysu(x, mq_lo(y)) >> 24) + mq_mpy(x, mq_hi(y)); }
/* CLB: 9 minus the number of equal leading bits (0 for 0); NORMF by it */
static int mq_clb(mq_acc a) {
   if (!a) return 0;
   const uint64_t u = (uint64_t)a << 8, s = u >> 63;
   int n = 0;
   while (n < 56 && ((u >> (63 - n)) & 1) == s) ++n;
   return 9 - n;
}
static inline mq_acc mq_normf(mq_acc a, int s) { return s >= 0 ? a >> (s > 63 ? 63 : s) : mq_shl(a, -s); }
static int32_t mq_div(mq_acc a, int32_t s) {   /* 24 DIV steps from a clear carry: the quotient bits */
   uint64_t d = (uint64_t)a & 0xffffffffffffffULL;
   const uint64_t sv = (uint64_t)mq_w(s);
   unsigned carry = 0;
   for (int i = 0; i < 24; ++i) {
      const int add = (int)((d >> 55) & 1) != ((s >> 23) & 1);
      d = ((d << 1) | carry) & 0xffffffffffffffULL;
      d = ((add ? d + sv : d - sv) & 0xffffffff000000ULL) | (d & 0xffffff);
      carry = !((d >> 55) & 1);
   }
   return (int32_t)(d & 0xffffff);
}

/* ---- tables --------------------------------------------------------------- */

/* The oscillator's band-limited impulse: half of a windowed sinc at 32
 * points per sample, peak 0.5, copied from the firmware's kernel image
 * (Y:$131..$333), as no formula reproduces it. Index -1 is mq_impulse[0]. */
static const int32_t mq_impulse[515] = {
   0x000000, 0x000000, 0x000000, 0x000001, 0x000002, 0x000005, 0x000008, 0x00000b,
   0x000010, 0x000016, 0x00001c, 0x000024, 0x00002c, 0x000034, 0x00003d, 0x000047,
   0x000050, 0x000059, 0x000062, 0x00006a, 0x000072, 0x000078, 0x00007c, 0x00007f,
   0x000080, 0x00007f, 0x00007a, 0x000073, 0x000069, 0x00005c, 0x00004a, 0x000035,
   0x00001d, 0x000000, -0x000021, -0x000046, -0x00006e, -0x00009b, -0x0000cb, -0x0000ff,
   -0x000136, -0x000170, -0x0001ad, -0x0001ec, -0x00022c, -0x00026d, -0x0002af, -0x0002f1,
   -0x000332, -0x000371, -0x0003ae, -0x0003e7, -0x00041d, -0x00044e, -0x000479, -0x00049e,
   -0x0004bc, -0x0004d1, -0x0004dd, -0x0004e0, -0x0004d8, -0x0004c6, -0x0004a7, -0x00047d,
   -0x000445, -0x000401, -0x0003af, -0x000350, -0x0002e4, -0x00026a, -0x0001e2, -0x00014e,
   -0x0000ad, 0x000000, 0x0000b8, 0x00017b, 0x000248, 0x00031d, 0x0003f9, 0x0004db,
   0x0005c1, 0x0006aa, 0x000795, 0x00087f, 0x000966, 0x000a49, 0x000b26, 0x000bfa,
   0x000cc3, 0x000d80, 0x000e2d, 0x000eca, 0x000f54, 0x000fc9, 0x001026, 0x00106b,
   0x001095, 0x0010a3, 0x001092, 0x001063, 0x001013, 0x000fa2, 0x000f0e, 0x000e58,
   0x000d7e, 0x000c82, 0x000b63, 0x000a21, 0x0008bd, 0x000739, 0x000595, 0x0003d4,
   0x0001f7, 0x000000, -0x00020e, -0x000430, -0x000664, -0x0008a6, -0x000af3, -0x000d46,
   -0x000f9d, -0x0011f2, -0x001441, -0x001687, -0x0018be, -0x001ae2, -0x001cee, -0x001edd,
   -0x0020ac, -0x002255, -0x0023d3, -0x002523, -0x00263f, -0x002725, -0x0027d0, -0x00283c,
   -0x002867, -0x00284d, -0x0027ed, -0x002743, -0x00264e, -0x00250e, -0x002381, -0x0021a7,
   -0x001f80, -0x001d0e, -0x001a51, -0x00174d, -0x001402, -0x001076, -0x000cab, -0x0008a5,
   -0x00046b, 0x000000, 0x000494, 0x00094c, 0x000e21, 0x00130a, 0x001801, 0x001cfc,
   0x0021f4, 0x0026e0, 0x002bb7, 0x00306f, 0x0034ff, 0x00395f, 0x003d84, 0x004166,
   0x0044fb, 0x00483b, 0x004b1d, 0x004d98, 0x004fa7, 0x005140, 0x00525d, 0x0052f8,
   0x00530c, 0x005295, 0x00518d, 0x004ff3, 0x004dc4, 0x004b00, 0x0047a5, 0x0043b5,
   0x003f32, 0x003a1f, 0x00347f, 0x002e58, 0x0027b1, 0x002090, 0x0018fd, 0x001103,
   0x0008aa, 0x000000, -0x0008f1, -0x00121a, -0x001b6f, -0x0024e1, -0x002e60, -0x0037dd,
   -0x004147, -0x004a8f, -0x0053a3, -0x005c71, -0x0064eb, -0x006cfd, -0x007498, -0x007baa,
   -0x008225, -0x0087f8, -0x008d14, -0x00916c, -0x0094f3, -0x00979b, -0x00995b, -0x009a28,
   -0x0099fa, -0x0098ca, -0x009693, -0x009350, -0x008f00, -0x0089a2, -0x008337, -0x007bc2,
   -0x007349, -0x0069d1, -0x005f65, -0x00540f, -0x0047da, -0x003ad5, -0x002d11, -0x001e9f,
   -0x000f92, 0x000000, 0x001001, 0x00205a, 0x0030f1, 0x0041ad, 0x005272, 0x006324,
   0x0073a8, 0x0083df, 0x0093ad, 0x00a2f5, 0x00b199, 0x00bf7d, 0x00cc85, 0x00d893,
   0x00e38f, 0x00ed5d, 0x00f5e7, 0x00fd14, 0x0102d0, 0x010707, 0x0109a8, 0x010aa5,
   0x0109f0, 0x01077f, 0x01034b, 0x00fd50, 0x00f58a, 0x00ebfc, 0x00e0a9, 0x00d399,
   0x00c4d6, 0x00b46c, 0x00a26e, 0x008eee, 0x007a03, 0x0063c6, 0x004c53, 0x0033cb,
   0x001a4e, 0x000000, -0x001af8, -0x003671, -0x005242, -0x006e3e, -0x008a39, -0x00a604,
   -0x00c16f, -0x00dc4b, -0x00f668, -0x010f96, -0x0127a5, -0x013e68, -0x0153af, -0x01674e,
   -0x01791c, -0x0188ee, -0x01969e, -0x01a209, -0x01ab0e, -0x01b18e, -0x01b570, -0x01b69c,
   -0x01b501, -0x01b08f, -0x01a93d, -0x019f05, -0x0191e5, -0x0181e3, -0x016f06, -0x01595c,
   -0x0140f8, -0x0125f1, -0x010863, -0x00e871, -0x00c640, -0x00a1fa, -0x007bce, -0x0053f1,
   -0x002a98, 0x000000, 0x002b9a, 0x0057f3, 0x0084c9, 0x00b1d3, 0x00dec9, 0x010b62,
   0x013751, 0x01624b, 0x018c04, 0x01b42f, 0x01da82, 0x01feb2, 0x022079, 0x023f8f,
   0x025bb4, 0x0274a6, 0x028a2a, 0x029c08, 0x02aa0e, 0x02b40e, 0x02b9e0, 0x02bb62,
   0x02b877, 0x02b10a, 0x02a50e, 0x029479, 0x027f4d, 0x026591, 0x024754, 0x0224ad,
   0x01fdba, 0x01d2a1, 0x01a390, 0x0170be, 0x013a65, 0x0100ca, 0x00c438, 0x008500,
   0x004379, 0x000000, -0x004508, -0x008b39, -0x00d227, -0x011963, -0x01607b, -0x01a6fb,
   -0x01ec6d, -0x023059, -0x027248, -0x02b1c4, -0x02ee56, -0x03278c, -0x035cf4, -0x038e22,
   -0x03baab, -0x03e22e, -0x04044a, -0x0420a9, -0x0436fb, -0x0446f7, -0x04505d, -0x0452f6,
   -0x044e96, -0x044319, -0x043067, -0x041672, -0x03f539, -0x03ccc5, -0x039d2b, -0x03668d,
   -0x032918, -0x02e505, -0x029a99, -0x024a25, -0x01f406, -0x0198a4, -0x013870, -0x00d3e8,
   -0x006b93, 0x000000, 0x006e38, 0x00de73, 0x01500b, 0x01c252, 0x023495, 0x02a61d,
   0x03162d, 0x03840a, 0x03eef6, 0x045631, 0x04b900, 0x0516a6, 0x056e6d, 0x05bfa2,
   0x060997, 0x064ba6, 0x068532, 0x06b5a6, 0x06dc76, 0x06f925, 0x070b40, 0x071262,
   0x070e34, 0x06fe6f, 0x06e2db, 0x06bb52, 0x0687bf, 0x06481d, 0x05fc7d, 0x05a4ff,
   0x0541da, 0x04d354, 0x0459c9, 0x03d5a8, 0x034773, 0x02afbd, 0x020f2f, 0x016681,
   0x00b67e, 0x000000, -0x00bc0c, -0x017cad, -0x0240d9, -0x03077c, -0x03cf73, -0x049793,
   -0x055ea7, -0x062371, -0x06e4ae, -0x07a115, -0x08575b, -0x090632, -0x09ac4e, -0x0a4861,
   -0x0ad925, -0x0b5d57, -0x0bd3ba, -0x0c3b1c, -0x0c9252, -0x0cd841, -0x0d0bd9, -0x0d2c1a,
   -0x0d3816, -0x0d2eee, -0x0d0fdb, -0x0cda28, -0x0c8d38, -0x0c2885, -0x0baba2, -0x0b163a,
   -0x0a6814, -0x09a112, -0x08c130, -0x07c889, -0x06b752, -0x058ddd, -0x044c9a, -0x02f415,
   -0x0184f5, 0x000000, 0x0199eb, 0x0347d2, 0x05089e, 0x06db24, 0x08be1b, 0x0ab027,
   0x0cafd5, 0x0ebb9e, 0x10d1e9, 0x12f109, 0x151746, 0x1742d7, 0x1971e9, 0x1ba29e,
   0x1dd312, 0x20015a, 0x222b89, 0x244fae, 0x266bdc, 0x287e26, 0x2a84a8, 0x2c7d81,
   0x2e66dc, 0x303eef, 0x3203ff, 0x33b45d, 0x354e71, 0x36d0b1, 0x3839ac, 0x398807,
   0x3aba7f, 0x3bcfec, 0x3cc740, 0x3d9f8a, 0x3e57f9, 0x3eefd7, 0x3f6691, 0x3fbbb3,
   0x3feee9, 0x400000, 0x3feee9
};

static int32_t mq_sine[0x2000];   /* sin(k pi / 4096), the boot code's 48-bit recurrence */
static int32_t mq_exp[0x800];     /* 2^(22 + k/2048), the kernel's pitch-to-frequency table */
static int32_t mq_shape[256];     /* the triangle-shaping low-pass by pitch */
static int32_t mq_block_dc[1600]; /* the DC blocker by pitch */

/* Entries of mq_exp where the firmware's table is one LSB below the formula. */
static const uint32_t mq_exp_low[64] = {
   0x00000001, 0x00000000, 0x00000000, 0x00008040, 0x00000000, 0x00000000, 0x40012004, 0x00000080,
   0x80004000, 0x00000000, 0x28000408, 0x06100000, 0x00000000, 0x00000006, 0x00200000, 0x02003018,
   0x00000000, 0x9000460c, 0x00000090, 0x01410002, 0x01810480, 0x00004800, 0x82000500, 0x00868020,
   0x41080610, 0x4a142004, 0x18018004, 0x20002800, 0x30010000, 0x22062492, 0x00502844, 0x00042db1,
   0x00140804, 0x30440014, 0x40235104, 0x02002448, 0x24370104, 0x40506120, 0x88610a22, 0x801a00b0,
   0x0c0c209d, 0x0c21210c, 0x2a418020, 0xc1048884, 0xe974ba59, 0x11082820, 0x83416318, 0x5208260c,
   0x2c84208a, 0x42638174, 0x205e8410, 0x0228a101, 0x0828a040, 0xa1097d08, 0xd6c79050, 0xc429d423,
   0x444d8846, 0x220afd63, 0x24fd40d8, 0xa80880ac, 0x04983580, 0x0364f9ac, 0x1b6c0653, 0x980c94c7
};

static void mq_tables(void) {
   const int32_t c1 = 0x7ffffd, c0 = 0x885869;                 /* round(cos(pi/4096) x 2^47) */
   mq_acc prev = 0, cur = ((mq_acc)0x1921 << 24) | 0xfb2aed;   /* round(sin(pi/4096) x 2^47) */
   mq_sine[0] = 0;
   mq_sine[1] = 0x1921;
   for (int k = 2; k < 0x2000; ++k) {
      const int32_t s1 = (int32_t)(cur >> 24), s0 = (int32_t)(cur & 0xffffff);
      mq_acc a = (mq_acc)c0 * s0 * 2;
      a = (a >> 24) + (mq_acc)c1 * s0 * 2;
      a += (mq_acc)s1 * c0 * 2;
      a = (a >> 24) + (mq_acc)c1 * s1 * 2;
      const mq_acc next = mq_sat48(a * 2 - prev);
      mq_sine[k] = (int32_t)(next >> 24);
      prev = cur;
      cur = next;
   }
   for (int k = 0; k < 0x800; ++k)
      mq_exp[k] = (int32_t)floor(exp2(22 + k / 2048.0)) - (int32_t)(mq_exp_low[k >> 5] >> (k & 31) & 1);
   const double pi = 3.14159265358979323846, m = sqrt(2.0) - 1.0;
   for (int k = 0; k < 256; ++k)   /* 1 - e^(-2 pi f / 44100), f from 4 Hz over 12.5 octaves */
      mq_shape[k] = (int32_t)lround((1 - exp(-2 * pi * 4.003922 * pow(2, 12.5 * k / 255) / 44100)) * 8388608);
   static const short high[] = {191, 193, 210, 225, 232};   /* one LSB higher in the original */
   for (unsigned i = 0; i < sizeof high / sizeof high[0]; ++i) mq_shape[high[i]] += 1;
   for (int k = 0; k < 1600; ++k) {   /* the track filters' one-pole formula at A4 - 83 semitones x 2^(k/128) */
      const double c = 1 - cos(2 * pi * 440 * pow(2, -83 / 12.0 + k / 128.0) / 44100);
      mq_block_dc[k] = (int32_t)lround((sqrt(c * c + 2 * c * m) - c) / m * 8388608);
   }
}

/* The kernel's pitch glide, from the track's note word ((note << 11) / 12,
 * sent with every control frame) to the pitch index for each block. With the
 * portamento at 0, as on an FX track, a new note is reached in two blocks:
 * 127/128 of the way, then the rest. */
typedef struct { mq_acc now; int32_t start, target; } MqPitch;

/* The machine's assignment starts the glide at the note it finds. */
static void mq_pitch_reset(MqPitch *g, int32_t word) { g->now = mq_w(word); g->start = g->target = word; }

static int32_t mq_pitch(MqPitch *g, int32_t word) {
   if (word != g->target) {   /* a glide still under way restarts from where it is */
      const int32_t now = mq_hi(g->now);
      if ((word < now && g->target >= now) || (word >= now && g->target < now)) g->start = g->target;
      g->target = word;
   }
   const mq_acc dist = mq_w(g->target) - mq_w(g->start);
   mq_acc a = g->now + mq_mpy(mq_sat(dist), 0x7f0000);
   if (dist >= 0 ? a > mq_w(g->target) : a < mq_w(g->target)) { g->start = g->target; a = mq_w(g->target); }
   g->now = mq_raw(a);
   const mq_acc p = a + mq_w(-1) + mq_mpy(0x2ab, 0x400000);   /* master tune at 440 Hz, + 341.5 */
   return p < 0 ? 0 : p >= mq_w(MQ_PITCH_MAX) ? MQ_PITCH_MAX : mq_hi(p);
}

/* The kernel's frequency word for a pitch index. */
static mq_acc mq_freq(int32_t pitch) {
   const int32_t x = mq_sat(mq_shl(mq_w(mq_exp[pitch & 0x7ff]) >> 10, pitch >> 11));
   return mq_mpy(x, 0x1d22a) >> 3;
}

/* ---- the machine ---------------------------------------------------------- */

#define MQ_SECTIONS 4
static const int32_t mq_ap[MQ_SECTIONS][2] = {   /* 2nd-order allpass: y = cb x + ca x1 + x2 - ca y1 - cb y2 */
   {0x349cf2, -0x2ce02a}, {0x568ef0, 0x1dafa8}, {0x0075b6, 0x188629}, {-0x4c66eb, 0x170fca},
};

typedef struct {
   /* oscillator */
   mq_acc edge;                  /* samples to the square's next edge (24.24) */
   int half, sign;               /* which half period and which edge come next */
   int slot;                     /* the impulse ring's current 16-sample slot */
   int32_t ring[64];             /* band-limited impulses still to be integrated */
   mq_acc inc, freq;             /* the sine's step and the frequency word, last block */
   mq_acc phase;                 /* the sine's position: table address $14a000 + index, and fraction */
   int32_t square, shaped;       /* the integrator and the shaping low-pass */
   int32_t shape_coef;
   int dc_index;
   int32_t dc, dc_in;            /* the DC blocker */
   int32_t carrier[2];           /* the last carrier samples, left and right */
   /* modulator */
   int32_t ap[2][2][MQ_SECTIONS][4];   /* [pass][channel][section]: x[n-2], x[n-1], y[n-2], y[n-1] */
   int32_t delay8[2][8], delay7[2][7]; /* the previous block's last samples */
   int32_t y6d[3], yad[3];             /* Y:$6d..$6f and Y:$ad..$af after the block */
} MonoRingmod;

/* The machine's init routine (but for the sine position, which the firmware
 * leaves at 1, an address outside the table, for the first block). */
static void mq_reset(MonoRingmod *q) {
   memset(q, 0, sizeof *q);
   q->slot = 3;
   q->phase = mq_w(0x14a000);
}

static void mq_allpass(int32_t s[MQ_SECTIONS][4], int32_t x[MQ_FRAMES]) {
   for (int j = 0; j < MQ_SECTIONS; ++j) {
      const int32_t ca = mq_ap[j][0], cb = mq_ap[j][1];
      int32_t *m = s[j];
      for (int i = 0; i < MQ_FRAMES; ++i) {
         const int32_t y = mq_sat(mq_w(m[0]) + mq_mpy(m[1], ca) - mq_mpy(m[3], ca) + mq_mpy(x[i], cb) - mq_mpy(m[2], cb));
         m[0] = m[1]; m[1] = x[i];
         m[2] = m[3]; m[3] = y;
         x[i] = y;
      }
   }
}

/* One 16-frame block. ctl: the block words of WAVE EXT MIX INP; pitch: the
 * pitch index (0..MQ_PITCH_MAX); ext: the external carrier input. */
static void mq_block(MonoRingmod *q, const int32_t ctl[MQ_CONTROLS], int32_t pitch,
                     const int32_t in[2][MQ_FRAMES], const int32_t ext[2][MQ_FRAMES], int32_t out[2][MQ_FRAMES]) {
   int32_t sine[MQ_FRAMES], sq[MQ_FRAMES + 1], shape[MQ_FRAMES], wave[MQ_FRAMES], car[2][MQ_FRAMES];
   int32_t dry[2][MQ_FRAMES], ap[2][MQ_FRAMES], p1[2][MQ_FRAMES];
   const mq_acc freq = mq_sat48(mq_freq(pitch));

   /* the period, 2822400 / frequency, normalised in and out of a 24-bit division */
   mq_acc f = freq >= mq_w(0x4074) ? mq_w(0x4074) : freq;
   const int t = mq_clb(f);
   const int32_t quot = mq_div(mq_w(0x2b1100), mq_sat(mq_normf(f, t)));
   const mq_acc period = mq_sat48(mq_normf(mq_w(quot), t + 0x1d));

   /* sine: this block from the last block's position and step; the next position from the square's phase */
   const mq_acc e9 = mq_shl(q->edge, 9), old_inc = q->inc;
   q->inc = mq_raw(mq_dmul(0x17c6f9, freq));
   mq_acc nb = mq_shl(mq_mpy(mq_hi(e9), mq_hi(mq_shl(old_inc, 10))), 4);
   if (q->sign == 0) nb += mq_w(0x1000);
   nb -= mq_w(0x800);
   nb = (nb & ~((mq_acc)0xffffff << 24)) | ((mq_acc)(((nb >> 24) & 0xffffff) & 0x1fff) << 24);   /* AND on b1 */
   nb = mq_s56(-(nb - mq_w(0x1fff))) + mq_w(0x14a000);
   mq_acc p = q->phase;
   q->phase = mq_raw(nb);
   const mq_acc step = mq_sat48(old_inc);
   for (int i = 0; i < MQ_FRAMES; ++i) {
      p += step;
      p = (mq_w(((mq_hi(p) & 0x1fff) + 0x14a000)) | mq_lo(p));
      const int k = mq_hi(p) - 0x14a000;
      const uint32_t fr = mq_lo(p);
      const int32_t a = mq_sine[k], b = mq_sine[(k + 1) & 0x1fff];
      sine[i] = mq_sat((-mq_mpysu(a, fr) + mq_w(a) + mq_w(a) + mq_mpysu(b, fr)) >> 1);
   }

   /* the square's phase, rescaled when the frequency changes */
   const mq_acc old_freq = q->freq;
   q->freq = freq;
   mq_acc r = mq_dmul(mq_sat(mq_shl(old_freq, 8)), period);
   const int t2 = mq_clb(r);
   r = mq_normf(r, t2);
   r = mq_dmul(mq_sat(r), q->edge);
   r = mq_normf(mq_dmul(0x5f1be3, mq_raw(r)), -t2 - 0x17);
   q->edge = mq_raw(r);

   /* edges in this block: half periods alternate; each edge gets a band-limited impulse */
   const mq_acc half = mq_sat48(period >> 1);
   int ev_time[64], ev_phase[64];
   uint32_t ev_frac[64];
   int events = 0;
   mq_acc e = q->edge;
   while (e <= mq_w(16) && events < 64) {
      const mq_acc a = mq_mpysu(16, mq_lo(e));
      ev_time[events] = mq_hi(e);
      ev_phase[events] = mq_hi(a);
      ev_frac[events++] = mq_lo(a);
      e += half;
      q->half ^= 1;
   }
   q->edge = mq_raw(e - mq_w(16));
   const int old_slot = q->slot;
   q->slot = (q->slot + 1) & 3;
   for (int i = 0; i < MQ_FRAMES; ++i) q->ring[(old_slot * 16 + i) & 63] = 0;
   const int base = q->slot * 16;
   for (int n = 0; n < events; ++n) {
      const int32_t h = q->sign ? -0x5ae148 : 0x5ae148;
      q->sign ^= 1;
      const mq_acc hb = mq_mpysu(h, ev_frac[n]) >> 1;
      const int32_t wf = mq_sat(hb), wn = mq_sat(-hb + mq_w(h));
      int r0 = (base + ev_time[n] - 1) & 63, r2 = (base + ev_time[n] + 16) & 63;
      int i4 = 33 - ev_phase[n], i5 = 481 + ev_phase[n];   /* mq_impulse index = table index + 1 */
      mq_acc a = mq_w(q->ring[r2]), b = mq_w(q->ring[r0]);
      for (int k = 0; k < MQ_FRAMES; ++k) {
         a += mq_mpy(mq_impulse[i5], wf) + mq_mpy(mq_impulse[i5 - 1], wn);
         i5 -= 32;
         q->ring[r0] = mq_sat(b);
         r0 = (r0 + 1) & 63;
         b = mq_w(q->ring[r0]) + mq_mpy(mq_impulse[i4], wf) + mq_mpy(mq_impulse[i4 + 1], wn);
         i4 += 32;
         q->ring[r2] = mq_sat(a);
         r2 = (r2 + 1) & 63;
         a = mq_w(q->ring[r2]);
      }
      q->ring[r0] = mq_sat(b);
   }

   /* integrate the impulses into a square (with a slight leak) */
   mq_acc a = mq_w(q->square);
   for (int i = 0; i < MQ_FRAMES; ++i) {
      sq[i] = mq_sat(a);
      a += mq_mpy(0x2ccccd, q->ring[(base + i) & 63]) - mq_mpy(0x54, sq[i]);
   }
   sq[MQ_FRAMES] = q->square = mq_sat(a);

   /* WAVE above 64: towards a triangle, through a low-pass that follows the pitch */
   const int32_t c_old = q->shape_coef;
   {
      mq_acc ix = mq_mpy(pitch, 0x15555) - mq_w(2);
      const int idx = ix < 0 ? 0 : ix >= mq_w(255) ? 255 : mq_hi(ix);
      q->shape_coef = mq_shape[idx];
   }
   mq_acc wb = mq_w(ctl[MQ_WAVE]) - mq_w(0x400000);
   const int32_t w2 = mq_sat((wb < 0 ? 0 : wb) * 2), keep = mq_sat(mq_w(MQ_ONE) - mq_w(w2)), tri = mq_sat(mq_mpy(w2, 0x19999a));
   a = mq_w(q->shaped);
   for (int i = 0; i < MQ_FRAMES; ++i) {
      const int32_t prev = mq_sat(a);
      a += mq_mpy(c_old, sq[i]) - mq_mpy(c_old, prev);
      shape[i] = mq_sat(mq_mpy(keep, prev) + mq_mpy(sq[i + 1], tri));
   }
   q->shaped = mq_sat(a);

   /* DC blocker, its corner following the pitch, then x 16 */
   const int32_t kd = mq_block_dc[q->dc_index];
   {
      mq_acc ix = mq_mpy(pitch, 0x80000) + mq_w(-128);
      q->dc_index = ix < 0 ? 0 : ix > mq_w(0x63f) ? 0x63f : mq_hi(ix);
   }
   const int32_t g = mq_sat((mq_w(MQ_ONE) - mq_w(kd) + mq_w(MQ_ONE)) >> 1);
   a = mq_w(q->dc);
   int32_t xp = q->dc_in;
   for (int i = 0; i < MQ_FRAMES; ++i) {
      const int32_t prev = mq_sat(a);
      wave[i] = mq_sat(mq_shl(a, 4));
      a += -mq_mpy(xp, g) + mq_mpy(shape[i], g) - mq_mpy(kd, prev);
      xp = shape[i];
   }
   q->dc = mq_sat(a);
   q->dc_in = xp;

   /* the carrier: sine and wave by WAVE (below 64), the external input by EXT */
   const int32_t w1 = mq_sat(mq_w(ctl[MQ_WAVE]) * 2), own = mq_sat(mq_w(0x7f0000) - mq_w(ctl[MQ_EXT]));
   const int32_t ws = mq_sat(mq_mpy(own, mq_sat(mq_w(0x7f0000) - mq_w(w1)))), ww = mq_sat(mq_mpy(own, w1));
   for (int i = 0; i < MQ_FRAMES; ++i) {
      const mq_acc c = mq_mpy(sine[i], ws) + mq_mpy(wave[i], ww);
      for (int ch = 0; ch < 2; ++ch) car[ch][i] = mq_sat(c + mq_mpy(ext[ch][i], ctl[MQ_EXT]));
   }

   /* the input, the allpass branch and the 8-sample delay branch */
   const int32_t gin = mq_sat(mq_mpy(ctl[MQ_INPUT], ctl[MQ_INPUT]));
   for (int ch = 0; ch < 2; ++ch)
      for (int i = 0; i < MQ_FRAMES; ++i) ap[ch][i] = dry[ch][i] = mq_sat(mq_mpy(in[ch][i], gin) * 4);
   mq_allpass(q->ap[0][1], ap[1]);   /* the right channel is the firmware's first */
   mq_allpass(q->ap[0][0], ap[0]);
   const int32_t mix = ctl[MQ_MIX], half_mix = mq_sat(mq_w(mix) >> 1);
   mq_acc gm = mq_w(0x7f0000) - mq_w(mix);
   const int32_t plain = mq_sat(gm < 0 ? 0 : gm);
   for (int ch = 0; ch < 2; ++ch) {
      int32_t last = q->carrier[ch];
      for (int i = 0; i < MQ_FRAMES; ++i) {
         const int32_t d8 = i < 8 ? q->delay8[ch][i] : dry[ch][i - 8];
         const int32_t avg = mq_sat(mq_mpy(last, half_mix) + mq_mpy(car[ch][i], half_mix));
         p1[ch][i] = mq_sat(mq_mpy(d8, avg) + mq_mpy(plain, d8));
         last = car[ch][i];
      }
      q->carrier[ch] = last;
      for (int i = 0; i < 8; ++i) q->delay8[ch][i] = dry[ch][8 + i];
   }
   for (int ch = 0; ch < 2; ++ch)   /* each channel's allpass branch times the other channel's carrier */
      for (int i = 0; i < MQ_FRAMES; ++i) {
         const int32_t m = mq_sat(mq_mpy(mix, car[!ch][i]));
         ap[ch][i] = mq_sat(mq_mpy(ap[ch][i], m) + mq_mpy(plain, ap[ch][i]));
      }
   mq_allpass(q->ap[1][1], ap[1]);
   mq_allpass(q->ap[1][0], ap[0]);
   q->y6d[0] = p1[1][4]; q->y6d[1] = p1[0][5]; q->y6d[2] = p1[1][5];   /* the delay branch, interleaved */
   memcpy(q->yad, &ap[0][3], sizeof q->yad);

   /* out: half the other channel's allpass branch and half this channel's delay branch, 7 samples later */
   for (int ch = 0; ch < 2; ++ch) {
      for (int i = 0; i < MQ_FRAMES; ++i) {
         const int32_t d7 = i < 7 ? q->delay7[ch][i] : p1[ch][i - 7];
         out[ch][i] = mq_sat(mq_mpy(0x400000, ap[!ch][i]) + mq_mpy(0x400000, d7));
      }
      for (int i = 0; i < 7; ++i) q->delay7[ch][i] = p1[ch][9 + i];
   }
}

#endif
