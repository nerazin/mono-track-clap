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

Each is monophonic, as a Monomachine track is.

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
| **TRACK** | LEVEL, LP KEY, HP KEY, BEND, MASTER TUNE |

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

The LFO controls:

- **PAGE**: PTCH, SYN, AMP, FILT, EFX, LFO1, LFO2, LFO3, MIDI. MIDI has no
  effect here, since a plug-in has no MIDI track to send to.
- **DEST**: the knob on that page, shown by name. On PTCH it is the range,
  1/12 to 16OC.
- **TRIG**: FREE, TRIG, HOLD, ONE, HALF. A note is the LFO trig.
- **WAVE**: TRI, ITRI, SAW, ISAW, SQR, ISQR, EXP, IEXP, RAMP, IRMP, RND.
- **MULT**: 1X to 64X. **SPD**, **INTL** and **DPTH**: 0–127.

LFO speed, HOLD, the portamento and the delay time all follow the host's tempo.

The TRACK page:

- **LEVEL** is the track's level.
- **LP KEY** and **HP KEY** are the kit's key-tracking switches (KIT ASSIGN KEY:
  LPF and HPF), both on by default.
- **BEND** is the range of MIDI pitch bend, 1/12 to 16OC, sent as KIT ASSIGN
  JOY R/L would be with PAGE PTCH.
- **MASTER TUNE** is 400.0–499.9 Hz.

## How it behaves

- **Notes.** One voice. The last note held wins, and releasing it returns to
  the previous note still held, with a new trig.
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
- **Not included.** The sequencer, parameter locks, MULTI TRIG transpose, and
  KIT ASSIGN other than pitch bend (velocity, key position, the joystick's
  vertical axis).
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
| The built plug-in, with random knobs, LFOs, notes, bends, tempo | the same | 61.4 M samples | 0 differ¹ |
| LFOs, knob slew, block words | the main OS's own code in a 68020 emulator | 300,000 frames | 0 differ |
| Regression hashes (`tests/test_core.c`) | hashes from the firmware | every machine | pass |

¹ With the SRR scratch words above set to 0 in the firmware too.

**Not verified:**

- It has not been opened in Bitwig or compared with a real Monomachine.
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

`test_core` needs no firmware. `clap_test` loads the bundle and checks its
pages, parameters, state and each section's effect on the sound. Both also pass
under AddressSanitizer and UBSan and as an Intel build under Rosetta, and the
bundle passes `clap-trap` at 44.1, 48 and 96 kHz.

## License

GPL-3.0-only. The CLAP headers in `vendor/clap` are under their own MIT
license. Elektron and Monomachine are trademarks of Elektron Music Machines;
this project is not affiliated with them.
