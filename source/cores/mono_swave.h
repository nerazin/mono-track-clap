/* mono_swave.h - Monomachine SWAVE SAW, SWAVE PULS and SWAVE ENS machines,
 * the track's AMP envelope and ENS's chorus, fixed-point core.
 *
 * A native C port of these parts of Monomachine OS 1.32B, written from its
 * DSP56300 code and checked sample for sample against that code running in
 * an emulator. Its tables are generated from formulas at start-up, except
 * the oscillators' band-limited impulse, which is copied from the firmware
 * (ms_impulse below; the same table Mono Ringmod ships).
 *
 * The three machines share the kernel's oscillator: each edge of a wave is a
 * band-limited impulse (from a 32-phase table), written into a ring of 64
 * samples; a leaky integrator turns the impulses into the wave, and a DC
 * blocker follows. The output is mono, 16 frames per block at 44.1 kHz.
 *  - SAW: five saws: the main one (its every other impulse raised by SUBX, a
 *    saw an octave down), two detuned up and down by UNIW at UNIL, two
 *    detuned by half as much at UNIL x UNIX; and two sines, one and two
 *    octaves down, at SUB1 and SUB2.
 *  - PULS: three pulses (high and low impulses alternating), the width PW
 *    swept by PWAD (and restarted on a note when PWRS is above 64); two of
 *    them detuned by UNIW at UNIL; and the two sines.
 *  - ENS: four oscillators an octave down, three of them at the intervals
 *    PCH2..PCH4 (muted at unison), shaped from saw to pulse by WAVE and PW;
 *    and a chorus that runs after the AMP section (CHRL its level, CHRW its
 *    width).
 *
 * The AMP section is the kernel's: an attack, a hold (in tempo time), a
 * decay and a release, then VOL and PAN, ramped across each block.
 *
 * The arithmetic follows the DSP: 24-bit words, 56-bit accumulators,
 * truncation, rounding and saturation where the original has them. Controls
 * are the 24-bit block words the Monomachine's main CPU sends (knob value
 * << 16), and the pitch is the track's pitch index (2048 per octave; MIDI
 * note n is (n << 11) / 12 + 341 at TUNE 64 and 440 Hz once ms_pitch has
 * glided to it).
 *
 * C99 and libm (for the tables). SPDX-License-Identifier: GPL-3.0-only */
#ifndef MONO_SWAVE_H
#define MONO_SWAVE_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#define MS_FRAMES 16
#define MS_ONE 0x7fffff
#define MS_PITCH_MAX 0x5800
#define MS_ADDR 0x14a000   /* the sine table's address; the sub-oscillators' phases hold it */

enum { MS_SAW, MS_PULS, MS_ENS, MS_MACHINES };
enum { MS_ATK, MS_HOLD, MS_DEC, MS_REL, MS_DIST, MS_VOL, MS_PAN, MS_PORT };   /* the AMP page's block words */

/* ---- DSP56300 arithmetic ------------------------------------------------- */

typedef int64_t ms_acc;           /* 56-bit accumulator, sign-extended; also 24.24 long words */

static inline ms_acc ms_s56(ms_acc v) { return (ms_acc)((uint64_t)v << 8) >> 8; }
static inline ms_acc ms_shl(ms_acc v, int n) { return ms_s56((ms_acc)((uint64_t)v << n)); }
static inline ms_acc ms_w(int32_t x) { return (ms_acc)x * 16777216; }
static inline ms_acc ms_mpy(int32_t x, int32_t y) { return (ms_acc)x * y * 2; }
static inline ms_acc ms_mpysu(int32_t s, uint32_t u) { return (ms_acc)s * (ms_acc)(u & 0xffffff) * 2; }
static inline int32_t ms_hi(ms_acc a) { return (int32_t)((uint32_t)(a >> 24) << 8) >> 8; }   /* a1 as stored */
static inline uint32_t ms_lo(ms_acc a) { return (uint32_t)a & 0xffffff; }                    /* a0 */
static inline ms_acc ms_long(int32_t hi, uint32_t lo) { return ms_w(hi) | (lo & 0xffffff); }
static inline ms_acc ms_raw(ms_acc a) { return ms_long(ms_hi(a), ms_lo(a)); }                /* a1:a0 stored and reloaded */
static inline int32_t ms_sat(ms_acc a) {
   const ms_acc v = a >> 24;
   return v > MS_ONE ? MS_ONE : v < -MS_ONE - 1 ? -MS_ONE - 1 : (int32_t)v;
}
static inline ms_acc ms_sat48(ms_acc a) {
   const ms_acc hi = ((ms_acc)1 << 47) - 1;
   return a > hi ? hi : a < -hi - 1 ? -hi - 1 : a;
}
/* x times a 48-bit long word, the upper 48 bits (MPYSU then DMAC) */
static inline ms_acc ms_dmul(int32_t x, ms_acc y) { return (ms_mpysu(x, ms_lo(y)) >> 24) + ms_mpy(x, ms_hi(y)); }
/* RND: convergent rounding of a at a1 */
static inline ms_acc ms_rnd(ms_acc a) {
   const uint32_t lo = ms_lo(a);
   ms_acc r = a - lo;
   if (lo > 0x800000 || (lo == 0x800000 && (ms_hi(a) & 1))) r += ms_w(1);
   return r;
}
/* CLB: 9 minus the number of equal leading bits (0 for 0); NORMF by it */
static int ms_clb(ms_acc a) {
   if (!a) return 0;
   const uint64_t u = (uint64_t)a << 8, s = u >> 63;
   int n = 0;
   while (n < 56 && ((u >> (63 - n)) & 1) == s) ++n;
   return 9 - n;
}
static inline ms_acc ms_normf(ms_acc a, int s) { return s >= 0 ? a >> (s > 63 ? 63 : s) : ms_shl(a, -s); }
static int32_t ms_div(ms_acc a, int32_t s) {   /* 24 DIV steps from a clear carry: the quotient bits */
   uint64_t d = (uint64_t)a & 0xffffffffffffffULL;
   const uint64_t sv = (uint64_t)ms_w(s);
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

/* The oscillators' band-limited impulse: half of a windowed sinc at 32
 * points per sample, peak 0.5, copied from the firmware's kernel image
 * (Y:$131..$333), as no formula reproduces it. Index -1 is ms_impulse[0]. */
static const int32_t ms_impulse[515] = {
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

static int32_t ms_sine[0x2000];   /* sin(k pi / 4096), the boot code's 48-bit recurrence */
static int32_t ms_exp[0x800];     /* 2^(22 + k/2048), the kernel's pitch-to-frequency table */
static int32_t ms_leak[256];      /* the integrator's leak by pitch */
static int32_t ms_dc[1600];       /* the DC blocker by pitch */
static int32_t ms_detune[4][129]; /* SAW by UNIW: period x (1 + k/4096), frequency / that, period x (1 - k/4096), frequency / that; x 2^22 */
static int32_t ms_interval[2][128];   /* ENS by PCHx: period and frequency ratios, x 2^21 */
static int32_t ms_attack[128];    /* the AMP envelope's rise per block by ATK */
static int32_t ms_decay[129];     /* minus its fall per block by DEC and REL */
static int32_t ms_port[128];      /* the glide's speed by PORT, per tick */

/* Entries of ms_exp where the firmware's table is one LSB below the formula. */
static const uint32_t ms_exp_low[64] = {
   0x00000001, 0x00000000, 0x00000000, 0x00008040, 0x00000000, 0x00000000, 0x40012004, 0x00000080,
   0x80004000, 0x00000000, 0x28000408, 0x06100000, 0x00000000, 0x00000006, 0x00200000, 0x02003018,
   0x00000000, 0x9000460c, 0x00000090, 0x01410002, 0x01810480, 0x00004800, 0x82000500, 0x00868020,
   0x41080610, 0x4a142004, 0x18018004, 0x20002800, 0x30010000, 0x22062492, 0x00502844, 0x00042db1,
   0x00140804, 0x30440014, 0x40235104, 0x02002448, 0x24370104, 0x40506120, 0x88610a22, 0x801a00b0,
   0x0c0c209d, 0x0c21210c, 0x2a418020, 0xc1048884, 0xe974ba59, 0x11082820, 0x83416318, 0x5208260c,
   0x2c84208a, 0x42638174, 0x205e8410, 0x0228a101, 0x0828a040, 0xa1097d08, 0xd6c79050, 0xc429d423,
   0x444d8846, 0x220afd63, 0x24fd40d8, 0xa80880ac, 0x04983580, 0x0364f9ac, 0x1b6c0653, 0x980c94c7
};

/* ENS's intervals: semitones, or a just ratio num/den; and how many PCH values each takes, from 0 up */
static const struct { signed char semi, num, den, run; } ms_ens_steps[33] = {
   {-12, 0, 0, 2}, {-11, 0, 0, 4}, {-10, 0, 0, 4}, {-9, 0, 0, 4}, {-8, 0, 0, 4}, {0, 2, 3, 4}, {-7, 0, 0, 4},
   {-6, 0, 0, 4}, {-5, 0, 0, 3}, {0, 3, 4, 4}, {-4, 0, 0, 4}, {0, 4, 5, 4}, {0, 5, 6, 4}, {-3, 0, 0, 4},
   {-2, 0, 0, 4}, {-1, 0, 0, 4}, {0, 0, 0, 3}, {1, 0, 0, 4}, {2, 0, 0, 4}, {3, 0, 0, 4}, {0, 6, 5, 4},
   {0, 5, 4, 4}, {4, 0, 0, 4}, {0, 4, 3, 4}, {5, 0, 0, 3}, {6, 0, 0, 4}, {7, 0, 0, 4}, {0, 3, 2, 4},
   {8, 0, 0, 4}, {9, 0, 0, 4}, {10, 0, 0, 4}, {11, 0, 0, 4}, {12, 0, 0, 5}
};

static void ms_tables(void) {
   const int32_t c1 = 0x7ffffd, c0 = 0x885869;                 /* round(cos(pi/4096) x 2^47) */
   ms_acc prev = 0, cur = ((ms_acc)0x1921 << 24) | 0xfb2aed;   /* round(sin(pi/4096) x 2^47) */
   ms_sine[0] = 0;
   ms_sine[1] = 0x1921;
   for (int k = 2; k < 0x2000; ++k) {
      const int32_t s1 = (int32_t)(cur >> 24), s0 = (int32_t)(cur & 0xffffff);
      ms_acc a = (ms_acc)c0 * s0 * 2;
      a = (a >> 24) + (ms_acc)c1 * s0 * 2;
      a += (ms_acc)s1 * c0 * 2;
      a = (a >> 24) + (ms_acc)c1 * s1 * 2;
      const ms_acc next = ms_sat48(a * 2 - prev);
      ms_sine[k] = (int32_t)(next >> 24);
      prev = cur;
      cur = next;
   }
   for (int k = 0; k < 0x800; ++k)
      ms_exp[k] = (int32_t)floor(exp2(22 + k / 2048.0)) - (int32_t)(ms_exp_low[k >> 5] >> (k & 31) & 1);
   const double pi = 3.14159265358979323846, m = sqrt(2.0) - 1.0;
   for (int k = 0; k < 256; ++k)   /* 1 - e^(-2 pi f / 44100), f from 4 Hz over 12.5 octaves (as in Mono Ringmod) */
      ms_leak[k] = (int32_t)floor((1 - exp(-2 * pi * 4.003922 * pow(2, 12.5 * k / 255) / 44100)) * 8388608 + 0.5);
   static const short high[] = {191, 193, 210, 225, 232};   /* one LSB higher in the original */
   for (unsigned i = 0; i < sizeof high / sizeof high[0]; ++i) ms_leak[high[i]] += 1;
   for (int k = 0; k < 1600; ++k) {   /* the track filters' one-pole formula at A4 - 83 semitones x 2^(k/128) */
      const double c = 1 - cos(2 * pi * 440 * pow(2, -83 / 12.0 + k / 128.0) / 44100);
      ms_dc[k] = (int32_t)floor((sqrt(c * c + 2 * c * m) - c) / m * 8388608 + 0.5);
   }
   for (int k = 0; k < 128; ++k) {
      ms_detune[0][k] = 0x400000 + 1024 * k;
      ms_detune[1][k] = (int32_t)floor(17179869184.0 / (4096 + k) + 0.5);
      ms_detune[2][k] = 0x400000 - 1024 * k;
      ms_detune[3][k] = (int32_t)floor(17179869184.0 / (4096 - k) + 0.5);
   }
   /* UNIW's last word (0x7f8000 and up) rounds to 128: the next table's first entry */
   ms_detune[0][128] = ms_detune[1][128] = ms_detune[2][128] = 0x400000;
   ms_detune[3][128] = 0x00d008;
   for (int s = 0, k = 0; s < 33; ++s) {
      const double r = ms_ens_steps[s].num ? (double)ms_ens_steps[s].num / ms_ens_steps[s].den : exp2(ms_ens_steps[s].semi / 12.0);
      for (int n = 0; n < ms_ens_steps[s].run; ++n, ++k) {
         ms_interval[0][k] = (int32_t)floor(2097152.0 / r + 0.5);
         ms_interval[1][k] = (int32_t)floor(2097152.0 * r + 0.5);
      }
   }
   /* attack: 0.5 x e^(-c k) to six decimals, in 2^-23 */
   for (int k = 0; k < 128; ++k) {
      const int64_t micro = (int64_t)floor(5e5 * exp(-0.08354855518 * k) + 0.5);
      ms_attack[k] = (int32_t)((micro * 8388608 + 500000) / 1000000);
   }
   /* decay: 60 dB in 64 x e^(c k) blocks; 127 holds, and the word after the table is 1 */
   for (int k = 0; k < 127; ++k)
      ms_decay[k] = -(int32_t)floor(8388608.0 * pow(10.0, -3.0 / 64 * exp(-0.06730291836 * k)) + 0.5);
   ms_decay[127] = -MS_ONE - 1;
   ms_decay[128] = 1;
   /* portamento: 2^30 / (14400 x 2^(13k/128)); 0 is a negative mark, a glide of two blocks */
   ms_port[0] = -0x12345;
   for (int k = 1; k < 128; ++k) ms_port[k] = (int32_t)floor(1073741824.0 / 14400.0 / exp2(13.0 * k / 128.0) + 0.5);
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
typedef struct { ms_acc now; int32_t start, target; } MsPitch;

static void ms_pitch_reset(MsPitch *g, int32_t word) { g->now = ms_w(word); g->start = g->target = word; }

static int32_t ms_pitch(MsPitch *g, int32_t word, int32_t tune, int32_t port, int32_t tick, int32_t pmod, int32_t mtune) {
   if (word != g->target) {   /* a glide still under way restarts from where it is */
      const int32_t now = ms_hi(g->now);
      if ((word < now && g->target >= now) || (word >= now && g->target < now)) g->start = g->target;
      g->target = word;
   }
   const ms_acc r = ms_mpy(ms_port[(port >> 16) & 0x7f], tick);
   const int32_t rate = r < 0 ? 0x7f0000 : (int32_t)((uint32_t)(r >> 8) << 8) >> 8;   /* a0 after ASR 8, as a signed word */
   const ms_acc dist = ms_w(g->target) - ms_w(g->start);
   ms_acc a = g->now + ms_mpy(ms_sat(dist), rate);
   if (dist >= 0 ? a > ms_w(g->target) : a < ms_w(g->target)) { g->start = g->target; a = ms_w(g->target); }
   g->now = ms_raw(a);
   const ms_acc p = a + ms_w(mtune) + ms_mpy(0x2ab, tune) + ms_mpy(0xb000, pmod) - ms_w(0x5800);   /* TUNE: 341.5 at 64 */
   return p < 0 ? 0 : p >= ms_w(MS_PITCH_MAX) ? MS_PITCH_MAX : ms_hi(p);
}

/* The main CPU's pitch words (main OS of OS 1.32B). Word 38 for MASTER TUNE
 * in tenths of a hertz, 4000..4999: its table at 0x251210 is
 * trunc(2048 log2(f / 440)). */
static int32_t ms_master_tune(int tenths) { return (int32_t)(2048.0 * log2(tenths / 4400.0)); }

/* The PTCH page's DEST ranges in semitones: 1/12 2/12 7/12 1OCT 2OCT 4OCT 8OCT 16OC. */
static const int32_t ms_range[8] = {1, 2, 7, 12, 24, 48, 96, 192};

/* What an LFO on page PTCH adds to word 30 with its wave at the peak (+-128 in
 * the high word) and DPTH d (negative for the inverted wave): (d x 128 >> 2) x
 * the range's step, 2 per semitone (the main OS's table at 0x2590d4). */
static int32_t ms_lfo_pitch(int d, int range) { return d * 64 * ms_range[range & 7]; }

/* What a pitch bend adds to word 30 through KIT ASSIGN JOY R/L with MIRR on,
 * PAGE PTCH, DEST range (0..7: 1/12 2/12 7/12 1OCT 2OCT 4OCT 8OCT 16OC) and
 * ADD +63: the bend's top 8 bits v (2 msb + (lsb >> 6)) are JOY R at v - 128
 * and JOY L at 127 - v, whose ADD the mirror negates; value x ADD x the
 * range's step, which is 4 x the range in semitones (the main OS's steps at
 * 0x251b30: 4 8 28 48 96 192 384 768). */
static int32_t ms_bend(int v, int range) {
   return (v >= 128 ? v - 128 : v - 127) * 63 * 4 * ms_range[range & 7];
}

/* A controller's value added to its word as 0x010003ba does: clamped on the side it moves to. */
static int32_t ms_add_mod(int32_t word, int32_t v) {
   const int32_t w = word + v;
   return v > 0 && w > 0x7effff ? 0x7effff : v < 0 && w < 0 ? 0 : w;
}

/* The kernel's frequency word for a pitch index (register a on the machine's entry). */
static ms_acc ms_freq(int32_t pitch) {
   const int32_t x = ms_sat(ms_shl(ms_w(ms_exp[pitch & 0x7ff]) >> 10, pitch >> 11));
   return ms_sat48(ms_mpy(x, 0x1d22a) >> 3);
}

/* ---- the machines -------------------------------------------------------------- */

typedef struct {
   int machine;
   /* the oscillators */
   int32_t ring[64];      /* impulses still to be integrated (X:$580..$5bf) */
   int slot;              /* the ring's current 16-sample slot: 0, 16, 32 or 48 */
   ms_acc edge[5];        /* each oscillator's time to its next edge */
   int list[4];           /* PULS, ENS: which interval comes next, high or low */
   int amp[4];            /* which impulse height comes next (SAW: one for all) */
   ms_acc freq[5];        /* SAW, ENS: each oscillator's frequency, last block */
   int32_t integ, leak;   /* the integrator, and its leak (SAW, ENS: last block's) */
   int32_t dc, dc_in;     /* the DC blocker */
   int dc_index;          /* SAW, PULS: last block's index into ms_dc */
   ms_acc sub_inc[2], sub_phase[2];   /* SAW, PULS: the sines' steps and phases */
   int32_t pw_sweep;      /* PULS: PWAD's sweep */
   /* ENS's chorus */
   int32_t count, gate, level;   /* blocks since the start (to 64), the wet's gate, CHRL smoothed */
   int32_t lfo[2];               /* its two LFOs, cosine and sine */
   int wp;                       /* the delay's write position */
   int32_t delay[1024];
   int32_t srr[2][2];            /* the track's SRR stage before it: at its default, a delay of two samples */
} MonoSwave;

/* The machine's init routine (on assignment), from cleared memory. */
static void ms_init(MonoSwave *s, int machine) {
   memset(s, 0, sizeof *s);
   s->machine = machine;
   s->slot = 0x88 - 0x58;
   if (machine == MS_ENS) { s->lfo[0] = -MS_ONE - 1; return; }
   s->sub_phase[0] = s->sub_phase[1] = ms_w(MS_ADDR);
}

/* The machine's trig routine (on a note). */
static void ms_trig(MonoSwave *s, const int32_t syn[8]) {
   if (s->machine == MS_PULS && syn[6] >= 0x400000) s->pw_sweep = 0;   /* PWRS */
}

/* P:$3e0 and P:$3d3: the edges in this block, from b (the time to the next
 * edge), every step[pos] samples, pos alternating. */
typedef struct { int n; int32_t time[64]; ms_acc phase[64]; } MsEdges;

static ms_acc ms_edges(ms_acc b, const ms_acc step[2], int *pos, MsEdges *e) {
   e->n = 0;
   while (b <= ms_w(16) && e->n < 64) {   /* shortcut: 64 edges a block at most; the machines make no more than a dozen */
      e->time[e->n] = ms_hi(b);
      e->phase[e->n++] = ms_mpysu(16, ms_lo(b));
      b += step[*pos];
      *pos ^= 1;
   }
   return ms_raw(b - ms_w(16));
}

/* P:$3aa: a band-limited impulse for each edge, of heights h[pos] alternating, into the ring. */
static void ms_impulses(MonoSwave *s, const MsEdges *e, const int32_t h2[2], int *pos) {
   for (int n = 0; n < e->n; ++n) {
      const int32_t h = h2[*pos];
      *pos ^= 1;
      const int ph = ms_hi(e->phase[n]);
      const ms_acc hb = ms_mpysu(h, ms_lo(e->phase[n])) >> 1;
      const int32_t wf = ms_sat(hb), wn = ms_sat(-hb + ms_w(h));
      int r0 = (s->slot + e->time[n] - 1) & 63, r2 = (s->slot + e->time[n] + 16) & 63;
      int i4 = 33 - ph, i5 = 481 + ph;   /* ms_impulse index = table index + 1 */
      ms_acc a = ms_w(s->ring[r2]), b = ms_w(s->ring[r0]);
      for (int k = 0; k < MS_FRAMES; ++k) {
         a += ms_mpy(ms_impulse[i5], wf) + ms_mpy(ms_impulse[i5 - 1], wn);
         i5 -= 32;
         s->ring[r0] = ms_sat(b);
         r0 = (r0 + 1) & 63;
         b = ms_w(s->ring[r0]) + ms_mpy(ms_impulse[i4], wf) + ms_mpy(ms_impulse[i4 + 1], wn);
         i4 += 32;
         s->ring[r2] = ms_sat(a);
         r2 = (r2 + 1) & 63;
         a = ms_w(s->ring[r2]);
      }
      s->ring[r0] = ms_sat(b);
   }
}

/* The ring moves on a slot; the slot just read is cleared. */
static void ms_next_slot(MonoSwave *s) {
   memset(&s->ring[s->slot], 0, MS_FRAMES * sizeof s->ring[0]);
   s->slot = (s->slot + MS_FRAMES) & 63;
}

/* An edge's time rescaled when the frequency changes: by the old frequency x the new period. */
static ms_acc ms_rescale(int32_t old_hi, ms_acc period, ms_acc edge) {
   ms_acc a = ms_dmul(old_hi, period);
   const int t = ms_clb(a);
   a = ms_dmul(ms_sat(ms_normf(a, t)), edge);
   return ms_raw(ms_sat48(ms_normf(a, -t - 24)));
}

/* The impulses integrated (with a leak), from the ring's current slot. */
static void ms_integrate(MonoSwave *s, int32_t gain, int32_t leak, int32_t x[MS_FRAMES]) {
   ms_acc a = ms_w(s->integ);
   for (int i = 0; i < MS_FRAMES; ++i) {
      x[i] = ms_sat(a);
      a += ms_mpy(gain, s->ring[s->slot + i]) - ms_mpy(leak, x[i]);
   }
   s->integ = ms_sat(a);
}

/* The leak by pitch, interpolated in ms_leak at pitch / 96 - offset. */
static int32_t ms_leak_of(int32_t pitch, int offset) {
   ms_acc ix = ms_mpy(pitch, 0x15555) - ms_w(offset);
   if (ix < 0) ix = 0;
   const int k = ms_hi(ix);
   const uint32_t fr = ms_lo(ix);
   return ms_sat(((ms_mpysu(ms_leak[k + 1], fr) - ms_mpysu(ms_leak[k], fr)) >> 1) + ms_w(ms_leak[k]));
}

/* The DC blocker index by pitch (pitch x scale - 128, to 0..1599). */
static int ms_dc_index(int32_t pitch, int32_t scale) {
   const ms_acc ix = ms_mpy(pitch, scale) + ms_w(-128);
   return ix < 0 ? 0 : ix > ms_w(0x63f) ? 0x63f : ms_hi(ix);
}

/* The DC blocker at coefficient kd, then x 2^shift. */
static void ms_dc_block(MonoSwave *s, int32_t kd, int shift, int32_t x[MS_FRAMES]) {
   const int32_t g = ms_sat((ms_w(MS_ONE) - ms_w(kd) + ms_w(MS_ONE)) >> 1);
   ms_acc a = ms_w(s->dc);
   int32_t xp = s->dc_in;
   for (int i = 0; i < MS_FRAMES; ++i) {
      const int32_t prev = ms_sat(a), in = x[i];
      x[i] = ms_sat(ms_shl(a, shift));
      a += -ms_mpy(xp, g) + ms_mpy(in, g) - ms_mpy(kd, prev);
      xp = in;
   }
   s->dc = ms_sat(a);
   s->dc_in = xp;
}

/* SAW and PULS: the two sines (one and two octaves down) added at their levels.
 * The second of each pair of samples takes the table's next entry instead of
 * the interpolated value, as the original does. */
static void ms_subs(MonoSwave *s, ms_acc freq, const int32_t level[2], int32_t x[MS_FRAMES]) {
   ms_acc inc = ms_sat48(ms_dmul(0x17c6f9, freq));
   for (int k = 0; k < 2; ++k) {
      inc >>= 1;
      const ms_acc old = s->sub_inc[k], delta = ms_raw((inc - old) >> 4);
      s->sub_inc[k] = ms_raw(inc);
      ms_acc a = old, b = s->sub_phase[k], ph[MS_FRAMES];
      for (int i = 0; i < MS_FRAMES; ++i) {
         a += delta;
         ph[i] = b;
         b += a;
         b = ms_w((ms_hi(b) & 0x1fff) + MS_ADDR) | ms_lo(b);
      }
      s->sub_phase[k] = ms_raw(b);
      for (int i = 0; i < MS_FRAMES; i += 2) {
         int j = ms_hi(ph[i]) - MS_ADDR;
         const uint32_t fr = ms_lo(ph[i]);
         const int32_t v = ms_sat((-ms_mpysu(ms_sine[j], fr) + ms_w(ms_sine[j]) + ms_w(ms_sine[j]) + ms_mpysu(ms_sine[(j + 1) & 0x1fff], fr)) >> 1);
         x[i] = ms_sat(ms_w(x[i]) + ms_mpy(level[k], v));
         j = ms_hi(ph[i + 1]) - MS_ADDR;
         x[i + 1] = ms_sat(ms_w(x[i + 1]) + ms_mpy(ms_sine[(j + 1) & 0x1fff], level[k]));
      }
   }
}

static void ms_saw(MonoSwave *s, const int32_t syn[8], int32_t pitch, ms_acc freq, int32_t x[MS_FRAMES]) {
   const int32_t p = pitch > 0x6000 ? 0x6000 : pitch;
   const int oct = p >> 11, idx = p & 0x7ff;
   ms_acc period[5], f[5], old[5];
   period[0] = ms_sat48(ms_mpy(0x17a6, ms_sat(ms_w(ms_exp[0x7ff - idx]) >> oct)));
   f[0] = ms_sat48(ms_mpy(ms_sat(ms_shl(ms_w(ms_exp[idx]) >> 12, oct)), 0x56982b));
   /* UNIW: the detune table's index, and half of it */
   const int i1 = ms_sat(ms_rnd(ms_mpy(syn[1], 0x80))), i2 = ms_sat(ms_rnd(ms_mpy(syn[1], 0x40)));
   const int at[4] = {i1, i1, i2, i2};
   for (int k = 1; k < 5; ++k) {
      f[k] = ms_sat48(ms_shl(ms_dmul(ms_detune[k & 1 ? 1 : 3][at[k - 1]], f[0]), 1));
      period[k] = ms_sat48(ms_shl(ms_dmul(ms_detune[k & 1 ? 0 : 2][at[k - 1]], period[0]), 1));
   }
   for (int k = 0; k < 5; ++k) { old[k] = s->freq[k]; s->freq[k] = f[k]; }
   if (ms_hi(f[0]) != ms_hi(old[0]))
      for (int k = 0; k < 5; ++k) s->edge[k] = ms_rescale(ms_hi(old[k]), period[k], s->edge[k]);
   /* heights: the main saw's alternate (SUBX), the outer pair at UNIL, the inner at UNIL x UNIX */
   const int32_t main2[2] = {ms_sat(ms_w(0x1d41d) + ms_mpy(syn[4], 0x0ebe67)), 0x1d41d};
   const int32_t unil = ms_sat(ms_mpy(syn[0], 0x1d41d)), unix = ms_sat(ms_mpy(syn[2], unil));
   for (int k = 0; k < 5; ++k) {
      MsEdges e;
      int pos = 0;
      const ms_acc step[2] = {period[k], period[k]};
      s->edge[k] = ms_edges(s->edge[k], step, &pos, &e);
      if (k == 0) {
         ms_next_slot(s);
         ms_impulses(s, &e, main2, &s->amp[0]);
      } else {
         const int32_t h = k < 3 ? unil : unix, h2[2] = {h, h};
         ms_impulses(s, &e, h2, &pos);
      }
   }
   const int32_t leak = s->leak;
   s->leak = ms_leak_of(p, 20);
   ms_integrate(s, 0x7eb852, leak, x);
   const int32_t kd = ms_dc[s->dc_index];
   s->dc_index = ms_dc_index(p, 0xb3333);
   ms_dc_block(s, kd, 3, x);
   const int32_t level[2] = {ms_sat(ms_mpy(syn[5], 0x80000)), ms_sat(ms_mpy(syn[6], 0x80000))};
   ms_subs(s, freq, level, x);
}

/* A pulse's high and low times from its period and duty. */
static void ms_split(int32_t duty, ms_acc period, ms_acc out[2]) {
   out[0] = ms_dmul(duty, period);
   out[1] = period - out[0];
}

static void ms_puls(MonoSwave *s, const int32_t syn[8], int32_t pitch, ms_acc freq, int32_t x[MS_FRAMES]) {
   /* the period, 2822400 / frequency, normalised in and out of a 24-bit division (as in Mono Ringmod) */
   ms_acc fz = freq >= ms_w(0x4074) ? ms_w(0x4074) : freq;
   const int t = ms_clb(fz);
   const int32_t quot = ms_div(ms_w(0x2b1100), ms_sat(ms_normf(fz, t)));
   const ms_acc period = ms_sat48(ms_normf(ms_w(quot), t + 0x1d));
   /* the width: PW plus PWAD's sweep, folded */
   s->pw_sweep = (int32_t)((uint32_t)(s->pw_sweep + (syn[5] >> 9)) << 8) >> 8;
   ms_acc b = (ms_w(syn[4]) + ms_w(s->pw_sweep)) >> 1;
   if (b >= ms_w(0x400000)) b -= ms_w(0x800000);
   b = ms_shl(b, 1);
   if (b < 0) b = -b;
   const int32_t duty = ms_sat(ms_w(0x147ae) + ms_mpy(ms_sat(b), 0x7d70a4));
   /* three periods: as is, and UNIW / 64 longer and shorter */
   ms_acc hl[3][2];
   ms_split(duty, period, hl[0]);
   ms_acc d = ((ms_acc)(-ms_mpysu(syn[1], ms_lo(period))) >> 24) - ms_mpy(syn[1], ms_hi(period));
   d = (d >> 6) + period;
   if (d < 0) d = 0;
   const ms_acc u = (ms_dmul(syn[1], period) >> 6) + period;
   ms_split(duty, ms_raw(u), hl[1]);
   ms_split(duty, ms_sat48(d), hl[2]);
   const int32_t g = ms_sat(ms_mpy(0x70a3d, syn[0]));
   const int32_t h2[3][2] = {{0x70a3d, -0x70a3d}, {g, ms_sat(-ms_mpy(0x70a3d, syn[0]))}, {g, ms_sat(-ms_mpy(0x70a3d, syn[0]))}};
   for (int k = 0; k < 3; ++k) {
      const ms_acc step[2] = {ms_raw(hl[k][0]), ms_raw(hl[k][1])};
      ms_acc e0 = s->edge[k];
      if (e0 > step[s->list[k] ^ 1]) e0 = step[s->list[k] ^ 1];
      MsEdges e;
      s->edge[k] = ms_edges(e0, step, &s->list[k], &e);
      if (k == 0) ms_next_slot(s);
      ms_impulses(s, &e, h2[k], &s->amp[k]);
   }
   ms_integrate(s, 0x333333, 0x54, x);
   const int32_t kd = ms_dc[s->dc_index];
   s->dc_index = ms_dc_index(pitch, 0x80000);
   ms_dc_block(s, kd, 2, x);
   const int32_t level[2] = {ms_sat(ms_mpy(syn[2], 0x200000)), ms_sat(ms_mpy(syn[3], 0x200000))};
   ms_subs(s, freq, level, x);
}

static void ms_ens(MonoSwave *s, const int32_t syn[8], int32_t pitch, int32_t x[MS_FRAMES]) {
   int32_t p = pitch >= 0x4d55 ? 0x4d55 : pitch;
   p = p < 0x800 ? 0 : p - 0x800;
   const int oct = p >> 11, idx = p & 0x7ff;
   ms_acc period[4], f[4];
   period[0] = ms_sat48(ms_mpy(ms_sat(ms_w(ms_exp[0x7ff - idx]) >> oct), 0x17a6));
   f[0] = ms_sat48(ms_mpy(ms_sat(ms_shl(ms_w(ms_exp[idx]) >> 12, oct)), 0x56982b));
   int32_t ratio[4] = {0};
   for (int k = 1; k < 4; ++k) {
      const int n = ms_hi(ms_mpy(0x80, syn[k - 1]));
      period[k] = ms_sat48(ms_shl(ms_dmul(ms_interval[0][n], period[0]), 2));
      f[k] = ms_sat48(ms_shl(ms_dmul(ms_interval[1][n], f[0]), 2));
      ratio[k] = ms_interval[1][n];
   }
   int32_t old[4];
   for (int k = 0; k < 4; ++k) { old[k] = ms_hi(s->freq[k]); s->freq[k] = f[k]; }
   const int32_t duty = ms_sat(ms_w(0x147ae) + ms_mpy(syn[4], 0x7d70a4));
   ms_acc hl[4][2];
   for (int k = 0; k < 4; ++k) {
      ms_split(duty, period[k], hl[k]);
      hl[k][0] = ms_sat48(hl[k][0]);
      hl[k][1] = ms_sat48(hl[k][1]);
      if (ms_hi(f[k]) != old[k]) s->edge[k] = ms_rescale(old[k], period[k], s->edge[k]);
   }
   /* heights by WAVE: from a saw (equal) towards a pulse (opposite) */
   const int32_t h[2] = {ms_sat(ms_w(0x5c28f) + ms_mpy(syn[3], -0x09de7d)), ms_sat(ms_w(0x5c28f) + ms_mpy(syn[3], -0x01bdda))};
   static const int32_t mute[2] = {0, 0};
   for (int k = 0; k < 4; ++k) {
      MsEdges e;
      s->edge[k] = ms_edges(s->edge[k], hl[k], &s->list[k], &e);
      if (k == 0) ms_next_slot(s);
      ms_impulses(s, &e, k && ratio[k] == 0x200000 ? mute : h, &s->amp[k]);
   }
   const int32_t leak = s->leak;
   s->leak = ms_leak_of(pitch, 21);
   ms_integrate(s, 0x400000, leak, x);
   ms_dc_block(s, ms_dc[0], 3, x);
}

/* One 16-frame block of the machine. syn: its eight SYN block words; pitch:
 * the pitch index (0..MS_PITCH_MAX); trig: the block carries a note's trig. */
static void ms_block(MonoSwave *s, const int32_t syn[8], int32_t pitch, int trig, int32_t out[MS_FRAMES]) {
   if (trig) ms_trig(s, syn);
   const ms_acc freq = ms_freq(pitch);
   if (s->machine == MS_SAW) ms_saw(s, syn, pitch, freq, out);
   else if (s->machine == MS_PULS) ms_puls(s, syn, pitch, freq, out);
   else ms_ens(s, syn, pitch, out);
}

/* ---- the kernel: the AMP section ---------------------------------------------- */

typedef struct {
   int stage;            /* 0 attack, 1 hold, 2 decay, 3 release, 4 quick release */
   int32_t level, held;  /* the envelope, and blocks held */
   int32_t gain[2];      /* left and right at the end of the last block */
} MsAmp;

/* PAN's quarter sine: the right and left multipliers (Y:$0, Y:$1). */
static void ms_pan(int32_t pan_word, int32_t pan[2]) {
   const int p = ms_sat(ms_shl(ms_mpy(0x408e05, pan_word), 1)) >> 12;
   pan[0] = ms_sine[p & 0x1fff];
   pan[1] = ms_sine[(p + 0x801) & 0x1fff];
}

/* One block of the envelope and the gains. note: the low bits of the trig
 * word (1 note on, 2 note off, 3 stop, 0 nothing new); amp: the AMP page's
 * block words; tick_recip: block word 35 (0x800000 / (24 x BPM)). gain: each
 * frame's left and right gain. (As in Mono FM+.) */
static void ms_amp(MsAmp *e, int note, const int32_t amp[8], int32_t tick_recip, int32_t gain[2][MS_FRAMES]) {
   if (note == 1) e->stage = 0;
   else if (note >= 2) e->stage = note == 3 ? 4 : 3;
   if (e->stage == 0) {
      const ms_acc a = ms_w(e->level) + ms_w(ms_attack[(amp[MS_ATK] >> 16) & 127]);
      e->level = ms_sat(a);
      if (a > ms_w(MS_ONE) + 0xffffff) e->stage = e->held = 1;
   } else if (e->stage == 1) {
      const int32_t hold = amp[MS_HOLD] < 0x10000 ? 0 : amp[MS_HOLD];
      const ms_acc limit = ms_shl(ms_mpy(ms_sat(ms_mpy(tick_recip, hold)), 0x791fd0), 1), a = ms_w(e->held) + ms_w(1);
      if (a > limit) e->stage = 2;
      else e->held = ms_sat(a);
   } else {
      const int32_t knob = e->stage == 2 ? amp[MS_DEC] : e->stage == 3 ? amp[MS_REL] : 0x200000;
      const int k = ms_hi(ms_rnd((ms_w(knob) + ms_w(0x7fff)) >> 16));
      e->level = ms_sat(-ms_mpy(e->level, ms_decay[k < 0 ? 0 : k > 128 ? 128 : k]));
   }
   /* VOL squared, and PAN along a quarter sine */
   int32_t pan[2];
   ms_pan(amp[MS_PAN], pan);
   const int32_t g = ms_sat(ms_mpy(e->level, ms_sat(ms_mpy(amp[MS_VOL], amp[MS_VOL]))));
   const ms_acc to[2] = {ms_mpy(g, pan[1]), ms_mpy(pan[0], g)};
   for (int c = 0; c < 2; ++c) {
      const int32_t step = ms_sat(to[c] - ms_w(e->gain[c]));
      ms_acc a = ms_w(e->gain[c]);
      for (int i = 0; i < MS_FRAMES; ++i) {
         gain[c][i] = ms_sat(a);
         a += ms_mpy(step, 0x080000);
      }
      e->gain[c] = ms_sat(to[c]);
   }
}

/* The AMP section's product: x (a 48-bit word, the filter's output on the
 * unit) times the gain, x 16. */
static inline int32_t ms_apply(int32_t gain, ms_acc x) { return ms_sat(ms_shl(ms_dmul(gain, x), 4)); }

/* ---- ENS's chorus, after the AMP section ----------------------------------------- */

/* io: the track's left and right after the AMP section, changed in place;
 * pan: ms_pan of the AMP page's PAN word. A mono sum goes into a delay of
 * 1024 samples, read at 400 +- 340 samples by two LFOs a quarter turn apart
 * (0.2 Hz); left gets the first and right the second, both at CHRL (smoothed)
 * and panned, each mixed into the other side by 1 - CHRW / 2; the dry part
 * falls by CHRL. The wet is muted for the first 64 blocks. */
static void ms_chorus(MonoSwave *s, const int32_t syn[8], const int32_t pan[2], int32_t io[2][MS_FRAMES]) {
   if (s->count < 0x40) { s->count += 1; s->gate = 0; }
   else s->gate = MS_ONE;
   const int32_t sm = s->level;
   s->level = ms_sat(ms_mpy(syn[5], 0x066666) + ms_mpy(sm, 0x79999a));
   const int32_t dry = ms_sat(ms_w(MS_ONE) - ms_w(sm));
   const int32_t across = ms_sat(ms_w(MS_ONE) + ms_mpy(syn[6], -0x400000));   /* 1 - CHRW / 2 */
   const int32_t s1 = ms_sat(ms_mpy(MS_ONE, sm)), sw = ms_sat(ms_mpy(sm, across));
   const int32_t gr = ms_sat(ms_mpy(pan[0], s->gate)), gl = ms_sat(ms_mpy(s->gate, pan[1]));
   const int32_t c[4] = {ms_sat(ms_mpy(gl, s1)), ms_sat(ms_mpy(gr, sw)), ms_sat(ms_mpy(sw, gl)), ms_sat(ms_mpy(s1, gr))};
   /* the LFOs, stepped forward (forward Euler, saturating at 48 bits) */
   int32_t lfo[2][MS_FRAMES];
   ms_acc a = ms_w(s->lfo[0]), b = ms_w(s->lfo[1]);
   int32_t y1 = s->lfo[0];
   for (int i = 0; i < MS_FRAMES; ++i) {
      const int32_t y0 = ms_sat(b);
      lfo[1][i] = y0;
      b = ms_sat48(b - ms_mpy(0xef, y1));
      lfo[0][i] = y1 = ms_sat(a);
      a = ms_sat48(a + ms_mpy(y0, 0xef));
   }
   s->lfo[0] = ms_sat(a);
   s->lfo[1] = ms_sat(b);
   /* the two taps */
   int32_t tap[2][MS_FRAMES];
   const int base = s->wp - 400;
   for (int t = 0; t < 2; ++t)
      for (int i = 0; i < MS_FRAMES; ++i) {
         const ms_acc off = ms_mpy(lfo[t][i], 0x154);
         const int at = base + i + ms_hi(off);
         const uint32_t fr = ms_lo(off);
         const int32_t d0 = s->delay[at & 1023], d1 = s->delay[(at + 1) & 1023];
         tap[t][i] = ms_sat(((ms_mpysu(d1, fr) - ms_mpysu(d0, fr)) >> 1) + ms_w(d0));
      }
   for (int i = 0; i < MS_FRAMES; ++i)
      s->delay[(s->wp + i) & 1023] = ms_sat(ms_mpy(0x400000, io[0][i]) + ms_mpy(0x400000, io[1][i]));
   s->wp = (s->wp + MS_FRAMES) & 1023;
   for (int i = 0; i < MS_FRAMES; ++i) {
      io[0][i] = ms_sat(ms_mpy(dry, io[0][i]) + ms_mpy(tap[0][i], c[0]) + ms_mpy(tap[1][i], c[2]));
      io[1][i] = ms_sat(ms_mpy(io[1][i], dry) + ms_mpy(tap[0][i], c[1]) + ms_mpy(tap[1][i], c[3]));
   }
}

/* ENS after the AMP section: the SRR stage at its default (two samples of
 * delay), then the chorus. io: the AMP section's left and right, in place. */
static void ms_after_amp(MonoSwave *s, const int32_t syn[8], int32_t pan_word, int32_t io[2][MS_FRAMES]) {
   for (int c = 0; c < 2; ++c) {
      const int32_t a = io[c][MS_FRAMES - 2], b = io[c][MS_FRAMES - 1];
      memmove(&io[c][2], &io[c][0], (MS_FRAMES - 2) * sizeof io[c][0]);
      io[c][0] = s->srr[c][0];
      io[c][1] = s->srr[c][1];
      s->srr[c][0] = a;
      s->srr[c][1] = b;
   }
   int32_t pan[2];
   ms_pan(pan_word, pan);
   ms_chorus(s, syn, pan, io);
}

#endif
