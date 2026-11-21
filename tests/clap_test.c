/* Loads the built bundle like a host and checks what the core test cannot:
 * the wrapper around mono_track.h. usage: clap_test "build/Mono Track.clap"
 *  - the bundle offers the eight machines, each with 61 parameters and the
 *    eight remote-control pages (SYN AMP FILT EFX LFO1 LFO2 LFO3 TRACK)
 *  - a note sounds and its release fades, at 44.1 and 48 kHz, and the latency
 *    is reported
 *  - each section is in the signal path: EQ, filter, DIST, SRR, delay, LEVEL
 *  - key tracking opens the low-pass for high notes only when LP KEY is on
 *  - an LFO on page FILT moves the sound, and a second LFO aimed at the first
 *    one's DPTH changes that movement
 *  - LFO list knobs show their entries, and DEST names follow PAGE
 *  - state round trip, rejected bad states, parameter text round trip */
#include <clap/clap.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); puts(""); exit(1); } } while (0)
#define NPARAM 61
#define N 88200
enum { SYN = 0, AMP = 8, FILT = 16, EFX = 24, LFO = 32, LEVEL = 56, LPKEY, HPKEY, BEND, MTUNE };
static const char *const ids[8] = {"sin", "nois", "saw", "puls", "ens", "fmstat", "fmpar", "fmdyn"};

typedef union { clap_event_header_t h; clap_event_param_value_t v; clap_event_note_t n; } Event;
typedef struct { clap_input_events_t api; Event v[NPARAM + 4]; uint32_t n; } Events;
static uint32_t ev_size(const clap_input_events_t *e) { return ((const Events *)e->ctx)->n; }
static const clap_event_header_t *ev_get(const clap_input_events_t *e, uint32_t i) { return &((const Events *)e->ctx)->v[i].h; }
static const void *host_ext(const clap_host_t *h, const char *id) { (void)h; (void)id; return NULL; }
static void host_cb(const clap_host_t *h) { (void)h; }
static const clap_host_t host = {CLAP_VERSION_INIT, NULL, "test", "", "", "1", host_ext, host_cb, host_cb, host_cb};
static const clap_plugin_factory_t *factory;

typedef struct { uint8_t data[1024]; uint64_t size, pos; } Buffer;
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
/* A note from frame 100 for `hold` frames; the output, frame by frame. */
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
      if (at <= 100 && 100 < at + n)
         ev.v[ev.n++].n = (clap_event_note_t){{sizeof ev.v[0].n, (uint32_t)(100 - at), CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_ON, 0}, -1, 0, 0, (int16_t)key, 1.0};
      if (at <= 100 + hold && 100 + hold < at + n)
         ev.v[ev.n++].n = (clap_event_note_t){{sizeof ev.v[0].n, (uint32_t)(100 + hold - at), CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_OFF, 0}, -1, 0, 0, (int16_t)key, 0.0};
      float *data[2] = {l + at, r + at};
      clap_audio_buffer_t out;
      memset(&out, 0, sizeof out);
      out.data32 = data;
      out.channel_count = 2;
      clap_process_t pr;
      memset(&pr, 0, sizeof pr);
      pr.frames_count = (uint32_t)n;
      pr.steady_time = -1;
      pr.transport = &tr;
      pr.audio_outputs = &out;
      pr.audio_outputs_count = 1;
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
   CHECK(factory && factory->get_plugin_count(factory) == 8, "eight plug-ins");

   static const char *const pages[8] = {"SYN", "AMP", "FILT", "EFX", "LFO1", "LFO2", "LFO3", "TRACK"};
   for (int m = 0; m < 8; ++m) {
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
            if (i == 7 && k >= 5) { CHECK(id == CLAP_INVALID_ID, "track page slot"); continue; }
            if (id == CLAP_INVALID_ID) { CHECK(i == 0, "only SYN has gaps"); continue; }
            clap_param_info_t info;
            CHECK(pa->get_info(p, id, &info) && !(info.flags & CLAP_PARAM_IS_HIDDEN), "page %u knob %d", i, k);
            CHECK(id == (i < 7 ? 8 * i + k : LEVEL + (unsigned)k), "page %u knob %d is id %u", i, k, id);
         }
      }
      char text[CLAP_NAME_SIZE];
      for (uint32_t i = 0; i < NPARAM; ++i) {   /* every value's text reads back to the same text */
         clap_param_info_t info;
         CHECK(pa->get_info(p, i, &info), "info");
         for (int t = 0; t < 8; ++t) {
            const double v = info.min_value + (info.max_value - info.min_value) * t / 7;
            const double x = info.flags & CLAP_PARAM_IS_STEPPED ? round(v) : v;
            char back[CLAP_NAME_SIZE];
            double y;
            CHECK(pa->value_to_text(p, i, x, text, sizeof text), "to text");
            CHECK(pa->text_to_value(p, i, text, &y), "from text %u '%s'", i, text);
            CHECK(pa->value_to_text(p, i, y, back, sizeof back) && !strcmp(text, back), "param %u: '%s' -> '%s'", i, text, back);
         }
      }
      p->destroy(p);
   }

   /* the LFO lists and DEST names */
   {
      const clap_plugin_t *p = create(2);
      const clap_plugin_params_t *pa = p->get_extension(p, CLAP_EXT_PARAMS);
      char t[CLAP_NAME_SIZE];
      CHECK(pa->value_to_text(p, LFO + 3, 10, t, sizeof t) && !strcmp(t, "RND"), "wave list: %s", t);
      CHECK(pa->value_to_text(p, LFO + 2, 4, t, sizeof t) && !strcmp(t, "HALF"), "trig list: %s", t);
      CHECK(pa->value_to_text(p, LFO + 4, 6, t, sizeof t) && !strcmp(t, "64X"), "mult list: %s", t);
      CHECK(pa->value_to_text(p, LFO + 1, 3, t, sizeof t) && !strcmp(t, "1OCT"), "PTCH dest: %s", t);
      set_params(p, (Set[]){{LFO, 3}}, 1);
      CHECK(pa->value_to_text(p, LFO + 1, 0, t, sizeof t) && !strcmp(t, "BASE"), "FILT dest: %s", t);
      set_params(p, (Set[]){{LFO, 1}}, 1);
      CHECK(pa->value_to_text(p, LFO + 1, 2, t, sizeof t) && !strcmp(t, "UNIX"), "SYN dest: %s", t);
      set_params(p, (Set[]){{LFO, 6}}, 1);
      CHECK(pa->value_to_text(p, LFO + 1, 7, t, sizeof t) && !strcmp(t, "DPTH"), "LFO dest: %s", t);

      /* state */
      set_params(p, (Set[]){{SYN, 33}, {FILT + 1, 77}, {EFX + 3, 21}, {LFO + 8 + 3, 5}, {LEVEL, 90}, {HPKEY, 0}}, 6);
      Buffer b = {{0}, 0, 0};
      clap_ostream_t os = {&b, buf_write};
      clap_istream_t is = {&b, buf_read};
      const clap_plugin_state_t *st = p->get_extension(p, CLAP_EXT_STATE);
      CHECK(st->save(p, &os) && b.size == 8 + 8 * NPARAM, "save");
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
      b.pos = 0; b.data[7] = 1; memset(b.data + 8 + 8 * LEVEL, 0xff, 8);
      CHECK(!sq->load(q, &is), "NaN value");
      q->destroy(q);
      p->destroy(p);
   }

   /* every machine sounds and fades, at 44.1 and 48 kHz */
   for (int m = 0; m < 8; ++m)
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
