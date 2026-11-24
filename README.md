# Mono Track

A whole track of the Elektron Monomachine around each synth machine, as eight
CLAP instruments for Bitwig (macOS, universal binary). Each one is the machine
with everything the unit puts after it: EQ, filter with key tracking and its
envelope, DIST, the AMP envelope, SRR, the tempo-synced delay, LEVEL, and the
three LFOs.

The DSP is a fixed-point C port of Monomachine OS 1.32B, written from its
DSP56300 kernel and machines. The LFOs and knob smoothing are a port of its
main-CPU code. Both were checked sample for sample against that code running
in emulators. No firmware code is in this repository. Its tables are generated
from formulas at start-up, except the oscillators' band-limited impulse (515
words), which no formula reproduces and which is copied from the firmware (the
same table Mono Ringmod and Mono SWAVE ship).

The bundle `Mono Track.clap` holds:

| Plug-in | Machine |
|---|---|
| Mono Track GND SIN | GND SIN |
| Mono Track GND NOIS | GND NOIS |
| Mono Track SWAVE SAW | SWAVE SAW |
| Mono Track SWAVE PULS | SWAVE PULS |
| Mono Track SWAVE ENS | SWAVE ENS, with its chorus |
| Mono Track FM+ STAT | FM+ STAT |
| Mono Track FM+ PAR | FM+ PAR |
| Mono Track FM+ DYN | FM+ DYN |

Each is monophonic, as a Monomachine track is, until POLY is on. Each has an
editor window with 16 trigs and parameter locks.

## The editor

The top row is the unit's data pages: SYN, AMP, FILT, EFX and LFO1-3. The right
column holds the track's settings, and the bottom row the 16 trigs. Pressing an
LFO tab and sliding across the others switches to each one under the pointer.

Each LFO tab shows its LFO: the wave it plays over one cycle, on a zero line,
and a dot where it is now, trailing a short glowing plume. The wave is drawn from its shape rather than from
samples, so it stays clean at any speed. RND shows its eight steps as they are
played. From about 4 cycles a second, where a moving dot starts to flicker, the
dot fades out and the whole wave fades to pink; from 12 the graph is pink and
holds still. With DPTH at 0 the graph is dimmed.

On a knob, the needle is the stored value. An amber arc goes to a lock sounding
now, and a violet arc shows what the host's modulation adds.

**Trigs** run on the host's transport, one per sixteenth note, and follow its
position. Notes come only from MIDI: a trig plays no note. It restarts the filter
envelope and the LFOs, and ends locks.

| Do this | To get this |
|---|---|
| Click a trig | Toggle it |
| Press and hold it | See its locks without changing it |
| Right-click or ctrl-click it | Latch the hold, so the mouse is free. Several can be held. Do it again to release. Closing the window releases all of them. |
| Turn a knob while holding | Lock that value to the held steps. The stored value does not change. A drag or scroll belongs to the trigs held when it began: if they change, the rest of it does nothing, including a trackpad's momentum. |
| Click a knob while holding | Lock it at its value, or remove its lock |
| **CPY** / **PST** under a selected trig | Copy the step's trig and all its locks, or replace this step with the copy. A trig is selected by a right-click latch or a held key. PST is gray until something is copied, and CPY is gray on an empty step. The copy lasts while the plug-in is loaded and is not saved with the project. |
| **CLR LOCKS** | Remove all locks from the held steps |
| **NOTE ENDS** (on by default) | A MIDI note ends the locks of earlier steps. Off, only a trig or Stop ends them, as on the unit. |
| **KEYBOARD** on (off by default) | `12345678` and `QWERTYUI` act as the 16 trigs: tap to toggle, hold to edit. They follow the key's position, so any keyboard layout works. Other keys stay with the host. |

**The LED bar above each trig** is red for a trig and green for a trigless step
(locks but no trig), the unit's colours. Its seven cells are the seven data
pages, and a page with a lock on that step turns amber. A step with locks pulses,
in place of the unit's rapid flash. A white line marks the playing step.

**A blue line below a trig** appears when a MIDI note lands on that step during
playback, and fades.

**While a trig is held**, its locked knobs get an inverted amber label, like the
unit's inverted LCD field, and show the locked value. All other knobs dim. The
section titles and LFO tabs count the locks they hold.

**MIDI TRIGS** (SYN, FILT, LFO, all on by default) choose what a MIDI note trigs:
the machine and AMP envelope, the filter envelope, and the LFOs. With SYN off, a
note only sets the pitch.

## Signal flow

The order is the kernel's, per 16-sample block:

```
note + PORT glide + TUNE + master tune + LFO/bend pitch
        │
        ▼
     machine ─► EQ ─► filter (HP + LP) + DIST ─► AMP (envelope, VOL, PAN) ─► SRR ─► [ENS chorus]
                        ▲                                                             │
     filter envelope ───┤                                                     dry ────┼──► delay
     key tracking ──────┘                                                             ▼      │
                                                                                    LEVEL ◄──┘
                                                                                      │
                                                                                 24-bit limit ─► out
```

LEVEL scales the delay's echoes too, as on the unit. Key tracking follows the
whole pitch (glide, TUNE, LFO on PTCH, bend, master tune), so a pitch LFO also
sweeps a key-tracked filter.

## Remote-control pages

Bitwig's remote controls have the unit's seven data pages, and an eighth for
the track's own settings. All knobs can be automated and modulated.

| Page | Knobs |
|---|---|
| **SYN** | The machine's eight knobs, with the unit's names (below). Knobs a machine does not read are left out. |
| **AMP** | ATK HOLD DEC REL DIST VOL PAN PORT |
| **FILT** | BASE WDTH HPQ LPQ ATK DEC BOFS WOFS |
| **EFX** | EQF EQG SRR DTIM DSND DFB DBAS DWID |
| **LFO1, LFO2, LFO3** | PAGE DEST TRIG WAVE MULT SPD INTL DPTH |
| **TRACK** | LEVEL, LP KEY, HP KEY, BEND, MASTER TUNE, POLY |

SYN by machine:

| Machine | Knobs |
|---|---|
| GND SIN | TUNE |
| GND NOIS | ST RED STON TUNE |
| SWAVE SAW | UNIL UNIW UNIX SUBX SUB1 SUB2 TUNE |
| SWAVE PULS | UNIL UNIW SUB1 SUB2 PW PWAD PWRS TUNE |
| SWAVE ENS | PCH2 PCH3 PCH4 WAVE PW CHRL CHRW TUNE |
| FM+ STAT | 1FRQ 1FIN 1ENV 1FB 2FRQ 2VOL TONE TUNE |
| FM+ PAR | 1FRQ 1ENV 2FRQ 2ENV 3FRQ 3ENV TONE TUNE |
| FM+ DYN | 1FRQ 1FEN 1VOL 1VEN 2FRQ 2ENV 2FB TUNE |

Values read as on the unit's display, taken from the knob descriptors in its
main OS:

- Knobs the unit shows as −64 to +63 read as the value − 64: TUNE, PW on PULS
  and ENS, 1FIN on FM+ STAT, 1FEN and 1VEN on FM+ DYN, DIST, PAN, BOFS, WOFS,
  EQG and DSND. All others read 0–127.
- NOIS's STON and PULS's PWRS read OFF or ON (64 and up).
- FM+ STAT's and PAR's 1FRQ, 2FRQ and 3FRQ read the listed ratio, 1/64 to 4.
- FM+ DYN's 1FRQ and 2FRQ read their ratio as the unit writes it: a fraction
  for the listed ratios ("1/4", "9/64"), else three decimals below 1 and two
  above, cut off (".046", "1.33"). At 127 they read 2.0 and 4.0, like the
  unit's display; the ratios there are 1.98 and 3.94.
- ENS's PCH2, PCH3 and PCH4 show each oscillator's interval to the first in
  semitones. Equal-tempered steps read as whole semitones ("+7 st"), and the
  just intervals show their cents and the ratio the unit names them by
  ("+7.02 st (3/2)", "-3.86 st (4/5)"); the editor puts the ratio on a second
  line. The unit writes the steps "+07". At unison the oscillator is muted, so
  the knob reads OFF.

Typed values work the same way: semitones or a ratio such as "3/2" on ENS, a
ratio such as "3/16" or "1.5" on the FM+ frequencies, OFF or ON.

On two kinds of knob the unit's display names a different step from the one it
plays, and the plug-in names the step that plays:

- **ENS PCH2-4.** The display picks the interval as knob × 33 / 128 rounded
  down; the DSP's table is centred, half a step earlier. On 64 of the 128 knob
  values the display names the interval below the one that sounds.
- **FM+ STAT and PAR xFRQ.** The display's list has 5/32 in the wrong place
  (after 3/8), so from 27 to 53 it names the next ratio up. The DSP also picks
  one step higher than the display at 5, 21, 37, ... 117 (every 16).

Both come from reading the main OS and the DSP code; they were not checked on a
unit.

The LFO controls:

- **PAGE**: PTCH, SYNT, AMP, FILT, EFFX, LFO1, LFO2, LFO3, MIDI. MIDI has no
  effect here, since a plug-in has no MIDI track to send to.
- **DEST**: the knob on that page, shown by name. On PTCH it is the range,
  1/12 to 16OC; on MIDI, LEN VEL PB PCHG CC 1-4.
- **TRIG**: FREE, TRIG, HOLD, ONE, HALF. A note is the LFO trig.
- **WAVE**: TRI, ITRI, SAW, ISAW, SQR, ISQR, EXP, IEXP, RMP, IRMP, RND.
- **MULT**: 1X to 64X. **SPD**, **INTL** and **DPTH**: 0–127.

LFO speed, HOLD, the portamento and the delay time all follow the host's tempo.

The TRACK page:

- **LEVEL** is the track's level.
- **LP KEY** and **HP KEY** are the kit's key-tracking switches (KIT ASSIGN KEY:
  LPF and HPF), both on by default.
- **BEND** is the range of MIDI pitch bend, 1/12 to 16OC, sent as KIT ASSIGN
  JOY R/L would be with PAGE PTCH.
- **MASTER TUNE** is 400.0–499.9 Hz.
- **POLY** turns the plug-in into six whole tracks playing one sound.

## How it behaves

- **Notes.** One voice. The last note held wins, and releasing it returns to
  the previous note still held, with a new trig.
- **POLY.** As on the unit, each of the six voices is a whole track, with its
  own envelopes, LFO phases, delay line and SRR. Notes go through a port of the
  firmware's allocator: a free voice that last played the pitch, otherwise the
  one free the longest, and a seventh note steals the oldest. The voices are not
  scaled down, so a chord can reach the 24-bit limiter. Turning POLY on or off
  releases all notes.
- **Locks.** A lock is applied when the playhead reaches its step, trig or no
  trig, and lands at once with no knob smoothing. It holds until the next MIDI
  note or trig that does not lock that parameter. A note on a locked step keeps
  that step's locks. With NOTE ENDS off, only a trig ends a lock. In POLY a lock
  reaches all six voices. Stop returns every parameter to its stored value.
- **A trig in POLY** restarts the filter envelope and LFOs of one voice only, as
  the firmware does for a pitchless trig.
- **Timing.** The main CPU's timing is kept: notes and modulation are taken
  every 48 samples, and knobs glide to their new value in steps every 384
  samples, as they do on the unit.
- **Sample rate.** The track runs at 44.1 kHz. At other rates its output goes
  through a windowed-sinc resampler, and the latency is reported to the host.
  At 44.1 kHz the latency is 0.
- **Start.** The plug-in starts with the AMP envelope idle and the delay at its
  time. NOIS's red noise and ENS's chorus start already run in, as they are
  long before a first note on the unit.

## Differences from the hardware

- **One track.** The buses between tracks are not here: NEIBOR and bus inputs,
  OUT BUS, AB=MIX and 6×MONO. SID and WAVE, which read a neighbour's pitch, are
  not among these machines. The output is the track's own, after its 24-bit
  limiter.
- **The sequencer plays no notes.** Trigs and locks act on notes from MIDI. On
  the unit a trig also plays its own note, and MIDI notes do not end locks (turn
  NOTE ENDS off for that). Not included: step pitch, note-off trigs, separate AMP/FILT/LFO trig tracks per step, pattern length,
  swing, slide, the arpeggiator, MULTI TRIG transpose, and KIT ASSIGN other than
  pitch bend (velocity, key position, the joystick's vertical axis).
- **Removing a trig keeps its locks** (the step turns green), where the unit
  clears them. Use CLR LOCKS. A hold on an empty step does not add a trig.
- **SRR just after it is turned down.** The firmware's SRR then reads memory
  belonging to other stages and, on the unit, partly to other tracks. The
  plug-in reads those words as 0. In a stress test with SRR jumping at random,
  0.02–0.1% of samples differed for this reason.
- **Neighbouring tracks' low bits.** AMP reads three low-order words that the
  previous track's delay left in shared memory. Here they come from this
  track's own delay, as in a one-track emulation. The effect is at most one
  LSB of 24.
- **The analog stage** after the DAC is not modelled.

## Accuracy

| What | Checked against | Amount | Result |
|---|---|---|---|
| Whole DSP track, all 8 machines, from random block words | the firmware in a DSP56300 emulator | 102.4 M samples | 0 differ¹ |
| The built plug-in, with random knobs, LFOs, notes, bends, tempo | the same | 30.7 M samples | 0 differ¹ |
| LFOs, knob slew, block words | the main OS's own code in a 68020 emulator | 200,000 frames | 0 differ |
| POLY's voice allocator | the same | 200,000 note events | 0 differ |
| Which locks a step sends, and returns to the kit | the same | 100,000 steps | 0 differ |
| Locks applied to the words and LFOs | the same | 54,050 locks over 100,000 frames | 0 differ |
| Regression hashes (`tests/test_core.c`) | hashes from the firmware | every machine | pass |
| FM+ DYN's 1FRQ and 2FRQ texts | the main OS's display strings | 256 values | 0 differ |

¹ With the SRR scratch words above set to 0 in the firmware too.

**Not verified:**

- It has not been compared with a real Monomachine. It was tried by hand in
  Bitwig on macOS, including the editor's trigs, locks, keyboard and trackpad,
  and in no other host.
- POLY as a whole was not run against the firmware. Its parts were: the
  allocator, the locks, and each voice as a track. The order in which the six
  voices enter the limiter is not traced, and only matters when a chord clips.
- The sequencer's step timing is the host's transport, not the unit's clock.
- The main CPU's and DSP's relative timing (which of a frame's three blocks a
  new word reaches first) follows the emulator's host model. It can shift
  modulation by up to about 1 ms.
- The KIT ASSIGN path for pitch bend was read from the main OS and checked in
  the earlier plug-ins, not re-run against it here.

## Build

```bash
./build_macos.sh
cp -R "build/Mono Track.clap" ~/Library/Audio/Plug-Ins/CLAP/
```

## Tests

```bash
cc -std=c99 -O2 -Wno-unused-function tests/test_core.c -lm -o tests/test_core && tests/test_core
xcrun clang -std=c11 -O2 -Wno-unused-function -Wno-missing-field-initializers -I vendor/clap/include tests/clap_test.c -o tests/clap_test && tests/clap_test "build/Mono Track.clap"
```

```bash
xcrun clang -std=c11 -O1 -fobjc-arc -w -I vendor/clap/include tests/gui_test.m -framework Cocoa -o tests/gui_test && tests/gui_test "build/Mono Track.clap"
```

`test_core` needs no firmware. `clap_test` loads the bundle and checks its
pages, parameters, state (including states saved by 0.1.0 and 0.2.0), each
section's effect on the sound, POLY, the MIDI TRIG switches, trigs and locks. `gui_test` opens the
editor off-screen and drives it with mouse and key events. All three also pass
under AddressSanitizer and UBSan and as an Intel build under Rosetta, and the
bundle passes `clap-trap` at 44.1, 48 and 96 kHz.

## License

GPL-3.0-only. The CLAP headers in `vendor/clap` are under their own MIT
license. Elektron and Monomachine are trademarks of Elektron Music Machines;
this project is not affiliated with them.
