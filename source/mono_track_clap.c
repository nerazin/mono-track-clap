/* Mono Track: a whole Monomachine track around each synth machine, as eight
 * CLAP instruments in one bundle.
 *
 * mono_track.h is the track, a fixed-point port of OS 1.32B: machine, EQ,
 * filter + DIST, AMP, SRR, delay, LEVEL, the filter envelope and key tracking,
 * and the main CPU's knob slew and three LFOs. This file adds what surrounds a
 * track in a DAW:
 *  - the notes: one voice, the last note held wins; releasing it returns to
 *    the previous note still held (a new trig), or ends the note;
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

enum { P_SYN = 0, P_AMP = 8, P_FILT = 16, P_EFX = 24, P_LFO = 32, P_LEVEL = 56, P_LPKEY, P_HPKEY, P_BEND, P_MTUNE, NPARAM };

typedef struct {
   const char *id, *name, *magic;
   const char *label[8];
   unsigned char def[8];
   unsigned unused, bipolar;   /* knobs the machine does not read (hidden); knobs shown as value - 64 */
} Machine;
static const Machine machines[MT_MACHINES] = {
   {"works.reconstruction.monotrack.sin", "Mono Track GND SIN", "MTSN", {"-", "-", "-", "-", "-", "-", "-", "TUNE"}, {0, 0, 0, 0, 0, 0, 0, 64}, 0x7f, 0x80},
   {"works.reconstruction.monotrack.nois", "Mono Track GND NOIS", "MTNS", {"ST", "RED", "STON", "-", "-", "-", "-", "TUNE"}, {0, 0, 0, 0, 0, 0, 0, 64}, 0x78, 0x80},
   {"works.reconstruction.monotrack.saw", "Mono Track SWAVE SAW", "MTSW", {"UNIL", "UNIW", "UNIX", "-", "SUBX", "SUB1", "SUB2", "TUNE"}, {0, 0, 0, 0, 0, 0, 0, 64}, 0x08, 0x80},
   {"works.reconstruction.monotrack.puls", "Mono Track SWAVE PULS", "MTPL", {"UNIL", "UNIW", "SUB1", "SUB2", "PW", "PWAD", "PWRS", "TUNE"}, {0, 0, 0, 0, 64, 0, 0, 64}, 0, 0x80},
   {"works.reconstruction.monotrack.ens", "Mono Track SWAVE ENS", "MTEN", {"PCH2", "PCH3", "PCH4", "WAVE", "PW", "CHRL", "CHRW", "TUNE"}, {63, 63, 63, 0, 64, 0, 127, 64}, 0, 0x80},
   {"works.reconstruction.monotrack.fmstat", "Mono Track FM+ STAT", "MTFS", {"1FRQ", "1FIN", "1ENV", "1FB", "2FRQ", "2VOL", "TONE", "TUNE"}, {60, 64, 80, 30, 80, 64, 98, 64}, 0, 0x82},
   {"works.reconstruction.monotrack.fmpar", "Mono Track FM+ PAR", "MTFP", {"1FRQ", "1ENV", "2FRQ", "2ENV", "3FRQ", "3ENV", "TONE", "TUNE"}, {60, 64, 80, 64, 102, 80, 98, 64}, 0, 0x80},
   {"works.reconstruction.monotrack.fmdyn", "Mono Track FM+ DYN", "MTFD", {"1FRQ", "1FEN", "1VOL", "1VEN", "2FRQ", "2ENV", "2FB", "TUNE"}, {64, 64, 64, 64, 74, 80, 30, 64}, 0, 0x80},
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
static const char *const lfo_page[9] = {"PTCH", "SYN", "AMP", "FILT", "EFX", "LFO1", "LFO2", "LFO3", "MIDI"};
static const char *const lfo_trig[5] = {"FREE", "TRIG", "HOLD", "ONE", "HALF"};
static const char *const lfo_wave[11] = {"TRI", "ITRI", "SAW", "ISAW", "SQR", "ISQR", "EXP", "IEXP", "RAMP", "IRMP", "RND"};
static const char *const lfo_mult[7] = {"1X", "2X", "4X", "8X", "16X", "32X", "64X"};
static const char *const range_name[8] = {"1/12", "2/12", "7/12", "1OCT", "2OCT", "4OCT", "8OCT", "16OC"};
static const char *const track_label[5] = {"LEVEL", "LP KEY", "HP KEY", "BEND", "MASTER TUNE"};

static const char *const features[] = {
   CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, CLAP_PLUGIN_FEATURE_STEREO, NULL
};
#define DESCRIPTOR(m) { .clap_version = CLAP_VERSION_INIT, .id = machines[m].id, .name = machines[m].name, \
   .vendor = "Independent DSP", .url = "", .manual_url = "", .support_url = "", .version = "0.1.0", \
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
   return id == P_LEVEL ? 100 : id == P_LPKEY || id == P_HPKEY ? 1 : id == P_BEND ? 1 : 440;
}
static double lo_of(int id) { return id == P_MTUNE ? 400 : 0; }
static double hi_of(int id) {
   if (id >= P_LFO && id < P_LEVEL && lfo_size[(id - P_LFO) % 8]) return lfo_size[(id - P_LFO) % 8] - 1;
   return id == P_MTUNE ? 499.9 : id == P_BEND ? 7 : id == P_LPKEY || id == P_HPKEY ? 1 : 127;
}
/* an LFO parameter's value as the kit byte the OS reads */
static int lfo_raw(int field, double v) {
   const int n = lfo_size[field], k = (int)lrint(v);
   return n ? (k * 128 + n - 1) / n : k;
}
static bool stepped(int id) { return (id >= P_LFO && id < P_LEVEL) || id == P_LPKEY || id == P_HPKEY || id == P_BEND; }

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

typedef struct {
   clap_plugin_t plugin;
   const clap_host_t *host;
   int machine;
   _Atomic uint64_t value[NPARAM];    /* parameter values (double bits): host thread and audio thread */
   _Atomic bool text_dirty;           /* an LFO's PAGE changed: its DEST reads differently */
   double mod[NPARAM];
   /* audio thread */
   bool active, direct, settled;
   uint32_t latency, blocks;
   int pos;
   int bend;                          /* the pitch bend's top 8 bits, 128 at the centre */
   double tempo;
   /* notes: the keys held, in the order pressed; the note sounding; the frame's news (1 on, 2 off) */
   int held[128], nheld, note, pending;
   MtHost cpu;
   MonoTrack track;
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
static void machine_start(Plugin *s) {
#ifndef MT_NO_START
   MonoTrack *t = &s->track;
   const int32_t *syn = s->cpu.block + W_SYN;
   int32_t x[2][MT_FRAMES] = {{0}};
   if (s->machine == MT_NOIS)
      for (int n = 0; n < NOIS_START; ++n) mg_block(&t->gnd, syn, 0, x);
   else if (s->machine == MT_ENS) {
      int32_t pan[2];
      ms_pan(s->cpu.word[W_PAN], pan);
      for (int n = 0; n < ENS_START; ++n) {
         memset(x, 0, sizeof x);
         ms_chorus(&t->sw, syn, pan, x);
      }
   }
#else
   (void)s;
#endif
}

/* Start of a control frame (three blocks): the words the main CPU sends. */
static void control_frame(Plugin *s) {
   MtHost *h = &s->cpu;
   int32_t mod[MT_KNOBS + 24];
   bool any = false;
   for (int p = 0; p < P_LEVEL; ++p) {
      const int n = p >= P_LFO ? lfo_size[(p - P_LFO) % 8] : 0;   /* a list's step is 128 / n raw values */
      mod[p] = s->mod[p] == 0 ? 0 : (int32_t)lrint(s->mod[p] * (n ? 128.0 / n : 1.0) * 65536.0);
      any |= mod[p] != 0;
   }
   for (int p = 0; p < MT_KNOBS; ++p) h->knob[p] = (int32_t)lrint(clamp(value(s, p), 0, 127) * 65536.0);
   for (int l = 0; l < 3; ++l)
      for (int k = 0; k < 8; ++k) h->raw[l][k] = lfo_raw(k, value(s, P_LFO + 8 * l + k));
   h->level = (int32_t)lrint(clamp(value(s, P_LEVEL) + s->mod[P_LEVEL], 0, 127) * 65536.0);
   h->bend = mf_bend(s->bend, (int)lrint(value(s, P_BEND)));
   const bool start = !s->settled;
   if (start) mt_host_reset(h);
   const int32_t tick = tick_of(s->tempo);
   h->word[W_TICK] = tick;
   h->word[W_RECIP] = 0x800000 / tick;
   h->word[W_PITCH] = (s->note << 11) / 12;
   h->word[W_MTUNE] = mf_master_tune((int)lrint(clamp(value(s, P_MTUNE) * 10, 4000, 4999)));
   h->word[W_ROUTE] = 1 | (value(s, P_LPKEY) >= 0.5 ? MT_LP_KEY : 0) | (value(s, P_HPKEY) >= 0.5 ? MT_HP_KEY : 0);
   h->word[W_NOTE] = s->pending == 1 ? 0x81 : s->pending == 2 ? 0x02 : 0;
   h->word[W_FTRIG] = s->pending == 1;
   if (s->pending == 1) h->lfo_trig = 1;
   s->pending = 0;
   if (start) {   /* the machine's assignment: its init and trig routines, the glide at the note, the AMP idle */
      mt_reset(&s->track, s->machine, h->word[W_PITCH]);
      md_settle(&s->track.delay, h->knob[24 + 3], (uint32_t)tick);
      h->word[W_NOTE] |= 0x80;
   }
   mt_host_frame(h, any ? mod : NULL);
   h->word[W_NOTE] = h->word[W_FTRIG] = 0;
   if (start) machine_start(s);
   s->settled = true;
}

/* One frame at 44.1 kHz; a block is computed at its first frame. */
static void native_frame(Plugin *s, double out[2]) {
   if (s->pos == 0) {
      const bool frame_start = s->blocks % 3 == 0;
      if (frame_start) control_frame(s);
      s->blocks++;
      int32_t y[2][MT_FRAMES];
      mt_block(&s->track, s->cpu.block, y);
#ifdef MT_CHECK
      if (mt_check_hook) mt_check_hook(&s->track, s->cpu.block, (const int32_t (*)[MT_FRAMES])y);
#endif
      s->cpu.block[W_NOTE] = s->cpu.block[W_FTRIG] = 0;   /* one-shots: only the frame's first block carries them */
      for (int c = 0; c < 2; ++c)
         for (int i = 0; i < MT_FRAMES; ++i) s->out[c][i] = y[c][i] / 8388608.0;
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

static void reset(Plugin *s) {
   s->settled = false;
   s->blocks = 0;
   s->pos = 0;
   s->nheld = 0;
   s->note = 60;
   s->pending = 0;
   s->bend = 128;
   rs_reset(&s->down);
}

static void note_on(Plugin *s, int key) {
   for (int i = 0; i < s->nheld; ++i)
      if (s->held[i] == key) { memmove(s->held + i, s->held + i + 1, (size_t)(--s->nheld - i) * sizeof *s->held); break; }
   s->held[s->nheld++] = key;
   s->note = key;
   s->pending = 1;
}
static void note_off(Plugin *s, int key) {
   for (int i = 0; i < s->nheld; ++i)
      if (s->held[i] == key) {
         memmove(s->held + i, s->held + i + 1, (size_t)(--s->nheld - i) * sizeof *s->held);
         if (key != s->note) return;
         if (s->nheld) { s->note = s->held[s->nheld - 1]; s->pending = 1; }
         else s->pending = 2;
         return;
      }
}

static bool plug_init(const clap_plugin_t *p) { (void)p; return true; }
static void plug_destroy(const clap_plugin_t *p) {
   Plugin *s = p->plugin_data;
   free(s->down.kernel);
   free(s);
}
static bool plug_activate(const clap_plugin_t *p, double rate, uint32_t min, uint32_t max) {
   (void)min; (void)max;
   Plugin *s = p->plugin_data;
   if (s->active || !isfinite(rate) || rate < 8000 || rate > 768000) return false;
   s->direct = rate == NATIVE;
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
         set_value(s, id, clamp(e->value, lo_of(id), hi_of(id)));
         if (id >= P_LFO && id < P_LEVEL && (id - P_LFO) % 8 == L_PAGE) {
            atomic_store(&s->text_dirty, true);
            if (s->host->request_callback) s->host->request_callback(s->host);
         }
      }
   } else if (h->type == CLAP_EVENT_PARAM_MOD && h->size >= sizeof(clap_event_param_mod_t)) {
      const clap_event_param_mod_t *e = (const void *)h;
      if (e->param_id <= P_LEVEL && e->note_id == -1 && e->port_index == -1 && e->channel == -1 && e->key == -1 && isfinite(e->amount))
         s->mod[e->param_id] = clamp(e->amount, -127, 127);
   } else if ((h->type == CLAP_EVENT_NOTE_ON || h->type == CLAP_EVENT_NOTE_OFF) && h->size >= sizeof(clap_event_note_t)) {
      const clap_event_note_t *e = (const void *)h;
      if (e->key >= 0 && e->key < 128) { if (h->type == CLAP_EVENT_NOTE_ON) note_on(s, e->key); else note_off(s, e->key); }
   } else if (h->type == CLAP_EVENT_MIDI && h->size >= sizeof(clap_event_midi_t)) {
      const clap_event_midi_t *e = (const void *)h;
      const int status = e->data[0] & 0xf0, key = e->data[1] & 0x7f;
      if (status == 0x90 && e->data[2] > 0) note_on(s, key);
      else if (status == 0x80 || status == 0x90) note_off(s, key);
      else if (status == 0xe0) s->bend = (e->data[2] & 0x7f) * 2 + ((e->data[1] & 0x7f) >> 6);
   }
}

static clap_process_status plug_process(const clap_plugin_t *p, const clap_process_t *pr) {
   Plugin *s = p->plugin_data;
   if (!s->active || !pr || pr->audio_outputs_count < 1) return CLAP_PROCESS_ERROR;
   clap_audio_buffer_t *out = &pr->audio_outputs[0];
   if (out->channel_count < 2 || (!out->data32 && !out->data64)) return CLAP_PROCESS_ERROR;
   s->tempo = pr->transport && (pr->transport->flags & CLAP_TRANSPORT_HAS_TEMPO) && isfinite(pr->transport->tempo) ? pr->transport->tempo : 120.0;
   const clap_input_events_t *ev = pr->in_events;
   const uint32_t count = ev && ev->size && ev->get ? ev->size(ev) : 0;
   uint32_t next = 0;
   for (uint32_t n = 0; n < pr->frames_count; ++n) {
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
   else snprintf(out, cap, "%s", track_label[id - P_LEVEL]);
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
   else snprintf(pi->module, sizeof pi->module, "TRACK");
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
   return "CC";
}
static bool bipolar(int m, int id) {
   if (id < P_AMP) return machines[m].bipolar >> id & 1;
   return id < P_LFO && (knob_bipolar[id / 8 - 1] >> (id % 8) & 1);
}
static bool params_to_text(const clap_plugin_t *p, clap_id cid, double v, char *out, uint32_t cap) {
   const Plugin *s = p->plugin_data;
   const int id = (int)cid, m = s->machine;
   if (cid >= NPARAM || !out || !cap || !isfinite(v)) return false;
   if (id >= P_LFO && id < P_LEVEL) {
      const int l = (id - P_LFO) / 8, f = (id - P_LFO) % 8, k = (int)lrint(clamp(v, 0, hi_of(id)));
      switch (f) {
      case L_PAGE: snprintf(out, cap, "%s", lfo_page[k]); break;
      case L_DEST: snprintf(out, cap, "%s", dest_name(m, (int)lrint(value(s, P_LFO + 8 * l + L_PAGE)), k)); break;
      case L_TRIG: snprintf(out, cap, "%s", lfo_trig[k]); break;
      case L_WAVE: snprintf(out, cap, "%s", lfo_wave[k]); break;
      case L_MULT: snprintf(out, cap, "%s", lfo_mult[k]); break;
      default: snprintf(out, cap, "%d", k); break;
      }
   } else if (id == P_LPKEY || id == P_HPKEY) snprintf(out, cap, "%s", v >= 0.5 ? "ON" : "OFF");
   else if (id == P_BEND) snprintf(out, cap, "%s", range_name[(int)lrint(clamp(v, 0, 7))]);
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
   if (id >= P_LFO && id < P_LEVEL) {
      const char *const *list[8] = {lfo_page, NULL, lfo_trig, lfo_wave, lfo_mult, NULL, NULL, NULL};
      const int f = (id - P_LFO) % 8;
      if (list[f]) {
         const int i = match(text, list[f], lfo_size[f]);
         if (i >= 0) { *v = i; return true; }
      } else if (f == L_DEST) {
         const int page = (int)lrint(value(s, id - 1));
         for (int d = 0; d < 8; ++d)
            if (!strcasecmp(text, dest_name(s->machine, page, d))) { *v = d; return true; }
      }
   }
   if (id == P_LPKEY || id == P_HPKEY) {
      if (!strcasecmp(text, "on")) { *v = 1; return true; }
      if (!strcasecmp(text, "off")) { *v = 0; return true; }
   }
   if (id == P_BEND) {
      const int i = match(text, range_name, 8);
      if (i >= 0) { *v = i; return true; }
   }
   char *end = NULL;
   const double x = strtod(text, &end);
   if (end == text || !isfinite(x)) return false;
   *v = clamp(bipolar(s->machine, id) ? x + 64 : x, lo_of(id), hi_of(id));
   return true;
}
static void params_flush(const clap_plugin_t *p, const clap_input_events_t *in, const clap_output_events_t *out) {
   (void)out;
   const uint32_t n = in && in->size && in->get ? in->size(in) : 0;
   for (uint32_t i = 0; i < n; ++i) {
      const clap_event_header_t *h = in->get(in, i);
      if (h && (h->type == CLAP_EVENT_PARAM_VALUE || h->type == CLAP_EVENT_PARAM_MOD)) event(p->plugin_data, h);
   }
}
static const clap_plugin_params_t params = {params_count, params_info, params_get, params_to_text, params_from_text, params_flush};

static bool state_save(const clap_plugin_t *p, const clap_ostream_t *st) {
   const Plugin *s = p->plugin_data;
   uint8_t buf[8 + 8 * NPARAM] = {0};
   memcpy(buf, machines[s->machine].magic, 4);
   buf[7] = 1;
   for (int i = 0; i < NPARAM; ++i)
      for (int b = 0; b < 8; ++b) buf[8 + 8 * i + b] = (uint8_t)(to_bits(value(s, i)) >> (8 * b));
   for (uint64_t done = 0; done < sizeof buf;) {
      const int64_t n = st && st->write ? st->write(st, buf + done, sizeof buf - done) : -1;
      if (n <= 0) return false;
      done += (uint64_t)n;
   }
   return true;
}
static bool state_load(const clap_plugin_t *p, const clap_istream_t *st) {
   Plugin *s = p->plugin_data;
   uint8_t buf[8 + 8 * NPARAM];
   for (uint64_t done = 0; done < sizeof buf;) {
      const int64_t n = st && st->read ? st->read(st, buf + done, sizeof buf - done) : -1;
      if (n <= 0) return false;
      done += (uint64_t)n;
   }
   if (memcmp(buf, machines[s->machine].magic, 4) || buf[4] || buf[5] || buf[6] || buf[7] != 1) return false;
   double v[NPARAM];
   for (int i = 0; i < NPARAM; ++i) {
      uint64_t u = 0;
      for (int b = 0; b < 8; ++b) u |= (uint64_t)buf[8 + 8 * i + b] << (8 * b);
      v[i] = bits_to(u);
      if (!isfinite(v[i]) || v[i] < lo_of(i) || v[i] > hi_of(i)) return false;
   }
   for (int i = 0; i < NPARAM; ++i) set_value(s, i, v[i]);
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
      page->param_ids[k] = i == 7 ? (k < 5 ? (clap_id)(P_LEVEL + k) : CLAP_INVALID_ID)
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

static const void *plug_extension(const clap_plugin_t *p, const char *id) {
   (void)p;
   if (!id) return NULL;
   if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &ports;
   if (!strcmp(id, CLAP_EXT_PARAMS)) return &params;
   if (!strcmp(id, CLAP_EXT_STATE)) return &state;
   if (!strcmp(id, CLAP_EXT_REMOTE_CONTROLS) || !strcmp(id, CLAP_EXT_REMOTE_CONTROLS_COMPAT)) return &remote;
   if (!strcmp(id, CLAP_EXT_LATENCY)) return &latency;
   if (!strcmp(id, CLAP_EXT_NOTE_PORTS)) return &notes;
   return NULL;
}

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
   for (int i = 0; i < NPARAM; ++i) atomic_init(&s->value[i], to_bits(def_of(m, i)));
   atomic_init(&s->text_dirty, false);
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
