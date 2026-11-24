/* mono_track_host.h - the Monomachine main CPU's side of one track: knob
 * slew, the three LFOs and the 52-word parameter block.
 *
 * A port of the main OS of OS 1.32B: its frame routine (0x247dba), LFO step
 * (0x267aba), evaluate (0x24010c), waves (0x240496..), apply (0x23fe46) and
 * slew (0x267dee). A frame is three DSP blocks (48 samples); the arithmetic is
 * the CPU's, 32-bit and wrapping.
 *
 * C99. SPDX-License-Identifier: GPL-3.0-only */
#ifndef MONO_TRACK_HOST_H
#define MONO_TRACK_HOST_H

enum { L_PAGE, L_DEST, L_TRIG, L_WAVE, L_MULT, L_SPD, L_INTL, L_DPTH };
enum { MT_FREE, MT_TRIG, MT_HOLD, MT_ONE, MT_HALF };
enum { MT_KNOBS = 32 };   /* SYN, AMP, FILT, EFX */
enum { MT_VOICES = 6, MT_STEPS = 64, MT_LOCKS = 56 };   /* lockable: SYN AMP FILT EFX LFO1-3, eight each */

typedef struct {
   int32_t pos, out, hold, inc, rem, gate, count;
   int32_t rnd_a, rnd_b, acc, delta, start;
   int seg, page, dest;     /* RND's last segment; the target, as of the last evaluation */
} MtLfo;

typedef struct {
   int32_t knob[MT_KNOBS];  /* what each knob sends (value << 16): SYN 0-7, AMP 8-15, FILT 16-23, EFX 24-31 */
   int32_t level;           /* LEVEL << 16 */
   int32_t raw[3][8];       /* the LFO pages' values, 0..127 */
   int32_t bend;            /* what KIT ASSIGN adds to word 30 */
   int32_t shadow[3][8];    /* LFO parameters as the LFOs read them: raw << 16, plus LFO-to-LFO modulation */
   int32_t word[MT_WORDS];  /* the block before modulation */
   int32_t block[MT_WORDS]; /* the block the DSP gets */
   MtLfo lfo[3];
   uint32_t frame;
   int lfo_trig;            /* an LFO trig is pending until the next step */
} MtHost;

static const int32_t mt_lfo_range[8] = {2, 4, 14, 24, 48, 96, 192, 384};   /* PTCH page by DEST (0x2590d4) */
static const int32_t mt_lfo_start[16] = {0, 0, -0x800000, 0x800000, 0x800000, -0x800000, 0x800000, -0x800000};   /* 0x259094 */

static inline int32_t mt_i32(int64_t v) { return (int32_t)(uint32_t)(uint64_t)v; }
static inline int32_t mt_hi16(int32_t v) { return (int16_t)(uint16_t)((uint32_t)v >> 16); }
/* a list parameter's index: raw x n / 128 */
static inline int mt_list(int32_t shadow, int n) {
   const int32_t v = mt_hi16(shadow) * n;
   return (v < 0 ? v + 127 : v) >> 7;
}
static inline int mt_dest(int32_t shadow) { return mt_i32((int64_t)(uint32_t)shadow * 8) >> 23; }

static void mt_wave(MtLfo *s, int wave, int restart, int32_t inc) {
   const int32_t p = s->pos;
   const int32_t p2 = mt_i32((int64_t)p * 2), p4 = mt_i32((int64_t)p * 4);
   const int edge = p == 0x800000 || p == 0x400000;
   switch (wave) {
   case 0: s->out = p <= 0x1fffff ? p4 : p > 0x5fffff ? mt_i32((int64_t)p4 - 0x2000000) : mt_i32(0x1000000 - (int64_t)p4); break;
   case 1: s->out = p <= 0x1fffff ? mt_i32(-(int64_t)p4) : p > 0x5fffff ? mt_i32(0x2000000 - (int64_t)p4) : mt_i32((int64_t)p4 - 0x1000000); break;
   case 3: s->out = mt_i32(0x800000 - (int64_t)p2); break;
   case 4: s->out = edge ? 0 : p <= 0x3fffff ? 0x800000 : -0x800000; break;
   case 5: s->out = edge ? 0 : p <= 0x3fffff ? -0x800000 : 0x800000; break;
   case 6: case 7:
      if (restart) s->out = wave == 6 ? 0x800000 : -0x800000;
      else s->out = mt_i32((int64_t)s->out - (mt_i32((int64_t)(s->out >> 10) * (inc >> 5)) >> 6));
      break;
   case 8: s->out = p <= 0x3fffff ? p2 : p == 0x400000 ? 0x800000 : 0; break;
   case 9: s->out = p <= 0x3fffff ? mt_i32(-(int64_t)p2) : p == 0x400000 ? -0x800000 : 0; break;
   case 10: {   /* eight random steps per cycle */
      if (edge) { s->out = 0; break; }
      const int seg = p >> 20;
      if (!restart && seg == s->seg) break;
      s->seg = (int8_t)seg;
      const int32_t sum = mt_i32((int64_t)s->rnd_a + s->rnd_b);
      s->out = (sum & 0x7fffff) - 0x400000;
      s->rnd_b = s->rnd_a;
      s->rnd_a = sum;
      break;
   }
   default: s->out = mt_i32((int64_t)p2 - 0x800000); break;   /* SAW, and the unused list entries */
   }
}

static void mt_lfo_evaluate(MtHost *h, int l, int restart, int32_t inc) {
   MtLfo *s = &h->lfo[l];
   const int32_t *sh = h->shadow[l];
   const int wave = mt_list(sh[L_WAVE], 11), mode = mt_list(sh[L_TRIG], 5);
   mt_wave(s, wave & 15, restart, inc);
   const int32_t src = mode == MT_HOLD ? s->hold : s->out, st = mt_lfo_start[wave & 15];
   const int retrig = mode != MT_FREE && mode != MT_HOLD;
   if (mt_list(sh[L_PAGE], 9) == 0) {
      const int32_t range = mt_lfo_range[mt_dest(sh[L_DEST]) & 7], d = mt_hi16(sh[L_DPTH]);
      s->delta = mt_i32((int64_t)mt_i32((int64_t)(mt_i32((int64_t)d * mt_hi16(src)) >> 2) * range) - s->acc) >> 2;
      if (retrig) s->start = mt_i32((int64_t)(mt_i32((int64_t)mt_hi16(st) * d) >> 2) * range);
   } else {
      const int32_t d = sh[L_DPTH] >> 11;
      s->delta = mt_i32((int64_t)mt_i32((int64_t)(src >> 12) * d) - s->acc) >> 2;
      if (retrig) s->start = mt_i32((int64_t)(st >> 12) * d);
   }
   s->page = mt_list(sh[L_PAGE], 9);
   s->dest = mt_dest(sh[L_DEST]) & 7;
}

/* Once per eight frames. elapsed: DSP blocks since the last step (24). */
static void mt_lfo_step(MtHost *h, int32_t elapsed, int32_t tick) {
   const int trig = h->lfo_trig;
   h->lfo_trig = 0;
   for (int l = 0; l < 3; ++l) {
      MtLfo *s = &h->lfo[l];
      const int32_t *sh = h->shadow[l];
      const int mode = mt_list(sh[L_TRIG], 5);
      int restart = mode == MT_HOLD ? 0 : trig;
      const int32_t d = mt_i32((int64_t)mt_i32((int64_t)(sh[L_SPD] >> 10) * tick) * elapsed);
      const int32_t q = mt_i32((int64_t)s->rem + d + 496125) / 992250;
      s->rem = mt_i32((int64_t)d + s->rem - (int64_t)q * 992250);
      const int32_t inc = mt_i32((int64_t)((uint64_t)(uint32_t)q << (6 + mt_list(sh[L_MULT], 7))));
      s->inc = inc;
      if (restart && mode > MT_FREE) {
         s->pos = 0;
         mt_lfo_evaluate(h, l, 1, inc);
      } else {
         const int32_t p = mt_i32((int64_t)s->pos + inc);
         s->pos = p;
         restart = 0;
         if (mode == MT_HALF && p > 0x400000) s->pos = 0x400000;
         else if (p > 0x7fffff) {
            if (mode == MT_ONE) s->pos = 0x800000;
            else { s->pos = mt_i32((int64_t)p - 0x800000); restart = 1; }
         }
         mt_lfo_evaluate(h, l, restart, inc);
      }
      if (trig) { s->hold = s->out; s->count = 0x791d0000; s->gate = 1; }
   }
}

/* Each LFO's value onto its target. With an LFO trig pending, TRIG/ONE/HALF
 * LFOs go to their start value; the firmware stops looking at the trig for
 * the rest of the track at the first gated LFO in FREE or HOLD. */
static void mt_lfo_apply(MtHost *h, int trig) {
   for (int l = 0; l < 3; ++l) {
      MtLfo *s = &h->lfo[l];
      if (!s->gate) continue;
      if (trig) {
         const int mode = mt_list(h->shadow[l][L_TRIG], 5);
         if (mode == MT_FREE || mode == MT_HOLD) trig = 0;
         else s->acc = s->start;
      }
      const int32_t v = s->acc;
      if (!v) continue;
      int32_t *p;
      if (s->page == 0) p = &h->block[W_PMOD];
      else if (s->page == 1) p = &h->block[W_SYN + s->dest];
      else if (s->page <= 4) p = &h->block[(s->page - 2) * 8 + s->dest];
      else if (s->page <= 7) p = &h->shadow[s->page - 5][s->dest];
      else continue;       /* MIDI: nothing on an audio track */
      *p = mt_i32((int64_t)*p + v);
      if (v > 0) { if (*p > 0x7effff) *p = 0x7effff; }
      else if (*p < 0) *p = 0;
   }
}

/* The LFO parameters as the LFOs read them, before LFO-to-LFO modulation. */
static void mt_host_shadows(MtHost *h, const int32_t *mod) {
   for (int l = 0; l < 3; ++l)
      for (int k = 0; k < 8; ++k) {
         const int64_t v = (int64_t)h->raw[l][k] * 65536 + (mod ? mod[MT_KNOBS + 8 * l + k] : 0);
         h->shadow[l][k] = v < 0 ? 0 : v > 0x7f0000 ? 0x7f0000 : (int32_t)v;
      }
}

/* mod: per-parameter additions from the host (word units; SYN..EFX then the
 * LFO pages), or NULL. */
static void mt_host_rebuild(MtHost *h, const int32_t *mod) {
   memcpy(h->block, h->word, sizeof h->block);
   mt_host_shadows(h, mod);
   if (mod)
      for (int k = 0; k < MT_KNOBS; ++k)
         if (mod[k]) {
            int32_t *p = &h->block[k < 8 ? W_SYN + k : k - 8];
            const int64_t v = (int64_t)*p + mod[k];
            *p = v < 0 ? 0 : v > 0x7effff ? 0x7effff : (int32_t)v;
         }
   /* KIT ASSIGN (pitch bend), then the LFOs */
   h->block[W_PMOD] = mf_add_mod(h->block[W_PMOD], h->bend);
   mt_lfo_apply(h, h->lfo_trig);
}

/* Words at their targets, LFOs at rest. */
static void mt_host_reset(MtHost *h) {
   memset(h->lfo, 0, sizeof h->lfo);
   memset(h->word, 0, sizeof h->word);
   for (int l = 0; l < 3; ++l) {
      h->lfo[l].rnd_a = 19741974;   /* the OS's power-on seed */
      h->lfo[l].rnd_b = 666;
   }
   for (int k = 0; k < MT_KNOBS; ++k) h->word[k < 8 ? W_SYN + k : k - 8] = h->knob[k];
   h->word[W_PMOD] = 64 << 16;
   h->word[W_LEVEL] = h->level;
   h->frame = 0;
   h->lfo_trig = 0;
}

/* One frame: the block for the next three DSP blocks. The caller has set the
 * one-shot and info words in h->word (W_NOTE, W_FTRIG, W_PITCH, W_TICK, W_RECIP,
 * W_ROUTE, W_MTUNE). */
static void mt_host_frame(MtHost *h, const int32_t *mod) {
   const uint32_t f = h->frame++;
   const int32_t tick = h->word[W_TICK];
   mt_host_rebuild(h, mod);
   if (!(f & 1)) {   /* interlace timers, then every LFO moves a quarter of the way */
      for (int l = 0; l < 3; ++l) {
         MtLfo *s = &h->lfo[l];
         const int32_t intl = h->shadow[l][L_INTL];
         if (!intl) s->gate = 1;
         else if (s->count < 0) {
            s->count = mt_i32((int64_t)s->count + 0x78f00000);
            s->gate = !s->gate;
         } else s->count = mt_i32((int64_t)s->count - mt_i32((int64_t)(intl >> 6) * tick));
      }
      for (int l = 0; l < 3; ++l) h->lfo[l].acc = mt_i32((int64_t)h->lfo[l].acc + h->lfo[l].delta);
   }
   if ((f & 7) == 1) {   /* every knob's word moves a quarter of the way to its value */
      for (int k = 0; k < MT_KNOBS; ++k) {
         int32_t *w = &h->word[k < 8 ? W_SYN + k : k - 8];
         *w = mt_i32((int64_t)*w * 3 + h->knob[k]) >> 2;
      }
      h->word[W_PMOD] = mt_i32((int64_t)h->word[W_PMOD] * 3 + (64 << 16)) >> 2;
      mt_host_rebuild(h, mod);
      h->word[W_LEVEL] = mt_i32((int64_t)h->word[W_LEVEL] * 3 + h->level) >> 2;
      h->block[W_LEVEL] = h->word[W_LEVEL];
   }
   if ((f & 7) == 0) mt_lfo_step(h, 24, tick);
}

/* ---- the pattern's length and speed (SCALE SETUP) -------------------------------
 * A pattern is 2-64 steps, in pages of 16 (length: pattern byte 0x424). Its speed
 * (0x425) is 1X, 2X, 3/4X or 3/2X: 6, 3, 8 or 4 MIDI clocks a step (0x257e94). */
static const int mt_clocks[4] = {6, 3, 8, 4};

/* The playhead as the sequencer's clock routine 0x23901e moves it, one MIDI clock at
 * a time (24 a beat; swing is not modelled). A step's routines run from a timer the
 * step sets to its own length, while the step counter runs on its own: it reads the
 * speed again at each step, and on the loop's first step at every clock. A speed
 * changed during that step therefore moves the counter at once but not the timer:
 * step 1 can run twice, or step 2 be skipped or run late. A new length waits for
 * the wrap. */
typedef struct {
   int step;    /* 0x2bc284 */
   int clock;   /* clocks into the step, 0x2bc29c */
   int speed;   /* the speed in use, 0x2bc2a8 */
   int end;     /* the length in use, 0x2bda3e */
   int wait;    /* clocks until the step routines run (0x2bc26c, in clocks) */
} MtSeq;

/* The playhead just before clock c, as if length and speed had been the same since clock 0. */
static void mt_seq_at(MtSeq *q, int64_t c, int length, int speed) {
   const int n = mt_clocks[speed];
   int64_t steps = c / n;
   if (steps * n > c) --steps;
   q->clock = (int)(c - steps * n);
   q->step = (int)(((steps % length) + length) % length);
   q->speed = speed;
   q->end = length;
   q->wait = q->clock ? n - q->clock : 0;
}
/* One clock; length and speed are the pattern's now. Returns the step whose routines run, or -1. */
static int mt_seq_clock(MtSeq *q, int length, int speed) {
   const int run = q->wait < 1 ? q->step : -1;
   if (++q->clock >= mt_clocks[q->speed]) {
      q->clock = 0;
      q->speed = speed;
      if (++q->step >= q->end) { q->step = 0; q->end = length; }
   }
   const int n = mt_clocks[q->speed];
   if (q->step == 0) q->speed = speed;
   if (run >= 0) q->wait = n;
   q->wait--;
   return run;
}

/* Setting the length (0x23d004) to one that reaches a new page fills that page from a
 * lower one when the old length did not reach it and it is empty: page 2 from 1, 3 from
 * 1, 4 from 2 (0-based here). Returns the page to fill and *src the one to copy, or -1. */
static int mt_grow_page(int old, int length, int *src) {
   if (length > 48) { *src = 1; return old <= 48 ? 3 : -1; }
   if (length > 32) { *src = 0; return old <= 32 ? 2 : -1; }
   if (length > 16) { *src = 0; return old <= 16 ? 1 : -1; }
   return -1;
}

/* ---- parameter locks ----------------------------------------------------------
 * A step's locks (0x2313c4): a locked value is sent whenever the playhead reaches
 * its step, trig or no trig. A parameter returns to its kit value only at the
 * next step with an AMP trig that does not lock it. on[page]: the parameters now
 * away from the kit. Writes (id, value) in the firmware's order, value -1 for a
 * return to the kit; returns how many. */
static int mt_step_locks(uint8_t on[7], const int8_t lock[MT_LOCKS], int amp, int *ids, int *values) {
   int n = 0;
   for (int id = 0; id < MT_LOCKS; ++id) {
      const int page = id >> 3, bit = 1 << (id & 7);
      if (lock[id] >= 0) {
         ids[n] = id; values[n++] = lock[id];
         on[page] |= (uint8_t)bit;
      } else if (amp && (on[page] & bit)) {
         ids[n] = id; values[n++] = -1;
         on[page] &= (uint8_t)~bit;
      }
   }
   return n;
}

/* One queued lock, as the frame routine applies it (0x2483e6): straight into the
 * word the DSP gets and the knob's target, with no slew. word: value << 16. */
static void mt_host_lock(MtHost *h, int id, int32_t word) {
   const int page = id >> 3, k = id & 7;
   if (page < 4) {
      h->knob[id] = word;
      h->word[page ? (page - 1) * 8 + k : W_SYN + k] = word;
   } else h->raw[page - 4][k] = word >> 16;
}
/* After a frame's locks: each LFO with a locked parameter is evaluated again (0x248318). */
static void mt_host_locked_lfos(MtHost *h, const int32_t *mod, unsigned lfos) {
   mt_host_shadows(h, mod);
   for (int l = 0; l < 3; ++l)
      if (lfos >> l & 1) mt_lfo_evaluate(h, l, 0, 0);
}

/* ---- POLY's voice allocator (note on 0x2449ba, note off 0x2448e2) ---------------
 * Six (note, voice) slots, newest first: [0, count) sound, the rest are free, the
 * most recently freed first, each keeping the note it last played. */
typedef struct { int note[MT_VOICES], voice[MT_VOICES], count; } MtAlloc;

static void mt_alloc_reset(MtAlloc *a) {
   for (int i = 0; i < MT_VOICES; ++i) { a->note[i] = -1; a->voice[i] = i; }
   a->count = 0;
}
/* The voice for a new note. *stolen: the note it must release first, or -1. */
static int mt_alloc_on(MtAlloc *a, int note, int *stolen) {
   int slot = MT_VOICES - 1;
   *stolen = -1;
   if (a->count == MT_VOICES) *stolen = a->note[slot];
   else {
      for (int i = a->count; i < MT_VOICES; ++i)
         if (a->note[i] == note) { slot = i; break; }
      a->count++;
   }
   const int voice = a->voice[slot];
   memmove(a->note + 1, a->note, (size_t)slot * sizeof *a->note);
   memmove(a->voice + 1, a->voice, (size_t)slot * sizeof *a->voice);
   a->note[0] = note;
   a->voice[0] = voice;
   return voice;
}
/* Frees every slot sounding this note, oldest first; returns how many voices it wrote. */
static int mt_alloc_off(MtAlloc *a, int note, int *voices) {
   int n = 0;
   for (int i = a->count - 1; i >= 0; --i) {
      if (a->note[i] != note) continue;
      const int voice = a->voice[i];
      a->count--;
      memmove(a->note + i, a->note + i + 1, (size_t)(a->count - i) * sizeof *a->note);
      memmove(a->voice + i, a->voice + i + 1, (size_t)(a->count - i) * sizeof *a->voice);
      a->note[a->count] = note;
      a->voice[a->count] = voice;
      voices[n++] = voice;
   }
   return n;
}

#endif
