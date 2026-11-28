/* Loads the built bundle, opens its editor in an off-screen window and drives it
 * with real AppKit events. usage: gui_test "build/Mono Track.clap" [snapshot-dir]
 *  - trigs: a click toggles; a long press does not; right-click and ctrl-click
 *    latch a hold; a drag on a knob then writes a lock, not the kit value
 *  - clicking a knob while holding locks / unlocks it; CLR LOCKS clears the step
 *  - VEL and KEY SETUP open KIT ASSIGN's rows in windows that take the mouse while open
 *  - the keyboard is ignored until KEYBOARD CAPTURE is on; 12345678qwertyui then hold
 *    trigs, by physical key; other keys stay the host's
 *  - holds end when the window hides, closes, or (for keys) loses focus
 *  - a drag is one gesture; a drag cut short by closing still ends its gesture
 *  - a modulated knob keeps its needle at the base; turning a stepped knob shows no modulation
 *  - a MIDI note draws a line under its step, which fades
 *  - a scroll or drag belongs to the trigs held when it began: momentum after a key
 *    is released moves neither the kit value nor the lock
 *  - CPY and PST under a selected trig copy its trig and all its locks
 *  - a press on an LFO tab slides across the tabs; turning PAGE redraws DEST's name
 *  - each LFO tab graphs its LFO: a moving dot redraws it, a still or very fast LFO does not,
 *    a moving dot trails a blue plume, and a very fast LFO's whole wave fades to pink
 *  - SCALE: the speed cycles; a page button shows its page on the pads, and one past the
 *    end extends the pattern, filling each empty page it reaches as the unit does; LEN
 *    drags, and a shorter pattern sends the pads back to page 1; switching pages lets go
 *    of held trigs; the playing page's button shows the playhead
 *  - a knob's or slider's value lights up under the mouse; a double-click on it types a
 *    new one in any of its forms (numbers, names, ratios, Hz), Enter or a click elsewhere
 *    takes it, Esc drops it, text that reads as no value beeps; while a step is held it
 *    types the lock; a double-click elsewhere on the knob still resets it
 *  - an FX machine's editor (PHASER): its unused knob takes no typing, its INP
 *    reads 0.0 at 64 and takes a typed -6
 *  - with no font, or an exception while drawing, the editor draws on and logs it
 *  - a trig is one named step in the host's undo, a lock drag one step, and no change stays open */
#import <Cocoa/Cocoa.h>
#include <dlfcn.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <clap/ext/draft/undo.h>
#include "../source/mono_track_ui.h"

#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); puts(""); exit(1); } } while (0)
enum { PAT = 8 + 8 * NPARAM, LOCKS = PAT + 10, STATE = LOCKS + UI_STEPS * UI_LOCKS };

static int flushes, begins, ends, values;
static void host_flush(const clap_host_params_t *x);
static void h_rescan(const clap_host_t *h, clap_param_rescan_flags f) { (void)h; (void)f; }
static void h_clear(const clap_host_t *h, clap_id i, clap_param_clear_flags f) { (void)h; (void)i; (void)f; }
static void h_flush(const clap_host_t *h) { (void)h; ++flushes; }
static const clap_host_params_t host_params = {h_rescan, h_clear, h_flush};
/* the host's undo: each pattern edit is a step, a drag one step; a change left open fails the test */
static int undoOpen, undoSteps, undoCancels;
static char undoName[64];
static void u_begin(const clap_host_t *h) { (void)h; CHECK(!undoOpen, "begin_change twice"); undoOpen = 1; }
static void u_cancel(const clap_host_t *h) { (void)h; CHECK(undoOpen, "cancel_change with none open"); undoOpen = 0; ++undoCancels; }
static void u_made(const clap_host_t *h, const char *name, const void *d, size_t n, bool can) {
   (void)h; (void)d; (void)n; (void)can;
   CHECK(name && *name, "change_made without a name");
   snprintf(undoName, sizeof undoName, "%s", name);
   undoOpen = 0;
   ++undoSteps;
}
static void u_redo(const clap_host_t *h) { (void)h; }
static void u_sub(const clap_host_t *h, bool b) { (void)h; (void)b; }
static const clap_host_undo_t host_undo = {u_begin, u_cancel, u_made, u_redo, u_redo, u_sub};
static const void *host_ext(const clap_host_t *h, const char *id) {
   (void)h;
   return !strcmp(id, CLAP_EXT_PARAMS) ? (const void *)&host_params : !strcmp(id, CLAP_EXT_UNDO) ? (const void *)&host_undo : NULL;
}
static void host_cb(const clap_host_t *h) { (void)h; }
static const clap_host_t host = {CLAP_VERSION_INIT, NULL, "test", "", "", "1", host_ext, host_cb, host_cb, host_cb};

static bool out_push(const clap_output_events_t *o, const clap_event_header_t *e) {
   (void)o;
   begins += e->type == CLAP_EVENT_PARAM_GESTURE_BEGIN;
   ends += e->type == CLAP_EVENT_PARAM_GESTURE_END;
   values += e->type == CLAP_EVENT_PARAM_VALUE;
   return true;
}
static const clap_output_events_t out = {NULL, out_push};
static uint32_t no_size(const clap_input_events_t *e) { (void)e; return 0; }
static const clap_event_header_t *no_get(const clap_input_events_t *e, uint32_t i) { (void)e; (void)i; return NULL; }
static const clap_input_events_t none = {NULL, no_size, no_get};

typedef struct { uint8_t d[STATE + 16]; uint64_t n, pos; } Buf;
static int64_t w(const clap_ostream_t *s, const void *p, uint64_t n) { Buf *b = s->ctx; memcpy(b->d + b->n, p, n); b->n += n; return (int64_t)n; }
static int64_t r(const clap_istream_t *s, void *p, uint64_t n) { Buf *b = s->ctx; if (n > b->n - b->pos) n = b->n - b->pos; memcpy(p, b->d + b->pos, n); b->pos += n; return (int64_t)n; }

static const clap_plugin_t *P;
static const clap_plugin_params_t *PA;
static NSWindow *W;
static NSView *V;

/* the editor's redraw requests, recorded */
static NSRect dirty[256];
static int ndirty;
static void (*invalidate)(id, SEL, NSRect);
static void recordInvalidate(id self, SEL s, NSRect r) { if (ndirty < 256) dirty[ndirty++] = r; invalidate(self, s, r); }
static BOOL redrawn(CGFloat x, CGFloat y) {
   for (int i = 0; i < ndirty; ++i)
      if (NSPointInRect(NSMakePoint(x, y), dirty[i])) return YES;
   return NO;
}
/* an off-screen window gets no display-link ticks, so the test runs the view's refresh itself */
static void pump(void) {
   [NSRunLoop.mainRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
   if (V) [V performSelector:NSSelectorFromString(@"tick:") withObject:nil];
}
static void flush(void) { PA->flush(P, &none, &out); }
static NSPoint at(CGFloat x, CGFloat y) { return [V convertPoint:NSMakePoint(x, y) toView:nil]; }
static void mouse(NSEventType t, CGFloat x, CGFloat y, NSEventModifierFlags m, int clicks) {
   /* an off-screen window of a background app gets no dispatch from AppKit, so the view takes the event directly */
   NSEvent *e = [NSEvent mouseEventWithType:t location:at(x, y) modifierFlags:m timestamp:0 windowNumber:W.windowNumber context:nil eventNumber:0 clickCount:clicks pressure:1];
   if (t == NSEventTypeLeftMouseDown) [V mouseDown:e];
   else if (t == NSEventTypeLeftMouseDragged) [V mouseDragged:e];
   else if (t == NSEventTypeLeftMouseUp) [V mouseUp:e];
   else if (t == NSEventTypeRightMouseDown) [V rightMouseDown:e];
}
static void click(CGFloat x, CGFloat y) { mouse(NSEventTypeLeftMouseDown, x, y, 0, 1); mouse(NSEventTypeLeftMouseUp, x, y, 0, 1); pump(); }
static void dbl(CGFloat x, CGFloat y) { click(x, y); mouse(NSEventTypeLeftMouseDown, x, y, 0, 2); mouse(NSEventTypeLeftMouseUp, x, y, 0, 2); pump(); }
static void right(CGFloat x, CGFloat y) { mouse(NSEventTypeRightMouseDown, x, y, 0, 1); mouse(NSEventTypeRightMouseUp, x, y, 0, 1); pump(); }
static void key(NSEventType t, unsigned short code) {
   NSEvent *e = [NSEvent keyEventWithType:t location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:W.windowNumber context:nil characters:@"x" charactersIgnoringModifiers:@"x" isARepeat:NO keyCode:code];
   if (W.firstResponder != V) return;   /* what AppKit does: only the first responder sees keys */
   if (t == NSEventTypeKeyDown) [V keyDown:e]; else [V keyUp:e];
}
static void tap(unsigned short code) { key(NSEventTypeKeyDown, code); key(NSEventTypeKeyUp, code); pump(); }
static CGFloat padx(int i) { return i < 8 ? 43 + 54 * i : 505 + 54 * (i - 8); }
#define PADY 329
/* knob k of section s */
static CGFloat kx(int s, int k) { return 16 + 240 * s + 6 + (k % 4) * 54 + 27; }
static CGFloat ky(int k) { return 52 + (k / 4) * 106 + 50; }
static void drag(CGFloat x, CGFloat y, CGFloat dy) {
   mouse(NSEventTypeLeftMouseDown, x, y, 0, 1);
   for (int i = 1; i <= 8; ++i) mouse(NSEventTypeLeftMouseDragged, x, y + dy * i / 8, 0, 1);
   mouse(NSEventTypeLeftMouseUp, x, y + dy, 0, 1);
   pump();
}
static Buf save(void) {
   Buf b;
   memset(&b, 0, sizeof b);
   clap_ostream_t os = {&b, w};
   CHECK(((const clap_plugin_state_t *)P->get_extension(P, CLAP_EXT_STATE))->save(P, &os) && b.n == STATE, "save");
   return b;
}
static uint64_t trigs(void) { Buf b = save(); uint64_t t = 0; for (int i = 0; i < 8; ++i) t |= (uint64_t)b.d[PAT + i] << (8 * i); return t; }
static int lockv(int step, int id) { Buf b = save(); return (int8_t)b.d[LOCKS + step * UI_LOCKS + id]; }
static int length(void) { return save().d[PAT + 8]; }
static int speed(void) { return save().d[PAT + 9]; }
static double val(int id) { double v; PA->get_value(P, (clap_id)id, &v); return v; }
static void shot(const char *dir, const char *name) {
   if (!dir) return;
   pump();
   [V setNeedsDisplay:YES];
   NSBitmapImageRep *rep = [V bitmapImageRepForCachingDisplayInRect:V.bounds];
   [V cacheDisplayInRect:V.bounds toBitmapImageRep:rep];
   NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
   [png writeToFile:[NSString stringWithFormat:@"%s/%s.png", dir, name] atomically:YES];
}

static uint64_t viewHash(void) {
   const NSRect knobs = NSMakeRect(0, 44, MT_GUI_W - 150, 226);   /* the panels: the trig LEDs pulse with time */
   [V setNeedsDisplay:YES];
   NSBitmapImageRep *rep = [V bitmapImageRepForCachingDisplayInRect:knobs];
   [V cacheDisplayInRect:knobs toBitmapImageRep:rep];
   uint64_t h = 0xcbf29ce484222325ULL;
   const unsigned char *d = rep.bitmapData;
   for (NSInteger i = 0; i < rep.bytesPerRow * rep.pixelsHigh; ++i) h = (h ^ d[i]) * 0x100000001b3ULL;
   return h;
}
/* Pixels of the host-modulation colour in the knob panels. */
static int violet(void) {
   const NSRect knobs = NSMakeRect(0, 44, MT_GUI_W - 150, 226);
   [V setNeedsDisplay:YES];
   NSBitmapImageRep *rep = [V bitmapImageRepForCachingDisplayInRect:knobs];
   [V cacheDisplayInRect:knobs toBitmapImageRep:rep];
   int n = 0;
   const unsigned char *d = rep.bitmapData;
   for (NSInteger y = 0; y < rep.pixelsHigh; ++y)
      for (NSInteger x = 0; x < rep.pixelsWide; ++x) {
         const unsigned char *q = d + y * rep.bytesPerRow + x * rep.bitsPerPixel / 8;
         n += q[2] > 200 && q[0] > 130 && q[0] < 210 && q[1] < 170 && q[2] - q[1] > 70;
      }
   return n;
}
/* How bright the blue MIDI-note line under step i is, 0..1. */
static double noteLine(int i) {
   const NSRect line = NSMakeRect(padx(i) - 15, 355, 30, 3);
   [V setNeedsDisplay:YES];
   NSBitmapImageRep *rep = [V bitmapImageRepForCachingDisplayInRect:line];
   [V cacheDisplayInRect:line toBitmapImageRep:rep];
   const unsigned char *q = rep.bitmapData + (rep.pixelsHigh / 2) * rep.bytesPerRow + (rep.pixelsWide / 2) * rep.bitsPerPixel / 8;
   const double b = (q[2] - 24.0) / (255 - 24.0);   /* the window's blue is 0x18 */
   return q[2] > q[0] + 8 && b > 0.01 ? b : 0;
}
/* The colour at a point of the view: red, green, blue. */
static void pixel(CGFloat x, CGFloat y, int c[3]) {
   const NSRect r = NSMakeRect(x - 1, y - 1, 2, 2);
   [V setNeedsDisplay:YES];
   NSBitmapImageRep *rep = [V bitmapImageRepForCachingDisplayInRect:r];
   [V cacheDisplayInRect:r toBitmapImageRep:rep];
   const unsigned char *q = rep.bitmapData + (rep.pixelsHigh / 2) * rep.bytesPerRow + (rep.pixelsWide / 2) * rep.bitsPerPixel / 8;
   for (int i = 0; i < 3; ++i) c[i] = q[i];
}
/* Pixels in a rect of the view that are clearly blue. */
static int blue(NSRect r) {
   [V setNeedsDisplay:YES];
   NSBitmapImageRep *rep = [V bitmapImageRepForCachingDisplayInRect:r];
   [V cacheDisplayInRect:r toBitmapImageRep:rep];
   int n = 0;
   for (NSInteger y = 0; y < rep.pixelsHigh; ++y)
      for (NSInteger x = 0; x < rep.pixelsWide; ++x) {
         const unsigned char *q = rep.bitmapData + y * rep.bytesPerRow + x * rep.bitsPerPixel / 8;
         n += q[2] > 120 && q[2] > q[0] + 40;
      }
   return n;
}
/* A trackpad or wheel event. phase: 1 began, 2 changed, 4 ended, 128 may begin. momentum: 1 begin, 2 continue, 3 end. */
static void scroll(CGFloat x, CGFloat y, int dy, int phase, int momentum, BOOL trackpad) {
   CGEventRef ev = CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitPixel, 1, dy);
   CGEventSetIntegerValueField(ev, kCGScrollWheelEventIsContinuous, trackpad);
   CGEventSetIntegerValueField(ev, kCGScrollWheelEventScrollPhase, phase);
   CGEventSetIntegerValueField(ev, kCGScrollWheelEventMomentumPhase, momentum);
   const NSPoint w = at(x, y);   /* an event with no window reports screen points as its window location */
   CGEventSetLocation(ev, CGPointMake(w.x, NSScreen.screens[0].frame.size.height - w.y));
   [V scrollWheel:[NSEvent eventWithCGEvent:ev]];
   CFRelease(ev);
}
static int brightIn(NSRect r) {
   [V setNeedsDisplay:YES];
   NSBitmapImageRep *rep = [V bitmapImageRepForCachingDisplayInRect:r];
   [V cacheDisplayInRect:r toBitmapImageRep:rep];
   int n = 0;
   const unsigned char *d = rep.bitmapData;
   for (NSInteger y = 0; y < rep.pixelsHigh; ++y)
      for (NSInteger x = 0; x < rep.pixelsWide; ++x) {
         const unsigned char *q = d + y * rep.bytesPerRow + x * rep.bitsPerPixel / 8;
         n += q[0] > 150 && q[1] > 150 && q[2] > 150;
      }
   return n;
}
static NSRect padButton(int i, BOOL paste) { return NSMakeRect(padx(i) + (paste ? 1 : -26), 364, 25, 20); }
static void clickButton(int i, BOOL paste) { const NSRect b = padButton(i, paste); click(NSMidX(b), NSMidY(b)); }
static BOOL sameStep(int a, int b) {
   Buf x = save();
   const uint64_t t = trigs();
   return (t >> a & 1) == (t >> b & 1) && !memcmp(x.d + LOCKS + a * UI_LOCKS, x.d + LOCKS + b * UI_LOCKS, UI_LOCKS);
}
typedef struct { clap_input_events_t api; const clap_event_header_t *e; } One;
static uint32_t one_size(const clap_input_events_t *x) { return ((const One *)x->ctx)->e ? 1 : 0; }
static const clap_event_header_t *one_get(const clap_input_events_t *x, uint32_t i) { (void)i; return ((const One *)x->ctx)->e; }
/* a host's value change, through the parameters' flush */
static void setp(int id, double v) {
   const clap_event_param_value_t e = {{sizeof e, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0}, (clap_id)id, NULL, -1, -1, -1, -1, v};
   One in = {{NULL, one_size, one_get}, &e.header};
   in.api.ctx = &in;
   PA->flush(P, &in.api, &out);
}
static void process(const clap_event_header_t *e, double beats, bool playing) {
   static float a[256], b[256];
   float *data[2] = {a, b};
   One in = {{NULL, one_size, one_get}, e};
   in.api.ctx = &in;
   clap_audio_buffer_t o;
   memset(&o, 0, sizeof o);
   o.data32 = data;
   o.channel_count = 2;
   clap_event_transport_t tr;
   memset(&tr, 0, sizeof tr);
   tr.flags = CLAP_TRANSPORT_HAS_TEMPO | (playing ? CLAP_TRANSPORT_IS_PLAYING | CLAP_TRANSPORT_HAS_BEATS_TIMELINE : 0);
   tr.tempo = 120;
   tr.song_pos_beats = (clap_beattime)(beats * CLAP_BEATTIME_FACTOR);
   clap_process_t pr;
   memset(&pr, 0, sizeof pr);
   pr.frames_count = 256;
   pr.steady_time = -1;
   pr.transport = &tr;
   pr.audio_outputs = &o;
   pr.audio_outputs_count = 1;
   pr.in_events = &in.api;
   pr.out_events = &out;
   CHECK(P->process(P, &pr) == CLAP_PROCESS_CONTINUE, "process");
}

int main(int argc, char **argv) {
   @autoreleasepool {
   CHECK(argc > 1, "usage");
   const char *dir = argc > 2 ? argv[2] : NULL;
   char home[1024], logs[1100];   /* the editor's log lands here, not in the user's Library */
   snprintf(home, sizeof home, "%s/mt_gui_test.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
   CHECK(mkdtemp(home), "mkdtemp %s", home);
   snprintf(logs, sizeof logs, "mkdir -p '%s/Library/Logs'", home);
   CHECK(!system(logs) && !setenv("HOME", home, 1), "the test's HOME");
   [NSApplication sharedApplication];
   [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];   /* never takes focus or shows on screen */
   char path[1024];
   snprintf(path, sizeof path, "%s/Contents/MacOS/Mono Track", argv[1]);
   void *lib = dlopen(path, RTLD_NOW);
   CHECK(lib, "dlopen %s", dlerror());
   const clap_plugin_entry_t *entry = dlsym(lib, "clap_entry");
   CHECK(entry && entry->init(path), "entry");
   const clap_plugin_factory_t *f = entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
   P = f->create_plugin(f, &host, "works.reconstruction.monotrack.fmstat");
   CHECK(P && P->init(P), "create");
   PA = P->get_extension(P, CLAP_EXT_PARAMS);
   const clap_plugin_gui_t *gui = P->get_extension(P, CLAP_EXT_GUI);
   CHECK(gui && gui->is_api_supported(P, CLAP_WINDOW_API_COCOA, false) && !gui->is_api_supported(P, CLAP_WINDOW_API_COCOA, true), "api");
   CHECK(gui->create(P, CLAP_WINDOW_API_COCOA, false), "create gui");
   uint32_t gw, gh;
   CHECK(gui->get_size(P, &gw, &gh) && gw == MT_GUI_W && gh == MT_GUI_H, "size");
   W = [[NSWindow alloc] initWithContentRect:NSMakeRect(-6000, -6000, gw, gh) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
   W.releasedWhenClosed = NO;
   clap_window_t cw = {CLAP_WINDOW_API_COCOA, {(__bridge void *)W.contentView}};
   CHECK(gui->set_parent(P, &cw) && gui->show(P), "parent/show");
   V = W.contentView.subviews.firstObject;
   CHECK(V, "the editor view");
   shot(dir, "1_default");

   /* no font (it closed Bitwig's plug-in host on 2026-11-27) skips the text, and an exception
      stays inside drawing; both go to $HOME/Library/Logs/Mono Track.log */
   {
      Method m = class_getClassMethod(NSFont.class, @selector(monospacedSystemFontOfSize:weight:));
      IMP font = method_getImplementation(m);
      NSBitmapImageRep *rep = [V bitmapImageRepForCachingDisplayInRect:V.bounds];
      method_setImplementation(m, imp_implementationWithBlock(^NSFont *(id c, CGFloat s, NSFontWeight w) { (void)c, (void)s, (void)w; return nil; }));
      [V cacheDisplayInRect:V.bounds toBitmapImageRep:rep];
      method_setImplementation(m, imp_implementationWithBlock(^NSFont *(id c, CGFloat s, NSFontWeight w) {
         (void)c, (void)s, (void)w;
         [NSException raise:@"TestFont" format:@"thrown by gui_test"];
         return nil;
      }));
      [V cacheDisplayInRect:V.bounds toBitmapImageRep:rep];
      method_setImplementation(m, font);
      NSString *log = [NSString stringWithContentsOfFile:[NSString stringWithFormat:@"%s/Library/Logs/Mono Track.log", getenv("HOME")]
                                                encoding:NSUTF8StringEncoding error:nil];
      CHECK([log containsString:@"drawText \"MIDI TRIGS\" skipped: font 0x0"] && [log containsString:@"drawRect: TestFont: thrown by gui_test"],
            "the log: %s", log.UTF8String);
   }

   /* a press on an LFO tab slides across the tabs; leaving them keeps the last */
   mouse(NSEventTypeLeftMouseDown, 976 + 30, 30, 0, 1);
   mouse(NSEventTypeLeftMouseDragged, 976 + 76 + 30, 30, 0, 1);
   mouse(NSEventTypeLeftMouseDragged, 976 + 152 + 30, 30, 0, 1);
   mouse(NSEventTypeLeftMouseDragged, 976 + 152 + 30, 120, 0, 1);
   mouse(NSEventTypeLeftMouseUp, 976 + 152 + 30, 120, 0, 1);
   pump();
   drag(kx(4, 5), ky(5), -25);
   CHECK(val(P_LFO + 16 + 5) > 64 && val(P_LFO + 5) == 64 && val(P_LFO + 8 + 5) == 64, "the slide ends on LFO3: %g", val(P_LFO + 16 + 5));
   mouse(NSEventTypeLeftMouseDown, kx(4, 5), ky(5), 0, 2);   /* double-click: back to the default */
   mouse(NSEventTypeLeftMouseUp, kx(4, 5), ky(5), 0, 2);
   CHECK(val(P_LFO + 16 + 5) == 64, "LFO3 SPD reset");
   click(976 + 30, 30);
   /* turning PAGE redraws DEST, whose name comes from it */
   SEL sel = @selector(setNeedsDisplayInRect:);
   invalidate = (void (*)(id, SEL, NSRect))[NSView instanceMethodForSelector:sel];
   CHECK(class_addMethod([V class], sel, (IMP)recordInvalidate, method_getTypeEncoding(class_getInstanceMethod([NSView class], sel))), "hook");
   pump();
   ndirty = 0;
   drag(kx(4, 0), ky(0), -40);
   CHECK(val(P_LFO) > 0 && redrawn(kx(4, 1), ky(1)), "PAGE %g redraws DEST", val(P_LFO));
   mouse(NSEventTypeLeftMouseDown, kx(4, 0), ky(0), 0, 2);
   mouse(NSEventTypeLeftMouseUp, kx(4, 0), ky(0), 0, 2);
   CHECK(val(P_LFO) == 0, "PAGE reset");

   /* a click toggles a trig, one undo step */
   undoSteps = 0;
   click(padx(0), PADY);
   CHECK(trigs() == 1, "click sets: %llx", (unsigned long long)trigs());
   CHECK(undoSteps == 1 && !strcmp(undoName, "Trig") && !undoOpen, "the trig's undo: %d %s", undoSteps, undoName);
   click(padx(0), PADY);
   CHECK(trigs() == 0, "click clears");
   /* a long press only shows the step */
   mouse(NSEventTypeLeftMouseDown, padx(2), PADY, 0, 1);
   [NSThread sleepForTimeInterval:0.3];
   mouse(NSEventTypeLeftMouseUp, padx(2), PADY, 0, 1);
   CHECK(trigs() == 0, "long press leaves the trig");

   /* right-click latches: a drag writes a lock, the kit value stays */
   click(padx(4), PADY);
   right(padx(4), PADY);
   const int filtW = P_FILT + 1;
   const double before = val(filtW);
   undoSteps = 0;
   drag(kx(2, 1), ky(1), 60);
   CHECK(val(filtW) == before, "a lock edit moved the kit value");
   CHECK(undoSteps == 1 && !strcmp(undoName, "Lock FILT WDTH") && !undoOpen, "a lock drag is one undo step: %d %s", undoSteps, undoName);
   CHECK(lockv(4, filtW) >= 0 && lockv(4, filtW) < before, "lock: %d", lockv(4, filtW));
   /* click on an unlocked knob locks it at its value, click again unlocks */
   click(kx(1, 5), ky(5));
   CHECK(lockv(4, P_AMP + 5) == (int)val(P_AMP + 5), "click locks");
   click(kx(1, 5), ky(5));
   CHECK(lockv(4, P_AMP + 5) == -1, "click unlocks");
   click(kx(1, 5), ky(5));
   /* an LFO knob on another tab */
   click(976 + 76 + 30, 30);
   drag(kx(4, 5), ky(5), -25);
   CHECK(lockv(4, P_LFO + 8 + 5) > 64, "LFO2 SPD lock: %d", lockv(4, P_LFO + 8 + 5));
   shot(dir, "2_held_step");
   /* several trigs held at once take the same edit */
   right(padx(9), PADY);
   drag(kx(3, 2), ky(2), -40);
   CHECK(lockv(4, P_EFX + 2) >= 0 && lockv(4, P_EFX + 2) == lockv(9, P_EFX + 2), "two held steps");
   CHECK(trigs() == 1 << 4, "a right-click hold adds no trig: %llx", (unsigned long long)trigs());
   right(padx(9), PADY);
   right(padx(4), PADY);
   /* released: a drag is a kit edit again, and one gesture */
   flush(); begins = ends = values = 0;
   const double v0 = val(P_FILT + 2);
   drag(kx(2, 2), ky(2), -30);
   flush();
   CHECK(val(P_FILT + 2) > v0 && lockv(4, P_FILT + 2) == -1, "released hold");
   CHECK(begins == 1 && ends == 1 && values >= 1, "one gesture: %d %d %d", begins, ends, values);
   shot(dir, "3_steps");

   /* ctrl-click latches too; CLR LOCKS clears only the held step */
   mouse(NSEventTypeLeftMouseDown, padx(4), PADY, NSEventModifierFlagControl, 1);
   mouse(NSEventTypeLeftMouseUp, padx(4), PADY, NSEventModifierFlagControl, 1);
   click(1136 + 30, 359);
   CHECK(lockv(4, filtW) == -1 && lockv(9, P_EFX + 2) >= 0, "CLR LOCKS");
   CHECK(trigs() == 1 << 4, "CLR LOCKS keeps the trig");
   /* closing the window releases the latch */
   gui->hide(P);
   gui->show(P);
   const double v1 = val(P_FILT + 3);
   drag(kx(2, 3), ky(3), -30);
   CHECK(val(P_FILT + 3) > v1 && lockv(4, P_FILT + 3) == -1, "a hold survived hide");

   /* the keyboard is ignored until KEYBOARD is on */
   CHECK(W.firstResponder != V, "first responder before KEYS");
   tap(18);
   CHECK(trigs() == 1 << 4, "key with capture off");
   const NSRect keysRow = NSMakeRect(1224, 12 + 53 * 6, 128, 44);
   click(NSMidX(keysRow), NSMidY(keysRow));
   CHECK(W.firstResponder == V, "first responder after KEYS");
   static const unsigned short codes[16] = {18, 19, 20, 21, 23, 22, 26, 28, 12, 13, 14, 15, 17, 16, 32, 34};
   for (int i = 0; i < 16; ++i) { uint64_t before16 = trigs(); tap(codes[i]); CHECK((trigs() ^ before16) == 1u << i, "key %d", i); tap(codes[i]); }
   /* hold a key, edit, release: a lock, and the trig unchanged */
   key(NSEventTypeKeyDown, 14);    /* E: the eleventh trig, index 10 */
   drag(kx(1, 0), ky(0), -20);
   key(NSEventTypeKeyUp, 14);
   pump();
   CHECK(lockv(10, P_AMP) >= 0 && !(trigs() >> 10 & 1), "key hold lock");
   shot(dir, "4_keys_on");
   /* a key held while the window loses focus is released */
   key(NSEventTypeKeyDown, 15);
   [NSNotificationCenter.defaultCenter postNotificationName:NSWindowDidResignKeyNotification object:W];
   const double v2 = val(P_AMP + 1);
   drag(kx(1, 1), ky(1), -20);
   CHECK(val(P_AMP + 1) > v2 && lockv(11, P_AMP + 1) == -1, "a key stayed held");
   click(NSMidX(keysRow), NSMidY(keysRow));
   CHECK(W.firstResponder != V, "KEYS off gives the keyboard back");

   /* a drag cut short by closing ends its gesture */
   flush(); begins = ends = 0;
   mouse(NSEventTypeLeftMouseDown, kx(2, 0), ky(0), 0, 1);
   mouse(NSEventTypeLeftMouseDragged, kx(2, 0), ky(0) - 10, 0, 1);
   flush();
   CHECK(begins == 1 && ends == 0, "gesture open");
   gui->hide(P);
   flush();
   CHECK(ends == 1, "gesture ended on hide");
   gui->show(P);

   /* the window closing releases a latched hold */
   right(padx(7), PADY);
   [NSNotificationCenter.defaultCenter postNotificationName:NSWindowWillCloseNotification object:W];
   const double v3 = val(P_FILT + 4);
   drag(kx(2, 4), ky(4), -30);
   CHECK(val(P_FILT + 4) > v3 && lockv(7, P_FILT + 4) == -1, "a hold survived the window closing");

   /* POLY and the MIDI TRIG switches are ordinary parameters */
   CHECK(val(P_POLY) == 0 && val(P_TRIG_SYN) == 1 && val(P_TRIG_FILT) == 1 && val(P_TRIG_LFO) == 1 && val(P_LOCK_END) == 1, "defaults");
   click(1224 + 64, 12 + 53 * 5 + 28);
   click(936 + 30, 307);
   click(1136 + 38, 307);
   CHECK(val(P_POLY) == 1 && val(P_TRIG_SYN) == 0 && val(P_LOCK_END) == 0, "switches");
   click(1136 + 38, 307);
   CHECK(val(P_LOCK_END) == 1, "NOTE ENDS back on");
   shot(dir, "5_poly");

   /* VEL and KEY SETUP: KIT ASSIGN's KEY VEL and KEY TRK tabs in small windows over the LFO page */
   {
      const CGFloat bx = 1224 + 64, velY = 12 + 53 + 29, keyY = 12 + 53 * 2 + 29;   /* the SETUP buttons */
      const NSRect vw = NSMakeRect(1214 - 178, 34, 178, 232), kw = NSMakeRect(1214 - 254, 34, 254, 232);
      CGFloat (^ax)(NSRect, int) = ^CGFloat(NSRect w, int k) { return w.origin.x + 8 + 54 * (k % 3) + 27; };
      CGFloat (^ay)(NSRect, int) = ^CGFloat(NSRect w, int k) { return w.origin.y + 30 + 100 * (k / 3) + 50; };
      CHECK(val(P_ASSIGN) == 2 && val(P_ASSIGN + 1) == 5 && val(P_ASSIGN + 2) == 64 && val(P_ASSIGN + 3) == 0 && val(P_ASSIGN + 6) == 1
            && val(P_ASSIGN + 9) == 0 && val(P_LPKEY) == 1 && val(P_HPKEY) == 1, "a cleared kit's VEL and KEY rows");
      double before[NPARAM];
      for (int i = 0; i < NPARAM; ++i) before[i] = val(i);
      /* closed: the LFO knob under where the window opens is the LFO's */
      const int lfoTrig = P_LFO + 2;
      click(976 + 15, 30);                       /* LFO1 */
      drag(kx(4, 2), ky(2), -40);
      CHECK(val(lfoTrig) != before[lfoTrig], "the LFO knob under the window");
      setp(lfoTrig, before[lfoTrig]);
      /* open: the same drag turns VEL 1 DEST, and nothing under the window moves */
      click(bx, velY);
      drag(ax(vw, 1), ay(vw, 1), -60);          /* 7 steps over 200 points */
      CHECK(val(P_ASSIGN + 1) == 7, "VEL 1 DEST dragged up: %g", val(P_ASSIGN + 1));
      CHECK(val(lfoTrig) == before[lfoTrig], "a knob under the open window moved");
      drag(ax(vw, 2), ay(vw, 2), -60);
      CHECK(val(P_ASSIGN + 2) > 64, "VEL 1 ADD up: %g", val(P_ASSIGN + 2));
      char t[CLAP_NAME_SIZE];
      PA->value_to_text(P, P_ASSIGN + 2, 100, t, sizeof t);
      CHECK(!strcmp(t, "+36"), "ADD text %s", t);
      PA->value_to_text(P, P_ASSIGN + 1, 5, t, sizeof t);
      CHECK(!strcmp(t, "VOL"), "DEST text %s", t);
      dbl(ax(vw, 2), ay(vw, 2));
      CHECK(val(P_ASSIGN + 2) == 64, "double-click: ADD back to 0");
      drag(ax(vw, 3), ay(vw, 3), -60);
      CHECK(val(P_ASSIGN + 3) > 0 && val(P_ASSIGN + 3) <= 8, "VEL 2 PAGE: %g", val(P_ASSIGN + 3));
      shot(dir, "12_vel_setup");
      /* a click inside the window but on no control keeps it open; one outside closes it and does nothing else */
      click(vw.origin.x + 10, vw.origin.y + 150);
      drag(ax(vw, 1), ay(vw, 1), 40);
      CHECK(val(P_ASSIGN + 1) < 7, "the window closed on a click inside it");
      const uint64_t tr = trigs();
      click(padx(2), PADY);
      CHECK(trigs() == tr, "the click that closed the window reached a pad");
      drag(kx(4, 2), ky(2), -40);
      CHECK(val(lfoTrig) != before[lfoTrig], "the window stayed open after a click outside");
      setp(lfoTrig, before[lfoTrig]);
      /* KEY: its rows and the HPF and LPF switches; VEL's button switches windows; x closes */
      click(bx, keyY);
      click(kw.origin.x + 180 + 32, kw.origin.y + 58 + 29);
      CHECK(val(P_HPKEY) == 0 && val(P_LPKEY) == 1, "HPF off");
      click(kw.origin.x + 180 + 32, kw.origin.y + 158 + 29);
      CHECK(val(P_LPKEY) == 0, "LPF off");
      drag(ax(kw, 5), ay(kw, 5), -30);
      CHECK(val(P_ASSIGN + 11) > 64, "KEY 2 ADD");
      shot(dir, "13_key_setup");
      click(bx, velY);
      drag(ax(vw, 2), ay(vw, 2), -30);
      CHECK(val(P_ASSIGN + 2) > 64 && val(P_ASSIGN + 11) > 64, "VEL's button opened VEL");
      click(bx, velY);                           /* its own button closes it */
      drag(kx(4, 2), ky(2), -40);
      CHECK(val(lfoTrig) != before[lfoTrig], "VEL's button did not close VEL");
      click(bx, keyY);
      click(NSMaxX(kw) - 17, kw.origin.y + 15);  /* x */
      drag(kx(4, 2), ky(2), 40);
      CHECK(val(lfoTrig) == before[lfoTrig], "x did not close KEY");
      for (int i = P_ASSIGN; i < NPARAM; ++i) setp(i, before[i]);
      setp(P_HPKEY, 1); setp(P_LPKEY, 1);
      pump();
   }

   /* a state with locks comes back, and the editor follows */
   Buf saved = save();
   clap_istream_t is = {&saved, r};
   CHECK(((const clap_plugin_state_t *)P->get_extension(P, CLAP_EXT_STATE))->load(P, &is), "load");
   CHECK(lockv(10, P_AMP) >= 0, "loaded lock");

   /* turning a stepped LFO knob with no modulation shows no violet, at any point of the drag */
   click(976 + 15, 30);
   mouse(NSEventTypeLeftMouseDown, kx(4, 3), ky(3), 0, 1);
   for (int i = 1; i <= 40; ++i) {
      mouse(NSEventTypeLeftMouseDragged, kx(4, 3), ky(3) - i * 1.7, 0, 1);
      CHECK(violet() == 0, "violet on an unmodulated stepped knob at drag %d: %d pixels", i, violet());
   }
   mouse(NSEventTypeLeftMouseUp, kx(4, 3), ky(3) - 68, 0, 1);
   CHECK(violet() == 0, "violet after the drag");

   /* a MIDI note on the running pattern draws a line under its step, and it fades */
   {
      CHECK(P->activate(P, 44100, 1, 512) && P->start_processing(P), "activate");
      process(NULL, 5.0 / 4, true);
      pump(); pump();
      CHECK(noteLine(5) == 0, "no line before a note");
      clap_event_note_t n = {{sizeof n, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_ON, 0}, -1, 0, 0, 60, 1.0};
      process(&n.header, 5.0 / 4 + 256 * 120.0 / 60 / 44100, true);
      pump(); pump();
      const double bright = noteLine(5);
      CHECK(bright > 0.5, "a note's line: %g", bright);
      CHECK(noteLine(4) == 0 && noteLine(6) == 0, "the line is only under its own step");
      shot(dir, "8_note_line");
      [NSThread sleepForTimeInterval:0.3];   /* a run loop with no sources returns at once */
      pump();
      const double half = noteLine(5);
      CHECK(half > 0 && half < bright, "fading: %g then %g", bright, half);
      [NSThread sleepForTimeInterval:0.6];   /* a run loop with no sources returns at once */
      pump();
      CHECK(noteLine(5) == 0, "faded out");
      n.header.type = CLAP_EVENT_NOTE_OFF;
      process(&n.header, 0, false);
      P->stop_processing(P);
      P->deactivate(P);
   }

   /* modulation and a sounding lock: rings move, the kit value and the needle do not */
   {
      CHECK(P->activate(P, 44100, 1, 512) && P->start_processing(P), "activate");
      const double base = val(P_FILT);
      const uint64_t clean = viewHash();
      clap_event_param_mod_t m = {{sizeof m, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_MOD, 0}, P_FILT, NULL, -1, -1, -1, -1, 30.0};
      process(&m.header, 0, false);
      pump(); pump();
      CHECK(val(P_FILT) == base, "modulation moved the base");
      CHECK(viewHash() != clean, "no modulation ring");
      shot(dir, "6_modulated");
      process(NULL, 9.0 / 4, true);          /* the playhead on the tenth step, which locks EFX SRR */
      pump(); pump();
      shot(dir, "7_playing");
      process(NULL, 0, false);               /* stop: the lock returns to the kit */
      m.amount = 0;
      process(&m.header, 0, false);
      pump(); pump();
      CHECK(viewHash() == clean, "the rings must clear");
      P->stop_processing(P);
      P->deactivate(P);
   }

   /* the LFO graphs: a running LFO's dot redraws its tab; a still one, or one too fast to follow, does not */
   {
      CHECK(P->activate(P, 44100, 1, 512) && P->start_processing(P), "activate");
      static const struct { int id; double v, back; } set[] = {
         {P_LFO + 5, 127, 64}, {P_LFO + 4, 1, 0}, {P_LFO + 8 + 5, 0, 64}, {P_LFO + 16 + 5, 127, 64}, {P_LFO + 16 + 4, 6, 0},
         {P_LFO + 16 + 7, 100, 0}};
      for (unsigned i = 0; i < sizeof set / sizeof set[0]; ++i) {
         clap_event_param_value_t e = {{sizeof e, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0}, (clap_id)set[i].id, NULL, -1, -1, -1, -1, set[i].v};
         process(&e.header, 0, false);
      }
      for (int i = 0; i < 20; ++i) { process(NULL, 0, false); [NSThread sleepForTimeInterval:0.03]; pump(); }   /* the fades settle */
      ndirty = 0;
      for (int i = 0; i < 20; ++i) { process(NULL, 0, false); pump(); }
      CHECK(redrawn(976 + 38, 30) && !redrawn(976 + 76 + 38, 30) && !redrawn(976 + 152 + 38, 30), "LFO tab redraws: %d %d %d",
            redrawn(976 + 38, 30), redrawn(976 + 76 + 38, 30), redrawn(976 + 152 + 38, 30));
      CHECK(blue(NSMakeRect(1018, 18, 30, 24)) > 0, "LFO1's dot trails a blue plume");
      int peak[3];
      pixel(1172 + 6.5, 30 - 6.5, peak);   /* LFO3, too fast to follow: its whole wave pink (TRI's peak) */
      CHECK(peak[0] > 200 && peak[1] < 150 && peak[2] > 110, "LFO3's wave: %d %d %d", peak[0], peak[1], peak[2]);
      for (unsigned i = 0; i < sizeof set / sizeof set[0]; ++i) {
         clap_event_param_value_t e = {{sizeof e, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0}, (clap_id)set[i].id, NULL, -1, -1, -1, -1, set[i].back};
         process(&e.header, 0, false);
      }
      P->stop_processing(P);
      P->deactivate(P);
   }

   /* a scroll belongs to the trigs held when it began */
   click(NSMidX(keysRow), NSMidY(keysRow));
   CHECK(W.firstResponder == V, "KEYS on");
   {
      const CGFloat x = kx(3, 0), y = ky(0);
      const int id = P_EFX;
      const double kit = val(id);
      key(NSEventTypeKeyDown, 17);               /* T: index 12 */
      scroll(x, y, 0, 128, 0, YES);
      CHECK(lockv(12, id) == -1, "a finger landing writes nothing");
      scroll(x, y, 6, 1, 0, YES);
      for (int i = 0; i < 4; ++i) scroll(x, y, 6, 2, 0, YES);
      const int locked = lockv(12, id);
      CHECK(locked > (int)kit && val(id) == kit, "a scroll while held edits the lock: %d", locked);
      scroll(x, y, 0, 4, 0, YES);
      CHECK(lockv(12, id) == locked, "a gesture's end moved the lock");
      key(NSEventTypeKeyUp, 17);
      pump();
      for (int i = 0; i < 12; ++i) scroll(x, y, 6, 0, i ? 2 : 1, YES);
      scroll(x, y, 0, 0, 3, YES);
      CHECK(val(id) == kit, "momentum after the key moved the kit value: %g -> %g", kit, val(id));
      CHECK(lockv(12, id) == locked, "momentum after the key moved the lock");
      CHECK(!(trigs() >> 12 & 1), "a key used for an edit must not toggle its trig");
      /* the fingers still down when the key is released */
      key(NSEventTypeKeyDown, 17);
      scroll(x, y, 4, 1, 0, YES);
      const int l2 = lockv(12, id);
      key(NSEventTypeKeyUp, 17);
      for (int i = 0; i < 6; ++i) scroll(x, y, 6, 2, 0, YES);
      CHECK(val(id) == kit && lockv(12, id) == l2, "a scroll kept going after its key");
      scroll(x, y, 0, 4, 0, YES);
      /* the next gesture is a normal kit edit */
      flush(); begins = ends = 0;
      scroll(x, y, 6, 1, 0, YES);
      scroll(x, y, 6, 2, 0, YES);
      CHECK(val(id) > kit && lockv(12, id) == l2, "a new scroll must move the kit value");
      const double moved = val(id);
      scroll(x, y, 0, 4, 0, YES);
      /* a kit scroll's momentum must not become a lock when a key goes down */
      key(NSEventTypeKeyDown, 16);               /* Y: index 13 */
      for (int i = 0; i < 6; ++i) scroll(x, y, 6, 0, i ? 2 : 1, YES);
      key(NSEventTypeKeyUp, 16);
      CHECK(val(id) == moved && lockv(13, id) == -1, "momentum spilled into a lock");
      scroll(x, y, 0, 0, 3, YES);
      [NSThread sleepForTimeInterval:0.3];   /* a run loop with no sources returns at once */
      pump();
      pump(); flush();
      CHECK(begins == 1 && ends == 1, "one scroll, one gesture: %d %d", begins, ends);

      /* a wheel has no phases: a pause ends its scroll */
      const CGFloat x2 = kx(3, 1), y2 = ky(1);   /* 10 pixels is one line, a wheel's notch: one step on 0-127 */
      const double kit2 = val(P_EFX + 1);
      key(NSEventTypeKeyDown, 32);               /* U: index 14 */
      scroll(x2, y2, 10, 0, 0, NO);
      const int l3 = lockv(14, P_EFX + 1);
      CHECK(l3 > (int)kit2, "a wheel click on a held trig: %d", l3);
      key(NSEventTypeKeyUp, 32);
      scroll(x2, y2, 10, 0, 0, NO);
      CHECK(val(P_EFX + 1) == kit2 && lockv(14, P_EFX + 1) == l3, "a wheel kept going after its key");
      [NSThread sleepForTimeInterval:0.3];   /* a run loop with no sources returns at once */
      pump();
      scroll(x2, y2, 10, 0, 0, NO);
      CHECK(val(P_EFX + 1) > kit2, "a wheel after a pause edits the kit value");
      [NSThread sleepForTimeInterval:0.3];   /* a run loop with no sources returns at once */
      pump();
      pump();

      /* a drag stops too */
      const CGFloat x3 = kx(3, 2), y3 = ky(2);
      const double kit3 = val(P_EFX + 2);
      key(NSEventTypeKeyDown, 34);               /* I: index 15 */
      mouse(NSEventTypeLeftMouseDown, x3, y3, 0, 1);
      mouse(NSEventTypeLeftMouseDragged, x3, y3 - 20, 0, 1);
      const int l4 = lockv(15, P_EFX + 2);
      CHECK(l4 >= 0, "a drag on a held trig");
      key(NSEventTypeKeyUp, 34);
      mouse(NSEventTypeLeftMouseDragged, x3, y3 - 50, 0, 1);
      mouse(NSEventTypeLeftMouseUp, x3, y3 - 50, 0, 1);
      pump();
      CHECK(val(P_EFX + 2) == kit3 && lockv(15, P_EFX + 2) == l4, "a drag kept going after its key");
      /* a key pressed during a lock drag does not receive the rest of it */
      key(NSEventTypeKeyDown, 34);
      mouse(NSEventTypeLeftMouseDown, x3, y3, 0, 1);
      mouse(NSEventTypeLeftMouseDragged, x3, y3 - 10, 0, 1);
      const int l5 = lockv(15, P_EFX + 2);
      key(NSEventTypeKeyDown, 32);
      mouse(NSEventTypeLeftMouseDragged, x3, y3 - 40, 0, 1);
      mouse(NSEventTypeLeftMouseUp, x3, y3 - 40, 0, 1);
      key(NSEventTypeKeyUp, 32);
      key(NSEventTypeKeyUp, 34);
      pump();
      CHECK(lockv(14, P_EFX + 2) == -1 && lockv(15, P_EFX + 2) == l5, "a lock drag spread to a key pressed during it");
      /* a kit drag stops when a key goes down */
      const double k4 = val(P_EFX + 2);
      mouse(NSEventTypeLeftMouseDown, x3, y3, 0, 1);
      mouse(NSEventTypeLeftMouseDragged, x3, y3 - 10, 0, 1);
      const double k5 = val(P_EFX + 2);
      key(NSEventTypeKeyDown, 32);
      mouse(NSEventTypeLeftMouseDragged, x3, y3 - 40, 0, 1);
      mouse(NSEventTypeLeftMouseUp, x3, y3 - 40, 0, 1);
      key(NSEventTypeKeyUp, 32);
      pump();
      CHECK(k5 > k4 && val(P_EFX + 2) == k5 && lockv(14, P_EFX + 2) == -1, "a kit drag kept going after a key went down");
   }
   click(NSMidX(keysRow), NSMidY(keysRow));

   /* CPY and PST */
   {
      const double kitW = val(P_FILT);
      /* not selected: no buttons */
      const uint64_t t0 = trigs();
      clickButton(1, YES);
      clickButton(1, NO);
      CHECK(trigs() == t0, "buttons on an unselected trig");
      CHECK(brightIn(padButton(1, NO)) == 0, "no buttons drawn on an unselected trig");
      /* an empty step: both gray, and PST does nothing */
      right(padx(0), PADY);
      const int grayCopy = brightIn(padButton(0, NO)), grayPaste = brightIn(padButton(0, YES));
      clickButton(0, NO);
      clickButton(0, YES);
      CHECK(brightIn(padButton(0, YES)) == grayPaste, "copying an empty step must not enable PST");
      right(padx(0), PADY);
      /* a step with a trig and two locks */
      right(padx(1), PADY);
      click(padx(1), PADY);
      drag(kx(2, 0), ky(0), -30);
      drag(kx(1, 7), ky(7), -20);
      CHECK((trigs() >> 1 & 1) && lockv(1, P_FILT) >= 0 && lockv(1, P_AMP + 7) >= 0, "the source step");
      CHECK(brightIn(padButton(1, NO)) > grayCopy + 20, "CPY lit on a step with content");
      CHECK(brightIn(padButton(1, YES)) == grayPaste, "PST gray with nothing copied");
      shot(dir, "9_nothing_to_paste");
      clickButton(1, NO);
      CHECK(brightIn(padButton(1, YES)) > grayPaste + 20, "PST lit after a copy");
      right(padx(1), PADY);
      /* paste replaces the target's trig and locks */
      right(padx(3), PADY);
      drag(kx(3, 3), ky(3), -25);
      CHECK(lockv(3, P_EFX + 3) >= 0, "the target's old lock");
      clickButton(3, YES);
      CHECK(sameStep(1, 3), "paste must copy the trig and every lock");
      CHECK(lockv(3, P_EFX + 3) == -1, "paste must replace old locks");
      CHECK(val(P_FILT) == kitW, "paste moved a kit value");
      shot(dir, "10_pasted");
      right(padx(3), PADY);
      /* an empty step's CPY leaves the clipboard alone */
      right(padx(0), PADY);
      clickButton(0, NO);
      right(padx(0), PADY);
      right(padx(5), PADY);
      clickButton(5, YES);
      CHECK(sameStep(1, 5), "an empty step's CPY replaced the clipboard");
      right(padx(5), PADY);
      /* a trigless source clears the target's trig */
      right(padx(9), PADY);
      CHECK(!(trigs() >> 9 & 1) && lockv(9, P_EFX + 2) >= 0, "the trigless step");
      clickButton(9, NO);
      right(padx(9), PADY);
      right(padx(5), PADY);
      clickButton(5, YES);
      CHECK(sameStep(9, 5) && !(trigs() >> 5 & 1), "a trigless paste");
      right(padx(5), PADY);
      /* held with a key: the button works, and releasing the key is not a tap */
      click(NSMidX(keysRow), NSMidY(keysRow));
      const uint64_t t1 = trigs();
      key(NSEventTypeKeyDown, 23);               /* 5: index 4 */
      [NSThread sleepForTimeInterval:0.3];
      pump();
      clickButton(4, NO);
      key(NSEventTypeKeyUp, 23);
      pump();
      CHECK(trigs() == t1, "releasing a key after CPY toggled its trig");
      key(NSEventTypeKeyDown, 21);               /* 4: index 3 */
      [NSThread sleepForTimeInterval:0.3];
      pump();
      clickButton(3, YES);
      key(NSEventTypeKeyUp, 21);
      pump();
      CHECK(sameStep(4, 3), "paste under a held key");
      /* a key pressed on a latched trig, CPY clicked at once: releasing the key is still no tap */
      right(padx(4), PADY);
      const uint64_t t2 = trigs();
      key(NSEventTypeKeyDown, 23);
      clickButton(4, NO);
      key(NSEventTypeKeyUp, 23);
      pump();
      CHECK(trigs() == t2, "a quick key release after CPY toggled the trig");
      right(padx(4), PADY);
      /* a quick tap never shows the buttons */
      key(NSEventTypeKeyDown, 19);               /* 2: index 1 */
      pump();
      CHECK(brightIn(padButton(1, NO)) == 0, "buttons during a tap");
      key(NSEventTypeKeyUp, 19);
      key(NSEventTypeKeyDown, 19); key(NSEventTypeKeyUp, 19);
      pump();
      click(NSMidX(keysRow), NSMidY(keysRow));
      /* the clipboard survives closing the window */
      gui->hide(P);
      gui->show(P);
      right(padx(6), PADY);
      clickButton(6, YES);
      CHECK(sameStep(4, 6), "the clipboard after reopening");
      right(padx(6), PADY);
   }

   /* SCALE: LEN, the speed, the pages */
   {
#define SCALEY 359
      const CGFloat lenx = 960, spdx = 1007;
      CGFloat (^pagex)(int) = ^CGFloat(int k) { return 1041 + 24 * k; };
      CHECK(length() == 16 && speed() == 0, "16 steps at 1X: %d %d", length(), speed());
      for (int i = 1; i <= 4; ++i) { click(spdx, SCALEY); CHECK(speed() == (i & 3), "speed %d", speed()); }
      /* page 2 is past the end: clicking it makes 32 steps, page 1 copied into it, and the pads show it */
      click(pagex(1), SCALEY);
      CHECK(length() == 32, "page 2 extends to 32: %d", length());
      for (int t = 0; t < 16; ++t) CHECK(sameStep(t, 16 + t), "page 2 step %d is page 1's", t);
      const uint64_t t3 = trigs();
      click(padx(5), PADY);
      CHECK(trigs() == (t3 ^ 1ull << 21), "pad 6 on page 2 is step 22");
      click(pagex(0), SCALEY);
      click(padx(5), PADY);
      CHECK(trigs() == (t3 ^ 1ull << 21 ^ 1ull << 5), "pad 6 on page 1 is step 6");
      click(padx(5), PADY);
      /* page 3: steps 33-48 come from page 1; page 2, no longer empty, is kept */
      click(pagex(2), SCALEY);
      CHECK(length() == 48, "48 steps");
      for (int t = 0; t < 16; ++t) CHECK(sameStep(t, 32 + t), "page 3 step %d is page 1's", t);
      CHECK(trigs() >> 21 & 1 ^ (t3 >> 21 & 1), "page 2 kept its own trig");
      /* page 4 comes from page 2 */
      click(pagex(3), SCALEY);
      CHECK(length() == 64, "64 steps");
      for (int t = 0; t < 16; ++t) CHECK(sameStep(16 + t, 48 + t), "page 4 step %d is page 2's", t);
      /* LEN: a drag down shortens; past page 4's start the pads go back to page 1 */
      drag(lenx, SCALEY, 40);
      const int l1 = length();
      CHECK(l1 < 64 && l1 >= 48, "LEN drag: %d", l1);
      drag(lenx, SCALEY, 100);
      CHECK(length() < 48, "LEN drag further: %d", length());
      pump();
      const uint64_t t4 = trigs();
      click(padx(2), PADY);
      CHECK(trigs() == (t4 ^ 1ull << 2), "back on page 1 after the pattern got shorter");
      click(padx(2), PADY);
      /* growing again over pages that hold steps copies nothing */
      const Buf before = save();
      drag(lenx, SCALEY, -200);
      CHECK(length() == 64, "LEN drag up: %d", length());
      CHECK(!memcmp(before.d + LOCKS, save().d + LOCKS, UI_STEPS * UI_LOCKS) && trigs() == t4, "full pages were overwritten");
      mouse(NSEventTypeLeftMouseDown, lenx, SCALEY, 0, 2);
      mouse(NSEventTypeLeftMouseUp, lenx, SCALEY, 0, 2);
      pump();
      CHECK(length() == 16, "double-click LEN: %d", length());
      /* a latched trig is let go when the page changes: the next knob turn is the kit's */
      click(pagex(2), SCALEY);
      click(pagex(0), SCALEY);
      right(padx(3), PADY);
      click(pagex(1), SCALEY);
      const double kit = val(P_FILT + 2);
      drag(kx(2, 2), ky(2), -30);
      CHECK(val(P_FILT + 2) > kit && lockv(3, P_FILT + 2) == -1 && lockv(19, P_FILT + 2) == -1, "a latch survived the page change");
      /* 40 steps; the playing page's button carries the playhead, here step 39 on page 3 */
      click(pagex(2), SCALEY);
      drag(lenx, SCALEY, 26);
      CHECK(length() == 40, "LEN 40: %d", length());
      click(pagex(0), SCALEY);
      const NSRect page3 = NSMakeRect(pagex(2) - 11, 346, 22, 26);
      const int still = brightIn(page3);
      CHECK(P->activate(P, 44100, 1, 512) && P->start_processing(P), "activate");
      process(NULL, 38 / 4.0, true);
      pump(); pump();
      CHECK(brightIn(page3) > still, "no playhead on page 3: %d %d", brightIn(page3), still);
      click(pagex(2), SCALEY);
      shot(dir, "11_pages");
      process(NULL, 0, false);
      P->stop_processing(P);
      P->deactivate(P);
      click(pagex(0), SCALEY);
      /* a press slides across the pages the pattern has (40 steps: three), never onto one past the end */
      void (^slide)(void) = ^{
         mouse(NSEventTypeLeftMouseDown, pagex(0), SCALEY, 0, 1);
         for (int k = 1; k < 4; ++k) mouse(NSEventTypeLeftMouseDragged, pagex(k), SCALEY, 0, 1);
         mouse(NSEventTypeLeftMouseUp, pagex(3), SCALEY, 0, 1);
         pump();
      };
      const uint64_t t5 = trigs();
      slide();
      click(padx(1), PADY);
      CHECK(length() == 40 && trigs() == (t5 ^ 1ull << 33), "slide to page 3: %d", length());
      click(padx(1), PADY);
      /* a double-click ends the pattern with that page; a slide then stops at the new last page */
      dbl(pagex(1), SCALEY);
      CHECK(length() == 32, "double-click page 2: %d", length());
      slide();
      click(padx(1), PADY);
      CHECK(length() == 32 && trigs() == (t5 ^ 1ull << 17), "slide past the end: %d", length());
      click(padx(1), PADY);
      dbl(pagex(0), SCALEY);
      CHECK(length() == 16 && trigs() == t5, "double-click page 1: %d", length());
   }

   /* a value lights up under the mouse, and a double-click on it types a new one */
   {
      NSTextField *(^field)(void) = ^NSTextField *(void) {
         for (NSView *v in V.subviews) if ([v isKindOfClass:NSTextField.class]) return (NSTextField *)v;
         return nil;
      };
      void (^move)(CGFloat, CGFloat) = ^(CGFloat x, CGFloat y) {
         [V mouseMoved:[NSEvent mouseEventWithType:NSEventTypeMouseMoved location:at(x, y) modifierFlags:0 timestamp:0 windowNumber:W.windowNumber context:nil eventNumber:0 clickCount:0 pressure:0]];
         pump();
      };
      /* types into the open field as a user would: the field editor's own text input and commands */
      void (^type)(const char *, SEL) = ^(const char *text, SEL how) {
         NSTextField *f = field();
         CHECK(f && [f.currentEditor isKindOfClass:NSTextView.class], "no field to type '%s' into", text);
         NSTextView *ed = (NSTextView *)f.currentEditor;
         [ed selectAll:nil];
         [ed insertText:[NSString stringWithUTF8String:text] replacementRange:ed.selectedRange];
         if (how) [ed doCommandBySelector:how];
         pump();
      };
      const SEL enter = @selector(insertNewline:), esc = @selector(cancelOperation:);
      CGFloat (^vy)(int) = ^CGFloat(int k) { return 52 + (k / 4) * 106 + 87; };   /* a knob's value line */
      const int pan = P_AMP + 6;
      char t[CLAP_NAME_SIZE];
      PA->value_to_text(P, pan, 64, t, sizeof t);
      CHECK(!strcmp(t, "0.0"), "PAN at the centre reads %s", t);
      move(4, 300);
      const uint64_t plain = viewHash();
      move(kx(1, 6), ky(6));
      CHECK(viewHash() == plain, "the knob itself lit up");
      move(kx(1, 6) + 20, vy(6));
      const uint64_t lit = viewHash();
      CHECK(lit != plain, "PAN's value did not light up");
      shot(dir, "14_value_hover");
      move(4, 300);
      CHECK(viewHash() == plain, "the light stayed after the mouse left");

      /* Enter takes it; the field shows the value as it reads */
      dbl(kx(1, 6), vy(6));
      CHECK(field() && !strcmp(field().stringValue.UTF8String, "0.0"), "the field holds '%s'", field() ? field().stringValue.UTF8String : "(none)");
      CHECK(W.firstResponder == field().currentEditor, "the field has the keys");
      type("-12", NULL);
      shot(dir, "15_value_typing");
      type("-12", enter);
      CHECK(!field() && val(pan) == 52, "typed -12: %g", val(pan));
      dbl(kx(1, 6), ky(6));
      CHECK(val(pan) == 64, "a double-click on the knob no longer resets it: %g", val(pan));
      /* Esc drops it; text that reads as no value beeps and stays; a click elsewhere takes what reads */
      dbl(kx(1, 6), vy(6));
      type("30", esc);
      CHECK(!field() && val(pan) == 64, "Esc kept %g", val(pan));
      dbl(kx(1, 6), vy(6));
      type("abc", enter);
      CHECK(field() && val(pan) == 64, "unreadable text closed the field or moved PAN to %g", val(pan));
      click(1218, 300);
      CHECK(!field() && val(pan) == 64, "unreadable text left by a click: %g", val(pan));
      dbl(kx(1, 6), vy(6));
      type("+5", NULL);
      click(1218, 300);
      CHECK(!field() && val(pan) == 69, "a click elsewhere took %g", val(pan));
      dbl(kx(1, 6), vy(6));
      type("0", enter);
      CHECK(val(pan) == 64, "typed 0: %g", val(pan));

      /* the other forms: names (LFO lists, DEST by its PAGE), ratios, Hz, BEND's ranges, KIT ASSIGN's ADD */
      click(976 + 15, 30);   /* LFO1 */
      const double lfo[4] = {val(P_LFO), val(P_LFO + 1), val(P_LFO + 3), val(P_SYN)};
      dbl(kx(4, 3), vy(3));
      type("rnd", enter);
      CHECK(val(P_LFO + 3) == 10, "LFO1 WAVE typed rnd: %g", val(P_LFO + 3));
      dbl(kx(4, 0), vy(0));
      type("AMP", enter);
      dbl(kx(4, 1), vy(1));
      type("vol", enter);
      CHECK(val(P_LFO) == 2 && val(P_LFO + 1) == 5, "LFO1 PAGE AMP, DEST VOL: %g %g", val(P_LFO), val(P_LFO + 1));
      dbl(kx(0, 0), vy(0));   /* FM+ STAT's 1FRQ: one of its listed ratios */
      type("1/2", enter);
      PA->value_to_text(P, P_SYN, val(P_SYN), t, sizeof t);
      CHECK(!strcmp(t, "1/2"), "1FRQ typed 1/2: %s", t);
      const double mtune = val(P_MTUNE), bend = val(P_BEND);
      const CGFloat sx = 1224 + 128 - 12;   /* the right column's values, at each slider's top right */
      dbl(sx, 12 + 53 * 4 + 7);
      CHECK(field() && !strcmp(field().stringValue.UTF8String, "440.0 Hz"), "MAST TUN's field: %s", field() ? field().stringValue.UTF8String : "(none)");
      type("441.5", enter);
      CHECK(val(P_MTUNE) == 441.5, "MAST TUN typed 441.5: %g", val(P_MTUNE));
      dbl(sx, 12 + 53 * 3 + 7);
      type("1oct", enter);
      CHECK(val(P_BEND) == 3, "BEND typed 1oct: %g", val(P_BEND));
      const CGFloat bx = 1224 + 64, velY = 12 + 53 + 29, wx = 1214 - 178 + 8 + 54 * 2 + 27, wy = 34 + 30 + 87;
      click(bx, velY);
      dbl(wx, wy);
      type("-20", enter);
      CHECK(val(P_ASSIGN + 2) == 44, "VEL 1 ADD typed -20: %g", val(P_ASSIGN + 2));
      dbl(wx, wy);
      type("0", enter);
      click(bx, velY);
      for (int i = 0; i < 4; ++i) setp(i < 2 ? P_LFO + i : i == 2 ? P_LFO + 3 : P_SYN, lfo[i]);
      setp(P_MTUNE, mtune);
      setp(P_BEND, bend);

      /* holding a step: the typed value is that step's lock; the kit value stays */
      right(padx(3), PADY);
      dbl(kx(1, 6), vy(6));
      type("+10", enter);
      CHECK(lockv(3, pan) == 74 && val(pan) == 64, "typed lock %d, kit %g", lockv(3, pan), val(pan));
      click(kx(1, 6), ky(6));   /* the unit's unlock */
      right(padx(3), PADY);
      CHECK(lockv(3, pan) == -1, "unlocked");
      /* with KEYBOARD CAPTURE on, the keys go to the field and come back after */
      click(1288, 12 + 53 * 6 + 29);
      CHECK(W.firstResponder == V, "capture on");
      dbl(kx(1, 6), vy(6));
      CHECK(W.firstResponder == field().currentEditor, "the field has the keys under capture");
      type("1", enter);   /* the key that is pad 1 */
      CHECK(val(pan) == 65 && W.firstResponder == V && !(trigs() & 1), "typed 1 under capture: %g", val(pan));
      setp(pan, 64);
      click(1288, 12 + 53 * 6 + 29);
      CHECK(W.firstResponder != V, "capture off");
   }

   /* a host resize scales the layout */
   uint32_t aw = 1000, ah = 100;
   CHECK(gui->can_resize(P) && gui->adjust_size(P, &aw, &ah), "adjust");
   CHECK(aw >= 820 && abs((int)ah - (int)(aw * MT_GUI_H / MT_GUI_W)) <= 1, "aspect %u %u", aw, ah);
   CHECK(gui->set_size(P, aw, ah), "set_size");
   CHECK(V.bounds.size.width == MT_GUI_W, "bounds");

   gui->destroy(P);
   CHECK(W.contentView.subviews.count == 0, "removed");
   [W close];
   P->destroy(P);

   /* an FX machine's editor */
   {
      P = f->create_plugin(f, &host, "works.reconstruction.monotrack.phaser");
      CHECK(P && P->init(P), "create PHASER");
      PA = P->get_extension(P, CLAP_EXT_PARAMS);
      const clap_plugin_gui_t *g = P->get_extension(P, CLAP_EXT_GUI);
      CHECK(g->create(P, CLAP_WINDOW_API_COCOA, false), "PHASER gui");
      W = [[NSWindow alloc] initWithContentRect:NSMakeRect(-6000, -6000, MT_GUI_W, MT_GUI_H) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
      W.releasedWhenClosed = NO;
      clap_window_t pw = {CLAP_WINDOW_API_COCOA, {(__bridge void *)W.contentView}};
      CHECK(g->set_parent(P, &pw) && g->show(P), "PHASER parent/show");
      V = W.contentView.subviews.firstObject;
      CHECK(V, "the PHASER editor view");
      shot(dir, "16_fx_phaser");
      NSTextField *(^field)(void) = ^NSTextField *(void) {
         for (NSView *sub in V.subviews) if ([sub isKindOfClass:NSTextField.class]) return (NSTextField *)sub;
         return nil;
      };
      CGFloat (^vy)(int) = ^CGFloat(int k) { return 52 + (k / 4) * 106 + 87; };
      dbl(kx(0, 6), vy(6));
      CHECK(!field(), "the unused knob opened a field");
      char t[CLAP_NAME_SIZE];
      CHECK(PA->value_to_text(P, P_SYN + 7, 64, t, sizeof t) && !strcmp(t, "0.0"), "PHASER INP at 64 reads %s", t);
      dbl(kx(0, 7), vy(7));
      NSTextField *fd = field();
      CHECK(fd && !strcmp(fd.stringValue.UTF8String, "0.0"), "INP's field: %s", fd ? fd.stringValue.UTF8String : "none");
      NSTextView *ed = (NSTextView *)fd.currentEditor;
      [ed selectAll:nil];
      [ed insertText:@"-6" replacementRange:ed.selectedRange];
      [ed doCommandBySelector:@selector(insertNewline:)];
      pump();
      CHECK(val(P_SYN + 7) == 58 && !field(), "typed INP -6: %g", val(P_SYN + 7));
      g->destroy(P);
      [W close];
      P->destroy(P);
   }
   entry->deinit();
   CHECK(!undoOpen && undoSteps > 0, "an undo change left open (%d steps)", undoSteps);
   }
   puts("PASS");
   return 0;
}
