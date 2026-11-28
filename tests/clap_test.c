/* Loads the built bundle like a host and checks what the core test cannot:
 * the wrapper around mono_track.h. usage: clap_test "build/Mono Track.clap"
 *  - the bundle offers the fifteen machines, each with 78 parameters and the
 *    eight remote-control pages (SYN AMP FILT EFX LFO1 LFO2 LFO3 TRACK)
 *  - every machine's state round trip, which no other machine loads
 *  - the FX machines: their ports, defaults and latency; each passes its input
 *    at 44.1 and 48 kHz with no note; INP at 0 silences it; a note reopens the
 *    AMP; RINGMOD's EXT takes the sidechain; no input buffer is silence; POLY
 *  - a note sounds and its release fades, at 44.1 and 48 kHz, and the latency
 *    is reported
 *  - each section is in the signal path: EQ, filter, DIST, SRR, delay, LEVEL
 *  - key tracking opens the low-pass for high notes only when LP KEY is on
 *  - an LFO on page FILT moves the sound, and a second LFO aimed at the first
 *    one's DPTH changes that movement
 *  - LFO list knobs show their entries, and DEST names follow PAGE
 *  - state round trip, rejected bad states, parameter text round trip, states of
 *    0.1.0, 0.2.0 and 0.4 (16 steps), 0.5 and 0.6 to 0.8.3 (no editor size)
 *  - the editor's size in the state: kept, fitted, and left out of presets
 *  - POLY: one note equals mono, a chord is the sum of its notes, a seventh steals
 *  - the MIDI TRIG switches
 *  - trigs play no note: they restart the filter envelope and LFOs and end locks
 *  - locks, trigless locks, MIDI notes ending locks (and the switch), stop
 *  - a host position that rounds backward does not play a step twice
 *  - 64 steps: a step past the length never plays; a short length wraps; the four
 *    speeds; a start in the middle of the song finds its step at that length */
#include <clap/clap.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); puts(""); exit(1); } } while (0)
#define NPARAM 78
#define PAT (8 + 8 * NPARAM)
#define WIDTH (PAT + 10 + 64 * 56)   /* the editor's, two bytes */
#define STATE_SIZE (WIDTH + 2)
#define N 88200
enum { SYN = 0, AMP = 8, FILT = 16, EFX = 24, LFO = 32, LEVEL = 56, LPKEY, HPKEY, BEND, MTUNE, POLY, TRIG_SYN, TRIG_FILT, TRIG_LFO, LOCK_END, ASSIGN };   /* ASSIGN: VEL 1, VEL 2, KEY 1, KEY 2, each PAGE DEST ADD */
static const char *const ids[15] = {"sin", "nois", "saw", "puls", "ens", "fmstat", "fmpar", "fmdyn",
   "thru", "reverb", "chorus", "dynamix", "ringmod", "phaser", "flanger"};
#define FX0 8   /* the first FX machine */

typedef union { clap_event_header_t h; clap_event_param_value_t v; clap_event_note_t n; } Event;
typedef struct { clap_input_events_t api; Event v[NPARAM + 4]; uint32_t n; } Events;
static uint32_t ev_size(const clap_input_events_t *e) { return ((const Events *)e->ctx)->n; }
static const clap_event_header_t *ev_get(const clap_input_events_t *e, uint32_t i) { return &((const Events *)e->ctx)->v[i].h; }
static const void *host_ext(const clap_host_t *h, const char *id) { (void)h; (void)id; return NULL; }
static void host_cb(const clap_host_t *h) { (void)h; }
static const clap_host_t host = {CLAP_VERSION_INIT, NULL, "test", "", "", "1", host_ext, host_cb, host_cb, host_cb};
static const clap_plugin_factory_t *factory;

typedef struct { uint8_t data[8192]; uint64_t size, pos; } Buffer;
static int64_t buf_write(const clap_ostream_t *s, const void *p, uint64_t n) { Buffer *b = s->ctx; if (b->size + n > sizeof b->data) return -1; memcpy(b->data + b->size, p, n); b->size += n; return (int64_t)n; }
static int64_t buf_read(const clap_istream_t *s, void *p, uint64_t n) { Buffer *b = s->ctx; if (n > b->size - b->pos) n = b->size - b->pos; memcpy(p, b->data + b->pos, n); b->pos += n; return (int64_t)n; }

static const clap_plugin_t *create(int m) {
   char id[64];
   snprintf(id, sizeof id, "works.reconstruction.monotrack.%s", ids[m]);
   const clap_plugin_t *p = factory->create_plugin(factory, &host, id);
   CHECK(p && p->init(p), "create %s", id);
   return p;
}
typedef struct { int id; double v; } Set;
static void set_params(const clap_plugin_t *p, const Set *set, int n) {
   const clap_plugin_params_t *params = p->get_extension(p, CLAP_EXT_PARAMS);
   Events ev = {{NULL, ev_size, ev_get}, {{{0}}}, 0};
   ev.api.ctx = &ev;
   for (int i = 0; i < n; ++i)
      ev.v[ev.n++].v = (clap_event_param_value_t){{sizeof ev.v[0].v, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0}, (clap_id)set[i].id, NULL, -1, -1, -1, -1, set[i].v};
   params->flush(p, &ev.api, NULL);
}
/* A note from frame 100 for `hold` frames (none for key -1), at this velocity; the output, frame by frame.
 * in_l/in_r and ext_l/ext_r, when set, are the input and the sidechain. */
static double note_vel = 1.0;
static float *in_l, *in_r, *ext_l, *ext_r;
static void render(int m, const Set *set, int nset, int key, int hold, double rate, float *l, float *r, int frames) {
   const clap_plugin_t *p = create(m);
   set_params(p, set, nset);
   CHECK(p->activate(p, rate, 1, 512) && p->start_processing(p), "activate");
   Events ev = {{NULL, ev_size, ev_get}, {{{0}}}, 0};
   ev.api.ctx = &ev;
   clap_event_transport_t tr;
   memset(&tr, 0, sizeof tr);
   tr.flags = CLAP_TRANSPORT_HAS_TEMPO;
   tr.tempo = 120;
   for (int at = 0; at < frames; at += 512) {
      const int n = frames - at < 512 ? frames - at : 512;
      ev.n = 0;
      if (key >= 0 && at <= 100 && 100 < at + n)
         ev.v[ev.n++].n = (clap_event_note_t){{sizeof ev.v[0].n, (uint32_t)(100 - at), CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_ON, 0}, -1, 0, 0, (int16_t)key, note_vel};
      if (key >= 0 && at <= 100 + hold && 100 + hold < at + n)
         ev.v[ev.n++].n = (clap_event_note_t){{sizeof ev.v[0].n, (uint32_t)(100 + hold - at), CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_OFF, 0}, -1, 0, 0, (int16_t)key, 0.0};
      float *data[2] = {l + at, r + at};
      clap_audio_buffer_t out;
      memset(&out, 0, sizeof out);
      out.data32 = data;
      out.channel_count = 2;
      float *idata[2][2] = {{in_l ? in_l + at : NULL, in_r ? in_r + at : NULL}, {ext_l ? ext_l + at : NULL, ext_r ? ext_r + at : NULL}};
      clap_audio_buffer_t ins[2];
      memset(ins, 0, sizeof ins);
      for (int k = 0; k < 2; ++k) { ins[k].data32 = idata[k]; ins[k].channel_count = 2; }
      clap_process_t pr;
      memset(&pr, 0, sizeof pr);
      pr.frames_count = (uint32_t)n;
      pr.steady_time = -1;
      pr.transport = &tr;
      pr.audio_outputs = &out;
      pr.audio_outputs_count = 1;
      pr.audio_inputs = ins;
      pr.audio_inputs_count = in_l ? (ext_l ? 2 : 1) : 0;
      pr.in_events = &ev.api;
      CHECK(p->process(p, &pr) == CLAP_PROCESS_CONTINUE, "process");
   }
   p->stop_processing(p);
   p->deactivate(p);
   p->destroy(p);
   for (int i = 0; i < frames; ++i) CHECK(isfinite(l[i]) && isfinite(r[i]), "not finite at %d", i);
}
static double rms(const float *x, int a, int b) { double s = 0; for (int i = a; i < b; ++i) s += x[i] * x[i]; return sqrt(s / (b - a)); }
static double diff(const float *x, const float *y, int a, int b) { double s = 0; for (int i = a; i < b; ++i) s += (x[i] - y[i]) * (x[i] - y[i]); return sqrt(s / (b - a)); }
/* The level of the signal's movement: the spread of short-time RMS values. */
static double wobble(const float *x, int a, int b) {
   double lo = 1e9, hi = 0;
   for (int i = a; i + 1024 <= b; i += 1024) { const double v = rms(x, i, i + 1024); lo = v < lo ? v : lo; hi = v > hi ? v : hi; }
   return hi - lo;
}

typedef struct { int at, key, on; } Sched;
typedef struct { int step, id, v; } LockAt;
typedef struct { uint64_t trig; LockAt lock[4]; int nlock, stop_at, wobble; double bpm; int length, speed; double start; } Pattern;

/* Runs a schedule of notes, with an optional pattern on a playing transport from beat 0. */
static void schedule(int m, const Set *set, int nset, const Sched *notes, int nnotes, const Pattern *pat, float *l, float *r, int frames) {
   const clap_plugin_t *p = create(m);
   set_params(p, set, nset);
   if (pat) {
      Buffer b = {{0}, 0, 0};
      clap_ostream_t os = {&b, buf_write};
      clap_istream_t is = {&b, buf_read};
      const clap_plugin_state_t *st = p->get_extension(p, CLAP_EXT_STATE);
      CHECK(st->save(p, &os) && b.size == STATE_SIZE, "save");
      uint8_t *q = b.data + PAT;
      for (int i = 0; i < 8; ++i) q[i] = (uint8_t)(pat->trig >> (8 * i));
      q[8] = (uint8_t)(pat->length ? pat->length : 16);
      q[9] = (uint8_t)pat->speed;
      for (int i = 0; i < pat->nlock; ++i) q[10 + pat->lock[i].step * 56 + pat->lock[i].id] = (uint8_t)pat->lock[i].v;
      CHECK(st->load(p, &is), "load pattern");
   }
   CHECK(p->activate(p, 44100, 1, 512) && p->start_processing(p), "activate");
   Events ev = {{NULL, ev_size, ev_get}, {{{0}}}, 0};
   ev.api.ctx = &ev;
   clap_event_transport_t tr;
   memset(&tr, 0, sizeof tr);
   tr.tempo = pat && pat->bpm > 0 ? pat->bpm : 120;
   for (int at = 0; at < frames; at += 256) {
      const int n = frames - at < 256 ? frames - at : 256;
      ev.n = 0;
      for (int i = 0; i < nnotes; ++i)
         if (notes[i].at >= at && notes[i].at < at + n)
            ev.v[ev.n++].n = (clap_event_note_t){{sizeof ev.v[0].n, (uint32_t)(notes[i].at - at), CLAP_CORE_EVENT_SPACE_ID, (uint16_t)(notes[i].on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0}, -1, 0, 0, (int16_t)notes[i].key, 1.0};
      tr.flags = CLAP_TRANSPORT_HAS_TEMPO;
      if (pat && (!pat->stop_at || at < pat->stop_at)) {
         tr.flags |= CLAP_TRANSPORT_IS_PLAYING | CLAP_TRANSPORT_HAS_BEATS_TIMELINE;
         /* wobble: each block's position off by +/- that many samples, as a host's rounding can leave it */
         const double shifted = at + (pat->wobble ? (at / 256 & 1 ? -pat->wobble : pat->wobble) : 0);
         tr.song_pos_beats = (clap_beattime)llround((pat->start + shifted * (tr.tempo / 60.0 / 44100.0)) * CLAP_BEATTIME_FACTOR);
      }
      float *data[2] = {l + at, r + at};
      clap_audio_buffer_t out;
      memset(&out, 0, sizeof out);
      out.data32 = data;
      out.channel_count = 2;
      float *idata[2] = {in_l ? in_l + at : NULL, in_r ? in_r + at : NULL};
      clap_audio_buffer_t inb;
      memset(&inb, 0, sizeof inb);
      inb.data32 = idata;
      inb.channel_count = 2;
      clap_process_t pr;
      memset(&pr, 0, sizeof pr);
      pr.frames_count = (uint32_t)n;
      pr.steady_time = -1;
      pr.transport = &tr;
      pr.audio_outputs = &out;
      pr.audio_outputs_count = 1;
      pr.audio_inputs = &inb;
      pr.audio_inputs_count = in_l ? 1 : 0;
      pr.in_events = &ev.api;
      CHECK(p->process(p, &pr) == CLAP_PROCESS_CONTINUE, "process");
   }
   p->stop_processing(p);
   p->deactivate(p);
   p->destroy(p);
   for (int i = 0; i < frames; ++i) CHECK(isfinite(l[i]) && isfinite(r[i]), "not finite at %d", i);
}
static int crossings(const float *x, int a, int b) { int n = 0; for (int i = a + 1; i < b; ++i) n += x[i - 1] < 0 && x[i] >= 0; return n; }

static float base_l[N], base_r[N], l[N], r[N];

int main(int argc, char **argv) {
   CHECK(argc > 1, "usage");
   char path[1024];
   const char *dot = strrchr(argv[1], '/');
   snprintf(path, sizeof path, "%s/Contents/MacOS/Mono Track", argv[1]);
   (void)dot;
   void *lib = dlopen(path, RTLD_NOW);
   CHECK(lib, "dlopen %s", dlerror());
   const clap_plugin_entry_t *entry = dlsym(lib, "clap_entry");
   CHECK(entry && entry->init(path), "entry");
   factory = entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
   CHECK(factory && factory->get_plugin_count(factory) == 15, "fifteen plug-ins");

   static const char *const pages[8] = {"SYN", "AMP", "FILT", "EFX", "LFO1", "LFO2", "LFO3", "TRACK"};
   for (int m = 0; m < 15; ++m) {
      const clap_plugin_t *p = create(m);
      const clap_plugin_params_t *pa = p->get_extension(p, CLAP_EXT_PARAMS);
      const clap_plugin_remote_controls_t *rc = p->get_extension(p, CLAP_EXT_REMOTE_CONTROLS);
      CHECK(pa && pa->count(p) == NPARAM, "parameters");
      CHECK(rc && rc->count(p) == 8, "remote pages");
      for (uint32_t i = 0; i < 8; ++i) {
         clap_remote_controls_page_t pg;
         CHECK(rc->get(p, i, &pg) && !strcmp(pg.page_name, pages[i]), "page %u", i);
         for (int k = 0; k < 8; ++k) {
            const clap_id id = pg.param_ids[k];
            if (i == 7 && k >= 6) { CHECK(id == CLAP_INVALID_ID, "track page slot"); continue; }
            if (id == CLAP_INVALID_ID) { CHECK(i == 0, "only SYN has gaps"); continue; }
            clap_param_info_t info;
            CHECK(pa->get_info(p, id, &info) && !(info.flags & CLAP_PARAM_IS_HIDDEN), "page %u knob %d", i, k);
            CHECK(id == (i < 7 ? 8 * i + k : LEVEL + (unsigned)k), "page %u knob %d is id %u", i, k, id);
         }
      }
      char text[CLAP_NAME_SIZE];
      for (uint32_t i = 0; i < NPARAM; ++i) {   /* every value's text reads back to the same text, as the editor's typing needs */
         clap_param_info_t info;
         CHECK(pa->get_info(p, i, &info), "info");
         const bool dest = (i >= LFO && i < LEVEL && (i - LFO) % 8 == 1) || (i >= ASSIGN && (i - ASSIGN) % 3 == 1);
         double page_hi = 0;
         if (dest) { clap_param_info_t pi; CHECK(pa->get_info(p, i - 1, &pi), "page info"); page_hi = pi.max_value; }
         for (int pg = 0; pg <= page_hi; ++pg) {   /* DEST's names come from its PAGE */
            if (dest) set_params(p, &(Set){(int)i - 1, pg}, 1);
            for (int t = 0; t <= 16 * 127; ++t) {
               const double v = info.min_value + (info.max_value - info.min_value) * t / (16 * 127);
               const double x = info.flags & CLAP_PARAM_IS_STEPPED ? round(v) : v;
               char back[CLAP_NAME_SIZE];
               double y;
               CHECK(pa->value_to_text(p, i, x, text, sizeof text), "to text");
               CHECK(!((text[0] == '+' || text[0] == '-') && text[1] == '0' && strspn(text + 1, "0.") == strcspn(text + 1, " ")), "param %u at %g: a signed zero '%s'", i, x, text);
               CHECK(pa->text_to_value(p, i, text, &y), "from text %u '%s'", i, text);
               CHECK(pa->value_to_text(p, i, y, back, sizeof back) && !strcmp(text, back), "machine %d param %u: '%s' -> '%s'", m, i, text, back);
            }
         }
         if (dest) { clap_param_info_t pi; pa->get_info(p, i - 1, &pi); set_params(p, &(Set){(int)i - 1, pi.default_value}, 1); }
      }
      {   /* the state round trip, into a fresh instance; another machine refuses it */
         set_params(p, (Set[]){{SYN + 3, 99}, {AMP + 3, 17}, {LFO + 5, 3}, {LPKEY, 1}}, 4);
         Buffer b = {{0}, 0, 0};
         clap_ostream_t os = {&b, buf_write};
         clap_istream_t is = {&b, buf_read};
         const clap_plugin_state_t *st = p->get_extension(p, CLAP_EXT_STATE);
         CHECK(st->save(p, &os) && b.size == STATE_SIZE, "machine %d save", m);
         const clap_plugin_t *q = create(m);
         const clap_plugin_params_t *pq = q->get_extension(q, CLAP_EXT_PARAMS);
         CHECK(((const clap_plugin_state_t *)q->get_extension(q, CLAP_EXT_STATE))->load(q, &is), "machine %d load", m);
         for (uint32_t i = 0; i < NPARAM; ++i) {
            double x, y;
            CHECK(pa->get_value(p, i, &x) && pq->get_value(q, i, &y) && x == y, "machine %d param %u after load", m, i);
         }
         q->destroy(q);
         const clap_plugin_t *o = create((m + 1) % 15);
         b.pos = 0;
         CHECK(!((const clap_plugin_state_t *)o->get_extension(o, CLAP_EXT_STATE))->load(o, &is), "machine %d's state loads into %d", m, (m + 1) % 15);
         o->destroy(o);
      }
      p->destroy(p);
   }

   /* the LFO lists and DEST names */
   {
      const clap_plugin_t *p = create(2);
      const clap_plugin_params_t *pa = p->get_extension(p, CLAP_EXT_PARAMS);
      char t[CLAP_NAME_SIZE];
      CHECK(pa->value_to_text(p, LFO + 3, 10, t, sizeof t) && !strcmp(t, "RND"), "wave list: %s", t);
      {   /* ENS's PCH2-4 read as intervals, and typed semitones pick the nearest one */
         const clap_plugin_t *e = create(4);
         const clap_plugin_params_t *pe = e->get_extension(e, CLAP_EXT_PARAMS);
         static const struct { double v; const char *text; } pch[] = {{0, "-12 st"}, {63, "OFF"}, {127, "+12 st"}, {20, "-7.02 st (2/3)"}, {64, "+1 st"}, {104, "+7.02 st (3/2)"}};
         for (unsigned i = 0; i < sizeof pch / sizeof pch[0]; ++i)
            CHECK(pe->value_to_text(e, SYN + 1, pch[i].v, t, sizeof t) && !strcmp(t, pch[i].text), "PCH3 %g: %s", pch[i].v, t);
         double v;
         CHECK(pe->text_to_value(e, SYN, "+7", &v) && pe->value_to_text(e, SYN, v, t, sizeof t) && !strcmp(t, "+7 st"), "typed +7: %s", t);
         CHECK(pe->text_to_value(e, SYN, "7.02", &v) && pe->value_to_text(e, SYN, v, t, sizeof t) && !strcmp(t, "+7.02 st (3/2)"), "typed 7.02: %s", t);
         CHECK(pe->text_to_value(e, SYN, "off", &v) && pe->value_to_text(e, SYN, v, t, sizeof t) && !strcmp(t, "OFF"), "typed off: %s", t);
         CHECK(pe->value_to_text(e, SYN + 3, 20, t, sizeof t) && !strcmp(t, "20.0"), "WAVE stays a number: %s", t);
         CHECK(pe->text_to_value(e, SYN, "3/2", &v) && pe->value_to_text(e, SYN, v, t, sizeof t) && !strcmp(t, "+7.02 st (3/2)"), "typed 3/2: %s", t);
         CHECK(pe->value_to_text(e, SYN + 4, 64, t, sizeof t) && !strcmp(t, "0.0"), "ENS PW is bipolar: %s", t);
         e->destroy(e);
      }
      {   /* the SYN knobs the unit shows as words or ratios */
         static const struct { int machine, id; double v; const char *text; } shown[] = {
            {1, SYN + 2, 63, "OFF"}, {1, SYN + 2, 64, "ON"}, {3, SYN + 6, 64, "ON"}, {3, SYN + 4, 64, "0.0"},
            {5, SYN, 60, "1/2"}, {5, SYN, 27, "5/32"}, {5, SYN + 4, 5, "1/32"}, {5, SYN + 4, 127, "4"}, {6, SYN + 2, 85, "1.25"},
            {7, SYN, 0, "0.0"}, {7, SYN, 1, "1/64"}, {7, SYN, 3, ".046"}, {7, SYN, 64, "1.0"}, {7, SYN, 96, "1.5"}, {7, SYN, 71, "1.10"},
            {7, SYN, 127, "2.0"}, {7, SYN + 4, 2, ".001"}, {7, SYN + 4, 24, "9/64"}, {7, SYN + 4, 74, "1.33"}, {7, SYN + 4, 127, "4.0"},
            {7, SYN + 1, 64, "0.0"}, {7, SYN + 3, 60, "-4.0"},
         };
         static const struct { int machine, id; const char *typed, *text; } typed[] = {
            {5, SYN, "3/16", "3/16"}, {5, SYN, "1.3", "1.25"}, {6, SYN + 4, "0.2", "3/16"}, {7, SYN, "1.5", "1.5"}, {7, SYN, ".3", ".300"},
            {7, SYN + 4, "9/64", "9/64"}, {7, SYN + 4, "2", "2.0"}, {7, SYN + 4, "4", "4.0"}, {1, SYN + 2, "on", "ON"},
            {7, SYN + 1, "+0.0", "0.0"}, {7, SYN + 1, "-0", "0.0"}, {7, SYN + 1, "-0.04", "0.0"}, {7, SYN + 1, "-0.06", "-0.1"},
            {7, SYN, ".000", ".000"}, {7, SYN + 4, ".000", ".000"}, {7, SYN, ".125", ".125"}, {7, SYN, "1/8", "1/8"}, {7, SYN, "0", "0.0"},
         };
         for (unsigned i = 0; i < sizeof shown / sizeof shown[0]; ++i) {
            const clap_plugin_t *e = create(shown[i].machine);
            const clap_plugin_params_t *pe = e->get_extension(e, CLAP_EXT_PARAMS);
            CHECK(pe->value_to_text(e, shown[i].id, shown[i].v, t, sizeof t) && !strcmp(t, shown[i].text), "machine %d knob %d at %g: %s", shown[i].machine, shown[i].id, shown[i].v, t);
            e->destroy(e);
         }
         for (unsigned i = 0; i < sizeof typed / sizeof typed[0]; ++i) {
            const clap_plugin_t *e = create(typed[i].machine);
            const clap_plugin_params_t *pe = e->get_extension(e, CLAP_EXT_PARAMS);
            double v;
            CHECK(pe->text_to_value(e, typed[i].id, typed[i].typed, &v) && pe->value_to_text(e, typed[i].id, v, t, sizeof t) && !strcmp(t, typed[i].text), "machine %d knob %d typed %s: %s", typed[i].machine, typed[i].id, typed[i].typed, t);
            e->destroy(e);
         }
      }
      CHECK(pa->value_to_text(p, LFO, 1, t, sizeof t) && !strcmp(t, "SYNT"), "page list: %s", t);
      CHECK(pa->value_to_text(p, LFO + 3, 8, t, sizeof t) && !strcmp(t, "RMP"), "wave list: %s", t);
      CHECK(pa->value_to_text(p, LFO + 2, 4, t, sizeof t) && !strcmp(t, "HALF"), "trig list: %s", t);
      CHECK(pa->value_to_text(p, LFO + 4, 6, t, sizeof t) && !strcmp(t, "64X"), "mult list: %s", t);
      CHECK(pa->value_to_text(p, LFO + 1, 3, t, sizeof t) && !strcmp(t, "1OCT"), "PTCH dest: %s", t);
      set_params(p, (Set[]){{LFO, 3}}, 1);
      CHECK(pa->value_to_text(p, LFO + 1, 0, t, sizeof t) && !strcmp(t, "BASE"), "FILT dest: %s", t);
      set_params(p, (Set[]){{LFO, 1}}, 1);
      CHECK(pa->value_to_text(p, LFO + 1, 2, t, sizeof t) && !strcmp(t, "UNIX"), "SYN dest: %s", t);
      set_params(p, (Set[]){{LFO, 6}}, 1);
      CHECK(pa->value_to_text(p, LFO + 1, 7, t, sizeof t) && !strcmp(t, "DPTH"), "LFO dest: %s", t);
      set_params(p, (Set[]){{LFO, 8}}, 1);
      CHECK(pa->value_to_text(p, LFO + 1, 0, t, sizeof t) && !strcmp(t, "LEN"), "MIDI dest: %s", t);
      /* KIT ASSIGN: a cleared kit's rows, their names, and DEST read on its row's PAGE */
      {
         static const double def[12] = {2, 5, 64, 0, 0, 64, 1, 0, 64, 0, 0, 64};
         static const char *const names[12] = {"VEL 1 PAGE", "VEL 1 DEST", "VEL 1 ADD", "VEL 2 PAGE", "VEL 2 DEST", "VEL 2 ADD",
                                               "KEY 1 PAGE", "KEY 1 DEST", "KEY 1 ADD", "KEY 2 PAGE", "KEY 2 DEST", "KEY 2 ADD"};
         for (int i = 0; i < 12; ++i) {
            clap_param_info_t info;
            double x;
            CHECK(pa->get_info(p, ASSIGN + i, &info) && !strcmp(info.name, names[i]) && !strcmp(info.module, "ASSIGN"), "ASSIGN %d: %s", i, info.name);
            CHECK(info.default_value == def[i] && pa->get_value(p, ASSIGN + i, &x) && x == def[i], "ASSIGN %d default", i);
            CHECK((info.flags & CLAP_PARAM_IS_STEPPED) && !(info.flags & CLAP_PARAM_IS_MODULATABLE), "ASSIGN %d flags", i);
            CHECK(info.max_value == (i % 3 == 0 ? 8 : i % 3 == 1 ? 7 : 127), "ASSIGN %d range", i);
         }
         CHECK(pa->value_to_text(p, ASSIGN, 2, t, sizeof t) && !strcmp(t, "AMP"), "VEL 1 PAGE: %s", t);
         CHECK(pa->value_to_text(p, ASSIGN + 1, 5, t, sizeof t) && !strcmp(t, "VOL"), "VEL 1 DEST: %s", t);
         CHECK(pa->value_to_text(p, ASSIGN + 7, 0, t, sizeof t) && !strcmp(t, "UNIL"), "KEY 1 DEST on SYNT: %s", t);
         CHECK(pa->value_to_text(p, ASSIGN + 10, 3, t, sizeof t) && !strcmp(t, "1OCT"), "KEY 2 DEST on PTCH: %s", t);
         CHECK(pa->value_to_text(p, ASSIGN + 2, 0, t, sizeof t) && !strcmp(t, "-64"), "ADD -64: %s", t);
         CHECK(pa->value_to_text(p, ASSIGN + 2, 127, t, sizeof t) && !strcmp(t, "+63"), "ADD +63: %s", t);
         double v;
         CHECK(pa->text_to_value(p, ASSIGN + 2, "-10", &v) && v == 54, "typed ADD");
         CHECK(pa->value_to_text(p, ASSIGN + 2, 64, t, sizeof t) && !strcmp(t, "0"), "ADD 0 has no sign: %s", t);
         CHECK(pa->text_to_value(p, ASSIGN + 2, "+0", &v) && v == 64 && pa->text_to_value(p, ASSIGN + 2, "0", &v) && v == 64, "typed ADD 0");
         CHECK(pa->text_to_value(p, ASSIGN + 6, "lfo2", &v) && v == 6, "typed PAGE");
         set_params(p, (Set[]){{ASSIGN + 6, 4}}, 1);
         CHECK(pa->value_to_text(p, ASSIGN + 7, 3, t, sizeof t) && !strcmp(t, "DTIM"), "KEY 1 DEST on EFFX: %s", t);
         set_params(p, (Set[]){{ASSIGN + 6, 1}}, 1);
      }

      /* state */
      set_params(p, (Set[]){{SYN, 33}, {FILT + 1, 77}, {EFX + 3, 21}, {LFO + 8 + 3, 5}, {LEVEL, 90}, {HPKEY, 0}, {ASSIGN + 2, 100}, {ASSIGN + 9, 7}}, 8);
      Buffer b = {{0}, 0, 0};
      clap_ostream_t os = {&b, buf_write};
      clap_istream_t is = {&b, buf_read};
      const clap_plugin_state_t *st = p->get_extension(p, CLAP_EXT_STATE);
      CHECK(st->save(p, &os) && b.size == STATE_SIZE, "save");
      const clap_plugin_t *q = create(2);
      const clap_plugin_state_t *sq = q->get_extension(q, CLAP_EXT_STATE);
      const clap_plugin_params_t *pq = q->get_extension(q, CLAP_EXT_PARAMS);
      CHECK(sq->load(q, &is), "load");
      for (uint32_t i = 0; i < NPARAM; ++i) { double x, y; pa->get_value(p, i, &x); pq->get_value(q, i, &y); CHECK(x == y, "state param %u", i); }
      const clap_plugin_t *other = create(3);
      b.pos = 0;
      CHECK(!((const clap_plugin_state_t *)other->get_extension(other, CLAP_EXT_STATE))->load(other, &is), "another machine's state");
      other->destroy(other);
      b.pos = 0; b.data[7] = 9;
      CHECK(!sq->load(q, &is), "unknown version");
      b.pos = 0; b.data[7] = 2; memset(b.data + 8 + 8 * LEVEL, 0xff, 8);
      CHECK(!sq->load(q, &is), "NaN value");
      /* a state saved by 0.1.0: 61 parameters, no pattern */
      {
         Buffer a = {{0}, 0, 0}, c = {{0}, 0, 0};
         clap_ostream_t oa = {&a, buf_write}, oc = {&c, buf_write};
         clap_istream_t ia = {&a, buf_read};
         CHECK(st->save(p, &oa), "save");
         a.data[7] = 1;
         a.size = 8 + 8 * 61;
         CHECK(sq->load(q, &ia), "load a 0.1.0 state");
         for (uint32_t i = 0; i < 61; ++i) { double x, y; pa->get_value(p, i, &x); pq->get_value(q, i, &y); CHECK(x == y, "0.1.0 param %u", i); }
         double x;
         pq->get_value(q, POLY, &x); CHECK(x == 0, "POLY default");
         for (int i = TRIG_SYN; i <= TRIG_LFO; ++i) { pq->get_value(q, (clap_id)i, &x); CHECK(x == 1, "MIDI TRIG default"); }
         CHECK(sq->save(q, &oc), "save");
         for (int i = 0; i < 8; ++i) CHECK(!c.data[PAT + i], "no trigs");
         CHECK(c.data[PAT + 8] == 16 && c.data[PAT + 9] == 0, "16 steps at 1X");
         for (int i = 0; i < 64 * 56; ++i) CHECK(c.data[PAT + 10 + i] == 0xff, "no locks");
         pq->get_value(q, LOCK_END, &x); CHECK(x == 1, "NOTE ENDS default");
         a.pos = 0; a.size = 8 + 8 * 61 - 1;
         CHECK(!sq->load(q, &ia), "a short state");
         /* states saved by 0.2.0 (65 parameters) and 0.3/0.4 (66): a 16-step pattern, two trig bytes */
         for (int version = 2; version <= 3; ++version) {
            const int count = version == 2 ? 65 : ASSIGN;
            Buffer old = {{0}, 0, 0}, back = {{0}, 0, 0};
            clap_ostream_t ov = {&old, buf_write}, ob = {&back, buf_write};
            clap_istream_t iv = {&old, buf_read};
            CHECK(st->save(p, &ov), "save");
            uint8_t *pat = old.data + 8 + 8 * count;
            memset(pat, 0xff, 2 + 16 * 56);
            pat[0] = 0x21; pat[1] = 0x80;
            pat[2 + 7 * 56 + AMP + 5] = 33;
            old.data[7] = (uint8_t)version;
            old.size = 8 + 8 * count + 2 + 16 * 56;
            CHECK(sq->load(q, &iv), "load a version %d state", version);
            pq->get_value(q, LOCK_END, &x); CHECK(version == 3 || x == 1, "0.2.0: NOTE ENDS default");
            CHECK(sq->save(q, &ob), "save");
            const uint8_t *r2 = back.data + PAT;
            CHECK(r2[0] == 0x21 && r2[1] == 0x80 && !r2[2] && r2[8] == 16 && r2[9] == 0, "version %d pattern head", version);
            CHECK(r2[10 + 7 * 56 + AMP + 5] == 33 && (int8_t)r2[10 + 16 * 56 + AMP + 5] == -1, "version %d locks", version);
            old.pos = 0; old.size--;
            CHECK(!sq->load(q, &iv), "a short version %d state", version);
         }
         /* a lock out of range, a length or speed out of range */
         a.pos = 0; a.size = c.size; memcpy(a.data, c.data, c.size); a.data[PAT + 10 + LFO] = 100;   /* LFO PAGE has 9 entries */
         CHECK(!sq->load(q, &ia), "lock out of range");
         const int bad[4][2] = {{8, 1}, {8, 65}, {9, 4}, {9, 0xff}};
         for (int i = 0; i < 4; ++i) {
            a.pos = 0; memcpy(a.data, c.data, c.size); a.data[PAT + bad[i][0]] = (uint8_t)bad[i][1];
            CHECK(!sq->load(q, &ia), "pattern byte %d = %d", bad[i][0], bad[i][1]);
         }
         a.pos = 0; memcpy(a.data, c.data, c.size); a.data[PAT + 8] = 64; a.data[PAT + 9] = 3; a.data[PAT + 7] = 0x80; a.data[PAT + 10 + 63 * 56 + AMP] = 5;
         CHECK(sq->load(q, &ia), "a 64-step pattern at 3/2X");
         Buffer d = {{0}, 0, 0};
         clap_ostream_t od = {&d, buf_write};
         CHECK(sq->save(q, &od) && !memcmp(d.data + PAT, a.data + PAT, 10 + 64 * 56), "64 steps round trip");
         /* a state saved by 0.5: 66 parameters and 64 steps; KIT ASSIGN's rows come back as a cleared kit's */
         {
            Buffer v4 = {{0}, 0, 0};
            clap_istream_t i4 = {&v4, buf_read};
            memcpy(v4.data, a.data, 8 + 8 * ASSIGN);
            memcpy(v4.data + 8 + 8 * ASSIGN, a.data + PAT, 10 + 64 * 56);
            v4.data[7] = 4;
            v4.size = 8 + 8 * ASSIGN + 10 + 64 * 56;
            CHECK(sq->load(q, &i4), "load a 0.5 state");
            pq->get_value(q, ASSIGN + 2, &x); CHECK(x == 64, "0.5: VEL 1 ADD default");
            pq->get_value(q, ASSIGN + 9, &x); CHECK(x == 0, "0.5: KEY 2 PAGE default");
            pq->get_value(q, HPKEY, &x); CHECK(x == 0, "0.5: HP KEY kept");
            Buffer e = {{0}, 0, 0};
            clap_ostream_t oe = {&e, buf_write};
            CHECK(sq->save(q, &oe) && e.data[7] == 6 && !memcmp(e.data + PAT, a.data + PAT, 10 + 64 * 56), "0.5 pattern kept");
            v4.pos = 0; v4.size--;
            CHECK(!sq->load(q, &i4), "a short 0.5 state");
         }
         /* the editor's width: none until it is opened; kept; fitted to what the editor can have; not in a preset,
            and none in a state saved by 0.6 to 0.8.3 */
         {
            const clap_plugin_state_context_t *cq = q->get_extension(q, CLAP_EXT_STATE_CONTEXT);
            const struct { int version, in; uint32_t from, to; int out; } w[] = {
               {6, 0, 0, 0, 0}, {6, 1710, 0, 0, 1710}, {6, 5000, 0, 0, 2736}, {6, 10, 0, 0, 821}, {5, 0, 0, 0, 0},
               {6, 1710, CLAP_STATE_CONTEXT_FOR_PROJECT, CLAP_STATE_CONTEXT_FOR_DUPLICATE, 1710},
               {6, 1500, CLAP_STATE_CONTEXT_FOR_PRESET, 0, 1710},                                /* a preset leaves it alone */
               {6, 1500, CLAP_STATE_CONTEXT_FOR_DUPLICATE, CLAP_STATE_CONTEXT_FOR_PRESET, 0},   /* and is saved without one */
            };
            CHECK(cq, "state-context");
            for (int i = 0; i < (int)(sizeof w / sizeof *w); ++i) {
               Buffer in = c, back = {{0}, 0, 0};
               clap_istream_t ii = {&in, buf_read};
               clap_ostream_t ob = {&back, buf_write};
               in.pos = 0;
               in.data[7] = (uint8_t)w[i].version;
               in.size = w[i].version < 6 ? WIDTH : STATE_SIZE;
               in.data[WIDTH] = (uint8_t)w[i].in; in.data[WIDTH + 1] = (uint8_t)(w[i].in >> 8);
               CHECK(w[i].from ? cq->load(q, &ii, w[i].from) : sq->load(q, &ii), "width %d load", i);
               CHECK((w[i].to ? cq->save(q, &ob, w[i].to) : sq->save(q, &ob)) && back.size == STATE_SIZE, "width %d save", i);
               const int got = back.data[WIDTH] | back.data[WIDTH + 1] << 8;
               CHECK(got == w[i].out, "width %d: %d", i, got);
            }
         }
      }
      q->destroy(q);
      p->destroy(p);
   }

   /* every synth machine sounds and fades, at 44.1 and 48 kHz */
   for (int m = 0; m < FX0; ++m)
      for (int rt = 0; rt < 2; ++rt) {
         render(m, NULL, 0, 60, 22050, rt ? 48000 : 44100, l, r, N);
         CHECK(rms(l, 1000, 20000) > 1e-4, "machine %d rate %d silent", m, rt);
         CHECK(rms(l, N - 8000, N) < rms(l, 1000, 20000) * 0.2, "machine %d rate %d does not fade", m, rt);
      }
   {
      const clap_plugin_t *p = create(2);
      const clap_plugin_latency_t *lat = p->get_extension(p, CLAP_EXT_LATENCY);
      p->activate(p, 44100, 1, 512); CHECK(lat->get(p) == 0, "latency at 44.1"); p->deactivate(p);
      p->activate(p, 48000, 1, 512); CHECK(lat->get(p) > 0 && lat->get(p) < 200, "latency at 48"); p->deactivate(p);
      p->destroy(p);
   }

   /* each section is in the path (SWAVE SAW) */
   render(2, NULL, 0, 48, 66150, 44100, base_l, base_r, N);
   static const struct { const char *name; Set s; } sections[] = {
      {"EQ", {EFX + 1, 110}}, {"filter", {FILT + 1, 40}}, {"DIST", {AMP + 4, 120}}, {"SRR", {EFX + 2, 60}},
      {"delay", {EFX + 4, 127}}, {"LEVEL", {LEVEL, 50}}, {"PAN", {AMP + 6, 10}}, {"VOL", {AMP + 5, 30}},
   };
   for (unsigned i = 0; i < sizeof sections / sizeof sections[0]; ++i) {
      render(2, &sections[i].s, 1, 48, 66150, 44100, l, r, N);
      CHECK(diff(l, base_l, 2000, N) > 1e-4, "%s has no effect", sections[i].name);
   }
   /* the delay's echoes outlast the note */
   {
      const Set echo[3] = {{EFX + 4, 127}, {EFX + 5, 90}, {AMP + 3, 10}}, dry[2] = {{EFX + 4, 64}, {AMP + 3, 10}};
      render(2, dry, 2, 60, 6000, 44100, base_l, base_r, N);
      render(2, echo, 3, 60, 6000, 44100, l, r, N);
      CHECK(rms(l, 40000, N) > 20 * rms(base_l, 40000, N) + 1e-5, "no echoes");
   }
   /* key tracking: a high note through a low filter */
   {
      const Set on[1] = {{FILT + 1, 30}}, off[2] = {{FILT + 1, 30}, {LPKEY, 0}};
      render(2, on, 1, 96, 66150, 44100, base_l, base_r, N);
      render(2, off, 2, 96, 66150, 44100, l, r, N);
      CHECK(rms(base_l, 2000, 30000) > 2 * rms(l, 2000, 30000), "LP KEY: %g vs %g", rms(base_l, 2000, 30000), rms(l, 2000, 30000));
   }
   /* KIT ASSIGN: velocity and key move their rows' targets, and do nothing at ADD 0 (the default) */
   {
      note_vel = 0.25;
      render(2, NULL, 0, 60, 22050, 44100, base_l, base_r, N);
      note_vel = 1.0;
      render(2, NULL, 0, 60, 22050, 44100, l, r, N);
      CHECK(!memcmp(base_l, l, sizeof l), "velocity changed the sound at ADD 0");
      const Set vel[1] = {{ASSIGN + 2, 127}};   /* VEL 1: AMP VOL, ADD +63 */
      static float soft[N], loud[N], tmp[N];
      note_vel = 0.25;
      render(2, vel, 1, 60, 22050, 44100, soft, tmp, N);
      note_vel = 1.0;
      render(2, vel, 1, 60, 22050, 44100, loud, tmp, N);
      const double r0 = rms(l, 2000, 20000), r1 = rms(soft, 2000, 20000), r2 = rms(loud, 2000, 20000);
      CHECK(r2 > 1.2 * r1 && r1 > 1.2 * r0, "VEL to VOL: %g %g %g", r0, r1, r2);
      /* KEY 2 on PTCH 1OCT, ADD +63: note 60 adds 60 x 63 x 48 to word 30, 0.48 octave */
      const Set key[3] = {{ASSIGN + 10, 3}, {ASSIGN + 11, 127}, {AMP + 2, 127}}, plain[1] = {{AMP + 2, 127}};
      render(0, plain, 1, 60, 30000, 44100, base_l, base_r, N);
      render(0, key, 3, 60, 30000, 44100, l, r, N);
      const double ratio = (double)crossings(l, 4000, 28000) / crossings(base_l, 4000, 28000);
      CHECK(fabs(ratio - pow(2, 60 * 63 * 48 / 65536.0 * 352 / 2048)) < 0.02, "KEY to PTCH: ratio %g", ratio);
   }

   /* LFOs: one on the filter moves the sound; another on the first one's depth changes that */
   {
      const Set still[2] = {{AMP + 2, 127}, {FILT + 1, 50}};   /* DEC 127 holds the note, so only the LFO moves it */
      const Set moving[8] = {{AMP + 2, 127}, {FILT + 1, 50}, {LFO, 3}, {LFO + 1, 1}, {LFO + 3, 0}, {LFO + 4, 3}, {LFO + 5, 64}, {LFO + 7, 90}};
      const Set both[14] = {{AMP + 2, 127}, {FILT + 1, 50}, {LFO, 3}, {LFO + 1, 1}, {LFO + 3, 0}, {LFO + 4, 3}, {LFO + 5, 64}, {LFO + 7, 90},
                            {LFO + 8, 5}, {LFO + 9, 7}, {LFO + 11, 4}, {LFO + 12, 2}, {LFO + 13, 40}, {LFO + 15, 127}};
      render(2, still, 2, 48, 88000, 44100, base_l, base_r, N);
      render(2, moving, 8, 48, 88000, 44100, l, r, N);
      const double w0 = wobble(base_l, 8000, N), w1 = wobble(l, 8000, N);
      CHECK(w1 > 3 * w0, "LFO on FILT: %g vs %g", w1, w0);
      static float m2[N], m3[N];
      render(2, both, 14, 48, 88000, 44100, m2, m3, N);
      CHECK(diff(m2, l, 8000, N) > 1e-3, "LFO2 on LFO1's DPTH has no effect");
   }

   /* POLY: six whole tracks */
   {
      static float a[7][N], tmp[N], sum[N];
      static const int keys[7] = {40, 47, 52, 59, 64, 71, 76};
      const Set poly[1] = {{POLY, 1}};
      for (int mm = 2; mm <= 5; mm += 3) {
         render(mm, NULL, 0, 60, 22050, 44100, base_l, base_r, N);
         render(mm, poly, 1, 60, 22050, 44100, l, r, N);
         CHECK(!memcmp(base_l, l, sizeof l), "machine %d: one POLY note is not the mono note", mm);
         for (int i = 0; i < 7; ++i) render(mm, NULL, 0, keys[i], 22050, 44100, a[i], tmp, N);
         Sched off[14];   /* each key's on and off */
         for (int i = 0; i < 7; ++i) { off[2 * i] = (Sched){100, keys[i], 1}; off[2 * i + 1] = (Sched){100 + 22050, keys[i], 0}; }
         schedule(mm, poly, 1, off, 6, NULL, l, r, N);
         memset(sum, 0, sizeof sum);
         for (int i = 0; i < 3; ++i) for (int n = 0; n < N; ++n) sum[n] += a[i][n];
         double worst = 0; for (int n = 0; n < N; ++n) worst = fmax(worst, fabs(sum[n] - l[n]));
         CHECK(worst < 1e-5, "machine %d: a chord is not the sum of its notes (%g)", mm, worst);
         /* seven notes at once: the first is stolen, the last six sound */
         schedule(mm, poly, 1, off, 14, NULL, l, r, N);
         memset(sum, 0, sizeof sum);
         for (int i = 1; i < 7; ++i) for (int n = 0; n < N; ++n) sum[n] += a[i][n];
         worst = 0; for (int n = 0; n < N; ++n) worst = fmax(worst, fabs(sum[n] - l[n]));
         CHECK(worst < 1e-5, "machine %d: voice stealing (%g)", mm, worst);
      }
   }

   /* the MIDI TRIG switches */
   {
      const Sched two[2] = {{100, 48, 1}, {30000, 55, 1}};
      Set a[10] = {{AMP + 2, 110}, {FILT + 1, 35}, {FILT + 5, 50}, {FILT + 7, 127}, {LFO, 3}, {LFO + 1, 1}, {LFO + 2, 1}, {LFO + 5, 8}, {LFO + 7, 60}, {TRIG_SYN, 1}};
      schedule(2, a, 10, two, 2, NULL, base_l, base_r, N);
      CHECK(rms(base_l, 2000, 20000) > 1e-3, "all on sounds");
      a[9] = (Set){TRIG_SYN, 0};
      schedule(2, a, 10, two, 2, NULL, l, r, N);
      CHECK(rms(l, 0, N) < 1e-7, "SYN off: a note must not sound (%g)", rms(l, 0, N));
      a[9] = (Set){TRIG_FILT, 0};
      schedule(2, a, 10, two, 2, NULL, l, r, N);
      CHECK(rms(l, 2000, 20000) > 1e-3 && diff(l, base_l, 2000, 20000) > 1e-4, "FILT off must change the sound");
      a[9] = (Set){TRIG_LFO, 0};
      schedule(2, a, 10, two, 2, NULL, l, r, N);
      CHECK(diff(l, base_l, 32000, 60000) > 1e-4, "LFO off: the second note must not restart the LFO");
   }

   /* the trigs, from the transport. A step at 120 BPM is 5512.5 frames. */
   {
      const int S = 5513;
      Set hold[3] = {{AMP + 2, 127}, {AMP + 3, 10}, {LEVEL, 100}};   /* DEC 127: a note sounds until it ends */
      const Sched key[1] = {{100, 48, 1}};
      Pattern pt = {0};

      /* a trig plays no note, with or without an earlier MIDI note */
      pt.trig = 0xffff;
      schedule(2, hold, 3, NULL, 0, &pt, l, r, N);
      CHECK(rms(l, 0, N) < 1e-7, "trigs alone must be silent: %g", rms(l, 0, N));
      {
         const Sched gone[2] = {{100, 48, 1}, {400, 48, 0}};
         Set shortnote[3] = {{AMP + 2, 40}, {AMP + 3, 10}, {LEVEL, 100}};
         schedule(2, shortnote, 3, gone, 2, &pt, l, r, N);
         CHECK(rms(l, 8 * S, N) < 1e-6, "a trig must not replay the last note: %g", rms(l, 8 * S, N));
      }
      /* a trig restarts the filter envelope of a held note, and leaves the sound alone before it */
      {
         Set f[6] = {{AMP + 2, 127}, {AMP + 3, 10}, {FILT + 1, 35}, {FILT + 5, 50}, {FILT + 7, 127}, {LEVEL, 100}};
         pt = (Pattern){0};
         schedule(2, f, 6, key, 1, &pt, base_l, base_r, N);
         pt.trig = 1 << 8;
         schedule(2, f, 6, key, 1, &pt, l, r, N);
         CHECK(diff(l, base_l, 200, 8 * S - 200) < 1e-7, "no change before the trig");
         CHECK(diff(l, base_l, 8 * S + 200, 11 * S) > 1e-4, "a trig must restart the filter envelope");
      }
      /* and the LFOs, in TRIG mode */
      {
         Set w[9] = {{AMP + 2, 127}, {FILT + 1, 50}, {LFO, 3}, {LFO + 1, 1}, {LFO + 2, 1}, {LFO + 5, 8}, {LFO + 7, 90}, {TRIG_LFO, 0}, {LEVEL, 100}};
         pt = (Pattern){0};
         schedule(2, w, 9, key, 1, &pt, base_l, base_r, N);
         pt.trig = 1 << 8;
         schedule(2, w, 9, key, 1, &pt, l, r, N);
         CHECK(diff(l, base_l, 8 * S + 800, N) > 1e-4, "a trig must restart the LFO");
      }

      /* a lock, then the kit value at the next trig */
      pt = (Pattern){1 << 0 | 1 << 8, {{0, AMP + 5, 0}}, 1};
      schedule(2, hold, 3, key, 1, &pt, l, r, N);
      CHECK(rms(l, 400, 8 * S - 400) < 1e-6, "VOL locked to 0: %g", rms(l, 400, 8 * S - 400));
      CHECK(rms(l, 8 * S + 600, 10 * S) > 1e-3, "the kit VOL returns at the next trig");

      /* a trigless lock acts at its step and holds until the next trig */
      pt = (Pattern){1 << 8, {{4, AMP + 5, 0}}, 1};
      schedule(2, hold, 3, key, 1, &pt, l, r, N);
      CHECK(rms(l, 800, 4 * S - 200) > 1e-3, "before the trigless lock");
      CHECK(rms(l, 4 * S + 800, 8 * S - 200) < 1e-5, "after the trigless lock: %g", rms(l, 4 * S + 800, 8 * S - 200));
      CHECK(rms(l, 8 * S + 800, 10 * S) > 1e-3, "back at the next trig");

      /* a MIDI note ends a lock, unless the switch is off */
      {
         const Sched two[2] = {{100, 48, 1}, {6 * S, 55, 1}};
         pt = (Pattern){0, {{4, AMP + 5, 0}}, 1};
         schedule(2, hold, 3, two, 2, &pt, l, r, N);
         CHECK(rms(l, 4 * S + 800, 6 * S - 200) < 1e-5, "locked until the note");
         CHECK(rms(l, 6 * S + 800, 8 * S) > 1e-3, "a MIDI note must end the lock");
         Set off[4] = {{AMP + 2, 127}, {AMP + 3, 10}, {LEVEL, 100}, {LOCK_END, 0}};
         schedule(2, off, 4, two, 2, &pt, l, r, N);
         CHECK(rms(l, 6 * S + 800, N) < 1e-5, "NOTE ENDS off: the lock must hold (%g)", rms(l, 6 * S + 800, N));
      }
      /* a note on a locked step keeps that step's lock, even a sample ahead of the step */
      for (int early = 0; early <= 1; ++early) {
         const int at = (int)ceil(4 * 5512.5) - early;
         const Sched on[2] = {{100, 48, 1}, {at, 55, 1}};
         pt = (Pattern){0, {{4, AMP + 5, 0}}, 1};
         schedule(2, hold, 3, on, 2, &pt, l, r, N);
         CHECK(rms(l, at + 800, 6 * S) < 1e-5, "a note on its own locked step, %d early: %g", early, rms(l, at + 800, 6 * S));
      }

      /* a note a sample ahead of the next step still ends the previous step's lock */
      {
         const Sched on[2] = {{100, 48, 1}, {(int)ceil(4 * 5512.5) - 1, 55, 1}};
         pt = (Pattern){0, {{3, AMP + 5, 0}}, 1};
         schedule(2, hold, 3, on, 2, &pt, l, r, N);
         CHECK(rms(l, 4 * S + 800, 6 * S) > 1e-3, "a note just ahead of its step must end the last step's lock");
      }

      /* a host position that rounds backward over a step boundary does not play the last step again */
      {
         const int step = 5376;                     /* whole blocks, so the wobble lands on the boundary */
         const double bpm = 44100.0 * 15.0 / step;
         /* step 5 starts on an odd block: the block before runs 8 samples ahead, this one 8 behind */
         const Sched on[2] = {{100, 48, 1}, {5 * step - 1, 55, 1}};
         pt = (Pattern){0, {{4, AMP + 5, 0}}, 1};
         pt.bpm = bpm;
         pt.wobble = 8;
         schedule(2, hold, 3, on, 2, &pt, l, r, N);
         CHECK(rms(l, 4 * step + 800, 5 * step - 200) < 1e-5, "the lock before the note");
         CHECK(rms(l, 5 * step + 800, 7 * step) > 1e-3, "a step played twice: its lock came back after the note ended it");
      }

      /* an LFO-page lock reaches the LFO: lock its depth on the first step */
      {
         Set wob[8] = {{AMP + 2, 127}, {FILT + 1, 50}, {LFO, 3}, {LFO + 1, 1}, {LFO + 3, 0}, {LFO + 4, 3}, {LFO + 5, 64}, {LFO + 7, 0}};
         pt = (Pattern){1};
         schedule(2, wob, 8, key, 1, &pt, base_l, base_r, N);
         pt = (Pattern){1, {{0, LFO + 7, 90}}, 1};
         schedule(2, wob, 8, key, 1, &pt, l, r, N);
         CHECK(wobble(l, 8000, N) > 3 * wobble(base_l, 8000, N), "a lock on LFO DPTH");
      }

      /* stop: locks return to the kit, and a held MIDI note keeps sounding */
      pt = (Pattern){1, {{0, AMP + 5, 20}}, 1};
      pt.stop_at = 3 * S;
      schedule(2, hold, 3, key, 1, &pt, l, r, N);
      CHECK(rms(l, 800, 3 * S - 200) > 1e-4, "sounds before stop");
      CHECK(rms(l, 3 * S + 800, N) > 2 * rms(l, 800, 3 * S - 200), "stop must return VOL to the kit and keep the note");

      /* 64 steps: from beat 9.75 the playhead is on step 39, so a lock on step 40 plays a step later;
       * at the default 16 steps that step is past the end and never plays */
      for (int len = 16; len <= 64; len += 48) {
         pt = (Pattern){0, {{40, AMP + 5, 0}}, 1};
         pt.length = len;
         pt.start = 39 / 4.0;
         schedule(2, hold, 3, key, 1, &pt, l, r, N);
         if (len == 64) CHECK(rms(l, S + 800, N) < 1e-5, "step 40 of 64: %g", rms(l, S + 800, N));
         else CHECK(rms(l, S + 800, N) > 1e-3, "step 40 of 16 must not play");
      }
      /* a short length wraps: six steps, the lock on step 0 comes back at the seventh sixteenth */
      for (int len = 6; len <= 16; len += 10) {
         pt = (Pattern){1 | 1 << 4, {{0, AMP + 5, 0}}, 1};
         pt.length = len;
         schedule(2, hold, 3, key, 1, &pt, l, r, N);
         CHECK(rms(l, 4 * S + 800, 6 * S - 200) > 1e-3, "kit VOL after the trig on step 4");
         if (len == 6) CHECK(rms(l, 6 * S + 800, 10 * S - 200) < 1e-5, "length 6 wraps to the lock: %g", rms(l, 6 * S + 800, 10 * S - 200));
         else CHECK(rms(l, 6 * S + 800, 10 * S - 200) > 1e-3, "length 16 does not wrap yet");
      }
      /* the speeds: 6, 3, 8 or 4 MIDI clocks a step. A lock on step k plays k * clocks / 6 sixteenths in. */
      {
         const int clocks[4] = {6, 3, 8, 4}, k[4] = {8, 8, 6, 12};
         for (int sp = 0; sp < 4; ++sp) {
            const int t = (int)ceil(k[sp] * clocks[sp] / 6.0 * 5512.5);
            pt = (Pattern){0, {{k[sp], AMP + 5, 0}}, 1};
            pt.speed = sp;
            schedule(2, hold, 3, key, 1, &pt, l, r, N);
            CHECK(rms(l, t - 2 * S + 400, t - 400) > 1e-3, "speed %d: before step %d", sp, k[sp]);
            CHECK(rms(l, t + 800, t + 2 * S) < 1e-5, "speed %d: step %d at %d", sp, k[sp], t);
         }
      }
      /* a start in the middle of the song: at beat 6 a 12-step pattern is on its step 0, a 16-step one on step 8 */
      for (int len = 12; len <= 16; len += 4) {
         pt = (Pattern){0, {{2, AMP + 5, 0}}, 1};
         pt.length = len;
         pt.start = 6;
         schedule(2, hold, 3, key, 1, &pt, l, r, N);
         if (len == 12) CHECK(rms(l, 2 * S + 800, 4 * S) < 1e-5, "12 steps from beat 6: step 2 at the third sixteenth");
         else CHECK(rms(l, 2 * S + 800, 4 * S) > 1e-3 && rms(l, 10 * S + 800, 12 * S) < 1e-5, "16 steps from beat 6: step 2 at the eleventh");
      }

      /* POLY: locks reach every voice (the chord's VOL goes to 0) */
      {
         Set p2[4] = {{AMP + 2, 127}, {AMP + 3, 10}, {POLY, 1}, {LEVEL, 100}};
         const Sched chord[2] = {{100, 48, 1}, {100, 60, 1}};
         pt = (Pattern){0, {{2, AMP + 5, 0}}, 1};
         schedule(2, p2, 4, chord, 2, &pt, l, r, N);
         CHECK(rms(l, 800, 2 * S - 200) > 1e-3, "chord before the lock");
         CHECK(rms(l, 2 * S + 1000, 4 * S) < 1e-5, "a lock must reach all voices: %g", rms(l, 2 * S + 1000, 4 * S));
      }
   }

   /* the FX machines */
   {
      static float sl[N], sr[N], xl[N], xr[N];
      for (int i = 0; i < N; ++i) {   /* a chord of two tones as the input, a 1 kHz tone as the sidechain */
         sl[i] = (float)(0.3 * sin(2 * M_PI * 220 * i / 44100.0) + 0.2 * sin(2 * M_PI * 331 * i / 44100.0));
         sr[i] = (float)(0.3 * sin(2 * M_PI * 277 * i / 44100.0));
         xl[i] = xr[i] = (float)(0.5 * sin(2 * M_PI * 1000 * i / 44100.0));
      }
      for (int m = 0; m < 15; ++m) {
         const clap_plugin_t *p = create(m);
         const clap_plugin_audio_ports_t *ap = p->get_extension(p, CLAP_EXT_AUDIO_PORTS);
         const clap_plugin_params_t *pa = p->get_extension(p, CLAP_EXT_PARAMS);
         const clap_plugin_latency_t *lat = p->get_extension(p, CLAP_EXT_LATENCY);
         const uint32_t ins = m < FX0 ? 0 : m == FX0 + 4 ? 2 : 1;
         CHECK(ap->count(p, true) == ins && ap->count(p, false) == 1, "machine %d ports", m);
         for (uint32_t i = 0; i < ins; ++i) {
            clap_audio_port_info_t info;
            CHECK(ap->get(p, i, true, &info) && info.id == i && info.channel_count == 2 && !!(info.flags & CLAP_AUDIO_PORT_IS_MAIN) == !i, "machine %d input %u", m, i);
         }
         const clap_plugin_descriptor_t *d = factory->get_plugin_descriptor(factory, (uint32_t)m);
         CHECK(!strcmp(d->features[0], m < FX0 ? CLAP_PLUGIN_FEATURE_INSTRUMENT : CLAP_PLUGIN_FEATURE_AUDIO_EFFECT), "machine %d features", m);
         clap_param_info_t info;
         CHECK(pa->get_info(p, AMP + 2, &info) && info.default_value == (m < FX0 ? 64 : 127), "machine %d DEC default", m);
         CHECK(pa->get_info(p, LPKEY, &info) && info.default_value == (m < FX0), "machine %d LP KEY default", m);
         p->activate(p, 44100, 1, 512); CHECK(lat->get(p) == (m < FX0 ? 0u : 16u), "machine %d latency %u", m, lat->get(p)); p->deactivate(p);
         p->activate(p, 48000, 1, 512); CHECK(lat->get(p) > (m < FX0 ? 0u : 16u) && lat->get(p) < 250, "machine %d latency at 48: %u", m, lat->get(p)); p->deactivate(p);
         p->destroy(p);
      }
      in_l = sl; in_r = sr;
      for (int m = FX0; m < 15; ++m)
         for (int rt = 0; rt < 2; ++rt) {   /* the input passes with no note, from the start, and stays */
            render(m, NULL, 0, -1, 0, rt ? 48000 : 44100, l, r, N);
            CHECK(rms(l, 30000, N) > 0.01 && rms(r, 30000, N) > 0.01, "machine %d rate %d: %g %g", m, rt, rms(l, 30000, N), rms(r, 30000, N));
            CHECK(rms(l, 12000, 20000) > 0.5 * rms(l, N - 8000, N), "machine %d rate %d: late", m, rt);
            const Set mute[1] = {{SYN + 7, 0}};
            render(m, mute, 1, -1, 0, rt ? 48000 : 44100, l, r, N);
            CHECK(rms(l, 30000, N) < 1e-4, "machine %d rate %d: INP 0 lets through %g", m, rt, rms(l, 30000, N));
         }
      {   /* THRU: one block late at 44.1 kHz; with DEC and REL down the AMP closes, and a note reopens it */
         render(FX0, NULL, 0, -1, 0, 44100, l, r, N);
         CHECK(rms(l, 0, 16) == 0 && rms(l, 16, 32) > 0, "THRU: the first block is silence");
         const Set shut[2] = {{AMP + 2, 20}, {AMP + 3, 20}};
         const Sched note[2] = {{44100, 60, 1}, {46100, 60, 0}};
         schedule(FX0, shut, 2, note, 2, NULL, l, r, N);
         CHECK(rms(l, 40000, 44100) < 1e-4, "THRU: the envelope does not close: %g", rms(l, 40000, 44100));
         CHECK(rms(l, 44300, 46000) > 0.01, "THRU: the note does not open it: %g", rms(l, 44300, 46000));
      }
      {   /* RINGMOD: with EXT and MIX up, the sidechain is the carrier */
         const Set ext[2] = {{SYN + 1, 127}, {SYN + 3, 127}};
         render(FX0 + 4, ext, 2, -1, 0, 44100, l, r, N);
         CHECK(rms(l, 30000, N) < 1e-3, "RINGMOD: no sidechain, no carrier: %g", rms(l, 30000, N));
         ext_l = xl; ext_r = xr;
         render(FX0 + 4, ext, 2, -1, 0, 44100, l, r, N);
         CHECK(rms(l, 30000, N) > 0.01, "RINGMOD: the sidechain does not carry: %g", rms(l, 30000, N));
         render(FX0 + 4, ext, 2, -1, 0, 48000, l, r, N);
         CHECK(rms(l, 30000, N) > 0.01, "RINGMOD: the sidechain at 48 kHz: %g", rms(l, 30000, N));
         ext_l = ext_r = NULL;
      }
      {   /* POLY on an effect: each voice is a whole FX track, all on the same input */
         const Set poly[1] = {{POLY, 1}};
         const Sched chord[2] = {{100, 48, 1}, {100, 60, 1}};
         schedule(FX0 + 4, poly, 1, chord, 2, NULL, l, r, N);
         CHECK(rms(l, 30000, N) > 0.01, "POLY RINGMOD: %g", rms(l, 30000, N));
      }
      in_l = in_r = NULL;
      render(FX0 + 1, NULL, 0, 60, 1000, 44100, l, r, N);   /* no input buffer: silence */
      CHECK(rms(l, 0, N) == 0, "REVERB without input: %g", rms(l, 0, N));
   }

   /* speed */
   {
      const clock_t t0 = clock();
      render(5, NULL, 0, 60, 66150, 44100, l, r, N);
      printf("FM+ STAT: %.1f%% of one core in real time\n", 100.0 * (double)(clock() - t0) / CLOCKS_PER_SEC / ((double)N / 44100));
   }
   entry->deinit();
   puts("PASS");
   return 0;
}
