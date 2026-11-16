/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 STUDIOMORPHIK
 * Copyright (C) 2026 Charles Vestal (the parts from fm1-base's template) */
/* DABKE's UI: the Arabic wedding keyboard behind the FM-1's panel. COMBO's frame (its pages, knob rows, saving, the USB
 * port's turns, the MIDImix ring and lights), with DABKE's own pages: the maqam, the sound, the ornaments, the rhythm,
 * and the singer over the eight tracks' lanes on the home page (scene.c, panel.c). The sound is dabke.h's engine: every
 * change the UI makes goes through its calls. Part of the unity build (device/unity.c, host/plat_host.c), after the
 * platform: gfx.c, announce.c and lib/touch.c are in scope.
 *
 * One logic, the same as COMBO's (fm1-organ/LOGIC.md). Eight tracks, each a sound, what it plays and its mix. One
 * picked track (or ALL): what the keys play, what SELECT changes, what REC records, what the sequencer shows and what a
 * held step locks. PLAY alone starts and stops everything. REC records the picked track live. SEQ is the sequencer:
 * step mode, the white keys the steps of the picked track. A step held while a knob turns: that knob locked on that
 * step. What he recorded or wrote is his until he clears it; the rhythm drives only the tracks he has not touched.
 * The main loop decides and queues; the audio side (dabke.c) plays the steps and holds the locks in time. */
#include "fastmath.h"
#include "dabke.h"

/* ------------------------------------------------------------- the MIDImix --- */
/* Akai MIDImix on channel 1: per strip i three knobs (top, middle, bottom) as CC MIX_BASE[i] + 0, 1, 2 and a fader
 * as + 3; the master fader CC 62; MUTE buttons notes 1 + 3 i, REC ARM notes 3 + 3 i, BANK LEFT 25, BANK RIGHT 26,
 * SOLO 27. Its LEDs: note on (127) / on at velocity 0 on channel 1 for the same notes. The eight strips are the eight
 * tracks (dabke.h PART_*), each its fader (volume), top knob (its effect: into the delay and the reverb), middle knob
 * (filter), bottom knob (pitch), MUTE, and REC ARM (REC for that track, which it picks). A fader or a knob never picks
 * its track: it moves its own strip's track whatever the keys play, and what moved is said. The master fader is the
 * scene fader; SOLO is HOLD; BANK the rhythms. */
static const uint8_t MIX_BASE[8] = {16, 20, 24, 28, 46, 50, 54, 58};
#define MIX_MASTER_CC 62
static const uint8_t PART_PAR[DAB_NPART] = {P_LEAD, P_DRONE, P_LEFT, P_BASS, P_GKICK, P_GDRUM, P_GCLAP, P_GMETAL};
static const char *const PART_NAME[DAB_NPART] = {"LEAD", "DRONE", "LEFT HAND", "BASS LINE", "KICK", "DARBUKA, TABL",
                                                 "CLAPS", "RIQ, SAGAT"};
#define MIX_FADER PART_PAR

#define MIX_MUTE_NOTE(i) (1u + 3u * (i))
#define MIX_RECARM_NOTE(i) (3u + 3u * (i))
#define MIX_BANK_LEFT 25u                             /* previous rhythm */
#define MIX_BANK_RIGHT 26u                            /* next rhythm */
#define MIX_SOLO 27u                                  /* HOLD */

/* ---------------------------------------------------------------- colours --- */
#define K_BLACK 0x0000u
#define K_WH RGB(244, 238, 226)
#define K_AMB RGB(255, 186, 64)
#define K_GR RGB(150, 140, 128)
#define K_DIM RGB(48, 40, 38)
#define K_BOX RGB(32, 26, 26)
#define K_KBG RGB(22, 18, 18)
#define K_RED RGB(232, 64, 52)
#define K_GREEN RGB(80, 190, 120)
#define K_LK RGB(96, 196, 255)           /* a lock: its mark on a step and on a knob, its value while it holds (as COMBO) */

/* ------------------------------------------------------------------ knobs --- */
/* kept as whole steps (value = steps x step); step in hundredths of the unit; the engine gets value x 0.01 (scale 0),
 * x 1 (1) or x 0.001 (2) */
enum { U_NONE, U_HZ, U_S, U_PCT, U_MS, U_ST, U_CT, U_QT };   /* (U_QT: a track's pitch, in quarter tones) */
static const char *const UNIT[] = {"", " HZ", " S", " %", " MS", " ST", " CT", ""};
typedef struct {
    const char *word;                    /* the knob page's name */
    int16_t lo, hi, start;               /* in steps */
    uint16_t step;                       /* hundredths of the unit, a step (0: not a knob) */
    uint8_t scale, unit, keep;           /* keep: a sound does not change it */
} par_t;
static const par_t PAR[P_COUNT] = {
    [P_LEAD] = {"LEAD", 0, 100, 80, 100, 0, U_NONE, 1},
    [P_LEFT] = {"LEFT", 0, 100, 55, 100, 0, U_NONE, 1},
    [P_BASS] = {"BASS LINE", 0, 100, 75, 100, 0, U_NONE, 1},
    [P_DRUMS] = {"DRUMS", 0, 100, 80, 100, 0, U_NONE, 1},
    [P_DRONE] = {"DRONE VOL", 0, 100, 35, 100, 0, U_NONE, 0},
    [P_TEMPO] = {"TEMPO", 40, 240, 140, 100, 1, U_NONE, 1},
    [P_TONE] = {"TONE", 0, 100, 70, 100, 0, U_NONE, 0},
    [P_BUZZ] = {"BUZZ", 0, 100, 45, 100, 0, U_NONE, 0},
    [P_DETUNE] = {"PIPES APART", 0, 40, 14, 100, 1, U_CT, 0},
    [P_BREATH] = {"BREATH", 0, 100, 15, 100, 0, U_NONE, 0},
    [P_BODY] = {"BODY", 0, 100, 55, 100, 0, U_NONE, 0},
    [P_ATTACK] = {"ATTACK", 1, 400, 2, 500, 2, U_MS, 0},
    [P_RELEASE] = {"RELEASE", 1, 400, 5, 1000, 2, U_MS, 0},
    [P_GLIDE] = {"GLIDE", 0, 100, 6, 500, 2, U_MS, 0},
    [P_VIBRATE] = {"VIB SPEED", 20, 90, 55, 10, 1, U_HZ, 0},
    [P_VIBDEPTH] = {"VIB DEPTH", 0, 100, 20, 100, 0, U_NONE, 0},
    [P_VIBDELAY] = {"VIB DELAY", 0, 150, 25, 1000, 2, U_MS, 0},
    [P_ORNSPEED] = {"ORN SPEED", 40, 240, 120, 10, 1, U_HZ, 0},
    [P_ORNAMOUNT] = {"HOW OFTEN", 0, 100, 40, 100, 0, U_PCT, 0},
    [P_ECHO] = {"DELAY", 0, 100, 0, 100, 0, U_NONE, 0},
    [P_ECHOTIME] = {"DELAY TIME", 6, 70, 30, 1000, 2, U_MS, 0},
    [P_ECHOFB] = {"REPEATS", 0, 90, 45, 100, 0, U_NONE, 0},
    [P_REVERB] = {"REVERB", 0, 100, 25, 100, 0, U_NONE, 0},
    [P_DRIVE] = {"DRIVE", 0, 100, 0, 100, 0, U_NONE, 0},
    [P_CHORUS] = {"CHORUS", 0, 100, 0, 100, 0, U_NONE, 0},
    [P_PA] = {"WEDDING PA", 0, 100, 0, 100, 0, U_NONE, 0},
    [P_BRIGHT] = {"BRIGHTNESS", 0, 100, 100, 100, 0, U_NONE, 0},
    [P_WOW] = {"WOBBLE", 0, 100, 0, 100, 0, U_NONE, 0},
    [P_AGE] = {"TAPE AGE", 0, 100, 0, 100, 0, U_NONE, 0},
    [P_HISS] = {"HISS", 0, 100, 0, 100, 0, U_NONE, 0},
    [P_TRANSPOSE] = {"TRANSPOSE", -12, 12, 0, 100, 1, U_ST, 0},
    [P_TUNE] = {"TUNE", -50, 50, 0, 100, 1, U_CT, 0},
    [P_VELAMT] = {"VEL AMOUNT", 0, 100, 50, 100, 0, U_NONE, 1},
    [P_BASSTONE] = {"BASS TONE", 6, 250, 60, 1000, 1, U_HZ, 1},
    [P_BASSDECAY] = {"BASS DECAY", 6, 60, 20, 500, 0, U_PCT, 1},
    [P_SWING] = {"SWING", 0, 100, 0, 100, 0, U_NONE, 1},
    [P_ACCENT] = {"ACCENT", 0, 100, 50, 100, 0, U_NONE, 1},
    [P_LOOSE] = {"LOOSENESS", 0, 100, 20, 100, 0, U_NONE, 1},
    [P_BALANCE] = {"LOW / HIGH", -20, 20, 0, 500, 0, U_NONE, 1},
    [P_KITTUNE] = {"KIT TUNE", 50, 200, 100, 100, 0, U_PCT, 1},
    [P_KITDECAY] = {"KIT LENGTH", 8, 50, 20, 500, 0, U_PCT, 1},
    [P_KITTONE] = {"KIT TONE", 0, 100, 100, 100, 0, U_NONE, 1},
    [P_GDRUM] = {"DARBUKA", 0, 100, 100, 100, 0, U_NONE, 1},   /* (with the tabl, the daf, the stick: the part's name) */
    [P_GKICK] = {"KICK", 0, 100, 100, 100, 0, U_NONE, 1},
    [P_GCLAP] = {"CLAPS", 0, 100, 100, 100, 0, U_NONE, 1},
    [P_GMETAL] = {"RIQ, SAGAT", 0, 100, 100, 100, 0, U_NONE, 1},
    [P_ARPGATE] = {"ARP LENGTH", 2, 20, 12, 500, 0, U_PCT, 1},
#define MIXK(k) [P_FILTER + (k)] = {"FILTER", -50, 50, 0, 200, 0, U_NONE, 1}, [P_LOWEQ + (k)] = {"BASS", -50, 50, 0, 200, 0, U_NONE, 1}
    MIXK(0), MIXK(1), MIXK(2), MIXK(3), MIXK(4), MIXK(5), MIXK(6), MIXK(7),   /* the mixer: each part's filter, bass */
#define TRKK(k, s0) [P_SEND + (k)] = {"EFFECT", 0, 100, s0, 100, 0, U_NONE, 1}, [P_PITCH + (k)] = {"PITCH", -48, 48, 0, 25, 1, U_QT, 1}
    TRKK(0, 100), TRKK(1, 100), TRKK(2, 100), TRKK(3, 0), TRKK(4, 30), TRKK(5, 30), TRKK(6, 30), TRKK(7, 30),   /* each
                                         * track into the delay and the reverb, and its pitch */
};
#define IS_KNOB(id) ((id) < P_COUNT && PAR[id].step)

/* ---------------------------------------------------------------- choices --- */
/* each kept in set.ch[]; the first NSCH are part of a sound */
enum { C_MODEL, C_PLAY, C_DOUBLE, C_ORN, C_TRILLTO, C_PICK, C_DRONE, C_DRONESND, C_DRONEOCT, C_BEND, C_DSYNC,
       C_PRESS, C_TOUCH, C_LEFTMODEL, C_BASSMODEL, C_CHORD, C_VAR, C_ADDCLAP, C_ADDRIQ, C_ADDSAGAT, C_ADDKICK,
       C_AUTOFILL, C_FILL, C_SYNC, C_LEFT, C_BACKING, C_BASSPLAY, C_ARPMODE, C_ARPRATE, C_ARPOCT, C_SCENE, C_USB,
       C_MAQAM, C_ROOT, C_PANEL, C_ARPPAT, C_ARPREP, C_ARPSWING, C_ARPLATCH, C_KICKV, C_DARBV, C_CLAPV, C_RIQV, C_HOLD,
       C_PSTEPS, NCH };
#define NCH6 C_PSTEPS                    /* (the choices in the settings before PITCH STEPS) */
#define NSCH (C_DSYNC + 1)
#define NOENG 255
static const char *const MODEL_NAME[MD_COUNT] = {"MIJWIZ", "ZURNA", "KEY LEAD", "VIOLIN", "NEY", "ACCORDION", "STRINGS",
                                                 "ORGAN", "BUZUQ", "OUD", "QANUN", "BRASS", "SQUARE"};
static const char *const NOTE_NAME[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
static const char *const OFF_ON[2] = {"OFF", "ON"};
static const char *const N_PLAY[3] = {"LEGATO", "MONO", "POLY"};
static const char *const N_DOUBLE[3] = {"OFF", "OCT DOWN", "OCT UP"};
static const char *const N_ORN[ORN_COUNT] = {"OFF", "MORDENT", "TURN", "SCOOP", "TRILL", "SHAKE", "AUTO"};
static const char *const N_TRILL[3] = {"NEXT NOTE", "1/2 TONE", "1/4 TONE"};
static const char *const N_PICK[2] = {"SINGLE", "TREMOLO"};
static const char *const N_DRONE[4] = {"OFF", "TONIC", "TONIC + 5TH", "LEFT KEY"};
static const char *const N_DSND[DAB_NDRONE] = {"MIJWIZ", "ORGAN", "STRINGS", "ACCORDION", "NEY", "VIOLIN", "BRASS"};
static const uint8_t DSND_MODEL[DAB_NDRONE] = {MD_MIJWIZ, MD_ORGAN, MD_STRINGS, MD_ACCORDION, MD_NEY, MD_VIOLIN, MD_BRASS};
static const char *const N_DOCT[2] = {"LOW", "MIDDLE"};
static const char *const N_BEND[4] = {"1/4 TONE", "1/2 TONE", "1 TONE", "1 1/2 TONE"};
static const char *const N_DSYNC[4] = {"OWN TIME", "1/8", "3/16", "1/4"};
static const char *const N_PRESS[5] = {"NOTHING", "BEND UP", "BEND DOWN", "VIBRATO", "BRIGHTER"};
static const char *const N_TOUCH[2] = {"PLAIN", "VELOCITY"};   /* (how hard a note lands, only: what a lean does is
                                         * PRESSURE's, whatever this says. Up to 0.14 a third choice, VEL+PRESS, had to
                                         * be on for PRESSURE to do anything: a record that kept it reads VELOCITY) */
static const char *const N_BASSM[DAB_BASSV] = {"ROUND", "SYNTH", "BUZZ", "WOODY", "PICKED"};
static const char *const N_KICKV[DAB_DRUMV] = {"MACHINE", "BOOM", "TIGHT", "THUMP"};   /* each drum track's sounds */
static const char *const N_DARBV[DAB_DRUMV] = {"CLAY", "DEEP", "BRIGHT", "METAL"};
static const char *const N_CLAPV[DAB_DRUMV] = {"HANDS", "CROWD", "SNAPS", "ROOM"};
static const char *const N_RIQV[DAB_DRUMV] = {"BRASS", "BRIGHT", "DARK", "LONG"};
static const char *const N_PSTEPS[2] = {"FULL", "MICRO"};
static const char *const N_CHORD[2] = {"FIFTHS", "MAQAM"};
static const char *const N_VAR[4] = {"PLAIN", "BUSY", "HEAVY", "BY ITSELF"};
static const char *const N_AUTOFILL[5] = {"NEVER", "2 BARS", "4 BARS", "8 BARS", "16 BARS"};
static const char *const N_FILL[3] = {"ROLL", "BREAK", "STOPS"};
static const char *const N_LEFT[3] = {"SAME", "SPLIT", "BASS"};
static const char *const N_BACKING[4] = {"STABS", "HELD", "ARPEGGIO", "NONE"};
static const char *const N_BASSPLAY[5] = {"PATTERN", "ROOT", "OCTAVES", "WALKING", "NONE"};
static const char *const N_ARPM[ARP_NORDER] = {"UP", "DOWN", "UP DOWN", "DOWN UP", "AS PLAYED", "ANY", "CHORD", "IN OUT",
                                                "THUMB"};
static const char *const N_ARPR[6] = {"QUARTERS", "8THS", "TRIPLETS", "16THS", "SEXTUPLETS", "32NDS"};
static const uint8_t ARP_RATE[6] = {1, 2, 3, 4, 6, 8};   /* (notes a beat) */
static const char *const N_ARPO[ARPO_N] = {"1 OCT", "2 UP", "3 UP", "4 UP", "2 DOWN", "3 DOWN", "2 BOTH WAYS"};
static const char *const N_ARPPAT[DAB_ARP_NPAT] = {"ALL", "3-3-2", "MAQSUM", "SAIDI", "GALLOP", "OFFBEATS", "PAIRS",
                                                   "ACCENTS"};
static const char *const ARP_PAT_STEPS[DAB_ARP_NPAT] = DAB_ARP_PATTERNS;
static const char *const N_ARPREP[4] = {"ONCE", "TWICE", "3 TIMES", "4 TIMES"};
static const char *const N_ARPSW[6] = {"AS RHYTHM", "NONE", "LIGHT", "MEDIUM", "HEAVY", "FULL"};
static const char *const N_SCENE[4] = {"PORTRAIT", "STAGE", "DANCERS", "CASSETTE"};
static const char *const N_PANEL[4] = {"DIALS", "STEPS", "NOTES", "MIX"};
static const char *const N_USB[2] = {"AUTO", "COMPUTER"};
typedef struct {
    const char *label;
    uint8_t n, eng, start;
    const char *const *names;            /* 0: the maqamat's, from the engine */
} choice_t;
static const choice_t CHOICE[NCH] = {
    [C_MODEL] = {"INSTRUMENT", MD_COUNT, P_MODEL, MD_MIJWIZ, MODEL_NAME},
    [C_PLAY] = {"PLAY", 3, P_PLAY, 0, N_PLAY},
    [C_DOUBLE] = {"DOUBLE", 3, P_DOUBLE, 0, N_DOUBLE},
    [C_ORN] = {"ORNAMENT", ORN_COUNT, P_ORNAMENT, ORN_AUTO, N_ORN},
    [C_TRILLTO] = {"TRILL TO", 3, P_TRILLTO, 0, N_TRILL},
    [C_PICK] = {"PICKING", 2, P_PICKING, 0, N_PICK},
    [C_DRONE] = {"DRONE", 4, P_DRONEMODE, 1, N_DRONE},
    [C_DRONESND] = {"SOUND", DAB_NDRONE, P_DRONESND, 0, N_DSND},   /* (the drone's: beside DRONE on both its rows) */
    [C_DRONEOCT] = {"DRONE PITCH", 2, P_DRONEOCT, 0, N_DOCT},
    [C_BEND] = {"BEND RANGE", 4, P_BENDRANGE, 1, N_BEND},
    [C_DSYNC] = {"DELAY SYNC", 4, P_DELAYSYNC, 0, N_DSYNC},
    [C_PRESS] = {"PRESSURE", 5, P_PRESS, 1, N_PRESS},
    [C_TOUCH] = {"KEY TOUCH", 2, NOENG, 1, N_TOUCH},
    [C_LEFTMODEL] = {"LEFT SOUND", MD_COUNT, P_LEFTMODEL, MD_STRINGS, MODEL_NAME},
    [C_BASSMODEL] = {"BASS SOUND", DAB_BASSV, P_BASSMODEL, 0, N_BASSM},
    [C_CHORD] = {"CHORD TYPE", 2, P_CHORDTYPE, 0, N_CHORD},
    [C_VAR] = {"VARIATION", 4, P_VARIATION, 0, N_VAR},
    [C_ADDCLAP] = {"ADD CLAPS", 2, P_ADDCLAP, 0, OFF_ON},
    [C_ADDRIQ] = {"ADD RIQ", 2, P_ADDRIQ, 0, OFF_ON},
    [C_ADDSAGAT] = {"ADD SAGAT", 2, P_ADDSAGAT, 0, OFF_ON},
    [C_ADDKICK] = {"ADD KICK", 2, P_ADDKICK, 0, OFF_ON},
    [C_AUTOFILL] = {"AUTO FILL", 5, P_AUTOFILL, 0, N_AUTOFILL},
    [C_FILL] = {"FILL IS", 3, P_FILLTYPE, 0, N_FILL},
    [C_SYNC] = {"SYNC START", 2, NOENG, 0, OFF_ON},
    [C_LEFT] = {"LEFT KEYS", 3, NOENG, 0, N_LEFT},
    [C_BACKING] = {"BACKING", 4, NOENG, 0, N_BACKING},
    [C_BASSPLAY] = {"BASS PLAYS", 5, NOENG, 0, N_BASSPLAY},
    [C_ARPMODE] = {"ARP ORDER", ARP_NORDER, P_ARPMODE, 0, N_ARPM},
    [C_ARPRATE] = {"ARP SPEED", 6, P_ARPRATE, 3, N_ARPR},
    [C_ARPOCT] = {"ARP OVER", ARPO_N, P_ARPOCT, 0, N_ARPO},
    [C_SCENE] = {"HOME SCENE", 4, NOENG, 1, N_SCENE},     /* (as he chose: the STAGE, over the DIALS panel) */
    [C_USB] = {"USB PORT", 2, NOENG, 1, N_USB},
    [C_MAQAM] = {"MAQAM", 0, NOENG, 2, 0},
    [C_ROOT] = {"ROOT", 12, NOENG, 2, NOTE_NAME},
    [C_PANEL] = {"HOME PANEL", 4, NOENG, 0, N_PANEL},   /* (no longer used: the home page is the tracks' lanes; kept so
                                                          * the settings stay as they were) */
    [C_ARPPAT] = {"ARP PATTERN", DAB_ARP_NPAT, P_ARPPAT, 0, N_ARPPAT},
    [C_ARPREP] = {"ARP REPEAT", 4, P_ARPREP, 0, N_ARPREP},
    [C_ARPSWING] = {"ARP SWING", 6, P_ARPSWING, 0, N_ARPSW},
    [C_ARPLATCH] = {"ARP LATCH", 2, P_ARPLATCH, 0, OFF_ON},
    [C_KICKV] = {"KICK SOUND", DAB_DRUMV, P_DRUMV, 0, N_KICKV},
    [C_DARBV] = {"DRUMS SOUND", DAB_DRUMV, P_DRUMV + 1, 0, N_DARBV},
    [C_CLAPV] = {"CLAPS SOUND", DAB_DRUMV, P_DRUMV + 2, 0, N_CLAPV},
    [C_RIQV] = {"RIQ SOUND", DAB_DRUMV, P_DRUMV + 3, 0, N_RIQV},
    [C_HOLD] = {"HOLD", 2, NOENG, 0, OFF_ON},   /* (no longer used: kept so the settings stay as they were) */
    [C_PSTEPS] = {"PITCH STEPS", 2, NOENG, 0, N_PSTEPS},
};
static int ch_count(int c) { return c == C_MAQAM ? dabke_maqam_count() : CHOICE[c].n; }
static float ch_engine(int c, int v)    /* a choice as the engine takes it */
{
    static const float FILLS[5] = {0, 2, 4, 8, 16}, BENDS[4] = {0.5f, 1.0f, 2.0f, 3.0f};
    switch (c) {
    case C_AUTOFILL: return FILLS[v % 5];
    case C_BEND: return BENDS[v & 3];
    case C_ARPRATE: return (float)ARP_RATE[v % 6];
    case C_ARPREP: return (float)(v + 1);
    case C_ARPSWING: return v ? (float)(v - 1) * 0.25f : -1.0f;   /* (0: as the rhythm) */
    default: return (float)v;
    }
}

/* knob ids beyond the knobs (P_*): a choice (K_CH + C_*), and the pages' own */
#define K_CH 128
#define CH(c) (K_CH + (c))
enum { K_SECOND = 200, K_QDEPTH, K_CENTS, K_DRUM, K_DLEVEL, K_DTUNE, K_DLEN, K_MUTE,
       K_TRACK = K_MUTE + 8, K_TSEND, K_TPITCH, K_TDRUM, K_FADE, K_TOPCLR, K_TLEN, K_TFILT, K_TVOL, K_NONE = 255 };
/* (K_MUTE + part: that part plays or is muted; the picked track's own: which it is, its effect, its pitch, the drum a
 * tapped step gets, its LENGTH, its filter, its volume (the last two with the effect and the pitch: its strip, K1 to K4
 * on the home page and in the step view); FADE: the scene fader's place, as the MIDImix's master fader; TOP: its top
 * sound cleared) */
static const char *const DRUM_NAME[DAB_NDRUM] = {"DUM", "TEK", "KA", "SLAP", "TABL", "STICK", "RIQ", "DAF", "SAGAT",
                                                 "KICK", "CLAP", "SNARE", "SHAKER"};
static const char *const DRUM_SHORT[DAB_NDRUM] = {"DUM", "TEK", "KA", "SLP", "TBL", "TIK", "RIQ", "DAF", "SAG", "KCK",
                                                  "CLP", "SNR", "SHK"};
static const char *const HIT_NAME[4] = {"NONE", "SOFT", "HIT", "ACCENT"};

/* ------------------------------------------------------------------ pages --- */
#define PART_PAR_0 P_LEAD                /* (the parts' volumes, for the mixer's rows) */
#define PART_PAR_1 P_DRONE
#define PART_PAR_2 P_LEFT
#define PART_PAR_3 P_BASS
#define PART_PAR_4 P_GKICK
#define PART_PAR_5 P_GDRUM
#define PART_PAR_6 P_GCLAP
#define PART_PAR_7 P_GMETAL
enum { PG_HOME, PG_SEL, PG_FX, PG_ENV, PG_LFO, PG_GLO };
static const char *const PAGE_TITLE[] = {"HOME", "MAQAM", "EFFECTS", "SOUND", "ORNAMENTS", "RHYTHM"};
#define MAXROWS 13
#define TRK_ROW 3                        /* the effects page's row 4, the track row as COMBO has it: the picked track,
                                          * its LENGTH, the pitch knob's steps, the drum a tapped step gets; row 5: its
                                          * effect, the scene fader, the top sound, its pitch */
#define MIX_ROW 5                        /* and from here on: the mixer, a part a row */
static const uint8_t PAGE_ROWS[4] = {MIX_ROW + DAB_NPART, 6, 4, 8};
static const uint8_t KROW[4][MAXROWS][4] = {
    {{P_ECHO, P_ECHOTIME, P_ECHOFB, P_REVERB}, {P_DRIVE, P_CHORUS, P_PA, P_BRIGHT}, {P_WOW, P_AGE, P_HISS, CH(C_DSYNC)},
     {K_TRACK, K_TLEN, CH(C_PSTEPS), K_TDRUM}, {K_TSEND, K_FADE, K_TOPCLR, K_TPITCH},
#define MIXROW(k) {PART_PAR_##k, P_FILTER + (k), P_LOWEQ + (k), K_MUTE + (k)}
     MIXROW(0), MIXROW(1), MIXROW(2), MIXROW(3), MIXROW(4), MIXROW(5), MIXROW(6), MIXROW(7)},
    {{CH(C_MODEL), CH(C_PLAY), P_TONE, P_BUZZ}, {P_DETUNE, P_BREATH, P_BODY, CH(C_DOUBLE)},
     {P_ATTACK, P_RELEASE, P_GLIDE, P_TRANSPOSE}, {P_TUNE, P_VELAMT, CH(C_BEND), CH(C_PICK)},
     {CH(C_ARPMODE), CH(C_ARPRATE), CH(C_ARPOCT), P_ARPGATE}, {CH(C_ARPPAT), CH(C_ARPREP), CH(C_ARPSWING), CH(C_ARPLATCH)}},
    {{P_VIBRATE, P_VIBDEPTH, P_VIBDELAY, CH(C_ORN)}, {P_ORNSPEED, P_ORNAMOUNT, CH(C_TRILLTO), CH(C_PRESS)},
     {CH(C_DRONE), CH(C_DRONESND), P_DRONE, CH(C_DRONEOCT)}, {CH(C_SCENE), K_NONE, CH(C_USB), CH(C_TOUCH)}},
    {{P_SWING, P_ACCENT, CH(C_VAR), P_LOOSE}, {CH(C_ADDCLAP), CH(C_ADDRIQ), CH(C_ADDSAGAT), CH(C_ADDKICK)},
     {CH(C_AUTOFILL), CH(C_FILL), K_SECOND, CH(C_SYNC)}, {CH(C_LEFT), CH(C_BASSPLAY), CH(C_BACKING), CH(C_CHORD)},
     {CH(C_LEFTMODEL), CH(C_BASSMODEL), P_BASSTONE, P_BASSDECAY}, {K_DRUM, K_DLEVEL, K_DTUNE, K_DLEN},
     {P_KITTUNE, P_KITDECAY, P_KITTONE, P_BALANCE}, {P_GKICK, P_GDRUM, P_GCLAP, P_GMETAL}},
};
static const uint8_t STRIP_ROW[4] = {K_TSEND, K_TFILT, K_TPITCH, K_TVOL};   /* K1 to K4 on the home page and in the step
                                          * view: the picked track's strip, as the MIDImix has it (top knob, middle,
                                          * bottom, fader) */

/* ------------------------------------------------------------ ready sounds --- */
/* each instrument's own knobs when it is chosen: tone, buzz, pipes apart (cents), breath, body, attack (5 ms steps),
 * release (10 ms steps), glide (5 ms steps), and how it is played */
static const int16_t MODEL_DEF[MD_COUNT][9] = {
    {70, 45, 14, 15, 55, 2, 5, 6, 0},    {85, 60, 6, 10, 75, 3, 6, 8, 0},    {65, 15, 10, 0, 40, 1, 12, 12, 0},
    {60, 10, 4, 15, 50, 12, 15, 14, 0},  {40, 0, 0, 55, 50, 16, 20, 10, 0},  {60, 20, 12, 5, 50, 3, 8, 0, 2},
    {55, 0, 10, 0, 40, 40, 60, 0, 2},    {60, 10, 0, 0, 50, 1, 6, 0, 2},     {75, 35, 0, 20, 50, 1, 150, 0, 2},
    {45, 5, 0, 15, 35, 1, 110, 0, 2},    {85, 10, 0, 10, 60, 1, 90, 0, 2},   {55, 30, 12, 0, 50, 6, 10, 4, 2},
    {70, 20, 8, 0, 50, 1, 8, 10, 0},
};
typedef struct {
    uint8_t id;
    int16_t v;                           /* in the knob's steps */
} pv_t;
typedef struct {                         /* (no drone: the drone is the player's part, kept when the sound changes) */
    const char *name;
    uint8_t model, orn, dbl, pick;
    pv_t par[7];
} preset_t;
static const preset_t PRESETS[] = {
    {"MIJWIZ", MD_MIJWIZ, ORN_AUTO, 0, 0, {{P_ORNAMOUNT, 45}, {P_REVERB, 25}}},
    {"MIJWIZ BUZZ", MD_MIJWIZ, ORN_AUTO, 0, 0, {{P_ORNAMOUNT, 60}, {P_BUZZ, 60}, {P_DETUNE, 18}, {P_REVERB, 30}}},
    {"ZURNA", MD_ZURNA, ORN_AUTO, 0, 0, {{P_VIBDEPTH, 30}, {P_ORNAMOUNT, 35}, {P_REVERB, 30}}},
    {"KEY LEAD", MD_LEAD, ORN_AUTO, 0, 0, {{P_ORNAMOUNT, 30}, {P_ECHO, 20}, {P_REVERB, 30}}},
    {"FAST LEAD", MD_LEAD, ORN_AUTO, 0, 0, {{P_ORNAMOUNT, 85}, {P_ORNSPEED, 160}, {P_GLIDE, 4}, {P_TONE, 75}, {P_REVERB, 20}}},
    {"WHISTLE LEAD", MD_NEY, ORN_SCOOP, 0, 0, {{P_BREATH, 5}, {P_VIBDEPTH, 45}, {P_VIBRATE, 62}, {P_GLIDE, 14}, {P_TONE, 55}, {P_REVERB, 35}}},
    {"SQUARE LEAD", MD_SQUARE, ORN_AUTO, 0, 0, {{P_ORNAMOUNT, 40}, {P_ECHO, 25}, {P_REVERB, 20}}},
    {"VIOLIN", MD_VIOLIN, ORN_AUTO, 0, 0, {{P_VIBDEPTH, 40}, {P_ORNAMOUNT, 30}, {P_REVERB, 35}}},
    {"STRINGS", MD_STRINGS, ORN_OFF, 0, 0, {{P_REVERB, 40}, {P_CHORUS, 25}}},
    {"NEY", MD_NEY, ORN_SCOOP, 0, 0, {{P_REVERB, 45}, {P_VIBDEPTH, 15}}},
    {"ACCORDION", MD_ACCORDION, ORN_MORDENT, 0, 0, {{P_VIBDEPTH, 0}, {P_REVERB, 25}}},
    {"ORGAN", MD_ORGAN, ORN_OFF, 0, 0, {{P_VIBDEPTH, 15}, {P_CHORUS, 20}, {P_REVERB, 30}}},
    {"BUZUQ", MD_BUZUQ, ORN_AUTO, 0, 0, {{P_ORNAMOUNT, 25}, {P_REVERB, 25}}},
    {"BUZUQ TREMOLO", MD_BUZUQ, ORN_OFF, 0, 1, {{P_ORNSPEED, 140}, {P_REVERB, 30}}},
    {"SAZ", MD_BUZUQ, ORN_AUTO, 0, 0, {{P_TONE, 90}, {P_BUZZ, 55}, {P_RELEASE, 200}, {P_ORNAMOUNT, 30}, {P_REVERB, 30}}},
    {"OUD", MD_OUD, ORN_AUTO, 0, 0, {{P_ORNAMOUNT, 20}, {P_REVERB, 30}}},
    {"QANUN", MD_QANUN, ORN_OFF, 1, 0, {{P_REVERB, 35}}},
    {"QANUN TREMOLO", MD_QANUN, ORN_OFF, 0, 1, {{P_ORNSPEED, 150}, {P_REVERB, 35}}},
    {"BRASS", MD_BRASS, ORN_OFF, 0, 0, {{P_REVERB, 30}}},
    {"WEDDING ZURNA", MD_ZURNA, ORN_AUTO, 0, 0, {{P_PA, 60}, {P_DRIVE, 25}, {P_REVERB, 40}, {P_ORNAMOUNT, 50}}},
    {"CASSETTE MIJWIZ", MD_MIJWIZ, ORN_AUTO, 0, 0, {{P_WOW, 40}, {P_AGE, 50}, {P_HISS, 25}, {P_ORNAMOUNT, 55}}},
    {"SHAABI LEAD", MD_LEAD, ORN_AUTO, 0, 0, {{P_ECHO, 30}, {P_CHORUS, 30}, {P_PA, 25}, {P_ORNAMOUNT, 50}, {P_BUZZ, 35}}},
};
#define NPRESET ((int)(sizeof PRESETS / sizeof PRESETS[0]))
#define NKEPT 8
#define SOUND_OWN 100                    /* set.sound: 0.. a ready sound, SOUND_OWN + i the kept place i, -1 none */

/* -------------------------------------------------------------- settings --- */
#define SET_MAGIC 0x38424B44u            /* "DKB8": DABKE's settings record, each track with a sound of its own (DKB7: one
                                          * sound, its tracks' instruments: read and turned into this, each track given
                                          * the sound it played; DKB6: four loops and the drum tracks' steps by drum, read
                                          * and turned into DKB7 first; DKB5 had one loop; DKB4 the small arpeggio; DKB3 a
                                          * step sequence; DKB2 no mixer, DKB1 no HOME PANEL); another magic loads the
                                          * starting sound */
#define SET_MAGIC7 0x37424B44u
#define SET_MAGIC6 0x36424B44u
#define KEPT_MAGIC 0x324B4B44u           /* "DKK2": DABKE's kept sounds, each with its tracks' sounds (DKK1: without
                                          * them, read and given the sounds the tracks start with) */
#define KEPT_MAGIC1 0x314B4B44u
#define NSK 30                           /* the knobs a sound changes (27 now), packed in P_* order */
typedef struct {                         /* a MY SOUND: a track's sound and the effects as they were (and the tracks' sounds
                                          * then, kept as 0.16 kept them, no longer given back) */
    int16_t par[NSK];                    /* those knobs, in their own steps */
    uint8_t ch[NSCH];                    /* its choices */
    uint8_t tsnd[DAB_NTRK][2];           /* each track's sound */
} sound_t;
/* Each track's own sound: what a ready sound or a MY SOUND gives a track (a lead sound's: its instrument and its knobs,
 * its vibrato and ornaments, glide, TRANSPOSE, TUNE, PLAY, DOUBLE, PICKING, TRILL TO and BEND RANGE). The effects
 * (the EFFECTS page) are the whole instrument's, as are the tempo, the maqam, the rhythm, the mixer and KEY TOUCH */
#define NTSC 7
static const uint8_t TSC[NTSC] = {C_MODEL, C_PLAY, C_DOUBLE, C_ORN, C_TRILLTO, C_PICK, C_BEND};   /* its choices */
#define NONAME 255
typedef struct {
    int16_t par[DAB_NSK];                /* its knobs, in their own steps (dabke.h DAB_SK's order) */
    uint8_t ch[NTSC];                    /* its choices (TSC's order) */
    uint8_t name;                        /* 0.. a ready sound's, SOUND_OWN + i MY SOUND i + 1; NONAME: its instrument's */
    uint8_t plain;                       /* 1: its tone is its instrument's own, as an instrument alone plays it (a track
                                          * that played a lead instrument beside the sound in 0.16), until a tone knob
                                          * of it turns */
} tsound_t;
typedef struct {
    int16_t par[NSK];
    uint8_t ch[NSCH];
} sound1_t;
typedef struct {
    uint32_t magic;
    uint16_t size;
    uint8_t used[NKEPT];
    sound1_t s[NKEPT];
} kept1_t;
typedef struct {
    uint32_t magic;
    uint16_t size;
    uint8_t used[NKEPT];
    sound_t s[NKEPT];
} kept_t;
#define OWN_BYTES (DAB_RMAX / 4)          /* a lane of your own rhythm: 2 bits a step */
typedef struct {
    uint32_t magic;
    uint16_t size;
    int16_t par[P_COUNT];                /* the knobs, in their own steps */
    uint8_t ch[NCH];                     /* every choice */
    int8_t ntune[12];                    /* each key's tuning, cents */
    uint8_t qdepth, arp;                 /* how flat a quarter flat is (cents); the arpeggio on */
    uint8_t dlv[DAB_NDRUM];              /* each drum: level % */
    uint16_t dtune[DAB_NDRUM], dlen[DAB_NDRUM];   /* tune %, length % */
    char rhy[2][24];                     /* the rhythms by name (the second "": none) */
    uint8_t own_used, own_len[DAB_NOWN], own_div[DAB_NOWN], own_bpm[DAB_NOWN];
    uint8_t own[DAB_NOWN][DAB_NDRUM][OWN_BYTES];
    int8_t oct, sound;
    uint8_t mute;                        /* the mixer's parts muted, a bit each (PART_*) */
    uint8_t track;                       /* the picked track: 0 all, 1 + PART_* */
    uint8_t loop_on;                     /* the loops play with the rhythm */
    uint8_t dedit;                       /* the tracks with steps of their own, a bit each */
    uint8_t tp[DAB_NTRK][DAB_TLANE][DAB_STEPS / 4];   /* their steps, lane by lane, 2 bits a step */
    uint32_t top_mask;                   /* the scene's top: the controls that have one, a bit each (4 a track: effect,
                                          * filter, pitch, volume) */
    int16_t top[4 * DAB_NPART];          /* and their values there */
    uint8_t tsnd[DAB_NTRK][2];           /* each track's sound: its family (SF_*), its number there */
    struct {                             /* each track's loop: its notes are in the pool, track after track */
        uint16_t n, len;
        uint8_t bars, takes, take[DAB_TAKES];
    } lp[DAB_NTRK];
    uint32_t pool[DAB_LOOP_N * 4];
    tsound_t ts[DAB_NTRK];               /* each track's own sound (DKB8; DKB7 ended before it) */
} settings_t;
#define SET7_SIZE 3384u                  /* DKB7, as 0.16 kept it: this record without each track's sound */
typedef struct {                         /* DKB6, read once and turned into DKB7 */
    uint32_t magic;
    uint16_t size;
    int16_t par[P_COUNT];
    uint8_t ch[NCH6];
    int8_t ntune[12];
    uint8_t qdepth, arp;
    uint8_t dlv[DAB_NDRUM];
    uint16_t dtune[DAB_NDRUM], dlen[DAB_NDRUM];
    char rhy[2][24];
    uint8_t own_used, own_len[DAB_NOWN], own_div[DAB_NOWN], own_bpm[DAB_NOWN];
    uint8_t own[DAB_NOWN][DAB_NDRUM][OWN_BYTES];
    int8_t oct, sound;
    uint8_t mute, track, loop_on, dedit;
    uint8_t dpat[DAB_NDRUM][DAB_STEPS];
    uint32_t top_mask;
    int16_t top[4 * DAB_NPART];
    struct {
        uint16_t n, len;
        uint8_t bars, takes, take[DAB_TAKES];
        uint32_t ev[DAB_LOOP_N];
    } trk[DAB_NKT];
} settings6_t;
_Static_assert(sizeof(settings_t) <= PLAT_STORE_MAX, "the settings: one store object");
_Static_assert(sizeof(settings_t) == SET7_SIZE + DAB_NTRK * sizeof(tsound_t), "DKB8: DKB7 as it was, then the tracks' sounds");
_Static_assert(P_COUNT <= K_CH, "the engine's knobs and choices: below the pages' own ids");
_Static_assert(K_CH + NCH <= K_SECOND, "the choices' ids: below the pages' own");
_Static_assert(sizeof(kept_t) <= PLAT_STORE_MAX, "the kept sounds: one store object");
_Static_assert(sizeof(settings6_t) == 3272u, "DKB6 as 0.8 kept it");
_Static_assert(sizeof(kept1_t) == 592u, "DKK1 as 0.8 kept it");
/* What the sequencer added, in a store object of its own beside the settings (which stay DKB7, as 0.9 kept them): each
 * track's LENGTH, its steps 17 to 32, a loop's length as it was played, and the locks */
#define SEQ_MAGIC 0x31534B44u            /* "DKS1" */
typedef struct {
    uint32_t magic;
    uint16_t size;
    uint8_t two;                         /* LENGTH two bars, a bit a track */
    uint8_t dn[DAB_NTRK];                /* his tracks' own steps: how many (0: a bar, as 0.9 had them) */
    uint16_t played[DAB_NTRK];           /* a loop's length as it was played, ticks (0: written step by step) */
    uint8_t tp2[DAB_NTRK][DAB_TLANE][DAB_STEPS / 4];   /* the tracks' steps 17 to 32, lane by lane, 2 bits a step */
    uint32_t lock[DAB_NLOCK];            /* the locks (dabke.h DAB_LK), each in its place; 0: none there */
} seq_t;
_Static_assert(sizeof(seq_t) <= PLAT_STORE_MAX, "the sequencer's part: one store object");
static settings_t set;
static kept_t kept;
static int picked(void);                 /* the picked track (ALL: track 1): whose sound the SOUND and ORNAMENTS pages show */
static int sc_of(int c)                  /* a choice's place in a track's own sound, -1: not one of its */
{
    int k;
    for (k = 0; k < NTSC; k++)
        if (TSC[k] == c)
            return k;
    return -1;
}
static seq_t sq PLAT_POOL;
static uint8_t set_dirty, sq_dirty;      /* changed since the last save (the settings; the sequencer's part) */
static uint32_t set_changed_at;
#define LAZY_SAVE_MS 3000u

/* ---------------------------------------------------------------- the UI --- */
static struct {
    uint8_t page, save, slot, cur, drum, step;
    uint8_t row[4];                      /* each knob page's row */
    int8_t r0, r1;                       /* the rhythms by index (r1 -1: none) */
    uint32_t keys, btn;
    uint16_t keynote[NKEYS];             /* a held key's manual and note (0x8000 | manual << 7 | note), 0 none */
    uint8_t vel[NKEYS];
    char msg[40];
    uint32_t msg_until;
    uint32_t ver, drawn_ver, anim_at;    /* what is shown changed: a redraw */
    uint32_t sig;                        /* the engine's state as last shown */
    uint32_t loop_ver, steps_ver;        /* the loops and the tracks' steps as last kept */

    uint8_t arp_held, arp_used, arp_row, arp_shown;   /* ARP held (used as a shift: no on/off), its row, its panel up */
    uint32_t arp_at;
    uint8_t master, master_was;          /* the scene fader: the MIDImix's master fader or FADE, 0..127 (as last moved) */
    uint8_t fade, fade_own;              /* FADE, 0..100 (fade_own: it moved last, not the MIDImix's) */
    uint8_t scene_dirty;                 /* the scene's controls to send again */
    /* a MIDImix strip's control moved: said ("TRACK 6: VOLUME 70") once it is plain that it moved alone (a SEND ALL
     * moves them all: nothing said), then its value as it goes on moving. cc_ctl: the control last moved (its CC + 1),
     * cc_n: how many different ones since cc_win_at, cc_live: this one is being said, its track, knob and value */
    uint8_t cc_ctl, cc_n, cc_live, cc_strip, cc_pid;
    int32_t cc_val;
    uint32_t cc_win_at, cc_at, spage_until;   /* (spage_until: when OCT+ asked for a second bar; 0: not waiting) */
    uint8_t sel_down, sel_used;          /* SEL held on the home page (a white key picked a track with it: no tap) */
    uint32_t top_said, seq_at, rec_at, mrec_at, play_at, edit_at, mix_resend;   /* (said, pressed, held, all lights
                                                                                 * again at) */
    uint8_t rec_trk, mrec_trk, rec_down, mrec_down, rec_done, mrec_done, seq_down, seq_done, bank_down, bank_both;
    uint8_t play_down, play_done, edit_done;   /* (PLAY held: a fill, not a stop; EDIT held in step mode: the locks) */
    int8_t rec_was, undo_trk;            /* the track recording as last seen; a take undone, its words still to say */
    uint8_t undo_steps;                  /* (that take was a drum sound's: its steps tell) */
    uint8_t hold_wait;                   /* HOLD pressed, its words still to say */
    uint32_t hold_ver, hold_snap, held_keys;   /* (the engine's count then; the FM-1's keys down then; those held) */
    uint32_t undo_ver, lit_keys, dim_keys, mid_keys;   /* (the FM-1's key lights as last sent) */
    uint8_t tdrum[DAB_NTRK];             /* each track's lane the keys played last (a drum sound on it) */
    uint8_t edit_held, edit_used;        /* EDIT held; it did something while it was (no HOLD when let go) */
    /* the sequencer: step mode, the page of steps shown, the track picked last (1 + it; 0: none yet); the white keys
     * down as steps (a bit each, by white key), those a key or a knob was used with (no tap when let go), each one's
     * step and when it went down; the step held now (its white key, -1 none; its step), the notes given to it in this
     * hold, "cannot be locked" said in it */
    uint8_t seq, spage, last_trk, sstep, snotes, lock_said, lk_feed;
    int8_t skey;
    uint16_t wdown, wused, m_trig, m_lock;   /* (the page's steps where something begins, where a lock is: for the
                                              * keys' lights, worked out once a frame) */
    uint8_t wstep[16];
    uint32_t wat[16], lock_full_at, full_seen, lk_sig, seq_sig, pick_at;
    /* the note or chord last played on each track (the keys pressed since none of its keys was down), and the notes
     * HOLD holds (their track + 1) */
    uint8_t lastn[DAB_NTRK][4], nlast[DAB_NTRK], heldn[4], nheld, held_trk;
    uint8_t press_sent, press_trk;       /* the keys' pressure as sent last, and the tracks it was on */
    uint8_t raw_peak, cnt_peak;          /* (the readout: the most any key read, and the most counted, since it was read) */
    uint8_t last_drone, last_orn, last_dbl;
    uint32_t beat_count, beat_at, notes_seen, bar_seen, fill_at;
    uint32_t hits_seen[DAB_NDRUM], hit_at[DAB_NDRUM];
    float level, lead;
    uint32_t last_draw;
    uint32_t mix_leds, mix_sent, mix_force;   /* the MIDImix's LEDs: MUTE bits 0..7, REC ARM 8..15, BANK 16, 17, SOLO 18 */
    uint8_t mix_seen, mix_all;
    uint8_t usb_host, usb_nomidi;
    uint32_t usb_since, touched_at, full_at;
    uint32_t head_sig, body_sig;         /* the home panel's moving parts as last drawn */
} ui;
static float t_now;                      /* (seconds and milliseconds now, a frame's) */
static uint32_t ms_now;


/* MIDI in, from the render: the keys straight to the engine, the rest through a ring to the main loop */
static volatile int8_t midi_pick = -1;    /* the picked track, for the notes in (-1: all) */
static volatile uint16_t midi_held[128];  /* a note in: 0x8000 | manual << 7 (4 bits) | the note it sounds, bit 14 from
                                           * TRS */
#define MIX_RING 256u                    /* a power of two: a two-handed sweep while the screen is being sent */
static volatile uint32_t mix_ring[MIX_RING];
static volatile uint32_t mix_w, mix_r;
static volatile uint8_t usb_lost_req;

/* ----------------------------------------------------------------- helpers --- */
static void text_cat(char *d, const char *s)
{
    while (*d)
        d++;
    while (*s)
        *d++ = *s++;
    *d = 0;
}

static void int_text(int32_t v, char *b)     /* signed, no padding */
{
    char t[12];
    int n = 0;
    uint32_t u = (uint32_t)(v < 0 ? -v : v);
    do
        t[n++] = (char)('0' + u % 10u);
    while ((u /= 10u) && n < 11);
    if (v < 0)
        *b++ = '-';
    while (n)
        *b++ = t[--n];
    *b = 0;
}

static int up_char(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

static int name_same(const char *a, const char *b)   /* letters' case aside */
{
    while (*a && up_char((uint8_t)*a) == up_char((uint8_t)*b))
        a++, b++;
    return up_char((uint8_t)*a) == up_char((uint8_t)*b);
}

static void changed(void)                /* a setting: shown again, saved a moment later */
{
    set_dirty = 1;
    set_changed_at = plat_ms();
    ui.ver++;
}
static void sq_changed(void)             /* the sequencer's part (a LENGTH, a step beyond the first bar, a lock): the same */
{
    if (!sq_dirty)
        sq_dirty = 1;
    set_changed_at = plat_ms();
    ui.ver++;
}

static int left_is_lower(void) { return set.ch[C_LEFT] != 0; }

static float par_value(int id)           /* in the knob's own unit */
{
    return (float)(set.par[id] * (int32_t)PAR[id].step) * 0.01f;
}

static float par_norm(int id)
{
    const par_t *p = &PAR[id];
    return p->hi > p->lo ? (float)(set.par[id] - p->lo) / (float)(p->hi - p->lo) : 0.0f;
}

/* the scene: the 32 controls of the MIDImix's strips (4 a strip: the top knob's effect, the middle knob's filter, the
 * bottom knob's own, the fader's volume) each have a value as set and, once moved with the scene fader at its top, a
 * top value; the scene fader (the MIDImix's master, or FADE on the effects page) goes between them */
#define MASTER_TOP 124u                  /* the scene fader from here up: its top (its last 3 %: FADE 97 and up) */
static int ctl_id(int c)
{
    int t = (c >> 2) & 7, k = c & 3;
    return k == 0 ? P_SEND + t : k == 1 ? P_FILTER + t : k == 2 ? P_PITCH + t : PART_PAR[t];
}
static int ctl_of(int id)                /* the scene's control a knob is, -1 none (the maqam and its root: none) */
{
    int c;
    if (id >= K_CH)
        return -1;
    for (c = 0; c < 4 * DAB_NPART; c++)
        if (ctl_id(c) == id)
            return c;
    return -1;
}
static int32_t par_eff(int id)           /* a knob as it sounds: as set, or towards its top as the scene fader goes up */
{
    int c = ctl_of(id);
    int32_t v = set.par[id];
    if (c >= 0 && (set.top_mask >> c & 1u) && ui.master) {
        int32_t d = (int32_t)set.top[c] - v;
        v += (d * (int32_t)ui.master + (d >= 0 ? 63 : -63)) / 127;
    }
    return v;
}
static const float PAR_SC[3] = {0.01f, 1.0f, 0.001f};
static float par_eng(int id, int32_t steps) { return (float)(steps * (int32_t)PAR[id].step) * 0.01f * PAR_SC[PAR[id].scale]; }
static void par_send(int id)             /* a knob's normal value (a track muted: dabke_mute, its volume left as it is). One
                                          * of a sound's: the picked track's own (the pages show it) */
{
    if (DAB_SK(id) >= 0)
        dabke_tpar(picked(), id, par_eng(id, set.par[id]));
    else
        dabke_par(id, par_eng(id, par_eff(id)));
}

static void set_par(int id, int32_t steps)   /* (one of a sound's knobs: the picked track's own sound's) */
{
    int k = DAB_SK(id);
    if (!IS_KNOB(id))
        return;
    steps = clamp(steps, PAR[id].lo, PAR[id].hi);
    if (steps == set.par[id])
        return;
    set.par[id] = (int16_t)steps;
    par_send(id);
    if (k >= 0) {
        tsound_t *o = &set.ts[picked()];
        o->par[k] = (int16_t)steps;
        if (o->plain && k <= DAB_SK(P_RELEASE)) {   /* (a tone knob turned: its tone is its knobs' from now) */
            o->plain = 0;
            dabke_tpar(picked(), DAB_PLAIN, 0.0f);
        }
    }
    changed();
}

static void par_text_at(int id, int32_t steps, char *b)   /* a knob at a value: "6.1 HZ", "320 MS", "1.5 S", "45"; a
                                                            * track's pitch "+1 1/4" */
{
    int32_t v = steps * (int32_t)PAR[id].step, a = v < 0 ? -v : v;
    char t[12];
    b[0] = 0;
    if (PAR[id].unit == U_QT) {
        static const char *const FR[4] = {"", " 1/4", " 1/2", " 3/4"};
        int q = (int)steps, aq = q < 0 ? -q : q;
        if (!q) {
            text_cat(b, "0");
            return;
        }
        text_cat(b, q > 0 ? "+" : "-");
        if (aq >= 4) {
            int_text(aq / 4, t);
            text_cat(b, t);
            text_cat(b, FR[aq % 4]);
        } else
            text_cat(b, FR[aq % 4] + 1);
        return;
    }
    if (v < 0)
        text_cat(b, "-");
    if (PAR[id].unit == U_MS && a >= 100000) {   /* a second or more: in seconds */
        int_text(a / 100000, t);
        text_cat(b, t);
        t[0] = '.';
        t[1] = (char)('0' + (a / 10000) % 10);
        t[2] = 0;
        text_cat(b, t);
        text_cat(b, " S");
        return;
    }
    int_text(a / 100, t);
    text_cat(b, t);
    if (PAR[id].step < 100u) {
        int dec = PAR[id].step < 10u ? 2 : 1;
        int32_t f = a % 100;
        t[0] = '.';
        t[1] = (char)('0' + f / 10);
        t[2] = dec == 2 ? (char)('0' + f % 10) : 0;
        t[3] = 0;
        text_cat(b, t);
    }
    text_cat(b, UNIT[PAR[id].unit]);
}

/* ------------------------------------------------------------------- locks --- */
/* A lock (dabke.h DAB_LK): a step of a track holds a knob at a value. Each has a place in the table, the same place on
 * the audio side (dabke_lock), so the engine's "the lock that holds this knob now" (its place) is one of these. A
 * step's sound (DAB_LK_SOUND, no knob) is a lock like the others: on a drum sound's track, that step's hits play
 * another drum sound. It is marked, named, taken back and kept as they are. */
static float lk_unit(int id)             /* a step of a knob, in the engine's units (a step's sound: none) */
{
    return id == DAB_LK_SOUND ? 0.0f : (float)PAR[id].step * 0.01f * PAR_SC[PAR[id].scale];
}
static int lk_ok(int id) { return id >= 0 && id < DAB_NLK && IS_KNOB(id) && id != P_TEMPO; }   /* a knob a lock can hold */
static int lk_find(int t, int step, int id)   /* the place of that lock, -1: none */
{
    int i;
    for (i = 0; i < DAB_NLOCK; i++) {
        uint32_t l = sq.lock[i];
        if (l && DAB_LK_STEP(l) == step && DAB_LK_TRK(l) == t && DAB_LK_ID(l) == id)
            return i;
    }
    return -1;
}
static int lk_count(int t, int step)     /* the locks on a step of a track (step < 0: on the whole track) */
{
    int i, n = 0;
    for (i = 0; i < DAB_NLOCK; i++) {
        uint32_t l = sq.lock[i];
        if (l && DAB_LK_TRK(l) == t && (step < 0 || DAB_LK_STEP(l) == step))
            n++;
    }
    return n;
}
static int lk_has(int t, int id)         /* track t has a lock of that knob somewhere */
{
    int i;
    for (i = 0; i < DAB_NLOCK; i++) {
        uint32_t l = sq.lock[i];
        if (l && DAB_LK_TRK(l) == t && DAB_LK_ID(l) == id)
            return 1;
    }
    return 0;
}
static void lk_put(int i, uint32_t l)    /* a lock in its place, here and on the audio side */
{
    sq.lock[i] = l;
    dabke_lock(i, l, l ? lk_unit(DAB_LK_ID(l)) : 0.0f);
    sq_changed();
}
static int lk_set(int t, int step, int id, int32_t v, int take)   /* a lock made, or its value changed. 0: no room */
{
    int i = lk_find(t, step, id);
    if ((!lk_ok(id) && id != DAB_LK_SOUND) || step < 0 || step > 127)
        return 1;
    v = id == DAB_LK_SOUND ? clamp(v, 0, 63) : clamp(v, PAR[id].lo, PAR[id].hi);
    if (i < 0) {
        for (i = 0; i < DAB_NLOCK && sq.lock[i]; i++)
            ;
        if (i == DAB_NLOCK)
            return 0;
    } else {
        if (DAB_LK_VAL(sq.lock[i]) == v)
            return 1;
        take = DAB_LK_TAKE(sq.lock[i]);      /* (it stays its take's) */
    }
    lk_put(i, DAB_LK(t, step, id, v, take));
    return 1;
}
static int lk_clear(int t, int step, int take)   /* locks gone: a step's (step >= 0), or a track's (take > 0: those
                                                  * recorded in that take or after; t < 0: every track's). How many */
{
    int i, n = 0;
    for (i = 0; i < DAB_NLOCK; i++) {
        uint32_t l = sq.lock[i];
        if (l && (t < 0 || DAB_LK_TRK(l) == t) && (step < 0 || DAB_LK_STEP(l) == step) && (take <= 0 || DAB_LK_TAKE(l) >= take)) {
            sq.lock[i] = 0;
            if (step >= 0)
                dabke_lock(i, 0, 0.0f);
            n++;
        }
    }
    if (n) {
        if (step < 0)
            dabke_lock_clear(t, take);   /* (a whole track's: one message, the same places) */
        sq_changed();
    }
    return n;
}
static void lk_sounds_drop(int t)        /* track t's sound locks gone (a note sound on it now: a step's sound is a drum's) */
{
    int i;
    for (i = 0; i < DAB_NLOCK; i++)
        if (sq.lock[i] && DAB_LK_TRK(sq.lock[i]) == t && DAB_LK_ID(sq.lock[i]) == DAB_LK_SOUND)
            lk_put(i, 0);
}
static int32_t lk_now(int id, int *held) /* a knob as it sounds now: a lock's value while one holds it, else its own */
{
    int i = id >= 0 && id < DAB_NLK ? dabke_status()->lk_at[DAB_LKK(picked(), id)] : 0;   /* (a sound's knob: the
                                              * picked track's own) */
    uint32_t l = i ? sq.lock[(i - 1) % DAB_NLOCK] : 0;
    if (held)
        *held = l && DAB_LK_ID(l) == id;
    return l && DAB_LK_ID(l) == id ? DAB_LK_VAL(l) : par_eff(id);
}

static void drum_send(int k)
{
    dabke_drum(k, (float)set.dlv[k] * 0.01f, (float)set.dtune[k] * 0.01f, (float)set.dlen[k] * 0.01f);
}

/* ------------------------------------------------------------------- maqam --- */
static void maqam_send(void)             /* every key's tuning, the maqam's tonic and keys, to the engine */
{
    int pc;
    for (pc = 0; pc < 12; pc++)
        dabke_ntune(pc, set.ntune[pc]);
    dabke_par(P_ROOT, (float)set.ch[C_ROOT]);
    dabke_par(P_SCALE, (float)dabke_maqam_scale(set.ch[C_MAQAM], set.ch[C_ROOT]));
}

static void maqam_apply(void)            /* the maqam on its root: each key a quarter flat or as it is */
{
    int pc;
    for (pc = 0; pc < 12; pc++)
        set.ntune[pc] = (int8_t)dabke_maqam_detune(set.ch[C_MAQAM], set.ch[C_ROOT], pc, set.qdepth);
    maqam_send();
    changed();
}

static int maqam_as_set(void)            /* 1: the keys are tuned as the maqam has them */
{
    int pc;
    for (pc = 0; pc < 12; pc++)
        if (set.ntune[pc] != dabke_maqam_detune(set.ch[C_MAQAM], set.ch[C_ROOT], pc, set.qdepth))
            return 0;
    return 1;
}

static void note_cents(int pc, int32_t cents)
{
    pc = ((pc % 12) + 12) % 12;
    set.ntune[pc] = (int8_t)clamp(cents, -100, 100);
    dabke_ntune(pc, set.ntune[pc]);
    changed();
}

static void say_note_tuning(int pc);
static void note_flip(int pc)            /* a key a quarter tone flat, or back */
{
    pc = ((pc % 12) + 12) % 12;
    note_cents(pc, set.ntune[pc] ? 0 : -(int)set.qdepth);
    say_note_tuning(pc);
}

static void maqam_name(char *b, int with_root)   /* "BAYATI ON D" */
{
    const char *n = dabke_maqam_name(set.ch[C_MAQAM]);
    int i = 0;
    for (; *n && i < 20; n++)
        b[i++] = (char)up_char((uint8_t)*n);
    b[i] = 0;
    if (with_root) {
        text_cat(b, " ON ");
        text_cat(b, NOTE_NAME[set.ch[C_ROOT] % 12]);
        if (with_root == 2 && set.ntune[set.ch[C_ROOT] % 12] < 0)   /* (Sikah, Huzam, Iraq: in words) */
            text_cat(b, " HALF FLAT");
    }
}

/* --------------------------------------------------------------- choices --- */
static void left_send(void)
{
    dabke_acc(set.ch[C_LEFT] == 2);
    dabke_style(set.ch[C_BACKING], set.ch[C_BASSPLAY]);
}

static int same_text(const char *a, const char *b)   /* (the device has no strcmp) */
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
#define TOUCH_ON() (set.ch[C_TOUCH] != 0 || set.ch[C_PRESS] != 0)   /* the keys are sensed for the velocity or for the pressure */
static void choice_send(int c)          /* (one of a sound's: the picked track's own, as the pages show it) */
{
    if (sc_of(c) >= 0)
        dabke_tpar(picked(), CHOICE[c].eng, ch_engine(c, set.ch[c]));
    else if (CHOICE[c].eng != NOENG)
        dabke_par(CHOICE[c].eng, ch_engine(c, set.ch[c]));
    else if (c == C_SYNC)
        dabke_sync(set.ch[c]);
    else if (c == C_LEFT || c == C_BACKING || c == C_BASSPLAY)
        left_send();
    else if (c == C_TOUCH)
        touch_on(TOUCH_ON());
    if (c == C_PRESS)
        touch_on(TOUCH_ON());
}

static void model_defaults(int m)        /* an instrument chosen (the picked track's): its own knobs */
{
    static const uint8_t ID[8] = {P_TONE, P_BUZZ, P_DETUNE, P_BREATH, P_BODY, P_ATTACK, P_RELEASE, P_GLIDE};
    tsound_t *o = &set.ts[picked()];
    int i;
    for (i = 0; i < 8; i++)
        set_par(ID[i], MODEL_DEF[m][i]);
    set.ch[C_PLAY] = o->ch[sc_of(C_PLAY)] = (uint8_t)MODEL_DEF[m][8];
    choice_send(C_PLAY);
    if (o->plain) {                      /* (its knobs' tone, as 0.16 gave the sound's instrument) */
        o->plain = 0;
        dabke_tpar(picked(), DAB_PLAIN, 0.0f);
    }
}

/* ---- each track's sound: a family's (SF_*) and its number there; the family a track starts with is its own ---- */
static const uint8_t HOME_CH[DAB_NTRK] = {C_MODEL, C_DRONESND, C_LEFTMODEL, C_BASSMODEL, C_KICKV, C_DARBV, C_CLAPV, C_RIQV};
static const char *const FAM_NAME[SF_N] = {"LEAD", "DRONE", "LEFT HAND", "BASS", "KICK", "DARBUKA", "CLAPS", "RIQ"};
static const char *const *const FAM_LIST[SF_N] = {MODEL_NAME, N_DSND, MODEL_NAME, N_BASSM, N_KICKV, N_DARBV, N_CLAPV, N_RIQV};
static int fam_size(int f) { return dabke_family_size(f); }
static int trk_drum(int t) { return SF_DRUM(set.tsnd[t & 7][0]); }   /* a drum sound on track t */
static int name_shared(int f, int x)     /* another family has a sound of that name */
{
    const char *a = FAM_LIST[f][x];
    int g, y;
    for (g = 0; g < SF_N; g++)
        if (g != f && FAM_LIST[g] != FAM_LIST[f])
            for (y = 0; y < fam_size(g); y++)
                if (same_text(a, FAM_LIST[g][y]))
                    return 1;
    return f == SF_LEAD || f == SF_LEFT;  /* (the lead's and the left hand's share all their names) */
}
static void sound_label(int f, int x, char *b)   /* its own name, its family in front where two share it */
{
    b[0] = 0;
    if (f < 0 || f >= SF_N)
        return;
    x = x < 0 || x >= fam_size(f) ? 0 : x;
    if (name_shared(f, x)) {
        text_cat(b, FAM_NAME[f]);
        text_cat(b, " ");
    }
    text_cat(b, FAM_LIST[f][x]);
}
static void tsnd_send(int t) { dabke_track_sound(t, set.tsnd[t][0], set.tsnd[t][1]); }
static void ts_send(int t);
static void view_load(void);
static void tsnd_put(int t, int f, int x)   /* track t's sound now (its own family's number kept as its choice too). A
                                             * lead instrument other than its own sound's: that instrument's own tone
                                             * in it, its name the instrument's (the rest of its sound as it is) */
{
    x = clamp(x, 0, fam_size(f) - 1);
    set.tsnd[t][0] = (uint8_t)f;
    set.tsnd[t][1] = (uint8_t)x;
    if (f == SF_LEAD && set.ts[t].ch[0] != x) {
        static const uint8_t ID[7] = {P_TONE, P_BUZZ, P_DETUNE, P_BREATH, P_BODY, P_ATTACK, P_RELEASE};
        tsound_t *o = &set.ts[t];
        int i;
        for (i = 0; i < 7; i++)
            o->par[DAB_SK(ID[i])] = MODEL_DEF[x][i];
        o->ch[0] = (uint8_t)x;
        o->plain = 1;
        o->name = NONAME;
        ts_send(t);
        if (t == picked())
            view_load();
    }
    tsnd_send(t);
    if (!SF_DRUM(f))
        lk_sounds_drop(t);
    changed();
}

static void set_choice(int c, int v)     /* (one of a sound's choices: the picked track's own sound's) */
{
    int n = ch_count(c), t;
    v = clamp(v, 0, n - 1);
    if (c == C_MODEL) {                  /* INSTRUMENT: the picked track plays it (ALL: track 1), with its own knobs; its
                                          * name is the instrument's */
        int was;
        t = picked();
        was = set.ts[t].ch[0];
        if (set.tsnd[t][0] != SF_LEAD || set.tsnd[t][1] != v)
            tsnd_put(t, SF_LEAD, v);
        if (was == v)
            return;
        set.ch[c] = set.ts[t].ch[0] = (uint8_t)v;
        set.ts[t].name = NONAME;
        choice_send(c);
        model_defaults(v);
        changed();
        return;
    }
    for (t = 0; t < DAB_NTRK; t++)       /* a track's own family's choice (its page knob): that sound on its track */
        if (HOME_CH[t] == c && (set.tsnd[t][0] != t || set.tsnd[t][1] != v))
            tsnd_put(t, t, v);
    if (set.ch[c] == v)
        return;
    set.ch[c] = (uint8_t)v;
    if (sc_of(c) >= 0)
        set.ts[picked()].ch[sc_of(c)] = (uint8_t)v;
    choice_send(c);
    if (c == C_MAQAM) {
        set.ch[C_ROOT] = (uint8_t)dabke_maqam_root(v);
        maqam_apply();
    } else if (c == C_ROOT)
        maqam_apply();
    changed();
}

/* --------------------------------------------------------------- rhythms --- */
static int rhythm_total(void) { return dabke_rhythm_count() + DAB_NOWN; }
static int rhythm_valid(int i)
{
    int n = dabke_rhythm_count();
    return i >= 0 && (i < n || (i < n + DAB_NOWN && (set.own_used >> (i - n) & 1u)));
}
static void rhythm_name_copy(char *d, int i)
{
    int n = dabke_rhythm_count(), k;
    if (i >= n && i < n + DAB_NOWN) {
        d[0] = 0;
        text_cat(d, "MINE ");
        d[5] = (char)('1' + i - n);
        d[6] = 0;
        return;
    }
    {
        const char *s = i >= 0 ? dabke_rhythm_name(i) : "";
        for (k = 0; k < 23 && s[k]; k++)
            d[k] = s[k];
        d[k] = 0;
    }
}
static int rhythm_find(const char *name)
{
    int i, n = rhythm_total();
    char t[24];
    for (i = 0; name[0] && i < n; i++) {
        if (!rhythm_valid(i))
            continue;
        rhythm_name_copy(t, i);
        if (name_same(t, name))
            return i;
    }
    return -1;
}
static int rhythm_bpm(int i)
{
    int n = dabke_rhythm_count();
    return i < n ? dabke_rhythm_bpm(i) : set.own_bpm[(i - n) % DAB_NOWN] ? set.own_bpm[(i - n) % DAB_NOWN] + 40 : 120;
}

/* the leading and the added rhythm; the tempo goes to the leader's unless the rhythm runs */
static void set_rhythms(int r0, int r1, int retempo)
{
    uint8_t list[2];
    if (!rhythm_valid(r0))
        r0 = 0;
    if (r1 == r0 || !rhythm_valid(r1))
        r1 = -1;
    ui.r0 = (int8_t)r0;
    ui.r1 = (int8_t)r1;
    rhythm_name_copy(set.rhy[0], r0);
    if (r1 >= 0)
        rhythm_name_copy(set.rhy[1], r1);
    else
        set.rhy[1][0] = 0;
    list[0] = (uint8_t)r0;
    list[1] = (uint8_t)(r1 < 0 ? 0 : r1);
    dabke_rhythms(list, r1 < 0 ? 1 : 2);
    if (retempo && !dabke_status()->running)
        set_par(P_TEMPO, rhythm_bpm(r0));
    changed();
}

/* your own rhythms: 2 bits a step */
static int own_get(int s, int lane, int step)
{
    return set.own[s][lane][step >> 2] >> ((step & 3) * 2) & 3;
}
static void own_send(int s)
{
    static uint8_t g[DAB_NDRUM * DAB_RMAX];
    int k, i;
    for (k = 0; k < DAB_NDRUM; k++)
        for (i = 0; i < DAB_RMAX; i++)
            g[k * DAB_RMAX + i] = (uint8_t)own_get(s, k, i);
    dabke_own(s, g, set.own_len[s] ? set.own_len[s] : 16, set.own_div[s] ? set.own_div[s] : 4, 120);
}

/* what the steps page shows: the leading rhythm, step by step */
static uint8_t gview[DAB_NLANE][DAB_RMAX];
static int gview_len = 16, gview_div = 4, gview_of = -2;
static void grid_view(void)
{
    int n = dabke_rhythm_count(), k, i;
    if (ui.r0 < n)
        dabke_rhythm_grid(ui.r0, gview, &gview_len, &gview_div);
    else {
        int s = ui.r0 - n;
        gview_len = set.own_len[s];
        gview_div = set.own_div[s];
        for (k = 0; k < DAB_NDRUM; k++)
            for (i = 0; i < DAB_RMAX; i++)
                gview[k][i] = (uint8_t)own_get(s, k, i);
    }
    gview_of = ui.r0;
}

static void say(const char *s);

/* ------------------------------------------------------------ the sequencer --- */
/* What a track plays, as the screens, the keys' lights and the taps see it. A step is the leading rhythm's step. A note
 * sound with a loop: its notes by the step they begin in, as many steps as its loop is long. A drum sound: his steps,
 * or the rhythm's own for its track (tracks 5 to 8). The automatic bass line and backing (tracks 4 and 3, the left
 * keys on BASS): the rhythm's, until he writes something there. */
static const uint8_t HOME_DRUM[DAB_NDRUM] = {5, 5, 5, 5, 5, 5, 7, 5, 7, 4, 6, 6, 7};   /* each drum's own track */
static int step_q(void)                  /* a step, in a loop's ticks */
{
    if (gview_of != ui.r0)
        grid_view();
    return DAB_TPB / (gview_div >= 2 && gview_div <= 4 ? gview_div : 4);
}
static int trk_loop(int t) { return !trk_drum(t) && dabke_status()->trk_len[t & 7] != 0; }   /* a note sound playing its loop */
static int trk_his(int t)                /* track t is his: something he played or wrote is on it */
{
    const dabke_status_t *s = dabke_status();
    return trk_drum(t) ? s->dedit >> t & 1 : s->trk_len[t & 7] != 0;
}
static int trk_rhythm(int t)             /* the rhythm drives track t while it is not his */
{
    if (trk_drum(t))
        return t >= PART_KICK;
    return set.ch[C_LEFT] == 2 && (t == PART_LEFT || t == PART_BASS);
}
static int trk_nsteps(int t)             /* its steps: its loop's, or its own on the rhythm's clock (a bar's or two) */
{
    const dabke_status_t *s = dabke_status();
    int q = step_q(), n = trk_loop(t) ? (s->trk_len[t] + q - 1) / q : s->tn[t];
    return n < 1 ? 1 : n;
}
static int page_steps(void)              /* the steps a page shows: a bar's, sixteen at most */
{
    int p = dabke_status()->page;
    return p < 1 ? 16 : p > 16 ? 16 : p;
}
static int trk_pages(int t) { return (trk_nsteps(t) + page_steps() - 1) / page_steps(); }
static int step_trig(int t, int st)      /* something of track t begins on its step st: 0 nothing, 1 his, 2 the rhythm's */
{
    const dabke_status_t *s = dabke_status();
    int i, r;
    if (trk_loop(t)) {
        int q = step_q(), p0 = st * q;
        for (i = 0; i < s->trk_n[t] && i < DAB_LOOP_N; i++) {
            int p = (int)DAB_EV_POS(s->trk_ev[t][i]);
            if (p >= p0 && p < p0 + q && p < s->trk_len[t])
                return 1;
        }
        return 0;
    }
    if (gview_of != ui.r0)
        grid_view();
    r = st % (gview_len < 1 ? 1 : gview_len);
    if (trk_drum(t)) {
        if (s->dedit >> t & 1u) {
            if (st >= DAB_TSTEPS)
                return 0;
            for (i = 0; i < DAB_TLANE; i++)
                if (s->tpat[t][i][st >> 2] >> ((st & 3) * 2) & 3u)
                    return 1;
            return 0;
        }
        for (i = 0; i < DAB_NDRUM; i++)
            if (HOME_DRUM[i] == t && gview[i][r])
                return 2;
        return 0;
    }
    if (set.ch[C_LEFT] != 2)
        return 0;
    if (t == PART_BASS)
        return set.ch[C_BASSPLAY] <= 2 ? (gview[DAB_LBASS][r] ? 2 : 0) : set.ch[C_BASSPLAY] == 3 && r % gview_div == 0 ? 2 : 0;
    if (t == PART_LEFT)
        return set.ch[C_BACKING] == 0 ? (gview[DAB_LCHORD][r] == 1 ? 2 : 0) : set.ch[C_BACKING] == 2 && (gview_div != 4 || !(r & 1)) ? 2 : 0;
    return 0;
}
static int step_lane(int t, int st, int lane)   /* a drum sound's step, one lane: 0 none, 1 soft, 2 a hit, 3 accented */
{
    const dabke_status_t *s = dabke_status();
    int g = set.tsnd[t & 7][0] - SF_KICK, k = 0, i;
    if (s->dedit >> t & 1u)
        return st < DAB_TSTEPS ? s->tpat[t][lane][st >> 2] >> ((st & 3) * 2) & 3 : 0;
    if (t < PART_KICK || g < 0)
        return 0;
    if (gview_of != ui.r0)
        grid_view();
    for (i = 0; i < DAB_NDRUM; i++)      /* (the rhythm's own: its track's drums in their order, a lane each) */
        if (HOME_DRUM[i] == t && k++ == lane)
            return gview[i][st % (gview_len < 1 ? 1 : gview_len)];
    return 0;
}

/* ------------------------------------------------------------------ sounds --- */
/* A whole sound (sound_t: a MY SOUND, a ready sound, 0.16's one sound) is a track's sound and the effects. Each track
 * has its own sound (set.ts): the SOUND and ORNAMENTS pages show the picked track's (ALL: track 1's), in set.par and
 * set.ch as every page reads them, and what they change goes to that track alone. A ready sound or a MY SOUND picked
 * with ALL is track 1's and the effects with it (the whole instrument, as 0.16 had it); picked on a track, that
 * track's alone */
#define SOUND_KNOB(i) (IS_KNOB(i) && !PAR[i].keep)
#define DRONE_OWN(i) ((i) == C_DRONE || (i) == C_DRONESND || (i) == C_DRONEOCT)   /* the player's, not the sound's */
static int16_t *sk_slot(sound_t *o, int id)  /* where knob id is in a sound (0: not one of its knobs) */
{
    int i, k = 0;
    for (i = 0; i < P_COUNT && k < NSK; i++)
        if (SOUND_KNOB(i)) {
            if (i == id)
                return &o->par[k];
            k++;
        }
    return 0;
}
static int sk_id(int k)                  /* a track's sound's knob k (dabke.h DAB_SK's order): which knob */
{
    return k <= DAB_SK(P_ORNAMOUNT) ? P_TONE + k : k == DAB_SK(P_TRANSPOSE) ? P_TRANSPOSE : P_TUNE;
}
static void whole_take(sound_t *o)       /* the sound as the pages have it (the picked track's) and the effects */
{
    int i, k = 0;
    memset(o, 0, sizeof *o);
    for (i = 0; i < P_COUNT && k < NSK; i++)
        if (SOUND_KNOB(i))
            o->par[k++] = set.par[i];
    for (i = 0; i < NSCH; i++)
        o->ch[i] = set.ch[i];
    memcpy(o->tsnd, set.tsnd, sizeof o->tsnd);
}
static void ts_from(tsound_t *d, sound_t *o)   /* a track's own sound: a whole sound's */
{
    int k;
    for (k = 0; k < DAB_NSK; k++)
        d->par[k] = (int16_t)clamp(*sk_slot(o, sk_id(k)), PAR[sk_id(k)].lo, PAR[sk_id(k)].hi);
    for (k = 0; k < NTSC; k++)
        d->ch[k] = (uint8_t)clamp(o->ch[TSC[k]], 0, CHOICE[TSC[k]].n - 1);
    d->plain = 0;
}
static void ts_send(int t)               /* track t's own sound, all of it, to the engine */
{
    const tsound_t *o = &set.ts[t];
    int k;
    for (k = 0; k < DAB_NSK; k++)
        dabke_tpar(t, sk_id(k), par_eng(sk_id(k), o->par[k]));
    for (k = 0; k < NTSC; k++)
        dabke_tpar(t, CHOICE[TSC[k]].eng, ch_engine(TSC[k], o->ch[k]));
    dabke_tpar(t, DAB_PLAIN, (float)o->plain);
}
static void view_load(void)              /* the pages: the picked track's own sound (ALL: track 1's). Nothing is sent */
{
    const tsound_t *o = &set.ts[picked()];
    int k;
    for (k = 0; k < DAB_NSK; k++)
        set.par[sk_id(k)] = o->par[k];
    for (k = 0; k < NTSC; k++)
        set.ch[TSC[k]] = o->ch[k];
}

/* a whole sound given to track t: its own sound that sound's (and its instrument on it), named; effects: the effects
 * kept with it too (ALL: the whole instrument's; the drone is the player's) */
static void sound_put(int t, sound_t *o, int name, int effects)
{
    int i, k = 0;
    ts_from(&set.ts[t], o);
    set.ts[t].name = (uint8_t)name;
    ts_send(t);
    if (set.tsnd[t][0] != SF_LEAD || set.tsnd[t][1] != set.ts[t].ch[0])
        tsnd_put(t, SF_LEAD, set.ts[t].ch[0]);
    else if (!effects)                   /* (a track's sound changed: what it played stops, as for any other sound) */
        dabke_track_sound(t, SF_LEAD, set.ts[t].ch[0] + DAB_AGAIN);
    if (t == picked())
        view_load();
    if (effects) {
        for (i = 0; i < DAB_NTRK; i++) {     /* the other tracks' sounds as it has them (a MY SOUND: as they were kept;
                                              * a ready sound: as they are) */
            int f = o->tsnd[i][0] < SF_N ? o->tsnd[i][0] : i, x = clamp(o->tsnd[i][1], 0, fam_size(f) - 1);
            if (i == t)
                continue;
            if (f == i && set.ch[HOME_CH[i]] != x) {   /* (its own family: its choice the same) */
                set.ch[HOME_CH[i]] = (uint8_t)x;
                choice_send(HOME_CH[i]);
            }
            if (set.tsnd[i][0] != f || set.tsnd[i][1] != x)
                tsnd_put(i, f, x);
        }
        for (i = 0; i < NSCH; i++)       /* (DELAY SYNC) */
            if (sc_of(i) < 0 && !DRONE_OWN(i)) {
                int v = clamp(o->ch[i], 0, CHOICE[i].n - 1);
                if (set.ch[i] != v) {
                    set.ch[i] = (uint8_t)v;
                    choice_send(i);
                }
            }
        for (i = 0; i < P_COUNT && k < NSK; i++)
            if (SOUND_KNOB(i)) {
                if (i != P_DRONE && DAB_SK(i) < 0)   /* (the drone's volume stays; its place in a sound is kept, unused) */
                    set_par(i, o->par[k]);
                k++;
            }
    }
    changed();
}

static void preset_make(int k, sound_t *o)   /* ready sound k, whole */
{
    const preset_t *p = &PRESETS[k % NPRESET];
    static const uint8_t ID[8] = {P_TONE, P_BUZZ, P_DETUNE, P_BREATH, P_BODY, P_ATTACK, P_RELEASE, P_GLIDE};
    int i, n = 0;
    for (i = 0; i < P_COUNT && n < NSK; i++)
        if (SOUND_KNOB(i))
            o->par[n++] = PAR[i].start;
    for (i = 0; i < NSCH; i++)
        o->ch[i] = CHOICE[i].start;
    for (i = 0; i < 8; i++)
        *sk_slot(o, ID[i]) = MODEL_DEF[p->model][i];
    o->ch[C_MODEL] = p->model;
    memcpy(o->tsnd, set.tsnd, sizeof o->tsnd);   /* (the tracks as they are; the first: the lead, this instrument) */
    o->tsnd[PART_LEAD][0] = SF_LEAD;
    o->tsnd[PART_LEAD][1] = p->model;
    o->ch[C_PLAY] = (uint8_t)MODEL_DEF[p->model][8];
    o->ch[C_ORN] = p->orn;
    o->ch[C_DOUBLE] = p->dbl;
    o->ch[C_PICK] = p->pick;
    for (i = 0; i < 7 && p->par[i].id; i++) {
        int16_t *q = sk_slot(o, p->par[i].id);
        if (q)
            *q = p->par[i].v;
    }
}
static void preset_give(int k)           /* ready sound k, ALL: track 1's and the effects */
{
    static sound_t o;
    preset_make(k, &o);
    sound_put(PART_LEAD, &o, k, 1);
}

static int sound_count(void)             /* the sounds a lead sound can be: the ready ones, then the MY SOUNDs kept */
{
    int i, n = NPRESET;
    for (i = 0; i < NKEPT; i++)
        n += kept.used[i] != 0;
    return n;
}
static int lead_pos(int t)               /* track t's own sound among them (no name: its instrument's first ready one) */
{
    int nm = set.ts[t].name, i, n = NPRESET;
    if (nm < NPRESET)
        return nm;
    if (nm >= SOUND_OWN && nm < SOUND_OWN + NKEPT && kept.used[nm - SOUND_OWN]) {
        for (i = 0; i < nm - SOUND_OWN; i++)
            n += kept.used[i] != 0;
        return n;
    }
    for (i = 0; i < NPRESET; i++)
        if (PRESETS[i].model == set.ts[t].ch[0])
            return i;
    return 0;
}
static void lead_give(int t, int pos, int effects)   /* the pos-th of them on track t (effects: with ALL) */
{
    static sound_t o;
    int i;
    if (pos < NPRESET) {
        preset_make(pos, &o);
        sound_put(t, &o, pos, effects);
        return;
    }
    pos -= NPRESET;
    for (i = 0; i < NKEPT; i++)
        if (kept.used[i] && pos-- == 0) {
            sound_put(t, &kept.s[i], SOUND_OWN + i, effects);
            return;
        }
}
static int tname(int t)                  /* track t's own sound's name: 0.. a ready sound, SOUND_OWN + i, -1 none */
{
    int nm = set.ts[t & 7].name;
    return nm < NPRESET || (nm >= SOUND_OWN && nm < SOUND_OWN + NKEPT) ? nm : -1;
}
static void lead_name(int t, char *b)    /* its name: a ready sound's, "MY SOUND 2", or its instrument's */
{
    int nm = tname(t);
    b[0] = 0;
    if (nm >= 0 && nm < NPRESET)
        text_cat(b, PRESETS[nm].name);
    else if (nm >= SOUND_OWN) {
        char d[4] = {(char)('1' + nm - SOUND_OWN), 0};
        text_cat(b, "MY SOUND ");
        text_cat(b, d);
    } else
        text_cat(b, MODEL_NAME[set.ts[t & 7].ch[0] % MD_COUNT]);
}
static void sound_name(char *b) { lead_name(PART_LEAD, b); }   /* (track 1's: what the home page names with ALL) */

static void sound_step(int n)            /* SELECT on the home page, ALL picked: the next sound, track 1's and the effects */
{
    int len = sound_count();
    lead_give(PART_LEAD, ((lead_pos(PART_LEAD) + n) % len + len) % len, 1);
}

/* 0.16's one sound (DKB7, DKB6 read: in set.par and set.ch, its name in set.sound) to each track, so that every track
 * sounds as it did: a lead instrument beside the sound's own its instrument's own tone, with the sound's vibrato,
 * ornaments, glide and the rest (as 0.16 played it); every other track the sound itself (track 1's: what the drone, the
 * left hand and the bass line follow) */
static void ts_from16(void)
{
    static sound_t o;
    static const uint8_t ID[7] = {P_TONE, P_BUZZ, P_DETUNE, P_BREATH, P_BODY, P_ATTACK, P_RELEASE};
    int t, i, nm = set.sound;
    whole_take(&o);
    for (t = 0; t < DAB_NTRK; t++) {
        tsound_t *d = &set.ts[t];
        ts_from(d, &o);
        d->name = (uint8_t)(nm >= 0 && (nm < NPRESET || (nm >= SOUND_OWN && nm < SOUND_OWN + NKEPT)) ? nm : NONAME);
        if (set.tsnd[t][0] == SF_LEAD && set.tsnd[t][1] != d->ch[0]) {
            int m = set.tsnd[t][1] % MD_COUNT;
            for (i = 0; i < 7; i++)
                d->par[DAB_SK(ID[i])] = MODEL_DEF[m][i];
            d->ch[0] = (uint8_t)m;
            d->plain = 1;
            d->name = NONAME;
        }
    }
}

/* ------------------------------------------------------------------- saving --- */
static void settings_defaults(void)
{
    int i;
    memset(&set, 0, sizeof set);
    set.magic = SET_MAGIC;
    set.size = (uint16_t)sizeof set;
    for (i = 0; i < P_COUNT; i++)
        set.par[i] = IS_KNOB(i) ? PAR[i].start : 0;
    for (i = 0; i < NCH; i++)
        set.ch[i] = CHOICE[i].start;
    set.qdepth = 50;
    for (i = 0; i < 12; i++)
        set.ntune[i] = (int8_t)dabke_maqam_detune(set.ch[C_MAQAM], set.ch[C_ROOT], i, set.qdepth);
    for (i = 0; i < DAB_NDRUM; i++) {
        set.dlv[i] = 100;
        set.dtune[i] = 100;
        set.dlen[i] = 100;
    }
    for (i = 0; i < DAB_NOWN; i++) {
        set.own_len[i] = 16;
        set.own_div[i] = 4;
    }
    rhythm_name_copy(set.rhy[0], 0);         /* Dabke Syria */
    set.sound = 0;
    set.loop_on = 1;                         /* (no longer used: the loops play whenever the rhythm runs) */
    for (i = 0; i < DAB_NTRK; i++) {         /* each track its own family's sound; its own lead sound the first ready one */
        static sound_t o;
        set.tsnd[i][0] = (uint8_t)i;
        set.tsnd[i][1] = set.ch[HOME_CH[i]];
        preset_make(0, &o);
        ts_from(&set.ts[i], &o);
        set.ts[i].name = 0;
    }
}
static void tp_put(int t, int j, int s, int v)   /* a step of a track's lane, 2 bits */
{
    uint8_t *b = &set.tp[t][j][s >> 2];
    int sh = (s & 3) * 2;
    *b = (uint8_t)((*b & ~(3 << sh)) | (v & 3) << sh);
}
static int tp_get(int t, int j, int s) { return set.tp[t][j][s >> 2] >> ((s & 3) * 2) & 3; }
static int loop_from(int t)              /* where track t's notes begin in the pool */
{
    int i, a = 0;
    for (i = 0; i < t; i++)
        a += set.lp[i].n;
    return a;
}
/* DKB6 read: its four loops into the pool, its drum tracks' steps (by drum) into lanes, its bottom knobs' tops gone (they
 * were the instrument's, now each track's pitch), its pitches (semitones) into quarter tones */
static int settings_from6(void)
{
    static settings6_t o;
    int i, t, j, k, a = 0;
    if (plat_store_load(OBJ_SETTINGS, &o, sizeof o) != (int)sizeof o || o.magic != SET_MAGIC6 || o.size != sizeof o)
        return 0;
    settings_defaults();
    memcpy(set.par, o.par, sizeof set.par);
    memcpy(set.ch, o.ch, sizeof o.ch);
    memcpy(set.ntune, o.ntune, sizeof set.ntune);
    set.qdepth = o.qdepth; set.arp = o.arp;
    memcpy(set.dlv, o.dlv, sizeof set.dlv);
    memcpy(set.dtune, o.dtune, sizeof set.dtune);
    memcpy(set.dlen, o.dlen, sizeof set.dlen);
    memcpy(set.rhy, o.rhy, sizeof set.rhy);
    set.own_used = o.own_used;
    memcpy(set.own_len, o.own_len, sizeof set.own_len);
    memcpy(set.own_div, o.own_div, sizeof set.own_div);
    memcpy(set.own_bpm, o.own_bpm, sizeof set.own_bpm);
    memcpy(set.own, o.own, sizeof set.own);
    set.oct = o.oct; set.sound = o.sound; set.mute = o.mute; set.track = o.track; set.loop_on = o.loop_on;
    for (i = 0; i < DAB_NPART; i++)
        set.par[P_PITCH + i] = (int16_t)clamp(o.par[P_PITCH + i] * 4, -48, 48);
    set.top_mask = o.top_mask & ~0x44444444u;
    memcpy(set.top, o.top, sizeof set.top);
    for (i = 0; i < DAB_NTRK; i++) {
        set.tsnd[i][0] = (uint8_t)i;
        set.tsnd[i][1] = set.ch[HOME_CH[i]];
    }
    for (t = 0; t < DAB_NKT; t++) {      /* the loops, in the pool */
        int n = o.trk[t].n > DAB_LOOP_N ? DAB_LOOP_N : o.trk[t].n;
        set.lp[t].n = (uint16_t)n; set.lp[t].len = o.trk[t].len; set.lp[t].bars = o.trk[t].bars;
        set.lp[t].takes = o.trk[t].takes > DAB_TAKES ? DAB_TAKES : o.trk[t].takes;
        memcpy(set.lp[t].take, o.trk[t].take, sizeof set.lp[t].take);
        memcpy(&set.pool[a], o.trk[t].ev, (uint32_t)n * 4u);
        a += n;
    }
    for (t = PART_KICK; t < DAB_NTRK; t++)   /* the drum tracks' steps, by lane */
        if (o.dedit >> (t - PART_KICK) & 1u) {
            set.dedit |= (uint8_t)(1u << t);
            for (k = 0, j = 0; k < DAB_NDRUM; k++) {
                static const uint8_t DRP[DAB_NDRUM] = {5, 5, 5, 5, 5, 5, 7, 5, 7, 4, 6, 6, 7};
                static const int8_t LANE[DAB_NDRUM] = {0, 1, 2, 3, 4, 5, 0, 6, 1, 0, 0, 1, 2};
                int s2;
                if (DRP[k] != t)
                    continue;
                for (s2 = 0; s2 < DAB_STEPS; s2++)
                    tp_put(t, LANE[k], s2, o.dpat[k][s2]);
                j++;
            }
        }
    set.ch[C_PSTEPS] = 0;
    return 1;
}

static void ts_sane(void)                /* each track's own sound, as it can be (a value out of its range: in it) */
{
    int t, k;
    for (t = 0; t < DAB_NTRK; t++) {
        tsound_t *o = &set.ts[t];
        for (k = 0; k < DAB_NSK; k++)
            o->par[k] = (int16_t)clamp(o->par[k], PAR[sk_id(k)].lo, PAR[sk_id(k)].hi);
        for (k = 0; k < NTSC; k++)
            o->ch[k] = (uint8_t)clamp(o->ch[k], 0, CHOICE[TSC[k]].n - 1);
        if (set.tsnd[t][0] == SF_LEAD)       /* (a lead sound: its instrument the track's) */
            o->ch[0] = set.tsnd[t][1];
        if (tname(t) < 0)
            o->name = NONAME;
        o->plain = o->plain ? 1 : 0;
    }
}
static int settings_sane(void)
{
    int i, total = 0;
    if (set.magic != SET_MAGIC || set.size != sizeof set || set.oct < -3 || set.oct > 3 || set.track > DAB_NPART ||
        set.loop_on > 1u || set.qdepth < 20 || set.qdepth > 80 || set.arp > 1)
        return 0;
    if (set.ch[C_TOUCH] > 1)                 /* (a record from 0.14 or before, KEY TOUCH on its third choice, VEL+PRESS:
                                              * it reads VELOCITY, and the pressure is PRESSURE's alone) */
        set.ch[C_TOUCH] = 1;
    for (i = 0; i < DAB_NTRK; i++) {
        int j;
        if (set.lp[i].n > DAB_LOOP_N || set.lp[i].bars > DAB_LOOP_BARS || set.lp[i].takes > DAB_TAKES ||
            set.lp[i].len > 2047u || set.tsnd[i][0] >= SF_N || set.tsnd[i][1] >= fam_size(set.tsnd[i][0]))
            return 0;
        for (j = 0; j < set.lp[i].takes; j++)
            if (set.lp[i].take[j] > set.lp[i].n)
                return 0;
        total += set.lp[i].n;
    }
    if (total > DAB_LOOP_N * 4)
        return 0;
    for (i = 0; i < 4 * DAB_NPART; i++)
        if ((set.top_mask >> i & 1u) && IS_KNOB(ctl_id(i)) && (set.top[i] < PAR[ctl_id(i)].lo || set.top[i] > PAR[ctl_id(i)].hi))
            return 0;
    for (i = 0; i < P_COUNT; i++)
        if (IS_KNOB(i) && (set.par[i] < PAR[i].lo || set.par[i] > PAR[i].hi))
            return 0;
    for (i = 0; i < NCH; i++)
        if (set.ch[i] >= ch_count(i))
            return 0;
    for (i = 0; i < 12; i++)
        if (set.ntune[i] < -100 || set.ntune[i] > 100)
            return 0;
    for (i = 0; i < DAB_NDRUM; i++)
        if (set.dlv[i] > 100u || set.dtune[i] < 25u || set.dtune[i] > 400u || set.dlen[i] < 20u || set.dlen[i] > 500u)
            return 0;
    for (i = 0; i < DAB_NOWN; i++)
        if (set.own_len[i] < 2u || set.own_len[i] > DAB_RMAX || set.own_div[i] < 2u || set.own_div[i] > 4u)
            return 0;
    ts_sane();
    return 1;
}

static int save_settings(void)           /* the settings, and the sequencer's part if it changed: each its own object */
{
    int bad = 0;
    if (sq_dirty) {
        if (plat_store_save(OBJ_SEQ, &sq, sizeof sq) == 0)
            sq_dirty = 0;
        else
            bad = 1;
    }
    if (set_dirty) {
        if (plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0)
            set_dirty = 0;
        else
            bad = 1;
    }
    if (bad) {
        set_changed_at = plat_ms();          /* not saved: tried again a minute later */
        if (set_dirty)
            set_dirty = 2;
        if (sq_dirty)
            sq_dirty = 2;
        return -1;
    }
    return 0;
}
static void seq_load(void)               /* the sequencer's part from the flash; none there (a 0.9 flash): nothing yet, each
                                          * track a bar long. A lock that makes no sense is dropped, the rest kept */
{
    int i;
    if (plat_store_load(OBJ_SEQ, &sq, sizeof sq) != (int)sizeof sq || sq.magic != SEQ_MAGIC || sq.size != sizeof sq) {
        memset(&sq, 0, sizeof sq);
        sq.magic = SEQ_MAGIC;
        sq.size = (uint16_t)sizeof sq;
    }
    for (i = 0; i < DAB_NTRK; i++) {
        if (sq.dn[i] > DAB_TSTEPS)
            sq.dn[i] = 0;
        if (sq.played[i] > 2047u)
            sq.played[i] = 0;
    }
    for (i = 0; i < DAB_NLOCK; i++) {
        uint32_t l = sq.lock[i];
        int id = DAB_LK_ID(l), v = DAB_LK_VAL(l);
        if (!l)
            continue;
        if (!(l >> 31))
            sq.lock[i] = 0;
        else if (id == DAB_LK_SOUND) {       /* (a step's sound: a drum family's sound that there is) */
            if (v < 0 || (v >> 4) >= SF_N - SF_KICK || (v & 15) >= fam_size(SF_KICK + (v >> 4)))
                sq.lock[i] = 0;
        } else if (!lk_ok(id) || v < PAR[id].lo || v > PAR[id].hi)
            sq.lock[i] = 0;
    }
    sq_dirty = 0;
}

/* everything set, to the engine: at the start */
static void push_all(void)
{
    int i, r0, r1;
    for (i = 0; i < P_COUNT; i++)
        if (IS_KNOB(i))
            par_send(i);
    for (i = 0; i < NCH; i++)
        choice_send(i);
    dabke_par(P_ARPON, (float)set.arp);
    dabke_par(P_BEND, 0.0f);
    dabke_mute(set.mute);
    maqam_send();
    for (i = 0; i < DAB_NDRUM; i++)
        drum_send(i);
    for (i = 0; i < DAB_NOWN; i++)
        if (set.own_used >> i & 1u)
            own_send(i);
    r0 = rhythm_find(set.rhy[0]);
    r1 = rhythm_find(set.rhy[1]);
    set_rhythms(r0 >= 0 ? r0 : 0, r1, 0);
    for (i = 0; i < DAB_NTRK; i++) {         /* each track's sound (its own lead sound), its loop kept, its own steps and
                                              * its length */
        tsnd_send(i);
        ts_send(i);
    }
    for (i = 0; i < DAB_NTRK; i++)
        dabke_loop_set(i, &set.pool[loop_from(i)], set.lp[i].n, set.lp[i].len, set.lp[i].bars, set.lp[i].take,
                       set.lp[i].takes, sq.played[i]);
    dabke_steps_set(&set.tp[0][0][0], &sq.tp2[0][0][0], set.dedit, sq.dn, sq.two);
    ui.lk_feed = 0;                          /* (the locks: a few a frame, once the audio side runs: app_frame) */
}

void app_init(void)
{
    int got;
    dabke_init();
    seq_load();
    got = plat_store_load(OBJ_SETTINGS, &set, sizeof set);
    if (got == (int)SET7_SIZE && set.magic == SET_MAGIC7 && set.size == SET7_SIZE) {   /* DKB7 (0.16): each track given
                                              * the sound it played. Nothing written now: the record is DKB8 once
                                              * something changes */
        set.magic = SET_MAGIC;
        set.size = (uint16_t)sizeof set;
        got = (int)sizeof set;
        ts_from16();
    }
    if ((got != (int)sizeof set || !settings_sane()) && settings_from6()) {
        set.rhy[0][23] = set.rhy[1][23] = 0;     /* (DKB6: turned into this record, saved a moment later) */
        ts_from16();
        ts_sane();
        view_load();
        push_all();
        changed();
    } else if (!settings_sane()) {
        settings_defaults();
        set.rhy[0][23] = set.rhy[1][23] = 0;
        view_load();
        push_all();
        preset_give(0);                      /* the first ready sound */
        set.sound = 0;
    } else {
        set.rhy[0][23] = set.rhy[1][23] = 0;
        view_load();
        push_all();
    }
    if (plat_store_load(OBJ_KEPT, &kept, sizeof kept) != (int)sizeof kept || kept.magic != KEPT_MAGIC ||
        kept.size != sizeof kept) {
        static kept1_t k1;                   /* DKK1: its sounds, each given the tracks' sounds as they start */
        int i, t, ok = plat_store_load(OBJ_KEPT, &k1, sizeof k1) == (int)sizeof k1 && k1.magic == KEPT_MAGIC1 &&
                       k1.size == sizeof k1;
        memset(&kept, 0, sizeof kept);
        kept.magic = KEPT_MAGIC;
        kept.size = (uint16_t)sizeof kept;
        if (ok) {
            for (i = 0; i < NKEPT; i++) {
                kept.used[i] = k1.used[i];
                memcpy(kept.s[i].par, k1.s[i].par, sizeof kept.s[i].par);
                memcpy(kept.s[i].ch, k1.s[i].ch, sizeof kept.s[i].ch);
                for (t = 0; t < DAB_NTRK; t++) {
                    kept.s[i].tsnd[t][0] = (uint8_t)t;
                    kept.s[i].tsnd[t][1] = t == PART_LEAD ? k1.s[i].ch[C_MODEL] : t == PART_DRONE ? k1.s[i].ch[C_DRONESND]
                                                                                                  : set.ch[HOME_CH[t]];
                }
            }

        }
    }
    set.ch[C_HOLD] = 0;                      /* (nothing held at the start) */
    set.loop_on = 1;
    ui.rec_was = -1;                         /* (no track recording, no take undone) */
    ui.undo_trk = -1;
    ui.skey = -1;                            /* (no step held; the track picked last: the one kept) */
    ui.last_trk = set.track;
    ui.last_drone = 1;
    ui.last_orn = ORN_AUTO;
    ui.last_dbl = 1;
    set_dirty = 0;
}

#ifdef PLAT_HOST
void app_host_reboot(void)               /* what RAM forgets */
{
    memset(&set, 0, sizeof set);
    memset(&kept, 0, sizeof kept);
    memset(&sq, 0, sizeof sq);
    memset(&ui, 0, sizeof ui);
    memset((void *)midi_held, 0, sizeof midi_held);
    set_dirty = 0;
    mix_w = mix_r = 0;
    gview_of = -2;
}
#endif

/* ----------------------------------------------------------------- audio --- */
static void midi_key(uint32_t note, int vel, int port)
{
    uint16_t h = midi_held[note];
    if (h) {                                 /* (a note on again, or its off) */
        dabke_key_audio(h >> 7 & 15u, h & 0x7Fu, 0);
        midi_held[note] = 0;
    }
    if (vel) {                               /* (as the FM-1's keys: the picked track; ALL, as set) */
        int rt = dabke_status()->rec_track, pk = midi_pick, m, t, f;
        int32_t s;
        if (pk >= 0 || rt >= 0)
            t = m = pk >= 0 ? pk : rt;
        else if (note < 60u && left_is_lower()) {
            t = PART_LEFT;
            m = set.ch[C_LEFT] == 1 && SF_DRUM(set.tsnd[PART_LEFT][0]) ? PART_LEFT : DAB_NTRK;
        } else
            t = m = 0;
        f = set.tsnd[t][0];
        if (m < DAB_NTRK && SF_DRUM(f)) {    /* a drum sound: its drums up the keys (recording: on its steps too) */
            dabke_track_hit_audio(t, (int)note, rt == t);
            return;
        }
        s = m == DAB_NTRK ? (int32_t)note : (int32_t)note + 12 * set.oct - (f == SF_BASS ? 24 : 0);
        if (s < 0 || s > 127)
            return;
        dabke_key_audio(m, s, vel);
        midi_held[note] = (uint16_t)(0x8000u | (uint32_t)m << 7 | (uint32_t)s | (port == PORT_TRS ? 0x4000u : 0u));
    }
}

static void midi_in(void)                /* audio interrupt: what came in */
{
    uint32_t pkt, i;
    int port;
    if (usb_lost_req) {                      /* the USB device went: its notes off (the TRS jack's stay) */
        usb_lost_req = 0;
        for (i = 0; i < 128u; i++)
            if (midi_held[i] && !(midi_held[i] & 0x4000u))
                midi_key(i, 0, PORT_USB);
    }
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu, ty = st & 0xF0u;
        if ((ty == 0x90u || ty == 0x80u) && d1 >= 28u)
            midi_key(d1, ty == 0x90u ? (int)d2 : 0, port);
        else if (ty == 0xE0u)                /* a pitch wheel */
            dabke_bend_audio((float)((int32_t)(d1 | d2 << 7) - 8192) * (1.0f / 8192.0f));
        else if (ty == 0xB0u && d1 == 1u)    /* a mod wheel: vibrato */
            dabke_mod_audio((float)d2 * (1.0f / 127.0f));
        else if ((ty == 0x90u || ty == 0x80u || ty == 0xB0u) && mix_w - mix_r < MIX_RING) {
            mix_ring[mix_w % MIX_RING] = pkt;      /* the MIDImix: the main loop acts on it */
            mix_w++;
        }
    }
}

#ifdef PLAT_HOST
static float host_out[8192];             /* (the simulator's checks: the output's last samples, for "pitch") */
static uint32_t host_out_w;
#endif
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    midi_in();
    dabke_render(lr, n, (float)master_q12 * (8300000.0f / 4096.0f));   /* MASTER: full scale x the gain */
#ifdef PLAT_HOST
    {
        uint32_t i;
        for (i = 0; i < n; i++)
            host_out[host_out_w++ & 8191u] = (float)(lr[2 * i] + lr[2 * i + 1]) * 0.5f;
    }
#endif
}

void app_panic(void) { dabke_panic(); }

/* ------------------------------------------------------------- messages --- */
static char cap_at(const char *s, const char *p)   /* capitals, but a note's flat ("Eb") stays a small b */
{
    if (*p == 'b' && p > s && p[-1] >= 'A' && p[-1] <= 'G' && (p[1] < 'a' || p[1] > 'z'))
        return 'b';
    return (char)up_char((uint8_t)*p);
}

void app_say(const char *a, const char *b)
{
    uint32_t i = 0;
    const char *p;
    for (p = a; p && *p && i < 38u; p++)
        ui.msg[i++] = cap_at(a, p);
    if (b && *b && i < 37u)
        ui.msg[i++] = ' ';
    for (p = b; p && *p && i < 38u; p++)
        ui.msg[i++] = cap_at(b, p);
    ui.msg[i] = 0;
    ui.msg_until = plat_ms() + 1400u;
    ui.ver++;
    if (ann_on())
        ann_say(ui.msg, 0, ANN_NOW);
}

static void say(const char *s) { app_say(s, 0); }

static void say_note_tuning(int pc)          /* "E: QUARTER FLAT", "E: AS IT IS", "E: -30 CT" */
{
    char t[24] = "";
    text_cat(t, NOTE_NAME[pc]);
    text_cat(t, ":");
    if (set.ntune[pc] == 0)
        app_say(t, "AS IT IS");
    else if (set.ntune[pc] == -(int)set.qdepth)
        app_say(t, "QUARTER FLAT");
    else {
        char b[12];
        int_text(set.ntune[pc], b);
        text_cat(b, " CT");
        app_say(t, b);
    }
}

/* ---------------------------------------------------------------- actions --- */
static void set_oct(int32_t v)
{
    char t[8] = "", n[8];
    set.oct = (int8_t)clamp(v, -3, 3);
    if (set.oct > 0)
        text_cat(t, "+");
    int_text(set.oct, n);
    text_cat(t, n);
    app_say("OCTAVE", t);
    changed();
}

/* ---- the tracks: the one picked, its sound, recording, its takes; the loops; the scene's top; the hold ---- */
static int picked(void) { return set.track ? set.track - 1 : PART_LEAD; }   /* the picked track (all: the first) */
static void track_sound(int t, char *b)  /* a track's sound by name ("SAZ", "MY SOUND 2", "LEAD ORGAN", "ROUND") */
{
    if (set.tsnd[t & 7][0] == SF_LEAD && tname(t) >= 0)
        lead_name(t, b);
    else
        sound_label(set.tsnd[t & 7][0], set.tsnd[t & 7][1], b);
}
static void trk_name(int t, char *b)     /* "TRACK 3" */
{
    char n[4] = {'0', 0, 0, 0};
    n[0] = (char)('1' + (t & 7));
    b[0] = 0;
    text_cat(b, "TRACK ");
    text_cat(b, n);
}
static void trk_words(int t, const char *what)   /* "TRACK 3: ..." said */
{
    char b[48];
    trk_name(t, b);
    text_cat(b, what);
    say(b);
}
static void pick_say(void)               /* "TRACK 3: LEAD ORGAN" */
{
    char b[48] = "", n[32];
    if (!set.track) {
        text_cat(b, "ALL: ");
        sound_name(b + 5);
    } else {
        trk_name(set.track - 1, b);
        text_cat(b, ": ");
        track_sound(set.track - 1, n);
        text_cat(b, n);
    }
    say(b);
}
static void keys_say(void)               /* the track picked: the keys play it, said ("KEYS PLAY TRACK 4: ROUND"); in
                                          * step mode the white keys are its steps: its name and sound */
{
    char b[48] = "KEYS PLAY ", n[32];
    if (ui.seq) {
        pick_say();
        return;
    }
    if (!set.track) {
        say("KEYS PLAY ALL, AS SET");
        return;
    }
    trk_name(set.track - 1, n);
    text_cat(b, n);
    text_cat(b, ": ");
    track_sound(set.track - 1, n);
    text_cat(b, n);
    say(b);
}
/* SELECT on a picked track: its own family's sounds first, in their order, then every other family's, LEAD, DRONE,
 * LEFT HAND, BASS, KICK, DARBUKA, CLAPS, RIQ (its own left out), round and round. LEAD's: the ready sounds, then the
 * MY SOUNDs kept, each the track's own sound as that sound has it (its effects: the whole instrument's, left as they
 * are) */
static int sel_size(int f) { return f == SF_LEAD ? sound_count() : fam_size(f); }
static int sel_total(void)
{
    int f, n = 0;
    for (f = 0; f < SF_N; f++)
        n += sel_size(f);
    return n;
}
static void sel_at(int t, int pos, int *f, int *x)   /* the pos-th sound in track t's order */
{
    int k;
    for (k = -1; k < SF_N; k++) {
        int g = k < 0 ? t : k;
        if (k >= 0 && k == t)
            continue;
        if (pos < sel_size(g)) {
            *f = g;
            *x = pos;
            return;
        }
        pos -= sel_size(g);
    }
    *f = t;
    *x = 0;
}
static int sel_pos(int t, int f, int x)
{
    int k, pos = 0;
    for (k = -1; k < SF_N; k++) {
        int g = k < 0 ? t : k;
        if (k >= 0 && k == t)
            continue;
        if (g == f)
            return pos + x;
        pos += sel_size(g);
    }
    return 0;
}
static void trk_sound_step(int t, int n) /* track t's sound, n on in its order */
{
    int tot = sel_total(), f = set.tsnd[t][0], pos = sel_pos(t, f, f == SF_LEAD ? lead_pos(t) : set.tsnd[t][1]), x;
    pos = ((pos + n) % tot + tot) % tot;
    sel_at(t, pos, &f, &x);
    if (f == SF_LEAD) {                      /* (a lead sound: that sound on it, the effects as they are) */
        lead_give(t, x, 0);
        return;
    }
    if (f == t)
        set_choice(HOME_CH[t], x);           /* (its own family: as its page knob does, the lead's settings with it) */
    else
        tsnd_put(t, f, x);
    if (set.tsnd[t][0] != f || set.tsnd[t][1] != x)
        tsnd_put(t, f, x);
}
static void track_pick(int trk)          /* the picked track: 0 all, 1 + a track. In step mode: a track, always; the
                                          * steps held are no longer taps, the first page shown */
{
    trk = clamp(trk, 0, DAB_NPART);
    if (ui.seq && !trk)
        trk = 1;
    set.track = (uint8_t)trk;
    view_load();                             /* (the SOUND and ORNAMENTS pages: its own sound) */
    midi_pick = (int8_t)(trk - 1);
    ui.pick_at = plat_ms() | 1u;             /* (it is "the track picked last" once it has stayed picked a moment: the
                                              * tracks SEL passes on its way round are not) */
    if (ui.seq && trk)
        ui.last_trk = (uint8_t)trk;
    ui.spage = 0;
    ui.wused = 0xFFFFu;
    ui.skey = -1;
    changed();
    keys_say();
}
static void pick_step(int n)             /* SEL on the home page: the next track (ALL, 1 .. 8; in step mode 1 .. 8) */
{
    if (ui.seq)
        track_pick(1 + (set.track - 1 + n + 8 * 100) % 8);
    else
        track_pick((set.track + n + 9 * 100) % 9);
}
static void pick_track(int t)            /* a REC ARM of the MIDImix pressed: its track picked */
{
    if (set.track != t + 1)
        track_pick(t + 1);
}
static void knob_step(int id, int n);
static void say_choice(int c);
static void track_sound_step(int n)      /* SELECT on the home page: the picked track's sound (ALL: the whole sound) */
{
    if (!set.track) {
        sound_step(n);
        return;
    }
    trk_sound_step(set.track - 1, n);
    pick_say();
}
static uint32_t loops_mask(void)         /* the tracks with a loop, a bit each */
{
    const dabke_status_t *s = dabke_status();
    uint32_t m = 0;
    int t;
    for (t = 0; t < DAB_NTRK; t++)
        if (s->trk_len[t])
            m |= 1u << t;
    return m;
}
static void track_rec(int t)             /* REC let go (the picked track), a REC ARM let go (its own): it records. The
                                          * rhythm stopped: it waits for the first key */
{
    const dabke_status_t *s = dabke_status();
    char b[40] = "";
    if (t < 0 || t >= DAB_NTRK)
        return;
    if (t == PART_DRONE && !trk_drum(t) && !set.ch[C_DRONE])   /* (a drone that is off cannot move: on, on the tonic) */
        set_choice(C_DRONE, 1);
    dabke_track_rec(t);
    trk_name(t, b);
    text_cat(b, s->running ? ": RECORDING" : ": PLAY TO START");
    say(b);
}
static void empty_say(int t)             /* a track with nothing of his left: "TRACK 3 EMPTY", or, where the rhythm plays
                                          * it again, "TRACK 4: BACK TO THE RHYTHM" */
{
    char b[40] = "";
    trk_name(t, b);
    text_cat(b, trk_rhythm(t) ? ": BACK TO THE RHYTHM" : " EMPTY");
    say(b);
}
static int trk_takes_n(int t);
static void track_undo(int t)            /* REC (a REC ARM) held a second, nothing recording there: its last take gone
                                          * (a recording, or what he wrote by hand since), and its locks. Its words: when
                                          * the engine has done it */
{
    const dabke_status_t *s = dabke_status();
    int i, n;
    n = trk_takes_n(t);
    for (i = 0; i < DAB_NLOCK; i++)          /* locks made since its last take (by hand after a recording, or in a
                                              * recording that kept nothing else): they are its last take, and go alone */
        if (sq.lock[i] && DAB_LK_TRK(sq.lock[i]) == t && (int)DAB_LK_TAKE(sq.lock[i]) > n)
            break;
    if (i < DAB_NLOCK) {
        lk_clear(t, -1, n + 1);
        if (trk_his(t))
            trk_words(t, ": LAST TAKE GONE");
        else
            empty_say(t);
        return;
    }
    if (!trk_his(t)) {                       /* nothing he played or wrote on it: said, and nothing changes. (A lock made
                                              * by hand outside any take stays on its step: EDIT or SEQ held takes it) */
        trk_words(t, ": NOTHING OF YOURS HERE");
        return;
    }
    lk_clear(t, -1, n ? n : 1);              /* (the locks of the take that goes) */
    ui.undo_steps = (uint8_t)trk_drum(t);
    ui.undo_ver = trk_drum(t) ? s->steps_ver : s->loop_ver;
    ui.undo_trk = (int8_t)t;
    dabke_take_undo(t);
}
static void step_mode(int on);
static void clear_all(void)              /* SEQ held a second: every recording, every track's own steps, every lock */
{
    dabke_clear_all();
    memset(sq.lock, 0, sizeof sq.lock);
    sq_changed();
    say("ALL RECORDINGS CLEARED");
}
static void hold_press(void)             /* HOLD (SOLO, EDIT tapped): the notes of the keys down now held (instead of those
                                          * held before); none down: the held ones let go. Said once done */
{
    uint32_t k;
    ui.hold_snap = 0;
    ui.nheld = 0;
    ui.held_trk = 0;
    for (k = 0; k < NKEYS; k++)              /* (the keys that sound a note: what a tapped step takes while they are held) */
        if ((ui.keys >> k & 1u) && (ui.keynote[k] & 0x8000u)) {
            int m = ui.keynote[k] >> 7 & 15, t = m < DAB_NTRK ? m : PART_LEFT;
            ui.hold_snap |= 1u << k;
            if (!ui.held_trk)
                ui.held_trk = (uint8_t)(t + 1);
            if (ui.held_trk == t + 1 && ui.nheld < 4)
                ui.heldn[ui.nheld++] = (uint8_t)(ui.keynote[k] & 0x7Fu);
        }
    ui.hold_ver = dabke_status()->hold_ver;
    ui.hold_wait = 1;
    dabke_hold(1);
}
static void master_set(int m)            /* the scene fader moved (the MIDImix's master fader, or FADE) */
{
    int was = ui.master >= MASTER_TOP;
    m = clamp(m, 0, 127);
    if (m == ui.master)
        return;
    ui.master = (uint8_t)m;
    if (set.top_mask)
        ui.scene_dirty = 1;
    ui.master_was = (uint8_t)m;
    if ((m >= (int)MASTER_TOP) != was)
        say(m >= (int)MASTER_TOP ? "FADER UP: THIS IS THE TOP SOUND" : "FADER DOWN: THE SOUND AS SET");
    ui.ver++;
}
static void top_clear(void)              /* both BANK buttons, or TOP on the effects page: no top sound */
{
    set.top_mask = 0;
    ui.scene_dirty = 1;
    changed();
    say("TOP SOUND CLEARED");
}
static void pitch_words(int q, char *b)  /* a pitch in quarter tones, in words: "UP 1 AND A QUARTER", "IN TUNE" */
{
    static const char *const FR[4] = {"", " AND A QUARTER", " AND A HALF", " AND THREE QUARTERS"};
    static const char *const FR0[4] = {"", "A QUARTER", "A HALF", "THREE QUARTERS"};
    int a = q < 0 ? -q : q;
    char t[8];
    b[0] = 0;
    if (!q) {
        text_cat(b, "IN TUNE");
        return;
    }
    text_cat(b, q > 0 ? "UP " : "DOWN ");
    if (a >= 4) {
        int_text(a / 4, t);
        text_cat(b, t);
        text_cat(b, FR[a % 4]);
    } else
        text_cat(b, FR0[a % 4]);
}
static int pitch_from_cc(int d2)         /* the bottom knob: its middle (60..67) in tune, an octave either way at its
                                          * ends; FULL: whole semitones, MICRO: quarter tones */
{
    int q, micro = set.ch[C_PSTEPS] != 0;
    if (d2 >= 60 && d2 <= 67)
        return 0;
    q = d2 < 60 ? -(((60 - d2) * 48 + 30) / 60) : ((d2 - 67) * 48 + 30) / 60;
    if (!micro)
        q = (q >= 0 ? (q + 2) / 4 : -((-q + 2) / 4)) * 4;
    if (!q)
        q = (d2 < 60 ? -1 : 1) * (micro ? 1 : 4);
    return clamp(q, -48, 48);
}
static int32_t ctl_base(int id)          /* where a control's turn starts: its top's value, with the fader at the top */
{
    int c = ctl_of(id);
    if (c >= 0 && ui.master >= MASTER_TOP && (set.top_mask >> c & 1u))
        return set.top[c];
    return set.par[id];
}
static void ctl_turn(int id, int32_t steps)   /* a control moved (the MIDImix's or the FM-1's): with the scene fader at
                                               * its top, its top value; anywhere else, the sound as it is */
{
    int c = ctl_of(id);
    if (c >= 0 && ui.master >= MASTER_TOP && IS_KNOB(id)) {
        steps = clamp(steps, PAR[id].lo, PAR[id].hi);
        if (!(set.top_mask >> c & 1u) || set.top[c] != steps) {
            set.top[c] = (int16_t)steps;
            set.top_mask |= 1u << c;
            par_send(id);
            changed();
        }
        if (ms_now - ui.top_said > 2000u) {
            ui.top_said = ms_now;
            say("FADER UP: THIS IS THE TOP SOUND");
        }
        return;
    }
    set_par(id, steps);
}

static void arp_toggle(void)
{
    set.arp = !set.arp;
    dabke_par(P_ARPON, (float)set.arp);
    say(set.arp ? "ARPEGGIO ON" : "ARPEGGIO OFF");
    changed();
}

static int home_lines(void) { return ui.page == PG_HOME && !ui.seq && !ui.save; }   /* the home page's top and bottom
                                              * lines are up: they show the tempo and the rhythm */
static void rhythm_step(int n)               /* PRESET, BANK: the leading rhythm (said, where the home page does not
                                              * show it) */
{
    int cnt = rhythm_total(), i = ui.r0, k;
    char b[24];
    for (k = 0; k < cnt; k++) {
        i = (i + n + cnt * 100) % cnt;
        if (rhythm_valid(i))
            break;
    }
    set_rhythms(i, ui.r1 != i ? ui.r1 : -1, 1);
    if (!home_lines()) {
        rhythm_name_copy(b, ui.r0);
        say(b);
    }
}

static void say_choice(int c)
{
    if (c == C_MAQAM || c == C_ROOT) {
        char b[40];
        maqam_name(b, 2);
        say(b);
    }
}

static void part_mute(int k, int on);        /* the mixer's part k muted or not (below) */
static void drone_up(int n)                  /* the drone's volume turned up while it is off: on, on the tonic */
{
    if (n > 0 && !set.ch[C_DRONE]) {
        set_choice(C_DRONE, 1);
        say("DRONE ON");
    }
}

/* ---- a knob with a step held: a lock. A recording track's effect, filter and pitch: locks on the steps it passes ---- */
static void knob_value(int id, char *b, const char **label);
static int step_held(void) { return ui.seq && ui.skey >= 0; }
static void lock_full_say(void)              /* (once in two seconds: a sweep asks many times) */
{
    uint32_t now = plat_ms();
    if (!ui.lock_full_at || now - ui.lock_full_at > 2000u) {
        ui.lock_full_at = now | 1u;
        say("NO ROOM FOR MORE LOCKS");
    }
}
static void lock_refuse(const char *what)    /* a control that cannot be locked, moved with a step held: said once, and
                                              * nothing else happens */
{
    char b[48] = "";
    ui.wused |= (uint16_t)(1u << ui.skey);
    if (ui.lock_said)
        return;
    ui.lock_said = 1;
    text_cat(b, what);
    text_cat(b, " CANNOT BE LOCKED TO A STEP");
    say(b);
}
static int trk_takes_n(int t)                /* track t's takes that can be taken back */
{
    const dabke_status_t *s = dabke_status();
    return trk_drum(t) ? s->dtakes[t & 7] : s->trk_takes[t & 7];
}
static int hand_take(int t)                  /* the take that what he makes by hand on track t now belongs to: the one he
                                              * is writing by hand, or the next one after a recording; before any take:
                                              * none (0: it goes when the track is emptied) */
{
    int n = trk_takes_n(t);
    if (!n)
        return 0;
    return clamp((dabke_status()->hand >> t & 1u) ? n : n + 1, 1, 31);
}
static void step_lock(int id, int32_t v)     /* the held step holds knob id at v */
{
    ui.wused |= (uint16_t)(1u << ui.skey);
    if (!lk_set(picked(), ui.sstep, id, v, hand_take(picked())))
        lock_full_say();
    ui.ver++;
}
/* A step held and SELECT turned: that step's hits play another drum sound (a sound lock, as on an Elektron). The drum
 * sounds in turn: the track's own family's first, then the other drum families' in their order, round and round. Back
 * on the track's own sound: the step's sound lock is gone. A note sound cannot: a note sounds on past its step with
 * its track's one sound: said once ("NOTE SOUNDS CANNOT BE LOCKED TO A STEP": a message holds 38 characters). */
static int dsnd_total(void)
{
    int f, n = 0;
    for (f = SF_KICK; f < SF_N; f++)
        n += fam_size(f);
    return n;
}
static void dsnd_at(int own, int pos, int *f, int *x)   /* the pos-th drum sound, for a track whose sound's family is own */
{
    int k;
    for (k = -1; k < SF_N - SF_KICK; k++) {
        int g = k < 0 ? own : SF_KICK + k;
        if (k >= 0 && g == own)
            continue;
        if (pos < fam_size(g)) {
            *f = g;
            *x = pos;
            return;
        }
        pos -= fam_size(g);
    }
    *f = own;
    *x = 0;
}
static int dsnd_pos(int own, int f, int x)
{
    int k, pos = 0;
    for (k = -1; k < SF_N - SF_KICK; k++) {
        int g = k < 0 ? own : SF_KICK + k;
        if (k >= 0 && g == own)
            continue;
        if (g == f)
            return pos + x;
        pos += fam_size(g);
    }
    return 0;
}
static void step_sound(int n)
{
    int t = picked(), own = set.tsnd[t][0], ownx = set.tsnd[t][1], tot = dsnd_total(), i, f, x, pos;
    if (!trk_drum(t)) {
        lock_refuse("NOTE SOUNDS");
        return;
    }
    ui.wused |= (uint16_t)(1u << ui.skey);
    i = lk_find(t, ui.sstep, DAB_LK_SOUND);
    f = i >= 0 ? SF_KICK + (DAB_LK_VAL(sq.lock[i]) >> 4 & 3) : own;
    x = i >= 0 ? DAB_LK_VAL(sq.lock[i]) & 15 : ownx;
    pos = ((dsnd_pos(own, f, x) + n) % tot + tot) % tot;
    dsnd_at(own, pos, &f, &x);
    if (f == own && x == ownx) {             /* (the track's own sound again: no lock) */
        if (i >= 0)
            lk_put(i, 0);
    } else if (!lk_set(t, ui.sstep, DAB_LK_SOUND, (f - SF_KICK) * 16 + x, hand_take(t)))
        lock_full_say();
    ui.ver++;
}
static int rec_strip(int id)                 /* the recording track's effect, filter or pitch (the fader is never kept) */
{
    const dabke_status_t *s = dabke_status();
    int rt = s->rec_track;
    return rt >= 0 && !s->rec_waiting && (id == P_SEND + rt || id == P_FILTER + rt || id == P_PITCH + rt);
}
static int32_t ctl_from(int id)              /* where a turn of a control starts: as it sounds while its track records
                                              * (a lock may hold it), else its own value */
{
    return rec_strip(id) ? lk_now(id, 0) : ctl_base(id);
}
static void ctl_move(int id, int32_t steps)  /* a control moved by hand. Its track recording: kept as a lock on the step
                                              * the recording is at, heard at once, its own value left as it was, so the
                                              * move comes back with the loop. Else: the control itself */
{
    const dabke_status_t *s = dabke_status();
    int rt = s->rec_track, take;
    if (!rec_strip(id)) {
        ctl_turn(id, steps);
        return;
    }
    take = trk_takes_n(rt) + 1;              /* (the take this recording becomes) */
    if (!lk_set(rt, s->tstep[rt], id, steps, take > 31 ? 31 : take))
        lock_full_say();
}
static int strip_id(int kid)                 /* the picked track's strip: the engine's knob a strip knob is (-1: not one) */
{
    int t = picked();
    return kid == K_TSEND ? P_SEND + t : kid == K_TFILT ? P_FILTER + t : kid == K_TPITCH ? P_PITCH + t
           : kid == K_TVOL ? PART_PAR[t] : -1;
}
static int32_t knob_next(int id, int32_t from, int n)   /* a knob n clicks on from a value */
{
    int32_t range = PAR[id].hi - PAR[id].lo, st = (range + 25) / 50;
    if (id >= P_PITCH && id <= P_PITCH_LAST) {   /* a track's pitch: a step a click as PITCH STEPS says (FULL: from the
                                                  * nearest semitone) */
        if (!set.ch[C_PSTEPS])
            return (from >= 0 ? (from + 2) / 4 : -((-from + 2) / 4)) * 4 + n * 4;
        return from + n;
    }
    if (id == P_TEMPO || id == P_DETUNE || id == P_TUNE || id == P_TRANSPOSE || range <= 50)
        st = 1;
    return from + n * (st < 1 ? 1 : st);
}
static void value_words(int id, int32_t v, char *b)   /* a knob's value in words: "DARK 30", "UP 2", "45" (b: 32
                                                        * characters: "DOWN 11 AND THREE QUARTERS") */
{
    char t[12];
    b[0] = 0;
    if (id >= P_PITCH && id <= P_PITCH_LAST)
        pitch_words((int)v, b);
    else if (id >= P_FILTER && id <= P_LOWEQ_LAST) {
        if (!v)
            text_cat(b, id <= P_FILTER_LAST ? "OPEN" : "FLAT");
        else {
            text_cat(b, id <= P_FILTER_LAST ? (v < 0 ? "DARK " : "THIN ") : (v < 0 ? "LESS " : "MORE "));
            int_text((v < 0 ? -v : v) * 2, t);
            text_cat(b, t);
        }
    } else
        par_text_at(id, v, b);
}
static const char *strip_word(int id)        /* a strip's control, by name */
{
    return id >= P_SEND && id <= P_SEND_LAST ? "EFFECT" : id >= P_FILTER && id <= P_FILTER_LAST ? "FILTER"
           : id >= P_PITCH && id <= P_PITCH_LAST ? "PITCH" : "VOLUME";
}
static void strip_say_at(int t, int id, int32_t val)   /* a track's control just moved: "TRACK 3: FILTER DARK 30" */
{
    char b[48] = ": ", v[32];
    if (id >= P_PITCH && id <= P_PITCH_LAST) {
        value_words(id, val, b + 2);
    } else {
        text_cat(b, strip_word(id));
        text_cat(b, " ");
        value_words(id, val, v);
        text_cat(b, v);
        if (id == PART_PAR[t] && (set.mute >> t & 1u))
            text_cat(b, ", MUTED");
    }
    trk_words(t, b);
}
static void strip_say(int id, int32_t val) { strip_say_at(picked(), id, val); }   /* (the picked track's) */
static void cc_moved(int cc, int t, int id, int32_t val)   /* a MIDImix strip's control moved (its CC, its track, its knob,
                                              * its value as it sounds): said, but never a burst of them (above) */
{
    uint32_t now = plat_ms();
    if (ui.cc_live && ui.cc_ctl == cc + 1 && now - ui.cc_at < 1500u) {   /* (the one being said, moving on: its value now) */
        ui.cc_at = now;
        strip_say_at(t, id, val);
        return;
    }
    ui.cc_live = 0;
    if (!ui.cc_win_at) {
        ui.cc_win_at = now | 1u;
        ui.cc_n = 0;
        ui.cc_ctl = 0;
    }
    if (ui.cc_ctl != cc + 1 && ui.cc_n < 255u)
        ui.cc_n++;
    ui.cc_ctl = (uint8_t)(cc + 1);
    ui.cc_strip = (uint8_t)t;
    ui.cc_pid = (uint8_t)id;
    ui.cc_val = val;
    ui.cc_at = now;
}
static int steps_free(int t, int from, int to)   /* nothing of his begins on track t's steps from .. to - 1 (playing or
                                                  * kept beyond its length), and no lock is there */
{
    const dabke_status_t *s = dabke_status();
    int i, j, st, q = step_q();
    for (i = 0; i < DAB_NLOCK; i++) {
        uint32_t l = sq.lock[i];
        if (l && DAB_LK_TRK(l) == t && DAB_LK_STEP(l) >= from && DAB_LK_STEP(l) < to)
            return 0;
    }
    if (trk_drum(t)) {
        if (s->dedit >> t & 1u)
            for (st = from; st < to && st < DAB_TSTEPS; st++)
                for (j = 0; j < DAB_TLANE; j++)
                    if (s->tpat[t][j][st >> 2] >> ((st & 3) * 2) & 3u)
                        return 0;
        return 1;
    }
    for (i = 0; i < s->trk_n[t] && i < DAB_LOOP_N; i++) {
        int p = (int)DAB_EV_POS(s->trk_ev[t][i]);
        if (p >= from * q && p < to * q)
            return 0;
    }
    return 1;
}
static int length_set(int t, int steps, int played)   /* track t that many steps long (played: a loop as long as it
                                              * was played, in its ticks). 0: it is recording, said, nothing done */
{
    const dabke_status_t *s = dabke_status();
    int P = s->page < 1 ? 16 : s->page, q = step_q(), cur = trk_nsteps(t);
    if (s->rec_track == t) {
        trk_words(t, " IS RECORDING");
        return 0;
    }
    if (steps > cur && steps_free(t, cur, steps)) {   /* longer, into a part with nothing in it: the engine plays there
                                              * again what the track played; its locks the same, so nothing sounds
                                              * different */
        int k, m, full = 0;
        for (k = 0; k < DAB_NLOCK; k++) {
            uint32_t l = sq.lock[k];
            if (l && DAB_LK_TRK(l) == t && DAB_LK_STEP(l) < cur)
                for (m = cur; DAB_LK_STEP(l) + m < steps; m += cur)
                    if (!lk_set(t, DAB_LK_STEP(l) + m, DAB_LK_ID(l), DAB_LK_VAL(l), DAB_LK_TAKE(l)))
                        full = 1;
        }
        if (full)
            lock_full_say();
    }
    dabke_track_len(t, steps > P, trk_loop(t) ? (played ? played : steps * q) : 0);
    ui.wused = 0xFFFFu;
    if (ui.spage * page_steps() >= steps)
        ui.spage = 0;
    return 1;
}
static void length_turn(int n)               /* LENGTH: the picked track a bar long (left), or two (right); a loop played
                                              * live can also be as long as it was played */
{
    const dabke_status_t *s = dabke_status();
    int t = picked(), P = s->page < 1 ? 16 : s->page, q = step_q(), cur = trk_nsteps(t), opt[3], no = 0, i, at = 0, pl = 0;
    char b[40] = ": ", x[8];
    opt[no++] = P;
    if (2 * P <= DAB_TSTEPS)
        opt[no++] = 2 * P;
    if (trk_loop(t) && s->trk_played[t]) {
        pl = (s->trk_played[t] + q - 1) / q;
        for (i = 0; i < no && opt[i] != pl; i++)
            ;
        if (i == no) {                       /* (in its place among them, shortest first) */
            for (i = no; i > 0 && opt[i - 1] > pl; i--)
                opt[i] = opt[i - 1];
            opt[i] = pl;
            no++;
        }
    }
    for (i = 0; i < no; i++)
        if (opt[i] <= cur)
            at = i;
    i = clamp(cur < opt[0] && n > 0 ? n - 1 : at + n, 0, no - 1);
    if (opt[i] != cur && !length_set(t, opt[i], opt[i] == pl ? s->trk_played[t] : 0))
        return;
    int_text(opt[i], x);
    text_cat(b, x);
    text_cat(b, opt[i] == pl && pl != P && pl != 2 * P ? " STEPS, AS PLAYED" : " STEPS");
    trk_words(t, b);
}

static void knob_step(int id, int n)
{
    int pid;
    if (id == K_NONE)
        return;
    pid = strip_id(id) >= 0 ? strip_id(id) : id;
    if (step_held()) {                       /* a step held: this knob locked on it, from its value there (or as it
                                              * sounds), if it is one a lock can hold */
        if (lk_ok(pid)) {
            int i = lk_find(picked(), ui.sstep, pid);
            step_lock(pid, knob_next(pid, i >= 0 ? DAB_LK_VAL(sq.lock[i]) : par_eff(pid), n));
        } else {
            char b[24];
            const char *label = "";
            knob_value(id, b, &label);
            lock_refuse(id >= K_MUTE && id < K_MUTE + DAB_NPART ? "MUTE" : label);
        }
        return;
    }
    if (id >= K_CH && id < K_CH + NCH) {
        int c = id - K_CH, v = set.ch[c] + n;
        if (c == C_MAQAM || c == C_ROOT || c == C_MODEL || c == C_LEFTMODEL || c == C_DRONESND || c == C_BASSMODEL ||
            (c >= C_KICKV && c <= C_RIQV))
            v = (v % ch_count(c) + ch_count(c)) % ch_count(c);   /* (round and round) */
        set_choice(c, v);
        say_choice(c);
        return;
    }
    switch (id) {
    case K_SECOND: {
        int cnt = rhythm_total(), i = ui.r1, k;
        for (k = 0; k < cnt + 1; k++) {
            i += n > 0 ? 1 : -1;
            if (i < -1)
                i = cnt - 1;
            if (i >= cnt)
                i = -1;
            if (i == -1 || (rhythm_valid(i) && i != ui.r0))
                break;
        }
        set_rhythms(ui.r0, i, 0);
        return;
    }
    case K_QDEPTH: {
        int was = set.qdepth, pc;
        set.qdepth = (uint8_t)clamp(set.qdepth + n, 20, 80);
        for (pc = 0; pc < 12; pc++)          /* the keys that were a quarter flat follow */
            if (set.ntune[pc] == -was)
                note_cents(pc, -(int)set.qdepth);
        changed();
        return;
    }
    case K_CENTS:
        note_cents(ui.cur, set.ntune[ui.cur] + n);
        return;
    case K_DRUM:
        ui.drum = (uint8_t)((ui.drum + n + DAB_NDRUM * 100) % DAB_NDRUM);
        ui.ver++;
        return;
    case K_DLEVEL:
        set.dlv[ui.drum] = (uint8_t)clamp(set.dlv[ui.drum] + n * 5, 0, 100);
        drum_send(ui.drum);
        changed();
        return;
    case K_DTUNE:
        set.dtune[ui.drum] = (uint16_t)clamp(set.dtune[ui.drum] + n * 5, 25, 400);
        drum_send(ui.drum);
        changed();
        return;
    case K_DLEN:
        set.dlen[ui.drum] = (uint16_t)clamp(set.dlen[ui.drum] + n * 10, 20, 500);
        drum_send(ui.drum);
        changed();
        return;
    case K_MUTE: case K_MUTE + 1: case K_MUTE + 2: case K_MUTE + 3:
    case K_MUTE + 4: case K_MUTE + 5: case K_MUTE + 6: case K_MUTE + 7:
        part_mute(id - K_MUTE, n < 0);
        return;
    case K_TRACK:                            /* the effects page's track row: the track picked (in step mode: 1 .. 8) */
        if (clamp(set.track + n, ui.seq ? 1 : 0, DAB_NPART) != set.track)
            track_pick(clamp(set.track + n, ui.seq ? 1 : 0, DAB_NPART));
        return;
    case K_TSEND:                            /* the picked track's strip: its effect, its filter, its pitch, its volume */
    case K_TFILT:
    case K_TPITCH:
    case K_TVOL:
    {
        int rec = rec_strip(pid);
        int32_t nv = clamp(knob_next(pid, ctl_from(pid), n), PAR[pid].lo, PAR[pid].hi);
        if (pid == P_DRONE)
            drone_up(n);
        ctl_move(pid, nv);
        if (id == K_TPITCH || (ui.page == PG_HOME && !ui.seq))   /* (the lanes show no values: said) */
            strip_say(pid, rec ? nv : par_eff(pid));
        return;
    }
    case K_TDRUM: {                          /* the drum a tapped step gets (a drum sound's drums in turn; a note
                                              * sound: the keys' notes, nothing to choose) */
        int t = picked(), f = set.tsnd[t][0], nd = dabke_sound_drums(f);
        char b[40] = ": A TAP ADDS ";
        if (!trk_drum(t) || nd < 1) {
            trk_words(t, ": A TAP ADDS THE KEYS' NOTES");
            return;
        }
        ui.tdrum[t] = (uint8_t)(((ui.tdrum[t] % nd) + n % nd + nd) % nd);
        text_cat(b, DRUM_NAME[dabke_sound_drum(f, ui.tdrum[t])]);
        trk_words(t, b);
        ui.ver++;
        return;
    }
    case K_TLEN:
        length_turn(n);
        return;
    case K_FADE:                             /* the scene fader, as the MIDImix's master: FADE 97 and up is its top */
        ui.fade = (uint8_t)clamp(ui.fade + n, 0, 100);
        master_set((ui.fade * 127 + 99) / 100);
        ui.fade_own = 1;
        return;
    case K_TOPCLR:
        top_clear();
        return;
    }
    if (id == P_DRONE)
        drone_up(n);
    if (IS_KNOB(id))                         /* (a control of the scene: with its fader at the top, its top value) */
        ctl_move(id, knob_next(id, ctl_from(id), n));
}

static void keep_sound(void)                 /* SAVE, the second time: the sound kept in its place */
{
    char t[4] = {(char)('1' + ui.slot), 0};
    whole_take(&kept.s[ui.slot]);            /* (the picked track's sound, as the pages show it, and the effects) */
    kept.used[ui.slot] = 1;
    set.ts[picked()].name = (uint8_t)(SOUND_OWN + ui.slot);
    changed();
    ui.save = 0;
    if (plat_store_save(OBJ_KEPT, &kept, sizeof kept) == 0 && save_settings() == 0)
        app_say("KEPT IN", t);
    else
        say("COULD NOT KEEP IT");
}

/* ---------------------------------------------------------------- the panel --- */
static void part_turn(int p, int n);         /* the home page's K1..K4 with ALL picked (panel.c) */
static const uint8_t ARP_ROW[2][4] = {{CH(C_ARPMODE), CH(C_ARPRATE), CH(C_ARPOCT), P_ARPGATE},
                                       {CH(C_ARPPAT), CH(C_ARPREP), CH(C_ARPSWING), CH(C_ARPLATCH)}};
static void turn(int e, int n)
{
    if (ui.arp_held && e != EN_PRESET && e != EN_ALGO) {   /* ARP held: its controls (SELECT or SEL: the other row) */
        if (e == EN_SELECT)
            ui.arp_row ^= 1u;
        else
            knob_step(ARP_ROW[ui.arp_row & 1u][e - EN_K1], n);
        ui.arp_used = 1;
        ui.ver++;
        return;
    }
    if (ui.save) {
        if (e == EN_SELECT) {
            ui.slot = (uint8_t)((ui.slot + n + NKEPT * 100) % NKEPT);
            ui.ver++;
        }
        return;
    }
    if (e == EN_PRESET) {                    /* the rhythm; the tempo (a step held: neither can be locked) */
        if (step_held())
            lock_refuse("THE RHYTHM");
        else
            rhythm_step(n);
        return;
    }
    if (e == EN_ALGO) {
        if (step_held())
            lock_refuse("THE TEMPO");
        else {
            set_par(P_TEMPO, set.par[P_TEMPO] + n * 2);
            if (!home_lines()) {             /* (said, where the home page's top line does not show it) */
                char b[8];
                int_text(set.par[P_TEMPO], b);
                app_say("TEMPO", b);
            }
        }
        return;
    }
    if (ui.page == PG_HOME) {                /* SELECT: the picked track's sound. K1..K4: its strip (effect, filter,
                                              * pitch, volume); ALL picked: the four parts' volumes */
        if (e == EN_SELECT) {
            if (step_held())
                step_sound(n);               /* (a step held: that step's sound, a lock) */
            else
                track_sound_step(n);
        } else if (set.track)
            knob_step(STRIP_ROW[e - EN_K1], n);
        else
            part_turn(e - EN_K1, n);
    } else if (ui.page == PG_SEL) {
        static const uint8_t MQK[4] = {K_CENTS, CH(C_MAQAM), CH(C_ROOT), K_QDEPTH};
        if (e == EN_SELECT) {
            ui.cur = (uint8_t)((ui.cur + n + 1200) % 12);
            ui.ver++;
        } else
            knob_step(MQK[e - EN_K1], n);
    } else {
        int pg = ui.page - PG_FX;
        if (e == EN_SELECT) {                /* (a step held: still the page's rows, to reach the knob to lock) */
            ui.row[pg] = (uint8_t)clamp(ui.row[pg] + n, 0, PAGE_ROWS[pg] - 1);
            ui.ver++;
        } else
            knob_step(KROW[pg][ui.row[pg]][e - EN_K1], n);
    }
}

/* The development readout (fm1_remote.py log): why a lean does or does not bend. The picked track and the two settings;
 * the pressure as counted now, on which tracks, and the most since the last readout (counted, and read from any key);
 * what the engine has of it; then the keys that are down: the note, the manual, the pressure read, sensed or not. */
static void log_pair(char *b, const char *k, int32_t v)
{
    char n[12];
    text_cat(b, k);
    int_text(v, n);
    text_cat(b, n);
}
void app_log(void (*line)(const char *t))
{
    char b[48];
    int32_t o[4];
    uint32_t k;
    int shown = 0;
    dabke_press_debug(o);
    b[0] = 0;
    log_pair(b, "trk ", set.track); log_pair(b, " touch ", set.ch[C_TOUCH]); log_pair(b, " mode ", set.ch[C_PRESS]);
    log_pair(b, " range ", set.ch[C_BEND]);
    line(b);
    b[0] = 0;
    log_pair(b, "lean ", ui.press_sent); log_pair(b, " on ", ui.press_trk); log_pair(b, " peak ", ui.cnt_peak);
    log_pair(b, " raw ", ui.raw_peak);
    line(b);
    b[0] = 0;
    log_pair(b, "eng p ", o[0]); log_pair(b, " cents ", o[1]); log_pair(b, " tr ", o[2]); log_pair(b, " mode ", o[3]);
    line(b);
    b[0] = 0;
    log_pair(b, "vib ", set.par[P_VIBDEPTH]); log_pair(b, " dly ", set.par[P_VIBDELAY]); log_pair(b, " orn ", set.ch[C_ORN]);
    log_pair(b, " row ", ui.page >= PG_FX ? ui.row[ui.page - PG_FX] : -1);
    line(b);
    for (k = 0; k < NKEYS && shown < 4; k++)
        if (ui.keys >> k & 1u) {
            b[0] = 0;
            log_pair(b, "key ", (int32_t)k); log_pair(b, " n ", ui.keynote[k] & 0x7Fu); log_pair(b, " m ", ui.keynote[k] >> 7 & 15u);
            log_pair(b, " f ", ui.keynote[k] >> 12); log_pair(b, " p ", touch_pressure(k)); log_pair(b, " a ", touch_sensed(k));
            line(b);
            shown++;
        }
    ui.raw_peak = ui.cnt_peak = 0;
}

/* ---- step mode (SEQ): the white keys are the steps of the picked track ---- */
static void steps_let_go(void)               /* the white keys held as steps: no longer taps, none held */
{
    ui.wused = 0xFFFFu;
    ui.skey = -1;
}
static void step_mode(int on)
{
    if (!on == !ui.seq)
        return;
    steps_let_go();
    ui.spage = 0;
    if (on) {                                /* (ALL picked: the track picked last, track 1 the first time) */
        ui.seq = 1;
        if (!set.track)
            track_pick(ui.last_trk ? ui.last_trk : 1);
        ui.page = PG_HOME;
        ui.save = 0;
        say("STEP MODE: WHITE KEYS ARE STEPS");
    } else {
        ui.seq = 0;
        say("STEP MODE OFF");
    }
    ui.ver++;
}
static void page_say(void)                   /* "PAGE 2 OF 2": a page of the step view (sixteen steps at most), never
                                              * called a bar: a recording counts its bars by the rhythm's ("5 BARS") */
{
    char b[24] = "PAGE ", n[8];
    int_text(ui.spage + 1, n);
    text_cat(b, n);
    text_cat(b, " OF ");
    int_text(trk_pages(picked()), n);
    text_cat(b, n);
    say(b);
}
static int length_set(int t, int steps, int played);
static void page_step(int n)                 /* OCT- and OCT+ in step mode: the page of steps before, after. OCT+ on the
                                              * only bar of a track a bar long: it gets its second bar (as LENGTH gives
                                              * it: the first bar again there, if nothing is there) and that bar is shown.
                                              * LENGTH is the way back */
{
    int t = picked(), pages = trk_pages(t), P = page_steps();
    steps_let_go();
    ui.ver++;
    if (n > 0 && pages == 1 && trk_nsteps(t) <= P && 2 * P <= DAB_TSTEPS) {
        char b[24] = ": ", x[8];
        int to = trk_nsteps(t) < P ? P : 2 * P;   /* (a loop played shorter than a page of steps, as one bar of an
                                              * eight-step rhythm is: the page's sixteen first, all in sight; then
                                              * the second page) */
        if (!length_set(t, to, 0))
            return;
        if (to > P) {
            ui.spage = 1;
            ui.spage_until = plat_ms() | 1u; /* (the engine has it a moment later: the second bar shown meanwhile) */
        }
        int_text(to, x);
        text_cat(b, x);
        text_cat(b, " STEPS");
        trk_words(t, b);
        return;
    }
    if (clamp(ui.spage + n, 0, pages - 1) == ui.spage)   /* (no bar that way: nothing happens) */
        return;
    ui.spage = (uint8_t)clamp(ui.spage + n, 0, pages - 1);
    page_say();
}
static void step_name(int st, char *b)       /* "STEP 5" */
{
    char n[8];
    b[0] = 0;
    text_cat(b, "STEP ");
    int_text(st + 1, n);
    text_cat(b, n);
}
static void step_unlock(int st)              /* a step's locks removed (EDIT with the step held): the note stays */
{
    char b[40];
    step_name(st, b);
    text_cat(b, lk_clear(picked(), st, 0) ? ": LOCKS REMOVED" : ": NO LOCKS");
    say(b);
}
static int chord_for(int t, uint8_t *out)    /* what a tapped step takes on a note sound: the notes HOLD holds on this
                                              * track, else the note or chord last played on it, else the maqam's tonic
                                              * as the keys would play it */
{
    int i, n = 0, note;
    if (dabke_status()->held_n && ui.held_trk == t + 1 && ui.nheld) {
        for (i = 0; i < ui.nheld && i < 4; i++)
            out[n++] = ui.heldn[i];
        return n;
    }
    for (i = 0; i < ui.nlast[t] && i < 4; i++)
        out[n++] = ui.lastn[t][i];
    if (n)
        return n;
    note = 53 + (set.ch[C_ROOT] + 7) % 12 + 12 * set.oct - (set.tsnd[t][0] == SF_BASS ? 24 : 0);
    out[0] = (uint8_t)clamp(note, 0, 127);
    return 1;
}
static void step_tap(int t, int st)          /* a white key tapped. A step with something: what begins there is removed,
                                              * and its locks with it. A step with only locks: they go. An empty one: a
                                              * hit (the drum played last), or the note or chord played last, a step long */
{
    const dabke_status_t *s = dabke_status();
    int q = step_q(), tr = step_trig(t, st), i, n;
    uint8_t notes[4];
    if (st >= trk_nsteps(t))
        return;
    if (trk_drum(t)) {
        if (!tr && lk_count(t, st)) {        /* (a lock only step: it goes) */
            lk_clear(t, st, 0);
            return;
        }
        if (tr)                              /* (its hits go, and its locks with them) */
            lk_clear(t, st, 0);
        n = dabke_sound_drums(set.tsnd[t][0]);
        dabke_step(t, st, ui.tdrum[t] % (n ? n : 1), DAB_STEP_FLIP);
        return;
    }
    if (tr == 1) {                           /* (its notes go, and its locks with them) */
        dabke_note_del(t, st * q, (st + 1) * q);
        lk_clear(t, st, 0);
        return;
    }
    if (lk_count(t, st)) {                   /* (a lock only step: it goes) */
        lk_clear(t, st, 0);
        return;
    }
    n = chord_for(t, notes);
    for (i = 0; i < n; i++)
        dabke_note_add(t, st * q, q, notes[i], s->tn[t] * q, 0);
    if (t == PART_DRONE)
        drone_up(1);                         /* (a drone that is off would not sound its step: on, on the tonic) */
}
static void step_give(int t, int note)       /* a key pressed while a step is held, a note sound: that key's note becomes
                                              * the step's (the first key: instead of what was there; four at most) */
{
    int q = step_q();
    ui.wused |= (uint16_t)(1u << ui.skey);
    if (ui.snotes >= 4)
        return;
    dabke_note_add(t, ui.sstep * q, q, note, dabke_status()->tn[t] * q, !ui.snotes);
    ui.snotes++;
    if (t == PART_DRONE)
        drone_up(1);
}
static void step_key_down(int t, int w, uint32_t k)   /* a white key in step mode: its step of the page held */
{
    int P = page_steps(), st = ui.spage * P + w;
    uint32_t now = plat_ms();
    ui.keynote[k] = 0x4000u;                 /* (nothing to let go) */
    if (w >= P || st >= trk_nsteps(t) || st > 127)
        return;                              /* (past the page's steps, or the track's: not a step) */
    ui.keynote[k] = (uint16_t)(0x2000u | (uint32_t)w);
    ui.wdown |= (uint16_t)(1u << w);
    ui.wused &= (uint16_t)~(1u << w);
    ui.wstep[w] = (uint8_t)st;
    ui.wat[w] = now;
    ui.skey = (int8_t)w;
    ui.sstep = (uint8_t)st;
    ui.snotes = 0;
    ui.lock_said = 0;
    ui.ver++;
}
static void step_key_up(int w)               /* let go: a tap if it was short and nothing was pressed or turned with it */
{
    int tap = (ui.wdown >> w & 1u) && !(ui.wused >> w & 1u) && plat_ms() - ui.wat[w] < 400u;
    ui.wdown &= (uint16_t)~(1u << w);
    if (ui.skey == w) {
        ui.skey = -1;
        ui.ver++;
    }
    if (tap && ui.seq && set.track)
        step_tap(picked(), ui.wstep[w]);
}

static void press(int b)
{
    const dabke_status_t *s = dabke_status();
    uint32_t now = plat_ms();
    ui.ver++;
    if (ui.save && b != B_SAVE) {
        ui.save = 0;
        if (b == B_HOME)
            return;
    }
    switch (b) {
    case B_HOME:                             /* home (in step mode: the step view), and nothing else */
        ui.page = PG_HOME;
        break;
    case B_SEL:                              /* the maqam page; on the home page: the track picked. ARP held: its other four */
        if (ui.arp_held) {
            ui.arp_row ^= 1u;
            ui.arp_used = 1;
        } else if (ui.page == PG_HOME && !ui.save) {   /* (the home page and the step view: the next track when it is
                                              * let go; held, one of the first eight white keys: that track) */
            ui.sel_down = 1;
            ui.sel_used = 0;
        } else
            ui.page = PG_SEL;
        break;
    case B_FX:
    case B_ENV:
    case B_LFO:
    case B_GLO: {
        int pg = b == B_FX ? PG_FX : b == B_ENV ? PG_ENV : b == B_LFO ? PG_LFO : PG_GLO;
        if (ui.page == pg)
            ui.row[pg - PG_FX] = (uint8_t)((ui.row[pg - PG_FX] + 1) % PAGE_ROWS[pg - PG_FX]);
        else
            ui.page = (uint8_t)pg;
        break;
    }
    case B_EDIT:                             /* HOLD, when it is let go. Held, a key: that key a quarter flat. Step mode:
                                              * with a step held, that step's locks removed, now; held a second with no
                                              * step held, all the picked track's locks */
        ui.edit_held = 1;
        ui.edit_used = 0;
        ui.edit_done = 0;
        ui.edit_at = now;
        if (step_held()) {
            ui.wused |= (uint16_t)(1u << ui.skey);
            step_unlock(ui.sstep);
            ui.edit_used = 1;
        }
        break;
    case B_SAVE:
        if (!ui.save)
            ui.save = 1;
        else
            keep_sound();
        break;
    case B_ARP:                              /* tapped: on or off (when let go); held: its panel, the knobs its controls */
        ui.arp_held = 1;
        ui.arp_used = 0;
        ui.arp_at = now;
        break;
    case B_SEQ:                              /* tapped (when let go): step mode on or off; held a second: every recording
                                              * and every lock cleared */
        ui.seq_down = 1;
        ui.seq_done = 0;
        ui.seq_at = now;
        break;
    case B_PLAY:                             /* stopped: everything starts, now. Running: a tap stops everything (when it
                                              * is let go); held 0.4 s: a fill-in, and it goes on */
        if (!s->running)
            dabke_run(1);
        else {
            ui.play_down = 1;
            ui.play_done = 0;
            ui.play_at = now;
        }
        break;
    case B_REC:                              /* recording: it stops, now. Else: the picked track records when the button is
                                              * let go; held a second instead: its last take gone */
        if (s->rec_track >= 0) {
            dabke_track_rec(-1);
            ui.rec_down = 0;
        } else {
            ui.rec_trk = (uint8_t)picked();
            ui.rec_down = 1;
            ui.rec_done = 0;
            ui.rec_at = now;
        }
        break;
    case B_OCTDN:
    case B_OCTUP:                            /* the octave; in step mode: the page of steps before, after */
        if (ui.seq)
            page_step(b == B_OCTUP ? 1 : -1);
        else
            set_oct(set.oct + (b == B_OCTUP ? 1 : -1));
        break;
    }
}

static const int8_t WHITE_IX[NKEYS] = {0, -1, 1, -1, 2, -1, 3, 4, -1, 5, -1, 6, 7, -1, 8, -1, 9, -1, 10, 11, -1, 12, -1,
                                       13, 14, -1, 15};   /* F3 upward: the white keys, the steps of a page */
static const int8_t BLACK_IX[NKEYS] = {-1, 0, -1, 1, -1, 2, -1, -1, 3, -1, 4, -1, -1, 5, -1, 6, -1, 7, -1, -1, 8, -1, 9,
                                       -1, -1, 10, -1};   /* and the black ones: in step mode, a drum sound's drums */
/* The keys play the picked track, all of them, the rhythm running or not, by the sound on it: a note sound's notes (the
 * drone's track: its line's note), a drum sound's drums repeating up the keys. ALL picked: the keys as they are set
 * (the left keys' mode); a track recording: it. In step mode the white keys are the steps of the page shown: tapped, a
 * step gets something or loses it; held, the keys pressed with it are given to it (a note sound: their notes; a drum
 * sound: a black key's drum), and a knob turned locks; the black keys go on playing the track's sound. */
static int key_track(int lower, int *m)  /* the track a key plays; its manual (dabke_key) in m: 8, the left keys as set */
{
    int pk = set.track ? set.track - 1 : -1, rt = dabke_status()->rec_track;
    if (pk >= 0 || rt >= 0) {
        *m = pk >= 0 ? pk : rt;
        return *m;
    }
    if (lower && left_is_lower()) {
        if (set.ch[C_LEFT] == 1 && trk_drum(PART_LEFT)) {   /* (split, a drum sound on the left hand's track: its drums) */
            *m = PART_LEFT;
            return PART_LEFT;
        }
        *m = DAB_NTRK;
        return PART_LEFT;
    }
    *m = 0;
    return 0;
}
static void chord_note(int t, int note, uint32_t k)   /* a note played on track t: the note or chord played last there
                                                        * (the keys pressed since none of its keys was down) */
{
    uint32_t i;
    int n;
    for (i = 0; i < NKEYS; i++)              /* (a key down has its note: this pass's keys too) */
        if (i != k && (ui.keynote[i] & 0x8000u) &&
            ((ui.keynote[i] >> 7 & 15) < DAB_NTRK ? (ui.keynote[i] >> 7 & 15) : PART_LEFT) == t)
            break;
    if (i == NKEYS)
        ui.nlast[t] = 0;
    for (n = 0; n < ui.nlast[t]; n++)
        if (ui.lastn[t][n] == note)
            return;
    if (ui.nlast[t] < 4)
        ui.lastn[t][ui.nlast[t]++] = (uint8_t)note;
}
static void key_down(uint32_t k, int vel)
{
    int rt = dabke_status()->rec_track, m, t, given = 0;
    int32_t note;
    if (ui.edit_held) {                      /* EDIT held: the key a quarter flat, or back; in step mode a white key:
                                              * its step's locks removed */
        ui.edit_used = 1;
        ui.keynote[k] = 0x4000u;
        if (ui.seq && WHITE_IX[k] >= 0) {
            int st = ui.spage * page_steps() + WHITE_IX[k];
            if (WHITE_IX[k] < page_steps() && st < trk_nsteps(picked()))
                step_unlock(st);
        } else
            note_flip((int)(53 + k) % 12);
        return;
    }
    if (ui.sel_down && ui.page == PG_HOME && !ui.save) {   /* SEL held: one of the first eight white keys picks its track,
                                              * at once. No key plays or is a step meanwhile, and SEL let go is no tap */
        ui.sel_used = 1;
        ui.keynote[k] = 0x4000u;
        if (WHITE_IX[k] >= 0 && WHITE_IX[k] < DAB_NPART) {
            if (set.track != WHITE_IX[k] + 1)
                track_pick(WHITE_IX[k] + 1);
            else
                keys_say();
        }
        return;
    }
    ui.held_keys &= ~(1u << k);              /* (a held key again: the key has its note now) */
    t = key_track(k < 12u, &m);
    if (ui.seq && set.track && !ui.save) {
        int dr = trk_drum(t), n = dabke_sound_drums(set.tsnd[t][0]);
        if (WHITE_IX[k] >= 0 && (dr || ui.skey < 0 || plat_ms() - ui.wat[ui.skey] < 180u)) {   /* a white key: a step. (A
                                              * note sound with a step held already for 180 ms: a note for that step,
                                              * below; sooner: a second step tapped) */
            step_key_down(t, WHITE_IX[k], k);
            return;
        }
        if (dr) {                            /* a black key, a drum sound: its drum (a step held: that step's drum) */
            int lane = BLACK_IX[k] % (n ? n : 1);
            ui.keynote[k] = 0x4000u;
            ui.tdrum[t] = (uint8_t)lane;
            if (ui.skey >= 0) {
                ui.wused |= (uint16_t)(1u << ui.skey);
                dabke_step(t, ui.sstep, lane, DAB_STEP_ONE);
                dabke_track_hit(t, lane, 0);
            } else
                dabke_track_hit(t, lane, rt == t);
            return;
        }
        given = ui.skey >= 0;                /* (a note sound, a step held: this key's note is the step's) */
    }
    if (m < DAB_NTRK && trk_drum(t)) {       /* a drum sound: its drums up the keys (recording: on its steps too) */
        int n = dabke_sound_drums(set.tsnd[t][0]);
        ui.keynote[k] = 0x4000u;             /* (nothing to let go) */
        ui.tdrum[t] = (uint8_t)(k % (uint32_t)(n ? n : 1));
        dabke_track_hit(t, (int)k, rt == t && !ui.save);
        return;
    }
    note = 53 + (int32_t)k + (m == DAB_NTRK ? 0 : 12 * set.oct) - (set.tsnd[t][0] == SF_BASS ? 24 : 0);
    if (note < 0 || note > 127)
        return;
    ui.keynote[k] = (uint16_t)(0x8000u | (uint32_t)m << 7 | (uint32_t)note);
    ui.vel[k] = (uint8_t)vel;
    if (given)
        step_give(t, (int)note);
    else
        chord_note(t, (int)note, k);
    dabke_key(given ? m | DAB_KEY_TRY : m, note, vel);
    if (note >= 28)                          /* (below: the MIDImix's buttons and lights, never sent as a key) */
        plat_midi_send(0x09u | 0x90u << 8 | (uint32_t)note << 16 | (uint32_t)vel << 24, TO_USB);
}

static void key_up(uint32_t k)
{
    uint16_t h = ui.keynote[k];
    if (!h)
        return;
    ui.keynote[k] = 0;
    if (h & 0x2000u) {                       /* (a step) */
        step_key_up(h & 15);
        return;
    }
    if (!(h & 0x8000u))
        return;
    dabke_key(h >> 7 & 15u, h & 0x7Fu, 0);
    if ((h & 0x7Fu) >= 28u)
        plat_midi_send(0x08u | 0x80u << 8 | (uint32_t)(h & 0x7Fu) << 16, TO_USB);
}

/* the mixer's parts: muted or not (the engine keeps a muted track silent, whatever its volume and its locks say) */
static void part_mute(int k, int on)
{
    char t[24] = "";
    k &= 7;
    if (!(set.mute >> k & 1u) == !on)
        return;
    set.mute = (uint8_t)(on ? set.mute | 1u << k : set.mute & ~(1u << k));
    dabke_mute(set.mute);
    changed();
    trk_name(k, t);
    app_say(t, on ? "MUTED" : "ON");
}

/* the MIDImix: what the main loop takes from the ring */
static void mix_take(uint32_t pkt)       /* the MIDImix: each strip a track (fader its volume, top knob its effect, middle
                                          * its filter, bottom its pitch, MUTE, REC ARM as REC for that track); the master
                                          * the scene fader; SOLO the hold; BANK the rhythms (both together: no top
                                          * sound). In step mode with a step held, a strip's knob or fader is locked on it */
{
    const dabke_status_t *s = dabke_status();
    uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu, i;
    int on = (st & 0xF0u) == 0x90u && d2;
    if ((st & 0x0Fu) != 0u)                  /* channel 1 only */
        return;
    if (!ui.mix_seen)
        ui.mix_all = 1;
    ui.mix_seen = 1;
    if ((st & 0xF0u) == 0xB0u) {
        if (d1 == MIX_MASTER_CC) {           /* the scene fader (FADE follows it) */
            if (step_held()) {
                lock_refuse("FADE");
                return;
            }
            master_set((int)d2);
            ui.fade = (uint8_t)(((int)d2 * 100 + 63) / 127);
            ui.fade_own = 0;
            return;
        }
        for (i = 0; i < 8u; i++) {
            int id;
            int32_t v, was;
            if (d1 < MIX_BASE[i] || d1 > MIX_BASE[i] + 3u)
                continue;
            id = d1 == MIX_BASE[i] + 3u ? MIX_FADER[i] : d1 == MIX_BASE[i] ? P_SEND + (int)i
                 : d1 == MIX_BASE[i] + 1u ? P_FILTER + (int)i : P_PITCH + (int)i;
            v = id == P_PITCH + (int)i ? pitch_from_cc((int)d2)   /* (the bottom knob: its steps as PITCH STEPS says) */
                                       : PAR[id].lo + ((int32_t)d2 * (PAR[id].hi - PAR[id].lo) + 63) / 127;
            if (step_held()) {               /* a step held: this control's value locked on it */
                step_lock(id, v);
                return;
            }
            {                                /* its own track's control, whatever track is picked: the keys stay where
                                              * they are while he rides another track's fader or filter. Said, once it is
                                              * plain that it moved alone */
                int rec = rec_strip(id);
                was = rec ? lk_now(id, 0) : par_eff(id);
                if (id == P_DRONE && ui.master < MASTER_TOP)
                    drone_up(v - set.par[id]);
                ctl_move(id, v);
                if (id != P_PITCH + (int)i || (rec ? v != was : (par_eff(id) != was || ui.master >= MASTER_TOP)))
                    cc_moved((int)d1, (int)i, id, rec ? v : par_eff(id));
            }
            return;
        }
        return;
    }
    if ((st & 0xF0u) != 0x90u && (st & 0xF0u) != 0x80u)
        return;
    if (d1 == MIX_SOLO) {                    /* HOLD */
        if (on)
            hold_press();
        return;
    }
    if (d1 == MIX_BANK_LEFT || d1 == MIX_BANK_RIGHT) {   /* the rhythms; both together: no top sound (when let go) */
        uint8_t bit = d1 == MIX_BANK_LEFT ? 1u : 2u;
        if (on) {
            if (ui.bank_down & ~bit)
                ui.bank_both = 1;
            ui.bank_down |= bit;
            return;
        }
        if (!(ui.bank_down & bit))
            return;
        ui.bank_down &= (uint8_t)~bit;
        if (ui.bank_both) {
            if (!ui.bank_down) {
                ui.bank_both = 0;
                top_clear();
            }
            return;
        }
        rhythm_step(bit == 2u ? 1 : -1);
        return;
    }
    for (i = 0; i < 8u; i++) {
        if (d1 == MIX_MUTE_NOTE(i)) {        /* a part off, and on again (its track is not picked) */
            if (on)
                part_mute((int)i, !(set.mute >> i & 1u));
            return;
        }
        if (d1 == MIX_RECARM_NOTE(i)) {      /* as REC, for its track, which it picks: recording, it stops now; else it
                                              * records when the button is let go; held a second instead: its last take
                                              * gone */
            if (on) {
                pick_track((int)i);
                if (s->rec_track == (int)i) {
                    dabke_track_rec(-1);
                    ui.mrec_down = 0;
                } else {
                    ui.mrec_trk = (uint8_t)i;
                    ui.mrec_down = 1;
                    ui.mrec_done = 0;
                    ui.mrec_at = plat_ms();
                }
            } else if (ui.mrec_down && ui.mrec_trk == i) {
                ui.mrec_down = 0;
                if (!ui.mrec_done)
                    track_rec((int)i);
            }
            return;
        }
    }
}

/* the MIDImix's lights say what their buttons say, steadily: none blinks. MUTE: lit while that part is muted, dark
 * while it plays. REC ARM: lit while its track records. SOLO: lit while notes are held. BANK LEFT and RIGHT: dark.
 * Every light again at once when a MIDImix first speaks or comes back, after any change and every 2 s: a few a frame,
 * only while the way out has room. */
static uint32_t mix_led_bits(void)
{
    const dabke_status_t *s = dabke_status();
    uint32_t b = set.mute;               /* MUTE: lit while its part is muted */
    if (s->rec_track >= 0)               /* REC ARM: lit while its track records (armed, waiting for its first key, too) */
        b |= 1u << (8u + (uint32_t)s->rec_track);
    if (s->hold)                         /* SOLO: lit while notes are held */
        b |= 1u << 18;
    return b;
}
static void mix_leds(void)               /* the main loop, once a frame */
{
    uint32_t b = mix_led_bits(), diff, i, sent = 0;
    ui.mix_leds = b;
    if (!ui.mix_seen)
        return;
    if (ui.mix_all || ms_now - ui.mix_resend >= 2000u) {   /* (at first, and every 2 s: every light again) */
        ui.mix_force = 0x7FFFFu;
        ui.mix_all = 0;
        ui.mix_resend = ms_now;
    }
    if ((b ^ ui.mix_sent) & ~(3u << 16))     /* a change (but the beat's tick): every light again */
        ui.mix_force = 0x7FFFFu;
    diff = (b ^ ui.mix_sent) | ui.mix_force;
    for (i = 0; i < 38u && diff && sent < 6u; i++) {   /* (the lights that changed first, then the rest) */
        uint32_t j = i % 19u, note = j < 8u ? MIX_MUTE_NOTE(j) : j < 16u ? MIX_RECARM_NOTE(j - 8u) : j == 16u ? MIX_BANK_LEFT
                                     : j == 17u ? MIX_BANK_RIGHT : MIX_SOLO;
        if (!(diff >> j & 1u) || (i < 19u && !((b ^ ui.mix_sent) >> j & 1u)))
            continue;
        if (plat_midi_out_room(TO_USB) < 4u)
            break;
        if (b >> j & 1u)
            plat_midi_send(0x09u | 0x90u << 8 | note << 16 | 127u << 24, TO_USB);
        else
            plat_midi_send(0x09u | 0x90u << 8 | note << 16, TO_USB);   /* dark: a note on at velocity 0 (the MIDImix
                                                                         * ignores a note off for its lights) */
        ui.mix_sent = (ui.mix_sent & ~(1u << j)) | (b & 1u << j);
        ui.mix_force &= ~(1u << j);
        diff &= ~(1u << j);
        sent++;
    }
}

static void seq_masks(void)              /* step mode, once a frame: the shown page's steps where something of the picked
                                          * track begins, and where a lock is, a bit each by white key */
{
    int t = picked(), P = page_steps(), n = trk_nsteps(t), from, w, i;
    ui.m_trig = ui.m_lock = 0;
    if (!ui.seq || !set.track)
        return;
    if (ui.spage_until && plat_ms() - ui.spage_until >= 400u)
        ui.spage_until = 0;
    if (ui.spage >= trk_pages(t) && !ui.spage_until)   /* (the track got shorter: its last page; not in the 0.4 s after a
                                              * second bar was asked for, while it is on its way) */
        ui.spage = (uint8_t)(trk_pages(t) - 1);
    from = ui.spage * P;
    for (w = 0; w < P && from + w < n; w++)
        if (step_trig(t, from + w))
            ui.m_trig |= (uint16_t)(1u << w);
    for (i = 0; i < DAB_NLOCK; i++) {
        uint32_t l = sq.lock[i];
        if (l && DAB_LK_TRK(l) == t && DAB_LK_STEP(l) >= from && DAB_LK_STEP(l) < from + P && DAB_LK_STEP(l) < n)
            ui.m_lock |= (uint16_t)(1u << (DAB_LK_STEP(l) - from));
    }
}
static void lights(void)
{
    const dabke_status_t *s = dabke_status();
    static const uint8_t PAGE_BTN[] = {B_HOME, B_SEL, B_FX, B_ENV, B_LFO, B_GLO};
    static const uint8_t WK[16] = PLAT_WHITE_KEYS;
    uint32_t b = ui.btn | 1u << (ui.save ? B_SAVE : PAGE_BTN[ui.page]), dk = 0, lit = ui.keys | ui.held_keys, mid = 0;
    if (s->running)
        b |= LED_PLAY_GREEN;
    if (set.arp)
        b |= 1u << B_ARP;
    if (ui.seq)                              /* SEQ: step mode */
        b |= 1u << B_SEQ;
    if (s->rec_track >= 0)                   /* REC: a track recording (waiting for the first key too) */
        b |= 1u << B_REC;
    if (ui.edit_held)
        b |= 1u << B_EDIT;
    if (ui.seq && set.track) {               /* step mode: the white keys are the page's steps. Lit: something begins
                                              * there. The second level, steadily: a lock there. Dim: the step playing.
                                              * The black keys: lit while played or held */
        int t = picked(), P = page_steps(), w, play = s->running ? (int)s->tstep[t] - ui.spage * P : -1;
        for (w = 0; w < 16; w++) {
            uint32_t bit = 1u << WK[w];
            lit &= ~bit;
            if (ui.m_lock >> w & 1u)
                mid |= bit;
            else if (ui.m_trig >> w & 1u)
                lit |= bit;
            if (w == play && w < P)
                dk |= bit;
        }
        lit &= ~dk;
        mid &= ~dk;
    } else if (ui.edit_held) {               /* EDIT held: the keys a quarter flat are lit dim */
        uint32_t k;
        for (k = 0; k < NKEYS; k++)
            if (set.ntune[(53 + k) % 12])
                dk |= 1u << k;
    } else if (!set.track && left_is_lower())
        dk = 0xFFFu & ~lit;
    if (ui.sel_down && ui.page == PG_HOME && !ui.save) {   /* SEL held: the first eight white keys are the tracks, lit at
                                              * the second level, the picked one fully; the other white keys dark */
        int w;
        for (w = 0; w < 16; w++) {
            uint32_t bit = 1u << WK[w];
            lit &= ~bit;
            mid &= ~bit;
            dk &= ~bit;
            if (w < DAB_NPART) {
                if (set.track == w + 1)
                    lit |= bit;
                else
                    mid |= bit;
            }
        }
    }
    ui.lit_keys = lit;                       /* (the held keys stay lit) */
    ui.dim_keys = dk;
    ui.mid_keys = mid;
    plat_lights(b, lit, 0, dk, 1);
    plat_lights_mid(0, mid);
}

/* The USB port, as COMBO has it. With USB PORT set to COMPUTER (as it starts) it is a computer's or a phone's device
 * and nothing more. With AUTO it takes turns: a device for USB_DEVICE_MS; if no computer has it by then, a host looking
 * for a MIDImix (or any USB MIDI controller) for USB_HOST_MS; finding none, a device again, and so on. A switch stops
 * the main loop for a moment, so a turn begins and ends only in a quiet moment, never under the fingers; and none
 * begins in the first USB_BOOT_MS, while a fault would still count as a failed start. */
#define USB_DEVICE_MS 5000u
#define USB_HOST_MS 7000u
#define USB_NOMIDI_MS 60000u
#define USB_QUIET_MS 1500u
#define USB_BOOT_MS 30000u
static void usb_watch(void)
{
    uint32_t now = plat_ms();
    int m, quiet = now - ui.touched_at >= USB_QUIET_MS;
    if (set.ch[C_USB] || (plat_usb_session() != 0 && plat_sysex_room() != 0)) {
        if (ui.usb_host) {
            plat_usb_host(0);
            ui.usb_host = 0;
        }
        ui.usb_since = now;
    } else if (!ui.usb_host) {
        if (now - ui.usb_since >= USB_DEVICE_MS && now >= USB_BOOT_MS && quiet) {
            plat_usb_host(1);
            ui.usb_host = 1;
            ui.usb_nomidi = 0;
            ui.usb_since = now;
        }
    } else if (plat_usb_host_state() == USBH_CONNECTED) {
        ui.usb_since = now;
    } else if (now - ui.usb_since >= (ui.usb_nomidi ? USB_NOMIDI_MS : USB_HOST_MS) && quiet) {
        plat_usb_host(0);
        ui.usb_host = 0;
        ui.usb_since = now;
    }
    m = plat_usb_host_msg();
    if (m == USBH_MSG_MIDI) {
        ui.mix_seen = ui.mix_all = 1;
        app_say("CONTROLLER", "READY");
    } else if (m == USBH_MSG_LOST) {
        ui.mix_seen = 0;                     /* (when it speaks again: every light again) */
        app_say("CONTROLLER", "UNPLUGGED");
    } else if (m == USBH_MSG_NOMIDI) {
        ui.usb_nomidi = 1;
    }
    if (plat_usb_lost()) {
        usb_lost_req = 1;
        ui.mix_seen = 0;
    }
}

static void input(void)
{
    uint32_t keys, btn = plat_buttons(), k, pmax = 0, ptrk = 0;
    int32_t e;
    int touch = TOUCH_ON();
    midi_pick = (int8_t)(set.track - 1);     /* (the notes in play the picked track too) */
    if (touch)
        touch_poll();                        /* the sensed keys' presses, velocities and pressure */
    keys = touch_keys();                     /* (plat_keys while the touch is off) */
    if (keys != ui.keys || btn != ui.btn)
        ui.touched_at = plat_ms();
    for (k = 0; k < NB; k++)                 /* buttons first: EDIT held before the keys it tunes */
        if ((btn >> k & 1u) && !(ui.btn >> k & 1u))
            press((int)k);
    if (ui.edit_held) {                      /* EDIT let go, nothing done with it: HOLD. Step mode, held a second with
                                              * no step held: all the picked track's locks removed */
        if (!(btn >> B_EDIT & 1u)) {
            ui.edit_held = 0;
            if (!ui.edit_used)
                hold_press();
            ui.ver++;
        } else if (ui.seq && set.track && !ui.edit_used && !ui.edit_done && !ui.wdown && plat_ms() - ui.edit_at >= 1000u) {
            ui.edit_done = ui.edit_used = 1;
            trk_words(picked(), lk_clear(picked(), -1, 0) ? ": ALL LOCKS REMOVED" : ": NO LOCKS");
        }
    }
    if (ui.sel_down && !(btn >> B_SEL & 1u)) {   /* SEL let go on the home page: the next track, unless a key picked one */
        ui.sel_down = 0;
        if (!ui.sel_used && ui.page == PG_HOME && !ui.save)
            pick_step(1);
        ui.ver++;
    }
    if (ui.arp_held && !(btn >> B_ARP & 1u)) {   /* ARP let go: on or off, unless it was used as a shift */
        ui.arp_held = 0;
        if (!ui.arp_used)
            arp_toggle();
        ui.ver++;
    }
    if (ui.rec_down) {                       /* REC let go: that track records. Held a second: its last take gone */
        if (!(btn >> B_REC & 1u)) {
            ui.rec_down = 0;
            if (!ui.rec_done)
                track_rec(ui.rec_trk);
        } else if (!ui.rec_done && plat_ms() - ui.rec_at >= 1000u) {
            ui.rec_done = 1;
            track_undo(ui.rec_trk);
        }
    }
    if (ui.seq_down) {                       /* SEQ: tapped, step mode on or off; held a second, every recording gone */
        if (!(btn >> B_SEQ & 1u)) {
            ui.seq_down = 0;
            if (!ui.seq_done)
                step_mode(!ui.seq);
        } else if (!ui.seq_done && plat_ms() - ui.seq_at >= 1000u) {
            ui.seq_done = 1;
            clear_all();
        }
    }
    if (ui.play_down) {                      /* PLAY while it runs: let go, everything stops; held 0.4 s, a fill-in */
        if (!(btn >> B_PLAY & 1u)) {
            ui.play_down = 0;
            if (!ui.play_done)
                dabke_run(0);
        } else if (!ui.play_done && plat_ms() - ui.play_at >= 400u) {
            ui.play_done = 1;
            dabke_fill(-1);
            say("FILL IN");
        }
    }
    if (ui.mrec_down && !ui.mrec_done && plat_ms() - ui.mrec_at >= 1000u) {   /* (a REC ARM held a second) */
        ui.mrec_done = 1;
        track_undo(ui.mrec_trk);
    }
    for (k = 0; k < NKEYS; k++) {
        uint32_t bit = 1u << k;
        if ((keys & bit) && !(ui.keys & bit))
            key_down(k, set.ch[C_TOUCH] ? (int)touch_vel(k) : 100);
        else if (!(keys & bit) && (ui.keys & bit))
            key_up(k);
        else if ((keys & bit) && set.ch[C_PRESS] && (ui.keynote[k] & 0x8000u)) {   /* a key with a note, on any
                                              * track: its pressure is that track's */
            uint32_t p = touch_pressure(k), m = ui.keynote[k] >> 7 & 15u;
            if (p > pmax)
                pmax = p;
            if (p >= 10u)
                ptrk |= 1u << (m < DAB_NTRK ? m : PART_LEFT);
        }
    }
    ui.keys = keys;
    if (pmax < 10u)                          /* (a key at rest reads a little: not a bend) */
        pmax = 0;
    if (touch)                               /* (the readout's peaks) */
        for (k = 0; k < NKEYS; k++)
            if ((keys >> k & 1u) && touch_pressure(k) > ui.raw_peak)
                ui.raw_peak = touch_pressure(k);
    if (pmax > ui.cnt_peak)
        ui.cnt_peak = (uint8_t)pmax;
    if (pmax && ptrk != ui.press_trk) {      /* (another track leant on: said even when the pressure is the same) */
        ui.press_trk = (uint8_t)ptrk;
        ui.press_sent = (uint8_t)pmax;
        dabke_pressure((int)ptrk, (float)pmax * (1.0f / 127.0f));
    } else if ((uint8_t)pmax != ui.press_sent && (pmax == 0 || pmax > ui.press_sent + 1u || pmax + 1u < ui.press_sent)) {
        ui.press_sent = (uint8_t)pmax;
        dabke_pressure(pmax ? (int)ptrk : 0, (float)pmax * (1.0f / 127.0f));
    }
    if (btn != ui.btn)
        ui.ver++;
    ui.btn = btn;
    for (k = 0; k < NE; k++)
        if ((e = plat_enc((int)k)) != 0) {
            ui.touched_at = plat_ms();
            turn((int)k, (int)e);
        }
    while (mix_r != mix_w) {
        uint32_t pkt = mix_ring[mix_r % MIX_RING];
        mix_r++;
        mix_take(pkt);
    }
    if (ui.cc_win_at && plat_ms() - ui.cc_win_at >= 120u) {   /* the MIDImix's controls moved in the last 120 ms: one alone,
                                              * said, and said on as it moves; more (a SEND ALL, even split over frames):
                                              * nothing said */
        ui.cc_win_at = 0;
        if (ui.cc_n == 1u) {
            ui.cc_live = 1;
            strip_say_at(ui.cc_strip, ui.cc_pid, ui.cc_val);
        }
    }
    if (ui.scene_dirty) {                    /* the scene fader moved: its controls again, once */
        int c;
        ui.scene_dirty = 0;
        for (c = 0; c < 4 * DAB_NPART; c++)
            if (IS_KNOB(ctl_id(c)))
                par_send(ctl_id(c));
    }
    lights();
}

void app_overload(void) { dabke_overload(); }

int app_sysex(const uint8_t *p, uint32_t n)
{
    (void)p, (void)n;
    return 0;
}

void app_input_only(void) { input(); }

/* ================================================================ drawing ===== */
/* The screen is drawn in strips (the canvas is at most 240 x 144), each in screen coordinates: the static parts when
 * something shown changes, the moving ones (the scene, the knobs' drawings, the wave) 25 to 30 times a second. */
static int32_t reg_y0, reg_y1;
static int32_t clip_x0, clip_y0, clip_x1 = 240, clip_y1 = 240;

static void clip_set(int32_t x, int32_t y, int32_t w, int32_t h)
{
    clip_x0 = x;
    clip_y0 = y;
    clip_x1 = x + w;
    clip_y1 = y + h;
}
static void clip_all(void) { clip_set(0, 0, 240, 240); }
static int in_strip(int32_t y0, int32_t y1) { return y1 > reg_y0 && y0 < reg_y1; }

static int32_t ri(float v) { return (int32_t)fm_floorf(v + 0.5f); }

static void px(int32_t x, int32_t y, uint16_t c)
{
    if (x < clip_x0 || x >= clip_x1 || y < clip_y0 || y >= clip_y1)
        return;
    cv_pset(x, y, c);
}

static void box(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    int32_t x1 = x + w, y1 = y + h;
    if (x < clip_x0)
        x = clip_x0;
    if (y < clip_y0)
        y = clip_y0;
    if (x1 > clip_x1)
        x1 = clip_x1;
    if (y1 > clip_y1)
        y1 = clip_y1;
    if (x1 > x && y1 > y)
        cv_rect(x, y, x1 - x, y1 - y, c);
}

static void boxf(float x, float y, float w, float h, uint16_t c)
{
    int32_t x0 = ri(x), y0 = ri(y), x1 = ri(x + w), y1 = ri(y + h);
    if (w <= 0.05f || h <= 0.05f)
        return;
    if (x1 <= x0)
        x1 = x0 + 1;
    if (y1 <= y0)
        y1 = y0 + 1;
    box(x0, y0, x1 - x0, y1 - y0, c);
}

static void seg(float fx0, float fy0, float fx1, float fy1, uint16_t c, int lw)
{
    int32_t x0 = ri(fx0), y0 = ri(fy0), x1 = ri(fx1), y1 = ri(fy1);
    int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int32_t dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy, guard = 1000, steep = -dy > dx;
    while (guard--) {
        int32_t e2 = 2 * err;
        px(x0, y0, c);
        if (lw >= 2)
            steep ? px(x0 + 1, y0, c) : px(x0, y0 + 1, c);
        if (lw >= 3)
            steep ? px(x0 - 1, y0, c) : px(x0, y0 - 1, c);
        if (x0 == x1 && y0 == y1)
            break;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static float pl_x, pl_y;
static int pl_n, pl_w;
static uint16_t pl_c;
static void pl_start(uint16_t c, int lw)
{
    pl_n = 0;
    pl_c = c;
    pl_w = lw;
}
static void pl_to(float x, float y)
{
    if (pl_n++)
        seg(pl_x, pl_y, x, y, pl_c, pl_w);
    pl_x = x;
    pl_y = y;
}

static void disc(float cx, float cy, float r, uint16_t c)
{
    int32_t y = ri(cy - r - 0.5f), y1 = ri(cy + r + 0.5f);
    if (r < 1.0f) {
        box(ri(cx - 0.5f), ri(cy - 0.5f), 1, 1, c);
        return;
    }
    for (; y <= y1; y++) {
        float dy = (float)y + 0.5f - cy, hw2 = r * r - dy * dy, hw;
        int32_t x0, x1;
        if (hw2 < 0.0f)
            continue;
        hw = fm_sqrtf(hw2);
        x0 = ri(cx - hw);
        x1 = ri(cx + hw);
        if (x1 <= x0)
            x1 = x0 + 1;
        box(x0, y, x1 - x0, 1, c);
    }
}

static void arc(float cx, float cy, float r, float a0, float a1, uint16_t c, int lw)
{
    int k, n = (int)(r * fm_fabsf(a1 - a0) * 0.6f) + 6;
    pl_start(c, lw);
    for (k = 0; k <= n; k++) {
        float a = a0 + (a1 - a0) * (float)k / (float)n;
        pl_to(cx + fm_cosf(a) * r, cy + fm_sinf(a) * r);
    }
}

static void circ(float cx, float cy, float r, uint16_t c, int lw) { arc(cx, cy, r, 0.0f, FM_TWO_PI, c, lw); }

static void ellipse(float cx, float cy, float rx, float ry, uint16_t c)   /* filled, upright: a row at a time */
{
    int32_t y = ri(cy - ry - 0.5f), y1 = ri(cy + ry + 0.5f);
    for (; y <= y1; y++) {
        float dy = ((float)y + 0.5f - cy) / ry, w2 = 1.0f - dy * dy, hw;
        int32_t x0, x1;
        if (w2 < 0.0f)
            continue;
        hw = rx * fm_sqrtf(w2);
        x0 = ri(cx - hw);
        x1 = ri(cx + hw);
        if (x1 <= x0)
            x1 = x0 + 1;
        box(x0, y, x1 - x0, 1, c);
    }
}

static void frame(float x, float y, float w, float h, uint16_t c)
{
    int32_t x0 = ri(x), y0 = ri(y), x1 = ri(x + w), y1 = ri(y + h);
    box(x0, y0, x1 - x0 + 1, 1, c);
    box(x0, y1, x1 - x0 + 1, 1, c);
    box(x0, y0, 1, y1 - y0 + 1, c);
    box(x1, y0, 1, y1 - y0 + 1, c);
}

static uint16_t mixc(uint16_t a, uint16_t b, float f)
{
    int32_t ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31, br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    int32_t q = ri(f * 256.0f);
    q = clamp(q, 0, 256);
    return (uint16_t)(((ar + ((br - ar) * q >> 8)) << 11) | ((ag + ((bg - ag) * q >> 8)) << 5) |
                      (ab + ((bb - ab) * q >> 8)));
}

static uint32_t rnd_s = 0x9E3779B9u;
static float rndf(void)
{
    rnd_s = rnd_s * 1664525u + 1013904223u;
    return (float)(rnd_s >> 8) * (1.0f / 16777216.0f);
}

/* text at a baseline (its pixel size picks the face), in capitals; max: the widest it may be (a smaller face, then cut) */
static const felucca_font_t *const FACE[5] = {&FONT_T, &FONT_XS, &FONT_B, &FONT_M, &FONT_L};
static const uint8_t ASC[5] = {10, 12, 13, 19, 26};
static int32_t T(const char *s, int32_t x, int32_t yb, int size, uint16_t c, int align, int32_t maxw)
{
    char u[48];
    int f = size <= 10 ? 0 : size <= 13 ? 1 : size <= 17 ? 2 : size <= 24 ? 3 : 4, n = 0, lo = 0;
    int32_t w, top;
    while (s[n] && n < 47) {             /* capitals, but a note's flat ("Bb") stays a small b */
        int flat = s[n] == 'b' && n > 0 && s[n - 1] >= 'A' && s[n - 1] <= 'G' && (s[n + 1] < 'a' || s[n + 1] > 'z');
        u[n] = flat ? 'b' : (char)up_char((uint8_t)s[n]);
        n++;
    }
    u[n] = 0;
    w = text_w(FACE[f], u);
    while (maxw && w > maxw && f > lo)
        w = text_w(FACE[--f], u);
    if (FACE[f]->last < 'b')             /* (a face of capitals only) */
        for (n = 0; u[n]; n++)
            u[n] = (char)up_char((uint8_t)u[n]);
    while (maxw && w > maxw && n > 1) {
        u[--n] = 0;
        w = text_w(FACE[f], u);
    }
    if (align == 1)
        x -= w / 2;
    else if (align == 2)
        x -= w;
    top = yb - ASC[f];
    if (!in_strip(top, top + FACE[f]->h))
        return x + w;
    return cv_text(x, top, FACE[f], u, c);
}

/* the half-flat sign (a flat with a stroke through its stem), as maqam music writes it */
static void halfflat(int32_t x, int32_t yb, int32_t h, uint16_t c)
{
    float top = (float)(yb - h), mid = (float)yb - (float)h * 0.38f;
    seg((float)x, top, (float)x, (float)yb, c, 2);
    arc((float)x + 1.0f, mid + (float)h * 0.18f, (float)h * 0.22f, -1.4f, 1.5f, c, 2);
    seg((float)x - 3.0f, top + (float)h * 0.38f, (float)x + 3.0f, top + (float)h * 0.22f, c, 1);
}

/* ---- the screens ---- */
static float fmodp(float a, float m) { return a - fm_floorf(a / m) * m; }   /* a % m, never below 0 */

static int arp_panel_up(void) { return ui.arp_held && (ui.arp_used || ms_now - ui.arp_at >= 250u); }
static void panel_head(void);
static void seq_head(void);
static void lock_name(int t, int id, char *b)   /* a locked knob by name, as seen from track t: its own strip's "FILTER",
                                                  * another track's "T2 FILTER", any other knob's own word */
{
    int k, own = -1;
    char n[4] = {'T', '1', ' ', 0};
    b[0] = 0;
    for (k = 0; k < DAB_NPART; k++)
        if (id == P_SEND + k || id == P_FILTER + k || id == P_PITCH + k || id == PART_PAR[k])
            own = k;
    if (own < 0 || (id >= P_LOWEQ && id <= P_LOWEQ_LAST)) {
        text_cat(b, PAR[id].word);
        return;
    }
    if (own != t) {
        n[1] = (char)('1' + own);
        text_cat(b, n);
    }
    text_cat(b, strip_word(id));
}
static int step_words(int t, int st, char *a, char *b2)   /* the held step: "STEP 5: FILTER DARK 30, PITCH UP 2" (a
                                                            * second line when the first is full; each 72 characters:
                                                            * a line takes no more once it is past 26). How many locks */
{
    char w[32], *o = a;
    int i, n = 0;
    step_name(st, a);
    b2[0] = 0;
    for (i = 0; i < DAB_NLOCK; i++) {
        uint32_t l = sq.lock[i];
        int len = 0;
        if (!l || DAB_LK_TRK(l) != t || DAB_LK_STEP(l) != st)
            continue;
        while (o[len])
            len++;
        if (o == a && len > 26) {            /* (the first line full: on to the second) */
            o = b2;
            len = 0;
        }
        if (len > 26)
            break;
        text_cat(o, o == a && !n ? ": " : len ? ", " : "");
        if (DAB_LK_ID(l) == DAB_LK_SOUND) {  /* (its sound: "SOUND CLAY") */
            text_cat(o, "SOUND ");
            sound_label(SF_KICK + (DAB_LK_VAL(l) >> 4 & 3), DAB_LK_VAL(l) & 15, w);
        } else {
            lock_name(t, DAB_LK_ID(l), w);
            text_cat(o, w);
            text_cat(o, " ");
            value_words(DAB_LK_ID(l), DAB_LK_VAL(l), w);
        }
        text_cat(o, w);
        n++;
    }
    if (!n)
        text_cat(a, ": NO LOCKS");
    return n;
}
static void header(void)
{
    if (!in_strip(0, 32))
        return;
    if (step_held() && !ui.save && !arp_panel_up()) {   /* a step held, on any page: its locks named here, whatever was
                                              * said a moment ago ("STEP 5: REVERB 37") */
        char a[72], b2[72];
        step_words(picked(), ui.sstep, a, b2);
        T(a, 8, 22, 16, K_AMB, 0, 224);
    } else if (ms_now < ui.msg_until) {
        T(ui.msg, 8, 22, 16, K_AMB, 0, 224);
    } else if (ui.page == PG_HOME && !ui.save && !arp_panel_up()) {
        if (ui.seq)
            seq_head();                      /* the step view: "STEPS", the bar shown */
        else
            panel_head();                    /* the home page: the sound, the maqam, the tempo */
    } else {
        char info[32] = "", t[8], ttl[32] = "";
        if (ui.page == PG_FX && ui.row[0] >= MIX_ROW) {   /* the mixer's rows: "MIX: TRACK 1" */
            text_cat(ttl, "MIX: ");
            trk_name((ui.row[0] - MIX_ROW) % DAB_NPART, ttl + 5);
        } else
            text_cat(ttl, ui.save ? "KEEP THE SOUND" : PAGE_TITLE[ui.page]);
        if (!ui.save && set.track && (ui.page == PG_ENV || ui.page == PG_LFO)) {   /* (the picked track's own sound:
                                              * "SOUND: TRACK 3") */
            char tn[12];
            trk_name(set.track - 1, tn);
            text_cat(ttl, ": ");
            text_cat(ttl, tn);
        }
        if (arp_panel_up()) {                /* ARP held: its panel */
            ttl[0] = 0;
            text_cat(ttl, set.arp ? "ARPEGGIO ON" : "ARPEGGIO OFF");
        }
        T(ttl, 8, 22, 16, K_WH, 0, 170);
        if (ui.save)
            ;
        else if (arp_panel_up())
            text_cat(info, "SEL: THE OTHER FOUR");
        else if (ui.page == PG_SEL) {
            text_cat(info, ui.edit_held ? "KEYS: FLAT OR BACK" : "EDIT + KEY: FLAT");
        } else if (ui.page >= PG_FX) {
            int pg = ui.page - PG_FX;
            int_text(ui.row[pg] + 1, t);
            text_cat(info, t);
            text_cat(info, " / ");
            int_text(PAGE_ROWS[pg], t);
            text_cat(info, t);
        }
        T(info, 232, 21, 11, K_GR, 2, 0);
    }
    box(0, 30, 240, 1, K_DIM);
}

static void save_page(void)
{
    int i;
    T("SELECT SAYS WHERE", 120, 58, 13, K_GR, 1, 0);
    T("SAVE KEEPS IT    HOME LEAVES", 120, 222, 11, K_GR, 1, 0);
    for (i = 0; i < NKEPT; i++) {
        int32_t x = 8 + (i % 4) * 57, y = 72 + (i / 4) * 66, on = i == ui.slot;
        char t[2] = {(char)('1' + i), 0};
        box(x, y, 52, 58, on ? K_AMB : K_BOX);
        T(t, x + 26, y + 34, 24, on ? K_BLACK : K_WH, 1, 0);
        T(kept.used[i] ? "USED" : "FREE", x + 26, y + 51, 9, on ? K_BLACK : K_GR, 1, 0);
    }
}

#include "scene.c"

#include "panel.c"

static void home_page(void)              /* the singer over the eight tracks' lanes; in step mode: the step view */
{
    if (ui.seq) {
        seq_view();
        return;
    }
    if (in_strip(32, 144))
        scene(0, 32, 240, 112);
    if (in_strip(PN_Y, 240))
        home_panel();
}

/* the maqam page: the twelve keys, which are a quarter flat, the maqam and its root */
static void maqam_page(void)
{
    static const uint8_t WK[7] = {0, 2, 4, 5, 7, 9, 11}, BK[5] = {1, 3, 6, 8, 10}, BX[5] = {1, 2, 4, 5, 6};
    uint32_t sc = dabke_maqam_scale(set.ch[C_MAQAM], set.ch[C_ROOT]);
    char b[40], t[12];
    int i;
    if (in_strip(32, 76)) {
        maqam_name(b, 0);
        T(b, 8, 58, 24, K_AMB, 0, 160);
        b[0] = 0;
        text_cat(b, "ON ");
        text_cat(b, NOTE_NAME[set.ch[C_ROOT] % 12]);
        {
            int fl = set.ntune[set.ch[C_ROOT] % 12] < 0;
            T(b, fl ? 220 : 232, 58, 16, K_WH, 2, 0);
            if (fl)                              /* the tonic itself a quarter flat */
                halfflat(225, 58, 13, K_WH);
        }
        T(maqam_as_set() ? "AS THE MAQAM HAS IT" : "CHANGED BY HAND", 8, 72, 9, maqam_as_set() ? K_GR : K_AMB, 0, 0);
    }
    if (in_strip(78, 160)) {                 /* one octave, C to B, each key with its cents */
        for (i = 0; i < 7; i++) {
            int pc = WK[i], fl = set.ntune[pc] != 0, in = sc >> pc & 1u, cur = pc == ui.cur;
            int32_t x = 8 + i * 32;
            uint16_t c = fl ? K_AMB : in ? K_WH : K_GR;
            box(x, 80, 30, 76, c);
            if (cur) {
                box(x - 2, 78, 34, 3, K_RED);
                box(x - 2, 155, 34, 3, K_RED);
                box(x - 2, 78, 3, 80, K_RED);
                box(x + 29, 78, 3, 80, K_RED);
            }
            T(NOTE_NAME[pc], x + 15, 148, 13, K_BLACK, 1, 0);
            if (set.ntune[pc]) {
                int_text(set.ntune[pc], t);
                T(t, x + 15, 132, 11, K_BLACK, 1, 28);
            }
        }
        for (i = 0; i < 5; i++) {
            int pc = BK[i], fl = set.ntune[pc] != 0, in = sc >> pc & 1u, cur = pc == ui.cur;
            int32_t x = 8 + BX[i] * 32 - 11;
            box(x, 80, 22, 44, fl ? K_AMB : in ? RGB(110, 100, 92) : K_BLACK);
            frame((float)x, 80.0f, 21.0f, 43.0f, K_GR);
            if (cur) {
                frame((float)x - 2.0f, 78.0f, 25.0f, 47.0f, K_RED);
                frame((float)x - 1.0f, 79.0f, 23.0f, 45.0f, K_RED);
            }
            T(NOTE_NAME[pc], x + 11, 99, 11, fl ? K_BLACK : K_WH, 1, 20);
            if (set.ntune[pc]) {
                int_text(set.ntune[pc], t);
                T(t, x + 11, 117, 11, K_BLACK, 1, 21);
            }
        }
    }
    if (in_strip(160, 240)) {                /* the scale written out, the knobs */
        int32_t x = 8;
        int k, r = set.ch[C_ROOT];
        for (k = 0; k < 12; k++) {
            int pc = (r + k) % 12;
            if (!(sc >> pc & 1u))
                continue;
            if (set.ntune[pc] < 0 && set.ntune[pc] > -100) {   /* a quarter flat: its letter and the half-flat sign */
                x = T(NOTE_NAME[pc], x, 184, 16, K_AMB, 0, 0);
                halfflat(x + 4, 184, 13, K_AMB);
                x += 18;
            } else
                x = T(NOTE_NAME[pc], x, 184, 16, K_WH, 0, 0) + 8;
            if (x > 226)
                break;
        }
        {
            static const char *const LAB[4] = {"K1 CENTS", "K2 MAQAM", "K3 ROOT", "K4 DEPTH"};
            for (k = 0; k < 4; k++) {
                int32_t bx = 4 + k * 59;
                box(bx, 196, 55, 40, K_BOX);
                T(LAB[k], bx + 4, 210, 9, K_GR, 0, 50);
                b[0] = 0;
                if (k == 0) {
                    text_cat(b, NOTE_NAME[ui.cur]);
                    text_cat(b, " ");
                    int_text(set.ntune[ui.cur], t);
                    text_cat(b, t);
                } else if (k == 1)
                    maqam_name(b, 0);
                else if (k == 2)
                    text_cat(b, NOTE_NAME[set.ch[C_ROOT] % 12]);
                else {
                    int_text(set.qdepth, t);
                    text_cat(b, t);
                    text_cat(b, " CT");
                }
                T(b, bx + 4, 229, 13, K_AMB, 0, 50);
            }
        }
    }
}

/* ---- the knobs' little drawings, in the box's own coordinates ---- */
static float IX, IY, IW, IH;
#define I_MID (IH * 0.5f)
#define I_BASE (IH - 3.0f)
static void iline(float x0, float y0, float x1, float y1, uint16_t c, int lw) { seg(IX + x0, IY + y0, IX + x1, IY + y1, c, lw); }
static void ito(float x, float y) { pl_to(IX + x, IY + y); }
static void idot(float x, float y, float r, uint16_t c) { disc(IX + x, IY + y, r, c); }
static void iring(float x, float y, float r, uint16_t c, int lw) { circ(IX + x, IY + y, r, c, lw); }
static void ibar(float bx, float bh, uint16_t c, float bw)
{
    if (bh > 0.3f)
        boxf(IX + bx, IY + IH - 2.0f - bh, bw, bh < 1.0f ? 1.0f : bh, c);
}
static void irect(float x, float y, float w, float h, uint16_t c) { boxf(IX + x, IY + y, w, h, c); }
static void iwave(float amp, float cyc, float ph, uint16_t c, float clip)
{
    float i, th = clip > 0.05f ? fm_tanhf(clip) : 1.0f;
    pl_start(c, 1);
    for (i = 0.0f; i <= IW; i += 2.0f) {
        float q = fm_sinf(i / IW * cyc * FM_TWO_PI + ph);
        if (clip > 0.05f)
            q = fm_tanhf(q * clip) / th;
        ito(i, IH * 0.5f - q * amp);
    }
}
static void ilowpass(float cut, float res)
{
    float i, cx = 5.0f + cut * (IW - 16.0f);
    pl_start(K_WH, 1);
    for (i = 0.0f; i <= IW; i += 2.0f) {
        float d = (i - cx) / 6.0f, f = d < 0.0f ? 0.0f : fm_minf(1.0f, d * d * 0.2f + d * 0.2f);
        ito(i, 16.0f + f * (IH - 22.0f) - res * 13.0f * fm_expf(-d * d * 0.5f));
    }
    idot(cx, 16.0f - res * 13.0f, 2.5f, K_AMB);
}
static void idecay(float len, uint16_t c)
{
    float i;
    pl_start(c, 1);
    for (i = 0.0f; i <= IW; i += 2.0f)
        ito(i, 6.0f + (IH - 10.0f) * (1.0f - fm_expf(-i / (3.0f + len * 30.0f))));
}
static void ilevel(float v, uint16_t c)      /* a fader: the value as a bar and a knob on it */
{
    irect(IW * 0.5f - 2.0f, 6.0f, 4.0f, IH - 12.0f, K_DIM);
    irect(IW * 0.5f - 2.0f, 6.0f + (IH - 12.0f) * (1.0f - v), 4.0f, (IH - 12.0f) * v, c);
    irect(IW * 0.5f - 10.0f, 3.0f + (IH - 12.0f) * (1.0f - v), 20.0f, 6.0f, K_WH);
}
static void iorn(int o, float t, uint16_t c)  /* an ornament's line: the pitch over a note's start */
{
    static const int8_t SH[ORN_COUNT][8] = {{0, 0, 0, 0, 0, 0, 0, 0}, {0, 1, 0, 0, 0, 0, 0, 0}, {1, 0, -1, 0, 0, 0, 0, 0},
                                            {-1, -1, 0, 0, 0, 0, 0, 0}, {0, 1, 0, 1, 0, 1, 0, 1}, {0, 1, 0, 1, 0, 1, 0, 1},
                                            {0, 1, 0, -1, 0, 0, 1, 0}};
    int k;
    pl_start(c, 2);
    for (k = 0; k < 8; k++) {
        float x0 = 3.0f + (float)k * (IW - 6.0f) / 8.0f, x1 = x0 + (IW - 6.0f) / 8.0f, y = I_MID - (float)SH[o][k] * 12.0f;
        if (o == ORN_SCOOP && k < 2)
            y = I_MID + 12.0f - (float)k * 6.0f;
        if (o == ORN_SHAKE)
            y = I_MID - 6.0f - 6.0f * fm_sinf((float)k * 1.6f + t * 6.0f);
        ito(x0, y);
        ito(x1, y);
    }
}

static void icon(int id, float x, float y, float w, float h, float t)
{
    float v, mid, base;
    uint16_t hot;
    int k, c = id >= K_CH && id < K_CH + NCH ? id - K_CH : -1;
    if (id == K_TFILT || id == K_TVOL)       /* (the picked track's filter and volume: as the mixer's rows draw them) */
        id = strip_id(id);
    v = IS_KNOB(id) ? par_norm(id) : 0.0f;
    hot = v > 0.01f ? K_AMB : K_WH;
    IX = x;
    IY = y;
    IW = w;
    IH = h;
    mid = I_MID;
    base = I_BASE;
    switch (id) {
    case P_ECHO: {
        float fbk = (float)set.par[P_ECHOFB] / 100.0f, dt = fm_maxf(0.06f, par_value(P_ECHOTIME) * 0.001f);
        int hop = (int)fmodp(t / dt, 6.0f);
        for (k = 0; k < 6; k++)
            ibar(2.0f + (float)k * 9.0f, (h - 8.0f) * (k ? v * fm_powf(fm_maxf(fbk, 0.05f), (float)(k - 1)) * 0.85f : 1.0f),
                 k && k == hop ? K_AMB : k ? K_GR : K_WH, 5.0f);
        return;
    }
    case P_ECHOTIME: {
        float gap = 8.0f + v * (w - 20.0f), ph = fmodp(t / fm_maxf(0.06f, par_value(P_ECHOTIME) * 0.001f), 2.0f);
        float pos = ph < 1.0f ? ph : 2.0f - ph;
        ibar(4.0f, h - 10.0f, K_WH, 3.0f);
        ibar(4.0f + gap, h - 10.0f, K_WH, 3.0f);
        idot(5.5f + pos * gap, mid, 3.0f, K_AMB);
        return;
    }
    case P_ECHOFB:
        for (k = 0; k < 7; k++)
            ibar(2.0f + (float)k * 7.5f, (h - 8.0f) * fm_powf(0.12f + v * 0.8f, (float)k), k ? K_AMB : K_WH, 4.0f);
        return;
    case P_REVERB:
        idot(4.0f, base - 2.0f, 2.5f, K_WH);
        for (k = 0; k < 4; k++) {
            float f = fmodp(t * 0.35f + (float)k / 4.0f, 1.0f);
            arc(x + 4.0f, y + base - 2.0f, 4.0f + f * (w - 6.0f), -1.5708f, 0.0f,
                mixc(K_KBG, K_AMB, (1.0f - f) * (0.12f + 0.88f * v)), 1);
        }
        return;
    case P_TONE:
    case P_BRIGHT:
    case P_KITTONE:
        ilowpass(v, id == P_TONE ? par_norm(P_BODY) * 0.4f : 0.0f);
        return;
    case P_BASSTONE:
        ilowpass(v, 0.3f);
        return;
    case P_BODY: {                           /* a formant: the body's hump */
        float cx = 8.0f + v * (w - 16.0f), i;
        pl_start(K_AMB, 1);
        for (i = 0.0f; i <= w; i += 2.0f)
            ito(i, base - 4.0f - (h - 18.0f) * fm_expf(-(i - cx) * (i - cx) / 60.0f));
        return;
    }
    case P_DRIVE:
    case P_BUZZ:
        iwave(h * 0.3f, 1.5f, t * 2.5f, hot, v * 7.0f);
        return;
    case P_DETUNE:                           /* two pipes: two waves drifting apart */
        iwave(h * 0.22f, 1.5f, t * 1.6f, K_WH, 0.0f);
        iwave(h * 0.22f, 1.5f + v * 0.6f, t * 1.6f + v * 1.4f * fm_sinf(t * (0.5f + v * 4.0f)), K_AMB, 0.0f);
        return;
    case P_BREATH:
    case P_HISS: {
        int n = 3 + ri(v * 70.0f);
        for (k = 0; k < n; k++)
            irect(rndf() * w, 4.0f + rndf() * (h - 8.0f), 1.5f, 1.5f, v > 0.01f ? K_AMB : K_GR);
        return;
    }
    case P_CHORUS:
        iwave(h * 0.22f, 1.5f, t * 1.6f, K_WH, 0.0f);
        if (v > 0.01f) {
            float o2 = v * 1.1f * (0.6f + 0.4f * fm_sinf(t * 0.9f));
            iwave(h * 0.22f, 1.5f, t * 1.6f + o2, K_AMB, 0.0f);
            iwave(h * 0.22f, 1.5f, t * 1.6f - o2, K_AMB, 0.0f);
        }
        return;
    case P_PA: {                             /* a horn speaker on a pole, its sound going out */
        iline(10.0f, base, 10.0f, mid, K_GR, 2);
        irect(4.0f, mid - 6.0f, 12.0f, 12.0f, K_WH);
        for (k = 0; k < 3; k++) {
            float f = fmodp(t * 1.2f + (float)k / 3.0f, 1.0f);
            arc(x + 16.0f, y + mid, 4.0f + f * (w - 20.0f), -0.7f, 0.7f, mixc(K_KBG, K_AMB, (1.0f - f) * (0.15f + 0.85f * v)), 2);
        }
        return;
    }
    case P_WOW: {
        float a = t * 2.2f + v * 1.2f * fm_sinf(t * 3.4f), i;
        int s;
        for (s = 0; s < 2; s++) {
            float cx = s ? w - 14.0f : 14.0f;
            iring(cx, mid + 6.0f, 11.0f, K_WH, 1);
            for (k = 0; k < 3; k++) {
                float b = a + (float)k * 2.0944f;
                iline(cx, mid + 6.0f, cx + fm_cosf(b) * 9.0f, mid + 6.0f + fm_sinf(b) * 9.0f, K_GR, 1);
            }
        }
        pl_start(hot, 1);
        for (i = 14.0f; i <= w - 14.0f; i += 2.0f)
            ito(i, mid - 7.0f + fm_sinf(i * 0.5f + t * 6.0f) * v * 5.0f);
        return;
    }
    case P_AGE: {
        int n = ri(v * 14.0f), tk = (int)fm_floorf(t * 7.0f);
        ilowpass(1.0f - v * 0.7f, 0.0f);
        for (k = 0; k < n; k++)
            irect((float)((k * 37 + tk * 13) % (int)w), 6.0f + (float)((k * 53) % (int)(h - 12.0f)), 1.5f, 1.5f, K_GR);
        return;
    }
    case P_ATTACK:
        pl_start(hot, 1);
        ito(0.0f, base);
        ito((0.03f + v * 0.62f) * w, 8.0f);
        ito(w - 12.0f, 8.0f);
        ito(w, base);
        return;
    case P_RELEASE:
        pl_start(K_AMB, 1);
        ito(0.0f, base);
        ito(3.0f, 8.0f);
        ito(12.0f, 8.0f);
        ito(12.0f + (0.08f + v * 0.92f) * (w - 13.0f), base);
        return;
    case P_GLIDE:                            /* two notes and the slide between them */
        iline(2.0f, base - 6.0f, 14.0f, base - 6.0f, K_WH, 2);
        iline(14.0f, base - 6.0f, 14.0f + 2.0f + v * (w - 32.0f), 10.0f, K_AMB, 2);
        iline(14.0f + 2.0f + v * (w - 32.0f), 10.0f, w - 2.0f, 10.0f, K_WH, 2);
        return;
    case P_KITDECAY:
    case P_BASSDECAY:
        idecay(v, K_AMB);
        return;
    case P_TRANSPOSE:
    case P_TUNE: {
        float a = (v - 0.5f) * 1.7f;
        for (k = -2; k <= 2; k++)
            idot(w / 2.0f + fm_sinf((float)k * 0.42f) * (h - 6.0f), base - fm_cosf((float)k * 0.42f) * (h - 6.0f), 1.2f, K_GR);
        iline(w / 2.0f, base, w / 2.0f + fm_sinf(a) * (h - 12.0f), base - fm_cosf(a) * (h - 12.0f),
              fm_fabsf(v - 0.5f) > 0.01f ? K_AMB : K_WH, 2);
        idot(w / 2.0f, base, 3.0f, K_WH);
        return;
    }
    case P_VIBRATE:
        iwave(h * 0.24f, 1.0f + v * 3.0f, t * par_value(P_VIBRATE) * 1.2f, K_AMB, 0.0f);
        return;
    case P_VIBDEPTH:
        iwave(2.0f + v * h * 0.38f, 2.0f, t * par_value(P_VIBRATE) * 1.2f, hot, 0.0f);
        return;
    case P_VIBDELAY: {
        float i, d = 4.0f + v * (w - 16.0f);
        pl_start(K_AMB, 1);
        for (i = 0.0f; i <= w; i += 2.0f)
            ito(i, mid - (i > d ? fm_minf(1.0f, (i - d) / 10.0f) * 9.0f * fm_sinf(i * 0.6f - t * 6.0f) : 0.0f));
        iline(d, 6.0f, d, h - 6.0f, K_DIM, 1);
        return;
    }
    case P_ORNSPEED:
        iorn(ORN_TRILL, t, K_AMB);
        return;
    case P_ORNAMOUNT:
        for (k = 0; k < 10; k++)
            idot(5.0f + (float)(k % 5) * 11.0f, mid - 8.0f + (float)(k / 5) * 16.0f, 3.0f,
                 (float)k < v * 10.0f ? K_AMB : K_DIM);
        return;
    case P_DRONE:
    case P_LEAD:
    case P_LEFT:
    case P_BASS:
    case P_DRUMS:
    case P_GDRUM:
    case P_GKICK:
    case P_GCLAP:
    case P_GMETAL:
    case P_VELAMT:
        ilevel(v, K_AMB);
        return;
    case P_SWING:
        for (k = 0; k < 4; k++) {
            idot(5.0f + (float)k * 13.0f, mid, 2.5f, K_WH);
            idot(5.0f + (float)k * 13.0f + 5.0f + v * 5.0f, mid, 2.0f, v > 0.01f ? K_AMB : K_GR);
        }
        return;
    case P_ACCENT:
        for (k = 0; k < 8; k++)
            ibar(3.0f + (float)k * 6.3f, k % 4 == 0 ? 14.0f + v * (h - 22.0f) : 14.0f, k % 4 == 0 ? K_AMB : K_WH, 4.0f);
        return;
    case P_LOOSE:
        for (k = 0; k < 8; k++)
            ibar(3.0f + (float)k * 6.3f + v * 2.0f * fm_sinf((float)k * 3.1f), 14.0f + v * 12.0f * fm_sinf((float)k * 1.7f + 1.0f),
                 K_AMB, 4.0f);
        return;
    case P_BALANCE: {
        float a = (v - 0.5f) * 0.9f, dx = fm_cosf(a) * 22.0f, dy = fm_sinf(a) * 22.0f;
        pl_start(K_GR, 1);
        ito(w / 2.0f - 6.0f, base);
        ito(w / 2.0f, mid);
        ito(w / 2.0f + 6.0f, base);
        iline(w / 2.0f - dx, mid - dy, w / 2.0f + dx, mid + dy, K_WH, 2);
        idot(w / 2.0f - dx, mid - dy - 6.0f, 5.0f, fm_fabsf(v - 0.5f) > 0.01f ? K_AMB : K_WH);
        idot(w / 2.0f + dx, mid + dy - 4.0f, 3.0f, K_WH);
        return;
    }
    case P_KITTUNE: {
        float f = 0.25f + v * 0.9f, i;
        pl_start(K_AMB, 1);
        for (i = 0.0f; i <= w; i += 1.5f)
            ito(i, mid - fm_sinf(i * f) * fm_expf(-i / 20.0f) * (h * 0.4f));
        return;
    }
    case P_ARPGATE: {                        /* four notes, each as long as it is set; at the top they join */
        int tied = set.par[id] >= PAR[id].hi;
        for (k = 0; k < 4; k++)
            irect(2.0f + (float)k * 13.0f, mid - 2.0f - (float)(k % 2) * 8.0f, tied ? 15.0f : 2.0f + v * 11.0f, 5.0f, K_AMB);
        return;
    }
    case K_DLEVEL:
        ilevel((float)set.dlv[ui.drum] / 100.0f, K_AMB);
        return;
    case K_DTUNE: {
        float f = 0.15f + (float)set.dtune[ui.drum] / 400.0f, i;
        pl_start(K_AMB, 1);
        for (i = 0.0f; i <= w; i += 1.5f)
            ito(i, mid - fm_sinf(i * f) * fm_expf(-i / 20.0f) * (h * 0.4f));
        return;
    }
    case K_DLEN:
        idecay((float)set.dlen[ui.drum] / 500.0f, K_AMB);
        return;
    case K_DRUM: {                           /* a darbuka, lit when its drum plays */
        int lit = ms_now - ui.hit_at[ui.drum] < 120u;
        ellipse(x + w / 2.0f, y + 14.0f, 17.0f, 5.0f, lit ? K_AMB : K_WH);
        boxf(x + w / 2.0f - 15.0f, y + 15.0f, 30.0f, 8.0f, K_GR);
        boxf(x + w / 2.0f - 7.0f, y + 23.0f, 14.0f, 14.0f, K_GR);
        boxf(x + w / 2.0f - 12.0f, y + 37.0f, 24.0f, 6.0f, K_GR);
        return;
    }
    case P_FILTER: case P_FILTER + 1: case P_FILTER + 2: case P_FILTER + 3:
    case P_FILTER + 4: case P_FILTER + 5: case P_FILTER + 6: case P_FILTER + 7: {
        float f = (float)set.par[id] / 50.0f, i;   /* the response: flat; the highs going (left); the lows (right) */
        pl_start(f != 0.0f ? K_AMB : K_WH, 1);
        for (i = 0.0f; i <= w; i += 2.0f) {
            float d = 0.0f;
            if (f < 0.0f) { float c = w * (1.0f + f * 0.85f); d = i > c ? fm_minf(1.0f, (i - c) / 10.0f) : 0.0f; }
            if (f > 0.0f) { float c = w * f * 0.85f; d = i < c ? fm_minf(1.0f, (c - i) / 10.0f) : 0.0f; }
            ito(i, 14.0f + d * (h - 24.0f));
        }
        return;
    }
    case P_LOWEQ: case P_LOWEQ + 1: case P_LOWEQ + 2: case P_LOWEQ + 3:
    case P_LOWEQ + 4: case P_LOWEQ + 5: case P_LOWEQ + 6: case P_LOWEQ + 7: {
        float f = (float)set.par[id] / 50.0f, i;   /* the lows raised or lowered, the rest as it is */
        pl_start(f != 0.0f ? K_AMB : K_WH, 1);
        for (i = 0.0f; i <= w; i += 2.0f)
            ito(i, mid - f * 14.0f * (1.0f - fm_minf(1.0f, fm_maxf(0.0f, (i - 10.0f) / 18.0f))));
        return;
    }
    case K_MUTE: case K_MUTE + 1: case K_MUTE + 2: case K_MUTE + 3:
    case K_MUTE + 4: case K_MUTE + 5: case K_MUTE + 6: case K_MUTE + 7: {   /* a speaker, its sound going out, or not */
        int m = set.mute >> (id - K_MUTE) & 1u;
        irect(8.0f, mid - 5.0f, 7.0f, 10.0f, m ? K_GR : K_WH);
        for (k = 0; k < 6; k++)
            iline(15.0f + (float)k, mid - 5.0f - (float)k * 1.5f, 15.0f + (float)k, mid + 5.0f + (float)k * 1.5f, m ? K_GR : K_WH, 1);
        if (m) {
            iline(28.0f, mid - 7.0f, 40.0f, mid + 7.0f, K_RED, 2);
            iline(28.0f, mid + 7.0f, 40.0f, mid - 7.0f, K_RED, 2);
        } else
            for (k = 0; k < 2; k++)
                arc(x + 22.0f, y + mid, 7.0f + (float)k * 7.0f, -0.8f, 0.8f, K_AMB, 1);
        return;
    }
    case K_TRACK:                            /* the eight tracks, the picked one lit (all: every one), drawn below */
        for (k = 0; k < DAB_NPART; k++) {
            int on = !set.track || set.track == k + 1;
            irect(4.0f + (float)(k & 3) * 12.0f, 6.0f + (float)(k >> 2) * 12.0f, 9.0f, 9.0f, on ? K_AMB : K_DIM);
        }
        track_icon(picked(), ri(x + w / 2.0f) - 5, ri(y + h) - 16, K_WH);
        return;
    case K_TSEND: {                          /* the picked track into the delay and the reverb: its echoes */
        float sv = par_norm(P_SEND + picked());
        idot(4.0f, base - 2.0f, 2.5f, K_WH);
        for (k = 0; k < 4; k++) {
            float f = fmodp(t * 0.35f + (float)k / 4.0f, 1.0f);
            arc(x + 4.0f, y + base - 2.0f, 4.0f + f * (w - 6.0f), -1.5708f, 0.0f,
                mixc(K_KBG, K_AMB, (1.0f - f) * sv), 1);
        }
        return;
    }
    case K_TPITCH: {                         /* its pitch: a note up or down from the middle line */
        float pv = (float)set.par[P_PITCH + picked()] / 48.0f;
        iline(4.0f, mid, w - 4.0f, mid, K_DIM, 1);
        iline(w / 2.0f, mid, w / 2.0f, mid - pv * (mid - 6.0f), pv != 0.0f ? K_AMB : K_WH, 1);
        idot(w / 2.0f, mid - pv * (mid - 6.0f), 4.0f, pv != 0.0f ? K_AMB : K_WH);
        return;
    }
    case K_TDRUM:                            /* the drum a tap adds: its track's sound's sign */
        track_icon(set.tsnd[picked()][0], ri(x + w / 2.0f) - 5, ri(y + mid) - 5, K_AMB);
        return;
    case K_FADE: {                           /* the scene fader: its cap, its top (the last 3 %) marked */
        float fy = 4.0f + (1.0f - (float)ui.master / 127.0f) * (h - 8.0f);
        irect(w / 2.0f - 1.0f, 4.0f, 2.0f, h - 8.0f, K_DIM);
        irect(w / 2.0f - 6.0f, 3.0f, 12.0f, 2.0f, set.top_mask ? K_AMB : K_GR);
        irect(w / 2.0f - 8.0f, fy - 2.0f, 16.0f, 4.0f, ui.master >= MASTER_TOP ? K_AMB : K_WH);
        return;
    }
    case K_TLEN: {                           /* LENGTH: a bar of steps, or two; a loop as it was played: its bars */
        int n = trk_nsteps(picked()), P = page_steps(), bars = (n + P - 1) / P, q;
        for (k = 0; k < bars && k < 8; k++)
            for (q = 0; q < 8; q++)
                irect(3.0f + (float)q * 6.0f, 6.0f + (float)k * (bars > 4 ? 6.0f : 12.0f), 4.0f, bars > 4 ? 4.0f : 8.0f,
                      k == 0 ? K_WH : K_AMB);
        return;
    }
    case K_TOPCLR:                           /* the controls with a top value, lit (4 a strip, 8 strips) */
        for (k = 0; k < 4 * DAB_NPART; k++)
            idot(6.0f + (float)(k >> 2) * 6.0f, 10.0f + (float)(k & 3) * 10.0f, 2.0f,
                 set.top_mask >> k & 1u ? K_AMB : K_DIM);
        return;
    case K_SECOND:
        for (k = 0; k < 8; k++)
            idot(4.0f + (float)k * 6.5f, mid - 10.0f, 2.2f, k % 4 == 0 ? K_WH : K_GR);
        if (ui.r1 >= 0)
            for (k = 0; k < 9; k++)
                idot(4.0f + (float)k * 5.8f, mid + 10.0f, 2.2f, k % 3 == 0 ? K_AMB : K_GR);
        else
            iline(4.0f, mid + 10.0f, w - 4.0f, mid + 10.0f, K_DIM, 1);
        return;
    }
    if (c < 0)
        return;
    switch (c) {                             /* the choices */
    case C_MODEL:
    case C_LEFTMODEL:
    case C_DRONESND: {                       /* the instrument: a reed, a bow, a pluck, keys */
        int m = c == C_DRONESND ? DSND_MODEL[set.ch[c] % DAB_NDRONE] : set.ch[c];
        if (MD_PLUCKED(m)) {
            ellipse(x + 18.0f, y + mid + 6.0f, 14.0f, 11.0f, K_AMB);
            boxf(x + 28.0f, y + mid + 2.0f, w - 30.0f, 4.0f, K_WH);
            disc(x + 18.0f, y + mid + 6.0f, 3.5f, K_BLACK);
        } else if (m == MD_STRINGS || m == MD_VIOLIN) {
            ellipse(x + w / 2.0f, y + mid + 4.0f, 9.0f, 16.0f, K_AMB);
            iline(4.0f, h - 6.0f, w - 4.0f, 8.0f, K_WH, 1);
        } else if (m == MD_ORGAN || m == MD_LEAD || m == MD_SQUARE || m == MD_ACCORDION) {
            for (k = 0; k < 7; k++)
                irect(3.0f + (float)k * 7.0f, mid - 6.0f, 6.0f, 20.0f, k == (int)fm_floorf(t * 3.0f) % 7 ? K_AMB : K_WH);
        } else {                             /* a pipe and its holes */
            irect(6.0f, mid - 4.0f, w - 12.0f, 8.0f, K_AMB);
            if (m == MD_MIJWIZ)
                irect(6.0f, mid + 6.0f, w - 12.0f, 6.0f, K_AMB);
            for (k = 0; k < 5; k++)
                idot(16.0f + (float)k * 7.0f, mid, 1.6f, K_BLACK);
        }
        return;
    }
    case C_PLAY:
        for (k = 0; k < 3; k++)
            irect(6.0f + (float)k * 16.0f, set.ch[C_PLAY] == 2 ? mid - 12.0f + (float)k * 6.0f : base - 10.0f - (float)k * 12.0f,
                  12.0f, 5.0f, set.ch[C_PLAY] == 2 || k == 2 ? K_AMB : K_GR);
        if (set.ch[C_PLAY] == 0)
            iline(18.0f, base - 8.0f, 22.0f, base - 20.0f, K_AMB, 1);
        return;
    case C_DOUBLE:
        irect(6.0f, mid - 12.0f, w - 12.0f, 5.0f, K_WH);
        if (set.ch[c])
            irect(6.0f, set.ch[c] == 1 ? mid + 8.0f : mid - 22.0f, w - 12.0f, 5.0f, K_AMB);
        return;
    case C_ORN:
        iorn(set.ch[c] == ORN_AUTO ? (int)fm_floorf(t * 1.5f) % 5 + 1 : set.ch[c], t, set.ch[c] ? K_AMB : K_GR);
        return;
    case C_TRILLTO: {
        static const float UP[3] = {16.0f, 10.0f, 5.0f};
        int on = (int)fm_floorf(t * 8.0f) & 1;
        iline(6.0f, mid + 8.0f, w - 6.0f, mid + 8.0f, K_WH, 2);
        iline(6.0f, mid + 8.0f - UP[set.ch[c]], w - 6.0f, mid + 8.0f - UP[set.ch[c]], on ? K_AMB : K_GR, 2);
        return;
    }
    case C_PICK:
        for (k = 0; k < (set.ch[c] ? 6 : 1); k++)
            iline(6.0f + (float)k * 8.0f, mid + 10.0f, 10.0f + (float)k * 8.0f, mid - 10.0f, K_AMB, 2);
        return;
    case C_DRONE:
    case C_DRONEOCT:
        irect(4.0f, (c == C_DRONEOCT && set.ch[c] ? mid - 10.0f : mid + 4.0f), w - 8.0f, 6.0f, set.ch[C_DRONE] ? K_AMB : K_DIM);
        if (set.ch[C_DRONE] == 2)
            irect(4.0f, mid - 4.0f, w - 8.0f, 4.0f, K_WH);
        return;
    case C_BEND: {
        float i, d = 6.0f + (float)set.ch[c] * 8.0f;
        pl_start(K_AMB, 2);
        for (i = 0.0f; i <= w; i += 2.0f)
            ito(i, mid + 10.0f - d * fm_minf(1.0f, fm_maxf(0.0f, (i - 12.0f) / 20.0f)));
        return;
    }
    case C_PRESS:
    case C_TOUCH:                            /* a finger on a key */
        irect(6.0f, mid + 6.0f, w - 12.0f, 10.0f, K_WH);
        ellipse(x + w / 2.0f, y + mid - 2.0f - (set.ch[c] ? fm_fabsf(fm_sinf(t * 2.0f)) * 4.0f : 6.0f), 6.0f, 9.0f,
                set.ch[c] ? K_AMB : K_GR);
        return;
    case C_DSYNC:
        for (k = 0; k < 4; k++)
            idot(6.0f + (float)k * 14.0f, mid, k == (int)fm_floorf(t * 4.0f) % 4 ? 4.0f : 2.0f, set.ch[c] ? K_AMB : K_GR);
        return;
    case C_ADDCLAP:
    case C_ADDRIQ:
    case C_ADDSAGAT:
    case C_ADDKICK:
    case C_SYNC: {
        uint16_t cc = set.ch[c] ? K_AMB : K_GR;
        if (c == C_ADDCLAP) {
            ellipse(x + w / 2.0f - 7.0f, y + mid, 6.0f, 10.0f, cc);
            ellipse(x + w / 2.0f + 7.0f, y + mid, 6.0f, 10.0f, cc);
        } else if (c == C_ADDRIQ) {
            circ(x + w / 2.0f, y + mid, 15.0f, cc, 2);
            for (k = 0; k < 5; k++)
                disc(x + w / 2.0f + fm_cosf((float)k * 1.2566f) * 15.0f, y + mid + fm_sinf((float)k * 1.2566f) * 15.0f, 3.0f, cc);
        } else if (c == C_ADDSAGAT) {
            ellipse(x + w / 2.0f - 8.0f, y + mid, 8.0f, 4.0f, cc);
            ellipse(x + w / 2.0f + 8.0f, y + mid, 8.0f, 4.0f, cc);
        } else if (c == C_ADDKICK) {
            circ(x + w / 2.0f, y + mid, 16.0f, cc, 2);
            disc(x + w / 2.0f, y + mid, 5.0f, cc);
        } else {
            float yy;
            irect(6.0f, 18.0f, 8.0f, h - 34.0f, cc);
            for (yy = 16.0f; yy < h - 16.0f; yy += 1.0f) {
                float f = 1.0f - fm_fabsf(yy + 0.5f - mid) / (mid - 16.0f);
                irect(24.0f, yy, 22.0f * f, 1.0f, cc);
            }
        }
        return;
    }
    case C_VAR:
        for (k = 0; k < 8; k++) {
            int on = k % 4 == 0 || (set.ch[c] >= 1 && (k & 1)) || (set.ch[c] == 2 && k % 2 == 0);
            ibar(3.0f + (float)k * 6.3f, on ? h - 16.0f : 10.0f, on ? K_AMB : K_GR, 4.0f);
        }
        return;
    case C_AUTOFILL: {
        static const uint8_t FB[5] = {0, 2, 4, 8, 16};
        int n = FB[set.ch[c] % 5];
        for (k = 0; k < 8; k++) {
            int f = n && (n >= 8 ? k == 7 : (k + 1) % n == 0);
            ibar(3.0f + (float)k * 6.3f, f ? h - 12.0f : 12.0f, f ? K_AMB : n ? K_WH : K_GR, 4.0f);
        }
        return;
    }
    case C_FILL:
        if (set.ch[c] == 1) {
            for (k = 0; k < 4; k++)
                ibar(3.0f + (float)k * 6.3f, 16.0f, K_WH, 4.0f);
            ibar(w - 8.0f, h - 12.0f, K_AMB, 4.0f);
        } else if (set.ch[c] == 2)
            for (k = 0; k < 8; k++)
                ibar(3.0f + (float)k * 6.3f, k % 2 == 0 && k >= 4 ? h - 12.0f : k < 4 ? 14.0f : 2.0f, k >= 4 ? K_AMB : K_WH, 4.0f);
        else
            for (k = 0; k < 9; k++)
                ibar(2.0f + (float)k * 5.8f, 8.0f + (float)k * 5.0f, K_AMB, 3.0f);
        return;
    case C_LEFT: {
        for (k = 0; k < 9; k++)
            irect(1.0f + (float)k * 6.0f, 16.0f, 4.5f, h - 30.0f, k < 4 && set.ch[c] ? K_AMB : K_WH);
        return;
    }
    case C_BACKING:
    case C_CHORD: {
        int m = c == C_CHORD ? 1 : set.ch[c], q;
        if (m == 0)
            for (k = 0; k < 4; k++)
                for (q = 0; q < 3; q++)
                    irect(4.0f + (float)k * 13.0f, 14.0f + (float)q * 12.0f, 5.0f, 6.0f, K_AMB);
        else if (m == 1)
            for (q = 0; q < 3; q++)
                irect(2.0f, 14.0f + (float)q * (c == C_CHORD && !set.ch[c] ? 10.0f : 12.0f), w - 4.0f, 6.0f, K_AMB);
        else if (m == 2)
            for (k = 0; k < 8; k++) {
                q = k < 4 ? k : 7 - k;
                irect(2.0f + (float)k * 6.5f, h - 14.0f - (float)q * 12.0f, 5.0f, 6.0f,
                      (int)fm_floorf(t * 4.0f) % 8 == k ? K_AMB : K_WH);
            }
        else
            iline(8.0f, mid, w - 8.0f, mid, K_DIM, 2);
        return;
    }
    case C_BASSPLAY:
    case C_BASSMODEL: {
        static const int8_t PAT[4][8] = {{0, -1, 2, -1, 0, 1, 2, -1}, {0, -1, -1, -1, 0, -1, -1, -1},
                                         {0, -1, 4, -1, 0, -1, 4, -1}, {0, 1, 2, 3, 4, 3, 2, 1}};
        int m = c == C_BASSMODEL ? 0 : set.ch[c];
        if (m > 3)
            iline(8.0f, mid, w - 8.0f, mid, K_DIM, 2);
        else
            for (k = 0; k < 8; k++)
                if (PAT[m][k] >= 0)
                    idot(5.0f + (float)k * 6.3f, base - 8.0f - (float)PAT[m][k] * 7.0f, 2.6f,
                         (int)fm_floorf(t * 4.0f) % 8 == k ? K_AMB : K_WH);
        return;
    }
    case C_ARPMODE: {                        /* its path over four keys, the note playing lit */
        static const int8_t SEQ[ARP_NORDER][8] = {{0, 1, 2, 3, 0, 1, 2, 3}, {3, 2, 1, 0, 3, 2, 1, 0}, {0, 1, 2, 3, 2, 1, 0, 1},
                                                  {3, 2, 1, 0, 1, 2, 3, 2}, {2, 0, 3, 1, 2, 0, 3, 1}, {1, 3, 0, 2, 3, 0, 1, 2},
                                                  {0, 0, 0, 0, 0, 0, 0, 0}, {0, 3, 1, 2, 0, 3, 1, 2}, {0, 1, 0, 2, 0, 3, 0, 1}};
        int m = set.ch[c] % ARP_NORDER, now = set.arp && dabke_status()->arp_n ? (int)((dabke_status()->arp_count + 7u) % 8u) : -1;
        float dx = (w - 10.0f) / 7.0f;
        if (m == ARP_CHORD) {                /* all together: a column of four, again and again */
            for (k = 0; k < 4; k++) {
                int q;
                for (q = 0; q < 4; q++)
                    idot(6.0f + (float)k * 13.0f, base - 6.0f - (float)q * 10.0f, 2.4f, now >= 0 && now % 4 == k ? K_AMB : K_WH);
            }
            return;
        }
        pl_start(K_DIM, 1);                  /* (any: its path changes) */
        for (k = 0; k < 8; k++) {
            int q = m == ARP_ANY ? (int)((uint32_t)(k * 7 + (int)fm_floorf(t * 2.0f) * 3) % 4u) : SEQ[m][k];
            ito(5.0f + (float)k * dx, base - 6.0f - (float)q * 10.0f);
        }
        for (k = 0; k < 8; k++) {
            int q = m == ARP_ANY ? (int)((uint32_t)(k * 7 + (int)fm_floorf(t * 2.0f) * 3) % 4u) : SEQ[m][k];
            idot(5.0f + (float)k * dx, base - 6.0f - (float)q * 10.0f, 2.6f, k == now ? K_AMB : K_WH);
        }
        return;
    }
    case C_ARPRATE: {                        /* a beat, its notes; one lit in turn at their pace */
        int r = ARP_RATE[set.ch[c] % 6], lit = (int)fm_floorf(t * (float)set.par[P_TEMPO] / 60.0f * (float)r) % r;
        iline(3.0f, base - 2.0f, w - 3.0f, base - 2.0f, K_GR, 1);
        iline(3.0f, base - 26.0f, 3.0f, base - 2.0f, K_GR, 1);
        iline(w - 3.0f, base - 26.0f, w - 3.0f, base - 2.0f, K_GR, 1);
        for (k = 0; k < r; k++)
            ibar(6.0f + (float)k * (w - 12.0f) / (float)r, 18.0f, k == lit ? K_AMB : K_WH, r > 6 ? 3.0f : 4.0f);
        return;
    }
    case C_ARPOCT: {                         /* the octaves it goes over: the played one white, the others it adds amber */
        static const int8_t OLO[ARPO_N] = {0, 0, 0, 0, -1, -2, -1}, OHI[ARPO_N] = {0, 1, 2, 3, 0, 0, 1};
        int o = set.ch[c] % ARPO_N;
        for (k = -2; k <= 3; k++)
            irect(8.0f, mid - 2.0f - (float)k * 8.0f, w - 16.0f, 5.0f, k == 0 ? K_WH : k >= OLO[o] && k <= OHI[o] ? K_AMB : K_DIM);
        return;
    }
    case C_ARPPAT: {                         /* its eight steps: an accent tall, a note, a rest a dot; the step playing framed */
        const char *p = ARP_PAT_STEPS[set.ch[c] % DAB_ARP_NPAT];
        int now = set.arp && dabke_status()->arp_n ? dabke_status()->arp_step : -1;
        float cw = (w - 4.0f) / 8.0f;
        for (k = 0; k < 8; k++) {
            float bx = 2.0f + (float)k * cw;
            if (p[k] == 'X')
                irect(bx, mid - 12.0f, cw - 2.0f, 24.0f, K_AMB);
            else if (p[k] == 'x')
                irect(bx, mid - 4.0f, cw - 2.0f, 16.0f, K_WH);
            else
                irect(bx + cw * 0.5f - 1.5f, mid + 10.0f, 2.0f, 2.0f, K_GR);
            if (k == now)
                frame(x + bx - 1.0f, y + mid - 14.0f, cw, 28.0f, K_RED);
        }
        return;
    }
    case C_ARPREP: {                         /* each note so many times: two notes, their repeats */
        int r = set.ch[c] % 4 + 1, q;
        for (q = 0; q < 2; q++)
            for (k = 0; k < r; k++)
                idot(6.0f + (float)(q * r + k) * (w - 12.0f) / (float)(2 * r - 1 > 0 ? 2 * r - 1 : 1), mid + 6.0f - (float)q * 14.0f,
                     2.6f, q ? K_AMB : K_WH);
        return;
    }
    case C_ARPSWING: {                       /* pairs of notes, the second later as it swings */
        float sw = set.ch[c] ? (float)(set.ch[c] - 1) * 0.25f : par_norm(P_SWING);
        for (k = 0; k < 4; k++) {
            idot(5.0f + (float)k * 13.0f, mid, 2.6f, K_WH);
            idot(5.0f + (float)k * 13.0f + 6.0f + sw * 5.0f, mid, 2.2f, set.ch[c] ? K_AMB : K_GR);
        }
        return;
    }
    case C_ARPLATCH: {                       /* a padlock: closed, it keeps playing; open, it stops with the keys */
        int on = set.ch[c] != 0;
        irect(w / 2.0f - 10.0f, mid - 2.0f, 20.0f, 16.0f, on ? K_AMB : K_GR);
        arc(x + w / 2.0f + (on ? 0.0f : 6.0f), y + mid - 2.0f - (on ? 0.0f : 6.0f), 7.0f, 3.1416f, 6.2832f, on ? K_AMB : K_GR, 2);
        if (!on)
            iline(w / 2.0f - 1.0f, mid - 8.0f, w / 2.0f - 1.0f, mid - 2.0f, K_GR, 2);
        idot(w / 2.0f, mid + 5.0f, 2.0f, K_BLACK);
        return;
    }
    case C_SCENE:
        for (k = 0; k < 4; k++)
            irect(3.0f + (float)k * 13.0f, mid - 8.0f, 10.0f, 16.0f, k == set.ch[c] ? K_AMB : K_DIM);
        return;
    case C_USB:
        irect(14.0f, mid - 9.0f, 20.0f, 18.0f, set.ch[c] ? K_GR : K_AMB);
        irect(34.0f, mid - 6.0f, 8.0f, 4.0f, set.ch[c] ? K_GR : K_AMB);
        irect(34.0f, mid + 2.0f, 8.0f, 4.0f, set.ch[c] ? K_GR : K_AMB);
        return;
    }
}

static int kv_locked;                    /* (the value knob_value just gave is a lock's) */
static int32_t knob_shown(int pid, int *locked)   /* the value a knob shows: the held step's lock of it; else the lock
                                                   * that holds it now, while its step plays; else its own */
{
    int i;
    *locked = 0;
    if (!lk_ok(pid))
        return set.par[pid];
    if (step_held() && (i = lk_find(picked(), ui.sstep, pid)) >= 0) {
        *locked = 1;
        return DAB_LK_VAL(sq.lock[i]);
    }
    i = dabke_status()->lk_at[DAB_LKK(picked(), pid)];
    if (i && i <= DAB_NLOCK && sq.lock[i - 1] && DAB_LK_ID(sq.lock[i - 1]) == pid) {
        *locked = 1;
        return DAB_LK_VAL(sq.lock[i - 1]);
    }
    return set.par[pid];
}
static void knob_value(int id, char *b, const char **label)   /* a knob's name and value, as words */
{
    char t[12];
    int pid = strip_id(id);
    b[0] = 0;
    kv_locked = 0;
    if (id >= K_CH && id < K_CH + NCH) {
        int c = id - K_CH;
        *label = CHOICE[c].label;
        if (c == C_MAQAM)
            maqam_name(b, 0);
        else
            text_cat(b, CHOICE[c].names[set.ch[c] % CHOICE[c].n]);
        return;
    }
    switch (id) {
    case K_SECOND:
        *label = "ADD RHYTHM";
        if (ui.r1 >= 0)
            rhythm_name_copy(b, ui.r1);
        else
            text_cat(b, "NONE");
        return;
    case K_QDEPTH:
        *label = "DEPTH";
        return;
    case K_CENTS:
        *label = "CENTS";
        return;
    case K_DRUM:
        *label = "DRUM";
        text_cat(b, DRUM_NAME[ui.drum]);
        return;
    case K_DLEVEL:
        *label = "DRUM LEVEL";
        int_text(set.dlv[ui.drum], b);
        return;
    case K_DTUNE:
        *label = "DRUM TUNE";
        int_text(set.dtune[ui.drum], t);
        text_cat(b, t);
        text_cat(b, " %");
        return;
    case K_DLEN:
        *label = "DRUM DECAY";
        int_text(set.dlen[ui.drum], t);
        text_cat(b, t);
        text_cat(b, " %");
        return;
    case K_TLEN: {                           /* "16 STEPS", "32 STEPS"; a loop as long as it was played: "64 PLAYED" */
        const dabke_status_t *s = dabke_status();
        int tk = picked(), n = trk_nsteps(tk), P = s->page;
        *label = "LENGTH";
        int_text(n, t);
        text_cat(b, t);
        text_cat(b, trk_loop(tk) && s->trk_played[tk] == s->trk_len[tk] && n != P && n != 2 * P ? " PLAYED" : " STEPS");
        return;
    }
    }
    if (id == K_TRACK) {
        *label = "TRACK";
        if (set.track)
            trk_name(set.track - 1, b);
        else
            text_cat(b, "ALL");
        return;
    }
    if (pid >= 0) {                          /* the picked track's strip */
        *label = strip_word(pid);
        if (pid >= P_PITCH && pid <= P_PITCH_LAST)   /* (in its box: "+1 1/4", shorter than the words) */
            par_text_at(pid, knob_shown(pid, &kv_locked), b);
        else
            value_words(pid, knob_shown(pid, &kv_locked), b);
        return;
    }
    if (id == K_TDRUM) {
        int tk = picked(), f = set.tsnd[tk][0], nd = dabke_sound_drums(f);
        *label = "DRUM";
        text_cat(b, trk_drum(tk) && nd > 0 ? DRUM_NAME[dabke_sound_drum(f, ui.tdrum[tk] % nd)] : "THE KEYS");
        return;
    }
    if (id == K_FADE) {
        *label = "FADE";
        int_text(ui.fade_own ? ui.fade : ((int)ui.master * 100 + 63) / 127, t);
        text_cat(b, t);
        if (ui.master >= MASTER_TOP)
            text_cat(b, " TOP");
        return;
    }
    if (id == K_TOPCLR) {
        *label = "TOP SOUND";
        text_cat(b, set.top_mask ? "TURN: CLEAR" : "NONE");
        return;
    }
    if (id >= K_MUTE && id < K_MUTE + DAB_NPART) {
        *label = "PART";
        text_cat(b, set.mute >> (id - K_MUTE) & 1u ? "MUTED" : "ON");
        return;
    }
    if (id == P_ARPGATE && knob_shown(id, &kv_locked) >= PAR[id].hi) {   /* the arpeggio's longest: the notes run into
                                                                           * each other */
        *label = PAR[id].word;
        text_cat(b, "TIED");
        return;
    }
    if (id >= P_FILTER && id <= P_LOWEQ_LAST) {   /* the mixer's filter and bass, in words: which way, how far */
        *label = PAR[id].word;
        value_words(id, knob_shown(id, &kv_locked), b);
        return;
    }
    if (IS_KNOB(id)) {
        *label = PAR[id].word;
        par_text_at(id, knob_shown(id, &kv_locked), b);
    } else
        *label = "";
}

static void scope(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    const int16_t *b = dabke_scope();
    int32_t i, s = 0;
    float mid = (float)y + (float)h * 0.5f, amp = (float)h * 0.5f - 1.0f;
    pl_start(c, 1);
    for (i = 1; i < DAB_SCOPE - w; i++)
        if (b[i - 1] < 0 && b[i] >= 0) {
            s = i;
            break;
        }
    for (i = 0; i < w && s + i < DAB_SCOPE; i++)
        pl_to((float)(x + i), mid - fm_clampf((float)b[s + i] * (2.4f / 32767.0f), -1.0f, 1.0f) * amp);
}

static void plock(int32_t x, int32_t y, uint16_t c)   /* a lock's mark: a small padlock, 5 x 7 */
{
    box(x, y + 3, 5, 4, c);
    box(x + 1, y, 3, 1, c);
    box(x + 1, y + 1, 1, 2, c);
    box(x + 3, y + 1, 1, 2, c);
}
static void knob_boxes(const uint8_t *row, int with_icons, int32_t top)   /* a row of four knobs: with their pictures
                                          * (a knob page), or small, from top down (the step view's strip) */
{
    int c;
    for (c = 0; c < 4; c++) {
        int32_t x = 3 + c * 59;
        int id = row[c], pid;
        char b[24], k[3] = {'K', (char)('1' + c), 0};
        const char *label = "";
        if (with_icons) {
            box(x, 36, 56, 122, K_KBG);
            T(k, x + 53, 154, 8, K_DIM, 2, 0);
        } else {
            box(x, top, 56, 46, K_KBG);
            T(k, x + 53, top + 43, 8, K_DIM, 2, 0);
        }
        if (id == K_NONE)
            continue;
        if (with_icons && in_strip(38, 102)) {
            clip_set(x + 1, 38, 54, 64);
            icon(id, (float)(x + 1), 39.0f, 54.0f, 62.0f, t_now);
            clip_all();
        }
        knob_value(id, b, &label);
        if (c == 0 && ui.page == PG_FX && ui.row[0] >= MIX_ROW)   /* (the mixer's rows: the header names the part) */
            label = "VOLUME";
        pid = strip_id(id) >= 0 ? strip_id(id) : id;
        if (set.track && lk_ok(pid) && lk_has(picked(), pid)) {   /* a lock of this knob somewhere on the picked track: a
                                                                  * small padlock by its name */
            plock(x + 49, with_icons ? 107 : top + 5, K_LK);
            T(label, x + 3, with_icons ? 116 : top + 14, 9, K_GR, 0, 44);
        } else
            T(label, x + 3, with_icons ? 116 : top + 14, 9, K_GR, 0, 51);
        T(b, x + 2, with_icons ? 134 : top + 33, 15, kv_locked ? K_LK : K_AMB, 0, 53);   /* (a lock's value: its blue) */
        {                                /* (a bar: the knob's place; the track row's: the picked track's) */
            float bv = id == K_FADE ? (float)ui.master / 127.0f : IS_KNOB(pid) ? par_norm(pid) : -1.0f;
            if (with_icons && bv >= 0.0f) {
                box(x + 3, 140, 50, 3, K_DIM);
                box(x + 3, 140, ri(50.0f * bv), 3, K_WH);
            }
        }
    }
}

static void knob_page(void)
{
    const uint8_t *row = KROW[ui.page - PG_FX][ui.row[ui.page - PG_FX]];
    knob_boxes(row, 1, 0);
    if (!in_strip(160, 240))
        return;
    if (ui.seq && set.track)                 /* step mode: the picked track's steps here, where he turns the knob */
        steps_bottom();
    else
        scope(6, 166, 228, 66, ui.level > 0.02f ? K_AMB : K_GR);
}

/* ARP held: the arpeggio's panel over any page, its two rows of four, the one the knobs turn now framed */
static void arp_page(void)
{
    int r, c;
    for (r = 0; r < 2; r++) {
        int32_t y0 = 34 + r * 102;
        int act = r == (ui.arp_row & 1u);
        if (!in_strip(y0, y0 + 100))
            continue;
        for (c = 0; c < 4; c++) {
            int32_t x = 3 + c * 59;
            int id = ARP_ROW[r][c];
            char b[24], k[3] = {'K', (char)('1' + c), 0};
            const char *label = "";
            box(x, y0, 56, 98, K_KBG);
            if (act)
                frame((float)x, (float)y0, 55.0f, 97.0f, K_AMB);
            if (in_strip(y0 + 2, y0 + 52)) {
                clip_set(x + 1, y0 + 2, 54, 50);
                icon(id, (float)(x + 1), (float)(y0 + 3), 54.0f, 48.0f, t_now);
                clip_all();
            }
            knob_value(id, b, &label);
            if (label[0] == 'A' && label[1] == 'R' && label[2] == 'P' && label[3] == ' ')  /* (its own panel: "ORDER", not "ARP ORDER") */
                label += 4;
            T(label, x + 3, y0 + 66, 9, act ? K_GR : RGB(100, 92, 86), 0, 51);
            T(b, x + 3, y0 + 84, 13, act ? K_AMB : K_GR, 0, 51);
            if (act)
                T(k, x + 52, y0 + 95, 8, K_DIM, 2, 0);
        }
    }
}

static void body(void)
{
    header();
    if (arp_panel_up())
        arp_page();
    else if (ui.save)
        save_page();
    else if (ui.page == PG_HOME)
        home_page();
    else if (ui.page == PG_SEL)
        maqam_page();
    else
        knob_page();
}

static void strip(int32_t y0, int32_t h)
{
    cv_begin(240, (uint32_t)h, K_BLACK);
    cv_oy = -y0;
    reg_y0 = y0;
    reg_y1 = y0 + h;
    clip_all();
    body();
    cv_oy = 0;
    cv_blit(0, (uint32_t)y0);
}

/* what the engine shows that the static parts draw: a change is a redraw */
static void engine_watch(void)
{
    const dabke_status_t *s = dabke_status();
    uint32_t sig;
    int k;
    dabke_load((int)plat_cpu_pct());
    if (s->beat_count != ui.beat_count) {
        ui.beat_count = s->beat_count;
        ui.beat_at = ms_now;
    }
    if (s->bar != ui.bar_seen) {
        ui.bar_seen = s->bar;
        if (s->bar && s->bar % 8u == 0u)
            ui.fill_at = ms_now;             /* (every eighth bar: confetti) */
    }
    if (s->in_fill)
        ui.fill_at = ms_now;
    for (k = 0; k < DAB_NDRUM; k++)
        if (s->hits[k] != ui.hits_seen[k]) {
            ui.hits_seen[k] = s->hits[k];
            ui.hit_at[k] = ms_now;
        }
    if (s->notes != ui.notes_seen)
        ui.notes_seen = s->notes;
    sig = (uint32_t)s->running | (uint32_t)(s->rec_track + 1) << 1 | (uint32_t)s->rec_waiting << 5 |
          (uint32_t)(s->left_root + 1) << 6 | (uint32_t)s->overdub << 10 | (uint32_t)s->hold << 11 |
          (uint32_t)(s->rec_track >= 0 ? s->trk_bars[s->rec_track] & 15u : 0u) << 12 | (uint32_t)s->dedit << 16 |
          (uint32_t)loops_mask() << 24;
    if (sig != ui.sig) {
        ui.sig = sig;
        ui.ver++;
    }
    {                                        /* the locks holding their knobs now: the pages show their values */
        uint32_t h = 0;
        for (k = 0; k < DAB_NLKS; k++)
            h = h * 31u + s->lk_at[k];
        if (h != ui.lk_sig) {
            ui.lk_sig = h;
            ui.ver++;
        }
    }
    if (ui.rec_was >= 0 && s->rec_track < 0 && ui.undo_trk < 0) {   /* a recording ended (not for another track's):
                                              * what it kept */
        int t = ui.rec_was;
        char b[40] = "", n[8];
        trk_name(t, b);
        if (trk_drum(t))
            text_cat(b, s->drum_take == t + 1 ? ": TAKE KEPT" : ": NOTHING PLAYED");
        else if (!s->trk_len[t])
            text_cat(b, ": NOTHING PLAYED");
        else {
            text_cat(b, ": ");
            int_text(s->trk_bars[t], n);
            text_cat(b, n);
            text_cat(b, s->trk_bars[t] == 1 ? " BAR, " : " BARS, ");
            int_text(s->trk_takes[t], n);
            text_cat(b, n);
            text_cat(b, s->trk_takes[t] == 1 ? " TAKE" : " TAKES");
        }
        say(b);
    }
    ui.rec_was = s->rec_track;
    if (ui.hold_wait && s->hold_ver != ui.hold_ver) {   /* HOLD done: what it holds */
        char b[24] = "HOLD: ", n[8];
        ui.hold_wait = 0;
        ui.held_keys = s->held_n ? ui.hold_snap : 0u;
        if (s->held_n) {
            int_text(s->held_n, n);
            text_cat(b, n);
            text_cat(b, s->held_n == 1 ? " NOTE" : " NOTES");
            say(b);
        } else
            say("HOLD OFF");
        ui.ver++;
    }
    if (!s->held_n && ui.held_keys && !ui.hold_wait)
        ui.held_keys = 0;
    if (ui.undo_trk >= 0 && (ui.undo_steps ? s->steps_ver : s->loop_ver) != ui.undo_ver) {   /* a take undone: what is
                                              * left. Nothing of his: it is empty, or the rhythm's again */
        int t = ui.undo_trk & 7;
        if (trk_his(t))
            trk_words(t, ": LAST TAKE GONE");
        else
            empty_say(t);                    /* (the locks he made outside any take stay on their steps) */
        ui.undo_trk = -1;
    }
    if (s->full != ui.full_seen) {           /* a note that found no room (128 a track, 512 in all) */
        ui.full_seen = s->full;
        say("NO ROOM FOR MORE NOTES");
    }
    if (s->loop_ver != ui.loop_ver && s->rec_track < 0) {   /* the loops' notes changed: kept with the settings (saved
                                                             * when quiet), track after track in the pool; a track
                                                             * recording: once it has ended */
        uint32_t v = s->loop_ver;
        int t, a = 0, ch = 0, ch2 = 0;
        for (t = 0; t < DAB_NTRK; t++) {
            int n = s->trk_n[t] > DAB_LOOP_N ? DAB_LOOP_N : s->trk_n[t], nt = s->trk_takes[t] > DAB_TAKES ? DAB_TAKES
                                                                                                          : s->trk_takes[t];
            if (!s->trk_len[t])
                n = nt = 0;
            if (a + n > DAB_LOOP_N * 4)
                n = DAB_LOOP_N * 4 - a;
            if (n != set.lp[t].n || (n ? s->trk_len[t] : 0) != set.lp[t].len || (n ? s->trk_bars[t] : 0) != set.lp[t].bars ||
                nt != set.lp[t].takes || memcmp(set.lp[t].take, s->trk_take[t], (uint32_t)nt))
                ch = 1;
            set.lp[t].n = (uint16_t)n;
            set.lp[t].len = n ? s->trk_len[t] : 0;
            set.lp[t].bars = n ? s->trk_bars[t] : 0;
            set.lp[t].takes = (uint8_t)nt;
            memcpy(set.lp[t].take, s->trk_take[t], (uint32_t)nt);
            if (n && memcmp(&set.pool[a], s->trk_ev[t], (uint32_t)n * 4u)) {   /* (track after track: the later ones
                                                                                * are written again too) */
                memcpy(&set.pool[a], s->trk_ev[t], (uint32_t)n * 4u);
                ch = 1;
            }
            if (sq.played[t] != (n ? s->trk_played[t] : 0)) {   /* (its length as it was played: the sequencer's part) */
                sq.played[t] = n ? s->trk_played[t] : 0;
                ch2 = 1;
            }
            a += n;
        }
        if (v == s->loop_ver) {              /* (not changed while it was copied: else again next frame) */
            ui.loop_ver = v;
            if (ch)
                changed();
            if (ch2)
                sq_changed();
            ui.ver++;
        }
    }
    if (s->steps_ver != ui.steps_ver) {      /* the tracks' own steps and lengths: steps 1 to 16 with the settings, the
                                              * rest with the sequencer's part */
        int t, j, k, ch = s->dedit != set.dedit, ch2 = s->two != sq.two;
        ui.steps_ver = s->steps_ver;
        for (t = 0; t < DAB_NTRK; t++) {
            int dn = s->dedit >> t & 1u ? s->tn[t] : 0;
            for (j = 0; j < DAB_TLANE; j++)
                for (k = 0; k < DAB_STEPS / 4; k++) {
                    if (set.tp[t][j][k] != s->tpat[t][j][k]) {
                        set.tp[t][j][k] = s->tpat[t][j][k];
                        ch = 1;
                    }
                    if (sq.tp2[t][j][k] != s->tpat[t][j][k + DAB_STEPS / 4]) {
                        sq.tp2[t][j][k] = s->tpat[t][j][k + DAB_STEPS / 4];
                        ch2 = 1;
                    }
                }
            if (sq.dn[t] != dn) {
                sq.dn[t] = (uint8_t)dn;
                ch2 = 1;
            }
        }
        set.dedit = s->dedit;
        sq.two = s->two;
        if (ch)
            changed();
        if (ch2)
            sq_changed();
        ui.ver++;
    }
}

static void speak(void)
{
    char t[160], b[48];
    if (ann_on() && ann_where()) {
        t[0] = 0;
        text_cat(t, APP_NAME ". ");
        text_cat(t, ui.save ? "KEEP THE SOUND" : PAGE_TITLE[ui.page]);
        text_cat(t, ". SOUND ");
        sound_name(b);
        text_cat(t, b);
        text_cat(t, ". MAQAM ");
        maqam_name(b, 2);
        text_cat(t, b);
        text_cat(t, ". RHYTHM ");
        text_cat(t, set.rhy[0]);
        text_cat(t, ". TEMPO ");
        int_text(set.par[P_TEMPO], b);
        text_cat(t, b);
        ann_say(t, 0, ANN_NOW);
    }
    ann_tick(plat_ms());
}

void app_ui_init(void)
{
    lcd_fill(0, 0, 240, 240, C_BLACK);
    ui.ver++;
    ui.drawn_ver = ui.ver - 1u;
}

void app_frame(void)
{
    const dabke_status_t *s;
    int full, anim;
    ms_now = plat_ms();
    t_now = (float)(ms_now % 3600000u) * 0.001f;
    seq_masks();
    input();
    if (ui.pick_at && ms_now - ui.pick_at >= 600u) {   /* the track picked last: one that stayed picked */
        ui.pick_at = 0;
        if (set.track)
            ui.last_trk = set.track;
    }
    usb_watch();
    engine_watch();
    if (ui.lk_feed < DAB_NLOCK) {            /* the kept locks to the audio side, each in its place: a few a frame */
        int n = 0;
        for (; ui.lk_feed < DAB_NLOCK && n < 16; ui.lk_feed++)
            if (sq.lock[ui.lk_feed]) {
                dabke_lock(ui.lk_feed, sq.lock[ui.lk_feed], lk_unit(DAB_LK_ID(sq.lock[ui.lk_feed])));
                n++;
            }
    }
    mix_leds();
    speak();
    s = dabke_status();
    ui.level += (s->level - ui.level) * (s->level > ui.level ? 0.5f : 0.1f);
    ui.lead += (s->lead - ui.lead) * (s->lead > ui.lead ? 0.5f : 0.15f);
    if (ui.msg_until && ms_now >= ui.msg_until) {
        ui.msg_until = 0;
        ui.ver++;
    }
    /* a few seconds after the last change, and only in a quiet moment: a save erases a flash sector with the
     * interrupts off, which stops the sound and the rhythm for that long */
    if ((set_dirty || sq_dirty) && ms_now - set_changed_at >= (set_dirty > 1 || sq_dirty > 1 ? 60000u : LAZY_SAVE_MS) &&
        !s->running && s->rec_track < 0 && !ui.keys &&
        (ui.level < 0.02f || (s->held_n && ms_now - set_changed_at >= 60000u)))   /* (notes held: a minute on, even so) */
        save_settings();
    if (arp_panel_up() != ui.arp_shown) {    /* (the arpeggio's panel up or down: everything again) */
        ui.arp_shown = (uint8_t)arp_panel_up();
        ui.ver++;
    }
    /* a whole redraw is 115 KB to the screen, about 21 ms the main loop waits for: twenty a second at most */
    full = ui.ver != ui.drawn_ver && ms_now - ui.full_at >= 50u;
    anim = ms_now - ui.anim_at >= (ui.page == PG_HOME && !ui.save ? 40u : 33u);
    if (!full && !anim)
        return;
    if (anim)
        ui.anim_at = ms_now;
    if (full) {
        ui.drawn_ver = ui.ver;
        ui.full_at = ms_now;
    }
    if (full)
        strip(0, 32);
    if (arp_panel_up()) {                    /* ARP held: its panel; its pictures move */
        if (full) {
            strip(32, 104);
            strip(136, 104);
        } else {
            strip(36, 50);
            strip(138, 50);
        }
    } else if (ui.save || ui.page == PG_SEL) {
        if (full) {
            strip(32, 104);
            strip(136, 104);
        }
    } else if (ui.page == PG_HOME && ui.seq) {   /* the step view: all of it on a change; the step playing and the
                                              * strip's values (a lock holding one) when they move */
        uint32_t ss = seq_sig();
        if (full) {
            strip(32, 104);
            strip(136, 104);
        } else if (ss != ui.seq_sig) {
            strip(SQ_ROW_Y - 2, SQ_ROW_H + 8);
            strip(SQ_KNOB_Y, 240 - SQ_KNOB_Y);
        }
        ui.seq_sig = ss;
    } else if (ui.page == PG_HOME) {
        uint32_t hs = panel_head_sig(), bs = panel_body_sig();
        if (full)
            strip(PN_Y, 240 - PN_Y);
        else {                               /* the moving parts, each alone, when they move: the metronome in the top
                                              * line, the steps playing in the lanes */
            if (hs != ui.head_sig)
                strip(0, 32);
            if (bs != ui.body_sig)
                strip(PN_Y, PN_FOOT_Y - PN_Y);
        }
        ui.head_sig = hs;
        ui.body_sig = bs;
        strip(32, 112);                      /* the scene, every time */
    } else {
        if (full)
            strip(104, 56);
        strip(32, 72);
        strip(160, 80);
    }
}

/* an update starts (the platform, right after app_panic, the interrupts on): what is not saved yet is saved now,
 * whatever was playing (the frame saves only a few seconds after a change, in a quiet moment), so this work is there
 * when DABKE is put back. The engine's panic ends a recording as REC does (dabke.c trk_runs, trk_stop) and stops the
 * sound; once it has taken that (its next block, 100 ms at most) its loops and steps are copied as a frame copies
 * them, and the settings and the sequencer's part saved if they changed (a failed save tried once more). Nothing
 * changed: nothing written. An update that installs nothing: the app goes on from here, saving as before */
void app_update_save(void)
{
#ifdef PLAT_HOST
    static int32_t lr[2u * APP_BLOCK];       /* (the simulator renders nothing in its "update": the blocks are run here) */
    int k;
    for (k = 0; k < 4 && !dabke_idle(); k++)
        app_render(lr, APP_BLOCK, 0);
#else
    uint32_t t0 = plat_ms();
    while (!dabke_idle() && plat_ms() - t0 < 100u)
        ;                                    /* (the audio interrupt takes the engine's messages) */
#endif
    ms_now = plat_ms();
    engine_watch();                          /* (the engine not done in time: a track still recording stays as it was kept) */
    if ((set_dirty || sq_dirty) && save_settings() != 0)
        save_settings();
}

/* ------------------------------------------------------------- host checks --- */
#ifdef PLAT_HOST
static int host_pitch_x10(void);
/* everything that is kept, as the settings, the sequencer's part and the engine have it now, in one number: the same
 * before and after a power cycle, if all of it was saved and came back */
static uint32_t host_hash_kept;
static uint32_t host_hash_of(int part)   /* part 0: all of it; 1 the settings, 2 the sequencer's part, 3 the engine's
                                          * tracks (their loops, sounds and steps) */
{
    const dabke_status_t *s = dabke_status();
    uint32_t h = 2166136261u;
    const uint8_t *p;
    uint32_t i;
    int t, j;
#define HB(ptr, n) for (p = (const uint8_t *)(ptr), i = 0; i < (uint32_t)(n); i++) h = (h ^ p[i]) * 16777619u
    if (part == 0 || part == 1)
        HB(&set, sizeof set);
    if (part == 0 || part == 2)
        HB(&sq, sizeof sq);
    if (part == 1 || part == 2)
        return h;
    for (t = 0; t < DAB_NTRK; t++) {
        HB(&s->trk_len[t], 2); HB(&s->trk_n[t], 2); HB(&s->trk_takes[t], 1); HB(&s->trk_bars[t], 1); HB(&s->trk_played[t], 2);
        HB(s->trk_take[t], s->trk_takes[t]);
        HB(s->trk_ev[t], 4u * s->trk_n[t]);
        HB(&s->tfam[t], 1); HB(&s->tidx[t], 1); HB(&s->tn[t], 1);
        for (j = 0; j < DAB_TLANE; j++)
            HB(s->tpat[t][j], DAB_TPAT);
    }
    HB(&s->dedit, 1); HB(&s->two, 1);
#undef HB
    return h;
}
static uint32_t host_hash(void) { return host_hash_of(0); }
int app_expect(const char *what, int *got)
{
    const dabke_status_t *s = dabke_status();
    if (!strcmp(what, "page"))
        *got = ui.page;
    else if (!strcmp(what, "save"))
        *got = ui.save;
    else if (!strcmp(what, "row"))
        *got = ui.page >= PG_FX ? ui.row[ui.page - PG_FX] : 0;
    else if (!strcmp(what, "cur"))
        *got = ui.cur;
    else if (!strcmp(what, "octave"))
        *got = set.oct;
    else if (!strcmp(what, "sound"))
        *got = tname(PART_LEAD);
    else if (!strcmp(what, "rhythm"))
        *got = ui.r0;
    else if (!strcmp(what, "rhythm2"))
        *got = ui.r1;
    else if (!strcmp(what, "tempo_is_bpm"))
        *got = set.par[P_TEMPO] == rhythm_bpm(ui.r0);
    else if (!strcmp(what, "running"))
        *got = s->running;
    else if (!strcmp(what, "arp"))
        *got = set.arp;
    else if (!strcmp(what, "sync"))
        *got = set.ch[C_SYNC];
    else if (!strcmp(what, "left"))
        *got = set.ch[C_LEFT];
    else if (!strcmp(what, "tempo"))
        *got = set.par[P_TEMPO];
    else if (!strcmp(what, "scene"))
        *got = set.ch[C_SCENE];
    else if (!strcmp(what, "seq"))           /* step mode; the page of steps shown; the step held (-1 none) */
        *got = ui.seq;
    else if (!strcmp(what, "spage"))
        *got = ui.spage;
    else if (!strcmp(what, "sstep"))
        *got = step_held() ? ui.sstep : -1;
    else if (!strcmp(what, "pages"))         /* the picked track: its pages of steps, its steps; a page's steps */
        *got = trk_pages(picked());
    else if (!strcmp(what, "nsteps"))
        *got = trk_nsteps(picked());
    else if (!strcmp(what, "page_steps"))
        *got = page_steps();
    else if (!strncmp(what, "tstep", 5))     /* tstepN: the step track N is on */
        *got = s->tstep[atoi(what + 5) & 7];
    else if (!strncmp(what, "tn", 2) && what[2] >= '0' && what[2] <= '9')   /* tnN: track N's steps on the rhythm's clock */
        *got = s->tn[atoi(what + 2) & 7];
    else if (!strncmp(what, "his", 3))       /* hisN: track N is his */
        *got = trk_his(atoi(what + 3) & 7);
    else if (!strncmp(what, "trig", 4)) {    /* trigT_S: something of track T begins on its step S (1 his, 2 the rhythm's) */
        int k = 4;
        while (what[k] && what[k] != '_')
            k++;
        *got = what[k] ? step_trig(atoi(what + 4) & 7, atoi(what + k + 1)) : -1;
    } else if (!strcmp(what, "locks")) {     /* the locks in all; lockT_S_ID: that lock's value (-999 none); locksT: track
                                              * T's locks */
        int i;
        for (i = 0, *got = 0; i < DAB_NLOCK; i++)
            *got += sq.lock[i] != 0;
    } else if (!strncmp(what, "locks", 5))
        *got = lk_count(atoi(what + 5) & 7, -1);
    else if (!strncmp(what, "lock", 4)) {
        int t2 = atoi(what + 4) & 7, k = 4, st2, i;
        while (what[k] && what[k] != '_')
            k++;
        st2 = what[k] ? atoi(what + k + 1) : 0;
        if (what[k])
            k++;
        while (what[k] && what[k] != '_')
            k++;
        i = what[k] ? lk_find(t2, st2, atoi(what + k + 1)) : -1;
        *got = i >= 0 ? DAB_LK_VAL(sq.lock[i]) : -999;
    } else if (!strncmp(what, "lk_at", 5))   /* lk_atID: the lock holding knob ID now: its value (-999: none, its own) */
        *got = s->lk_at[DAB_LKK(picked(), atoi(what + 5) % DAB_NLK)] ? DAB_LK_VAL(sq.lock[s->lk_at[DAB_LKK(picked(), atoi(what + 5) % DAB_NLK)] - 1]) : -999;
    else if (!strncmp(what, "epar", 4))      /* eparID: knob ID as the engine has it now, x 1000 */
        *got = dabke_host_par(atoi(what + 4));
    else if (!strncmp(what, "played", 6))    /* playedN: track N's loop as long as it was played, ticks */
        *got = s->trk_played[atoi(what + 6) & 7];
    else if (!strcmp(what, "two"))           /* LENGTH two bars, a bit a track */
        *got = s->two;
    else if (!strcmp(what, "drum_take"))
        *got = s->drum_take;
    else if (!strncmp(what, "dtakes", 6))    /* dtakesN: track N's drum takes that can be taken back */
        *got = s->dtakes[atoi(what + 6) & 7];
    else if (!strcmp(what, "hand"))          /* the tracks whose last take he wrote by hand, a bit each */
        *got = s->hand;
    else if (!strcmp(what, "mid_keys"))      /* the FM-1's keys at the second level (locked steps) */
        *got = (int)ui.mid_keys;
    else if (!strncmp(what, "mid_key", 7))
        *got = (int)(ui.mid_keys >> (atoi(what + 7) & 31) & 1u);
    else if (!strncmp(what, "dim_key", 7) && what[7] >= '0' && what[7] <= '9')
        *got = (int)(ui.dim_keys >> (atoi(what + 7) & 31) & 1u);
    else if (!strcmp(what, "why_unsaved")) { /* what keeps a change from being saved now, as bits: 1 running, 2 a
                                              * recording, 4 a key down, 8 not quiet, 16 notes held; 0: nothing does */
        *got = (s->running ? 1 : 0) | (s->rec_track >= 0 ? 2 : 0) | (ui.keys ? 4 : 0) | (ui.level >= 0.02f ? 8 : 0) |
               (s->held_n ? 16 : 0);
    } else if (!strcmp(what, "level1000"))
        *got = ui.level != ui.level || s->level != s->level ? -777 : (int)(ui.level * 1000.0f);   /* (-777: not a number) */
    else if (!strcmp(what, "gated")) {       /* the notes sounding by a key, a hold, a loop or the accompaniment (not the
                                              * ones dying away) */
        int i;
        for (i = 0, *got = 0; i < DAB_NTRK; i++)
            *got += dabke_host_voices(i);
    } else if (!strcmp(what, "held_locks")) {  /* the knobs a lock holds now */
        int i;
        for (i = 0, *got = 0; i < DAB_NLKS; i++)
            *got += s->lk_at[i] != 0;
    } else if (!strcmp(what, "hash_same"))   /* everything kept, as "hash_keep" last found it: 1 the same */
        *got = host_hash() == host_hash_kept;
    else if (!strncmp(what, "hash_part", 9)) /* hash_partN: part N's sum (0..9999), to tell which part differs */
        *got = (int)(host_hash_of(what[9] - '0') % 10000u);
    else if (!strcmp(what, "mirror")) { /* the locks here and on the audio side: the places that differ (0: none) */
        int i;
        for (i = 0, *got = 0; i < DAB_NLOCK; i++)
            *got += sq.lock[i] != dabke_host_lock(i);
    } else if (!strcmp(what, "lksums"))   /* the audio side's counts of its locks in step with them: 0 */
        *got = dabke_host_lock_sums();
    else if (!strcmp(what, "size_seq"))
        *got = (int)sizeof sq;

    else if (!strcmp(what, "size_ui"))       /* (the UI's state, bytes: for the RAM's count) */
        *got = (int)sizeof ui;
    else if (!strncmp(what, "starts", 6))    /* startsN: track N's notes and hits since the start */
        *got = (int)s->starts[atoi(what + 6) & 7];
    else if (!strcmp(what, "in_fill"))
        *got = s->in_fill;
    else if (!strncmp(what, "tapn", 4)) {    /* tapnT: the notes a tap adds on track T; tapT_I: note I of them */
        uint8_t nn[4];
        *got = chord_for(atoi(what + 4) & 7, nn);
    } else if (!strncmp(what, "tap", 3)) {
        uint8_t nn[4] = {0, 0, 0, 0};
        int k = 3, n2;
        while (what[k] && what[k] != '_')
            k++;
        n2 = chord_for(atoi(what + 3) & 7, nn);
        *got = what[k] && atoi(what + k + 1) < n2 ? nn[atoi(what + k + 1) & 3] : -1;
    }
    else if (!strcmp(what, "mute"))
        *got = set.mute;
    else if (!strcmp(what, "rec_track"))     /* the tracks: the one recording (-1 none), waiting for its first key */
        *got = s->rec_track;
    else if (!strcmp(what, "rec_waiting"))
        *got = s->rec_waiting;
    else if (!strcmp(what, "overdub"))
        *got = s->overdub;
    else if (!strncmp(what, "trk_n", 5))     /* keys track N's loop: its notes, length in ticks, bars, takes */
        *got = s->trk_n[atoi(what + 5) & 7];
    else if (!strncmp(what, "trk_len", 7))
        *got = s->trk_len[atoi(what + 7) & 7];
    else if (!strncmp(what, "trk_bars", 8))
        *got = s->trk_bars[atoi(what + 8) & 7];
    else if (!strncmp(what, "trk_takes", 9))
        *got = s->trk_takes[atoi(what + 9) & 7];
    else if (!strncmp(what, "trk_key", 7)) { /* trk_keyT_I: track T's note I's key; trk_posT_I its tick */
        int t = atoi(what + 7) & 7, i = atoi(what + 9) % DAB_LOOP_N;
        *got = s->trk_ev[t] ? (int)DAB_EV_NOTE(s->trk_ev[t][i]) : -1;
    } else if (!strncmp(what, "trk_dur", 7)) {   /* trk_durT_I: its length in ticks */
        int t = atoi(what + 7) & 7, i = atoi(what + 9) % DAB_LOOP_N;
        *got = s->trk_ev[t] ? (int)DAB_EV_LEN(s->trk_ev[t][i]) : -1;
    } else if (!strncmp(what, "trk_pos", 7)) {
        int t = atoi(what + 7) & 7, i = atoi(what + 9) % DAB_LOOP_N;
        *got = s->trk_ev[t] ? (int)DAB_EV_POS(s->trk_ev[t][i]) : -1;
    } else if (!strncmp(what, "kept_n", 6))  /* track N's loop as the settings keep it: its notes, takes */
        *got = set.lp[atoi(what + 6) & 7].n;
    else if (!strncmp(what, "kept_takes", 10))
        *got = set.lp[atoi(what + 10) & 7].takes;
    else if (!strcmp(what, "loop_plays"))
        *got = (int)s->loop_plays;
    else if (!strcmp(what, "loop_last_trk")) /* the last note the loops played: its track, its key */
        *got = (int)(s->loop_last >> 24);
    else if (!strcmp(what, "loop_last_key"))
        *got = (int)(s->loop_last & 255u);
    else if (!strcmp(what, "dedit"))         /* the tracks with their own steps; track T's lane L's step S: tpatT_L_S */
        *got = s->dedit;
    else if (!strncmp(what, "tpat", 4)) {
        int t = atoi(what + 4) & 7, k = 4, l;
        while (what[k] && what[k] != '_')
            k++;
        l = what[k] ? atoi(what + k + 1) % DAB_TLANE : 0;
        if (what[k])
            k++;
        while (what[k] && what[k] != '_')
            k++;
        *got = what[k] ? s->tpat[t][l][(atoi(what + k + 1) % DAB_TSTEPS) >> 2] >> (((atoi(what + k + 1) % DAB_TSTEPS) & 3) * 2) & 3 : -1;
    } else if ((!strncmp(what, "snd", 3) && what[3] != '_') || !strncmp(what, "esnd", 4)) {   /* track T's own sound: sndpT_ID its knob ID
                                              * (its steps), sndcT_C its choice C, sndnT its name (sound's numbers),
                                              * sndxT 1: its instrument's own tone; esndT_ID knob or choice ID (P_*) of
                                              * it as the engine has it now, x 1000 (DAB_PLAIN: P_COUNT) */
        int e = what[0] == 'e', t = atoi(what + 4) & 7, k = 4, id;   /* (sndp4_14, esnd4_14: the track at 4) */
        char w = what[3];
        while (what[k] && what[k] != '_')
            k++;
        id = what[k] ? atoi(what + k + 1) : 0;
        if (e)
            *got = dabke_host_tpar(t, id);
        else if (w == 'p')
            *got = DAB_SK(id) >= 0 ? set.ts[t].par[DAB_SK(id)] : -999;
        else if (w == 'c')
            *got = id >= 0 && id < NCH && sc_of(id) >= 0 ? set.ts[t].ch[sc_of(id)] : -999;
        else if (w == 'n')
            *got = tname(t);
        else
            *got = set.ts[t].plain;
    } else if (!strncmp(what, "tfam", 4))    /* track N's sound: its family, its number */
        *got = s->tfam[atoi(what + 4) & 7];
    else if (!strncmp(what, "tidx", 4))
        *got = s->tidx[atoi(what + 4) & 7];
    else if (!strncmp(what, "ktp", 3)) {     /* ktpT_L_S: a step as the settings keep it */
        int t = atoi(what + 3) & 7, k = 3, l;
        while (what[k] && what[k] != '_')
            k++;
        l = what[k] ? atoi(what + k + 1) % DAB_TLANE : 0;
        if (what[k])
            k++;
        while (what[k] && what[k] != '_')
            k++;
        *got = what[k] ? tp_get(t, l, atoi(what + k + 1) % DAB_STEPS) : -1;
    } else if (!strncmp(what, "ksnd", 4))    /* ksndT: track T's sound as the settings keep it, family x 100 + number */
        *got = set.tsnd[atoi(what + 4) & 7][0] * 100 + set.tsnd[atoi(what + 4) & 7][1];
    else if (!strcmp(what, "kept_dedit"))
        *got = set.dedit;
    else if (!strncmp(what, "voices", 6) && what[6] >= '0' && what[6] <= '9')   /* track N's notes sounding (key or
                                              * hold) */
        *got = dabke_host_voices(atoi(what + 6));
    else if (!strcmp(what, "drone_lvl"))     /* the drone's level x 1000; its note from the keys (-1: the tonic) */
        *got = dabke_host_drone();
    else if (!strcmp(what, "drone_note"))
        *got = dabke_host_drone_note();
    else if (!strcmp(what, "voice_cap"))     /* the load's guard: the notes it lets sound at once; sounding now; let go
                                              * early; renders past their time */
        *got = s->voice_cap;
    else if (!strcmp(what, "voices_all"))
        *got = s->voices;
    else if (!strcmp(what, "shed"))
        *got = (int)s->shed;
    else if (!strcmp(what, "late"))
        *got = (int)s->late;
    else if (!strncmp(what, "vfam", 4))      /* the family of a note sounding on track N (-1: none) */
        *got = dabke_host_vfam(atoi(what + 4) & 7);
    else if (!strcmp(what, "orn_live"))      /* lead voices trilling or shaking now */
        *got = dabke_host_orn();
    else if (!strcmp(what, "snd_locks")) {   /* the steps' sound locks, in all */
        int i;
        for (i = 0, *got = 0; i < DAB_NLOCK; i++)
            *got += sq.lock[i] && DAB_LK_ID(sq.lock[i]) == DAB_LK_SOUND;
    } else if (!strncmp(what, "dsnd", 4))    /* dsndN: drum N's last hit: the track it came from x 100 + the number of the
                                              * sound (of that drum's family) that played it */
        *got = dabke_host_dsound(atoi(what + 4));
    else if (!strncmp(what, "hits", 4))      /* drum N's hits so far */
        *got = (int)s->hits[atoi(what + 4) % DAB_NDRUM];
    else if (!strcmp(what, "held_keys"))     /* the FM-1's keys held (lit) */
        *got = (int)ui.held_keys;
    else if (!strcmp(what, "hold"))          /* HOLD: notes held (the engine's), how many */
        *got = s->hold;
    else if (!strcmp(what, "held_n"))
        *got = s->held_n;
    else if (!strcmp(what, "track"))         /* the picked track: 0 all, 1 + PART_* */
        *got = set.track;
    else if (!strcmp(what, "master"))        /* the scene fader, 0..127; its top's controls, a bit each; control C's top */
        *got = ui.master;
    else if (!strcmp(what, "top_mask"))
        *got = (int)set.top_mask;
    else if (!strncmp(what, "top", 3))
        *got = set.top[atoi(what + 3) % (4 * DAB_NPART)];
    else if (!strncmp(what, "eff", 3))       /* knob N as it sounds (the scene fader between it and its top) */
        *got = par_eff(atoi(what + 3) % P_COUNT);
    else if (!strcmp(what, "lit_keys"))      /* the FM-1's key lights as last sent: lit, dim */
        *got = (int)ui.lit_keys;
    else if (!strcmp(what, "dim_keys"))
        *got = (int)ui.dim_keys;
    else if (!strncmp(what, "lit_key", 7))   /* key N lit */
        *got = (int)(ui.lit_keys >> (atoi(what + 7) & 31) & 1u);
    else if (!strcmp(what, "arp_panel"))     /* the arpeggio: its panel up, its row; notes in its set; played since the
                                              * set began, the first sixteen; its pattern's step */
        *got = arp_panel_up();
    else if (!strcmp(what, "arp_row"))
        *got = ui.arp_row;
    else if (!strcmp(what, "arp_n"))
        *got = s->arp_n;
    else if (!strcmp(what, "arp_count"))
        *got = s->arp_count;
    else if (!strncmp(what, "arp_first", 9))
        *got = s->arp_first[atoi(what + 9) & 15];
    else if (!strcmp(what, "pitch_x10"))     /* what sounds now: its pitch, tenths of a hertz */
        *got = host_pitch_x10();
    else if (!strcmp(what, "left_root"))
        *got = s->left_root;
    else if (!strcmp(what, "dirty"))
        *got = set_dirty ? set_dirty : sq_dirty;
    else if (!strcmp(what, "kept")) {
        int i;
        for (i = 0, *got = 0; i < NKEPT; i++)
            *got |= kept.used[i] << i;
    } else if (!strcmp(what, "mixleds"))
        *got = (int)ui.mix_leds;
    else if (!strcmp(what, "size_settings"))
        *got = (int)sizeof set;
    else if (!strcmp(what, "size_kept"))
        *got = (int)sizeof kept;
    else if (!strcmp(what, "sound_knobs")) {     /* the knobs a sound changes: at most NSK */
        int i;
        for (i = 0, *got = 0; i < P_COUNT; i++)
            *got += SOUND_KNOB(i);
    }
    else if (!strcmp(what, "slot"))
        *got = ui.slot;
    else if (!strcmp(what, "maqam"))
        *got = set.ch[C_MAQAM];
    else if (!strcmp(what, "root"))
        *got = set.ch[C_ROOT];
    else if (!strcmp(what, "qdepth"))
        *got = set.qdepth;
    else if (!strcmp(what, "drum"))
        *got = ui.drum;
    else if (!strcmp(what, "step"))
        *got = ui.step;
    else if (!strcmp(what, "own"))
        *got = set.own_used;
    else if (!strcmp(what, "own_len"))
        *got = ui.r0 >= dabke_rhythm_count() ? set.own_len[ui.r0 - dabke_rhythm_count()] : 0;
    else if (!strcmp(what, "hit"))
        *got = (gview_of == ui.r0 || (grid_view(), 1)) ? gview[ui.drum][ui.step % gview_len] : 0;
    else if (!strcmp(what, "keysdim"))
        *got = left_is_lower() ? (int)(0xFFFu & ~ui.keys) : 0;
    else if (!strcmp(what, "dlevel"))
        *got = set.dlv[ui.drum];
    else if (!strcmp(what, "dtune"))
        *got = set.dtune[ui.drum];
    else if (!strcmp(what, "dlen"))
        *got = set.dlen[ui.drum];
    else if (!strcmp(what, "pressure"))
        *got = ui.press_sent;
    else if (!strncmp(what, "cents", 5))
        *got = set.ntune[atoi(what + 5) % 12];
    else if (!strncmp(what, "par", 3))
        *got = set.par[atoi(what + 3) % P_COUNT];
    else if (!strncmp(what, "ch", 2))
        *got = set.ch[atoi(what + 2) % NCH];
    else
        return 0;
    return 1;
}

/* the pitch of what sounds now (tenths of a hertz, 30 .. 440 Hz; 0 none): the normalized autocorrelation of the last
 * 4096 samples, its first peak near the highest, between the samples' neighbours */
static int host_pitch_x10(void)
{
    static float x[4096], nsdf[1500];
    uint32_t w = host_out_w, i;
    int t, best = 0, k;
    float top = 0.0f;
    for (i = 0; i < 4096u; i++)
        x[i] = host_out[(w - 4096u + i) & 8191u];
    for (t = 100; t < 1470; t++) {
        float r = 0.0f, m = 0.0f;
        for (k = 0; k + t < 4096; k++) {
            r += x[k] * x[k + t];
            m += x[k] * x[k] + x[k + t] * x[k + t];
        }
        nsdf[t] = m > 0.0f ? 2.0f * r / m : 0.0f;
        if (nsdf[t] > top)
            top = nsdf[t];
    }
    if (top < 0.3f)
        return 0;
    for (t = 101; t < 1469; t++)
        if (nsdf[t] >= 0.9f * top && nsdf[t] >= nsdf[t - 1] && nsdf[t] >= nsdf[t + 1]) {
            best = t;
            break;
        }
    if (!best)
        return 0;
    {
        float a = nsdf[best - 1], b = nsdf[best], c = nsdf[best + 1], d = a - 2.0f * b + c;
        float tt = (float)best + (d != 0.0f ? 0.5f * (a - c) / d : 0.0f);
        return (int)(441000.0f / tt + 0.5f);
    }
}

/* the script's own commands: "scene N" (the home page's scene), "cpu PCT" */
static int host_fail_n;                  /* the app's own checks that failed ("held") */
int app_host_fails(void) { return host_fail_n; }
int app_host_cmd(const char *cmd, const char *a, const char *b, const char *c, const char *d)
{
    (void)b, (void)c, (void)d;
    if (!strcmp(cmd, "scene")) {
        set.ch[C_SCENE] = (uint8_t)(atoi(a) & 3);
        changed();
        return 1;
    }
    if (!strcmp(cmd, "settle")) {        /* settle: everything stopped and let go (the rhythm, a recording, the held
                                          * notes, the arpeggio, step mode), for a check of what is left sounding or kept */
        dabke_run(0);
        dabke_track_rec(-1);
        dabke_hold(0);
        ui.held_keys = 0;
        if (set.arp)
            arp_toggle();
        step_mode(0);
        ui.rec_down = ui.mrec_down = ui.seq_down = ui.play_down = 0;
        return 1;
    }
    if (!strcmp(cmd, "held")) {          /* held TEXT: the held step's line on the screen is TEXT (no step held: "") */
        char x[72] = "", y[72], want[96];
        const char *w = plat_host_line + 4;
        int k = 0;
        while (*w == ' ')
            w++;
        while (w[k] && w[k] != '\n' && w[k] != '\r' && k < 95) {
            want[k] = w[k];
            k++;
        }
        while (k > 0 && want[k - 1] == ' ')
            k--;
        want[k] = 0;
        if (step_held())
            step_words(picked(), ui.sstep, x, y);
        if (strcmp(x, want)) {
            printf("FAIL held line \"%s\", wanted \"%s\"\n", x, want);
            host_fail_n++;
        } else
            printf("  ok held \"%s\"\n", x);
        return 1;
    }
    if (!strcmp(cmd, "hash_keep")) {     /* hash_keep: everything kept, remembered for "expect hash_same 1" */
        host_hash_kept = host_hash();
        return 1;
    }
    if (!strcmp(cmd, "cpu")) {           /* cpu PCT: the FM-1 as if PCT % as slow as the guard first assumes */
        dabke_host_cpu(atoi(a));
        return 1;
    }
    return 0;
}
#endif
