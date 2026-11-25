/* Mono Track: a whole Monomachine track around each synth machine, as eight
 * CLAP instruments in one bundle.
 *
 * mono_track.h is the track, a fixed-point port of OS 1.32B: machine, EQ,
 * filter + DIST, AMP, SRR, delay, LEVEL, the filter envelope and key tracking,
 * and the main CPU's knob slew and three LFOs. This file adds what surrounds a
 * track in a DAW:
 *  - the notes: one voice, the last note held wins; releasing it returns to
 *    the previous note still held (a new trig), or ends the note. POLY runs six
 *    whole tracks from the firmware's voice allocator;
 *  - a pattern of up to 64 steps on the host's transport, with trigs and
 *    parameter locks, and SCALE SETUP's length and speed;
 *  - a Cocoa editor (mono_track_gui.m);
 *  - every hardware data page as a page of Bitwig remote controls;
 *  - MIDI pitch bend, as KIT ASSIGN JOY R/L would send it (MIRR on, PAGE PTCH,
 *    ADD +63) with DEST as Bend Range;
 *  - sample-rate conversion when the host does not run at 44.1 kHz.
 * SPDX-License-Identifier: GPL-3.0-only */
#include <clap/clap.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "mono_track.h"
#include "mono_track_ui.h"

typedef struct {
   const char *id, *name, *magic;
   const char *label[8];
   unsigned char def[8];
   unsigned unused, bipolar;   /* knobs the machine does not read (hidden); knobs shown as value - 64, as the unit flags them */
} Machine;
static const Machine machines[MT_MACHINES] = {
   {"works.reconstruction.monotrack.sin", "Mono Track GND SIN", "MTSN", {"-", "-", "-", "-", "-", "-", "-", "TUNE"}, {0, 0, 0, 0, 0, 0, 0, 64}, 0x7f, 0x80},
   {"works.reconstruction.monotrack.nois", "Mono Track GND NOIS", "MTNS", {"ST", "RED", "STON", "-", "-", "-", "-", "TUNE"}, {0, 0, 0, 0, 0, 0, 0, 64}, 0x78, 0x80},
   {"works.reconstruction.monotrack.saw", "Mono Track SWAVE SAW", "MTSW", {"UNIL", "UNIW", "UNIX", "-", "SUBX", "SUB1", "SUB2", "TUNE"}, {0, 0, 0, 0, 0, 0, 0, 64}, 0x08, 0x80},
   {"works.reconstruction.monotrack.puls", "Mono Track SWAVE PULS", "MTPL", {"UNIL", "UNIW", "SUB1", "SUB2", "PW", "PWAD", "PWRS", "TUNE"}, {0, 0, 0, 0, 64, 0, 0, 64}, 0, 0x90},
   {"works.reconstruction.monotrack.ens", "Mono Track SWAVE ENS", "MTEN", {"PCH2", "PCH3", "PCH4", "WAVE", "PW", "CHRL", "CHRW", "TUNE"}, {63, 63, 63, 0, 64, 0, 127, 64}, 0, 0x90},
   {"works.reconstruction.monotrack.fmstat", "Mono Track FM+ STAT", "MTFS", {"1FRQ", "1FIN", "1ENV", "1FB", "2FRQ", "2VOL", "TONE", "TUNE"}, {60, 64, 80, 30, 80, 64, 98, 64}, 0, 0x82},
   {"works.reconstruction.monotrack.fmpar", "Mono Track FM+ PAR", "MTFP", {"1FRQ", "1ENV", "2FRQ", "2ENV", "3FRQ", "3ENV", "TONE", "TUNE"}, {60, 64, 80, 64, 102, 80, 98, 64}, 0, 0x80},
   {"works.reconstruction.monotrack.fmdyn", "Mono Track FM+ DYN", "MTFD", {"1FRQ", "1FEN", "1VOL", "1VEN", "2FRQ", "2ENV", "2FB", "TUNE"}, {64, 64, 64, 64, 74, 80, 30, 64}, 0, 0x8a},
};
static const char *const page_name[4] = {"SYN", "AMP", "FILT", "EFX"};
static const char *const knob_label[3][8] = {
   {"ATK", "HOLD", "DEC", "REL", "DIST", "VOL", "PAN", "PORT"},
   {"BASE", "WDTH", "HPQ", "LPQ", "ATK", "DEC", "BOFS", "WOFS"},
   {"EQF", "EQG", "SRR", "DTIM", "DSND", "DFB", "DBAS", "DWID"},
};
static const unsigned char knob_def[3][8] = {{0, 0, 64, 64, 64, 64, 64, 0}, {0, 127, 0, 0, 0, 32, 64, 64}, {64, 64, 0, 64, 64, 28, 0, 127}};
static const unsigned char knob_bipolar[3] = {0x50, 0xc0, 0x12};   /* DIST PAN; BOFS WOFS; EQG DSND */
static const char *const lfo_label[8] = {"PAGE", "DEST", "TRIG", "WAVE", "MULT", "SPD", "INTL", "DPTH"};
/* PAGE DEST TRIG WAVE MULT are lists: the knob holds the entry, sent as the first raw value that selects it */
static const unsigned char lfo_def[8] = {0, 4, 0, 0, 0, 64, 0, 0};
static const unsigned char lfo_size[8] = {9, 8, 5, 11, 7, 0, 0, 0};
static const char *const lfo_page[9] = {"PTCH", "SYNT", "AMP", "FILT", "EFFX", "LFO1", "LFO2", "LFO3", "MIDI"};
static const char *const lfo_trig[5] = {"FREE", "TRIG", "HOLD", "ONE", "HALF"};
static const char *const lfo_wave[11] = {"TRI", "ITRI", "SAW", "ISAW", "SQR", "ISQR", "EXP", "IEXP", "RMP", "IRMP", "RND"};
static const char *const lfo_mult[7] = {"1X", "2X", "4X", "8X", "16X", "32X", "64X"};
static const char *const midi_label[8] = {"LEN", "VEL", "PB", "PCHG", "CC 1", "CC 2", "CC 3", "CC 4"};
static const char *const range_name[8] = {"1/12", "2/12", "7/12", "1OCT", "2OCT", "4OCT", "8OCT", "16OC"};
static const char *const track_label[10] = {"LEVEL", "LP KEY", "HP KEY", "BEND", "MASTER TUNE", "POLY", "MIDI TRIG SYN", "MIDI TRIG FILT", "MIDI TRIG LFO", "NOTE ENDS LOCK"};
/* KIT ASSIGN's KEY VEL and KEY TRK rows as a cleared kit has them (0x251aa0): VEL AMP VOL, KEY SYNT PAR1, then PTCH 1/12, all ADD 0 */
static const char *const assign_field[3] = {"PAGE", "DEST", "ADD"};
static const unsigned char assign_def[12] = {2, 5, 64, 0, 0, 64, 1, 0, 64, 0, 0, 64};

static const char *const features[] = {
   CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, CLAP_PLUGIN_FEATURE_STEREO, NULL
};
#define DESCRIPTOR(m) { .clap_version = CLAP_VERSION_INIT, .id = machines[m].id, .name = machines[m].name, \
   .vendor = "Independent DSP", .url = "", .manual_url = "", .support_url = "", .version = "0.6.0", \
   .description = "A whole Monomachine track: machine, EQ, filter, AMP, SRR, delay, LFOs, ported from its code", .features = features }
static const clap_plugin_descriptor_t descriptors[MT_MACHINES] = {
   DESCRIPTOR(0), DESCRIPTOR(1), DESCRIPTOR(2), DESCRIPTOR(3), DESCRIPTOR(4), DESCRIPTOR(5), DESCRIPTOR(6), DESCRIPTOR(7),
};

#define NATIVE 44100.0
#define HISTORY 4096        /* resampler input ring */
#define PHASES 128
#define NOIS_START 2048     /* blocks: NOIS's red integrator filling */
#define ENS_START 64        /* blocks: ENS's chorus unmuting */

static double clamp(double x, double lo, double hi) { return x < lo ? lo : x > hi ? hi : x; }
static double def_of(int m, int id) {
   if (id < P_AMP) return machines[m].def[id];
   if (id < P_LFO) return knob_def[id / 8 - 1][id % 8];
   if (id < P_LEVEL) return lfo_def[(id - P_LFO) % 8];
   if (id >= P_ASSIGN) return assign_def[id - P_ASSIGN];
   return id == P_LEVEL ? 100 : id == P_MTUNE ? 440 : id == P_POLY ? 0 : 1;
}
static bool is_switch(int id) { return id == P_LPKEY || id == P_HPKEY || (id >= P_POLY && id < P_ASSIGN); }
static double lo_of(int id) { return id == P_MTUNE ? 400 : 0; }
static double hi_of(int id) {
   if (id >= P_LFO && id < P_LEVEL && lfo_size[(id - P_LFO) % 8]) return lfo_size[(id - P_LFO) % 8] - 1;
   if (id >= P_ASSIGN) return (int[]){8, 7, 127}[(id - P_ASSIGN) % 3];   /* PAGE, DEST, ADD + 64 */
   return id == P_MTUNE ? 499.9 : id == P_BEND ? 7 : is_switch(id) ? 1 : 127;
}
/* an LFO parameter's value as the kit byte the OS reads */
static int lfo_raw(int field, double v) {
   const int n = lfo_size[field], k = (int)lrint(v);
   return n ? (k * 128 + n - 1) / n : k;
}
static bool stepped(int id) { return (id >= P_LFO && id < P_LEVEL) || is_switch(id) || id == P_BEND || id >= P_ASSIGN; }
/* The list fields: an LFO's, and ASSIGN's PAGE and DEST, which read as an LFO's do. -1 for the others. */
static int list_field(int id) {
   if (id >= P_LFO && id < P_LEVEL) return (id - P_LFO) % 8;
   if (id >= P_ASSIGN && (id - P_ASSIGN) % 3 < 2) return (id - P_ASSIGN) % 3;
   return -1;
}

/* ---- sample-rate conversion (windowed sinc, as in Mono Delay) -------------- */

typedef struct { double x[2]; } Frame;
typedef struct {
   float *kernel;
   Frame history[HISTORY];
   double ratio;   /* output n is the input at n x ratio */
   int half, taps;
   int64_t last;
   uint64_t index;
} Resampler;

static bool rs_prepare(Resampler *r, double from, double to) {
   r->ratio = from / to;
   r->half = (int)ceil(40.0 * fmax(1.0, r->ratio));
   r->taps = 2 * r->half;
   if (r->taps + 2 >= HISTORY) return false;
   r->kernel = calloc((size_t)(PHASES + 1) * r->taps, sizeof(float));
   if (!r->kernel) return false;
   const double cutoff = 0.95 * fmin(1.0, to / from), pi = 3.14159265358979323846;
   for (int p = 0; p <= PHASES; ++p) {
      float *k = r->kernel + (size_t)p * r->taps;
      double sum = 0.0;
      for (int i = 0; i < r->taps; ++i) {
         const double d = (double)(i - r->half + 1) - (double)p / PHASES, x = d / r->half, u = pi * cutoff * d;
         const double window = fabs(x) <= 1.0 ? 0.42 + 0.5 * cos(pi * x) + 0.08 * cos(2.0 * pi * x) : 0.0;
         k[i] = (float)(cutoff * (fabs(u) < 1e-14 ? 1.0 : sin(u) / u) * window);
         sum += k[i];
      }
      for (int i = 0; i < r->taps; ++i) k[i] /= (float)sum;
   }
   return true;
}
static void rs_reset(Resampler *r) { memset(r->history, 0, sizeof r->history); r->last = -1; r->index = 0; }
static void rs_push(Resampler *r, Frame f) { r->history[(size_t)(++r->last) & (HISTORY - 1)] = f; }
static bool rs_ready(const Resampler *r) { return (int64_t)floor((double)r->index * r->ratio) + r->half <= r->last; }
static Frame rs_pop(Resampler *r) {
   const double pos = (double)r->index * r->ratio, pp = (pos - floor(pos)) * PHASES;
   r->index++;
   const int64_t base = (int64_t)floor(pos);
   const int phase = (int)fmin(PHASES - 1, (int)pp);
   const float *a = r->kernel + (size_t)phase * r->taps, *b = a + r->taps;
   Frame out = {{0, 0}};
   for (int i = 0; i < r->taps; ++i) {
      const int64_t at = base - r->half + 1 + i;
      if (at < 0) continue;
      const Frame *f = &r->history[(size_t)at & (HISTORY - 1)];
      const double w = a[i] + (b[i] - a[i]) * (pp - phase);
      for (int c = 0; c < 2; ++c) out.x[c] += f->x[c] * w;
   }
   return out;
}

/* ---- plug-in --------------------------------------------------------------- */

/* One DSP track. Mono uses the first; POLY all six. */
enum { PEND_ON = 1, PEND_OFF = 2, PEND_FILT = 4, PEND_LFO = 8 };   /* ON/OFF share the firmware's AMP cell: the last wins */
typedef struct {
   MtHost cpu;
   MonoTrack track;
   bool settled, quick, reload;       /* quick: assigned by POLY, so the machine starts from its init */
   bool gate;                         /* a MIDI note started the AMP, so its release ends it */
   /* the keys held, in the order pressed; the note sounding; the frame's news */
   int held[128], nheld, note;
   int vel, key;                      /* KIT ASSIGN's controllers: the last note-on's velocity, the note sounding (0 until one) */
   unsigned pending;
   int32_t y[2][MT_FRAMES];
} Voice;

enum { UI_WANT = 1, UI_TOUCH = 2, UI_DIRTY = 4 };   /* the editor's gesture open; begun; a new value */
#define SEQ_STOPPED INT64_MIN
enum { QUEUE_NONE = -2, QUEUE_KIT = -1 };
/* In samples. A step starts this early, so its locks are in before a note on the same beat. */
#define SEQ_LEAD 2.0
/* In samples. A host position this far behind the last one is rounding, not a jump back. */
#define SEQ_SLACK 32.0

typedef struct {
   clap_plugin_t plugin;
   const clap_host_t *host;
   int machine;
   _Atomic uint64_t value[NPARAM];    /* parameter values (double bits): host thread and audio thread */
   _Atomic bool text_dirty;           /* an LFO's PAGE changed: its DEST reads differently */
   _Atomic bool pattern_loaded;       /* new state: the sounding locks are stale */
   _Atomic uint8_t ui_flag[NPARAM];   /* editor -> host: see UI_* */
   bool ui_open[NPARAM];              /* gestures the host has been told about */
   MtUi ui;
   void *gui;
   uint32_t gui_w, gui_h;
   double mod[NPARAM];
   /* audio thread */
   bool active, direct, poly;
   uint32_t latency, blocks;
   int pos;
   int bend;                          /* the pitch bend's top 8 bits, 128 at the centre */
   double tempo, rate;
   MtAlloc alloc;
   bool poly_held[128];               /* POLY: pitches held through MIDI in (0x2bc018) */
   int64_t seq_clock;                 /* the MIDI clock (24 a beat) the playhead is in, SEQ_STOPPED when stopped */
   MtSeq seq;
   int seq_step;                      /* the step the playhead is in */
   uint8_t lock_on[7];                /* parameters away from the kit (0x2bc7c0) */
   int8_t queue[MT_LOCKS];            /* the next frame's locks: a value, QUEUE_KIT or QUEUE_NONE */
   bool queued;
   Voice voice[MT_VOICES];
   double out[2][MT_FRAMES];
   Resampler down;
} Plugin;

#ifdef MT_CHECK   /* the lab's firmware comparison sees every block */
void (*mt_check_hook)(void *track, const int32_t *words, const int32_t out[2][MT_FRAMES]);
#endif

static double bits_to(uint64_t u) { double x; memcpy(&x, &u, sizeof x); return x; }
static uint64_t to_bits(double x) { uint64_t u; memcpy(&u, &x, sizeof u); return u; }
static double value(const Plugin *s, int id) { return bits_to(atomic_load_explicit(&s->value[id], memory_order_relaxed)); }
static void set_value(Plugin *s, int id, double x) { atomic_store_explicit(&s->value[id], to_bits(x), memory_order_relaxed); }

/* Block word 42 for a tempo, as the CPU sends it: 24 x BPM, the tempo kept to 30..300. */
static int32_t tick_of(double bpm) { return (int32_t)clamp(lround(24.0 * bpm), 720, 7200); }

/* The machine's own start, which on the unit is long over by the first note. */
static void machine_start(Plugin *s, Voice *v) {
#ifndef MT_NO_START
   MonoTrack *t = &v->track;
   const int32_t *syn = v->cpu.block + W_SYN;
   int32_t x[2][MT_FRAMES] = {{0}};
   if (s->machine == MT_NOIS)
      for (int n = 0; n < NOIS_START; ++n) mg_block(&t->gnd, syn, 0, x);
   else if (s->machine == MT_ENS) {
      int32_t pan[2];
      ms_pan(v->cpu.word[W_PAN], pan);
      for (int n = 0; n < ENS_START; ++n) {
         memset(x, 0, sizeof x);
         ms_chorus(&t->sw, syn, pan, x);
      }
   }
#else
   (void)s; (void)v;
#endif
}

static double active_or(const Plugin *s, int id) {
   const int a = id < MT_LOCKS ? atomic_load_explicit(&s->ui.active[id], memory_order_relaxed) : -1;
   return a >= 0 ? a : value(s, id);
}
/* A lockable parameter's word: value << 16, an LFO list entry as its kit byte. */
static int32_t lock_word(int id, double v) {
   return id >= P_LFO ? lfo_raw((id - P_LFO) % 8, v) * 65536 : (int32_t)lrint(clamp(v, 0, 127) * 65536.0);
}

/* What the editor draws by each LFO tab. */
static void publish_lfos(Plugin *s, const MtHost *h) {
   for (int l = 0; l < 3; ++l) {
      const MtLfo *o = &h->lfo[l];
      const int wave = mt_list(h->shadow[l][L_WAVE], 11), depth = mt_hi16(h->shadow[l][L_DPTH]);
      atomic_store_explicit(&s->ui.lfo_pos[l], o->pos, memory_order_relaxed);
      atomic_store_explicit(&s->ui.lfo_inc[l], o->inc, memory_order_relaxed);
      atomic_store_explicit(&s->ui.lfo_wave[l], (int8_t)wave, memory_order_relaxed);
      atomic_store_explicit(&s->ui.lfo_depth[l], (int8_t)(depth < 0 ? 0 : depth > 127 ? 127 : depth), memory_order_relaxed);
      if (wave == 10 && o->pos != 0x400000 && o->pos != 0x800000)   /* not on an edge, where RND reads 0 */
         atomic_store_explicit(&s->ui.lfo_rnd[l][o->pos >> 20 & 7], (int8_t)(o->out >> 16), memory_order_relaxed);
   }
}

/* Start of a control frame (three blocks): the words the main CPU sends. */
static void control_frame(Plugin *s, Voice *v) {
   MtHost *h = &v->cpu;
   int32_t mod[MT_KNOBS + 24];
   bool any = false;
   for (int p = 0; p < P_LEVEL; ++p) {
      const int n = p >= P_LFO ? lfo_size[(p - P_LFO) % 8] : 0;   /* a list's step is 128 / n raw values */
      mod[p] = s->mod[p] == 0 ? 0 : (int32_t)lrint(s->mod[p] * (n ? 128.0 / n : 1.0) * 65536.0);
      any |= mod[p] != 0;
   }
   for (int p = 0; p < MT_KNOBS; ++p) h->knob[p] = (int32_t)lrint(clamp(active_or(s, p), 0, 127) * 65536.0);
   for (int l = 0; l < 3; ++l)
      for (int k = 0; k < 8; ++k) h->raw[l][k] = lfo_raw(k, active_or(s, P_LFO + 8 * l + k));
   h->level = (int32_t)lrint(clamp(value(s, P_LEVEL) + s->mod[P_LEVEL], 0, 127) * 65536.0);
   h->bend = mf_bend(s->bend, (int)lrint(value(s, P_BEND)));
   for (int i = 0; i < 4; ++i) {   /* VEL 1, KEY 1, VEL 2, KEY 2 */
      const int id = P_ASSIGN + 6 * (i & 1) + 3 * (i >> 1), page = (int)lrint(value(s, id)), dest = (int)lrint(value(s, id + 1));
      h->assign[i] = (MtAssign){page, dest, mt_assign_value(i & 1 ? v->key : v->vel, page, dest, (int)lrint(value(s, id + 2)) - 64)};
   }
   const bool start = !v->settled;
   if (start) mt_host_reset(h);
   const int32_t tick = tick_of(s->tempo);
   h->word[W_TICK] = tick;
   h->word[W_RECIP] = 0x800000 / tick;
   h->word[W_PITCH] = (v->note << 11) / 12;
   h->word[W_MTUNE] = mf_master_tune((int)lrint(clamp(value(s, P_MTUNE) * 10, 4000, 4999)));
   h->word[W_ROUTE] = 1 | (value(s, P_LPKEY) >= 0.5 ? MT_LP_KEY : 0) | (value(s, P_HPKEY) >= 0.5 ? MT_HP_KEY : 0);
   const unsigned news = v->pending;
   h->word[W_NOTE] = news & PEND_ON ? 0x81 : news & PEND_OFF ? 0x02 : 0;
   h->word[W_FTRIG] = (news & PEND_FILT) != 0;
   if (news & PEND_LFO) h->lfo_trig = 1;
   v->pending = 0;
   if (v->reload && (news & PEND_ON)) {   /* POLY: the voice takes the sound at its next note, with no slew (0x24b998) */
      v->reload = false;
      for (int k = 0; k < MT_KNOBS; ++k) h->word[k < 8 ? W_SYN + k : k - 8] = h->knob[k];
      h->word[W_LEVEL] = h->level;
   }
   if (start) {   /* the machine's assignment: its init and trig routines, the glide at the note, the AMP idle */
      mt_reset(&v->track, s->machine, h->word[W_PITCH]);
      md_settle(&v->track.delay, h->knob[24 + 3], (uint32_t)tick);
      h->word[W_NOTE] |= 0x80;
   }
   unsigned lfos = 0;
   if (s->queued)
      for (int id = 0; id < MT_LOCKS; ++id) {
         if (s->queue[id] == QUEUE_NONE) continue;
         mt_host_lock(h, id, lock_word(id, s->queue[id] >= 0 ? s->queue[id] : value(s, id)));
         if (id >= P_LFO) lfos |= 1u << ((id - P_LFO) / 8);
      }
   if (lfos) mt_host_locked_lfos(h, any ? mod : NULL, lfos);
   mt_host_frame(h, any ? mod : NULL);
   if (v == &s->voice[s->poly ? s->alloc.voice[0] : 0]) publish_lfos(s, h);
   h->word[W_NOTE] = h->word[W_FTRIG] = 0;
   if (start && !v->quick) machine_start(s, v);
   v->quick = false;
   v->settled = true;
}

/* POLY on or off (0x24a342): every held note is released; on, the voices take the track's machine and sound. */
static void poly_sync(Plugin *s) {
   const bool on = value(s, P_POLY) >= 0.5;
   if (on == s->poly) return;
   s->poly = on;
   mt_alloc_reset(&s->alloc);
   memset(s->poly_held, 0, sizeof s->poly_held);
   for (int i = 0; i < MT_VOICES; ++i) {
      Voice *v = &s->voice[i];
      v->nheld = 0;
      v->gate = false;
      if (i == 0) { v->pending = PEND_OFF; v->reload = on; continue; }
      v->pending = 0;
      v->note = s->voice[0].note;
      v->vel = s->voice[0].vel;   /* each voice takes the poly track's controllers (0x242058) */
      v->key = s->voice[0].key;
      v->settled = false;
      v->quick = true;
      v->reload = false;
   }
}

/* One frame at 44.1 kHz; a block is computed at its first frame. */
static void native_frame(Plugin *s, double out[2]) {
   if (s->pos == 0) {
      const bool frame_start = s->blocks % 3 == 0;
      if (frame_start) poly_sync(s);
      const int voices = s->poly ? MT_VOICES : 1;
      s->blocks++;
      for (int n = 0; n < voices; ++n) {
         Voice *v = &s->voice[n];
         if (frame_start) control_frame(s, v);
         mt_block(&v->track, v->cpu.block, v->y);
#ifdef MT_CHECK
         if (mt_check_hook) mt_check_hook(&v->track, v->cpu.block, (const int32_t (*)[MT_FRAMES])v->y);
#endif
         v->cpu.block[W_NOTE] = v->cpu.block[W_FTRIG] = 0;   /* one-shots: only the frame's first block carries them */
      }
      if (frame_start && s->queued) { memset(s->queue, QUEUE_NONE, sizeof s->queue); s->queued = false; }
      for (int c = 0; c < 2; ++c)
         for (int i = 0; i < MT_FRAMES; ++i) {
            int32_t sum = 0;   /* the bus mix: every running sum goes through the 24-bit limiter */
            for (int n = 0; n < voices; ++n) {
               sum += s->voice[n].y[c][i];
               sum = sum > 0x7fffff ? 0x7fffff : sum < -0x800000 ? -0x800000 : sum;
            }
            s->out[c][i] = sum / 8388608.0;
         }
   }
   out[0] = s->out[0][s->pos];
   out[1] = s->out[1][s->pos];
   s->pos = (s->pos + 1) % MT_FRAMES;
}

static void host_frame(Plugin *s, double out[2]) {
   if (s->direct) { native_frame(s, out); return; }
   while (!rs_ready(&s->down)) {
      Frame f;
      native_frame(s, f.x);
      rs_push(&s->down, f);
   }
   const Frame f = rs_pop(&s->down);
   out[0] = f.x[0];
   out[1] = f.x[1];
}

static void seq_clear(Plugin *s) {
   for (int i = 0; i < MT_LOCKS; ++i) atomic_store_explicit(&s->ui.active[i], -1, memory_order_relaxed);
   memset(s->lock_on, 0, sizeof s->lock_on);
   memset(s->queue, QUEUE_NONE, sizeof s->queue);
   s->queued = false;
}

static void reset(Plugin *s) {
   for (int i = 0; i < MT_VOICES; ++i) {
      Voice *v = &s->voice[i];
      v->settled = v->quick = v->reload = v->gate = false;
      v->nheld = 0;
      v->note = 60;
      v->vel = v->key = 0;
      v->pending = 0;
   }
   s->poly = value(s, P_POLY) >= 0.5;
   mt_alloc_reset(&s->alloc);
   memset(s->poly_held, 0, sizeof s->poly_held);
   s->blocks = 0;
   s->pos = 0;
   s->bend = 128;
   s->seq_clock = SEQ_STOPPED;
   seq_clear(s);
   atomic_store_explicit(&s->ui.step, -1, memory_order_relaxed);
   rs_reset(&s->down);
}

/* What a MIDI note trigs: the three MIDI TRIG switches. */
static unsigned midi_trigs(const Plugin *s) {
   return (value(s, P_TRIG_SYN) >= 0.5 ? PEND_ON : 0u) | (value(s, P_TRIG_FILT) >= 0.5 ? PEND_FILT : 0u)
        | (value(s, P_TRIG_LFO) >= 0.5 ? PEND_LFO : 0u);
}
static void voice_trig(Voice *v, unsigned t) {
   if (t & PEND_ON) v->pending &= ~(unsigned)PEND_OFF;
   v->pending |= t;
}
/* A key's release ends only a sound a key started: with MIDI TRIG SYN off, the trigs' sounds stay. */
static void voice_release(Voice *v) {
   if (v->gate) v->pending = (v->pending & ~(unsigned)PEND_ON) | PEND_OFF;
   v->gate = false;
}
/* A note-on also sets KIT ASSIGN's KEY and VEL (0x246060). */
static void voice_on(Plugin *s, Voice *v, int key, int vel) {
   for (int i = 0; i < v->nheld; ++i)
      if (v->held[i] == key) { memmove(v->held + i, v->held + i + 1, (size_t)(--v->nheld - i) * sizeof *v->held); break; }
   v->held[v->nheld++] = key;
   v->note = v->key = key;
   v->vel = vel;
   const unsigned t = midi_trigs(s);
   if (t & PEND_ON) v->gate = true;
   voice_trig(v, t);
}
static void voice_off(Plugin *s, Voice *v, int key) {
   for (int i = 0; i < v->nheld; ++i)
      if (v->held[i] == key) {
         memmove(v->held + i, v->held + i + 1, (size_t)(--v->nheld - i) * sizeof *v->held);
         if (key != v->note) return;
         if (v->nheld) { v->note = v->key = v->held[v->nheld - 1]; voice_trig(v, midi_trigs(s)); }   /* KEY follows (0x24b6f6) */
         else voice_release(v);
         return;
      }
}
static void seq_note(Plugin *s);
static void note_on(Plugin *s, int key, int vel) {
   seq_note(s);
   if (!s->poly) { voice_on(s, &s->voice[0], key, vel); return; }
   if (s->poly_held[key]) return;   /* a repeat with no note-off */
   s->poly_held[key] = true;
   int stolen;
   Voice *v = &s->voice[mt_alloc_on(&s->alloc, key, &stolen)];
   if (stolen >= 0) voice_off(s, v, stolen);
   voice_on(s, v, key, vel);
}
static void note_off(Plugin *s, int key) {
   if (!s->poly) { voice_off(s, &s->voice[0], key); return; }
   if (!s->poly_held[key]) return;
   s->poly_held[key] = false;
   int freed[MT_VOICES];
   const int n = mt_alloc_off(&s->alloc, key, freed);
   for (int i = 0; i < n; ++i) voice_off(s, &s->voice[freed[i]], key);
}

/* ---- the pattern: up to 64 steps --------------------------------------------- */

static int seq_length(const Plugin *s) { return atomic_load_explicit(&s->ui.length, memory_order_relaxed); }
static int seq_speed(const Plugin *s) { return atomic_load_explicit(&s->ui.speed, memory_order_relaxed); }

/* Queues what mt_step_locks found. only_returns: leave the step's own locks alone. */
static void seq_apply(Plugin *s, int step, bool trig, bool only_returns) {
   int8_t lock[MT_LOCKS];
   int ids[MT_LOCKS], values[MT_LOCKS];
   for (int i = 0; i < MT_LOCKS; ++i) lock[i] = atomic_load_explicit(&s->ui.lock[step][i], memory_order_relaxed);
   const int n = mt_step_locks(s->lock_on, lock, trig, ids, values);
   for (int i = 0; i < n; ++i) {
      if (only_returns && values[i] >= 0) continue;
      s->queue[ids[i]] = (int8_t)(values[i] >= 0 ? values[i] : QUEUE_KIT);
      s->queued = true;
      atomic_store_explicit(&s->ui.active[ids[i]], (int8_t)values[i], memory_order_relaxed);
   }
}

/* The playhead reaches a step. A trig plays no note, since a step has no pitch: it
 * restarts the filter envelope and the LFOs and ends locks. In POLY those trigs
 * reach only the poly track's own voice (0x24bb50, CC 99 = 0x7f), voice 0 here. */
static void seq_step(Plugin *s, int step) {
   const bool trig = atomic_load_explicit(&s->ui.trig, memory_order_relaxed) >> step & 1;
   seq_apply(s, step, trig, false);
   if (trig) voice_trig(&s->voice[0], PEND_FILT | PEND_LFO);
   s->seq_step = step;
   atomic_store_explicit(&s->ui.step, step, memory_order_relaxed);
}
/* pos, frame: the playhead and one sample, in MIDI clocks. Each clock it enters runs
 * the firmware's clock routine (mt_seq_clock). A start or a jump puts it where the
 * pattern would be had it played from the start of the song at its present length
 * and speed; it then waits for the next step to begin. */
static void seq_advance(Plugin *s, double pos, double frame) {
   pos += SEQ_LEAD * frame;
   const int64_t c = (int64_t)floor(pos);
   const bool running = s->seq_clock != SEQ_STOPPED;
   if (running && c == s->seq_clock) return;
   if (running && c == s->seq_clock - 1 && (double)s->seq_clock - pos < SEQ_SLACK * frame) return;
   if (!running || c != s->seq_clock + 1) {
      mt_seq_at(&s->seq, c, seq_length(s), seq_speed(s));
      s->seq_step = s->seq.step;
   }
   s->seq_clock = c;
   const int step = mt_seq_clock(&s->seq, seq_length(s), seq_speed(s));
   if (step >= 0) seq_step(s, step);
}
/* A MIDI note on the running pattern: shown under its step, and by default it ends
 * every lock except the ones its own step holds. */
static void seq_note(Plugin *s) {
   if (s->seq_clock == SEQ_STOPPED) return;
   const int step = s->seq_step;
   atomic_fetch_add_explicit(&s->ui.notes[step], 1, memory_order_relaxed);
   if (value(s, P_LOCK_END) >= 0.5) seq_apply(s, step, true, true);
}
/* Stop (0x2358e6): every locked parameter returns to the kit. */
static void seq_stop(Plugin *s) {
   for (int id = 0; id < MT_LOCKS; ++id)
      if (s->lock_on[id >> 3] >> (id & 7) & 1) {
         s->queue[id] = QUEUE_KIT;
         s->queued = true;
         atomic_store_explicit(&s->ui.active[id], -1, memory_order_relaxed);
      }
   memset(s->lock_on, 0, sizeof s->lock_on);
   s->seq_clock = SEQ_STOPPED;
   atomic_store_explicit(&s->ui.step, -1, memory_order_relaxed);
}

static bool plug_init(const clap_plugin_t *p) { (void)p; return true; }
static void plug_destroy(const clap_plugin_t *p) {
   Plugin *s = p->plugin_data;
#ifndef MT_CHECK
   if (s->gui) mt_gui_destroy(s->gui);
#endif
   free(s->down.kernel);
   free(s);
}
static bool plug_activate(const clap_plugin_t *p, double rate, uint32_t min, uint32_t max) {
   (void)min; (void)max;
   Plugin *s = p->plugin_data;
   if (s->active || !isfinite(rate) || rate < 8000 || rate > 768000) return false;
   s->direct = rate == NATIVE;
   s->rate = rate;
   uint32_t latency = 0;
   if (!s->direct) {
      if (!rs_prepare(&s->down, NATIVE, rate)) { free(s->down.kernel); s->down.kernel = NULL; return false; }
      latency = (uint32_t)lround(s->down.half * rate / NATIVE);   /* the resampler reads that far ahead */
   }
   reset(s);
   s->active = true;
   if (latency != s->latency) {
      s->latency = latency;
      const clap_host_latency_t *hl = s->host->get_extension ? s->host->get_extension(s->host, CLAP_EXT_LATENCY) : NULL;
      if (hl && hl->changed) hl->changed(s->host);
   }
   return true;
}
static void plug_deactivate(const clap_plugin_t *p) {
   Plugin *s = p->plugin_data;
   free(s->down.kernel);
   s->down.kernel = NULL;
   s->active = false;
}
static bool plug_start(const clap_plugin_t *p) { (void)p; return true; }
static void plug_stop(const clap_plugin_t *p) { (void)p; }
static void plug_reset(const clap_plugin_t *p) { reset(p->plugin_data); }
static void plug_on_main(const clap_plugin_t *p) {
   Plugin *s = p->plugin_data;
   if (!atomic_exchange(&s->text_dirty, false)) return;
   const clap_host_params_t *hp = s->host->get_extension ? s->host->get_extension(s->host, CLAP_EXT_PARAMS) : NULL;
   if (hp && hp->rescan) hp->rescan(s->host, CLAP_PARAM_RESCAN_TEXT);
}

static void event(Plugin *s, const clap_event_header_t *h) {
   if (!h || h->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
   if (h->type == CLAP_EVENT_PARAM_VALUE && h->size >= sizeof(clap_event_param_value_t)) {
      const clap_event_param_value_t *e = (const void *)h;
      if (e->param_id < NPARAM && e->note_id == -1 && e->port_index == -1 && e->channel == -1 && e->key == -1 && isfinite(e->value)) {
         const int id = (int)e->param_id;
         const double x = clamp(e->value, lo_of(id), hi_of(id));
         if (id < MT_LOCKS && x != value(s, id)) atomic_store_explicit(&s->ui.active[id], -1, memory_order_relaxed);   /* a new value replaces a sounding lock, as a knob does on the unit */
         set_value(s, id, x);
         if (list_field(id) == L_PAGE) {
            atomic_store(&s->text_dirty, true);
            if (s->host->request_callback) s->host->request_callback(s->host);
         }
      }
   } else if (h->type == CLAP_EVENT_PARAM_MOD && h->size >= sizeof(clap_event_param_mod_t)) {
      const clap_event_param_mod_t *e = (const void *)h;
      if (e->param_id <= P_LEVEL && e->note_id == -1 && e->port_index == -1 && e->channel == -1 && e->key == -1 && isfinite(e->amount))
      {
         s->mod[e->param_id] = clamp(e->amount, -127, 127);
         atomic_store_explicit(&s->ui.mod[e->param_id], (float)s->mod[e->param_id], memory_order_relaxed);
      }
   } else if ((h->type == CLAP_EVENT_NOTE_ON || h->type == CLAP_EVENT_NOTE_OFF) && h->size >= sizeof(clap_event_note_t)) {
      const clap_event_note_t *e = (const void *)h;
      if (e->key >= 0 && e->key < 128) {
         if (h->type == CLAP_EVENT_NOTE_OFF) note_off(s, e->key);
         else note_on(s, e->key, (int)lrint(clamp(isfinite(e->velocity) ? e->velocity : 0, 0, 1) * 127));
      }
   } else if (h->type == CLAP_EVENT_MIDI && h->size >= sizeof(clap_event_midi_t)) {
      const clap_event_midi_t *e = (const void *)h;
      const int status = e->data[0] & 0xf0, key = e->data[1] & 0x7f;
      if (status == 0x90 && e->data[2] > 0) note_on(s, key, e->data[2] & 0x7f);
      else if (status == 0x80 || status == 0x90) note_off(s, key);
      else if (status == 0xe0) s->bend = (e->data[2] & 0x7f) * 2 + ((e->data[1] & 0x7f) >> 6);
   }
}

/* The editor's edits to the host: begin, the latest value, end. Nothing is lost
 * when the host's queue is full: the flags stay set for the next try. */
static bool push_gesture(const clap_output_events_t *out, uint16_t type, int id) {
   const clap_event_param_gesture_t e = {{sizeof e, 0, CLAP_CORE_EVENT_SPACE_ID, type, 0}, (clap_id)id};
   return out->try_push(out, &e.header);
}
static void ui_flush(Plugin *s, const clap_output_events_t *out) {
   if (!out || !out->try_push) return;
   for (int i = 0; i < NPARAM; ++i) {
      const unsigned seen = atomic_load_explicit(&s->ui_flag[i], memory_order_acquire);
      if (!(seen & (UI_TOUCH | UI_DIRTY)) && ((seen & UI_WANT) != 0) == s->ui_open[i]) continue;
      const unsigned f = atomic_fetch_and_explicit(&s->ui_flag[i], (uint8_t)UI_WANT, memory_order_acq_rel);
      bool ok = true;
      if (!s->ui_open[i]) {
         ok = push_gesture(out, CLAP_EVENT_PARAM_GESTURE_BEGIN, i);
         s->ui_open[i] = ok;
      }
      if (ok && (f & UI_DIRTY)) {
         const clap_event_param_value_t e = {{sizeof e, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0}, (clap_id)i, NULL, -1, -1, -1, -1, value(s, i)};
         ok = out->try_push(out, &e.header);
      }
      if (ok && !(f & UI_WANT)) {
         ok = push_gesture(out, CLAP_EVENT_PARAM_GESTURE_END, i);
         if (ok) s->ui_open[i] = false;
      }
      if (!ok) {
         atomic_fetch_or_explicit(&s->ui_flag[i], (uint8_t)(f & (UI_TOUCH | UI_DIRTY)), memory_order_acq_rel);
         return;
      }
   }
}

static clap_process_status plug_process(const clap_plugin_t *p, const clap_process_t *pr) {
   Plugin *s = p->plugin_data;
   if (!s->active || !pr || pr->audio_outputs_count < 1) return CLAP_PROCESS_ERROR;
   clap_audio_buffer_t *out = &pr->audio_outputs[0];
   if (out->channel_count < 2 || (!out->data32 && !out->data64)) return CLAP_PROCESS_ERROR;
   s->tempo = pr->transport && (pr->transport->flags & CLAP_TRANSPORT_HAS_TEMPO) && isfinite(pr->transport->tempo) ? pr->transport->tempo : 120.0;
   if (atomic_exchange_explicit(&s->pattern_loaded, false, memory_order_relaxed)) {
      seq_clear(s);
      s->seq_clock = SEQ_STOPPED;   /* a new pattern: placed again at the next sample, as at a start */
   }
   const clap_event_transport_t *tr = pr->transport;
   const bool playing = tr && (tr->flags & CLAP_TRANSPORT_IS_PLAYING) && (tr->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE);
   const double beat = playing ? (double)tr->song_pos_beats / (double)CLAP_BEATTIME_FACTOR : 0.0, per_frame = s->tempo / (60.0 * s->rate);
   if (!playing && s->seq_clock != SEQ_STOPPED) seq_stop(s);
   const clap_input_events_t *ev = pr->in_events;
   const uint32_t count = ev && ev->size && ev->get ? ev->size(ev) : 0;
   uint32_t next = 0;
   for (uint32_t n = 0; n < pr->frames_count; ++n) {
      if (playing) seq_advance(s, (beat + n * per_frame) * 24.0, per_frame * 24.0);   /* before the events: a step's locks precede its notes */
      for (; next < count; ++next) {
         const clap_event_header_t *h = ev->get(ev, next);
         if (h && h->time > n) break;
         event(s, h);
      }
      double y[2];
      host_frame(s, y);
      for (int c = 0; c < 2; ++c)
         if (out->data32) out->data32[c][n] = (float)y[c]; else out->data64[c][n] = y[c];
   }
   for (; next < count; ++next) event(s, ev->get(ev, next));
   ui_flush(s, pr->out_events);
   out->constant_mask = 0;
   return CLAP_PROCESS_CONTINUE;
}

/* ---- extensions ------------------------------------------------------------ */

static uint32_t ports_count(const clap_plugin_t *p, bool input) { (void)p; return input ? 0 : 1; }
static bool ports_get(const clap_plugin_t *p, uint32_t i, bool input, clap_audio_port_info_t *port) {
   if (i >= ports_count(p, input) || !port) return false;
   memset(port, 0, sizeof *port);
   port->id = 0;
   snprintf(port->name, sizeof port->name, "Stereo Out");
   port->flags = CLAP_AUDIO_PORT_IS_MAIN | CLAP_AUDIO_PORT_SUPPORTS_64BITS | CLAP_AUDIO_PORT_REQUIRES_COMMON_SAMPLE_SIZE;
   port->channel_count = 2;
   port->port_type = CLAP_PORT_STEREO;
   port->in_place_pair = CLAP_INVALID_ID;
   return true;
}
static const clap_plugin_audio_ports_t ports = {ports_count, ports_get};

static bool hidden(int m, int id) { return id < 8 && (machines[m].unused >> id & 1); }
static void param_name(int m, int id, char *out, size_t cap) {
   if (id < P_AMP) snprintf(out, cap, "%s", machines[m].label[id]);
   else if (id < P_LFO) snprintf(out, cap, "%s %s", page_name[id / 8], knob_label[id / 8 - 1][id % 8]);
   else if (id < P_LEVEL) snprintf(out, cap, "LFO%d %s", (id - P_LFO) / 8 + 1, lfo_label[(id - P_LFO) % 8]);
   else if (id < P_ASSIGN) snprintf(out, cap, "%s", track_label[id - P_LEVEL]);
   else snprintf(out, cap, "%s %d %s", id - P_ASSIGN < 6 ? "VEL" : "KEY", (id - P_ASSIGN) / 3 % 2 + 1, assign_field[(id - P_ASSIGN) % 3]);
}

static uint32_t params_count(const clap_plugin_t *p) { (void)p; return NPARAM; }
static bool params_info(const clap_plugin_t *p, uint32_t i, clap_param_info_t *pi) {
   const Plugin *s = p->plugin_data;
   if (i >= NPARAM || !pi) return false;
   memset(pi, 0, sizeof *pi);
   pi->id = i;
   pi->flags = hidden(s->machine, (int)i) ? CLAP_PARAM_IS_HIDDEN
             : CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_REQUIRES_PROCESS | (i <= P_LEVEL ? CLAP_PARAM_IS_MODULATABLE : 0)
               | (stepped((int)i) ? CLAP_PARAM_IS_STEPPED : 0);
   param_name(s->machine, (int)i, pi->name, sizeof pi->name);
   if (i < P_LFO) snprintf(pi->module, sizeof pi->module, "%s", page_name[i / 8]);
   else if (i < P_LEVEL) snprintf(pi->module, sizeof pi->module, "LFO%d", (int)(i - P_LFO) / 8 + 1);
   else snprintf(pi->module, sizeof pi->module, i < P_ASSIGN ? "TRACK" : "ASSIGN");
   pi->min_value = lo_of((int)i);
   pi->max_value = hi_of((int)i);
   pi->default_value = def_of(s->machine, (int)i);
   return true;
}
static bool params_get(const clap_plugin_t *p, clap_id id, double *v) {
   if (id >= NPARAM || !v) return false;
   *v = value(p->plugin_data, (int)id);
   return true;
}

/* What an LFO's DEST names, on the page its PAGE selects. */
static const char *dest_name(int m, int page, int dest) {
   if (page == 0) return range_name[dest];
   if (page == 1) return machines[m].label[dest];
   if (page <= 4) return knob_label[page - 2][dest];
   if (page <= 7) return lfo_label[dest];
   return midi_label[dest];
}
static bool bipolar(int m, int id) {
   if (id < P_AMP) return machines[m].bipolar >> id & 1;
   if (id >= P_ASSIGN) return (id - P_ASSIGN) % 3 == 2;   /* ADD: knob - 64 */
   return id < P_LFO && (knob_bipolar[id / 8 - 1] >> (id % 8) & 1);
}
/* ENS's PCH2-4: the step of ms_ens_steps a knob value selects, and its interval in semitones */
static bool is_ens_pitch(int m, int id) { return m == MT_ENS && id >= P_SYN && id < P_SYN + 3; }
static int ens_step(double v) {
   int k = (int)(lrint(clamp(v, 0, 127) * 65536.0) >> 16), s = 0;
   while (k >= ms_ens_steps[s].run) k -= ms_ens_steps[s++].run;
   return s;
}
static double ens_semitones(int s) {
   return ms_ens_steps[s].num ? 12 * log2((double)ms_ens_steps[s].num / ms_ens_steps[s].den) : ms_ens_steps[s].semi;
}
/* SYN knobs the unit shows as words or ratios (the main OS's knob descriptors at
 * 0x258f58): STON and PWRS as OFF/ON; FM+ STAT's and PAR's xFRQ as one of the 24
 * listed ratios, DYN's 1FRQ (2 x knob) and 2FRQ (4 x knob^2) as the ratio. */
static bool is_onoff(int m, int id) { return (m == MT_NOIS && id == P_SYN + 2) || (m == MT_PULS && id == P_SYN + 6); }
static bool is_listed(int m, int id) { return (m == MT_STAT && (id == 0 || id == 4)) || (m == MT_PAR && (id == 0 || id == 2 || id == 4)); }
static bool is_dyn_ratio(int m, int id) { return m == MT_DYN && (id == 0 || id == 4); }
static int32_t knob_word(double v) { return (int32_t)lrint(clamp(v, 0, 127) * 65536.0); }
static double dyn_ratio(int id, int32_t w) { const double x = w / 8388608.0; return id ? 4 * x * x : 2 * x; }
/* As the unit writes a ratio: these as fractions, others with three decimals
 * below 1 and two above, cut off, x.00 and x.50 shortened; the 24 listed ratios
 * of 1 and above as plain numbers. */
static void ratio_text(char *out, uint32_t cap, double r, bool listed) {
   static const unsigned char named[16][2] = {{1, 64}, {1, 32}, {1, 16}, {3, 32}, {1, 8}, {5, 32}, {3, 16}, {1, 4},
                                              {5, 16}, {3, 8}, {7, 16}, {1, 2}, {9, 16}, {5, 8}, {3, 4}, {7, 8}};
   for (int i = 0; i < 16; ++i)
      if (r * named[i][1] == named[i][0]) { snprintf(out, cap, "%d/%d", named[i][0], named[i][1]); return; }
   const int c = (int)floor(r * 100);
   if (listed) snprintf(out, cap, "%g", r);
   else if (r == 0) snprintf(out, cap, "0.0");
   else if (r < 1) snprintf(out, cap, ".%03d", (int)floor(r * 1000));
   else if (c % 100 == 0 || c % 100 == 50) snprintf(out, cap, "%d.%d", c / 100, c % 100 / 10);
   else snprintf(out, cap, "%d.%02d", c / 100, c % 100);
}
/* DYN's texts where the unit's own list departs from that rule */
static const char *dyn_fix(int id, int32_t w) {
   if (w & 0xffff) return NULL;
   const int k = w >> 16;
   return k == 127 ? (id ? "4.0" : "2.0") : !id ? NULL : k == 2 ? ".001" : k == 24 ? "9/64" : NULL;
}
/* a typed number or fraction ("3/2"); 2 when it was a fraction */
static int parse_number(const char *text, double *x) {
   char *end = NULL;
   *x = strtod(text, &end);
   if (end == text || !isfinite(*x)) return 0;
   if (*end != '/') return 1;
   const char *den = end + 1;
   const double d = strtod(den, &end);
   if (end == den || !(d > 0) || !isfinite(d)) return 0;
   *x /= d;
   return 2;
}
static bool params_to_text(const clap_plugin_t *p, clap_id cid, double v, char *out, uint32_t cap) {
   const Plugin *s = p->plugin_data;
   const int id = (int)cid, m = s->machine;
   if (cid >= NPARAM || !out || !cap || !isfinite(v)) return false;
   if (list_field(id) >= 0) {
      const int k = (int)lrint(clamp(v, 0, hi_of(id)));
      switch (list_field(id)) {
      case L_PAGE: snprintf(out, cap, "%s", lfo_page[k]); break;
      case L_DEST: snprintf(out, cap, "%s", dest_name(m, (int)lrint(value(s, id - 1)), k)); break;
      case L_TRIG: snprintf(out, cap, "%s", lfo_trig[k]); break;
      case L_WAVE: snprintf(out, cap, "%s", lfo_wave[k]); break;
      case L_MULT: snprintf(out, cap, "%s", lfo_mult[k]); break;
      default: snprintf(out, cap, "%d", k); break;
      }
   } else if (id >= P_ASSIGN) snprintf(out, cap, "%+d", (int)lrint(clamp(v, 0, 127)) - 64);   /* ADD */
   else if (is_switch(id)) snprintf(out, cap, "%s", v >= 0.5 ? "ON" : "OFF");
   else if (is_ens_pitch(m, id)) {   /* unison mutes the oscillator; the just intervals show their cents and ratio */
      const int st = ens_step(v);
      if (ms_ens_steps[st].num) snprintf(out, cap, "%+.2f st (%d/%d)", ens_semitones(st), ms_ens_steps[st].num, ms_ens_steps[st].den);
      else if (ms_ens_steps[st].semi) snprintf(out, cap, "%+d st", ms_ens_steps[st].semi);
      else snprintf(out, cap, "OFF");
   } else if (is_onoff(m, id)) snprintf(out, cap, "%s", knob_word(v) >= 0x400000 ? "ON" : "OFF");
   else if (is_listed(m, id)) ratio_text(out, cap, mf_ratio_of(knob_word(v)) / 1048576.0, true);
   else if (is_dyn_ratio(m, id)) {
      const char *fix = dyn_fix(id, knob_word(v));
      if (fix) snprintf(out, cap, "%s", fix);
      else ratio_text(out, cap, dyn_ratio(id, knob_word(v)), false);
   } else if (id == P_BEND) snprintf(out, cap, "%s", range_name[(int)lrint(clamp(v, 0, 7))]);
   else if (id == P_MTUNE) snprintf(out, cap, "%.1f Hz", lrint(clamp(v, 400, 499.9) * 10) / 10.0);
   else if (bipolar(m, id)) snprintf(out, cap, "%+.1f", v - 64);
   else snprintf(out, cap, "%.1f", v);
   return true;
}
static int match(const char *text, const char *const *names, int n) {
   for (int i = 0; i < n; ++i)
      if (!strcasecmp(text, names[i])) return i;
   return -1;
}
static bool params_from_text(const clap_plugin_t *p, clap_id cid, const char *text, double *v) {
   const Plugin *s = p->plugin_data;
   const int id = (int)cid;
   if (cid >= NPARAM || !text || !v) return false;
   if (list_field(id) >= 0) {
      const char *const *list[8] = {lfo_page, NULL, lfo_trig, lfo_wave, lfo_mult, NULL, NULL, NULL};
      const int f = list_field(id);
      if (list[f]) {
         const int i = match(text, list[f], lfo_size[f]);
         if (i >= 0) { *v = i; return true; }
      } else if (f == L_DEST) {
         const int page = (int)lrint(value(s, id - 1));
         for (int d = 0; d < 8; ++d)
            if (!strcasecmp(text, dest_name(s->machine, page, d))) { *v = d; return true; }
      }
   }
   if (is_switch(id)) {
      if (!strcasecmp(text, "on")) { *v = 1; return true; }
      if (!strcasecmp(text, "off")) { *v = 0; return true; }
   }
   if (id == P_BEND) {
      const int i = match(text, range_name, 8);
      if (i >= 0) { *v = i; return true; }
   }
   const int m = s->machine;
   if (is_onoff(m, id)) {
      if (!strcasecmp(text, "on")) { *v = 127; return true; }
      if (!strcasecmp(text, "off")) { *v = 0; return true; }
   }
   double x;
   const int kind = parse_number(text, &x);
   if (is_ens_pitch(m, id)) {   /* "OFF", semitones or a ratio: the first knob value of the nearest interval */
      const bool off = !strcasecmp(text, "off");
      if (!off && (!kind || (kind == 2 && !(x > 0)))) return false;
      if (kind == 2) x = 12 * log2(x);
      double gap = 1e9;
      for (int st = 0, k = 0; st < 33; k += ms_ens_steps[st++].run) {
         const double d = off ? (ms_ens_steps[st].num || ms_ens_steps[st].semi) : fabs(ens_semitones(st) - x);
         if (d < gap - 1e-9) { gap = d; *v = k; }
      }
      return true;
   }
   if (!kind) return false;
   if (is_listed(m, id)) {   /* the first knob value of the nearest listed ratio */
      if (!(x > 0)) return false;
      double gap = 1e9;
      for (int k = 0; k < 128; ++k) {
         const double d = fabs(log(mf_ratio_of(k << 16) / 1048576.0 / x));
         if (d < gap - 1e-9) { gap = d; *v = k; }
      }
      return true;
   }
   if (is_dyn_ratio(m, id)) {   /* rounded up to the next word, as the texts are cut off */
      const double k = id ? 64 * sqrt(fmax(x, 0)) : 64 * x;
      *v = clamp(ceil(k * 65536 - 1e-6) / 65536, 0, 127);
      return true;
   }
   *v = clamp(bipolar(m, id) ? x + 64 : x, lo_of(id), hi_of(id));
   return true;
}
static void params_flush(const clap_plugin_t *p, const clap_input_events_t *in, const clap_output_events_t *out) {
   const uint32_t n = in && in->size && in->get ? in->size(in) : 0;
   for (uint32_t i = 0; i < n; ++i) {
      const clap_event_header_t *h = in->get(in, i);
      if (h && (h->type == CLAP_EVENT_PARAM_VALUE || h->type == CLAP_EVENT_PARAM_MOD)) event(p->plugin_data, h);
   }
   ui_flush(p->plugin_data, out);
}
static const clap_plugin_params_t params = {params_count, params_info, params_get, params_to_text, params_from_text, params_flush};

/* Version 1 (0.1.0) held the first 61 parameters; version 2 (0.2.0) 65, then a
 * 16-step pattern: the trig mask and every step's locks; version 3 adds NOTE ENDS
 * LOCK. Version 4 (0.5.0) holds 64 steps: the trig mask (8 bytes), length, speed
 * and every step's locks. Version 5 (0.6.0) adds KIT ASSIGN's 12 parameters. */
#define STATE_V1 (8 + 8 * 61)
#define STATE_16(n) (8 + 8 * (n) + 2 + 16 * MT_LOCKS)
#define STATE_64(n) (8 + 8 * (n) + 10 + MT_STEPS * MT_LOCKS)
static bool state_save(const clap_plugin_t *p, const clap_ostream_t *st) {
   const Plugin *s = p->plugin_data;
   uint8_t buf[STATE_64(NPARAM)] = {0};
   memcpy(buf, machines[s->machine].magic, 4);
   buf[7] = 5;
   for (int i = 0; i < NPARAM; ++i)
      for (int b = 0; b < 8; ++b) buf[8 + 8 * i + b] = (uint8_t)(to_bits(value(s, i)) >> (8 * b));
   uint8_t *pat = buf + 8 + 8 * NPARAM;
   const uint64_t trig = atomic_load_explicit(&s->ui.trig, memory_order_relaxed);
   for (int b = 0; b < 8; ++b) pat[b] = (uint8_t)(trig >> (8 * b));
   pat[8] = (uint8_t)seq_length(s);
   pat[9] = (uint8_t)seq_speed(s);
   for (int t = 0; t < MT_STEPS; ++t)
      for (int i = 0; i < MT_LOCKS; ++i)
         pat[10 + t * MT_LOCKS + i] = (uint8_t)atomic_load_explicit(&s->ui.lock[t][i], memory_order_relaxed);
   for (uint64_t done = 0; done < sizeof buf;) {
      const int64_t n = st && st->write ? st->write(st, buf + done, sizeof buf - done) : -1;
      if (n <= 0) return false;
      done += (uint64_t)n;
   }
   return true;
}
static bool state_load(const clap_plugin_t *p, const clap_istream_t *st) {
   Plugin *s = p->plugin_data;
   uint8_t buf[STATE_64(NPARAM)];
   uint64_t got = 0;
   while (got < sizeof buf) {
      const int64_t n = st && st->read ? st->read(st, buf + got, sizeof buf - got) : -1;
      if (n < 0) return false;
      if (n == 0) break;
      got += (uint64_t)n;
   }
   if (got < 8 || memcmp(buf, machines[s->machine].magic, 4) || buf[4] || buf[5] || buf[6]) return false;
   const int version = buf[7];
   if (version < 1 || version > 5) return false;
   const int count = version == 1 ? 61 : version == 2 ? 65 : version < 5 ? P_ASSIGN : NPARAM;
   if (got != (uint64_t)(version == 1 ? STATE_V1 : version < 4 ? STATE_16(count) : STATE_64(count))) return false;
   double v[NPARAM];
   for (int i = 0; i < NPARAM; ++i) {
      if (i >= count) { v[i] = def_of(s->machine, i); continue; }
      uint64_t u = 0;
      for (int b = 0; b < 8; ++b) u |= (uint64_t)buf[8 + 8 * i + b] << (8 * b);
      v[i] = bits_to(u);
      if (!isfinite(v[i]) || v[i] < lo_of(i) || v[i] > hi_of(i)) return false;
   }
   /* the pattern; 16 steps before version 4, at length 16 and 1X */
   const uint8_t *pat = version >= 2 ? buf + 8 + 8 * count : NULL;
   const int steps = version >= 4 ? MT_STEPS : 16, head = version >= 4 ? 10 : 2;
   uint64_t trig = 0;
   int length = 16, speed = 0;
   if (pat) {
      for (int b = 0; b < steps / 8; ++b) trig |= (uint64_t)pat[b] << (8 * b);
      if (version >= 4) { length = pat[8]; speed = pat[9]; }
      if (length < 2 || length > MT_STEPS || speed > 3) return false;
      for (int t = 0; t < steps; ++t)
         for (int i = 0; i < MT_LOCKS; ++i) {
            const int x = (int8_t)pat[head + t * MT_LOCKS + i];
            if (x < -1 || x > hi_of(i)) return false;
         }
   }
   for (int i = 0; i < NPARAM; ++i) set_value(s, i, v[i]);
   atomic_store_explicit(&s->ui.trig, trig, memory_order_relaxed);
   atomic_store_explicit(&s->ui.length, (int8_t)length, memory_order_relaxed);
   atomic_store_explicit(&s->ui.speed, (int8_t)speed, memory_order_relaxed);
   for (int t = 0; t < MT_STEPS; ++t)
      for (int i = 0; i < MT_LOCKS; ++i)
         atomic_store_explicit(&s->ui.lock[t][i], pat && t < steps ? (int8_t)pat[head + t * MT_LOCKS + i] : -1, memory_order_relaxed);
   atomic_store_explicit(&s->pattern_loaded, true, memory_order_relaxed);
   const clap_host_params_t *hp = s->host->get_extension ? s->host->get_extension(s->host, CLAP_EXT_PARAMS) : NULL;
   if (hp && hp->rescan) hp->rescan(s->host, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
   return true;
}
static const clap_plugin_state_t state = {state_save, state_load};

/* The hardware's seven data pages, then the track's own settings. */
static uint32_t remote_count(const clap_plugin_t *p) { (void)p; return 8; }
static bool remote_get(const clap_plugin_t *p, uint32_t i, clap_remote_controls_page_t *page) {
   const Plugin *s = p->plugin_data;
   if (i >= remote_count(p) || !page) return false;
   static const char *const names[8] = {"SYN", "AMP", "FILT", "EFX", "LFO1", "LFO2", "LFO3", "TRACK"};
   memset(page, 0, sizeof *page);
   snprintf(page->section_name, sizeof page->section_name, "%s", machines[s->machine].name);
   snprintf(page->page_name, sizeof page->page_name, "%s", names[i]);
   page->page_id = i;
   for (int k = 0; k < CLAP_REMOTE_CONTROLS_COUNT; ++k)
      page->param_ids[k] = i == 7 ? (k < 6 ? (clap_id)(P_LEVEL + k) : CLAP_INVALID_ID)
                         : i == 0 && hidden(s->machine, k) ? CLAP_INVALID_ID : (clap_id)(8 * i + k);
   return true;
}
static const clap_plugin_remote_controls_t remote = {remote_count, remote_get};
static uint32_t latency_get(const clap_plugin_t *p) { return ((const Plugin *)p->plugin_data)->latency; }
static const clap_plugin_latency_t latency = {latency_get};
static uint32_t notes_count(const clap_plugin_t *p, bool input) { (void)p; return input ? 1 : 0; }
static bool notes_get(const clap_plugin_t *p, uint32_t i, bool input, clap_note_port_info_t *port) {
   (void)p;
   if (i || !input || !port) return false;
   memset(port, 0, sizeof *port);
   port->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
   port->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
   snprintf(port->name, sizeof port->name, "Notes");
   return true;
}
static const clap_plugin_note_ports_t notes = {notes_count, notes_get};

#ifndef MT_CHECK
static const clap_plugin_gui_t gui_ext;
#endif
static const void *plug_extension(const clap_plugin_t *p, const char *id) {
   (void)p;
   if (!id) return NULL;
   if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &ports;
   if (!strcmp(id, CLAP_EXT_PARAMS)) return &params;
   if (!strcmp(id, CLAP_EXT_STATE)) return &state;
   if (!strcmp(id, CLAP_EXT_REMOTE_CONTROLS) || !strcmp(id, CLAP_EXT_REMOTE_CONTROLS_COMPAT)) return &remote;
   if (!strcmp(id, CLAP_EXT_LATENCY)) return &latency;
   if (!strcmp(id, CLAP_EXT_NOTE_PORTS)) return &notes;
#ifndef MT_CHECK
   if (!strcmp(id, CLAP_EXT_GUI)) return &gui_ext;
#endif
   return NULL;
}

/* ---- the editor ------------------------------------------------------------- */

#ifndef MT_CHECK
static void ui_request_flush(const Plugin *s) {
   const clap_host_params_t *hp = s->host->get_extension ? s->host->get_extension(s->host, CLAP_EXT_PARAMS) : NULL;
   if (hp && hp->request_flush) hp->request_flush(s->host);
}
void mt_ui_begin(const clap_plugin_t *p, int id) {
   Plugin *s = p->plugin_data;
   atomic_fetch_or_explicit(&s->ui_flag[id], (uint8_t)(UI_WANT | UI_TOUCH), memory_order_acq_rel);
   ui_request_flush(s);
}
void mt_ui_set(const clap_plugin_t *p, int id, double x) {
   Plugin *s = p->plugin_data;
   x = clamp(x, lo_of(id), hi_of(id));
   if (stepped(id)) x = round(x);
   if (id < MT_LOCKS && x != value(s, id)) atomic_store_explicit(&s->ui.active[id], -1, memory_order_relaxed);
   set_value(s, id, x);
   atomic_fetch_or_explicit(&s->ui_flag[id], (uint8_t)UI_DIRTY, memory_order_acq_rel);
   if (list_field(id) == L_PAGE) {
      atomic_store(&s->text_dirty, true);
      if (s->host->request_callback) s->host->request_callback(s->host);
   }
   ui_request_flush(s);
}
void mt_ui_end(const clap_plugin_t *p, int id) {
   Plugin *s = p->plugin_data;
   atomic_fetch_and_explicit(&s->ui_flag[id], (uint8_t)~UI_WANT, memory_order_acq_rel);
   ui_request_flush(s);
}
void mt_ui_pattern_changed(const clap_plugin_t *p) {
   const Plugin *s = p->plugin_data;
   const clap_host_state_t *hs = s->host->get_extension ? s->host->get_extension(s->host, CLAP_EXT_STATE) : NULL;
   if (hs && hs->mark_dirty) hs->mark_dirty(s->host);
}
/* SCALE SETUP's length, moved one step at a time as the unit's arrow keys move it, so
 * each page it reaches is filled as the unit fills it (mt_grow_page). The playhead
 * takes a new length when the pattern next wraps. */
static bool page_empty(const Plugin *s, int page) {
   if (atomic_load_explicit(&s->ui.trig, memory_order_relaxed) >> (16 * page) & 0xffff) return false;
   for (int t = 16 * page; t < 16 * page + 16; ++t)
      for (int i = 0; i < MT_LOCKS; ++i)
         if (atomic_load_explicit(&s->ui.lock[t][i], memory_order_relaxed) >= 0) return false;
   return true;
}
void mt_ui_set_length(const clap_plugin_t *p, int length) {
   Plugin *s = p->plugin_data;
   length = length < 2 ? 2 : length > MT_STEPS ? MT_STEPS : length;
   for (int old = seq_length(s); old != length;) {
      const int to = old + (length > old ? 1 : -1);
      int src;
      const int dst = mt_grow_page(old, to, &src);
      if (dst >= 0 && page_empty(s, dst)) {   /* 0x23cb3a: the trigs are ORed in, the locks copied */
         const uint64_t trig = atomic_load_explicit(&s->ui.trig, memory_order_relaxed);
         atomic_fetch_or_explicit(&s->ui.trig, (trig >> (16 * src) & 0xffff) << (16 * dst), memory_order_relaxed);
         for (int t = 0; t < 16; ++t)
            for (int i = 0; i < MT_LOCKS; ++i)
               atomic_store_explicit(&s->ui.lock[16 * dst + t][i], atomic_load_explicit(&s->ui.lock[16 * src + t][i], memory_order_relaxed), memory_order_relaxed);
      }
      atomic_store_explicit(&s->ui.length, (int8_t)to, memory_order_relaxed);
      old = to;
   }
   mt_ui_pattern_changed(p);
}
int mt_ui_bipolar(const clap_plugin_t *p, int id) { return bipolar(((const Plugin *)p->plugin_data)->machine, id); }
int mt_ui_unused(const clap_plugin_t *p, int id) { return hidden(((const Plugin *)p->plugin_data)->machine, id); }

static bool gui_supported(const clap_plugin_t *p, const char *api, bool floating) { (void)p; return api && !strcmp(api, CLAP_WINDOW_API_COCOA) && !floating; }
static bool gui_preferred(const clap_plugin_t *p, const char **api, bool *floating) { (void)p; *api = CLAP_WINDOW_API_COCOA; *floating = false; return true; }
static bool gui_create(const clap_plugin_t *p, const char *api, bool floating) {
   Plugin *s = p->plugin_data;
   if (s->gui || !gui_supported(p, api, floating)) return false;
   s->gui = mt_gui_create(p, &s->ui);
   if (s->gui) mt_gui_set_size(s->gui, s->gui_w, s->gui_h);
   return s->gui != NULL;
}
static void gui_destroy(const clap_plugin_t *p) {
   Plugin *s = p->plugin_data;
   if (s->gui) mt_gui_destroy(s->gui);
   s->gui = NULL;
}
static bool gui_set_scale(const clap_plugin_t *p, double scale) { (void)p; (void)scale; return false; }   /* Cocoa works in points */
static bool gui_get_size(const clap_plugin_t *p, uint32_t *w, uint32_t *h) { const Plugin *s = p->plugin_data; *w = s->gui_w; *h = s->gui_h; return true; }
static bool gui_can_resize(const clap_plugin_t *p) { (void)p; return true; }
static bool gui_hints(const clap_plugin_t *p, clap_gui_resize_hints_t *r) {
   (void)p;
   *r = (clap_gui_resize_hints_t){true, true, true, MT_GUI_W, MT_GUI_H};
   return true;
}
static bool gui_adjust(const clap_plugin_t *p, uint32_t *w, uint32_t *h) {
   (void)p;
   const double x = clamp(fmax(*w, (double)*h * MT_GUI_W / MT_GUI_H), MT_GUI_W * 0.6, MT_GUI_W * 2.0);
   *w = (uint32_t)lround(x);
   *h = (uint32_t)lround(x * MT_GUI_H / MT_GUI_W);
   return true;
}
static bool gui_set_size(const clap_plugin_t *p, uint32_t w, uint32_t h) {
   Plugin *s = p->plugin_data;
   if (!s->gui) return false;
   s->gui_w = w;
   s->gui_h = h;
   mt_gui_set_size(s->gui, w, h);
   return true;
}
static bool gui_set_parent(const clap_plugin_t *p, const clap_window_t *w) {
   Plugin *s = p->plugin_data;
   if (!s->gui || !w || !w->cocoa) return false;
   mt_gui_set_parent(s->gui, w->cocoa);
   return true;
}
static bool gui_set_transient(const clap_plugin_t *p, const clap_window_t *w) { (void)p; (void)w; return false; }
static void gui_suggest_title(const clap_plugin_t *p, const char *t) { (void)p; (void)t; }
static bool gui_show(const clap_plugin_t *p) { Plugin *s = p->plugin_data; if (!s->gui) return false; mt_gui_show(s->gui); return true; }
static bool gui_hide(const clap_plugin_t *p) { Plugin *s = p->plugin_data; if (!s->gui) return false; mt_gui_hide(s->gui); return true; }
static const clap_plugin_gui_t gui_ext = {
   gui_supported, gui_preferred, gui_create, gui_destroy, gui_set_scale, gui_get_size, gui_can_resize, gui_hints,
   gui_adjust, gui_set_size, gui_set_parent, gui_set_transient, gui_suggest_title, gui_show, gui_hide,
};
#endif

/* ---- factory and entry ----------------------------------------------------- */

static uint32_t factory_count(const clap_plugin_factory_t *f) { (void)f; return MT_MACHINES; }
static const clap_plugin_descriptor_t *factory_desc(const clap_plugin_factory_t *f, uint32_t i) { (void)f; return i < MT_MACHINES ? &descriptors[i] : NULL; }
static const clap_plugin_t *factory_create(const clap_plugin_factory_t *f, const clap_host_t *host, const char *id) {
   (void)f;
   if (!host || !id || !clap_version_is_compatible(host->clap_version)) return NULL;
   int m = 0;
   while (m < MT_MACHINES && strcmp(id, descriptors[m].id)) ++m;
   if (m == MT_MACHINES) return NULL;
   Plugin *s = calloc(1, sizeof *s);
   if (!s) return NULL;
   s->host = host;
   s->machine = m;
   s->tempo = 120.0;
   s->rate = NATIVE;
   s->gui_w = MT_GUI_W;
   s->gui_h = MT_GUI_H;
   for (int i = 0; i < NPARAM; ++i) atomic_init(&s->value[i], to_bits(def_of(m, i)));
   atomic_init(&s->text_dirty, false);
   atomic_init(&s->pattern_loaded, false);
   atomic_init(&s->ui.trig, 0);
   atomic_init(&s->ui.length, 16);
   atomic_init(&s->ui.speed, 0);
   atomic_init(&s->ui.step, -1);
   for (int l = 0; l < 3; ++l) {
      atomic_init(&s->ui.lfo_pos[l], 0);
      atomic_init(&s->ui.lfo_inc[l], 0);
      atomic_init(&s->ui.lfo_wave[l], 0);
      atomic_init(&s->ui.lfo_depth[l], 0);
      for (int k = 0; k < 8; ++k) atomic_init(&s->ui.lfo_rnd[l][k], 0);
   }
   for (int t = 0; t < MT_STEPS; ++t) atomic_init(&s->ui.notes[t], 0);
   memset(s->queue, QUEUE_NONE, sizeof s->queue);
   for (int i = 0; i < NPARAM; ++i) { atomic_init(&s->ui_flag[i], 0); atomic_init(&s->ui.mod[i], 0.f); }
   for (int i = 0; i < MT_LOCKS; ++i) {
      atomic_init(&s->ui.active[i], -1);
      for (int t = 0; t < MT_STEPS; ++t) atomic_init(&s->ui.lock[t][i], -1);
   }
   s->plugin = (clap_plugin_t){
      .desc = &descriptors[m], .plugin_data = s, .init = plug_init, .destroy = plug_destroy,
      .activate = plug_activate, .deactivate = plug_deactivate, .start_processing = plug_start,
      .stop_processing = plug_stop, .reset = plug_reset, .process = plug_process,
      .get_extension = plug_extension, .on_main_thread = plug_on_main,
   };
   return &s->plugin;
}
static const clap_plugin_factory_t factory = {factory_count, factory_desc, factory_create};

static bool entry_init(const char *path) { (void)path; mt_tables(); return true; }   /* read-only from here on */
static void entry_deinit(void) {}
static const void *entry_factory(const char *id) { return id && !strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &factory : NULL; }
CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entry_init, entry_deinit, entry_factory};
