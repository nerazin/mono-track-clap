/* mono_track_ui.h - what the plug-in and its Cocoa editor share.
 * SPDX-License-Identifier: GPL-3.0-only */
#ifndef MONO_TRACK_UI_H
#define MONO_TRACK_UI_H

#include <clap/clap.h>
#include <stdatomic.h>
#include <stdint.h>

enum {
   P_SYN = 0, P_AMP = 8, P_FILT = 16, P_EFX = 24, P_LFO = 32, P_LEVEL = 56,
   P_LPKEY, P_HPKEY, P_BEND, P_MTUNE, P_POLY, P_TRIG_SYN, P_TRIG_FILT, P_TRIG_LFO, P_LOCK_END,
   P_ASSIGN, NPARAM = P_ASSIGN + 12   /* KIT ASSIGN: VEL rows 1 and 2, then KEY rows 1 and 2, each PAGE DEST ADD */
};
enum { UI_STEPS = 64, UI_PAGE = 16, UI_LOCKS = 56, UI_VOICES = 6 };

/* Written by the editor or the audio thread, read by the other: all relaxed. */
typedef struct {
   _Atomic uint64_t trig;                     /* the steps with a trig */
   _Atomic int8_t length, speed;              /* SCALE SETUP: 2-64 steps; 1X 2X 3/4X 3/2X */
   _Atomic int8_t lock[UI_STEPS][UI_LOCKS];   /* each step's locked values, -1 for none */
   _Atomic int8_t active[UI_LOCKS];           /* the lock sounding now, -1 for the kit value */
   _Atomic int step;                          /* the playing step, -1 when stopped */
   _Atomic float mod[NPARAM];                 /* the host's modulation amount */
   _Atomic uint32_t notes[UI_STEPS];          /* MIDI notes that landed on each step, counted */
   /* each LFO as it runs, on the track (POLY: the newest voice) */
   _Atomic int32_t lfo_pos[3];                /* place in the cycle, 0..0x800000 */
   _Atomic int32_t lfo_inc[3];                /* the step it moves by, 384 samples apart */
   _Atomic int8_t lfo_wave[3], lfo_depth[3];  /* the wave played (0-10) and DPTH, LFO modulation included */
   _Atomic int8_t lfo_rnd[3][8];              /* RND's eight steps, as last played, in 2^-7 of full scale */
   _Atomic int8_t lfo_mode[3];                /* TRIG as played: FREE TRIG HOLD ONE HALF */
   /* where each voice's last LFO trig sampled its LFO, newest voice first: POLY's voices holding keys, else one */
   _Atomic int8_t lfo_holds;                  /* how many voices below */
   _Atomic int32_t lfo_hold_pos[3][UI_VOICES]; /* place in the cycle, -1 before the voice's first trig */
   _Atomic int32_t lfo_hold_val[3][UI_VOICES]; /* the value held, full scale 0x800000 */
   /* the copied trig: main thread only, kept while the plug-in lives, not saved */
   int clip_full, clip_trig;
   int8_t clip_lock[UI_LOCKS];
} MtUi;

/* Implemented by the plug-in, called on the main thread. */
void mt_ui_begin(const clap_plugin_t *p, int id);
void mt_ui_set(const clap_plugin_t *p, int id, double value);   /* outside begin/end it is one whole gesture */
void mt_ui_end(const clap_plugin_t *p, int id);
void mt_ui_pattern_changed(const clap_plugin_t *p, const char *what);   /* one undo step, named, unless a change is open */
void mt_ui_change_begin(const clap_plugin_t *p);   /* the edits until mt_ui_change_end are one undo step */
void mt_ui_change_end(const clap_plugin_t *p);
void mt_ui_set_length(const clap_plugin_t *p, int length);   /* fills pages it reaches, as the unit does */
int mt_ui_bipolar(const clap_plugin_t *p, int id);
int mt_ui_unused(const clap_plugin_t *p, int id);

/* The editor (mono_track_gui.m). */
enum { MT_GUI_W = 1368, MT_GUI_H = 388 };
void *mt_gui_create(const clap_plugin_t *p, MtUi *ui);
void mt_gui_destroy(void *gui);
void mt_gui_set_parent(void *gui, void *ns_view);
void mt_gui_set_size(void *gui, uint32_t w, uint32_t h);
void mt_gui_show(void *gui);
void mt_gui_hide(void *gui);

#endif
