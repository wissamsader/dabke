/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 STUDIOMORPHIK */
/* DABKE's sound engine: lead voices for Arabic wedding and shaabi music (reeds, keyboard leads, strings, plucked
 * lutes), quarter tones on any note, ornaments, a drone, a bass and backing that follow the left hand, a rhythm box of
 * synthesized darbuka, tabl, riq, daf, sagat, kick and claps, an arpeggio, eight tracks each with a recorder, steps
 * and locks (a knob held at a value of its own on a step), and the effects.
 *
 * Two sides, as COMBO's organ.h. The audio side (dabke_render, dabke_key_audio, dabke_bend_audio) runs in the audio
 * interrupt. Everything else is called from the main loop and only queues a message that the audio side takes at its
 * next block, so the two never touch the same data at once. */
#pragma once
#include <stdint.h>

/* the drums, in the rhythm box's lanes; the rhythm's two more lanes are the bass and the backing */
enum { D_DUM, D_TEK, D_KA, D_SLAP, D_TABL, D_TIK, D_RIQ, D_DAF, D_SAGAT, D_KICK, D_CLAP, D_SNARE, D_SHAKER, DAB_NDRUM };
#define DAB_NLANE (DAB_NDRUM + 2)
#define DAB_LBASS DAB_NDRUM
#define DAB_LCHORD (DAB_NDRUM + 1)
#define DAB_RMAX 32                     /* steps in a rhythm, at most */
#define DAB_NOWN 4                      /* rhythms of your own */
#define DAB_LOOP_N 128                  /* notes in a keys track's loop, at most */
#define DAB_LOOP_BARS 8                 /* its bars, at most */
#define DAB_TAKES 16                    /* the takes a keys track keeps apart (more: the oldest two become one) */
#define DAB_TPB 24                      /* a loop's ticks a beat (a rhythm's step: 6, 8 or 12 of them; the arpeggio's 3 ..) */
#define DAB_STEPS 16                    /* a page of steps (a bar of sixteen), at most */
#define DAB_TSTEPS 32                   /* a track's own steps: LENGTH two bars, at most */
#define DAB_TPAT (DAB_TSTEPS / 4)       /* (kept 2 bits a step: 0 none, 1 soft, 2 a hit, 3 accented) */
#define DAB_NLOCK 240                   /* the locks, all the tracks' together, at most */
#define DAB_NKT 4                       /* the tracks of the keys as they start: the lead, the drone, the left hand, the bass
                                         * line (the first four parts); the other four start as the drums' */
#define DAB_NTRK 8                      /* the tracks, each with a loop (a note sound on it) or steps (a drum sound) */
#define DAB_TLANE 7                     /* a track's steps: its lanes (each the track's sound's drum by its number) */
#define DAB_EV(pos, len, note) ((uint32_t)(pos) | (uint32_t)(len) << 11 | (uint32_t)(note) << 22)
#define DAB_EV_POS(e) ((e) & 2047u)     /* a loop's note: where it begins and how long it is (ticks), its key */
#define DAB_EV_LEN(e) ((e) >> 11 & 2047u)
#define DAB_EV_NOTE(e) ((e) >> 22 & 127u)
#define DAB_SCOPE 256

#define DAB_NDRONE 7                    /* the drone's sounds */

/* the mixer's eight parts. Each has its volume, and its own filter and bass control before the effects */
enum { PART_LEAD, PART_DRONE, PART_LEFT, PART_BASS, PART_KICK, PART_DRUMS, PART_CLAPS, PART_METAL, DAB_NPART };

/* Any sound on any track. A track's sound is one of a family's: the lead's instruments, the drone's sounds, the left
 * hand's instruments, the bass line's sounds, or one of each drum family's four. A note sound makes the track a keys
 * track, a drum sound a drum track. The families in their fixed order (each track starts with its own: track N, family
 * N) */
enum { SF_LEAD, SF_DRONE, SF_LEFT, SF_BASS, SF_KICK, SF_DARB, SF_CLAP, SF_RIQ, SF_N };
#define SF_DRUM(f) ((f) >= SF_KICK)

/* the instruments a sound is made on */
enum { MD_MIJWIZ, MD_ZURNA, MD_LEAD, MD_VIOLIN, MD_NEY, MD_ACCORDION, MD_STRINGS, MD_ORGAN, MD_BUZUQ, MD_OUD, MD_QANUN,
       MD_BRASS, MD_SQUARE, MD_COUNT };
#define MD_PLUCKED(m) ((m) == MD_BUZUQ || (m) == MD_OUD || (m) == MD_QANUN)

/* the knobs and choices, in the engine's own units */
enum {
    P_LEAD, P_LEFT, P_BASS, P_DRUMS, P_DRONE,           /* volumes 0..1 */
    P_TEMPO,                                            /* beats a minute */
    P_TONE, P_BUZZ, P_DETUNE, P_BREATH, P_BODY,         /* 0..1; 0..1; cents 0..40; 0..1; 0..1 */
    P_ATTACK, P_RELEASE, P_GLIDE,                       /* seconds */
    P_VIBRATE, P_VIBDEPTH, P_VIBDELAY,                  /* Hz; 0..1; seconds */
    P_ORNSPEED, P_ORNAMOUNT,                            /* Hz; 0..1 */
    P_ECHO, P_ECHOTIME, P_ECHOFB, P_REVERB,             /* 0..1; seconds 0.06..0.7; 0..0.9; 0..1 */
    P_DRIVE, P_CHORUS, P_PA, P_BRIGHT,                  /* 0..1 */
    P_WOW, P_AGE, P_HISS,                               /* 0..1 */
    P_TRANSPOSE, P_TUNE, P_VELAMT,                      /* semitones; cents; 0..1 */
    P_BASSTONE, P_BASSDECAY,                            /* Hz 60..2500; x 0.3..3 */
    P_SWING, P_ACCENT, P_LOOSE, P_BALANCE,              /* 0..1; 0..1; 0..1; -1..1 */
    P_KITTUNE, P_KITDECAY, P_KITTONE,                   /* x 0.5..2; x 0.4..2.5; 0..1 */
    P_GDRUM, P_GKICK, P_GCLAP, P_GMETAL,                /* the drum parts' volumes 0..1: darbuka and tabl (dum, tek, ka,
                                                         * slap, tabl, stick, daf), the kick, the claps (and the snare),
                                                         * riq and sagat (and the shaker) */
    P_ARPGATE,                                          /* 0..1 */
    P_FILTER, P_FILTER_LAST = P_FILTER + DAB_NPART - 1, /* each part's filter (PART_* order), -1 .. 1: below 0 the highs
                                                         * close, 0 open, above 0 the lows go */
    P_LOWEQ, P_LOWEQ_LAST = P_LOWEQ + DAB_NPART - 1,    /* each part's bass, -1 .. 1: less low end, 0 as it is, more */
    P_SEND, P_SEND_LAST = P_SEND + DAB_NPART - 1,       /* each part into the delay, 0..1 (its DELAY SEND; up to 0.19 one
                                                         * send fed the delay and the reverb: P_RSEND took its value) */
    P_PITCH, P_PITCH_LAST = P_PITCH + DAB_NPART - 1,    /* each part's pitch, semitones -12..12 (the drums' too) */
    P_RSEND, P_RSEND_LAST = P_RSEND + DAB_NPART - 1,    /* each part into the reverb, 0..1 (its REVERB SEND; 0.20) */
    /* choices, as whole numbers */
    P_MODEL, P_PLAY, P_DOUBLE,                          /* MD_*; 0 legato, 1 mono, 2 poly; 0 off, 1 octave down, 2 up */
    P_ORNAMENT, P_TRILLTO, P_PICKING,                   /* ORN_*; 0 the scale's next note, 1 a half step, 2 a quarter tone; 0 single, 1 tremolo */
    P_DRONEMODE, P_DRONESND, P_DRONEOCT,                /* 0 off, 1 tonic, 2 tonic and fifth, 3 the left hand's note; its
                                                         * sound 0..DAB_NDRONE-1: mijwiz, organ, strings, accordion, ney,
                                                         * violin, brass; 0 low, 1 middle */
    P_BENDRANGE, P_PRESS,                               /* semitones; 0 nothing, 1 bend up, 2 bend down, 3 vibrato, 4 tone */
    P_LEFTMODEL, P_BASSMODEL, P_CHORDTYPE,              /* MD_*; DAB_BASSV: 0 round, 1 synth, 2 buzz, 3 woody, 4 picked;
                                                         * 0 fifths, 1 the maqam's notes */
    P_DRUMV, P_DRUMV_LAST = P_DRUMV + 3,                /* each drum track's sound, 0..DAB_DRUMV-1 (0: its first) */
    P_VARIATION, P_ADDCLAP, P_ADDRIQ, P_ADDSAGAT, P_ADDKICK,   /* 0 plain, 1 busy, 2 heavy, 3 by itself; 0 / 1 each */
    P_AUTOFILL, P_FILLTYPE,                             /* 0, 2, 4, 8, 16 bars; 0 roll, 1 break, 2 stops */
    P_ARPON, P_ARPMODE, P_ARPRATE, P_ARPOCT,            /* 0 / 1; ARP_* (its order); notes a beat (1, 2, 3, 4, 6, 8);
                                                         * over ARPO_* (1 octave, 2..4 up, 2..3 down, both ways) */
    P_ARPPAT, P_ARPREP, P_ARPSWING, P_ARPLATCH,         /* its own rhythm (DAB_ARP_PATTERNS); each note 1..4 times; its
                                                         * swing: below 0 the rhythm's, else 0..1; 1: it keeps going after
                                                         * the keys are let go */
    P_DELAYSYNC,                                        /* 0 its own time, 1 an eighth, 2 a dotted eighth, 3 a beat */
    P_ROOT, P_SCALE,                                    /* the maqam's tonic, C = 0; its keys, a bit each (C = bit 0) */
    P_BEND, P_PRESSURE, P_MOD,                          /* -1..1 (x bend range); 0..1 the keys' pressure; 0..1 mod wheel */
    P_COUNT
};
enum { ORN_OFF, ORN_MORDENT, ORN_TURN, ORN_SCOOP, ORN_TRILL, ORN_SHAKE, ORN_AUTO, ORN_COUNT };
/* A lock: a step of a track holds a knob at a value of its own, from that step until the track's next step with a hit
 * or a note (or another lock of the same knob); then the knob is back at its normal value. The knobs a lock can hold
 * are the first DAB_NLK (up to the tracks' reverb sends). One word each: the step (0..127), the track, the knob, its value
 * in the knob's own steps (-64..447), the take it was recorded in (0: made by hand) */
#define DAB_NLK (P_RSEND_LAST + 1)
#define DAB_LK(trk, step, id, val, take) (0x80000000u | (uint32_t)((take) & 31) << 26 | (uint32_t)(((val) + 64) & 511) << 17 | \
                                          (uint32_t)((id) & 127) << 10 | (uint32_t)((trk) & 7) << 7 | (uint32_t)((step) & 127))
#define DAB_LK_STEP(l) ((int)((l) & 127u))
#define DAB_LK_TRK(l) ((int)((l) >> 7 & 7u))
#define DAB_LK_ID(l) ((int)((l) >> 10 & 127u))
#define DAB_LK_VAL(l) ((int)((l) >> 17 & 511u) - 64)
#define DAB_LK_TAKE(l) ((int)((l) >> 26 & 31u))
#define DAB_LK_SOUND 127                /* a lock's knob number that is no knob: the step's sound. Its value: (the drum
                                         * family - SF_KICK) * 16 + the sound's number in it. The hits of a drum sound's
                                         * track on that step play that drum sound instead of the track's own */
/* Each track's own lead sound (dabke_tpar): a lead sound on a track plays with that track's own instrument knobs,
 * vibrato, ornaments, glide, TRANSPOSE, TUNE, PLAY, DOUBLE, PICKING, TRILL TO and BEND RANGE. The whole instrument's
 * knobs of those names (par) are track 1's, and the sounds that have none of their own follow them: the drone, the left
 * hand, the bass line. A lock of one of a sound's knobs holds its own track's (on a track without a lead sound: track
 * 1's, as the pages show there). DAB_SK: a knob's number among a sound's knobs (-1: not one); DAB_LKK: knob id of
 * track t's sound as dabke_status's lk_at counts the knobs (the whole instrument's, then each track's own) */
#define DAB_NSK 15
#define DAB_SK(id) ((id) >= P_TONE && (id) <= P_ORNAMOUNT ? (id) - P_TONE : (id) == P_TRANSPOSE ? 13 : (id) == P_TUNE ? 14 : -1)
#define DAB_NLKS (DAB_NLK + DAB_NTRK * DAB_NSK)
#define DAB_LKK(t, id) (DAB_SK(id) < 0 ? (id) : DAB_NLK + ((t) & 7) * DAB_NSK + DAB_SK(id))
#define DAB_PLAIN P_COUNT               /* dabke_tpar's: 1 the instrument's own tone (as it plays alone); 0 its knobs' */
/* the arpeggio: its orders, how far it goes, its patterns ('X' an accent, 'x' a note, '.' a rest, a step each) */
enum { ARP_UP, ARP_DOWN, ARP_UPDOWN, ARP_DOWNUP, ARP_PLAYED, ARP_ANY, ARP_CHORD, ARP_INOUT, ARP_THUMB, ARP_NORDER };
enum { ARPO_1, ARPO_2UP, ARPO_3UP, ARPO_4UP, ARPO_2DOWN, ARPO_3DOWN, ARPO_BOTH, ARPO_N };
#define DAB_ARP_NPAT 8
#define DAB_ARP_PATTERNS {"xxxxxxxx", "X..X..X.", "Xx.xX.x.", "Xx.XX.x.", "X.xxX.xx", ".x.x.x.x", "xx.xx.x.", "XxxXxxXx"}

void dabke_init(void);                                  /* once, before any sound */

/* ---- the audio side ---- */
void dabke_render(int32_t *lr, uint32_t n, float gain); /* n stereo frames; a full-scale sample is 1.0 x gain */
void dabke_key_audio(int manual, int note, int vel);    /* a key, from inside the audio interrupt (MIDI in); vel 0 = off */
void dabke_bend_audio(float bend);                      /* a pitch wheel, -1..1, from inside the audio interrupt */
void dabke_mod_audio(float mod);                        /* a mod wheel, 0..1, from inside the audio interrupt */
void dabke_overload(void);                              /* the render took too long: play fewer notes at once (any side) */
void dabke_load(int pct);                               /* the audio load now, % of a render's time (main loop) */

/* ---- the main loop's side: each call is queued for the audio side ---- */
void dabke_key(int manual, int note, int vel);          /* manual 0..7: that track's keys (track 2, the drone's: its
                                                         * note); 8: the left keys as they are set (track 3 or the
                                                         * bass and backing); + DAB_KEY_TRY: heard, not recorded (a
                                                         * note given to a step); vel 0 = off */
#define DAB_KEY_TRY 16
void dabke_track_sound(int track, int family, int index);   /* a track's sound: SF_*, its number in that family (+
                                                             * DAB_AGAIN: the same again, another sound of its own:
                                                             * what it played stops as for any other) */
#define DAB_AGAIN 128
int dabke_family_size(int family);                      /* the sounds a family has */
int dabke_sound_drums(int family);                      /* a drum family's drums (a track's lanes play them in turn) */
int dabke_sound_drum(int family, int lane);             /* the drum (D_*) a lane of that family's plays */
void dabke_par(int id, float v);
void dabke_tpar(int track, int id, float v);            /* a knob or choice of track's own lead sound (above), DAB_PLAIN */
void dabke_press_debug(int32_t o[4]);                   /* (the readout: the pressure 0..127, its bend in cents, its tracks, PRESSURE) */
void dabke_pressure(int tracks, float v);                /* the keys' pressure 0..1 and the tracks leant on (a bit each):
                                                         * it bends, shakes or brightens those tracks' key notes */
void dabke_ntune(int pc, int cents);                    /* one note's tuning, C = 0: -50 is a quarter tone flat */
void dabke_rhythms(const uint8_t *list, int n);         /* the rhythms playing together, 1..4, the first leads */
void dabke_run(int on);
void dabke_sync(int on);                                /* the first left-hand key starts the rhythm */
void dabke_acc(int on);                                 /* the left hand plays bass, backing and drone */
void dabke_style(int backing, int bass);                /* 0 the rhythm's, 1 held / root, 2 arpeggio / octaves, 3 none / walking, 4 - / none */
void dabke_fill(int type);                              /* -1 the chosen fill, else FILL type now */
void dabke_fade(int dir);                               /* +1 start and fade in, -1 fade out to a stop */
void dabke_drum(int k, float level, float tune, float length);   /* one drum: 0..1, x 0.25..4, x 0.2..5 */
void dabke_own(int slot, const uint8_t *grid, int len, int div, int bpm);   /* a rhythm of your own: DAB_NDRUM x DAB_RMAX
                                                                             * hits 0..3 (lane by lane) */
/* The tracks are the mixer's parts (PART_*). Each keeps what it plays, by the kind of its sound. A note sound: a loop,
 * its notes on ticks (DAB_TPB a beat), played in time while the rhythm runs; each recording in which something was
 * played is a take; the drone's loop moves the drone's note. A drum sound: steps, its drums' hits lane by lane, on the
 * rhythm's own steps. Until its player adds, takes away or records something, a track follows the rhythm: tracks 5 to
 * 8 the rhythm's own steps, track 4 the automatic bass line, track 3 the backing. From then on it is his: the rhythm,
 * its variations and its fills leave it alone, and the automatic part of that track is silent.
 * A track is one bar long or two (LENGTH; a bar: sixteen steps, or as many whole bars of the rhythm as fit in
 * sixteen: twelve in 6/8, fourteen in 7/8, ten in 10/8); a loop played live is as long as it was played (up to
 * DAB_LOOP_BARS of the rhythm's bars) until LENGTH is turned.
 * dabke_track_rec(t): track t records (one at a time); the rhythm stopped, the first key (a drum: its first hit)
 * starts it; a track with a loop records over it. Again, or -1: it stops at once; what was played stays, as many bars
 * as were begun (the rest of the last one empty), and plays in time from the next bar. A drum sound: its hits are kept
 * on the steps they fall on.
 * Keys reach a recording track whatever their manual. */
#define DAB_BASSV 5                     /* the bass line's sounds */
#define DAB_DRUMV 4                     /* each drum track's sounds */
void dabke_track_rec(int track);
void dabke_take_undo(int track);        /* the last take gone (recording now: that one first); the last: empty. A drum
                                         * sound: its last live take and what was changed since; none: its own steps go */
void dabke_clear_all(void);             /* every recording gone, and every lock */
void dabke_loop_set(int track, const uint32_t *ev, int n, int len, int bars, const uint8_t *takes, int ntakes, int played);
                                        /* a kept loop: its notes (DAB_EV), its length in ticks, where each take begins,
                                         * its length as it was played (0: made by steps) */
enum { DAB_STEP_FLIP, DAB_STEP_ONE, DAB_STEP_NONE };
void dabke_step(int track, int step, int lane, int how);   /* a drum sound's step. FLIP: its hits there go; none
                                         * there, its lane's drum is added. ONE: that lane's drum alone there. NONE: none */
void dabke_note_add(int track, int pos, int len, int note, int newlen, int alone);   /* a note sound's step: a note
                                         * from tick pos, len ticks long; newlen: the loop's length (ticks) if the track
                                         * has none; alone: the notes that began in those ticks go first */
void dabke_note_del(int track, int from, int to);   /* its notes that begin at ticks from .. to - 1 go */
void dabke_track_len(int track, int two, int ticks);   /* LENGTH: two bars (1) or one; a loop: its length now, ticks (0:
                                         * as it is). Longer: what it played is copied into the new part if that is
                                         * empty; shorter: the rest is kept, silent */
void dabke_track_hit(int track, int lane, int record);   /* a drum track's drum (its sound's, by lane) now; record: on
                                         * its nearest step too (the rhythm stopped: it starts, the hit its first step) */
void dabke_track_hit_audio(int track, int lane, int record);   /* the same, from inside the audio interrupt (MIDI in) */
void dabke_steps_set(const uint8_t *lo, const uint8_t *hi, int edited, const uint8_t *steps, int two);
                                        /* kept steps: the tracks' steps 1 to 16 and 17 to 32 (each DAB_NTRK x DAB_TLANE x
                                         * 4 bytes, 2 bits a step; hi 0: none), his tracks a bit each, each one's length
                                         * in steps (0: a bar, as kept before tracks had one), LENGTH two bars a bit each */
void dabke_lock(int place, uint32_t lock, float unit);   /* a lock (DAB_LK) in its place, 0..DAB_NLOCK-1; 0: none there
                                         * now. unit: one step of its knob, in the engine's units */
void dabke_lock_clear(int track, int take);   /* a track's locks gone (-1: every track's); take > 0: only those
                                         * recorded in that take or a later one */
void dabke_mute(int mask);              /* the tracks muted, a bit each (the panel's MUTE, the background's and the SOLOs
                                         * together: a track silent from its next note's first sample) */
/* ---- the one logic (fm1-organ/LOGIC.md version 2, as RAGA MEOW): freezing, the loops' own stop and clear, the MIXER's
 * effects out, the MASTER EQ, the MIDImix's master ---- */
void dabke_freeze(int track, int on);   /* REC: the track frozen: the notes of its keys down now sound on without the
                                         * keys (on 0: its held notes let go). On a frozen track a key of a one-note
                                         * sound moves the held note there (no new attack, held still); a POLY sound's,
                                         * the left hand's and the bass line's keys are held too; the key of a held note
                                         * again lets that note go (the last one: the track no longer frozen) */
void dabke_loop_off(int track, int off);   /* SEQ: the track's loop (a drum sound: its own steps) stopped, kept (off 1);
                                         * playing again (0), from the next bar */
void dabke_track_clear(int track);      /* SEQ held: the track's loop gone, every take (a drum sound: its own steps, the
                                         * rhythm's again); a recording on it ends with nothing kept */
void dabke_fx_off(int mask);            /* the tracks whose effects are out (the MIXER's FX), a bit each: their delay and
                                         * reverb sends and their BASS, each kept as set */
void dabke_master_eq(float low, float mid, float high);   /* the MASTER EQ, dB each, -24 (that band out, about 40 dB
                                         * down) .. +12: a shelf under 250 Hz, a wide bell at 1 kHz, a shelf over 3.5 kHz,
                                         * the last on the whole sound, after every effect; out when all three are 0 */
void dabke_master(float level);         /* the MIDImix's master fader: the whole level, 0..1, gliding */
#ifdef PLAT_HOST
int dabke_host_voices(int track);
int dabke_host_cpu(int scale_pct);      /* (the tests: the render's measured time as if this % of it; the cap now) */
int dabke_host_drone(void);
int dabke_host_drone_note(void);
int dabke_host_par(int id);
int dabke_host_tpar(int track, int id);
uint32_t dabke_host_lock(int i);
int dabke_host_lock_sums(void);
int dabke_host_dsound(int drum);
int dabke_host_orn(void);
int dabke_host_vfam(int track);
int dabke_host_maqam(int what);       /* (the maqamat: 0 lines it cannot read, 1 those not as 0.17 had them, 2 how many) */
int dabke_host_maqam_try(const char *notes, int *root, int *scale, int *flat);
#endif
void dabke_hold(int press);             /* HOLD pressed (1): the notes of the keys down now sound on after they are let
                                         * go (the lead's, the left hand's, the bass line's), instead of those held
                                         * before; no key down: the held notes let go. 0: they are let go */
void dabke_hold_off(int manual, int note);   /* a held note's own key pressed again: that note let go (as its key
                                         * would), the other held notes still held */
void dabke_hold_move(int manual, int from, int to, int last);   /* a held note moved to another note (the octave
                                         * changed under its key): it sounds there, held still, with no new attack
                                         * (last: the last of a set, hold_ver counts it when they are all done) */
int dabke_held(int manual, int note);   /* HOLD keeps that note of that manual sounding now */
void dabke_panic(void);                                 /* every note off, now */
int dabke_idle(void);                                   /* 1: the audio side has taken everything asked of it (what it
                                                         * shows is up to date with the main loop's calls) */

/* ---- what the engine shows ---- */
typedef struct {
    uint8_t running;
    uint8_t beat, beats;                /* the beat of the bar now, and how many it has */
    uint8_t step, steps;                /* the leading rhythm's step now, and its length */
    uint8_t in_fill;
    uint32_t beat_count, bar;           /* beats since the rhythm started (a change is a new beat), bars */
    int8_t left_root;                   /* the left hand's note, 0..11; -1 none */
    int8_t rec_track;                   /* the track recording, -1 none */
    uint8_t rec_waiting, overdub;       /* (for its first key: the rhythm stopped); recording over its loop */
    uint8_t trk_bars[DAB_NTRK];         /* each track's loop: its bars (recording a first take: so far) */
    uint16_t trk_len[DAB_NTRK], trk_n[DAB_NTRK];   /* its length in ticks (0: none), its notes */
    uint8_t trk_takes[DAB_NTRK], trk_take[DAB_NTRK][DAB_TAKES];   /* its takes, where each begins in its notes */
    const uint32_t *trk_ev[DAB_NTRK];   /* its notes (the engine's own: read them while that track does not record) */
    uint32_t loop_ver;                  /* a change of a loop's notes or length (the app keeps them) */
    uint32_t loop_plays;                /* notes the loops have played */
    uint32_t loop_last;                 /* the last of them: its track << 24 | its tick << 8 | its key */
    uint16_t trk_played[DAB_NTRK];      /* its length as it was played, ticks (0: made by steps) */
    uint8_t dedit;                      /* the tracks with steps of their own (his), a bit each */
    uint8_t tpat[DAB_NTRK][DAB_TLANE][DAB_TPAT];   /* their steps by lane, 2 bits each: 0 none, 1 soft, 2 a hit, 3 accented */
    uint8_t tn[DAB_NTRK];               /* each track's steps on the rhythm's clock: his own, or a bar's or two (LENGTH) */
    uint8_t two;                        /* LENGTH two bars, a bit a track */
    uint8_t page;                       /* a bar in steps, as LENGTH counts it: the rhythm's whole bars in sixteen */
    uint8_t tstep[DAB_NTRK];            /* the step each track is on now (its loop's, or the rhythm's clock's) */
    uint8_t drum_take;                  /* 1 + the drum track whose last recording kept a take (0: none) */
    uint8_t dtakes[DAB_NTRK];           /* a drum sound's takes that can be taken back, each track's */
    uint8_t hand;                       /* the tracks whose last take he wrote by hand (not a recording), a bit each */
    uint8_t lk_at[DAB_NLKS];            /* each knob (DAB_LKK): the lock that holds it now (its place + 1; 0: none, its
                                         * normal value) */
    uint32_t full;                      /* notes that found no room */
    uint8_t tfam[DAB_NTRK], tidx[DAB_NTRK];   /* each track's sound */
    uint8_t voice_cap, voices;          /* the notes that may sound at once now (the load's guard), sounding now */
    uint32_t shed;                      /* notes the guard has let go early */
    uint32_t steps_ver;                 /* a change of them */
    uint8_t hold, held_n;               /* HOLD keeps notes sounding; how many */
    uint32_t hold_ver;                  /* a HOLD press done */
    uint32_t hold_letgo;                /* held notes let go by a note of theirs again from MIDI (not the panel's keys) */
    uint8_t arp_n, arp_step;            /* the arpeggio: the notes in its set; its pattern's step now */
    uint8_t arp_last;                   /* the last note it played */
    uint32_t arp_plays;                 /* notes it has played */
    uint16_t arp_count;                 /* notes it has played since this set began */
    uint8_t arp_first[16];              /* and the first sixteen of them */
    float level, lead;                  /* how loud the output and the lead are now, 0..1 */
    float bend;                         /* the lead's bend now, semitones */
    uint32_t notes;                     /* keys pressed since the start: a change is a new note */
    uint32_t hits[DAB_NDRUM];           /* each drum's hits since the start */
    uint8_t lead_on;                    /* a lead note sounds */
    uint32_t starts[DAB_NPART];         /* each track's notes and hits since the start (the drone's: none) */
    uint32_t late;                      /* renders that ran past the guard's budget (its model too low) */
    uint8_t frozen;                     /* the tracks REC holds (frozen), a bit each */
    uint8_t holds;                      /* the tracks with a note held now, a bit each */
    uint8_t fz_n[DAB_NTRK], fz_note[DAB_NTRK][4];   /* each track's held notes (four at most; the left keys' with track
                                         * 1's), for PLAY to bring back */
    uint8_t loop_off;                   /* the tracks whose loop (or own steps) SEQ stopped, a bit each */
    float tlevel[DAB_NTRK];             /* how loud each track is now (after its level), 0..1: the MIXER's live bars */
} dabke_status_t;
const dabke_status_t *dabke_status(void);
const int16_t *dabke_scope(void);                       /* the last DAB_SCOPE samples of the output, +-32767 full scale */

/* the rhythms (built in; then the DAB_NOWN of your own) */
int dabke_rhythm_count(void);                           /* the built-in ones */
const char *dabke_rhythm_name(int i);
const char *dabke_rhythm_group(int i);
int dabke_rhythm_bpm(int i);
int dabke_rhythm_grid(int i, uint8_t grid[DAB_NLANE][DAB_RMAX], int *len, int *div);   /* a built-in one, step by step */

/* the maqamat: their names, tonic, and the cents each key is moved by (0 or -depth) with a tonic on root */
int dabke_maqam_count(void);
const char *dabke_maqam_name(int m);
int dabke_maqam_root(int m);                            /* its usual tonic */
int dabke_maqam_detune(int m, int root, int pc, int depth);   /* the cents key pc is moved by */
uint32_t dabke_maqam_scale(int m, int root);            /* its keys, a bit each */
int dabke_maqam_tonic_flat(int m);                      /* 1: its tonic itself is a quarter tone flat (Sikah, Huzam, Iraq) */
