/* The Mono Track editor: one Cocoa view that draws and handles every control.
 *
 * Three values per knob. The needle is the kit value (base). An amber arc goes
 * to a lock sounding now, a violet arc to what the host's modulation adds.
 * Holding a trig (mouse, right-click latch or keyboard) shows that step: its
 * locked knobs light up with inverted labels, like the unit's LCD, the others
 * dim, and turning a knob writes a lock instead of the kit value.
 *
 * Each colour has one meaning: white value, amber lock, violet host modulation,
 * red trig, green trigless step, blue a MIDI note landing on a step.
 * SPDX-License-Identifier: GPL-3.0-only */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "mono_track_ui.h"

/* Plug-ins share one host process, and Objective-C classes one namespace: the version keeps builds apart. */
#define MTEditorView MonoTrackEditorView_0_4_0

enum { C_TITLE, C_KNOB, C_TAB, C_SLIDER, C_SWITCH, C_KEYS, C_PAD, C_CLEAR, C_COPY, C_PASTE, C_MAX = 128 };
typedef struct { int kind, id, section; NSRect r; const char *label; } Ctl;

static const char *const kSection[5] = {"SYN", "AMP", "FILT", "EFX", "LFO"};
static const char *const kPadKey[16] = {"1", "2", "3", "4", "5", "6", "7", "8", "Q", "W", "E", "R", "T", "Y", "U", "I"};
/* Physical keys, so Shift and non-Latin layouts do not change them. */
static const unsigned short kKeyCode[16] = {18, 19, 20, 21, 23, 22, 26, 28, 12, 13, 14, 15, 17, 16, 32, 34};
static const double kTapSeconds = 0.25;   /* shorter is a tap (toggles the trig), longer only shows the step */
static const double kNoteFade = 0.7;      /* seconds a MIDI note's line takes to fade */
static const double kScrollPause = 0.25;  /* a wheel idle this long has finished its scroll */
static const double kPressFlash = 0.15;   /* seconds CPY or PST stays lit after a click */

static NSColor *C(unsigned rgb, double a) {
   return [NSColor colorWithSRGBRed:(rgb >> 16 & 255) / 255.0 green:(rgb >> 8 & 255) / 255.0 blue:(rgb & 255) / 255.0 alpha:a];
}
enum : unsigned { BG = 0x141518, PANEL = 0x1d1f24, LINE = 0x31343c, TEXT = 0xd5d8df, MUTED = 0x858a96, VALUE = 0xeef1f6,
                  LOCK = 0xffb020, MOD = 0xb18cff, TRIG = 0xff453a, TRIGLESS = 0x32d74b, NOTE = 0x4cc9ff, DARK = 0x101114 };

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

@interface MTEditorView : NSView
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
   NSTimeInterval _downAt[16], _noteAt[16];
   uint32_t _noteSeen[16];
   int _dragCtl, _dragId;
   BOOL _dragMoved, _dragGesture, _scrollIsLock, _scrollDead;
   unsigned _dragHeld, _scrollHeld;      /* the trigs held when this twist began */
   int _flashCtl;
   NSTimeInterval _flashAt, _scrollLast;
   double _dragValue;
   NSPoint _dragLast;
   int _scrollId;
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
   _dragCtl = _dragId = _scrollId = _flashCtl = -1;

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
   [self add:C_SWITCH id:P_LPKEY section:0 rect:NSMakeRect(rx, ry, rw, 44) label:"LP KEY"]; ry += rowH;
   [self add:C_SWITCH id:P_HPKEY section:0 rect:NSMakeRect(rx, ry, rw, 44) label:"HP KEY"]; ry += rowH;
   [self add:C_SLIDER id:P_BEND section:0 rect:NSMakeRect(rx, ry, rw, 44) label:"BEND"]; ry += rowH;
   [self add:C_SLIDER id:P_MTUNE section:0 rect:NSMakeRect(rx, ry, rw, 44) label:"MAST TUN"]; ry += rowH;
   [self add:C_SWITCH id:P_POLY section:0 rect:NSMakeRect(rx, ry, rw, 44) label:"POLY"]; ry += rowH;
   [self add:C_KEYS id:0 section:0 rect:NSMakeRect(rx, ry, rw, 44) label:"KEYBOARD"];

   for (int i = 0; i < 16; ++i) {
      const CGFloat cx = i < 8 ? 43 + 54 * i : 505 + 54 * (i - 8);
      [self add:C_PAD id:i section:0 rect:NSMakeRect(cx - 24, 280, 48, 84) label:kPadKey[i]];
      [self add:C_COPY id:i section:0 rect:NSMakeRect(cx - 26, 364, 25, 20) label:"CPY"];
      [self add:C_PASTE id:i section:0 rect:NSMakeRect(cx + 1, 364, 25, 20) label:"PST"];
   }
   [self add:C_SWITCH id:P_TRIG_SYN section:1 rect:NSMakeRect(936, 302, 60, 28) label:"SYN"];
   [self add:C_SWITCH id:P_TRIG_FILT section:1 rect:NSMakeRect(1000, 302, 60, 28) label:"FILT"];
   [self add:C_SWITCH id:P_TRIG_LFO section:1 rect:NSMakeRect(1064, 302, 60, 28) label:"LFO"];
   [self add:C_SWITCH id:P_LOCK_END section:1 rect:NSMakeRect(1136, 302, 76, 28) label:"NOTE ENDS"];
   [self add:C_CLEAR id:0 section:0 rect:NSMakeRect(1136, 336, 76, 28) label:"CLR LOCKS"];
   return self;
}

/* ---- what the controls show --------------------------------------------------- */

- (unsigned)held { return _heldMouse | _heldKey | _heldLatch; }
- (int)paramOf:(const Ctl *)c { return c->kind == C_KNOB && c->section == 4 ? P_LFO + _tab * 8 + c->id : c->id; }
- (double)base:(int)ident { double v = 0; _params->get_value(_p, (clap_id)ident, &v); return v; }
- (int)lockAt:(int)step id:(int)ident { return atomic_load_explicit(&_ui->lock[step][ident], memory_order_relaxed); }
/* The first held step's lock on this parameter, or -1. */
- (int)heldLock:(int)ident {
   const unsigned h = [self held];
   if (ident >= UI_LOCKS) return -1;
   for (int t = 0; t < UI_STEPS; ++t)
      if (h >> t & 1) { const int v = [self lockAt:t id:ident]; if (v >= 0) return v; }
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
   drawText("MIDI TRIGS", NSMakeRect(936, 284, 188, 14), 10, NSFontWeightMedium, C(MUTED, 1), NSTextAlignmentLeft);
   drawText("LOCKS", NSMakeRect(1136, 284, 76, 14), 10, NSFontWeightMedium, C(MUTED, 1), NSTextAlignmentLeft);
   for (int i = 0; i < _n; ++i) {
      const Ctl *c = &_c[i];
      if (![self needsToDrawRect:NSInsetRect(c->r, -3, -3)]) continue;
      switch (c->kind) {
      case C_TITLE: [self drawTitle:c]; break;
      case C_KNOB: [self drawKnob:c context:g]; break;
      case C_TAB: [self drawTab:c]; break;
      case C_SLIDER: [self drawSlider:c]; break;
      case C_SWITCH: case C_KEYS: [self drawSwitch:c]; break;
      case C_PAD: [self drawPad:c]; break;
      case C_CLEAR: [self drawClear:c]; break;
      case C_COPY: case C_PASTE: [self drawPadButton:c index:i]; break;
      }
   }
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
   drawText(c->label, NSMakeRect(r.origin.x, r.origin.y + 6, r.size.width - (n ? 14 : 0), 16), 12, NSFontWeightSemibold,
            n ? C(LOCK, 1) : on ? C(TEXT, 1) : C(MUTED, 1), NSTextAlignmentCenter);
   if (n) [self drawChip:n at:NSMakePoint(NSMaxX(r) - 18, r.origin.y + 6)];
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

   char text[CLAP_NAME_SIZE] = "";
   _params->value_to_text(_p, (clap_id)ident, ident == _dragId || lit ? b : eff, text, sizeof text);
   drawText(text, NSMakeRect(c->r.origin.x, c->r.origin.y + 80, c->r.size.width, 14), 10, NSFontWeightRegular,
            lit ? C(LOCK, 1) : C(MUTED, holding && lockable ? 0.6 : 1), NSTextAlignmentCenter);
}

- (void)drawSlider:(const Ctl *)c {
   const int ident = c->id;
   const double b = [self shown:ident], eff = [self effective:ident shown:b];
   char text[CLAP_NAME_SIZE] = "";
   _params->value_to_text(_p, (clap_id)ident, ident == _dragId ? b : eff, text, sizeof text);
   drawText(c->label, NSMakeRect(c->r.origin.x, c->r.origin.y, 68, 14), 10, NSFontWeightMedium, C(MUTED, 1), NSTextAlignmentLeft);
   drawText(text, NSMakeRect(c->r.origin.x + 44, c->r.origin.y, c->r.size.width - 44, 14), 10, NSFontWeightRegular, C(TEXT, 1), NSTextAlignmentRight);
   const NSRect t = NSMakeRect(c->r.origin.x, c->r.origin.y + 20, c->r.size.width, 8);
   fillRound(t, 4, C(LINE, 1));
   const CGFloat x = t.origin.x + t.size.width * [self fraction:ident value:b];
   fillRound(NSMakeRect(t.origin.x, t.origin.y, x - t.origin.x, t.size.height), 4, C(VALUE, 0.85));
   fillRound(NSMakeRect(x - 3, t.origin.y - 4, 6, 16), 2, C(VALUE, 1));
   const CGFloat xe = t.origin.x + t.size.width * [self fraction:ident value:eff];
   if (fabs(xe - x) > 0.3) fillRound(NSMakeRect(fmin(x, xe), t.origin.y + 12, fabs(xe - x), 2.5), 1, C(MOD, 1));
}

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
   drawText(c->label, NSMakeRect(c->r.origin.x, c->r.origin.y + 7, c->r.size.width, 14), 9.5, NSFontWeightSemibold,
            can ? C(LOCK, 1) : C(MUTED, 0.55), NSTextAlignmentCenter);
}
- (BOOL)canClear {
   const unsigned h = [self held];
   for (int t = 0; t < UI_STEPS; ++t)
      if ((h >> t & 1) && [self stepPages:t]) return YES;
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
- (BOOL)padButtonEnabled:(const Ctl *)c { return c->kind == C_COPY ? [self stepHasContent:c->id] : _ui->clip_full; }
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
   if (_ui->clip_trig) atomic_fetch_or_explicit(&_ui->trig, (uint16_t)(1u << step), memory_order_relaxed);
   else atomic_fetch_and_explicit(&_ui->trig, (uint16_t)~(1u << step), memory_order_relaxed);
   mt_ui_pattern_changed(_p);
}

/* The LED above a trig. The whole bar is the unit's LED colour (red trig, green
 * trigless). It also says what the unit cannot: its seven cells are the seven
 * data pages, and a page with a lock on this step burns amber. A step with locks
 * pulses, the unit's rapid flash slowed to stay comfortable to look at. */
- (double)pulse { return 0.72 + 0.28 * sin(2 * M_PI * 2.5 * CACurrentMediaTime()); }
- (int)pulseLevel { return (int)lround([self pulse] * 8); }
- (void)drawPad:(const Ctl *)c {
   const int step = c->id;
   const BOOL trig = atomic_load_explicit(&_ui->trig, memory_order_relaxed) >> step & 1;
   const unsigned pages = [self stepPages:step];
   const BOOL held = [self held] >> step & 1, latched = _heldLatch >> step & 1;
   const BOOL playing = atomic_load_explicit(&_ui->step, memory_order_relaxed) == step;
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
- (uint64_t)signature:(const Ctl *)c {
   uint64_t h = 0xcbf29ce484222325ULL ^ (uint64_t)c->kind;
   switch (c->kind) {
   case C_TITLE: return mix(h, [self held] ? [self heldLocksInSection:c->section lfo:0] : 0);
   case C_TAB: return mix(mix(h, c->id == _tab), [self held] ? [self heldLocksInSection:4 lfo:c->id] : 0);
   case C_KNOB: case C_SLIDER: {
      const int ident = [self paramOf:c];
      const double b = [self shown:ident];
      h = mix(mix(mix(h, ident), b), [self effective:ident shown:b]);
      h = mix(mix(h, [self heldLock:ident]), [self activeLock:ident]);
      return mix(mix(h, [self held] != 0), ident == _dragId);
   }
   case C_SWITCH: case C_KEYS: return mix(h, [self switchOn:c]);
   case C_CLEAR: return mix(h, [self canClear]);
   case C_COPY: case C_PASTE: {
      const BOOL shown = [self padButtonsShown:c->id];
      return mix(mix(h, shown), shown ? [self padButtonEnabled:c] * 2 + [self flashing:(int)(c - _c)] : 0);
   }
   default: {
      const int t = c->id;
      const unsigned pages = [self stepPages:t];
      h = mix(mix(h, atomic_load_explicit(&_ui->trig, memory_order_relaxed) >> t & 1), pages);
      h = mix(mix(h, [self held] >> t & 1), _heldLatch >> t & 1);
      h = mix(mix(h, atomic_load_explicit(&_ui->step, memory_order_relaxed) == t), _keys);
      return mix(mix(h, pages ? [self pulseLevel] : 0), [self noteLevel:t]);
   }
   }
}
- (void)tick:(id)sender {
   (void)sender;
   if (_scrollId >= 0 && CACurrentMediaTime() > _scrollUntil) [self endScroll];
   [self checkHolds];
   for (int t = 0; t < UI_STEPS; ++t) {
      const uint32_t n = atomic_load_explicit(&_ui->notes[t], memory_order_relaxed);
      if (n != _noteSeen[t]) { _noteSeen[t] = n; _noteAt[t] = CACurrentMediaTime(); }
   }
   for (int i = 0; i < _n; ++i) {
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
- (void)windowResigned:(NSNotification *)n { (void)n; _heldKey = 0; }
- (void)windowBecameKey:(NSNotification *)n { (void)n; if (_keys) [self.window makeFirstResponder:self]; }
- (void)windowClosing:(NSNotification *)n { (void)n; [self closeEditing]; }

/* The window is going away: no trig stays held and no gesture stays open. */
- (void)closeEditing {
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
      if (_c[i].kind == C_TITLE) continue;
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
   for (int t = 0; t < UI_STEPS; ++t)
      if (h >> t & 1) atomic_store_explicit(&_ui->lock[t][ident], x, memory_order_relaxed);
   _edited |= h;
   mt_ui_pattern_changed(_p);
}
- (void)clearLock:(int)ident {
   const unsigned h = [self held];
   for (int t = 0; t < UI_STEPS; ++t)
      if (h >> t & 1) atomic_store_explicit(&_ui->lock[t][ident], -1, memory_order_relaxed);
   _edited |= h;
   mt_ui_pattern_changed(_p);
}
- (void)toggleTrig:(int)step {
   atomic_fetch_xor_explicit(&_ui->trig, (uint16_t)(1u << step), memory_order_relaxed);
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
   const NSPoint pt = [self convertPoint:e.locationInWindow fromView:nil];
   const int i = [self hit:pt];
   if (i < 0) return;
   const Ctl *c = &_c[i];
   switch (c->kind) {
   case C_PAD:
      if (e.modifierFlags & NSEventModifierFlagControl) { _heldLatch ^= 1u << c->id; return; }   /* ctrl-click is right-click */
      _heldMouse |= 1u << c->id;
      [self padDown:c->id];
      _dragCtl = i;
      break;
   case C_TAB: _tab = c->id; break;
   case C_KEYS: [self setKeys:!_keys]; break;
   case C_SWITCH: mt_ui_set(_p, c->id, [self base:c->id] >= 0.5 ? 0 : 1); break;
   case C_COPY: case C_PASTE:
      _edited |= 1u << c->id;        /* so releasing the key does not count as a tap */
      if ([self padButtonEnabled:c]) {
         if (c->kind == C_COPY) [self copyStep:c->id]; else [self pasteStep:c->id];
         _flashCtl = i;
         _flashAt = CACurrentMediaTime();
      }
      break;
   case C_CLEAR: {
      const unsigned h = [self held];
      for (int t = 0; t < UI_STEPS; ++t)
         if (h >> t & 1)
            for (int id = 0; id < UI_LOCKS; ++id) atomic_store_explicit(&_ui->lock[t][id], -1, memory_order_relaxed);
      _edited |= h;
      mt_ui_pattern_changed(_p);
      break;
   }
   case C_KNOB: case C_SLIDER: {
      const int ident = [self paramOf:c];
      if (mt_ui_unused(_p, ident)) return;
      const BOOL lockEdit = [self held] && ident < UI_LOCKS;
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
   [self tick:nil];
}
- (void)mouseDragged:(NSEvent *)e {
   [self checkHolds];
   if (_dragId < 0) return;
   const NSPoint pt = [self convertPoint:e.locationInWindow fromView:nil];
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
   (void)e;
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
   if (_scrollDead) return;
   const int i = [self hit:[self convertPoint:e.locationInWindow fromView:nil]];
   if (i < 0 || (_c[i].kind != C_KNOB && _c[i].kind != C_SLIDER)) return;
   const int ident = [self paramOf:&_c[i]];
   if (mt_ui_unused(_p, ident)) return;
   const clap_param_info_t *info = &_info[ident];
   const double px = e.scrollingDeltaY * (e.hasPreciseScrollingDeltas ? 1 : 8);
   const double delta = px * (info->max_value - info->min_value) / 400 * (e.modifierFlags & NSEventModifierFlagShift ? 0.1 : 1);
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
