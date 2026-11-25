/* The Mono Track editor: one Cocoa view that draws and handles every control.
 *
 * Three values per knob. The needle is the kit value (base). An amber arc goes
 * to a lock sounding now, a violet arc to what the host's modulation adds.
 * Holding a trig (mouse, right-click latch or keyboard) shows that step: its
 * locked knobs light up with inverted labels, like the unit's LCD, the others
 * dim, and turning a knob writes a lock instead of the kit value.
 *
 * A knob's or slider's value lights up under the mouse; a double-click on it types
 * a new one (Enter or a click elsewhere takes it, Esc drops it), a double-click
 * elsewhere on the knob resets it.
 *
 * Each colour has one meaning: white value, amber lock, violet host modulation,
 * red trig, green trigless step, blue a MIDI note landing on a step.
 *
 * The 16 pads show one page of the pattern's up to 64 steps; SCALE picks the page,
 * the length and the speed, as the unit's SCALE key and SCALE SETUP window do.
 * SPDX-License-Identifier: GPL-3.0-only */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "mono_track_ui.h"

#ifndef MT_COMMIT
#define MT_COMMIT "unknown"   /* build_macos.sh passes the git commit */
#endif

/* Plug-ins share one host process, and Objective-C classes one namespace: the version keeps builds apart. */
#define MTEditorView MonoTrackEditorView_0_7_0

enum { C_TITLE, C_KNOB, C_TAB, C_SLIDER, C_SWITCH, C_KEYS, C_PAD, C_CLEAR, C_COPY, C_PASTE, C_LEN, C_SPEED, C_PAGE, C_SETUP, C_CLOSE, C_MAX = 160 };
/* Controls of the setup windows have these sections: KIT ASSIGN's KEY VEL and KEY TRK tabs. */
enum { S_VEL = 5, S_KEY = 6 };
typedef struct { int kind, id, section; NSRect r; const char *label; } Ctl;

static const char *const kSection[5] = {"SYN", "AMP", "FILT", "EFX", "LFO"};
static const char *const kSpeed[4] = {"1X", "2X", "3/4X", "3/2X"};
static const char *const kPadKey[16] = {"1", "2", "3", "4", "5", "6", "7", "8", "Q", "W", "E", "R", "T", "Y", "U", "I"};
/* Physical keys, so Shift and non-Latin layouts do not change them. */
static const unsigned short kKeyCode[16] = {18, 19, 20, 21, 23, 22, 26, 28, 12, 13, 14, 15, 17, 16, 32, 34};
static const double kTapSeconds = 0.25;   /* shorter is a tap (toggles the trig), longer only shows the step */
static const double kNoteFade = 0.7;      /* seconds a MIDI note's line takes to fade */
static const double kScrollPause = 0.25;  /* a wheel idle this long has finished its scroll */
static const double kScrollSpan = 1000;   /* points of scroll that sweep a whole range (400 before 0.5.1) */
static const double kPressFlash = 0.15;   /* seconds CPY or PST stays lit after a click */

static NSColor *C(unsigned rgb, double a) {
   return [NSColor colorWithSRGBRed:(rgb >> 16 & 255) / 255.0 green:(rgb >> 8 & 255) / 255.0 blue:(rgb & 255) / 255.0 alpha:a];
}
enum : unsigned { BG = 0x141518, PANEL = 0x1d1f24, LINE = 0x31343c, TEXT = 0xd5d8df, MUTED = 0x858a96, VALUE = 0xeef1f6,
                  LOCK = 0xffb020, MOD = 0xb18cff, TRIG = 0xff453a, TRIGLESS = 0x32d74b, NOTE = 0x4cc9ff, DARK = 0x101114,
                  FAST = 0xff5fa2, PLUME = 0x8fd3ff };   /* an LFO too quick for its graph's dot to follow; the dot's plume */

static void drawText(const char *s, NSRect r, CGFloat size, NSFontWeight w, NSColor *color, NSTextAlignment align) {
   NSMutableParagraphStyle *p = [NSMutableParagraphStyle new];
   p.alignment = align;
   p.lineBreakMode = NSLineBreakByClipping;
   NSDictionary *a = @{NSFontAttributeName: [NSFont monospacedSystemFontOfSize:size weight:w],
                       NSForegroundColorAttributeName: color, NSParagraphStyleAttributeName: p};
   [[NSString stringWithUTF8String:s] drawInRect:r withAttributes:a];
}
static void fillRound(NSRect r, CGFloat radius, NSColor *c) {
   [c setFill];
   [[NSBezierPath bezierPathWithRoundedRect:r xRadius:radius yRadius:radius] fill];
}
static void strokeRound(NSRect r, CGFloat radius, CGFloat width, NSColor *c) {
   [c setStroke];
   NSBezierPath *p = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(r, width / 2, width / 2) xRadius:radius yRadius:radius];
   p.lineWidth = width;
   [p stroke];
}
static double knobAngle(double f) { return (135.0 + 270.0 * f) * M_PI / 180.0; }   /* y grows downward */
static void arc(CGContextRef g, CGPoint o, double r, double f0, double f1, CGFloat width, NSColor *c) {
   const double a0 = knobAngle(fmin(f0, f1)), a1 = knobAngle(fmax(f0, f1));
   CGContextSetStrokeColorWithColor(g, c.CGColor);
   CGContextSetLineWidth(g, width);
   CGContextSetLineCap(g, kCGLineCapRound);
   CGContextBeginPath(g);
   CGContextAddArc(g, o.x, o.y, r, a0, a1, 0);
   CGContextStrokePath(g);
}
static void dot(CGContextRef g, CGPoint o, double r, double f, double size, NSColor *c) {
   const double a = knobAngle(f);
   CGContextSetFillColorWithColor(g, c.CGColor);
   CGContextFillEllipseInRect(g, CGRectMake(o.x + r * cos(a) - size, o.y + r * sin(a) - size, 2 * size, 2 * size));
}

@interface MTEditorView : NSView <NSTextFieldDelegate>
- (instancetype)initWithPlugin:(const clap_plugin_t *)plugin ui:(MtUi *)ui;
- (void)startRefresh;
- (void)stopRefresh;
- (void)closeEditing;
@end

@implementation MTEditorView {
   const clap_plugin_t *_p;
   const clap_plugin_params_t *_params;
   MtUi *_ui;
   clap_param_info_t _info[NPARAM];
   Ctl _c[C_MAX];
   int _n, _tab;
   uint64_t _sig[C_MAX];
   BOOL _keys;
   unsigned _heldMouse, _heldKey, _heldLatch, _edited;   /* step masks */
   NSTimeInterval _downAt[16], _noteAt[UI_STEPS];
   uint32_t _noteSeen[UI_STEPS];
   int _page;           /* the page the pads show */
   int _setup;          /* the setup window open: 0 VEL, 1 KEY, -1 none */
   int _hover;          /* the control whose value is under the mouse, or -1 */
   NSTextField *_field; /* a value being typed: for control _fieldCtl, parameter _fieldId, */
   int _fieldCtl, _fieldId;
   uint64_t _fieldSteps;   /* or the locks of these steps, held when it opened */
   double _lenValue;    /* LEN's drag or scroll, between whole steps */
   int _dragCtl, _dragId;
   BOOL _dragMoved, _dragGesture, _scrollIsLock, _scrollDead;
   unsigned _dragHeld, _scrollHeld;      /* the trigs held when this twist began */
   int _flashCtl;
   NSTimeInterval _flashAt, _scrollLast;
   double _dragValue;
   NSPoint _dragLast;
   int _scrollId;
   int8_t _rnd[3][8];   /* RND's steps as drawn: frozen while the LFO is too fast to follow */
   double _fast[3];     /* how fast each LFO graph shows its LFO, eased (lfoHz) */
   int32_t _seenPos[3];
   NSTimeInterval _movedAt[3], _lfoTick;
   double _scrollValue;
   NSTimeInterval _scrollUntil;
   id _refresh;
}

- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { (void)e; return YES; }
- (BOOL)acceptsFirstResponder { return _keys; }

- (void)add:(int)kind id:(int)ident section:(int)section rect:(NSRect)r label:(const char *)label {
   _c[_n++] = (Ctl){kind, ident, section, r, label};
}

- (instancetype)initWithPlugin:(const clap_plugin_t *)plugin ui:(MtUi *)ui {
   if (!(self = [super initWithFrame:NSMakeRect(0, 0, MT_GUI_W, MT_GUI_H)])) return nil;
   _p = plugin;
   _ui = ui;
   _params = plugin->get_extension(plugin, CLAP_EXT_PARAMS);
   for (int i = 0; i < NPARAM; ++i) _params->get_info(plugin, (clap_id)i, &_info[i]);
   _dragCtl = _dragId = _scrollId = _flashCtl = _setup = _hover = _fieldCtl = -1;
   [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:NSZeroRect
                                                      options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways | NSTrackingInVisibleRect
                                                        owner:self userInfo:nil]];

   for (int s = 0; s < 5; ++s) {
      const CGFloat x = 16 + 240 * s;
      if (s < 4) [self add:C_TITLE id:0 section:s rect:NSMakeRect(x, 12, 228, 30) label:kSection[s]];
      for (int k = 0; k < 8; ++k)
         [self add:C_KNOB id:s < 4 ? s * 8 + k : k section:s
              rect:NSMakeRect(x + 6 + (k % 4) * 54, 52 + (k / 4) * 106, 54, 104) label:NULL];
   }
   static const char *const tabs[3] = {"LFO1", "LFO2", "LFO3"};
   for (int l = 0; l < 3; ++l) [self add:C_TAB id:l section:4 rect:NSMakeRect(976 + 76 * l, 16, 76, 28) label:tabs[l]];

   const CGFloat rx = 1224, rw = 128;
   CGFloat ry = 12;
   const CGFloat rowH = 53;
   [self add:C_SLIDER id:P_LEVEL section:0 rect:NSMakeRect(rx, ry, rw, 44) label:"TRK LV"]; ry += rowH;
   [self add:C_SETUP id:0 section:0 rect:NSMakeRect(rx, ry, rw, 44) label:"VEL"]; ry += rowH;
   [self add:C_SETUP id:1 section:0 rect:NSMakeRect(rx, ry, rw, 44) label:"KEY"]; ry += rowH;
   [self add:C_SLIDER id:P_BEND section:0 rect:NSMakeRect(rx, ry, rw, 44) label:"BEND"]; ry += rowH;
   [self add:C_SLIDER id:P_MTUNE section:0 rect:NSMakeRect(rx, ry, rw, 44) label:"MAST TUN"]; ry += rowH;
   [self add:C_SWITCH id:P_POLY section:0 rect:NSMakeRect(rx, ry, rw, 44) label:"POLY"]; ry += rowH;
   [self add:C_KEYS id:0 section:0 rect:NSMakeRect(rx, ry, rw, 44) label:"KEYBOARD CAPTURE"];

   for (int i = 0; i < 16; ++i) {
      const CGFloat cx = i < 8 ? 43 + 54 * i : 505 + 54 * (i - 8);
      [self add:C_PAD id:i section:0 rect:NSMakeRect(cx - 24, 280, 48, 84) label:kPadKey[i]];
      [self add:C_COPY id:i section:0 rect:NSMakeRect(cx - 26, 364, 25, 20) label:"CPY"];
      [self add:C_PASTE id:i section:0 rect:NSMakeRect(cx + 1, 364, 25, 20) label:"PST"];
   }
   [self add:C_SWITCH id:P_TRIG_SYN section:1 rect:NSMakeRect(936, 294, 60, 26) label:"SYN"];
   [self add:C_SWITCH id:P_TRIG_FILT section:1 rect:NSMakeRect(1000, 294, 60, 26) label:"FILT"];
   [self add:C_SWITCH id:P_TRIG_LFO section:1 rect:NSMakeRect(1064, 294, 60, 26) label:"LFO"];
   [self add:C_SWITCH id:P_LOCK_END section:1 rect:NSMakeRect(1136, 294, 76, 26) label:"NOTE ENDS"];
   [self add:C_CLEAR id:0 section:0 rect:NSMakeRect(1136, 346, 76, 26) label:"CLR LOCKS"];
   [self add:C_LEN id:0 section:0 rect:NSMakeRect(936, 346, 48, 26) label:"LEN"];
   [self add:C_SPEED id:0 section:0 rect:NSMakeRect(988, 346, 38, 26) label:NULL];
   static const char *const pages[4] = {"1", "2", "3", "4"};
   for (int k = 0; k < 4; ++k) [self add:C_PAGE id:k section:0 rect:NSMakeRect(1030 + 24 * k, 346, 22, 26) label:pages[k]];
   /* the setup windows, as the unit's ASSIGN window lays out its tabs: rows 1 and 2 of PAGE DEST ADD; KEY TRK adds HPF and LPF */
   for (int w = 0; w < 2; ++w) {
      const NSRect r = [self setupRect:w];
      for (int k = 0; k < 6; ++k)
         [self add:C_KNOB id:P_ASSIGN + 6 * w + k section:S_VEL + w
              rect:NSMakeRect(r.origin.x + 8 + 54 * (k % 3), r.origin.y + 30 + 100 * (k / 3), 54, 100) label:NULL];
      [self add:C_CLOSE id:w section:S_VEL + w rect:NSMakeRect(NSMaxX(r) - 28, r.origin.y + 4, 22, 22) label:NULL];
   }
   const NSRect key = [self setupRect:1];
   [self add:C_SWITCH id:P_HPKEY section:S_KEY rect:NSMakeRect(key.origin.x + 180, key.origin.y + 58, 64, 44) label:"HPF"];
   [self add:C_SWITCH id:P_LPKEY section:S_KEY rect:NSMakeRect(key.origin.x + 180, key.origin.y + 158, 64, 44) label:"LPF"];
   return self;
}

/* ---- what the controls show --------------------------------------------------- */

- (unsigned)held { return _heldMouse | _heldKey | _heldLatch; }
- (int)stepOf:(int)pad { return UI_PAGE * _page + pad; }
- (int)length { return atomic_load_explicit(&_ui->length, memory_order_relaxed); }
- (int)lastPage { return ([self length] - 1) / UI_PAGE; }
- (int)playing { return atomic_load_explicit(&_ui->step, memory_order_relaxed); }
- (int)paramOf:(const Ctl *)c { return c->kind == C_KNOB && c->section == 4 ? P_LFO + _tab * 8 + c->id : c->id; }
- (double)base:(int)ident { double v = 0; _params->get_value(_p, (clap_id)ident, &v); return v; }
/* A setup window, over the LFO page, its right edge by the track column. */
- (NSRect)setupRect:(int)w { const CGFloat width = w ? 254 : 178; return NSMakeRect(1214 - width, 34, width, 232); }   /* down to the panels' bottom */
/* Whether a control is on screen: a setup window's only while it is open. */
- (BOOL)visible:(const Ctl *)c { return c->section < S_VEL || c->section - S_VEL == _setup; }
- (int)lockAt:(int)step id:(int)ident { return atomic_load_explicit(&_ui->lock[step][ident], memory_order_relaxed); }
/* The first held step's lock on this parameter, or -1. */
- (int)heldLock:(int)ident {
   const unsigned h = [self held];
   if (ident >= UI_LOCKS) return -1;
   for (int t = 0; t < UI_PAGE; ++t)
      if (h >> t & 1) { const int v = [self lockAt:[self stepOf:t] id:ident]; if (v >= 0) return v; }
   return -1;
}
- (int)activeLock:(int)ident { return ident < UI_LOCKS ? atomic_load_explicit(&_ui->active[ident], memory_order_relaxed) : -1; }
- (double)mod:(int)ident { return ident <= P_LEVEL ? atomic_load_explicit(&_ui->mod[ident], memory_order_relaxed) : 0; }
/* Needle: the base, or while a step is held, what that step locks. A drag owns only this value. */
- (double)shown:(int)ident {
   if (ident == _dragId) return [self clampTo:ident value:_dragValue];   /* the float is only the drag's memory: a stepped knob shows its step */
   const int lock = [self heldLock:ident];
   return lock >= 0 ? lock : [self base:ident];
}
- (double)clampTo:(int)ident value:(double)v {
   const clap_param_info_t *i = &_info[ident];
   v = fmin(fmax(v, i->min_value), i->max_value);
   return i->flags & CLAP_PARAM_IS_STEPPED ? round(v) : v;
}
/* What sounds: a sounding lock replaces the base; the host's modulation adds to either. */
- (double)effective:(int)ident shown:(double)b {
   const int active = [self held] ? -1 : [self activeLock:ident];
   return [self clampTo:ident value:(active >= 0 ? active : b) + [self mod:ident]];
}
- (double)fraction:(int)ident value:(double)v {
   const clap_param_info_t *i = &_info[ident];
   return i->max_value > i->min_value ? (v - i->min_value) / (i->max_value - i->min_value) : 0;
}
- (unsigned)stepPages:(int)step {
   unsigned pages = 0;
   for (int id = 0; id < UI_LOCKS; ++id)
      if ([self lockAt:step id:id] >= 0) pages |= 1u << (id >> 3);
   return pages;
}
- (int)heldLocksInSection:(int)section lfo:(int)l {
   int n = 0;
   const int first = section < 4 ? section * 8 : P_LFO + l * 8;
   for (int k = 0; k < 8; ++k)
      if ([self heldLock:first + k] >= 0) ++n;
   return n;
}

/* ---- drawing -------------------------------------------------------------------- */

- (void)drawRect:(NSRect)dirty {
   (void)dirty;
   CGContextRef g = NSGraphicsContext.currentContext.CGContext;
   [C(BG, 1) setFill];
   NSRectFill(self.bounds);
   for (int s = 0; s < 5; ++s) {
      const NSRect r = NSMakeRect(16 + 240 * s, 44, 228, 222);
      fillRound(r, 6, C(PANEL, 1));
      strokeRound(r, 6, 1, C(LINE, 1));
   }
   drawText("MIDI TRIGS", NSMakeRect(936, 278, 188, 14), 10, NSFontWeightMedium, C(MUTED, 1), NSTextAlignmentLeft);
   drawText("LOCKS", NSMakeRect(1136, 278, 76, 14), 10, NSFontWeightMedium, C(MUTED, 1), NSTextAlignmentLeft);
   drawText("SCALE", NSMakeRect(936, 330, 188, 14), 10, NSFontWeightMedium, C(MUTED, 1), NSTextAlignmentLeft);
   char about[64];   /* barely there: the version and the commit it was built from */
   snprintf(about, sizeof about, "Mono Track %s (%s)", _p->desc->version, MT_COMMIT);
   drawText(about, NSMakeRect(16, 2, 300, 12), 9, NSFontWeightRegular, C(MUTED, 0.3), NSTextAlignmentLeft);   /* above SYN, clear of the pads' CPY/PST */
   for (int i = 0; i < _n; ++i)
      if (_c[i].section < S_VEL) [self drawCtl:i context:g];
   if (_setup < 0) return;
   const NSRect w = [self setupRect:_setup];
   if (![self needsToDrawRect:NSInsetRect(w, -6, -6)]) return;
   fillRound(NSOffsetRect(NSInsetRect(w, -2, -2), 0, 3), 10, C(DARK, 0.7));   /* a shadow, so the window reads as on top */
   fillRound(w, 8, C(PANEL, 1));
   strokeRound(w, 8, 1, C(MUTED, 1));
   drawText(_setup ? "KEY SETUP" : "VEL SETUP", NSMakeRect(w.origin.x + 12, w.origin.y + 8, 140, 15), 11, NSFontWeightSemibold, C(TEXT, 1), NSTextAlignmentLeft);
   [C(LINE, 1) setFill];
   NSRectFill(NSMakeRect(w.origin.x + 1, w.origin.y + 29, w.size.width - 2, 1));
   for (int i = 0; i < _n; ++i)
      if (_c[i].section == S_VEL + _setup) [self drawCtl:i context:g];
}
- (void)drawCtl:(int)i context:(CGContextRef)g {
   const Ctl *c = &_c[i];
   if (![self needsToDrawRect:NSInsetRect(c->r, -3, -3)]) return;
   {
      switch (c->kind) {
      case C_TITLE: [self drawTitle:c]; break;
      case C_KNOB: [self drawKnob:c context:g]; break;
      case C_TAB: [self drawTab:c]; break;
      case C_SLIDER: [self drawSlider:c]; break;
      case C_SWITCH: case C_KEYS: [self drawSwitch:c]; break;
      case C_PAD: [self drawPad:c]; break;
      case C_CLEAR: [self drawClear:c]; break;
      case C_COPY: case C_PASTE: [self drawPadButton:c index:i]; break;
      case C_LEN: case C_SPEED: case C_PAGE: [self drawScale:c]; break;
      case C_SETUP: [self drawSetupButton:c]; break;
      case C_CLOSE:
         drawText("\u00d7", NSMakeRect(c->r.origin.x, c->r.origin.y + 1, c->r.size.width, 18), 15, NSFontWeightRegular, C(MUTED, 1), NSTextAlignmentCenter);
         break;
      }
   }
}
/* VEL and KEY: a button that opens its setup window, lit while it is open. */
- (void)drawSetupButton:(const Ctl *)c {
   const BOOL open = _setup == c->id;
   const NSRect box = NSMakeRect(c->r.origin.x, c->r.origin.y + 17, c->r.size.width, 24);
   drawText(c->label, NSMakeRect(c->r.origin.x, c->r.origin.y, c->r.size.width, 14), 10, NSFontWeightMedium, C(MUTED, 1), NSTextAlignmentLeft);
   if (open) fillRound(box, 5, C(VALUE, 1));
   else strokeRound(box, 5, 1, C(MUTED, 0.8));
   drawText("SETUP", NSMakeRect(box.origin.x, box.origin.y + 4, box.size.width, 15), 11, NSFontWeightSemibold,
            open ? C(DARK, 1) : C(TEXT, 0.85), NSTextAlignmentCenter);
}

- (void)drawTitle:(const Ctl *)c {
   const int n = [self held] ? [self heldLocksInSection:c->section lfo:0] : 0;
   drawText(c->label, NSMakeRect(c->r.origin.x, c->r.origin.y + 5, c->r.size.width, 22), 15, NSFontWeightSemibold,
            n ? C(LOCK, 1) : C(TEXT, 1), NSTextAlignmentCenter);
   if (n) [self drawChip:n at:NSMakePoint(NSMidX(c->r) + 34, c->r.origin.y + 7)];
}
- (void)drawChip:(int)n at:(NSPoint)o {
   char t[4];
   snprintf(t, sizeof t, "%d", n);
   const NSRect r = NSMakeRect(o.x, o.y, 16, 15);
   fillRound(r, 4, C(LOCK, 1));
   drawText(t, NSMakeRect(o.x, o.y + 1, 16, 13), 10, NSFontWeightBold, C(DARK, 1), NSTextAlignmentCenter);
}
- (void)drawTab:(const Ctl *)c {
   const BOOL on = c->id == _tab;
   const int n = [self held] ? [self heldLocksInSection:4 lfo:c->id] : 0;
   const NSRect r = NSInsetRect(c->r, 1, 0);
   fillRound(r, 5, on ? C(PANEL, 1) : C(BG, 1));
   strokeRound(r, 5, 1, on ? C(MUTED, 1) : C(LINE, 1));
   drawText(c->label, NSMakeRect(r.origin.x + 2, r.origin.y + 6, 40, 16), 12, NSFontWeightSemibold,
            n ? C(LOCK, 1) : on ? C(TEXT, 1) : C(MUTED, 1), NSTextAlignmentCenter);
   if (n) [self drawChip:n at:NSMakePoint(NSMaxX(r) - 22, r.origin.y + 6)];   /* a held step's locks take the graph's place */
   else [self drawLfo:c->id in:NSMakeRect(NSMaxX(r) - 31, r.origin.y + 6, 26, 16) on:on];
}

- (void)drawKnob:(const Ctl *)c context:(CGContextRef)g {
   const int ident = [self paramOf:c];
   if (mt_ui_unused(_p, ident)) {
      CGContextSetAlpha(g, 0.25);
      arc(g, CGPointMake(NSMidX(c->r), c->r.origin.y + 50), 17, 0, 1, 3, C(LINE, 1));
      drawText("-", NSMakeRect(c->r.origin.x, c->r.origin.y + 5, c->r.size.width, 14), 10, NSFontWeightMedium, C(MUTED, 1), NSTextAlignmentCenter);
      CGContextSetAlpha(g, 1);
      return;
   }
   const BOOL holding = [self held] != 0, lockable = ident < UI_LOCKS;
   const int lock = [self heldLock:ident];
   const BOOL lit = lock >= 0;
   const int active = holding ? -1 : [self activeLock:ident];
   const double b = [self shown:ident], eff = [self effective:ident shown:b];
   const CGPoint o = CGPointMake(NSMidX(c->r), c->r.origin.y + 50);
   const double r = 17, fb = [self fraction:ident value:b], fe = [self fraction:ident value:eff];
   const double origin = mt_ui_bipolar(_p, ident) ? [self fraction:ident value:64] : 0;

   /* the label: inverted on a locked knob, as the unit's LCD inverts it */
   const char *name = strrchr(_info[ident].name, ' ');
   name = name ? name + 1 : _info[ident].name;
   const NSRect labelBox = NSMakeRect(c->r.origin.x + 4, c->r.origin.y + 4, c->r.size.width - 8, 16);
   if (lit) fillRound(labelBox, 3, C(LOCK, 1));
   else if (active >= 0) strokeRound(labelBox, 3, 1, C(LOCK, 0.9));
   const CGFloat dim = holding && lockable && !lit ? 0.42 : 1;
   drawText(name, NSMakeRect(labelBox.origin.x, labelBox.origin.y + 1, labelBox.size.width, 14), 10, NSFontWeightSemibold,
            lit ? C(DARK, 1) : C(TEXT, dim), NSTextAlignmentCenter);

   CGContextSetAlpha(g, dim);
   arc(g, o, r, 0, 1, 3, C(LINE, 1));
   arc(g, o, r, origin, fb, 3, lit ? C(LOCK, 1) : C(VALUE, 1));
   if (active >= 0) {
      const double fa = [self fraction:ident value:active];
      if (fabs(fa - fb) > 1e-4) arc(g, o, r + 4, fb, fa, 2, C(LOCK, 1));
      dot(g, o, r + 4, fa, 2.5, C(LOCK, 1));
   }
   const double from = active >= 0 ? [self fraction:ident value:active] : fb;
   if (fabs(fe - from) > 1e-4) {
      arc(g, o, r + 7, from, fe, 2, C(MOD, 1));
      dot(g, o, r + 7, fe, 2.5, C(MOD, 1));
   }
   const double a = knobAngle(fb);
   CGContextSetStrokeColorWithColor(g, (lit ? C(LOCK, 1) : C(VALUE, 1)).CGColor);
   CGContextSetLineWidth(g, 2.2);
   CGContextSetLineCap(g, kCGLineCapRound);
   CGContextBeginPath(g);
   CGContextMoveToPoint(g, o.x + 5 * cos(a), o.y + 5 * sin(a));
   CGContextAddLineToPoint(g, o.x + (r - 3) * cos(a), o.y + (r - 3) * sin(a));
   CGContextStrokePath(g);
   CGContextSetAlpha(g, 1);

   char text[CLAP_NAME_SIZE];
   [self valueOf:c text:text];
   char *ratio = strstr(text, " (");   /* a ratio after the value, as ENS's "+7.02 st (3/2)", goes on a second line */
   if (ratio) *ratio++ = 0;
   const BOOL hot = c - _c == _hover;
   if (hot) fillRound([self valueBox:c], 3, C(MUTED, 0.3));
   NSColor *color = lit ? C(LOCK, 1) : hot ? C(VALUE, 1) : C(MUTED, holding && lockable ? 0.6 : 1);
   drawText(text, NSMakeRect(c->r.origin.x, c->r.origin.y + 80, c->r.size.width, 14), 10, NSFontWeightRegular, color, NSTextAlignmentCenter);
   if (ratio) drawText(ratio, NSMakeRect(c->r.origin.x, c->r.origin.y + 93, c->r.size.width, 14), 10, NSFontWeightRegular, color, NSTextAlignmentCenter);
}

- (void)drawSlider:(const Ctl *)c {
   const int ident = c->id;
   const double b = [self shown:ident], eff = [self effective:ident shown:b];
   char text[CLAP_NAME_SIZE];
   [self valueOf:c text:text];
   const BOOL hot = c - _c == _hover;
   drawText(c->label, NSMakeRect(c->r.origin.x, c->r.origin.y, 68, 14), 10, NSFontWeightMedium, C(MUTED, 1), NSTextAlignmentLeft);
   if (hot) fillRound([self valueBox:c], 3, C(MUTED, 0.3));
   drawText(text, NSMakeRect(c->r.origin.x + 44, c->r.origin.y, c->r.size.width - 44, 14), 10, NSFontWeightRegular, C(hot ? VALUE : TEXT, 1), NSTextAlignmentRight);
   const NSRect t = NSMakeRect(c->r.origin.x, c->r.origin.y + 20, c->r.size.width, 8);
   fillRound(t, 4, C(LINE, 1));
   const CGFloat x = t.origin.x + t.size.width * [self fraction:ident value:b];
   fillRound(NSMakeRect(t.origin.x, t.origin.y, x - t.origin.x, t.size.height), 4, C(VALUE, 0.85));
   fillRound(NSMakeRect(x - 3, t.origin.y - 4, 6, 16), 2, C(VALUE, 1));
   const CGFloat xe = t.origin.x + t.size.width * [self fraction:ident value:eff];
   if (fabs(xe - x) > 0.3) fillRound(NSMakeRect(fmin(x, xe), t.origin.y + 12, fabs(xe - x), 2.5), 1, C(MOD, 1));
}

/* The value a knob or slider writes: what sounds, but while it is dragged or a held
 * step locks it, the value it sets. */
- (void)valueOf:(const Ctl *)c text:(char *)text {
   const int ident = [self paramOf:c];
   const double b = [self shown:ident];
   text[0] = 0;
   _params->value_to_text(_p, (clap_id)ident, ident == _dragId || [self heldLock:ident] >= 0 ? b : [self effective:ident shown:b], text, CLAP_NAME_SIZE);
}
/* The value's text, hugged: lit under the mouse. */
- (NSRect)valueBox:(const Ctl *)c {
   static CGFloat cw;   /* the monospaced font's advance */
   if (!cw) cw = [@"0" sizeWithAttributes:@{NSFontAttributeName: [NSFont monospacedSystemFontOfSize:10 weight:NSFontWeightRegular]}].width;
   char t[CLAP_NAME_SIZE];
   [self valueOf:c text:t];
   const char *ratio = strstr(t, " (");
   const size_t n = ratio ? (size_t)(ratio - t) : strlen(t), m = ratio ? strlen(ratio + 2) : 0;
   const CGFloat tw = cw * (n > m ? n : m);
   if (c->kind == C_SLIDER) return NSMakeRect(NSMaxX(c->r) - tw - 4, c->r.origin.y - 1, tw + 7, 16);
   return NSMakeRect(NSMidX(c->r) - tw / 2 - 4, c->r.origin.y + 79, tw + 8, ratio ? 29 : 16);
}
/* Where the mouse lights the value and a double-click types it: on a knob its whole
 * line, so a value that just got shorter is still under a second click. */
- (NSRect)valueArea:(const Ctl *)c {
   const NSRect b = [self valueBox:c];
   return c->kind == C_SLIDER ? b : NSMakeRect(c->r.origin.x, b.origin.y, c->r.size.width, b.size.height);
}
- (int)valueHit:(NSPoint)pt {
   const int i = [self hit:pt];
   if (i < 0 || (_c[i].kind != C_KNOB && _c[i].kind != C_SLIDER) || mt_ui_unused(_p, [self paramOf:&_c[i]])) return -1;
   return NSPointInRect(pt, [self valueArea:&_c[i]]) ? i : -1;
}
- (void)setHover:(int)i {
   if (_field) i = -1;
   if (i == _hover) return;
   if (_hover >= 0) [self setNeedsDisplayInRect:NSInsetRect(_c[_hover].r, -3, -3)];
   if (i >= 0) [self setNeedsDisplayInRect:NSInsetRect(_c[i].r, -3, -3)];
   _hover = i;
}
- (void)hoverAt:(NSEvent *)e { [self setHover:[self valueHit:[self convertPoint:e.locationInWindow fromView:nil]]]; }
- (void)mouseMoved:(NSEvent *)e { [self hoverAt:e]; }
- (void)mouseExited:(NSEvent *)e { (void)e; [self setHover:-1]; }

/* ---- typing a value ------------------------------------------------------------ */

/* A field over the value, holding its text, selected. While a step is held it types
 * that step's lock, as turning the knob would. */
- (void)openField:(int)i {
   const Ctl *c = &_c[i];
   const int ident = [self paramOf:c];
   char t[CLAP_NAME_SIZE] = "";
   _params->value_to_text(_p, (clap_id)ident, [self shown:ident], t, sizeof t);
   [self endScroll];
   const unsigned h = [self held];
   _fieldCtl = i;
   _fieldId = ident;
   _fieldSteps = 0;
   if (ident < UI_LOCKS)
      for (int s = 0; s < UI_PAGE; ++s)
         if (h >> s & 1) _fieldSteps |= 1ull << [self stepOf:s];
   _edited |= h;   /* a key let go while typing is no tap */
   const NSRect box = [self valueBox:c];
   const CGFloat w = fmax(box.size.width + 8, c->kind == C_KNOB ? c->r.size.width : 64);
   NSTextField *f = [NSTextField textFieldWithString:[NSString stringWithUTF8String:t]];
   f.frame = NSMakeRect(c->kind == C_KNOB ? NSMidX(box) - w / 2 : NSMaxX(box) - w, box.origin.y, w, 17);
   f.font = [NSFont monospacedSystemFontOfSize:10 weight:NSFontWeightRegular];
   f.alignment = c->kind == C_KNOB ? NSTextAlignmentCenter : NSTextAlignmentRight;
   f.bezeled = f.bordered = NO;
   f.drawsBackground = YES;
   f.backgroundColor = C(DARK, 1);
   f.textColor = C(_fieldSteps ? LOCK : VALUE, 1);
   f.focusRingType = NSFocusRingTypeNone;
   f.cell.usesSingleLineMode = YES;
   f.cell.scrollable = YES;
   f.wantsLayer = YES;
   f.layer.cornerRadius = 3;
   f.layer.borderWidth = 1;
   f.layer.borderColor = C(_fieldSteps ? LOCK : MUTED, 1).CGColor;
   f.delegate = self;
   [self setHover:-1];
   _field = f;
   [self addSubview:f];
   [self.window makeKeyWindow];
   [self.window makeFirstResponder:f];
   if ([f.currentEditor isKindOfClass:NSTextView.class]) ((NSTextView *)f.currentEditor).insertionPointColor = C(VALUE, 1);
}
/* how: 0 drops the text (Esc); 1 takes it, or beeps and stays open when it reads as no
 * value (Enter, Tab); 2 takes it if it reads (the field left any other way). */
- (void)closeField:(int)how {
   NSTextField *f = _field;
   if (!f) return;
   double v = 0;
   const BOOL ok = how && _params->text_to_value(_p, (clap_id)_fieldId, f.stringValue.UTF8String, &v);
   if (how == 1 && !ok) { NSBeep(); [f.currentEditor selectAll:nil]; return; }   /* selectText: would end the editing */
   _field = nil;   /* first: giving up the focus below ends the editing, which calls back here */
   if (ok && _fieldSteps) {
      const int8_t x = (int8_t)lrint([self clampTo:_fieldId value:v]);
      for (int t = 0; t < UI_STEPS; ++t)
         if (_fieldSteps >> t & 1) atomic_store_explicit(&_ui->lock[t][_fieldId], x, memory_order_relaxed);
      mt_ui_pattern_changed(_p);
   } else if (ok) mt_ui_set(_p, _fieldId, v);
   [self.window makeFirstResponder:_keys ? self : nil];
   [f removeFromSuperview];
   [self setNeedsDisplayInRect:NSInsetRect(NSUnionRect(f.frame, _c[_fieldCtl].r), -3, -3)];
   _fieldCtl = -1;
   [self tick:nil];
}
- (BOOL)control:(NSControl *)control textView:(NSTextView *)tv doCommandBySelector:(SEL)sel {
   (void)control; (void)tv;
   if (sel == @selector(cancelOperation:)) [self closeField:0];
   else if (sel == @selector(insertNewline:) || sel == @selector(insertTab:) || sel == @selector(insertBacktab:)) [self closeField:1];
   else return NO;
   return YES;
}
- (void)controlTextDidEndEditing:(NSNotification *)n { (void)n; [self closeField:2]; }

- (BOOL)switchOn:(const Ctl *)c { return c->kind == C_KEYS ? _keys : [self base:c->id] >= 0.5; }
- (void)drawSwitch:(const Ctl *)c {
   const BOOL on = [self switchOn:c], inRow = c->section == 1;
   const NSRect box = inRow ? c->r : NSMakeRect(c->r.origin.x, c->r.origin.y + 17, c->r.size.width, 24);
   if (!inRow) drawText(c->label, NSMakeRect(c->r.origin.x, c->r.origin.y, c->r.size.width, 14), 10, NSFontWeightMedium, C(MUTED, 1), NSTextAlignmentLeft);
   if (on) fillRound(box, 5, C(VALUE, 1));
   else strokeRound(box, 5, 1, C(MUTED, 0.8));
   const char *t = inRow ? c->label : on ? "ON" : "OFF";
   drawText(t, NSMakeRect(box.origin.x, box.origin.y + (box.size.height - 15) / 2, box.size.width, 15), 11, NSFontWeightSemibold,
            on ? C(DARK, 1) : C(TEXT, 0.85), NSTextAlignmentCenter);
}

- (void)drawClear:(const Ctl *)c {
   const BOOL can = [self canClear];
   strokeRound(c->r, 5, 1, can ? C(LOCK, 1) : C(LINE, 1));
   drawText(c->label, NSMakeRect(c->r.origin.x, c->r.origin.y + (c->r.size.height - 14) / 2, c->r.size.width, 14), 9.5, NSFontWeightSemibold,
            can ? C(LOCK, 1) : C(MUTED, 0.55), NSTextAlignmentCenter);
}
- (BOOL)canClear {
   const unsigned h = [self held];
   for (int t = 0; t < UI_PAGE; ++t)
      if ((h >> t & 1) && [self stepPages:[self stepOf:t]]) return YES;
   return NO;
}

/* CPY and PST under a selected trig. A left click only taps the trig, so a selection
 * is a latch, or a key held past a tap. */
- (BOOL)padButtonsShown:(int)step {
   return (_heldLatch >> step & 1) || ((_heldKey >> step & 1) && CACurrentMediaTime() - _downAt[step] >= kTapSeconds);
}
- (BOOL)stepHasContent:(int)step {
   return (atomic_load_explicit(&_ui->trig, memory_order_relaxed) >> step & 1) || [self stepPages:step];
}
/* An empty step has nothing to copy, so it cannot replace what is on the clipboard. */
- (BOOL)padButtonEnabled:(const Ctl *)c { return c->kind == C_COPY ? [self stepHasContent:[self stepOf:c->id]] : _ui->clip_full; }
- (BOOL)flashing:(int)index { return index == _flashCtl && CACurrentMediaTime() - _flashAt < kPressFlash; }
- (void)drawPadButton:(const Ctl *)c index:(int)index {
   if (![self padButtonsShown:c->id]) return;
   const BOOL on = [self padButtonEnabled:c], lit = on && [self flashing:index];
   if (lit) fillRound(c->r, 3, C(VALUE, 1));
   else strokeRound(c->r, 3, 1, on ? C(TEXT, 0.9) : C(LINE, 1));
   drawText(c->label, NSMakeRect(c->r.origin.x, c->r.origin.y + 4, c->r.size.width, 13), 9, NSFontWeightSemibold,
            lit ? C(DARK, 1) : on ? C(TEXT, 1) : C(MUTED, 0.4), NSTextAlignmentCenter);
}
- (void)copyStep:(int)step {
   _ui->clip_full = 1;
   _ui->clip_trig = atomic_load_explicit(&_ui->trig, memory_order_relaxed) >> step & 1;
   for (int id = 0; id < UI_LOCKS; ++id) _ui->clip_lock[id] = (int8_t)[self lockAt:step id:id];
}
/* Replaces the step: its trig and every lock. */
- (void)pasteStep:(int)step {
   for (int id = 0; id < UI_LOCKS; ++id) atomic_store_explicit(&_ui->lock[step][id], _ui->clip_lock[id], memory_order_relaxed);
   if (_ui->clip_trig) atomic_fetch_or_explicit(&_ui->trig, 1ull << step, memory_order_relaxed);
   else atomic_fetch_and_explicit(&_ui->trig, ~(1ull << step), memory_order_relaxed);
   mt_ui_pattern_changed(_p);
}

/* The LED above a trig. The whole bar is the unit's LED colour (red trig, green
 * trigless). It also says what the unit cannot: its seven cells are the seven
 * data pages, and a page with a lock on this step burns amber. A step with locks
 * pulses, the unit's rapid flash slowed to stay comfortable to look at. */
- (double)pulse { return 0.72 + 0.28 * sin(2 * M_PI * 2.5 * CACurrentMediaTime()); }
- (int)pulseLevel { return (int)lround([self pulse] * 8); }
- (void)drawPad:(const Ctl *)c {
   const int step = [self stepOf:c->id];
   const BOOL trig = atomic_load_explicit(&_ui->trig, memory_order_relaxed) >> step & 1;
   const unsigned pages = [self stepPages:step];
   const BOOL held = [self held] >> c->id & 1, latched = _heldLatch >> c->id & 1;
   const BOOL playing = [self playing] == step;
   CGContextRef g = NSGraphicsContext.currentContext.CGContext;
   CGContextSaveGState(g);
   if (step >= [self length]) CGContextSetAlpha(g, 0.3);   /* past the pattern's end: kept, not played */
   const CGFloat cx = NSMidX(c->r), top = c->r.origin.y + 8, a = pages ? [self pulseLevel] / 8.0 : 1;
   const unsigned base = trig ? TRIG : pages ? TRIGLESS : LINE;
   for (int i = 0; i < 7; ++i) {
      const NSRect cell = NSMakeRect(cx - 21 + i * 6, top, 5, 9);
      const BOOL locked = pages >> i & 1;
      fillRound(cell, 1.2, locked ? C(LOCK, a) : C(base, base == LINE ? 1 : trig || pages ? a : 1));
   }
   if (playing) fillRound(NSMakeRect(cx - 21, top + 11, 41, 2), 1, C(VALUE, 1));

   const NSRect pad = NSMakeRect(cx - 21, c->r.origin.y + 28, 42, 42);
   [(held ? C(LOCK, 0.2) : C(PANEL, 1)) setFill];
   [[NSBezierPath bezierPathWithOvalInRect:pad] fill];
   [(held ? C(LOCK, 1) : playing ? C(VALUE, 1) : C(LINE, 1)) setStroke];
   NSBezierPath *ring = [NSBezierPath bezierPathWithOvalInRect:NSInsetRect(pad, 1, 1)];
   ring.lineWidth = held || playing ? 2 : 1;
   if (latched) { const CGFloat dash[2] = {4, 3}; [ring setLineDash:dash count:2 phase:0]; }
   [ring stroke];
   drawText(c->label, NSMakeRect(pad.origin.x, pad.origin.y + 12, pad.size.width, 18), 14, NSFontWeightSemibold,
            held ? C(LOCK, 1) : _keys ? C(TEXT, 1) : C(MUTED, 1), NSTextAlignmentCenter);
   const int note = [self noteLevel:step];
   if (note) fillRound(NSMakeRect(cx - 21, c->r.origin.y + 75, 42, 3), 1.5, C(NOTE, note / 16.0));
   CGContextRestoreGState(g);
}

/* SCALE: LEN, the speed, and the four pages. A page button shows that page on the pads
 * (filled), is lit while the pattern reaches it, and while the playhead is on that page
 * is outlined white with a tick where it is. A page the pattern does not reach yet extends it. */
- (void)drawScale:(const Ctl *)c {
   char t[16];
   if (c->kind == C_PAGE) {
      const int play = [self playing];
      const BOOL shown = c->id == _page, used = c->id <= [self lastPage], here = play >= 0 && play / UI_PAGE == c->id;
      if (shown) fillRound(c->r, 5, C(VALUE, 1));
      else strokeRound(c->r, 5, 1, here ? C(VALUE, 0.9) : used ? C(MUTED, 0.8) : C(LINE, 1));
      drawText(c->label, NSMakeRect(c->r.origin.x, c->r.origin.y + 5, c->r.size.width, 15), 11, NSFontWeightSemibold,
               shown ? C(DARK, 1) : used ? C(TEXT, 0.85) : C(MUTED, 0.5), NSTextAlignmentCenter);
      if (here)
         fillRound(NSMakeRect(c->r.origin.x + 3 + (c->r.size.width - 10) * (play % UI_PAGE) / 15.0, NSMaxY(c->r) - 5, 4, 2), 1,
                   shown ? C(DARK, 1) : C(VALUE, 1));
      return;
   }
   if (c->kind == C_LEN) snprintf(t, sizeof t, "LEN %d", [self length]);
   else snprintf(t, sizeof t, "%s", kSpeed[atomic_load_explicit(&_ui->speed, memory_order_relaxed) & 3]);
   strokeRound(c->r, 5, 1, C(MUTED, 0.8));
   drawText(t, NSMakeRect(c->r.origin.x, c->r.origin.y + 5, c->r.size.width, 15), 11, NSFontWeightSemibold, C(TEXT, 0.85), NSTextAlignmentCenter);
}
/* The pads show another page: what was held on this one is let go. */
- (void)showPage:(int)k {
   if (k == _page) return;
   _page = k;
   _heldKey = _heldLatch = 0;
   if (_dragId >= 0) [self endDrag];
   [self endScroll];
   self.needsDisplay = YES;
}
/* Opens setup window w (0 VEL, 1 KEY), or closes it at -1. A twist in the old one ends. */
- (void)showSetup:(int)w {
   if (w == _setup) return;
   if (_dragId >= 0) [self endDrag];
   _dragCtl = -1;
   [self endScroll];
   if (_setup >= 0) [self setNeedsDisplayInRect:NSInsetRect([self setupRect:_setup], -6, -6)];
   _setup = w;
   if (w >= 0) {
      [self setNeedsDisplayInRect:NSInsetRect([self setupRect:w], -6, -6)];
      for (int i = 0; i < _n; ++i)
         if (_c[i].section == S_VEL + w) _sig[i] = [self signature:&_c[i]];
   }
}
/* A MIDI note's line under its step: 16 at the note, fading to 0. */
- (int)noteLevel:(int)step {
   const double t = (CACurrentMediaTime() - _noteAt[step]) / kNoteFade;
   return t >= 1 ? 0 : (int)ceil((1 - t) * (1 - t) * 16);
}

/* ---- refresh: at the display rate, redrawing only what changed --------------------- */

static uint64_t mix(uint64_t h, double x) {
   uint64_t u;
   memcpy(&u, &x, sizeof u);
   return (h ^ u) * 0x100000001b3ULL;
}

/* ---- the LFO graphs ------------------------------------------------------------------ */

/* An LFO wave at x (0..1 of its cycle), in units of full scale: mt_wave's shapes. EXP
 * falls by e^-4 a cycle at any speed; RND is eight held steps at half scale. */
static double lfoShape(int wave, double x, const int8_t *rnd) {
   switch (wave) {
   case 0: return x <= 0.25 ? 4 * x : x > 0.75 ? 4 * x - 4 : 2 - 4 * x;
   case 1: return -lfoShape(0, x, rnd);
   case 3: return 1 - 2 * x;
   case 4: return x < 0.5 ? 1 : -1;
   case 5: return x < 0.5 ? -1 : 1;
   case 6: return exp(-4 * x);
   case 7: return -exp(-4 * x);
   case 8: return x < 0.5 ? 2 * x : 0;
   case 9: return x < 0.5 ? -2 * x : 0;
   case 10: return rnd[(int)fmin(x * 8, 7)] / 128.0;
   default: return 2 * x - 1;
   }
}
/* An LFO's speed in cycles a second, and how fast that is for its graph: 0 slow enough to
 * follow the dot, rising to 1 from 4 to 12 cycles a second. At 60 frames a second a faster
 * dot lands anywhere on the wave from one frame to the next and only flickers, so the dot
 * fades out and the whole wave turns pink instead. */
- (double)lfoHz:(int)l { return atomic_load_explicit(&_ui->lfo_inc[l], memory_order_relaxed) / 8388608.0 * 44100 / 384; }
/* Once a frame: each graph's speed eases toward its LFO's (a fade of about a quarter of a
 * second), the dot's last move is noted, and RND's steps are read while they can be followed. */
- (void)followLfos {
   const NSTimeInterval now = CACurrentMediaTime(), dt = fmin(now - _lfoTick, 0.1);
   _lfoTick = now;
   for (int l = 0; l < 3; ++l) {
      const double target = fmin(fmax(([self lfoHz:l] - 4) / 8, 0), 1);
      _fast[l] += (target - _fast[l]) * (1 - exp(-dt / 0.08));
      if (fabs(target - _fast[l]) < 0.02) _fast[l] = target;
      const int32_t pos = atomic_load_explicit(&_ui->lfo_pos[l], memory_order_relaxed);
      if (pos != _seenPos[l]) { _seenPos[l] = pos; _movedAt[l] = now; }
      if (_fast[l] < 0.5)
         for (int k = 0; k < 8; ++k) _rnd[l][k] = atomic_load_explicit(&_ui->lfo_rnd[l][k], memory_order_relaxed);
   }
}
/* The dot's plume: full while it moves, gone 0.3 s after it stops. */
- (double)lfoTrail:(int)l { return fmax(0, 1 - (CACurrentMediaTime() - _movedAt[l]) / 0.3); }
- (uint64_t)lfoSignature:(int)l {
   const int wave = atomic_load_explicit(&_ui->lfo_wave[l], memory_order_relaxed);
   uint64_t h = mix(mix(0, wave), atomic_load_explicit(&_ui->lfo_depth[l], memory_order_relaxed) == 0);
   h = mix(h, floor(_fast[l] * 32));
   if (_fast[l] < 1)
      h = mix(mix(h, floor(atomic_load_explicit(&_ui->lfo_pos[l], memory_order_relaxed) / 8388608.0 * 104)), floor([self lfoTrail:l] * 16));
   if (wave == 10)
      for (int k = 0; k < 8; ++k) h = mix(h, _rnd[l][k]);
   return h;
}
/* The wave over one cycle, drawn from its shape so it stays clean at any speed, on a zero
 * line, with a dot where the LFO is now trailing a comet's plume. Fast, the dot and plume
 * fade out while the wave turns pink and glows. */
- (void)drawLfo:(int)l in:(NSRect)g on:(BOOL)on {
   CGContextRef ctx = NSGraphicsContext.currentContext.CGContext;
   const int wave = atomic_load_explicit(&_ui->lfo_wave[l], memory_order_relaxed);
   const double fast = _fast[l], x0 = g.origin.x, w = g.size.width, mid = NSMidY(g), half = g.size.height / 2 - 1.5;
   const int8_t *rnd = _rnd[l];
   CGPoint (^at)(double) = ^(double x) { return CGPointMake(x0 + x * w, mid - lfoShape(wave, x, rnd) * half); };
   const double alpha = atomic_load_explicit(&_ui->lfo_depth[l], memory_order_relaxed) == 0 ? 0.4 : 1;   /* DPTH 0: it moves nothing */
   CGContextSaveGState(ctx);
   CGContextSetAlpha(ctx, alpha);
   CGContextSetLineWidth(ctx, 1);
   CGContextSetStrokeColorWithColor(ctx, C(MUTED, 0.5).CGColor);
   CGContextMoveToPoint(ctx, x0, mid);
   CGContextAddLineToPoint(ctx, NSMaxX(g), mid);
   CGContextStrokePath(ctx);
   CGMutablePathRef path = CGPathCreateMutable();
   enum { N = 104 };
   for (int i = 0; i <= N; ++i) {
      const CGPoint p = at(fmin((double)i / N, 0.9999));
      if (i) CGPathAddLineToPoint(path, NULL, p.x, p.y); else CGPathMoveToPoint(path, NULL, p.x, p.y);
   }
   CGContextSetLineJoin(ctx, kCGLineJoinRound);
   CGContextSetLineCap(ctx, kCGLineCapRound);
   if (fast > 0) {
      CGContextAddPath(ctx, path);
      CGContextSetLineWidth(ctx, 3.5);
      CGContextSetStrokeColorWithColor(ctx, C(FAST, 0.3 * fast).CGColor);
      CGContextStrokePath(ctx);
   }
   CGContextAddPath(ctx, path);
   CGContextSetLineWidth(ctx, 1.5);
   CGContextSetStrokeColorWithColor(ctx, [C(on ? VALUE : TEXT, 1) blendedColorWithFraction:fast ofColor:C(FAST, 1)].CGColor);
   CGContextStrokePath(ctx);
   CGPathRelease(path);
   if (fast < 1) {
      const double x = fmin(atomic_load_explicit(&_ui->lfo_pos[l], memory_order_relaxed) / 8388608.0, 0.9999);
      const double trail = [self lfoTrail:l], tail = fmin(fmax(0.1 + [self lfoHz:l] * 0.12, 0.1), 0.45);
      const CGPoint p = at(x);
      CGContextSetAlpha(ctx, alpha * (1 - fast));
      CGContextSetFillColorWithColor(ctx, C(on ? PANEL : BG, 1).CGColor);   /* a ring of the tab's own colour sets the dot off the wave */
      CGContextFillEllipseInRect(ctx, CGRectMake(p.x - 3.75, p.y - 3.75, 7.5, 7.5));
      if (trail > 0) {   /* the plume: strokes from ever nearer the dot, ever wider, adding up to a fading, widening tail */
         enum { K = 12, S = 36 };
         CGPoint q[S + 1];
         double qx[S + 1];
         for (int j = 0; j <= S; ++j) {
            qx[j] = x - tail * (1 - (double)j / S);
            if (qx[j] < 0) qx[j] += 1;
            q[j] = at(qx[j]);
         }
         for (int i = 0; i < K; ++i) {
            CGContextBeginPath(ctx);
            for (int j = i * S / K; j <= S; ++j)
               if (j == i * S / K || qx[j] < qx[j - 1]) CGContextMoveToPoint(ctx, q[j].x, q[j].y);   /* the cycle wraps */
               else CGContextAddLineToPoint(ctx, q[j].x, q[j].y);
            CGContextSetLineWidth(ctx, 1 + 3.5 * i / K);
            CGContextSetStrokeColorWithColor(ctx, C(PLUME, 0.16 * trail).CGColor);
            CGContextStrokePath(ctx);
         }
      }
      CGContextSetFillColorWithColor(ctx, C(VALUE, 1).CGColor);
      CGContextFillEllipseInRect(ctx, CGRectMake(p.x - 2.5, p.y - 2.5, 5, 5));
   }
   CGContextRestoreGState(ctx);
}
- (uint64_t)signature:(const Ctl *)c {
   uint64_t h = 0xcbf29ce484222325ULL ^ (uint64_t)c->kind;
   switch (c->kind) {
   case C_TITLE: return mix(h, [self held] ? [self heldLocksInSection:c->section lfo:0] : 0);
   case C_TAB: return mix(mix(mix(h, c->id == _tab), [self held] ? [self heldLocksInSection:4 lfo:c->id] : 0), [self lfoSignature:c->id]);
   case C_KNOB: case C_SLIDER: {
      const int ident = [self paramOf:c];
      const double b = [self shown:ident];
      h = mix(mix(mix(h, ident), b), [self effective:ident shown:b]);
      h = mix(mix(h, [self heldLock:ident]), [self activeLock:ident]);
      if ((ident >= P_LFO && ident < P_LEVEL && (ident - P_LFO) % 8 == 1) || (ident >= P_ASSIGN && (ident - P_ASSIGN) % 3 == 1))
         h = mix(h, [self base:ident - 1]);   /* DEST's name comes from its PAGE */
      return mix(mix(h, [self held] != 0), ident == _dragId);
   }
   case C_SWITCH: case C_KEYS: return mix(h, [self switchOn:c]);
   case C_SETUP: return mix(h, _setup == c->id);
   case C_CLEAR: return mix(h, [self canClear]);
   case C_COPY: case C_PASTE: {
      const BOOL shown = [self padButtonsShown:c->id];
      return mix(mix(h, shown), shown ? [self padButtonEnabled:c] * 2 + [self flashing:(int)(c - _c)] : 0);
   }
   case C_LEN: return mix(h, [self length]);
   case C_SPEED: return mix(h, atomic_load_explicit(&_ui->speed, memory_order_relaxed));
   case C_PAGE: {
      const int play = [self playing];
      return mix(mix(mix(h, c->id == _page), c->id <= [self lastPage]), play >= 0 && play / UI_PAGE == c->id ? play : -1);
   }
   default: {
      const int t = [self stepOf:c->id];
      const unsigned pages = [self stepPages:t];
      h = mix(mix(mix(h, t), atomic_load_explicit(&_ui->trig, memory_order_relaxed) >> t & 1), pages);
      h = mix(mix(h, [self held] >> c->id & 1), _heldLatch >> c->id & 1);
      h = mix(mix(mix(h, [self playing] == t), _keys), t >= [self length]);
      return mix(mix(h, pages ? [self pulseLevel] : 0), [self noteLevel:t]);
   }
   }
}
- (void)tick:(id)sender {
   (void)sender;
   if (_scrollId >= 0 && CACurrentMediaTime() > _scrollUntil) [self endScroll];
   [self checkHolds];
   if (_page > [self lastPage]) [self showPage:0];   /* the pattern got shorter: back to the first page, as the unit goes */
   for (int t = 0; t < UI_STEPS; ++t) {
      const uint32_t n = atomic_load_explicit(&_ui->notes[t], memory_order_relaxed);
      if (n != _noteSeen[t]) { _noteSeen[t] = n; _noteAt[t] = CACurrentMediaTime(); }
   }
   [self followLfos];
   for (int i = 0; i < _n; ++i) {
      if (![self visible:&_c[i]]) continue;
      const uint64_t s = [self signature:&_c[i]];
      if (s != _sig[i]) { _sig[i] = s; [self setNeedsDisplayInRect:NSInsetRect(_c[i].r, -3, -3)]; }
   }
}
- (void)startRefresh {
   if (_refresh || !self.window) return;
   for (int t = 0; t < UI_STEPS; ++t) _noteSeen[t] = atomic_load_explicit(&_ui->notes[t], memory_order_relaxed);   /* notes played while closed are old news */
   if (@available(macOS 14.0, *)) {
      CADisplayLink *link = [self displayLinkWithTarget:self selector:@selector(tick:)];
      link.preferredFrameRateRange = CAFrameRateRangeMake(30, 120, 120);
      [link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
      _refresh = link;
   } else {
      NSTimer *t = [NSTimer timerWithTimeInterval:1.0 / 60.0 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
      [NSRunLoop.mainRunLoop addTimer:t forMode:NSRunLoopCommonModes];
      _refresh = t;
   }
}
- (void)stopRefresh {
   [_refresh invalidate];   /* both retain their target and would keep ticking */
   _refresh = nil;
}
- (void)viewWillMoveToWindow:(NSWindow *)w {
   NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
   if (self.window) {
      [nc removeObserver:self name:NSWindowDidResignKeyNotification object:self.window];
      [nc removeObserver:self name:NSWindowDidBecomeKeyNotification object:self.window];
      [nc removeObserver:self name:NSWindowWillCloseNotification object:self.window];
   }
   if (!w) { [self closeEditing]; [self stopRefresh]; }
}
- (void)viewDidMoveToWindow {
   if (!self.window) return;
   NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
   [nc addObserver:self selector:@selector(windowResigned:) name:NSWindowDidResignKeyNotification object:self.window];
   [nc addObserver:self selector:@selector(windowBecameKey:) name:NSWindowDidBecomeKeyNotification object:self.window];
   [nc addObserver:self selector:@selector(windowClosing:) name:NSWindowWillCloseNotification object:self.window];
   [self startRefresh];
}
/* A key released while another window has focus never arrives. */
- (void)windowResigned:(NSNotification *)n { (void)n; _heldKey = 0; [self closeField:2]; }
- (void)windowBecameKey:(NSNotification *)n { (void)n; if (_keys && !_field) [self.window makeFirstResponder:self]; }
- (void)windowClosing:(NSNotification *)n { (void)n; [self closeEditing]; }

/* The window is going away: no trig stays held and no gesture stays open. */
- (void)closeEditing {
   [self closeField:2];
   _heldMouse = _heldKey = _heldLatch = 0;
   if (_dragId >= 0) [self endDrag];
   _dragCtl = -1;
   [self endScroll];
   self.needsDisplay = YES;
}
- (void)endDrag {
   if (_dragGesture) mt_ui_end(_p, _dragId);
   _dragGesture = NO;
   _dragCtl = _dragId = -1;
}
- (void)endScroll {
   if (_scrollId >= 0 && !_scrollIsLock) mt_ui_end(_p, _scrollId);
   _scrollId = -1;
}
/* A twist belongs to the trigs held when it began. When they change, the rest of it
 * does nothing, so a trackpad's momentum cannot spill from a lock into the kit value,
 * or from the kit value into a lock. */
- (void)checkHolds {
   const unsigned h = [self held];
   if (_dragId >= 0 && h != _dragHeld) [self endDrag];
   if (h != _scrollHeld) { [self endScroll]; _scrollDead = YES; }
}

/* ---- editing --------------------------------------------------------------------- */

- (int)hit:(NSPoint)pt {
   for (int i = 0; i < _n; ++i) {
      if (_c[i].kind == C_TITLE || ![self visible:&_c[i]]) continue;
      if (_setup >= 0 && _c[i].section < S_VEL && _c[i].kind != C_SETUP) continue;   /* an open window takes the mouse */
      if ((_c[i].kind == C_COPY || _c[i].kind == C_PASTE) && ![self padButtonsShown:_c[i].id]) continue;
      if (_c[i].kind == C_PAD) {
         const NSPoint o = NSMakePoint(NSMidX(_c[i].r), _c[i].r.origin.y + 49);
         if (pt.x >= _c[i].r.origin.x && pt.x < NSMaxX(_c[i].r) && pt.y >= _c[i].r.origin.y && hypot(pt.x - o.x, fmax(0, pt.y - o.y)) <= 24) return i;
      } else if (NSPointInRect(pt, _c[i].r)) return i;
   }
   return -1;
}
- (void)writeLock:(int)ident value:(double)v {
   const unsigned h = [self held];
   const int8_t x = (int8_t)lrint([self clampTo:ident value:v]);
   for (int t = 0; t < UI_PAGE; ++t)
      if (h >> t & 1) atomic_store_explicit(&_ui->lock[[self stepOf:t]][ident], x, memory_order_relaxed);
   _edited |= h;
   mt_ui_pattern_changed(_p);
}
- (void)clearLock:(int)ident {
   const unsigned h = [self held];
   for (int t = 0; t < UI_PAGE; ++t)
      if (h >> t & 1) atomic_store_explicit(&_ui->lock[[self stepOf:t]][ident], -1, memory_order_relaxed);
   _edited |= h;
   mt_ui_pattern_changed(_p);
}
- (void)toggleTrig:(int)step {
   atomic_fetch_xor_explicit(&_ui->trig, 1ull << [self stepOf:step], memory_order_relaxed);
   mt_ui_pattern_changed(_p);
}
- (void)padDown:(int)step { _downAt[step] = CACurrentMediaTime(); _edited &= ~(1u << step); }
/* A tap toggles the trig; a longer press, or one that edited a lock, leaves it alone. */
- (void)padUp:(int)step {
   if (!(_edited >> step & 1) && CACurrentMediaTime() - _downAt[step] < kTapSeconds) [self toggleTrig:step];
}
- (void)setKeys:(BOOL)on {
   _keys = on;
   if (on) { [self.window makeKeyWindow]; [self.window makeFirstResponder:self]; }
   else {
      _heldKey = 0;
      if (self.window.firstResponder == self) [self.window makeFirstResponder:nil];
   }
}

- (void)rightMouseDown:(NSEvent *)e {
   const int i = [self hit:[self convertPoint:e.locationInWindow fromView:nil]];
   if (i >= 0 && _c[i].kind == C_PAD) _heldLatch ^= 1u << _c[i].id;
}
- (void)mouseDown:(NSEvent *)e {
   [self closeField:2];   /* a click elsewhere takes what was typed */
   const NSPoint pt = [self convertPoint:e.locationInWindow fromView:nil];
   const int i = [self hit:pt];
   if (_setup >= 0 && i < 0 && !NSPointInRect(pt, [self setupRect:_setup])) { [self showSetup:-1]; return; }   /* a click outside closes it */
   if (i < 0) return;
   const Ctl *c = &_c[i];
   switch (c->kind) {
   case C_PAD:
      if (e.modifierFlags & NSEventModifierFlagControl) { _heldLatch ^= 1u << c->id; return; }   /* ctrl-click is right-click */
      _heldMouse |= 1u << c->id;
      [self padDown:c->id];
      _dragCtl = i;
      break;
   case C_TAB: _tab = c->id; _dragCtl = i; break;
   case C_PAGE:   /* past the end, or a double-click: the pattern ends with this page, as the unit's SCALE sets whole pages */
      if (c->id > [self lastPage] || e.clickCount == 2) mt_ui_set_length(_p, UI_PAGE * (c->id + 1));
      [self showPage:c->id];
      _dragCtl = i;
      break;
   case C_SETUP: [self showSetup:_setup == c->id ? -1 : c->id]; break;
   case C_CLOSE: [self showSetup:-1]; break;
   case C_SPEED:
      atomic_store_explicit(&_ui->speed, (int8_t)((atomic_load_explicit(&_ui->speed, memory_order_relaxed) + 1) & 3), memory_order_relaxed);
      mt_ui_pattern_changed(_p);
      break;
   case C_LEN:
      if (e.clickCount == 2) { mt_ui_set_length(_p, 16); break; }
      _lenValue = [self length];
      _dragCtl = i;
      _dragLast = pt;
      break;
   case C_KEYS: [self setKeys:!_keys]; break;
   case C_SWITCH: mt_ui_set(_p, c->id, [self base:c->id] >= 0.5 ? 0 : 1); break;
   case C_COPY: case C_PASTE:
      _edited |= 1u << c->id;        /* so releasing the key does not count as a tap */
      if ([self padButtonEnabled:c]) {
         if (c->kind == C_COPY) [self copyStep:[self stepOf:c->id]]; else [self pasteStep:[self stepOf:c->id]];
         _flashCtl = i;
         _flashAt = CACurrentMediaTime();
      }
      break;
   case C_CLEAR: {
      const unsigned h = [self held];
      for (int t = 0; t < UI_PAGE; ++t)
         if (h >> t & 1)
            for (int id = 0; id < UI_LOCKS; ++id) atomic_store_explicit(&_ui->lock[[self stepOf:t]][id], -1, memory_order_relaxed);
      _edited |= h;
      mt_ui_pattern_changed(_p);
      break;
   }
   case C_KNOB: case C_SLIDER: {
      const int ident = [self paramOf:c];
      if (mt_ui_unused(_p, ident)) return;
      const BOOL lockEdit = [self held] && ident < UI_LOCKS;
      if (e.clickCount == 2 && NSPointInRect(pt, [self valueArea:c])) { [self openField:i]; return; }
      if (e.clickCount == 2 && !lockEdit) {
         mt_ui_set(_p, ident, _info[ident].default_value);
         return;
      }
      _dragValue = [self shown:ident];
      _dragCtl = i;
      _dragId = ident;
      _dragHeld = [self held];
      _dragLast = pt;
      _dragMoved = NO;
      _dragGesture = !lockEdit;
      if (!lockEdit) mt_ui_begin(_p, ident);
      break;
   }
   }
   [self hoverAt:e];   /* a tab or a window may have put another control here */
   [self tick:nil];
}
- (void)mouseDragged:(NSEvent *)e {
   [self checkHolds];
   const NSPoint pt = [self convertPoint:e.locationInWindow fromView:nil];
   if (_dragCtl >= 0 && _c[_dragCtl].kind == C_TAB) {   /* a press on a tab selects whichever tab it slides onto */
      const int i = [self hit:pt];
      if (i >= 0 && _c[i].kind == C_TAB && _c[i].id != _tab) { _tab = _c[i].id; [self tick:nil]; }
      return;
   }
   if (_dragCtl >= 0 && _c[_dragCtl].kind == C_PAGE) {   /* the same for pages, but only those the pattern has */
      const int i = [self hit:pt];
      if (i >= 0 && _c[i].kind == C_PAGE && _c[i].id != _page && _c[i].id <= [self lastPage]) { [self showPage:_c[i].id]; [self tick:nil]; }
      return;
   }
   if (_dragCtl >= 0 && _c[_dragCtl].kind == C_LEN) {   /* 62 steps over 200 points, as a knob's range */
      [self moveLength:(_dragLast.y - pt.y) * 62 / 200 * (e.modifierFlags & NSEventModifierFlagShift ? 0.1 : 1)];
      _dragLast = pt;
      return;
   }
   if (_dragId < 0) return;
   const clap_param_info_t *info = &_info[_dragId];
   const double range = info->max_value - info->min_value, fine = e.modifierFlags & NSEventModifierFlagShift ? 0.1 : 1;
   const double delta = _c[_dragCtl].kind == C_SLIDER ? (pt.x - _dragLast.x) * range / _c[_dragCtl].r.size.width
                                                      : (_dragLast.y - pt.y) * range / 200;
   _dragLast = pt;
   if (delta == 0) return;
   _dragMoved = YES;
   _dragValue = fmin(fmax(_dragValue + delta * fine, info->min_value), info->max_value);   /* the float keeps slow drags moving through steps */
   if (_dragGesture) mt_ui_set(_p, _dragId, _dragValue);
   else [self writeLock:_dragId value:_dragValue];
   [self tick:nil];
}
- (void)mouseUp:(NSEvent *)e {
   [self checkHolds];
   if (_dragCtl >= 0 && _c[_dragCtl].kind == C_PAD) {
      const int step = _c[_dragCtl].id;
      _heldMouse &= ~(1u << step);
      [self padUp:step];
   } else if (_dragId >= 0) {
      if (_dragGesture) mt_ui_end(_p, _dragId);
      else if (!_dragMoved) {   /* click a knob while holding a trig: the unit's lock / unlock */
         if ([self heldLock:_dragId] >= 0) [self clearLock:_dragId];
         else [self writeLock:_dragId value:[self base:_dragId]];
      }
   }
   _dragCtl = _dragId = -1;
   _dragGesture = NO;
   [self hoverAt:e];
   [self tick:nil];
}
/* LEN moves by whole steps; the fraction stays for the next move. */
- (void)moveLength:(double)delta {
   if (lround(_lenValue) != [self length]) _lenValue = [self length];   /* changed elsewhere since */
   _lenValue = fmin(fmax(_lenValue + delta, 2), UI_STEPS);
   if (lround(_lenValue) != [self length]) mt_ui_set_length(_p, (int)lround(_lenValue));
   [self tick:nil];
}
- (void)scrollWheel:(NSEvent *)e {
   const NSTimeInterval now = CACurrentMediaTime();
   const BOOL wheel = e.phase == NSEventPhaseNone && e.momentumPhase == NSEventPhaseNone;
   const BOOL starts = e.phase == NSEventPhaseBegan || e.phase == NSEventPhaseMayBegin || (wheel && now - _scrollLast > kScrollPause);
   _scrollLast = now;
   if (starts) {
      [self endScroll];
      _scrollDead = NO;
      _scrollHeld = [self held];
   }
   [self checkHolds];
   const int i = [self hit:[self convertPoint:e.locationInWindow fromView:nil]];
   if (i >= 0 && _c[i].kind == C_LEN) {
      [self moveLength:e.scrollingDeltaY * (e.hasPreciseScrollingDeltas ? 1 : 8) * 62 / kScrollSpan * (e.modifierFlags & NSEventModifierFlagShift ? 0.1 : 1)];
      return;
   }
   if (_scrollDead) return;
   if (i < 0 || (_c[i].kind != C_KNOB && _c[i].kind != C_SLIDER)) return;
   const int ident = [self paramOf:&_c[i]];
   if (mt_ui_unused(_p, ident)) return;
   const clap_param_info_t *info = &_info[ident];
   const double px = e.scrollingDeltaY * (e.hasPreciseScrollingDeltas ? 1 : 8);
   const double delta = px * (info->max_value - info->min_value) / kScrollSpan * (e.modifierFlags & NSEventModifierFlagShift ? 0.1 : 1);
   if (delta == 0) return;         /* a gesture's begin and end events carry none */
   const BOOL lockEdit = [self held] && ident < UI_LOCKS;
   if (_scrollId != ident) {       /* one scroll is one gesture, ended in tick: after a pause */
      [self endScroll];
      _scrollId = ident;
      _scrollIsLock = lockEdit;
      _scrollValue = [self shown:ident];
      if (!lockEdit) mt_ui_begin(_p, ident);
   }
   _scrollUntil = now + kScrollPause;
   _scrollValue = fmin(fmax(_scrollValue + delta, info->min_value), info->max_value);   /* the float keeps small trackpad moves adding up */
   if (lockEdit) [self writeLock:ident value:_scrollValue];
   else mt_ui_set(_p, ident, _scrollValue);
   [self tick:nil];
}

- (int)padForKey:(NSEvent *)e {
   if (e.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption)) return -1;
   for (int i = 0; i < 16; ++i)
      if (kKeyCode[i] == e.keyCode) return i;
   return -1;
}
- (void)keyDown:(NSEvent *)e {
   const int step = _keys ? [self padForKey:e] : -1;
   if (step < 0) { [super keyDown:e]; return; }   /* every other key stays the host's */
   if (e.isARepeat) return;
   _heldKey |= 1u << step;
   [self padDown:step];
   [self tick:nil];
}
- (void)keyUp:(NSEvent *)e {
   const int step = _keys ? [self padForKey:e] : -1;
   if (step < 0 || !(_heldKey >> step & 1)) { [super keyUp:e]; return; }
   _heldKey &= ~(1u << step);
   [self padUp:step];
   [self tick:nil];
}
@end

/* ---- what the plug-in calls -------------------------------------------------------- */

void *mt_gui_create(const clap_plugin_t *p, MtUi *ui) {
   return (__bridge_retained void *)[[MTEditorView alloc] initWithPlugin:p ui:ui];
}
void mt_gui_destroy(void *gui) {
   MTEditorView *v = (__bridge_transfer MTEditorView *)gui;
   [v closeEditing];
   [v stopRefresh];
   [NSNotificationCenter.defaultCenter removeObserver:v];
   [v removeFromSuperview];
}
void mt_gui_set_parent(void *gui, void *ns_view) {
   [(__bridge NSView *)ns_view addSubview:(__bridge MTEditorView *)gui];
}
void mt_gui_set_size(void *gui, uint32_t w, uint32_t h) {
   MTEditorView *v = (__bridge MTEditorView *)gui;
   [v setFrameSize:NSMakeSize(w, h)];
   [v setBoundsSize:NSMakeSize(MT_GUI_W, MT_GUI_H)];   /* everything is laid out in these units */
   v.needsDisplay = YES;
}
void mt_gui_show(void *gui) {
   MTEditorView *v = (__bridge MTEditorView *)gui;
   v.hidden = NO;
   [v startRefresh];
}
void mt_gui_hide(void *gui) {
   MTEditorView *v = (__bridge MTEditorView *)gui;
   [v closeEditing];
   [v stopRefresh];
   v.hidden = YES;
}
