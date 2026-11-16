/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 STUDIOMORPHIK */
/* DABKE's sound engine. See dabke.h.
 *
 * The audio side keeps COMBO's frame (organ.c): a queue of messages from the main loop, the sound made 32 samples at a
 * time, the rhythm's clock counted in whole steps, the effects after the notes, the floor under every feedback path,
 * the number of notes adapted to the processor's load. What sounds is new: oscillators and filters for reeds and
 * keyboard leads, plucked strings, synthesized drums, quarter tones on every note, ornaments and a drone. No double
 * and no libm (fastmath.h): the FM-1's processor has neither. */
#include <stdint.h>
#include "plat.h"
#include "fastmath.h"
#include "dabke.h"

#define SR 44100.0f
#define BS 32                       /* the sound is made 32 samples at a time */
#define NV 16                       /* notes sounding at once (the drone has its own) */
#define TWO_PI 6.2831853f
#define BEAT 4096u                  /* the clocks count 4096 to a beat */
#define ECHO_N 30870                /* 0.7 s of echo */
#define KSN 1024                    /* a plucked string's delay line (lowest pitch 43 Hz) */
#define SLAP_N 4096                 /* the PA's slap-back, 93 ms */
#define DN 1e-20f                   /* the floor under every feedback path (see bq_run) */

typedef struct {
    const char *name, *group;
    uint8_t div, bpm, len;          /* steps in a beat, its tempo, steps in all */
    const char *lane[DAB_NLANE];    /* the 13 drums, then bass, then backing; one character a step */
} rhythm_t;
#include "dabke_rhythms.h"

/* ---------------------------------------------------------------- maqamat --- */
/* each as the keys of its scale and the keys a quarter tone flat, a bit each from its tonic (bit 0 the tonic) */
typedef struct {
    const char *name;
    uint8_t root;
    uint16_t scale, flat;
} maqam_t;
static const maqam_t MAQAM[] = {
    {"Plain", 0, 0xAB5, 0x000},       /* no quarter tones: the twelve as they are */
    {"Rast", 0, 0xAB5, 0x810},        /* C D Eh F G A Bh: the third and seventh half flat */
    {"Bayati", 2, 0x5AD, 0x004},      /* D Eh F G A Bb C */
    {"Saba", 2, 0x59D, 0x004},        /* D Eh F Gb A Bb C */
    {"Sikah", 4, 0x5AB, 0x081},       /* Eh F G A Bh C D */
    {"Huzam", 4, 0x59B, 0x001},       /* Eh F G Ab B C D */
    {"Iraq", 11, 0x56B, 0x021},       /* Bh C D Eh F G A */
    {"Bayati Shuri", 2, 0x66D, 0x004},   /* D Eh F G Ab B C (Bayati with Hijaz on G) */
    {"Hijaz", 2, 0x5B3, 0x000},       /* D Eb F# G A Bb C */
    {"Hijaz Kar", 0, 0x9B3, 0x000},   /* C Db E F G Ab B */
    {"Kurd", 2, 0x5AB, 0x000},        /* D Eb F G A Bb C */
    {"Nahawand", 0, 0x9AD, 0x000},    /* C D Eb F G Ab B */
    {"Ajam", 10, 0xAB5, 0x000},       /* Bb C D Eb F G A */
    {"Nakriz", 0, 0x6CD, 0x000},      /* C D Eb F# G A Bb */
    {"Saba Zamzam", 2, 0x59B, 0x000}, /* D Eb F Gb A Bb C */
};
#define NMAQAM ((int)(sizeof MAQAM / sizeof MAQAM[0]))
int dabke_maqam_count(void) { return NMAQAM; }
const char *dabke_maqam_name(int m) { return m >= 0 && m < NMAQAM ? MAQAM[m].name : ""; }
int dabke_maqam_root(int m) { return m >= 0 && m < NMAQAM ? MAQAM[m].root : 0; }
int dabke_maqam_detune(int m, int root, int pc, int depth)
{
    int rel;
    if (m < 0 || m >= NMAQAM)
        return 0;
    rel = ((pc - root) % 12 + 12) % 12;
    return (MAQAM[m].flat >> rel & 1u) ? -depth : 0;
}
uint32_t dabke_maqam_scale(int m, int root)
{
    uint32_t s, r;
    if (m < 0 || m >= NMAQAM)
        m = 0;
    r = (uint32_t)(((root % 12) + 12) % 12);
    s = MAQAM[m].scale;
    return ((s << r) | (s >> (12u - r))) & 0xFFFu;
}
int dabke_maqam_tonic_flat(int m) { return m >= 0 && m < NMAQAM ? (int)(MAQAM[m].flat & 1u) : 0; }

/* ------------------------------------------------------------------ tables --- */
static float SINE[2049] PLAT_POOL;              /* one cycle in 2048 points */
static float ksbuf[NV][KSN] PLAT_POOL;          /* each note's string, for the plucked instruments */

static inline float sin_ph(uint32_t ph)         /* sine of a 32-bit phase */
{
    uint32_t j = ph >> 21;
    return SINE[j] + (SINE[j + 1] - SINE[j]) * ((float)(ph & 0x1FFFFFu) * (1.0f / 2097152.0f));
}
static inline float sin_turn(float t)           /* sine of a phase in turns, 0 <= t < 1 */
{
    float x = t * 2048.0f;
    int32_t j = (int32_t)x;
    return SINE[j] + (SINE[j + 1] - SINE[j]) * (x - (float)j);
}
static inline float fracf(float x) { return x - (float)(int32_t)x; }   /* x >= 0 */
static inline float blep(float t, float dt)
{
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}
#define PHF(ph) ((float)((ph) >> 8) * (1.0f / 16777216.0f))
static inline float saw_s(uint32_t ph, float dt) { float p = PHF(ph); return 2.0f * p - 1.0f - blep(p, dt); }
static inline float pulse_s(uint32_t ph, float dt, float pw)   /* a pulse of width pw, its mean taken out */
{
    float p = PHF(ph), q = p - pw;
    if (q < 0.0f) q += 1.0f;
    return (p < pw ? 1.0f : -1.0f) + blep(p, dt) - blep(q, dt) + (1.0f - 2.0f * pw);
}
static inline float clip3(float x)              /* a soft clip: x, rounding off to +-1 at 1.5 */
{
    if (x > 1.5f) return 1.0f;
    if (x < -1.5f) return -1.0f;
    return x - x * x * x * 0.148148148f;
}

typedef struct { float b0, b1, b2, a1, a2, z1, z2; } bq_t;
enum { LP, HP, BP };
static void bq_set(bq_t *f, int type, float fc, float q)
{
    float w, c, s, al, b0, b1, b2, a0;
    if (fc > SR * 0.45f) fc = SR * 0.45f;
    w = TWO_PI * fc / SR; c = fm_cosf(w); s = fm_sinf(w); al = s / (2.0f * q);
    if (type == LP) { b0 = (1.0f - c) * 0.5f; b1 = 1.0f - c; b2 = b0; }
    else if (type == HP) { b0 = (1.0f + c) * 0.5f; b1 = -(1.0f + c); b2 = b0; }
    else { b0 = al; b1 = 0.0f; b2 = -al; }
    a0 = 1.0f / (1.0f + al);
    f->b0 = b0 * a0; f->b1 = b1 * a0; f->b2 = b2 * a0; f->a1 = -2.0f * c * a0; f->a2 = (1.0f - al) * a0;
}
/* A floor under every feedback path (COMBO's finding): left alone, a filter or a reverb comb decays into subnormal
 * numbers and stays there; how the FM-1's FPU treats those is not documented. */
static inline float bq_run(bq_t *f, float x)
{
    float y;
    x += DN;
    y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}
/* the state-variable filter's frequency coefficient (Chamberlin), kept where it is stable */
static float svf_f(float fc)
{
    if (fc < 20.0f) fc = 20.0f;
    if (fc > 8800.0f) fc = 8800.0f;
    return 2.0f * fm_sinf(3.14159265f * fc / SR);
}
/* its top for a damping q: at f*f + 2*q*f = 4 the filter feeds itself and its output never comes back (TONE near its
 * top with a damped sound did that: no sound until the next start). Kept a little under */
static float svf_top(float q) { return (fm_sqrtf(q * q + 4.0f) - q) * 0.96f; }

/* ------------------------------------------------------------------- state --- */
/* an instrument's sound worked out from its knobs: the lead's, the left hand's, the bass's, the drone's */
typedef struct {
    uint8_t model;
    float lpF, lpQ, lpTop;          /* the tone filter: coefficient, damping, the coefficient's top for that damping */
    float bpF, bpQ, bpMix, dryMix;  /* the body: a formant */
    float drive, driveN;            /* the buzz: gain into the clip, gain after it */
    float noise, detA, detB, pw, gain;   /* breath, the two oscillators' ratios (apart around the note), a pulse's width, loudness */
    float atkN, relN;               /* the envelope, a block at a time */
    float rings, bright, fenv;      /* a string's ring (s), its pick's brightness (0..1); brass: the filter's opening */
} tone_t;
static tone_t TT[DAB_NTRK];             /* each track's note sound, worked out (a drum sound: unused) */
#define TD TT[PART_DRONE]               /* (the drone's line, made by its own generator while its track has a drone sound) */
static uint8_t tfam[DAB_NTRK] = {SF_LEAD, SF_DRONE, SF_LEFT, SF_BASS, SF_KICK, SF_DARB, SF_CLAP, SF_RIQ}, tidx[DAB_NTRK];
enum { R_KEY, R_BASS = 2, R_BACK, R_LOOP, R_DRN };   /* a note's role: a key's, the bass line's, the backing's, a loop's,
                                         * the drone line's (its track's sound not the drone's own) */

typedef struct {
    uint8_t used, m, n, gate, model;    /* m: its role (R_*) */
    uint8_t tr, fam;                    /* its track, that track's sound's family as it began */
    int8_t oct;                         /* the octave layer: -12, 0, +12 */
    uint8_t orn, ostep, slot, pick;
    float vel, g, pp;                   /* the velocity's gain, the envelope, bass pluck / brass filter */
    float gl, oo, ot, okk, up, down;    /* glide offset, the ornament's offset and target and pace, neighbours */
    float vamt, age, shake, oph, base;  /* vibrato's share, seconds since on, shake's start, its phase, the pitch */
    int32_t otime, kexc, kpick;
    uint32_t ph1, ph2, vage;
    float s1, s2, s3, s4, kprev, kexl, kloss, klen, kexa;
    uint32_t kw;
} voice_t;
static voice_t vo[NV];
static uint32_t vo_age;
static int voice_cap = NV;
static volatile uint32_t overloads;
static uint32_t overloads_seen, calm;

static float par[P_COUNT];
/* each track's own lead sound (dabke.h DAB_SK): its knobs, then its choices; the whole instrument's of the same names
 * (par) are track 1's */
enum { S_TONE, S_BUZZ, S_DETUNE, S_BREATH, S_BODY, S_ATTACK, S_RELEASE, S_GLIDE, S_VIBRATE, S_VIBDEPTH, S_VIBDELAY,
       S_ORNSPEED, S_ORNAMOUNT, S_TRANSPOSE, S_TUNE, S_MODEL, S_PLAY, S_DOUBLE, S_ORNAMENT, S_TRILLTO, S_PICKING,
       S_BENDRANGE, NSP };
static float tpar[DAB_NTRK][NSP];
static uint8_t tplain[DAB_NTRK];        /* 1: the track's tone is its instrument's own (tone_default), not its knobs' */
static int sp_of(int id)                /* where a knob or choice is in a track's own sound, -1: not there */
{
    int k = DAB_SK(id);
    return k >= 0 ? k : id >= P_MODEL && id <= P_PICKING ? S_MODEL + id - P_MODEL : id == P_BENDRANGE ? S_BENDRANGE : -1;
}
static int sp_id(int k)                 /* and back: the knob or choice at k */
{
    return k <= S_ORNAMOUNT ? P_TONE + k : k == S_TRANSPOSE ? P_TRANSPOSE : k == S_TUNE ? P_TUNE
           : k == S_BENDRANGE ? P_BENDRANGE : P_MODEL + k - S_MODEL;
}
static int16_t ntune[12];
static uint32_t seed = 22222u;

static struct {                     /* worked out from the knobs when one moves */
    float tune, vib, vibDelay, ornD, velAmt, stabRelN;
    float d[DAB_NDRUM], dt[DAB_NDRUM], dg[DAB_NDRUM];
    float tone, acc, balLow, balHigh, glideK, bassDecN, bassRelN, bassAtk;
    float ageLp, ageHp, ageG, ageN, dg2, dn2, echoTarget, kclick;
} K;
static struct { float tune, vib, vibDelay, ornD, glideK; } KT[DAB_NTRK];   /* the same, from each track's own sound */
static int clapV;                       /* (the claps' band as last set) */

/* the lines the notes add into, a block at a time: a track each */
#define NBUS DAB_NTRK

/* the tracks' that the rest reads: the drone's note from its track, the drum tracks' own steps, the left hand's hold */
static int16_t droneNote = -1;          /* the drone's note from its track (-1: the maqam's tonic, as set) */
static uint8_t tpat[DAB_NTRK][DAB_TLANE][DAB_TPAT], dedit;   /* the tracks' own steps, 2 bits each (lanes: their drum
                                         * sound's drums in turn); the tracks with them (his), a bit each */
static uint8_t dn[DAB_NTRK], tl2;       /* how many steps his track has; LENGTH two bars, a bit a track */
static uint8_t muteM;                   /* the tracks muted, a bit each */
#define TP_GET(t, j, s) ((uint32_t)tpat[t][j][(s) >> 2] >> (((s) & 3) << 1) & 3u)
static void tp_put(int t, int j, int s, int v)
{
    uint8_t *b = &tpat[t][j][s >> 2];
    int sh = (s & 3) << 1;
    *b = (uint8_t)((*b & ~(3u << sh)) | (uint32_t)(v & 3) << sh);
}
/* The locks (dabke.h DAB_LK). The main loop puts each in its place; the audio side holds a knob at a lock's value
 * from its step on, and gives the knob its normal value back at that track's next step with a hit or a note. */
static uint32_t lk[DAB_NLOCK] PLAT_POOL;
static uint8_t lkN[DAB_NTRK], lkHeld[DAB_NTRK];   /* each track's locks; the knobs it holds now */
static uint8_t lkAt[DAB_NLKS];          /* each knob (DAB_LKK): the lock that holds it (its place + 1; 0: none) */
static uint8_t sndLk[DAB_NTRK];         /* the sound lock of the step each track is on (1 + its value; 0: none): that
                                         * step's hits play that drum sound */
static float parN[DAB_NLKS], lkU[DAB_NLK];   /* each knob's normal value (DAB_LKK); one step of a knob in its units */
/* a knob whose jump would click (a filter, an effect's amount) goes to its new value in a few blocks (4 ms): half
 * of what is left each block, then there */
#define NGL 12
#define GL_BLOCKS 6
static struct { uint8_t id, left; float to; } gl[NGL];
static int glN;
#define NMAN (DAB_NTRK + 1)             /* the manuals (dabke_key): a track's keys each, and the left keys as set */
static uint32_t keyDn[NMAN][4];         /* the keys down, a bit a note */
static uint32_t heldK[NMAN][4], heldArp[4];   /* the notes HOLD keeps sounding; the lead's in the arpeggio's set */
#define BGET(a, n) ((a)[(n) >> 5] >> ((n) & 31) & 1u)
#define BSET(a, n) ((a)[(n) >> 5] |= 1u << ((n) & 31))
#define BCLR(a, n) ((a)[(n) >> 5] &= ~(1u << ((n) & 31)))
static uint32_t droneIdle = 0x7FFFFFFFu;   /* samples since a drone key was down or held */

/* rhythm, accompaniment, arpeggio */
static uint8_t grid[4][DAB_NLANE][DAB_RMAX], glen[4], gdiv[4];
static uint8_t ownG[DAB_NOWN][DAB_NDRUM][DAB_RMAX], ownLen[DAB_NOWN] = {16, 16, 16, 16}, ownDiv[DAB_NOWN] = {4, 4, 4, 4};
static uint8_t active[4], nact = 1;
static int32_t cnt[4];
static uint32_t nextt[4], beatFix, bar, uaClock, arpPos, walkPos;
static float beatAcc, uaAcc, fadeG = 1.0f;
static int8_t fadeDir;
static uint8_t running, syncOn, accOn, fill, inFill, fillType, backStyle, bassStyle, holding, varNow;
static uint8_t lowerHeld[128];
static int nLower, chordRoot = -1, accId = -1, bassNote = -1;
static uint8_t accNotes[8];
static int nAcc;
static float dlv[DAB_NDRUM], dtune[DAB_NDRUM], ddec[DAB_NDRUM];

/* the lead's keys held, oldest first (one note at a time: the last one sounds) */
static uint8_t mstack[16];
static int nms, monoNote = -1;
static float lastPitch = 60.0f;
static uint32_t leadIdle;           /* samples since the last lead note let go (the drone's hold) */

/* movement */
static float vibPh, bendS, droneG, droneLvl, pressS;
static float pbendS, vibP;               /* the keys' pressure as a bend and as vibrato: the leant track's notes only */
static float vibPhT[DAB_NTRK], vibST[DAB_NTRK], vibPT[DAB_NTRK], bendST[DAB_NTRK], pbendST[DAB_NTRK];   /* the same,
                                         * a lead sound's on each track: its own vibrato's speed and depth, its bend range */
static uint8_t pressTr;                  /* the tracks whose keys are leant on, a bit each */
static float pitchS[DAB_NTRK];          /* each track's pitch as it sounds now (semitones): its knob's, eased in ~3 ms */
static uint32_t seedOrn = 777u;

/* the drone */
static struct { uint32_t ph1, ph2; float s1, s2, s3, s4; } DR;

/* effects */
static bq_t fBass, fBrL, fBrR, fBassT[DAB_NTRK];   /* (the bass sound's tone: one setting, a filter a track) */
static float dlpT[DAB_NTRK];            /* the kit's tone, a track's drums */
static float gm[4][BS];                 /* the mix's places: through the chorus, the left hand's, after; the effects'; the
                                         * lead's (for its meter) */
static float ensBuf[1024] PLAT_POOL, chBuf[1024] PLAT_POOL, wowL[1024] PLAT_POOL, wowR[1024] PLAT_POOL;
static int16_t echoBuf[ECHO_N] PLAT_POOL;
static int16_t slapBuf[SLAP_N] PLAT_POOL;
static float comb0[1557] PLAT_POOL, comb1[1617] PLAT_POOL, comb2[1491] PLAT_POOL, comb3[1422] PLAT_POOL, ap0[225] PLAT_POOL, ap1[556] PLAT_POOL;
static float *const COMB[4] = {comb0, comb1, comb2, comb3};
static const uint16_t COMB_N[4] = {1557, 1617, 1491, 1422};
static uint32_t combI[4], apI[2], ensW, chW, wowW, echoW, slapW;
static float combLp[4], ensPh, ensPh2, chPh, echoD = 0.3f * SR, echoLp, wowPh, wowPh2, agL, agR, ahL, ahR;
static float paH, paL, paB, paLp, paF, paHk, paLk;
static int32_t ensLive;             /* the ensemble's taps still have something in them */

/* drums */
static struct {
    float dum, dumP, dumPh, dumPh2, tek, tekPh, ka, kaPh, slap, slapPh, tabl, tablP, tablPh, tablC, tik, tikPh;
    float riq, riqS, daf, dafP, dafPh, dafJ, sag, sagPh[3], kick, kickP, kickPh, kickC;
    float clap, clapT, snare, snareN, snPh, snPh2, shk, shkA;
    int32_t clapN;
} D;
static bq_t fTek, fKa, fClap, fSn, fRiq;
static float hp1, hp2, dlp, mph[4], mhp;
static int32_t drumLive;
static const uint8_t DR_LOW[DAB_NDRUM] = {1, 0, 0, 0, 1, 0, 0, 1, 0, 1, 0, 0, 0};
static uint8_t dtr[DAB_NDRUM] = {PART_DRUMS, PART_DRUMS, PART_DRUMS, PART_DRUMS, PART_DRUMS, PART_DRUMS, PART_METAL,
                                 PART_DRUMS, PART_METAL, PART_KICK, PART_CLAPS, PART_CLAPS, PART_METAL};   /* each drum's
                                         * track as it was last hit (its sound goes out there), and that sound's number */
static uint8_t dvar[DAB_NDRUM];
/* each drum family's drums, in the order a track's lanes play them (the rhythm's lanes of the drum tracks as they
 * start: the kick's one, the darbuka and tabl's seven, the claps' two, the riq and sagat's three) */
static const uint8_t GDR[4][DAB_TLANE] = {{D_KICK}, {D_DUM, D_TEK, D_KA, D_SLAP, D_TABL, D_TIK, D_DAF}, {D_CLAP, D_SNARE},
                                          {D_RIQ, D_SAGAT, D_SHAKER}};
static const uint8_t GN[4] = {1, 7, 2, 3};
static const int8_t DR_LANE[DAB_NDRUM] = {0, 1, 2, 3, 4, 5, 0, 6, 1, 0, 0, 1, 2};   /* each drum's lane on its own track */
static const uint8_t DR_PART[DAB_NDRUM] = {PART_DRUMS, PART_DRUMS, PART_DRUMS, PART_DRUMS, PART_DRUMS, PART_DRUMS,
                                           PART_METAL, PART_DRUMS, PART_METAL, PART_KICK, PART_CLAPS, PART_CLAPS,
                                           PART_METAL};   /* the mixer's part each drum is in */
static const uint8_t PART_VOL[DAB_NPART] = {P_LEAD, P_DRONE, P_LEFT, P_BASS, P_GKICK, P_GDRUM, P_GCLAP, P_GMETAL};

/* the mixer: each part's filter (a state-variable one, solved each sample: stable to the top) and its bass (a
 * shelf: the part plus some of its own lows, or less of them). A part that is flat, or silent a while, skips both. */
#define SVF_K 1.2f                      /* the filter's damping: a little rise at its corner */
#define SHELF_C 0.0211430f              /* the bass's low-pass: 1 - e^(-2 pi 150 / 44100) */
typedef struct {
    float a1, a2, a3;                   /* the filter's coefficients, for its g as the last block left it */
    float gf, gt, gw;                   /* its g (tan(pi fc / SR)): as the last block left it; where this block takes
                                         * it; the one it is to come in with */
    float g, gS, ic1, ic2, lp;          /* its bass as set and as last mixed; the filter's and the bass's memory */
    int8_t mode, want;                  /* -1 low-pass, 0 none, 1 high-pass: what runs now, what is to run */
} strip_t;
static strip_t sp[DAB_NPART];
static int32_t partLive[DAB_NPART];     /* samples a part has still to be filtered after its last sound */
static void strip_coef(strip_t *s, float g)
{
    float a1 = 1.0f / (1.0f + g * (g + SVF_K));
    s->a1 = a1; s->a2 = g * a1; s->a3 = g * g * a1;
}
static void strip_settle(strip_t *s)    /* nothing sounds on it: the filter as it is to be, at once, its memory empty */
{
    s->mode = s->want; s->gf = s->gt = s->gw;
    strip_coef(s, s->gf);
    s->ic1 = s->ic2 = s->lp = 0.0f; s->gS = s->g;
}
/* A part's block, in place: its filter, then its bass. Nothing in it jumps while the part sounds: a filter that comes
 * in or goes out (or changes its kind, by way of none) does so across a block, faded with the sound as it is; one that
 * moves goes from the last block's place to this one's a sample at a time; the bass's amount the same. */
static void strip_block(strip_t *s, float *x, int n)
{
    float a1, a2, a3, ic1, ic2, g = s->g, g0 = s->gS, lp = s->lp, w = 0.0f, dw = 1.0f / (float)n;
    int i;
    if (s->want != s->mode) {
        int out = s->mode != 0, hp;
        if (!out) {                     /* it comes in: a low-pass at rest on the sound, a high-pass empty */
            s->mode = s->want; s->gf = s->gt = s->gw;
            strip_coef(s, s->gf);
            s->ic1 = 0.0f; s->ic2 = s->mode < 0 ? x[0] : 0.0f;
        }
        a1 = s->a1; a2 = s->a2; a3 = s->a3; ic1 = s->ic1; ic2 = s->ic2; hp = s->mode > 0;
        for (i = 0; i < n; i++) {
            float x0 = x[i] + DN, v3 = x0 - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3, y;
            ic1 = 2.0f * v1 - ic1; ic2 = 2.0f * v2 - ic2;
            y = hp ? x0 - SVF_K * v1 - v2 : v2;
            x[i] = out ? y + (x0 - y) * w : x0 + (y - x0) * w;
            w += dw;
        }
        if (out) { s->mode = 0; ic1 = ic2 = 0.0f; }
        s->ic1 = ic1; s->ic2 = ic2;
    } else if (s->mode && s->gf != s->gt) {   /* it moves: its g along a line through the block */
        float gg = s->gf, dg = (s->gt - s->gf) * dw;
        int hp = s->mode > 0;
        ic1 = s->ic1; ic2 = s->ic2;
        for (i = 0; i < n; i++) {
            float x0 = x[i] + DN, v3 = x0 - ic2, v1, v2;
            gg += dg;
            a1 = 1.0f / (1.0f + gg * (gg + SVF_K)); a2 = gg * a1; a3 = gg * a2;
            v1 = a1 * ic1 + a2 * v3; v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0f * v1 - ic1; ic2 = 2.0f * v2 - ic2;
            x[i] = hp ? x0 - SVF_K * v1 - v2 : v2;
        }
        s->gf = s->gt;
        strip_coef(s, s->gf);
        s->ic1 = ic1; s->ic2 = ic2;
    } else if (s->mode) {
        a1 = s->a1; a2 = s->a2; a3 = s->a3; ic1 = s->ic1; ic2 = s->ic2;
        if (s->mode < 0)
            for (i = 0; i < n; i++) {
                float v3 = x[i] + DN - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
                ic1 = 2.0f * v1 - ic1; ic2 = 2.0f * v2 - ic2;
                x[i] = v2;
            }
        else
            for (i = 0; i < n; i++) {
                float x0 = x[i] + DN, v3 = x0 - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
                ic1 = 2.0f * v1 - ic1; ic2 = 2.0f * v2 - ic2;
                x[i] = x0 - SVF_K * v1 - v2;
            }
        s->ic1 = ic1; s->ic2 = ic2;
    }
    if (g != g0) {                      /* its bass on its way to another amount: along a line through this block */
        float dg = (g - g0) * dw;
        for (i = 0; i < n; i++) { lp += SHELF_C * (x[i] - lp) + DN; g0 += dg; x[i] += g0 * lp; }
        s->gS = g;
    } else if (g != 0.0f)
        for (i = 0; i < n; i++) { lp += SHELF_C * (x[i] - lp) + DN; x[i] += g * lp; }
    s->lp = lp;
}
static const float DRUM_TIME[DAB_NDRUM] = {0.22f, 0.05f, 0.04f, 0.07f, 0.42f, 0.03f, 0.13f, 0.32f, 0.55f, 0.30f, 0.09f, 0.07f, 0.05f};

/* what the screen reads */
static dabke_status_t st;
static int16_t scopeRing[DAB_SCOPE], scopeOut[DAB_SCOPE];
static uint32_t scopeW, scopeDiv;

/* messages from the main loop */
enum { M_KEY, M_PAR, M_NTUNE, M_RHY, M_RUN, M_SYNC, M_ACC, M_STYLE, M_FILL, M_FADE, M_DRUM, M_OWN,
       M_TRKREC, M_UNDO, M_CLEARALL, M_LOOPSET, M_STEP, M_DHIT, M_STEPSSET, M_HOLD, M_TSND, M_PANIC,
       M_NADD, M_NDEL, M_TLEN, M_LOCK, M_LKCLEAR, M_MUTE, M_TPAR };
typedef struct { uint8_t t; int16_t a, b; float f, g, h; } msg_t;
#define QN 512u
static msg_t q[QN];
static volatile uint32_t q_w, q_r;
static uint8_t sh_rhy[4], sh_nrhy;
static uint32_t sh_ev[DAB_NTRK][DAB_LOOP_N];  /* kept loops on their way to the audio side */
static uint16_t sh_n[DAB_NTRK], sh_len[DAB_NTRK], sh_flen[DAB_NTRK];
static uint8_t sh_bars[DAB_NTRK], sh_takes[DAB_NTRK][DAB_TAKES], sh_ntakes[DAB_NTRK];
static uint8_t sh_pat[DAB_NTRK][DAB_TLANE][DAB_TPAT], sh_edit, sh_dn[DAB_NTRK], sh_two;
static uint8_t sh_own[DAB_NDRUM * DAB_RMAX];

#ifdef DAB_TRACE
void dab_trace(int drum, float amp, uint32_t clock);   /* the test harness's: each drum hit, when */
static uint64_t trace_t;
#endif

static inline float noise(void)
{
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return (float)(int32_t)seed * (1.0f / 2147483648.0f);
}
static inline float rnd01(void)                 /* the ornaments' own dice, 0..1 */
{
    seedOrn ^= seedOrn << 13; seedOrn ^= seedOrn >> 17; seedOrn ^= seedOrn << 5;
    return (float)(seedOrn >> 8) * (1.0f / 16777216.0f);
}
static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

/* ----------------------------------------------------------- the instruments --- */
/* each instrument's own knobs when the left hand or the drone plays it: tone, buzz, cents apart, breath, body,
 * attack s, release s */
static const float MD_DEF[MD_COUNT][7] = {
    {0.70f, 0.45f, 14.0f, 0.15f, 0.55f, 0.010f, 0.05f},    /* mijwiz */
    {0.85f, 0.60f, 6.0f, 0.10f, 0.75f, 0.015f, 0.06f},     /* zurna */
    {0.65f, 0.15f, 10.0f, 0.00f, 0.40f, 0.005f, 0.12f},    /* keyboard lead */
    {0.60f, 0.10f, 4.0f, 0.15f, 0.50f, 0.060f, 0.15f},     /* violin */
    {0.40f, 0.00f, 0.0f, 0.55f, 0.50f, 0.080f, 0.20f},     /* ney */
    {0.60f, 0.20f, 12.0f, 0.05f, 0.50f, 0.015f, 0.08f},    /* accordion */
    {0.55f, 0.00f, 10.0f, 0.00f, 0.40f, 0.200f, 0.60f},    /* strings */
    {0.60f, 0.10f, 0.0f, 0.00f, 0.50f, 0.005f, 0.06f},     /* organ */
    {0.75f, 0.35f, 0.0f, 0.20f, 0.50f, 0.005f, 1.50f},     /* buzuq */
    {0.45f, 0.05f, 0.0f, 0.15f, 0.35f, 0.005f, 1.10f},     /* oud */
    {0.85f, 0.10f, 0.0f, 0.10f, 0.60f, 0.005f, 0.90f},     /* qanun */
    {0.55f, 0.30f, 12.0f, 0.00f, 0.50f, 0.030f, 0.10f},    /* brass */
    {0.70f, 0.20f, 8.0f, 0.00f, 0.50f, 0.005f, 0.08f},     /* square */
};
static float kexpN(float sec) { return fm_expf(-(float)BS / (sec * SR)); }
static float kexp(float sec) { return fm_expf(-1.0f / (sec * SR)); }

static void make_tone(tone_t *t, int model, float tone, float buzz, float det, float breath, float body, float atk,
                      float rel)
{
    float fc = 300.0f * fm_exp2f(tone * 5.0f), bf;   /* 300 Hz .. 9.6 kHz */
    if (model < 0 || model >= MD_COUNT) model = MD_MIJWIZ;
    t->model = (uint8_t)model;
    t->lpQ = 1.2f; t->bpQ = 0.6f; t->bpMix = 0.0f; t->dryMix = 1.0f;
    t->drive = 1.0f + buzz * 5.0f; t->driveN = 1.0f / (0.6f + 0.4f * t->drive);
    t->noise = breath * 0.35f;
    t->detA = fm_exp2f(-det / 2400.0f); t->detB = fm_exp2f(det / 2400.0f);
    t->pw = 0.5f; t->gain = 1.0f; t->rings = rel; t->bright = 0.25f + tone * 0.7f; t->fenv = 0.0f;
    bf = 700.0f * fm_exp2f(body * 2.0f);             /* 700 Hz .. 2.8 kHz */
    switch (model) {
    case MD_MIJWIZ:                                  /* two pipes, a little apart: the beating, a nasal body, the buzz */
        t->pw = 0.46f; t->bpMix = 1.6f; t->dryMix = 0.55f; t->bpQ = 0.45f; t->gain = 0.55f; break;   /* (each gain: about -18 dB on a phrase) */
    case MD_ZURNA:                                   /* a double reed through a narrow bell: loud and nasal */
        bf *= 1.25f; t->bpMix = 2.6f; t->dryMix = 0.25f; t->bpQ = 0.25f; t->drive += 2.0f; t->gain = 0.60f; break;
    case MD_LEAD:                                    /* the keyboard's lead: two saws through a resonant low-pass */
        t->lpQ = 1.4f - body * 1.15f; t->gain = 0.50f; break;
    case MD_VIOLIN:
        bf *= 0.8f; t->bpMix = 1.0f; t->dryMix = 0.7f; t->bpQ = 0.7f; t->gain = 0.5f; break;
    case MD_NEY:                                     /* a pure tone and breath tuned to the note */
        t->noise = breath * 1.6f; t->gain = 0.38f; t->bpQ = 0.18f; break;
    case MD_ACCORDION:
        t->pw = 0.32f; t->bpMix = 0.9f; t->dryMix = 0.7f; t->bpQ = 0.6f; t->gain = 0.45f; break;
    case MD_STRINGS:
        t->gain = 0.40f; break;
    case MD_ORGAN:
        t->gain = 0.27f; break;
    case MD_BRASS:
        t->fenv = 2.5f; t->lpQ = 1.0f; t->gain = 0.55f; break;
    case MD_SQUARE:
        t->pw = 0.5f - body * 0.42f; t->gain = 0.40f; break;
    default:                                         /* the strings plucked: the pick's brightness, the body's resonance */
        t->bpF = svf_f(bf * 0.35f); t->bpMix = model == MD_OUD ? 1.2f : 0.6f; t->bpQ = 0.5f;
        t->gain = model == MD_QANUN ? 1.3f : model == MD_OUD ? 1.1f : 0.9f; t->bright = 0.08f + tone * 0.85f; break;
    }
    if (!MD_PLUCKED(model)) t->bpF = svf_f(bf);
    if (t->lpQ < 0.15f) t->lpQ = 0.15f;
    t->lpTop = svf_top(t->lpQ);
    if (t->lpTop > 1.18f) t->lpTop = 1.18f;
    t->lpF = svf_f(fc);
    if (t->lpF > t->lpTop) t->lpF = t->lpTop;
    t->atkN = 1.0f - kexpN((atk < 0.001f ? 0.001f : atk) * 0.33f);   /* (most of the way there in the attack's time) */
    t->relN = kexpN((rel < 0.01f ? 0.01f : rel) * 0.145f);            /* (60 dB down in the release's time) */
}
static void tone_default(tone_t *t, int model)
{
    const float *d;
    if (model < 0 || model >= MD_COUNT) model = MD_STRINGS;
    d = MD_DEF[model];
    make_tone(t, model, d[0], d[1], d[2], d[3], d[4], d[5], d[6]);
}

/* every note's pitch: its key, that key's tuning (the quarter tones), the transposition and the fine tuning */
static inline float key_pitch(int n) { return (float)n + (float)ntune[((n % 12) + 12) % 12] * 0.01f; }
static uint32_t scale_mask(void) { uint32_t s = (uint32_t)par[P_SCALE]; return s ? s & 0xFFFu : 0xAB5u; }
static int scale_has(int n) { return (int)(scale_mask() >> (((n % 12) + 12) % 12) & 1u); }
static float step_up(int n, int tr)    /* to the neighbour above, semitones (TRILL TO, of track tr's sound) */
{
    int k, t = (int)tpar[tr][S_TRILLTO];
    if (t == 2) return 0.5f;
    if (t == 1) return 1.0f;
    for (k = n + 1; k <= n + 4; k++) if (scale_has(k)) return key_pitch(k) - key_pitch(n);
    return 2.0f;
}
static float step_down(int n, int tr)
{
    int k, t = (int)tpar[tr][S_TRILLTO];
    if (t == 2) return 0.5f;
    if (t == 1) return 1.0f;
    for (k = n - 1; k >= n - 4; k--) if (scale_has(k)) return key_pitch(n) - key_pitch(k);
    return 2.0f;
}
static int scale_deg(int r, int k)  /* the k-th note of the maqam above key r, semitones (by its keys) */
{
    uint32_t s = scale_mask();
    int d = 0, i = 0;
    if (!(s >> (r % 12) & 1u)) {    /* r is not one of its keys: the major scale's steps */
        static const int8_t MAJ[8] = {0, 2, 4, 5, 7, 9, 11, 12};
        return MAJ[k & 7] + 12 * (k >> 3);
    }
    while (i < k && d < 36) { d++; if (s >> ((r + d) % 12) & 1u) i++; }
    return d;
}

/* ----------------------------------------------------------- what follows --- */
static uint8_t dirty;               /* a knob moved: derive() before anything that reads what it works out */
static void drum_shape(int i);
static void derive(void)
{
    int i;
    float g;
    for (i = 0; i < DAB_NTRK; i++) {    /* each track's note sound, by its family */
        tone_t *t = &TT[i];
        int x = tidx[i];
        switch (tfam[i]) {
        case SF_LEAD: {                 /* a lead sound: its track's own knobs (or its instrument's own tone) */
            const float *p = tpar[i];
            if (!tplain[i])
                make_tone(t, x, p[S_TONE], p[S_BUZZ], p[S_DETUNE], p[S_BREATH], p[S_BODY], p[S_ATTACK], p[S_RELEASE]);
            else tone_default(t, x);
            break;
        }
        case SF_LEFT:
            tone_default(t, x);
            t->gain *= 0.8f;
            break;
        case SF_BASS:
            make_tone(t, MD_ORGAN, 0.5f, x == 2 ? 0.6f : 0.1f, 0.0f, 0.0f, 0.5f, 0.004f, 0.08f);
            t->model = (uint8_t)(x < 0 || x >= DAB_BASSV ? 0 : x);
            break;
        case SF_DRONE: {                /* the drone's sound: one of the lead's instruments that hold a note */
            static const uint8_t DM[DAB_NDRONE] = {MD_MIJWIZ, MD_ORGAN, MD_STRINGS, MD_ACCORDION, MD_NEY, MD_VIOLIN, MD_BRASS};
            tone_default(t, DM[x < 0 || x >= DAB_NDRONE ? 0 : x]);
            if (t->model == MD_MIJWIZ) { t->drive = 1.0f + 0.25f * 5.0f; t->driveN = 1.0f / (0.6f + 0.4f * t->drive); }
            if (t->model == MD_BRASS) t->lpF = svf_f(2200.0f);    /* (held: its filter as it sits once the note is open) */
            break;
        }
        default:
            break;
        }
    }
    K.tune = par[P_TUNE] * 0.01f + par[P_TRANSPOSE];
    K.vib = par[P_VIBDEPTH] * 0.5f;                                     /* semitones at full depth */
    K.vibDelay = par[P_VIBDELAY];
    K.ornD = 0.5f / clampf(par[P_ORNSPEED], 2.0f, 40.0f) * SR;          /* a grace note's length, samples */
    K.velAmt = par[P_VELAMT];
    K.glideK = par[P_GLIDE] < 0.002f ? 1.0f : 1.0f - kexpN(par[P_GLIDE] * 0.5f);
    for (i = 0; i < DAB_NTRK; i++) {    /* (each track's own sound's: as the whole instrument's from its knobs) */
        const float *p = tpar[i];
        KT[i].tune = p[S_TUNE] * 0.01f + p[S_TRANSPOSE];
        KT[i].vib = p[S_VIBDEPTH] * 0.5f;
        KT[i].vibDelay = p[S_VIBDELAY];
        KT[i].ornD = 0.5f / clampf(p[S_ORNSPEED], 2.0f, 40.0f) * SR;
        KT[i].glideK = p[S_GLIDE] < 0.002f ? 1.0f : 1.0f - kexpN(p[S_GLIDE] * 0.5f);
    }
    K.stabRelN = kexpN(0.025f);          /* the backing's stabs let go in 0.17 s */
    K.bassDecN = kexpN(0.5f * par[P_BASSDECAY]); K.bassRelN = kexpN(0.06f); K.bassAtk = 1.0f - kexpN(0.003f);
    for (i = 0; i < DAB_NDRUM; i++) {
        /* each drum: its decay and tuning by the sound it was last hit with (DRUMV_*), its track's pitch */
        drum_shape(i);
        K.dg[i] = dlv[i] * (DR_LOW[i] ? (par[P_BALANCE] > 0.0f ? 1.0f - par[P_BALANCE] : 1.0f)    /* (its part's volume: */
                                     : (par[P_BALANCE] < 0.0f ? 1.0f + par[P_BALANCE] : 1.0f));  /* at the mix) */
    }
    K.tone = par[P_KITTONE] >= 0.999f ? 1.0f : 1.0f - fm_expf(-TWO_PI * 1200.0f * fm_exp2f(par[P_KITTONE] * 3.8f) / SR);
    K.acc = 1.0f + par[P_ACCENT] * 0.7f;
    K.ageLp = 1.0f - fm_expf(-TWO_PI * 16000.0f * fm_exp2f(-par[P_AGE] * 2.3f) / SR);
    K.ageHp = 1.0f - fm_expf(-TWO_PI * (30.0f + par[P_AGE] * 150.0f) / SR);
    K.ageG = 1.0f + par[P_AGE] * 2.2f; K.ageN = 1.0f / (1.0f + par[P_AGE] * 0.9f);
    K.dg2 = 1.0f + par[P_DRIVE] * 6.0f; K.dn2 = 1.0f / (0.55f + 0.45f * K.dg2);
    {
        int sy = (int)par[P_DELAYSYNC];
        float beat = 60.0f / clampf(par[P_TEMPO], 30.0f, 300.0f), tm = sy == 1 ? beat * 0.5f : sy == 2 ? beat * 0.75f
                     : sy == 3 ? beat : par[P_ECHOTIME];
        while (tm > 0.69f) tm *= 0.5f;
        K.echoTarget = clampf(tm * SR, 100.0f, (float)(ECHO_N - 4));
    }
    g = 500.0f * fm_exp2f(par[P_BRIGHT] * 4.9f);
    bq_set(&fBrL, LP, g, 0.7f); bq_set(&fBrR, LP, g, 0.7f);
    bq_set(&fBass, LP, par[P_BASSTONE] < 60.0f ? 60.0f : par[P_BASSTONE], 0.8f);
    for (i = 0; i < DAB_NTRK; i++) {     /* (each track's bass filter: the setting, its own state) */
        fBassT[i].b0 = fBass.b0; fBassT[i].b1 = fBass.b1; fBassT[i].b2 = fBass.b2; fBassT[i].a1 = fBass.a1; fBassT[i].a2 = fBass.a2;
    }
}

/* ---------------------------------------------------- a knob's move, the locks --- */
static void strip_set(int k, float v)   /* a track's filter, -1..1: a low-pass from 20 kHz (just left of 0) down to 150 Hz;
                                         * a high-pass from 20 Hz up to 2.5 kHz; 0 none. Taken up by its next block */
{
    strip_t *t = &sp[k];
    float fc, g;
    int mode;
    v = clampf(v, -1.0f, 1.0f);
    mode = v > -0.015f && v < 0.015f ? 0 : v < 0.0f ? -1 : 1;
    t->want = (int8_t)mode;
    if (!mode) return;
    fc = v < 0.0f ? 20000.0f * fm_exp2f(v * 7.06f) : 20.0f * fm_exp2f(v * 6.97f);
    g = fm_tanf(3.14159265f * fc / SR);
    t->gw = g;
    if (mode == t->mode) t->gt = g;     /* (the kind it is already: it moves there) */
}
static void shelf_set(int k, float v)   /* its lows under 150 Hz: x + g lp(x), g -0.95 (-26 dB) .. 2.2 (+10 dB) */
{
    float g = v < 0.0f ? clampf(v, -1.0f, 0.0f) * 0.95f : clampf(v, 0.0f, 1.0f) * 2.2f;
    if (sp[k].g == 0.0f && sp[k].gS == 0.0f) sp[k].lp = 0.0f;
    sp[k].g = g;
}
static int par_plain(int id)            /* read where it is used: nothing to work out again when it moves */
{
    return id <= P_DRONE || (id >= P_GDRUM && id <= P_GMETAL) || (id >= P_SEND && id <= P_SEND_LAST) || id == P_ECHO ||
           id == P_ECHOFB || id == P_REVERB || id == P_CHORUS || id == P_PA || id == P_WOW || id == P_HISS;
}
static int par_glides(int id)           /* a jump of it would click: it goes along a line (the volumes and the effects'
                                         * shares: the mix does that itself, a sample at a time) */
{
    return (id >= P_FILTER && id <= P_LOWEQ_LAST) || id == P_ECHO || id == P_ECHOFB || id == P_REVERB || id == P_CHORUS ||
           id == P_PA || id == P_WOW || id == P_HISS;
}
static void par_now(int id, float v)    /* a knob at a value, now */
{
    par[id] = v;
    if (id >= P_FILTER && id <= P_FILTER_LAST) strip_set(id - P_FILTER, v);
    else if (id >= P_LOWEQ && id <= P_LOWEQ_LAST) shelf_set(id - P_LOWEQ, v);
    else if (!par_plain(id)) dirty = 1;
}
static void par_to(int id, float v)     /* a knob to a value: at once, or in a few blocks */
{
    int i;
    for (i = 0; i < glN; i++) if (gl[i].id == id) break;
    if (!par_glides(id) || v == par[id] || (i == glN && glN >= NGL)) {
        if (i < glN) gl[i] = gl[--glN];
        par_now(id, v);
        return;
    }
    if (i == glN) glN++;
    gl[i].id = (uint8_t)id; gl[i].to = v; gl[i].left = GL_BLOCKS;
}
static void glide_block(void)           /* each block: the knobs on their way */
{
    int i;
    for (i = 0; i < glN; i++) {
        int id = gl[i].id;
        if (--gl[i].left == 0) { par_now(id, gl[i].to); gl[i] = gl[--glN]; i--; }
        else par_now(id, par[id] + (gl[i].to - par[id]) * 0.5f);
    }
}
static void kn_to(int k, float v)       /* a knob (DAB_LKK) to a value: the whole instrument's, or a track's own sound's
                                         * (track 1's: the whole instrument's of that name with it) */
{
    int t, s;
    if (k < DAB_NLK) { par_to(k, v); return; }
    t = (k - DAB_NLK) / DAB_NSK; s = (k - DAB_NLK) % DAB_NSK;
    tpar[t][s] = v; dirty = 1;
    if (!t) par_to(sp_id(s), v);
}
static int lk_knob(uint32_t l) { return DAB_LKK(DAB_LK_TRK(l), DAB_LK_ID(l)); }   /* the knob a lock holds */
static void lock_free(int k)            /* a knob no lock holds any more: its normal value again */
{
    int i = lkAt[k];
    if (!i) return;
    if (lkHeld[DAB_LK_TRK(lk[i - 1])]) lkHeld[DAB_LK_TRK(lk[i - 1])]--;
    lkAt[k] = 0; st.lk_at[k] = 0;
    kn_to(k, parN[k]);
}
static void lock_hold(int i)            /* the lock in place i holds its knob from now */
{
    uint32_t l = lk[i];
    int k = lk_knob(l), t = DAB_LK_TRK(l);
    if (lkAt[k] && lkHeld[DAB_LK_TRK(lk[lkAt[k] - 1])]) lkHeld[DAB_LK_TRK(lk[lkAt[k] - 1])]--;
    lkAt[k] = (uint8_t)(i + 1); st.lk_at[k] = lkAt[k];
    lkHeld[t]++;
    kn_to(k, (float)DAB_LK_VAL(l) * lkU[DAB_LK_ID(l)]);
}
static void lock_put(int i, uint32_t v) /* a lock in its place (0: none there now) */
{
    uint32_t old;
    int id, k;
    if (i < 0 || i >= DAB_NLOCK || (v && DAB_LK_ID(v) >= DAB_NLK && DAB_LK_ID(v) != DAB_LK_SOUND)) return;
    old = lk[i];
    if (old) {
        id = DAB_LK_ID(old);
        k = id < DAB_NLK ? lk_knob(old) : 0;
        if (id < DAB_NLK && lkAt[k] == i + 1 &&
            (!v || DAB_LK_ID(v) != id || DAB_LK_TRK(v) != DAB_LK_TRK(old) || DAB_LK_STEP(v) != DAB_LK_STEP(old)))
            lock_free(k);               /* (it held its knob: gone, or moved) */
        if (lkN[DAB_LK_TRK(old)]) lkN[DAB_LK_TRK(old)]--;
    }
    lk[i] = v;
    if (!v) return;
    id = DAB_LK_ID(v);
    lkN[DAB_LK_TRK(v)]++;
    if (id >= DAB_NLK) return;          /* (a step's sound: no knob to hold; taken when that step plays) */
    k = lk_knob(v);
    if (lkAt[k] == i + 1) kn_to(k, (float)DAB_LK_VAL(v) * lkU[id]);   /* (holding now: its new value) */
    else if (running && st.tstep[DAB_LK_TRK(v)] == DAB_LK_STEP(v)) lock_hold(i);   /* (its step plays now: from now) */
}
static void lock_clear(int t, int take) /* a track's locks gone (t < 0: every track's); take > 0: those of that take on */
{
    int i;
    for (i = 0; i < DAB_NLOCK; i++) {
        uint32_t l = lk[i];
        if (l && (t < 0 || DAB_LK_TRK(l) == t) && (take <= 0 || DAB_LK_TAKE(l) >= take)) lock_put(i, 0);
    }
}
static void lk_step(int t, int s, int trig)   /* track t comes to its step s (trig: a hit or a note begins there) */
{
    int i;
    if (trig && lkHeld[t]) {            /* what it held ends here (a lock of the same knob here: that value, below): the
                                         * whole instrument's knobs, its own sound's */
        for (i = 0; i < DAB_NLK; i++) if (lkAt[i] && DAB_LK_TRK(lk[lkAt[i] - 1]) == t) lock_free(i);
        for (i = DAB_NLK + t * DAB_NSK; i < DAB_NLK + (t + 1) * DAB_NSK; i++) if (lkAt[i]) lock_free(i);
    }
    if (lkN[t])
        for (i = 0; i < DAB_NLOCK; i++) {
            uint32_t l = lk[i];
            if (!l || DAB_LK_STEP(l) != s || DAB_LK_TRK(l) != t) continue;
            if (DAB_LK_ID(l) == DAB_LK_SOUND) sndLk[t] = (uint8_t)(1 + (DAB_LK_VAL(l) & 63));   /* (this step's sound) */
            else lock_hold(i);
        }
}
static void lk_stop(void)               /* the rhythm stops: every knob its normal value, no step's sound */
{
    int i;
    for (i = 0; i < DAB_NLKS; i++) lock_free(i);
    for (i = 0; i < DAB_NTRK; i++) sndLk[i] = 0;
}

/* ------------------------------------------------------------------- notes --- */
static void release(int m, int tr, int note)   /* a note of that role on that track let go */
{
    int i;
    for (i = 0; i < NV; i++) if (vo[i].used && vo[i].m == m && vo[i].tr == tr && vo[i].n == note && vo[i].gate) vo[i].gate = 0;
}
static int voices_used(void) { int i, used = 0; for (i = 0; i < NV; i++) if (vo[i].used) used++; return used; }
static int guard_cap = NV;              /* the notes the load's guard lets sound at once now (below) */
static int free_slot(void)
{
    int i, slot = -1, cap = voice_cap < guard_cap ? voice_cap : guard_cap;
    if (voices_used() < cap) for (i = 0; i < NV && slot < 0; i++) if (!vo[i].used) slot = i;
    if (slot < 0) {                 /* none free, or as many as the processor carries: the oldest goes, a released one first */
        int best = -1;
        for (i = 0; i < NV; i++) if (vo[i].used && (best < 0 || (!vo[i].gate && vo[best].gate) || (vo[i].gate == vo[best].gate && vo[i].vage < vo[best].vage))) best = i;
        slot = best < 0 ? 0 : best;
        if (best >= 0 && vo[best].gate) st.shed++;   /* (a note sounding let go early for this one) */
    }
    return slot;
}
static uint8_t leadTr;                  /* the track the lead's keys (and its arpeggio, its one-note playing) are on */
static float vel_gain(int vel)
{
    float x = (float)(vel < 1 ? 1 : vel > 127 ? 127 : vel) * (1.0f / 127.0f);
    return 1.0f - K.velAmt + K.velAmt * (0.3f * x + 0.7f * x * x);
}
static void ks_pluck(voice_t *v, float amp)          /* a string struck: a period of noise into its loop */
{
    v->kexc = (int32_t)v->klen + 2;
    v->kexa = amp;
}
static void orn_start(voice_t *v)       /* (its track's own sound's ornament) */
{
    const float *p = tpar[v->tr];
    int o = (int)p[S_ORNAMENT];
    v->up = step_up(v->n, v->tr); v->down = step_down(v->n, v->tr);
    v->ostep = 0; v->otime = 0; v->ot = 0.0f; v->okk = 0.6f; v->shake = -1.0f; v->oph = 0.0f;
    if (o == ORN_AUTO) {            /* the fast automatic ornament: now and then a grace, a scoop, a turn, a shake */
        float r = rnd01();
        o = ORN_OFF;
        if (r < p[S_ORNAMOUNT]) {
            float c = rnd01();
            o = c < 0.35f ? ORN_MORDENT : c < 0.6f ? ORN_SCOOP : c < 0.75f ? ORN_TURN : 7;   /* 7: two graces */
        }
        if (!MD_PLUCKED(v->model) && rnd01() < p[S_ORNAMOUNT] * 0.5f) v->shake = 0.32f;
    }
    v->orn = (uint8_t)o;
    if (o == ORN_SCOOP) { v->oo = -v->down; v->okk = 1.0f - kexpN(KT[v->tr].ornD * 1.4f / SR); }
    if (o == ORN_SHAKE) v->shake = 0.0f;
}
static void orn_block(voice_t *v, int n)             /* the ornament's offset, a block at a time */
{
    float d = KT[v->tr].ornD;
    if (v->orn == ORN_MORDENT || v->orn == ORN_TURN || v->orn == 7 || v->orn == ORN_TRILL) {
        if ((v->otime -= n) <= 0) {
            static const int8_t MOR[2] = {1, 0}, TRN[4] = {1, 0, -1, 0}, TWO[4] = {1, 0, 1, 0};
            int s = v->ostep++;
            if (v->orn == ORN_MORDENT) { if (s < 2) { v->ot = MOR[s] > 0 ? v->up : 0.0f; v->otime = s < 1 ? (int32_t)d : 0x7FFFFFFF; } }
            else if (v->orn == ORN_TURN || v->orn == 7) {
                const int8_t *p = v->orn == 7 ? TWO : TRN;
                if (s < 4) { v->ot = p[s] > 0 ? v->up : p[s] < 0 ? -v->down : 0.0f; v->otime = s < 3 ? (int32_t)d : 0x7FFFFFFF; }
            } else {                /* the trill: the note, then the neighbour and back while it is held */
                v->ot = s == 0 ? 0.0f : (s & 1) ? v->up : 0.0f;
                v->otime = s == 0 ? (int32_t)(2.0f * d) : (int32_t)d;
                if (!v->gate) { v->ot = 0.0f; v->otime = 0x7FFFFFFF; }
            }
        }
    }
    if (v->shake >= 0.0f && v->age >= v->shake && v->gate) {   /* the shake: the wheel rocked up and back */
        v->oph += clampf(tpar[v->tr][S_ORNSPEED], 2.0f, 40.0f) * 0.55f * (float)n / SR;
        if (v->oph >= 1.0f) v->oph -= 1.0f;
        v->ot = v->up * 0.5f * (1.0f - sin_turn(fracf(v->oph + 0.25f)));
        v->okk = 0.5f;
    } else if (v->shake >= 0.0f && !v->gate) v->ot = 0.0f;
    v->oo += (v->ot - v->oo) * v->okk;
}
static uint8_t ornOff;                  /* the notes started now get no ornament (a fast arpeggio's) */
static void voice_init(voice_t *v, int slot, int m, int tr, int note, int vel, int oct)
{
    const tone_t *t = &TT[tr];
    v->used = 1; v->m = (uint8_t)m; v->n = (uint8_t)note; v->gate = 1; v->oct = (int8_t)oct; v->slot = (uint8_t)slot;
    v->tr = (uint8_t)tr; v->fam = tfam[tr];
    v->model = t->model; v->vel = vel_gain(vel); v->g = 0.0f; v->pp = 1.0f; v->vage = ++vo_age;
    v->gl = 0.0f; v->oo = 0.0f; v->ot = 0.0f; v->okk = 0.6f; v->orn = ORN_OFF; v->shake = -1.0f;
    v->vamt = 0.0f; v->age = 0.0f; v->kexc = 0; v->kpick = 0; v->pick = 0;
    v->s1 = v->s2 = v->s3 = v->s4 = 0.0f; v->kprev = 0.0f; v->kexl = 0.0f; v->kw = 0;
    v->ph1 = seed; v->ph2 = seed * 2654435761u;
    v->base = key_pitch(note) + (float)oct + (v->fam == SF_LEAD ? KT[tr].tune : K.tune);   /* (a lead sound: its own) */
    if (v->fam != SF_BASS && MD_PLUCKED(t->model)) {
        float f = 440.0f * fm_exp2f((v->base - 69.0f) * (1.0f / 12.0f)), L = SR / f - 0.5f;
        int i;
        while (L > (float)(KSN - 4)) L *= 0.5f;
        v->klen = L < 2.0f ? 2.0f : L;
        for (i = 0; i < KSN; i++) ksbuf[slot][i] = 0.0f;
        v->g = 1.0f;
        ks_pluck(v, v->vel);
    }
    if (v->fam == SF_LEAD && (m == R_KEY || m == R_LOOP) && !ornOff) orn_start(v);
}
static int start(int m, int tr, int note, int vel)   /* a note of that role on that track, with the track's sound (a
                                                      * drum sound on it: none) */
{
    int slot, oct = 0, lead, dbl;
    if (tr < 0 || tr >= DAB_NTRK || SF_DRUM(tfam[tr])) return -1;
    lead = tfam[tr] == SF_LEAD && (m == R_KEY || m == R_LOOP);
    dbl = lead ? (int)tpar[tr][S_DOUBLE] : 0;
    slot = free_slot();
    st.starts[tr]++;
    release(m, tr, note);
    voice_init(&vo[slot], slot, m, tr, note, vel, 0);
    if (lead) {
        float from = lastPitch;
        if (KT[tr].glideK < 1.0f) vo[slot].gl = from - vo[slot].base;
        if (vo[slot].gl > 24.0f || vo[slot].gl < -24.0f) vo[slot].gl = 0.0f;
        lastPitch = vo[slot].base;
    }
    if (dbl) {                      /* the octave layer: a second note with it */
        int s2;
        oct = dbl == 1 ? -12 : 12;
        vo[slot].used = 2;          /* (kept while the second slot is chosen) */
        s2 = free_slot();
        vo[slot].used = 1;
        if (s2 != slot) {
            voice_init(&vo[s2], s2, m, tr, note, vel, oct);
            vo[s2].gl = vo[slot].gl; vo[s2].vel *= 0.6f;
            vo[s2].orn = vo[slot].orn; vo[s2].up = vo[slot].up; vo[s2].down = vo[slot].down; vo[s2].oo = vo[slot].oo;
            vo[s2].okk = vo[slot].okk; vo[s2].shake = vo[slot].shake;
        }
    }
    return slot;
}
static void retarget(int note, int vel)             /* one note at a time, legato: the sounding note moves to another key */
{
    int i;
    for (i = 0; i < NV; i++) {
        voice_t *v = &vo[i];
        float now, nb;
        if (!v->used || v->m != R_KEY || v->tr != leadTr || !v->gate) continue;
        now = v->base + v->gl;
        nb = key_pitch(note) + (float)v->oct + KT[v->tr].tune;
        v->n = (uint8_t)note; v->base = nb; v->gl = KT[v->tr].glideK < 1.0f ? now - nb : 0.0f;
        if (v->gl > 24.0f || v->gl < -24.0f) v->gl = 0.0f;
        v->vel += (vel_gain(vel) * (v->oct ? 0.6f : 1.0f) - v->vel) * 0.5f;
        orn_start(v); v->age = 0.0f;
        if (v->oct == 0) lastPitch = nb;
    }
}

/* ------------------------------------------------- automatic accompaniment --- */
static void chord_off(void)
{
    int i;
    for (i = 0; i < nAcc; i++) release(R_BACK, PART_LEFT, accNotes[i]);
    nAcc = 0;
}
static int loop_has(int t);             /* (track t has a loop: it is his) */
static void chord_on(void)
{
    int i, iv[3], n = 3;
    chord_off();
    accId = chordRoot;
    if (loop_has(PART_LEFT)) return;    /* (track 3 is his: what he recorded, not the backing with it) */
    if ((int)par[P_CHORDTYPE] == 1 && scale_has(chordRoot)) { iv[0] = 0; iv[1] = scale_deg(chordRoot, 2); iv[2] = scale_deg(chordRoot, 4); }
    else { iv[0] = 0; iv[1] = 7; iv[2] = 12; }
    for (i = 0; i < n; i++) {
        int nt = 48 + chordRoot + iv[i];
        start(R_BACK, PART_LEFT, nt, 100); accNotes[nAcc++] = (uint8_t)nt;
    }
    accId = chordRoot;
}
static void play_bass(int iv)
{
    int n = 28 + ((chordRoot - 4 + 24) % 12) + iv;      /* E1 .. Eb2, and above */
    if (bassNote >= 0) release(R_BASS, PART_BASS, bassNote);
    bassNote = -1;
    if (loop_has(PART_BASS)) return;    /* (track 4 is his: no bass line with it) */
    bassNote = n;
    start(R_BASS, PART_BASS, n, 100);
}
static void stop_acc(void)
{
    chord_off();
    if (bassNote >= 0) { release(R_BASS, PART_BASS, bassNote); bassNote = -1; }
}
static void hold_chord(void)
{
    stop_acc();
    if (backStyle != 3) chord_on();
    if (bassStyle != 4) play_bass(0);
    holding = 1;
}
static void find_root(void)
{
    int i, root = -1;
    for (i = 0; i < 128; i++) if (lowerHeld[i]) { root = i % 12; break; }
    if (root < 0) return;
    st.left_root = (int8_t)root;        /* (the same root again: shown again) */
    if (root != chordRoot) { chordRoot = root; if (!running) hold_chord(); }
    else if (!running && !holding) hold_chord();
}
static void acc_step(int a, int step)
{
    int div = gdiv[a];
    if (backStyle == 0) {
        uint8_t c = grid[a][DAB_LCHORD][step];
        if (c != 2) chord_off();
        if (c == 1) chord_on();
    } else if (backStyle == 1) {
        if (!nAcc || accId != chordRoot) chord_on();
    } else if (backStyle == 2) {    /* the maqam's notes in turn, an eighth at a time */
        if (div != 4 || !(step & 1)) {
            static const int8_t DEG[6] = {0, 2, 4, 7, 4, 2};
            int nt = 48 + chordRoot + scale_deg(chordRoot, DEG[arpPos++ % 6u]);
            chord_off(); start(R_BACK, PART_LEFT, nt, 90); accNotes[nAcc++] = (uint8_t)nt;
        }
    }
    if (bassStyle <= 2) {
        uint8_t b = grid[a][DAB_LBASS][step];
        if (b) {
            if (bassStyle == 1) play_bass(0);
            else if (bassStyle == 2) play_bass((walkPos++ & 1u) ? 12 : 0);
            else play_bass(b == 2 ? scale_deg(chordRoot, 4) : b == 3 ? 12 : b == 4 ? scale_deg(chordRoot, 2)
                           : b == 5 ? scale_deg(chordRoot, 3) : b == 6 ? scale_deg(chordRoot, 6) : 0);
        }
    } else if (bassStyle == 3 && step % div == 0) {
        static const int8_t WK[8] = {0, 1, 2, 3, 4, 3, 2, 1};
        play_bass(scale_deg(chordRoot, WK[walkPos++ % 8u]));
    }
}

/* ------------------------------------------------------------------- drums --- */
static void drum_shape(int i)           /* drum i's decay and tuning, by the sound and the track it is hit from */
{
    static const float VT[4][DAB_DRUMV] = {{1.0f, 0.72f, 1.4f, 0.9f}, {1.0f, 0.78f, 1.3f, 1.12f}, {1.0f, 0.85f, 1.8f, 1.0f},
                                           {1.0f, 1.35f, 0.72f, 1.0f}};
    static const float VD[4][DAB_DRUMV] = {{1.0f, 2.2f, 0.4f, 0.7f}, {1.0f, 1.6f, 0.7f, 2.4f}, {1.0f, 1.9f, 0.35f, 3.0f},
                                           {1.0f, 0.8f, 1.3f, 2.4f}};
    static const float KC[DAB_DRUMV] = {1.0f, 0.3f, 1.6f, 3.0f}, CF[DAB_DRUMV] = {1250.0f, 900.0f, 2600.0f, 1100.0f};
    static const float CQ[DAB_DRUMV] = {1.4f, 0.9f, 2.0f, 1.2f};
    int g = DR_PART[i] - PART_KICK, dv = dvar[i] & 3, tr = dtr[i];
    K.d[i] = kexp(DRUM_TIME[i] * par[P_KITDECAY] * ddec[i] * VD[g][dv]);
    K.dt[i] = par[P_KITTUNE] * dtune[i] * VT[g][dv] * fm_exp2f(par[P_PITCH + tr] * (1.0f / 12.0f)) / SR;
    if (i == D_KICK) K.kclick = KC[dv];
    if (i == D_CLAP && dv != clapV) { clapV = dv; bq_set(&fClap, BP, CF[dv], CQ[dv]); }
}
static void trig(int k, float a, int tr, int var)   /* drum k now, from track tr's sound (number var of its family) */
{
    float g = a * K.dg[k];
    if (par[P_LOOSE] > 0.001f) g *= 1.0f + par[P_LOOSE] * 0.35f * noise();
    if (g <= 0.001f || par[PART_VOL[tr]] <= 0.0f || (muteM >> tr & 1u)) return;   /* (its track turned down or
                                                                                     * muted: not played at all) */
    if (dtr[k] != tr || dvar[k] != var) { dtr[k] = (uint8_t)tr; dvar[k] = (uint8_t)var; drum_shape(k); }
    st.hits[k]++;
    st.starts[tr]++;
    partLive[tr] = (int32_t)(SR * 2.0f);
#ifdef DAB_TRACE
    dab_trace(k, g, (uint32_t)trace_t);
#endif
    drumLive = (int32_t)(SR * 2.0f);
    switch (k) {
    case D_DUM: D.dum = g; D.dumP = 1.0f; D.dumPh = 0.0f; D.dumPh2 = 0.0f; break;
    case D_TEK: D.tek = g; D.tekPh = 0.0f; break;
    case D_KA: D.ka = g * 0.7f; D.kaPh = 0.0f; break;
    case D_SLAP: D.slap = g; D.slapPh = 0.0f; break;
    case D_TABL: D.tabl = g; D.tablP = 1.0f; D.tablPh = 0.0f; D.tablC = g; break;
    case D_TIK: D.tik = g; D.tikPh = 0.0f; break;
    case D_RIQ: D.riq = g; D.riqS = g; break;
    case D_DAF: D.daf = g; D.dafP = 1.0f; D.dafPh = 0.0f; D.dafJ = g * 0.6f; break;
    case D_SAGAT: D.sag = g; break;
    case D_KICK: D.kick = g; D.kickP = 1.0f; D.kickPh = 0.0f; D.kickC = g; break;
    case D_CLAP: D.clap = g; D.clapN = 0; break;
    case D_SNARE: D.snare = g; D.snareN = g; break;
    default: D.shk = g; D.shkA = 0.0f; break;
    }
}
#define WRAP(p) do { if ((p) >= 1.0f) (p) -= 1.0f; } while (0)
static void trig_lane(int t, int lane, float a);
/* the rhythm's drum k (its steps, its variations, its fills): its track's sound plays it, unless that track is his */
#define RTRIG(k, a) do { if (!(dedit >> DR_PART[k] & 1u)) trig_lane(DR_PART[k], DR_LANE[k], a); } while (0)
static float bus[DAB_NTRK][BS];
static void drums(int i)                /* sample i of the block: each drum into the bus of the track it was hit from */
{
    const float *T = K.dt, *kd = K.d;
    float n = noise(), h1, h2;
    if (D.dum > 0.0005f) {          /* the darbuka's dum: a deep ring that drops as it starts */
        D.dumPh += (92.0f + 70.0f * D.dumP) * T[D_DUM]; WRAP(D.dumPh);
        D.dumPh2 += 215.0f * T[D_DUM]; WRAP(D.dumPh2);
        bus[dtr[D_DUM]][i] += (sin_turn(D.dumPh) + 0.25f * D.dumP * sin_turn(D.dumPh2)) * D.dum * 1.15f;
        D.dum *= kd[D_DUM]; D.dumP *= 0.9975f;
    }
    if (D.tek > 0.0005f) {          /* tek: the rim, bright, with the drum's metal ringing */
        D.tekPh += 2350.0f * T[D_TEK]; WRAP(D.tekPh);
        bus[dtr[D_TEK]][i] += (bq_run(&fTek, n) * 1.1f + sin_turn(D.tekPh) * 0.45f) * D.tek;
        D.tek *= kd[D_TEK];
    }
    if (D.ka > 0.0005f) {
        D.kaPh += 1880.0f * T[D_KA]; WRAP(D.kaPh);
        bus[dtr[D_KA]][i] += (bq_run(&fKa, n) * 0.9f + sin_turn(D.kaPh) * 0.3f) * D.ka;
        D.ka *= kd[D_KA];
    }
    if (D.slap > 0.0005f) {
        D.slapPh += 330.0f * T[D_SLAP]; WRAP(D.slapPh);
        bus[dtr[D_SLAP]][i] += (n * 0.6f + sin_turn(D.slapPh) * 0.5f) * D.slap;
        D.slap *= kd[D_SLAP];
    }
    if (D.tabl > 0.0005f) {         /* the tabl's big side: a boom and the beater */
        D.tablPh += (56.0f + 85.0f * D.tablP) * T[D_TABL]; WRAP(D.tablPh);
        bus[dtr[D_TABL]][i] += sin_turn(D.tablPh) * D.tabl * 1.3f + n * D.tablC * 0.35f;
        D.tabl *= kd[D_TABL]; D.tablP *= 0.997f; D.tablC *= 0.985f;
    }
    if (D.tik > 0.0005f) {          /* its thin stick on the other side */
        D.tikPh += 1650.0f * T[D_TIK]; WRAP(D.tikPh);
        bus[dtr[D_TIK]][i] += (n - hp1) * D.tik * 0.6f + sin_turn(D.tikPh) * D.tik * 0.3f;
        D.tik *= kd[D_TIK];
    }
    hp1 += 0.6f * (n - hp1); h1 = n - hp1;
    hp2 += 0.6f * (h1 - hp2); h2 = h1 - hp2;
    if (D.riq > 0.0005f || D.dafJ > 0.0005f) {      /* the jingles: four squares out of tune with each other */
        float m = 0.0f, j;
        int k;
        for (k = 0; k < 4; k++) { static const float MT[4] = {3110.0f, 4370.0f, 5530.0f, 6820.0f}; mph[k] += MT[k] * T[D_RIQ]; WRAP(mph[k]); m += mph[k] < 0.5f ? 0.25f : -0.25f; }
        mhp += 0.5f * (m - mhp);
        j = (m - mhp) * 0.8f + h2 * 0.5f;
        bus[dtr[D_RIQ]][i] += j * D.riq * 0.45f + bq_run(&fRiq, n) * D.riqS * 0.5f;
        bus[dtr[D_DAF]][i] += j * D.dafJ * 0.3f;
        D.riq *= kd[D_RIQ]; D.dafJ *= kd[D_RIQ]; D.riqS *= 0.992f;
    }
    if (D.daf > 0.0005f) {
        D.dafPh += (78.0f + 30.0f * D.dafP) * T[D_DAF]; WRAP(D.dafPh);
        bus[dtr[D_DAF]][i] += sin_turn(D.dafPh) * D.daf * 0.95f;
        D.daf *= kd[D_DAF]; D.dafP *= 0.996f;
    }
    if (D.sag > 0.0005f) {          /* sagat: finger cymbals, three partials apart */
        static const float SG[3] = {2930.0f, 4410.0f, 6230.0f};
        int k;
        float s = 0.0f;
        for (k = 0; k < 3; k++) { D.sagPh[k] += SG[k] * T[D_SAGAT]; WRAP(D.sagPh[k]); s += sin_turn(D.sagPh[k]); }
        bus[dtr[D_SAGAT]][i] += s * D.sag * 0.22f;
        D.sag *= kd[D_SAGAT];
    }
    if (D.kick > 0.0005f) {         /* the drum machine's kick */
        D.kickPh += (46.0f + 120.0f * D.kickP) * T[D_KICK]; WRAP(D.kickPh);
        bus[dtr[D_KICK]][i] += clip3(sin_turn(D.kickPh) * 1.4f) * D.kick * 1.1f + n * D.kickC * 0.3f * K.kclick;
        D.kick *= kd[D_KICK]; D.kickP *= 0.9965f; D.kickC *= 0.9f;
    }
    if (D.clap > 0.0005f) {         /* hands: three quick slaps and a tail */
        int32_t c = D.clapN++;
        float e;
        if (c < 1300) e = 1.0f - (float)(c % 433) * (1.0f / 433.0f);
        else { D.clapT = c == 1300 ? 0.75f : D.clapT * kd[D_CLAP]; e = D.clapT; }
        bus[dtr[D_CLAP]][i] += bq_run(&fClap, n) * e * D.clap * 1.6f;
        if (c > 1300 && D.clapT < 0.0005f) D.clap = 0.0f;
    }
    if (D.snare > 0.0005f || D.snareN > 0.0005f) {
        D.snPh += 185.0f * T[D_SNARE]; WRAP(D.snPh);
        D.snPh2 += 330.0f * T[D_SNARE]; WRAP(D.snPh2);
        bus[dtr[D_SNARE]][i] += (sin_turn(D.snPh) + 0.6f * sin_turn(D.snPh2)) * D.snare * 0.3f + bq_run(&fSn, n) * D.snareN * 0.85f;
        D.snare *= kd[D_SNARE]; D.snareN *= kd[D_SNARE] * 0.9995f;
    }
    if (D.shk > 0.0005f) {
        D.shkA += (1.0f - D.shkA) * 0.01f;
        bus[dtr[D_SHAKER]][i] += h2 * D.shk * D.shkA * 0.35f;
        D.shk *= kd[D_SHAKER];
    }
}

/* ------------------------------------------------------------------ rhythm --- */
static void compile(int r, int slot)            /* a rhythm into the slot's steps */
{
    int k, s;
    if (r < DAB_NRHYTHM) {
        int len, div;
        dabke_rhythm_grid(r, grid[slot], &len, &div);
        glen[slot] = (uint8_t)len; gdiv[slot] = (uint8_t)div;
        return;
    }
    r -= DAB_NRHYTHM;
    if (r >= DAB_NOWN) r = 0;
    glen[slot] = ownLen[r]; gdiv[slot] = ownDiv[r];
    for (k = 0; k < DAB_NDRUM; k++) for (s = 0; s < DAB_RMAX; s++) grid[slot][k][s] = ownG[r][k][s];
    for (s = 0; s < DAB_RMAX; s++) {    /* your own: the bass on its low drums, the backing on its teks */
        int lo = ownG[r][D_DUM][s] >= 2 || ownG[r][D_TABL][s] >= 2 || ownG[r][D_KICK][s] >= 2;
        grid[slot][DAB_LBASS][s] = (uint8_t)(lo ? (s * 2 >= ownLen[r] && s % ownDiv[r] ? 2 : 1) : 0);
        grid[slot][DAB_LCHORD][s] = (uint8_t)(ownG[r][D_TEK][s] >= 2 || ownG[r][D_CLAP][s] >= 2);
    }
}
static uint32_t step_time(int32_t s, int div)   /* when step s is due, with its swing */
{
    uint32_t t = (uint32_t)(s / div) * BEAT + (uint32_t)(s % div) * BEAT / (uint32_t)div;
    if ((s & 1) && div != 3) t += (uint32_t)(par[P_SWING] * 0.42f * (float)BEAT / (float)div);
    return t;
}
static void set_active(const uint8_t *list, int n)
{
    uint8_t a[4];
    int na = 0, i, k;
    for (i = 0; i < n && na < 4; i++) {
        int dup = 0;
        if (list[i] >= DAB_NRHYTHM + DAB_NOWN) continue;
        for (k = 0; k < na; k++) if (a[k] == list[i]) dup = 1;
        if (!dup) a[na++] = list[i];
    }
    if (!na) return;
    nact = (uint8_t)na;
    for (i = 0; i < na; i++) {      /* each joins on the common beat */
        int32_t s;
        active[i] = a[i];
        compile(a[i], i);
        s = (int32_t)(beatFix / BEAT * gdiv[i] + ((beatFix % BEAT) * gdiv[i] + BEAT - 1u) / BEAT);
        cnt[i] = s; nextt[i] = step_time(s, gdiv[i]);
    }
    st.steps = glen[0];
}
static void trk_runs(int go);
static void trk_first_key(void);
static void set_run(int go)
{
    int i;
    running = go ? 1 : 0;
    beatFix = 0; beatAcc = 0.0f; bar = 0; fill = 0; inFill = 0; arpPos = 0; walkPos = 0;
    for (i = 0; i < 4; i++) { cnt[i] = 0; nextt[i] = 0; }
    stop_acc(); holding = 0;
    if (!running && accOn && chordRoot >= 0 && nLower) hold_chord();
    st.running = running; st.beat = 0; st.beat_count = 0; st.bar = 0; st.in_fill = 0;
    trk_runs(go);
    if (go) trk_first_key();            /* (a track waiting to record: it records from the rhythm's start, PLAY's too) */
}
static void trk_bar(uint32_t t0);
/* A bar as LENGTH counts it: sixteen steps, or as many whole bars of the leading rhythm as fit in sixteen (twelve in
 * 6/8, fourteen in 7/8, ten in 10/8); a rhythm longer than sixteen: its own bar */
static int bar_steps(void)
{
    int L = glen[0] < 1 ? 1 : glen[0];
    return L >= 16 ? L : L * (16 / L);
}
static int trk_steps(int t)             /* a track's steps on the rhythm's clock: his own; else a bar's, or two bars' */
{
    int b;
    if ((dedit >> t & 1u) && dn[t]) return dn[t];
    b = bar_steps();
    return (tl2 >> t & 1u) && 2 * b <= DAB_TSTEPS ? 2 * b : b > DAB_TSTEPS ? DAB_TSTEPS : b;
}
static void grid_clock(int32_t s);
static void fire(int a)
{
    int32_t s = cnt[a];
    int len = glen[a], div = gdiv[a], step = (int)(s % len), k;
    uint32_t t0 = nextt[a];         /* (when this step was due) */
    cnt[a] = s + 1;
    nextt[a] = step_time(s + 1, div);
    if (a == 0) grid_clock(s);      /* (each track's step now and its locks, before this step's hits) */
    if (a == 0) {
        if (step == 0) {
            trk_bar(t0);
            int nb = (int)par[P_AUTOFILL], v = (int)par[P_VARIATION];
            if (fill == 2) {        /* out of a fill: the downbeat lands hard */
                fill = 0; inFill = 0; st.in_fill = 0;
                if (fillType == 0) { RTRIG(D_SAGAT, 1.2f); RTRIG(D_TABL, 1.2f); }
                else { RTRIG(D_DUM, 1.3f); RTRIG(D_TABL, 1.3f); RTRIG(D_KICK, 1.2f); RTRIG(D_SAGAT, 1.2f); }
            }
            bar++; st.bar = bar;
            if (nb > 0 && bar % (uint32_t)nb == 0u && !fill) { fill = 1; fillType = (uint8_t)par[P_FILLTYPE]; }
            varNow = (uint8_t)(v == 3 ? (bar % 4u == 0u ? 1 : bar % 8u == 6u ? 2 : 0) : v);
        }
        if (fill == 1 && len - step <= 2 * div) { fill = 2; inFill = 1; st.in_fill = 1; }
        if (fill == 2) {            /* the last two beats of the bar: the fill */
            int qq = step - (len - 2 * div);
            if (qq < 0) qq = 0;
            if (fillType == 0) {    /* a roll: tek and ka in turn, louder and louder, a dum on each beat */
                RTRIG((qq & 1) ? D_KA : D_TEK, 0.45f + 0.8f * (float)qq / (float)(2 * div));
                if (qq % div == 0) { RTRIG(D_DUM, 1.0f); RTRIG(D_TABL, 0.8f); }
            } else if (fillType == 1) {   /* a break: silence, then everyone on the last step */
                if (step == len - 1) { RTRIG(D_TEK, 1.3f); RTRIG(D_SLAP, 1.2f); RTRIG(D_CLAP, 1.2f); RTRIG(D_TIK, 1.2f); }
            } else if (qq % div == 0) {   /* stops: the band hits each beat together */
                RTRIG(D_DUM, 1.3f); RTRIG(D_TABL, 1.2f); RTRIG(D_CLAP, 1.1f); RTRIG(D_KICK, 1.0f);
                if (accOn && chordRoot >= 0 && bassStyle != 4) play_bass(0);
            }
        }
    }
    if (a == 0 && dedit) {              /* his tracks' own steps, lane by lane: their sounds' drums (through a fill too:
                                         * the rhythm's fills are for the tracks it drives) */
        int t, j;
        for (t = 0; t < DAB_NTRK; t++)
            if (dedit >> t & 1u) {
                int s2 = (int)(s % (dn[t] ? dn[t] : 1));
                for (j = 0; j < DAB_TLANE; j++) {
                    uint32_t c = TP_GET(t, j, s2);
                    if (c) trig_lane(t, j, c == 3 ? K.acc : c == 1 ? 0.45f : 1.0f);
                }
            }
    }
    if (!inFill) {
        int busy = a == 0 && varNow == 1, heavy = a == 0 && varNow == 2, dk = 0;
        for (k = 0; k < DAB_NDRUM; k++) {   /* (a track that is his: its own steps, above; the rhythm's not) */
            uint8_t c = grid[a][k][step];
            if (c) RTRIG(k, c == 3 ? K.acc : c == 1 ? 0.45f : 1.0f);
        }
        dk = grid[a][D_DUM][step] | grid[a][D_TEK][step] | grid[a][D_KA][step] | grid[a][D_SLAP][step];
        if (busy && !dk) RTRIG(D_KA, 0.35f + 0.1f * (float)(step & 1));   /* the ka fills every gap */
        if (busy && step % 2 == 0) RTRIG(D_RIQ, 0.5f);
        if (heavy) {                /* the tabl doubles the dums, its stick the teks, claps on the accents */
            if (grid[a][D_DUM][step] >= 2) { RTRIG(D_TABL, 1.0f); RTRIG(D_KICK, 0.8f); }
            if (grid[a][D_TEK][step] >= 2) { RTRIG(D_TIK, 1.0f); RTRIG(D_CLAP, 0.8f); }
        }
        if (a == 0) {               /* the added voices, over any rhythm */
            int b = step % div;
            if (par[P_ADDCLAP] != 0.0f && grid[a][D_TEK][step] >= 2) RTRIG(D_CLAP, 0.9f);
            if (par[P_ADDRIQ] != 0.0f && (div != 4 || !(step & 1))) RTRIG(D_RIQ, b == 0 ? 0.9f : 0.55f);
            if (par[P_ADDSAGAT] != 0.0f && (b == 0 || (div == 4 && b == 2))) RTRIG(D_SAGAT, b == 0 ? 0.9f : 0.5f);
            if (par[P_ADDKICK] != 0.0f && b == 0) RTRIG(D_KICK, 1.0f);
        }
    }
    if (a != 0) return;
    if (accOn && chordRoot >= 0 && !(inFill && fillType != 0)) acc_step(a, step);
    st.step = (uint8_t)step;
    if (step % div == 0) { st.beat = (uint8_t)(step / div); st.beats = (uint8_t)((len + div - 1) / div); st.beat_count++; }
}

/* ---------------------------------------------------------------- arpeggio --- */
/* The keys held (or kept, latched) make a set; the arpeggio plays it a note at a time (or all together: CHORD) on its
 * own grid of ticks, SPEED a beat, in time with the rhythm's beat when it runs (its own clock when not). Each tick takes
 * its pattern's step (a rest plays nothing, an accent louder); each note sounds REPEAT times before the next; the
 * order walks the set over the octaves of OVER. The notes are the keys' own, so the maqam's quarter tones go with them
 * into every octave. A note shorter than four of the ornaments' graces gets no ornament. */
static const char *const ARP_PAT[DAB_ARP_NPAT] = DAB_ARP_PATTERNS;
static uint8_t arpNotes[16], arpOrd[16], arpDown[128];   /* the set lowest first; in the order the keys went down; held */
static int nArp, nDown;
static uint32_t arpK, arpNext, arpPlayed;   /* the next tick, when it is due, the notes played in this set */
static uint8_t arpSync = 1, arpWasRun;   /* (a new set or another clock: the grid found again) */
static int32_t arpGather;               /* a new set's first note waits this many samples for the rest of a chord */
typedef struct { uint8_t note; uint32_t off; } asnd_t;
static asnd_t arpSnd[16];               /* the arpeggio's notes sounding, each until its time */
static uint8_t arpTr;                   /* the track it plays on (a lead sound's, its keys') */
static int nArpSnd;
static void trk_key(int manual, int note, int vel);
static int arp_rate(void)
{
    int r = (int)par[P_ARPRATE];
    return r < 1 ? 4 : r > 8 ? 8 : r;
}
static uint32_t arp_time(uint32_t k, int r)   /* when tick k is due: on the beat's grid, the odd ones swung */
{
    uint32_t t = (k / (uint32_t)r) * BEAT + (k % (uint32_t)r) * BEAT / (uint32_t)r;
    float sw = par[P_ARPSWING] < 0.0f ? par[P_SWING] : par[P_ARPSWING];
    if ((k & 1u) && r != 3 && r != 6 && sw > 0.001f) t += (uint32_t)(sw * 0.42f * (float)BEAT / (float)r);
    return t;
}
static void arp_off_all(void)
{
    int i;
    for (i = 0; i < nArpSnd; i++) { release(R_KEY, arpTr, arpSnd[i].note); trk_key(arpTr, arpSnd[i].note, 0); }
    nArpSnd = 0;
}
static void arp_clear(void)             /* the set gone (the sounding notes end at their time) */
{
    nArp = 0; arpPlayed = 0; arpSync = 1; st.arp_n = 0;
}
static void arp_flush(void)             /* ARP off: its notes end; the keys still held sound as they are */
{
    int i;
    arp_off_all();
    arp_clear();
    for (i = 0; i < 128; i++) {         /* (the held ones too, held still) */
        int h = (int)BGET(heldArp, i);
        if (h) BCLR(heldArp, i);
        if (arpDown[i] || h) { arpDown[i] = 0; start(R_KEY, arpTr, i, 100); }
    }
    nDown = 0;
}
static void arp_forget(void)            /* the arpeggio leaves its track (another track's keys now, or that track's
                                         * sound changed): its notes end there, its set is gone, its keys still down
                                         * are plain keys with nothing sounding, its held notes are let go. (Left as
                                         * they were, ARP off or HOLD let go started or ended them on the wrong track:
                                         * a note that never stopped) */
{
    int n, m, held = 0;
    arp_off_all();
    arp_clear();
    for (n = 0; n < 128; n++) {
        arpDown[n] = 0;
        if (BGET(heldArp, n)) { BCLR(heldArp, n); if (arpTr < DAB_NTRK) BCLR(heldK[arpTr], n); }
    }
    nDown = 0;
    for (m = 0; m < NMAN; m++) for (n = 0; n < 4; n++) { uint32_t w = heldK[m][n]; while (w) { held++; w &= w - 1u; } }
    st.held_n = (uint8_t)held; st.hold = held != 0;
}
static void arp_add(int note)
{
    int i;
    for (i = 0; i < nArp; i++) if (arpNotes[i] == note) return;
    if (nArp >= 16) return;
    arpOrd[nArp] = (uint8_t)note;
    for (i = nArp; i > 0 && arpNotes[i - 1] > note; i--) arpNotes[i] = arpNotes[i - 1];
    arpNotes[i] = (uint8_t)note; nArp++;
}
static void arp_remove(int note)
{
    int i, k;
    for (i = 0; i < nArp; i++) if (arpNotes[i] == note) { for (k = i; k < nArp - 1; k++) arpNotes[k] = arpNotes[k + 1]; break; }
    for (i = 0; i < nArp; i++) if (arpOrd[i] == note) { for (k = i; k < nArp - 1; k++) arpOrd[k] = arpOrd[k + 1]; nArp--; break; }
    if (!nArp) arp_clear();
}
static void arp_latch_off(void)         /* LATCH let go: only the keys held stay in the set */
{
    int i;
    for (i = nArp - 1; i >= 0; i--) if (!arpDown[arpNotes[i]] && !BGET(heldArp, arpNotes[i])) arp_remove(arpNotes[i]);
}
static void arp_sound(int note, int vel, uint32_t off)
{
    if (note < 0 || note > 127) return;
    if (nArpSnd >= 16) {                /* (full: the oldest ends) */
        int i;
        release(R_KEY, arpTr, arpSnd[0].note); trk_key(arpTr, arpSnd[0].note, 0);
        for (i = 0; i < 15; i++) arpSnd[i] = arpSnd[i + 1];
        nArpSnd = 15;
    }
    start(R_KEY, arpTr, note, vel);
    trk_key(arpTr, note, vel);          /* (its track records what is heard: the arpeggio's notes) */
    arpSnd[nArpSnd].note = (uint8_t)note; arpSnd[nArpSnd].off = off; nArpSnd++;
    st.arp_plays++; st.arp_last = (uint8_t)note;
    if (st.arp_count < 16u) st.arp_first[st.arp_count] = (uint8_t)note;
    if (st.arp_count < 65535u) st.arp_count++;
}
static void arp_tick(uint32_t k, uint32_t t, int r)   /* tick k, due at t */
{
    static const int8_t OLO[ARPO_N] = {0, 0, 0, 0, -1, -2, -1}, OHI[ARPO_N] = {0, 1, 2, 3, 0, 0, 1};
    int pat = (int)par[P_ARPPAT], mode = (int)par[P_ARPMODE], o = (int)par[P_ARPOCT], rep = (int)par[P_ARPREP];
    int lo, noct, L, i, vel, n = nArp;
    char c;
    uint32_t pos, tick = BEAT / (uint32_t)r, off;
    float g = par[P_ARPGATE];
    if (pat < 0 || pat >= DAB_ARP_NPAT) pat = 0;
    if (o < 0 || o >= ARPO_N) o = 0;
    if (rep < 1) rep = 1;
    c = ARP_PAT[pat][k % 8u];
    st.arp_step = (uint8_t)(k % 8u);
    if (c == '.' || !n) return;
    vel = c == 'X' ? 124 : 96;
    lo = OLO[o]; noct = OHI[o] - lo + 1; L = n * noct;
    pos = arpPlayed++ / (uint32_t)rep;
    off = g >= 0.999f ? t + tick + tick / 8u : t + (uint32_t)(g * (float)tick);   /* (at the top: into the next note) */
    ornOff = SR * 60.0f / (par[P_TEMPO] < 20.0f ? 20.0f : par[P_TEMPO]) / (float)r < KT[arpTr & 7].ornD * 4.0f;
    if (mode == ARP_CHORD) {            /* all the set together, an octave a time */
        int oc = lo + (int)(pos % (uint32_t)noct);
        for (i = 0; i < n; i++) arp_sound(arpNotes[i] + 12 * oc, vel, off);
        ornOff = 0;
        return;
    }
    switch (mode) {
    case ARP_DOWN: i = L - 1 - (int)(pos % (uint32_t)L); break;
    case ARP_UPDOWN: case ARP_DOWNUP: {
        int per = 2 * L - 2 < 1 ? 1 : 2 * L - 2, q = (int)(pos % (uint32_t)per);
        i = q < L ? q : per - q;
        if (mode == ARP_DOWNUP) i = L - 1 - i;
        break;
    }
    case ARP_ANY: i = (int)(rnd01() * (float)L); if (i > L - 1) i = L - 1; break;
    case ARP_INOUT: { int q = (int)(pos % (uint32_t)L); i = (q & 1) ? L - 1 - q / 2 : q / 2; break; }
    case ARP_THUMB: {                   /* the lowest between each of the others */
        int per = L < 2 ? 1 : 2 * (L - 1), q = (int)(pos % (uint32_t)per);
        i = (q & 1) ? 1 + q / 2 : 0;
        break;
    }
    default: i = (int)(pos % (uint32_t)L); break;   /* UP; AS PLAYED (its own list, below) */
    }
    arp_sound((mode == ARP_PLAYED ? arpOrd[i % n] : arpNotes[i % n]) + 12 * (lo + i / n), vel, off);
    ornOff = 0;
}
static void arp_step(uint32_t clock, int n)   /* each block of n: notes ending, the ticks due */
{
    int r = arp_rate(), i, guard = 16;
    for (i = 0; i < nArpSnd; i++)
        if ((int32_t)(clock - arpSnd[i].off) >= 0) {
            release(R_KEY, arpTr, arpSnd[i].note); trk_key(arpTr, arpSnd[i].note, 0);
            arpSnd[i] = arpSnd[--nArpSnd]; i--;
        }
    if (!nArp) return;
    if (arpGather > 0) { arpGather -= n; return; }   /* (20 ms: the keys of a chord are seldom down at once) */
    if (arpSync || arpWasRun != running) {   /* the grid found again: the tick now plays at once, then the beat's grid */
        uint32_t kf = (clock / BEAT) * (uint32_t)r + (clock % BEAT) * (uint32_t)r / BEAT;
        int first = arpSync;
        arpSync = 0; arpWasRun = running;
        arpK = kf + 1u; arpNext = arp_time(arpK, r);
        if (first) arp_tick(kf, clock, r);
    }
    while ((int32_t)(clock - arpNext) >= 0 && guard-- > 0) {
        arp_tick(arpK, arpNext, r);
        arpK++; arpNext = arp_time(arpK, r);
    }
    st.arp_n = (uint8_t)nArp;
}

/* -------------------------------------------------------------------- keys --- */
static void lower_hold(int note, uint8_t bit, int on)   /* a left key held, by a finger (bit 1) or by the loop (2) */
{
    uint8_t was = lowerHeld[note];
    lowerHeld[note] = (uint8_t)(on ? was | bit : was & ~bit);
    if (!was && lowerHeld[note]) nLower++;
    else if (was && !lowerHeld[note]) nLower--;
}
static void left_after_off(void)        /* a left key let go, the left hand playing bass and backing */
{
    int i;
    if (!running && !nLower) { stop_acc(); holding = 0; }
    if (!nLower) st.left_root = -1;
    else { int r = -1; for (i = 0; i < 128; i++) if (lowerHeld[i]) { r = i % 12; break; } if (r >= 0 && r != chordRoot) find_root(); }
}
static void trk_first_key(void);
static void trk_key(int manual, int note, int vel);
static int lead_poly(void) { return (int)tpar[leadTr][S_PLAY] == 2; }   /* (the lead's track's own sound's PLAY) */
#define LEADV(i) (vo[i].used && vo[i].m == R_KEY && vo[i].tr == leadTr && vo[i].gate)
static void lead_on(int note, int vel)  /* a lead sound's key, on its track (leadTr): one note or many, as it plays */
{
    int i, legato = (int)tpar[leadTr][S_PLAY] == 0 && !MD_PLUCKED(TT[leadTr].model), sounding = 0;
    if (lead_poly()) { start(R_KEY, leadTr, note, vel); return; }
    for (i = 0; i < nms; i++) if (mstack[i] == note) { for (; i < nms - 1; i++) mstack[i] = mstack[i + 1]; nms--; break; }
    if (nms >= 16) { for (i = 0; i < 15; i++) mstack[i] = mstack[i + 1]; nms = 15; }
    mstack[nms++] = (uint8_t)note;
    for (i = 0; i < NV; i++) if (LEADV(i)) sounding = 1;
    if (legato && sounding && monoNote >= 0) retarget(note, vel);
    else {
        for (i = 0; i < NV; i++) if (LEADV(i)) vo[i].gate = 0;
        start(R_KEY, leadTr, note, vel);
    }
    monoNote = note;
}
static void lead_off(int note)
{
    int i, was = -1;
    if (lead_poly()) { release(R_KEY, leadTr, note); return; }
    for (i = 0; i < nms; i++) if (mstack[i] == note) { was = i; break; }
    if (was < 0) { release(R_KEY, leadTr, note); return; }
    for (i = was; i < nms - 1; i++) mstack[i] = mstack[i + 1];
    nms--;
    if (note != monoNote) return;
    if (nms > 0) {                  /* back to the key still held */
        int top = mstack[nms - 1];
        if ((int)tpar[leadTr][S_PLAY] == 0 && !MD_PLUCKED(TT[leadTr].model)) retarget(top, 100);
        else { for (i = 0; i < NV; i++) if (LEADV(i)) vo[i].gate = 0; start(R_KEY, leadTr, top, 100); }
        monoNote = top;
    } else {
        for (i = 0; i < NV; i++) if (LEADV(i)) vo[i].gate = 0;
        monoNote = -1;
    }
}
static void lead_track(int t)           /* the lead's keys move to another track: its one-note playing starts afresh */
{
    if (t == leadTr) return;
    leadTr = (uint8_t)t; nms = 0; monoNote = -1;
}
#define KTRK(m) ((m) < DAB_NTRK ? (m) : PART_LEFT)   /* a manual's track (8, the left keys as set: the left hand's) */
static int arp_keys(int m)              /* a manual whose keys go into the arpeggio: a lead sound's, with ARP on */
{
    return m < DAB_NTRK && m != PART_DRONE && tfam[m] == SF_LEAD && par[P_ARPON] != 0.0f;
}
static void key_on(int manual, int note, int vel, int norec)   /* (norec: heard, not recorded: a note given to a step) */
{
    int t;
    if (note < 0 || note > 127 || manual < 0 || manual >= NMAN) return;
    t = KTRK(manual);
    st.notes++;
    if (!norec) trk_first_key();        /* (waiting to record: the first key starts) */
    if (!arp_keys(manual) && !norec)
        trk_key(t, note, vel);          /* (recording: what is played goes into its track; the arpeggio's keys: its notes) */
    BSET(keyDn[manual], note);
    if (BGET(heldK[manual], note)) {    /* (a held note's key again: the key has it now, it ends when let go) */
        BCLR(heldK[manual], note);
        BCLR(heldArp, note);
        if (st.held_n) st.held_n--;
        st.hold = st.held_n != 0;
        if (manual == DAB_NTRK) lower_hold(note, 4, 0);
    }
    if (manual == DAB_NTRK) {           /* the left keys as set: the bass and backing's chord, or the left hand's notes */
        lower_hold(note, 1, 1);
        if (syncOn && !running) set_run(1);
        if (accOn) { find_root(); return; }
        start(R_KEY, PART_LEFT, note, vel);
        return;
    }
    if (SF_DRUM(tfam[t])) return;       /* (a drum sound: its keys hit its drums, dabke_track_hit) */
    if (t == PART_DRONE) { droneNote = (int16_t)note; return; }   /* the drone's track: its line's note (it sounds while
                                                                    * a key is down) */
    if (tfam[t] != SF_LEAD) { start(R_KEY, t, note, vel); return; }
    if (par[P_ARPON] != 0.0f) {        /* into the arpeggio's set (latched, with no key held: a new set, the held notes
                                         * in it still) */
        if (arpTr != t) arp_forget();
        arpTr = (uint8_t)t;
        if (par[P_ARPLATCH] != 0.0f && !nDown) {
            int k;
            arp_clear();
            for (k = 0; k < 128; k++) if (BGET(heldArp, k)) arp_add(k);
        }
        if (!arpDown[note]) { arpDown[note] = 1; nDown++; }
        if (!nArp) {                    /* a new set: counted from here; its first note once the chord is in */
            int k;
            arpSync = 1; arpGather = (int32_t)(SR * 0.02f); st.arp_count = 0;
            for (k = 0; k < 16; k++) st.arp_first[k] = 0;
        }
        arp_add(note);
        st.arp_n = (uint8_t)nArp;
        return;
    }
    lead_track(t);
    lead_on(note, vel);
}
static void orn_settle(int t, int note)  /* a held note: its ornament was at its start; no trill or shake for ever */
{
    int i;
    for (i = 0; i < NV; i++) {
        voice_t *v = &vo[i];
        if (!v->used || v->tr != t || v->n != note || !v->gate) continue;
        if (v->orn == ORN_TRILL) { v->orn = ORN_OFF; v->ot = 0.0f; v->otime = 0x7FFFFFFF; }
        if (v->shake >= 0.0f) { v->shake = -1.0f; v->ot = 0.0f; }
    }
}
static void key_off(int manual, int note)
{
    int t;
    if (note < 0 || note > 127 || manual < 0 || manual >= NMAN) return;
    t = KTRK(manual);
    BCLR(keyDn[manual], note);
    if (BGET(heldK[manual], note)) {    /* HOLD keeps it: it sounds on (recorded until the hold lets it go) */
        if (arpDown[note] && arp_keys(manual)) { arpDown[note] = 0; nDown--; }   /* (in the arpeggio's set still) */
        if (manual == DAB_NTRK) { lower_hold(note, 4, 1); lower_hold(note, 1, 0); }
        orn_settle(t, note);
        return;
    }
    if (!(arp_keys(manual) && arpDown[note]))
        trk_key(t, note, 0);
    if (manual == DAB_NTRK) {
        lower_hold(note, 1, 0);
        if (accOn) { left_after_off(); return; }
        release(R_KEY, PART_LEFT, note);
        return;
    }
    if (SF_DRUM(tfam[t]) || t == PART_DRONE) return;   /* (the drone holds its note) */
    if (tfam[t] != SF_LEAD) { release(R_KEY, t, note); return; }
    if (arpDown[note]) {                /* an arpeggio's key: out of the set (latched: it stays) */
        arpDown[note] = 0; nDown--;
        if (par[P_ARPLATCH] == 0.0f) arp_remove(note);
        st.arp_n = (uint8_t)nArp;
        return;
    }
    if (t != leadTr) { release(R_KEY, t, note); return; }
    lead_off(note);
}

/* --------------------------------------------------------------- the tracks --- */
/* The eight tracks are the mixer's parts. Each keeps a loop (what a note sound plays): what was played on it, on the
 * rhythm's steps (the arpeggio's when finer), a whole number of bars, or written step by step, played in time while
 * the rhythm runs; each recording in which something was played is a take. The drone's loop moves the drone's note.
 * And each keeps steps (what a drum sound plays), its own once changed. One track records at a time. A track with
 * something of its player's is his: the rhythm leaves it alone. All of it on the audio side, on whole ticks (DAB_TPB
 * a beat); the clocks compared by difference, so their wrapping does no harm. */
typedef struct {
    uint32_t ev[DAB_LOOP_N];            /* its notes, DAB_EV (a note still being recorded: length 0) */
    uint16_t n, len;                    /* its notes; its length in ticks (0: none) */
    uint16_t flen;                      /* its length as it was played (0: written step by step) */
    uint8_t bars, takes, take[DAB_TAKES];
    uint32_t base, next;                /* this pass's start (a clock); when its next tick is due */
    uint16_t tk;                        /* that tick */
    uint8_t wait;                       /* (a take just ended: it plays from the next bar) */
    uint8_t snd[DAB_LOOP_N / 8];        /* its notes sounding, a bit each */
} ktrack_t;
static ktrack_t kt[DAB_NTRK];
static int loop_has(int t) { return kt[t].len != 0; }
#define LOOP_POOL (DAB_LOOP_N * 4)      /* the loops' notes, all tracks together, at most (as the settings keep them) */
static int loop_total(void) { int t, c = 0; for (t = 0; t < DAB_NTRK; t++) c += kt[t].n; return c; }
static int8_t recTrk = -1;              /* the track recording, -1 none */
static uint8_t recWait, recOver, recBars, recQ = 6, recMax = DAB_LOOP_BARS;   /* waiting for its first key; over its
                                         * loop; bars so far (a first take); ticks of a step; its bars at most */
static uint16_t recFrom, recBarT;       /* where this take's notes begin; a bar, in ticks */
static uint32_t recAnchor, barAt;       /* the take's first bar's start; the leading rhythm's bar's */
static int16_t recOpen[128];            /* the note each key is recording (+1; 0 none) */
/* A drum sound's last takes can be taken back, the newest first: its steps as they were before each. A take: a
 * recording in which a hit was recorded, and what he wrote by hand after a recording (what he wrote before any take is
 * no take: taken back, the track is empty). DAB_SNAPS are kept, of all the tracks together; the oldest goes first. */
#define DAB_SNAPS 4
typedef struct {
    int8_t t;                           /* its track */
    uint8_t n, his, hand;               /* its steps then, his then; this take is written by hand */
    uint8_t p[DAB_TLANE][DAB_TPAT];
} snap_t;
static snap_t snaps[DAB_SNAPS];         /* oldest first */
static uint8_t nSnap, drumTake;         /* how many; 1 + the track whose last recording kept a take */
static int8_t snapArm = -1;             /* the track whose next recorded hit begins a take */
static uint16_t takeHand[DAB_NTRK];     /* a loop's takes written by hand, a bit a take (the others: recordings). What he
                                         * writes by hand after a recording is a take of its own */
static int snap_top(int t)              /* track t's newest kept take (-1: none) */
{
    int i;
    for (i = (int)nSnap - 1; i >= 0; i--) if (snaps[i].t == t) return i;
    return -1;
}

static int arp_rate(void);
static int step_ticks(void) { return DAB_TPB / (gdiv[0] ? gdiv[0] : 4); }   /* a step of the leading rhythm, ticks */
static int bars_of(int ticks)           /* a loop's length in the leading rhythm's bars, for the screen (1 .. 8) */
{
    int bt = glen[0] * step_ticks(), b;
    if (bt < 1) bt = 1;
    b = (ticks + bt - 1) / bt;
    return b < 1 ? 1 : b > DAB_LOOP_BARS ? DAB_LOOP_BARS : b;
}
static void takes_tell(void)            /* each track's takes that can be taken back; the tracks whose last one he wrote
                                         * by hand */
{
    int t, i;
    uint8_t h = 0;
    for (t = 0; t < DAB_NTRK; t++) {
        int c = 0, top = -1;
        for (i = 0; i < nSnap; i++) if (snaps[i].t == t) { c++; top = i; }
        st.dtakes[t] = (uint8_t)c;
        if (SF_DRUM(tfam[t]) ? top >= 0 && snaps[top].hand : kt[t].takes && (takeHand[t] >> (kt[t].takes - 1) & 1u))
            h |= (uint8_t)(1u << t);
    }
    st.hand = h; st.drum_take = drumTake;
}
static void trk_tell(int notes)
{
    int t;
    takes_tell();
    st.rec_track = recTrk; st.rec_waiting = recWait; st.overdub = recOver;
    for (t = 0; t < DAB_NTRK; t++) {
        st.trk_len[t] = kt[t].len; st.trk_n[t] = kt[t].n; st.trk_takes[t] = kt[t].takes; st.trk_ev[t] = kt[t].ev;
        st.trk_played[t] = kt[t].flen;
        st.trk_bars[t] = recTrk == t && !recOver ? recBars : kt[t].bars;
        if (notes) { int k; for (k = 0; k < DAB_TAKES; k++) st.trk_take[t][k] = kt[t].take[k]; }
    }
    if (notes) st.loop_ver++;
}
static void trk_note(int t, int note, int on)   /* track t's note sounds (or stops) in that track's own way (a drum
                                                 * sound on it: silent, the loop kept) */
{
    if (t == PART_DRONE) { if (on && !SF_DRUM(tfam[t])) droneNote = (int16_t)note; }   /* (the drone holds its last note) */
    else if (t == PART_LEFT && accOn && !SF_DRUM(tfam[t])) {   /* (the left hand's loop: its own notes, as he played
                                                                * them, and the chord the bass line follows) */
        if (on) { start(R_LOOP, t, note, 100); lower_hold(note, 2, 1); find_root(); }
        else {
            release(R_LOOP, t, note);
            if (lowerHeld[note] & 2) { lower_hold(note, 2, 0); left_after_off(); }
        }
    } else if (on) start(R_LOOP, t, note, 100);
    else {
        release(R_LOOP, t, note);
        if (t == PART_LEFT && (lowerHeld[note] & 2)) { lower_hold(note, 2, 0); if (accOn) left_after_off(); }
    }
}
static void trk_off(int t)              /* track t's notes all stop */
{
    int i;
    for (i = 0; i < kt[t].n; i++)
        if (kt[t].snd[i >> 3] >> (i & 7) & 1u) { kt[t].snd[i >> 3] &= (uint8_t)~(1u << (i & 7)); trk_note(t, (int)DAB_EV_NOTE(kt[t].ev[i]), 0); }
    if (t == PART_DRONE) droneNote = -1;
}
static void auto_hush(int t)            /* track t has a loop now: it is his, the automatic part on it stops */
{
    if (t == PART_LEFT) chord_off();
    else if (t == PART_BASS && bassNote >= 0) { release(R_BASS, PART_BASS, bassNote); bassNote = -1; }
}
static void trk_empty(int t)            /* track t's loop gone */
{
    trk_off(t);
    kt[t].n = 0; kt[t].len = 0; kt[t].bars = 0; kt[t].takes = 0; kt[t].flen = 0;
}
static uint32_t trk_time(int t, uint32_t k) { return kt[t].base + k * BEAT / DAB_TPB; }
static void trk_join(int t, uint32_t base)   /* its first tick at base (a bar's start): on from where that puts it now */
{
    ktrack_t *k = &kt[t];
    k->base = base; k->tk = 0; k->wait = 0;
    while (k->tk < k->len && (int32_t)(beatFix - trk_time(t, k->tk)) > 0) k->tk++;
    if (k->tk >= k->len) { k->tk = 0; k->base += (uint32_t)k->len * BEAT / DAB_TPB; }
    k->next = trk_time(t, k->tk);
}
static void trk_align(int t)            /* a loop written step by step (or given another length) while the rhythm runs:
                                         * its first step where the rhythm's clock puts it, as the drum tracks' steps
                                         * are, so a step 5 written on one track sounds with a step 5 on another */
{
    ktrack_t *k = &kt[t];
    int q = DAB_TPB / (gdiv[0] ? gdiv[0] : 4), n = k->len / q, d = gdiv[0] ? gdiv[0] : 4;
    int32_t cur = cnt[0] > 0 ? cnt[0] - 1 : 0, s0;
    if (!running || !k->len) return;
    if (n < 1 || k->len % q) { trk_join(t, barAt); return; }
    s0 = cur - cur % n;
    trk_join(t, (uint32_t)(s0 / d) * BEAT + (uint32_t)(s0 % d) * BEAT / (uint32_t)d);
}
static void trk_grid(void)              /* a first take's steps: the rhythm's, or the arpeggio's (finer, a multiple) */
{
    int d0 = gdiv[0] ? gdiv[0] : 4, d = d0, r = arp_rate(), m;
    if (par[P_ARPON] != 0.0f && r > d0 && r % d0 == 0) d = r;
    recQ = (uint8_t)(DAB_TPB / d);
    recBarT = (uint16_t)(glen[0] * (DAB_TPB / d0));
    if (recBarT < 1) recBarT = 1;
    m = 2047 / recBarT;
    recMax = (uint8_t)(m < 1 ? 1 : m > DAB_LOOP_BARS ? DAB_LOOP_BARS : m);
}
static void trk_stop(void)              /* the recording stops at once; what was played stays (a first take: as many
                                         * bars as were begun, playing from the next bar) */
{
    int t = recTrk, i;
    ktrack_t *k;
    if (t < 0) return;
    recTrk = -1; recWait = 0; snapArm = -1;
    if (SF_DRUM(tfam[t])) { trk_tell(0); return; }
    k = &kt[t];
    for (i = 0; i < 128; i++) if (recOpen[i]) {   /* the keys still held end here */
        uint32_t *e = &k->ev[recOpen[i] - 1];
        int32_t rel = (int32_t)(beatFix - (recOver ? k->base : recAnchor)) * DAB_TPB / (int32_t)BEAT;
        int32_t ln = rel - (int32_t)DAB_EV_POS(*e);
        if (recOver && k->len) ln = ((ln % (int32_t)k->len) + (int32_t)k->len) % (int32_t)k->len;
        if (ln < recQ) ln = recQ;
        *e = DAB_EV(DAB_EV_POS(*e), ln > 2047 ? 2047 : ln, DAB_EV_NOTE(*e));
        recOpen[i] = 0;
    }
    if (!recOver) {                     /* the first take: the bars begun, whole */
        k->bars = (uint8_t)(recBars < 1 ? 1 : recBars);
        k->len = (uint16_t)(k->bars * recBarT);
        for (i = 0; i < k->n; i++) {
            uint32_t e = k->ev[i], pos = DAB_EV_POS(e) % k->len, ln = DAB_EV_LEN(e);
            if (pos + ln > k->len) ln = k->len - pos;
            k->ev[i] = DAB_EV(pos, ln, DAB_EV_NOTE(e));
        }
        if (!k->n) k->len = 0, k->bars = 0;
        k->wait = k->len > 0;
        k->flen = k->len;
        st.tstep[t] = 0;                /* (it plays from the next bar: its first step until then) */
        if (k->len) auto_hush(t);
    }
    if (k->n > recFrom) {               /* something played: a take */
        if (k->takes >= DAB_TAKES) {    /* (too many: the oldest two become one) */
            for (i = 1; i < DAB_TAKES - 1; i++) k->take[i] = k->take[i + 1];
            takeHand[t] = (uint16_t)((takeHand[t] & 1u) | (takeHand[t] >> 1 & 0x7FFEu));
            k->takes = DAB_TAKES - 1;
        }
        takeHand[t] &= (uint16_t)~(1u << k->takes);
        k->take[k->takes++] = (uint8_t)recFrom;
    }
    recOver = 0;
    trk_tell(1);
}
static void trk_rec(int t)              /* REC, a REC ARM: track t records; again (or -1): it stops at once */
{
    if (recTrk >= 0 && (t < 0 || t == recTrk)) { trk_stop(); return; }
    if (t < 0 || t >= DAB_NPART) return;
    trk_stop();
    recTrk = (int8_t)t; recWait = !running;
    if (SF_DRUM(tfam[t])) { snapArm = (int8_t)t; if (drumTake == t + 1) drumTake = 0; }   /* (its first recorded hit begins
                                                                                         * a take) */
    else {
        ktrack_t *k = &kt[t];
        int i;
        for (i = 0; i < 128; i++) recOpen[i] = 0;
        recOver = k->len > 0; recFrom = k->n;
        if (!recOver) { trk_grid(); recAnchor = barAt; recBars = running ? 1 : 0; k->n = 0; k->takes = 0; }
        else if (running && k->wait) trk_join(t, barAt);
    }
    trk_tell(0);
}
static void trk_first_key(void)         /* waiting to record, the first key: the rhythm starts with it */
{
    if (!recWait || recTrk < 0) return;
    recWait = 0;
    if (!running) set_run(1);
    if (!SF_DRUM(tfam[recTrk])) {
        if (!recOver) { recAnchor = 0; recBars = 0; }
        else trk_join(recTrk, 0);
    }
    trk_tell(0);
}
static void trk_key(int t, int note, int vel)   /* a key of track t while it records: a note opened (vel) or closed */
{
    int at;
    ktrack_t *k;
    int32_t rel;
    if (recTrk != t || recWait || t < 0 || t >= DAB_NTRK || SF_DRUM(tfam[t])) return;
    k = &kt[t];
    rel = (int32_t)(beatFix - (recOver ? k->base : recAnchor)) * DAB_TPB / (int32_t)BEAT;
    if (rel < 0) rel = 0;
    at = (int)((rel + recQ / 2) / recQ) * recQ;
    if (recOver) at %= k->len;
    if (vel) {
        if (recOpen[note]) recOpen[note] = 0;
        if (k->n >= DAB_LOOP_N || loop_total() >= LOOP_POOL || at > 2047 || (!recOver && at >= recMax * recBarT)) {
            if (k->n >= DAB_LOOP_N || loop_total() >= LOOP_POOL) st.full++;
            return;
        }
        k->ev[k->n] = DAB_EV(at, 0, note);
        recOpen[note] = (int16_t)(++k->n);
        trk_tell(0);
    } else if (recOpen[note]) {
        uint32_t *e = &k->ev[recOpen[note] - 1];
        int ln = recOver ? (at - (int)DAB_EV_POS(*e) + k->len) % k->len : at - (int)DAB_EV_POS(*e);
        if (ln < recQ) ln = recQ;
        *e = DAB_EV(DAB_EV_POS(*e), ln > 2047 ? 2047 : ln, note);
        recOpen[note] = 0;
    }
}
static void dsteps_tell(void);
static void trk_undo(int t)             /* the last take gone (recording now: that one first); the last: empty. A drum
                                         * sound: its steps as they were before its last take; none kept: its own steps
                                         * go (the rhythm's again) */
{
    ktrack_t *k;
    if (t < 0 || t >= DAB_NTRK) return;
    if (SF_DRUM(tfam[t])) {
        int i = snap_top(t);
        if (recTrk == t) { recTrk = -1; recWait = 0; }
        snapArm = -1;
        if (i >= 0 && snaps[i].his) {
            int j, s;
            for (j = 0; j < DAB_TLANE; j++) for (s = 0; s < DAB_TPAT; s++) tpat[t][j][s] = snaps[i].p[j][s];
            dn[t] = snaps[i].n; dedit |= (uint8_t)(1u << t);
        } else dedit &= (uint8_t)~(1u << t);
        if (i >= 0) {                   /* (that take is no more) */
            for (; i < (int)nSnap - 1; i++) snaps[i] = snaps[i + 1];
            nSnap--;
        }
        if (drumTake == t + 1) drumTake = 0;
        dsteps_tell();
        trk_tell(0);
        return;
    }
    k = &kt[t];
    trk_off(t);
    if (recTrk == t) {
        int was_over = recOver, i;
        for (i = 0; i < 128; i++) recOpen[i] = 0;
        k->n = recFrom; recTrk = -1; recWait = 0; recOver = 0;
        if (!was_over) { k->n = 0; k->len = 0; k->bars = 0; k->takes = 0; }
    } else if (k->takes) k->n = k->take[--k->takes];
    if (!k->takes) { k->n = 0; k->len = 0; k->bars = 0; k->flen = 0; }
    trk_tell(1);
}
static void trk_runs(int go)            /* the rhythm started (the loops from their start) or stopped */
{
    int t;
    for (t = 0; t < DAB_NTRK; t++) {
        trk_off(t);
        if (go) { kt[t].base = 0; kt[t].tk = 0; kt[t].next = 0; kt[t].wait = 0; }
        st.tstep[t] = 0;
    }
    if (!go) lk_stop();                 /* (every knob a lock held: its normal value) */
    if (!go && recTrk >= 0 && !recWait) trk_stop();   /* (a recording ends with the rhythm) */
    trk_tell(0);
}
static void trk_bar(uint32_t t0)        /* the leading rhythm's bar begins, at t0 */
{
    int t;
    barAt = t0;
    for (t = 0; t < DAB_NTRK; t++) if (kt[t].wait && kt[t].len) trk_join(t, t0);
    if (recTrk < 0 || SF_DRUM(tfam[recTrk]) || recOver || recWait) return;
    if (t0 == recAnchor) { recBars = 1; trk_tell(0); return; }   /* (the take's first bar) */
    if (recBars >= recMax) {            /* as long as a loop may be: it ends here and plays from here */
        trk_stop();
        for (t = 0; t < DAB_NTRK; t++) if (kt[t].wait && kt[t].len) trk_join(t, t0);
        return;
    }
    recBars++;
    trk_tell(0);
}
static int loop_trig(const ktrack_t *k, int tick, int q)   /* a note of the loop begins in the step that starts at tick */
{
    int i;
    for (i = 0; i < k->n; i++) {
        int p = (int)DAB_EV_POS(k->ev[i]);
        if (p >= tick && p < tick + q && DAB_EV_LEN(k->ev[i])) return 1;
    }
    return 0;
}
static void trk_run(void)               /* each block while the rhythm runs: the loops' ticks now due */
{
    int t, i, q = step_ticks();
    for (t = 0; t < DAB_NTRK; t++) {
        ktrack_t *k = &kt[t];
        int guard = 64;
        if (!k->len || k->wait || (recTrk == t && !recOver)) continue;
        while ((int32_t)(beatFix - k->next) >= 0 && guard-- > 0) {
            uint32_t tick = k->tk;
            if (!SF_DRUM(tfam[t]) && tick % (uint32_t)q == 0u) {   /* a step of its own begins: where it is, its locks
                                                                    * (before the step's notes) */
                int s = (int)(tick / (uint32_t)q);
                if (s > 127) s = 127;
                st.tstep[t] = (uint8_t)s;
                if (lkN[t] | lkHeld[t]) lk_step(t, s, loop_trig(k, (int)tick, q));
            }
            for (i = 0; i < k->n; i++) {       /* the notes ending here, then the ones starting */
                uint32_t e = k->ev[i], ln = DAB_EV_LEN(e);
                if ((k->snd[i >> 3] >> (i & 7) & 1u) && ln && (DAB_EV_POS(e) + ln) % k->len == tick) {
                    k->snd[i >> 3] &= (uint8_t)~(1u << (i & 7)); trk_note(t, (int)DAB_EV_NOTE(e), 0);
                }
            }
            for (i = 0; i < k->n; i++) {
                uint32_t e = k->ev[i];
                if (DAB_EV_POS(e) == tick && DAB_EV_LEN(e) && !(recTrk == t && recOpen[DAB_EV_NOTE(e)] == i + 1)) {
                    if (k->snd[i >> 3] >> (i & 7) & 1u) trk_note(t, (int)DAB_EV_NOTE(e), 0);
                    k->snd[i >> 3] |= (uint8_t)(1u << (i & 7));
                    trk_note(t, (int)DAB_EV_NOTE(e), 1);
                    st.loop_plays++; st.loop_last = (uint32_t)t << 24 | tick << 8 | DAB_EV_NOTE(e);
                }
            }
            if (++k->tk >= k->len) { k->tk = 0; k->base += (uint32_t)k->len * BEAT / DAB_TPB; }
            k->next = trk_time(t, k->tk);
        }
    }
}

/* the tracks on the rhythm's own clock (a drum sound's steps; a note sound with no loop, for its locks): the step each
 * is on, counted from the rhythm's start, and whether something of the track begins there */
static int grid_trig(int t, int s2, int r)   /* (s2: the track's step; r: the rhythm's) */
{
    int j;
    if (SF_DRUM(tfam[t])) {
        if (dedit >> t & 1u) {
            for (j = 0; j < DAB_TLANE; j++) if (TP_GET(t, j, s2)) return 1;
            return 0;
        }
        if (t >= PART_KICK) for (j = 0; j < GN[t - PART_KICK]; j++) if (grid[0][GDR[t - PART_KICK][j]][r]) return 1;
        return 0;
    }
    if (!accOn || chordRoot < 0) return 0;   /* (the automatic bass line and backing: the rhythm's own) */
    if (t == PART_BASS) return bassStyle <= 2 ? grid[0][DAB_LBASS][r] != 0 : bassStyle == 3 && r % (gdiv[0] ? gdiv[0] : 4) == 0;
    if (t == PART_LEFT) return backStyle == 0 && grid[0][DAB_LCHORD][r] == 1;
    return 0;
}
static void grid_clock(int32_t s)       /* the leading rhythm's step s, before its hits */
{
    int t, r = (int)(s % (glen[0] ? glen[0] : 1));
    for (t = 0; t < DAB_NTRK; t++) {
        int n, s2;
        sndLk[t] = 0;                   /* (the step before's sound lock is over) */
        if (!SF_DRUM(tfam[t]) && (kt[t].len || recTrk == t)) continue;   /* (its loop's clock, or its first take's) */
        n = trk_steps(t);
        s2 = (int)(s % (n < 1 ? 1 : n));
        st.tstep[t] = (uint8_t)s2;
        if (lkN[t] | lkHeld[t]) lk_step(t, s2, grid_trig(t, s2, r));
    }
}

/* the tracks' steps: their drums (lane by lane) on the rhythm's steps */
static int trk_lanes(int t)             /* the rhythm's lanes a track has (the drum tracks as they start: their drums) */
{
    return t >= PART_KICK && t < DAB_NTRK ? GN[t - PART_KICK] : 0;
}
static void snap_push(int t, int hand)  /* track t's steps as they are now, kept: a take begins (no room: the oldest
                                         * kept one goes) */
{
    snap_t *p;
    int i, j, s;
    if (nSnap >= DAB_SNAPS) {
        for (i = 0; i < DAB_SNAPS - 1; i++) snaps[i] = snaps[i + 1];
        nSnap = DAB_SNAPS - 1;
    }
    p = &snaps[nSnap++];
    p->t = (int8_t)t; p->n = dn[t]; p->his = (uint8_t)(dedit >> t & 1u); p->hand = (uint8_t)hand;
    for (j = 0; j < DAB_TLANE; j++) for (s = 0; s < DAB_TPAT; s++) p->p[j][s] = tpat[t][j][s];
}
static void dsteps_own(int t)           /* a track's steps changed the first time: his from now, the rhythm's own to
                                         * begin with, over a bar or two (LENGTH) */
{
    int j, s, nl = trk_lanes(t), n, len = glen[0] < 1 ? 1 : glen[0];
    if (dedit >> t & 1u) return;
    n = trk_steps(t);
    for (j = 0; j < DAB_TLANE; j++)
        for (s = 0; s < DAB_TSTEPS; s++)
            tp_put(t, j, s, j < nl && s < n ? grid[0][GDR[t - PART_KICK][j]][s % len] : 0);
    dn[t] = (uint8_t)n;
    dedit |= (uint8_t)(1u << t);
}
static void dsteps_tell(void)
{
    int t, j, s;
    for (t = 0; t < DAB_NTRK; t++) {
        for (j = 0; j < DAB_TLANE; j++) for (s = 0; s < DAB_TPAT; s++) st.tpat[t][j][s] = tpat[t][j][s];
        st.tn[t] = (uint8_t)trk_steps(t);
    }
    st.dedit = dedit; st.two = tl2; st.page = (uint8_t)bar_steps(); st.steps_ver++;
    takes_tell();
}
static void dstep(int t, int s, int d, int how)   /* track t's step s. FLIP: its hits there go; none there, lane d's drum
                                         * is added. ONE: lane d's drum alone there. NONE: nothing there */
{
    int j, any = 0;
    if (t < 0 || t >= DAB_NTRK || s < 0 || s >= DAB_TSTEPS || d < 0 || d >= DAB_TLANE) return;
    if (recTrk == t) {                  /* (written while it records: this take's) */
        if (snapArm == t) { snap_push(t, 0); snapArm = -1; drumTake = (uint8_t)(t + 1); }
    } else {                            /* (by hand: after a recording, a take of its own; more by hand joins it) */
        int i = snap_top(t);
        if (i >= 0 && !snaps[i].hand) snap_push(t, 1);
    }
    dsteps_own(t);
    if (s < dn[t]) {
        for (j = 0; j < DAB_TLANE; j++) if (TP_GET(t, j, s)) any = 1;
        for (j = 0; j < DAB_TLANE; j++) tp_put(t, j, s, 0);
        if (how == DAB_STEP_ONE || (how == DAB_STEP_FLIP && !any)) tp_put(t, d, s, 2);
    }
    dsteps_tell();
}
static void trig(int k, float a, int tr, int var);
static void trig_lane(int t, int lane, float a)   /* track t's lane on the step it is on: its drum sound's drum (by
                                         * lane, in turn). A sound lock on that step: that drum sound's instead, the
                                         * same stroke where it has it, else its strokes in turn. Nothing more is
                                         * worked out than when two tracks share a drum: its shape, on the hit */
{
    int f = tfam[t], g, x = tidx[t];
    if (!SF_DRUM(f)) return;
    g = f - SF_KICK;
    if (sndLk[t]) {
        g = (sndLk[t] - 1) >> 4 & 3; x = (sndLk[t] - 1) & 15;
        if (x >= DAB_DRUMV) x = 0;
    }
    trig(GDR[g][lane % GN[g]], a, t, x);
}
static void dhit(int t, int lane, int record)   /* track t's lane now; recording: on its nearest step too */
{
    if (t < 0 || t >= DAB_NTRK || !SF_DRUM(tfam[t])) return;
    lane = lane % GN[tfam[t] - SF_KICK];
    if (record && !running) { set_run(1); recWait = 0; }   /* (the rhythm stopped: it starts, the hit its first step) */
    trig(GDR[tfam[t] - SF_KICK][lane], 1.0f, t, tidx[t]);   /* (played by hand: the track's own sound, whatever the
                                                             * step playing has locked) */
    if (record) {
        int32_t c = cnt[0] - 1;         /* the step playing now; nearer the next one: that */
        uint32_t half = BEAT / (uint32_t)(gdiv[0] ? gdiv[0] : 4) / 2u;
        if (c < 0) c = 0;
        else if ((int32_t)(nextt[0] - beatFix) < (int32_t)half) c++;
        if (snapArm == t) { snap_push(t, 0); snapArm = -1; drumTake = (uint8_t)(t + 1); }   /* (this take's first hit: the
                                                                                         * steps as they were, kept) */
        dsteps_own(t);
        tp_put(t, lane, (int)(c % (dn[t] ? dn[t] : 1)), 2);
        dsteps_tell();
    }
}

/* a loop's notes written step by step */
static void ev_cut(int t, int idx)      /* a note out of track t's loop: the takes, the keys recording and the notes
                                         * sounding follow */
{
    ktrack_t *k = &kt[t];
    int i, j;
    if (k->snd[idx >> 3] >> (idx & 7) & 1u) trk_note(t, (int)DAB_EV_NOTE(k->ev[idx]), 0);
    for (i = idx; i < k->n - 1; i++) {
        k->ev[i] = k->ev[i + 1];
        if (k->snd[(i + 1) >> 3] >> ((i + 1) & 7) & 1u) k->snd[i >> 3] |= (uint8_t)(1u << (i & 7));
        else k->snd[i >> 3] &= (uint8_t)~(1u << (i & 7));
    }
    k->snd[(k->n - 1) >> 3] &= (uint8_t)~(1u << ((k->n - 1) & 7));
    k->n--;
    for (j = 0; j < k->takes; j++) if (k->take[j] > idx) k->take[j]--;
    if (recTrk == t) {
        if (recFrom > idx) recFrom--;
        for (i = 0; i < 128; i++) {
            if (recOpen[i] == idx + 1) recOpen[i] = 0;
            else if (recOpen[i] > idx + 1) recOpen[i]--;
        }
    }
}
static void takes_tidy(int t)           /* a take with no note left is no take */
{
    ktrack_t *k = &kt[t];
    int j, o = 0;
    uint16_t hb = 0;
    for (j = 0; j < k->takes; j++) {
        int end = j + 1 < k->takes ? k->take[j + 1] : recTrk == t ? recFrom : k->n;
        if (k->take[j] < end) {
            if (takeHand[t] >> j & 1u) hb |= (uint16_t)(1u << o);
            k->take[o++] = k->take[j];
        }
    }
    k->takes = (uint8_t)o; takeHand[t] = hb;
}
static void ndel(int t, int p0, int p1) /* track t's notes that begin at ticks p0 .. p1 - 1 go; none left: empty */
{
    ktrack_t *k;
    int i, cut = 0;
    if (t < 0 || t >= DAB_NTRK) return;
    k = &kt[t];
    for (i = k->n - 1; i >= 0; i--) {
        int p = (int)DAB_EV_POS(k->ev[i]);
        if (p >= p0 && p < p1) { ev_cut(t, i); cut = 1; }
    }
    if (!cut) return;
    takes_tidy(t);
    if (!k->n && recTrk != t) trk_empty(t);
    trk_tell(1);
}
static void nadd(int t, int pos, int len, int note, int newlen, int alone)   /* a note on track t, from tick pos, len
                                         * ticks long (the track has no loop: one of newlen ticks, his from now); alone:
                                         * the notes that began in those ticks go first */
{
    ktrack_t *k;
    int i, first;
    if (t < 0 || t >= DAB_NTRK || note < 0 || note > 127 || pos < 0 || pos > 2047 || len < 1) return;
    k = &kt[t];
    if (alone) {
        for (i = k->n - 1; i >= 0; i--) {
            int p = (int)DAB_EV_POS(k->ev[i]);
            if (p >= pos && p < pos + len) ev_cut(t, i);
        }
        takes_tidy(t);
    }
    first = !k->len && recTrk != t;     /* (no loop, and not recording its first take: this note makes one) */
    if (first) {
        if (newlen < 1 || newlen > 2047) return;
        if (loop_total() - k->n >= LOOP_POOL) { st.full++; return; }
        k->n = 0; k->takes = 0; k->len = (uint16_t)newlen; k->flen = 0; k->bars = (uint8_t)bars_of(newlen); k->wait = 0;
        for (i = 0; i < DAB_LOOP_N / 8; i++) k->snd[i] = 0;
        auto_hush(t);
        trk_align(t);
    } else {
        if (k->n >= DAB_LOOP_N || loop_total() >= LOOP_POOL) { st.full++; return; }
        for (i = 0; i < k->n; i++) if ((int)DAB_EV_POS(k->ev[i]) == pos && (int)DAB_EV_NOTE(k->ev[i]) == note) return;
    }
    if (recTrk != t && k->takes < DAB_TAKES && (!k->takes || !(takeHand[t] >> (k->takes - 1) & 1u))) {
        takeHand[t] |= (uint16_t)(1u << k->takes);   /* (written by hand: a take of its own, the first of all or after a
                                                     * recording; more by hand joins it) */
        k->take[k->takes++] = (uint8_t)k->n;
    }
    k->ev[k->n] = DAB_EV(pos, len > 2047 ? 2047 : len, note);
    k->snd[k->n >> 3] &= (uint8_t)~(1u << (k->n & 7));
    k->n++;
    trk_tell(1);
}
static void tlen(int t, int two, int ticks)   /* LENGTH: a bar or two. His steps: that many; longer, what they played
                                         * again in the new part if it is empty; shorter, the rest kept, silent. A loop
                                         * (ticks > 0): the same, in ticks */
{
    ktrack_t *k;
    int b, n;
    if (t < 0 || t >= DAB_NTRK) return;
    b = bar_steps();
    n = two && 2 * b <= DAB_TSTEPS ? 2 * b : b > DAB_TSTEPS ? DAB_TSTEPS : b;
    tl2 = (uint8_t)(two ? tl2 | 1u << t : tl2 & ~(1u << t));
    if (dedit >> t & 1u) {
        int old = dn[t], j, s, any = 0;
        if (n > old && old > 0) {
            for (j = 0; j < DAB_TLANE; j++) for (s = old; s < n; s++) if (TP_GET(t, j, s)) any = 1;
            if (!any) for (j = 0; j < DAB_TLANE; j++) for (s = old; s < n; s++) tp_put(t, j, s, (int)TP_GET(t, j, s % old));
        }
        dn[t] = (uint8_t)n;
    }
    dsteps_tell();
    k = &kt[t];
    if (ticks > 0 && k->len && recTrk != t) {
        int old = k->len, i, m, any = 0, nn = k->n, need = 0;
        if (ticks > 2047) ticks = 2047;
        if (ticks > old) {
            for (i = 0; i < nn; i++) { int p = (int)DAB_EV_POS(k->ev[i]); if (p >= old && p < ticks) any = 1; }
            if (!any) {
                for (i = 0; i < nn; i++) { int p = (int)DAB_EV_POS(k->ev[i]); if (p < old) for (m = old; p + m < ticks; m += old) need++; }
                if (k->n + need <= DAB_LOOP_N && loop_total() + need <= LOOP_POOL) {
                    for (i = 0; i < nn; i++) {
                        uint32_t e = k->ev[i];
                        int p = (int)DAB_EV_POS(e);
                        if (p < old)
                            for (m = old; p + m < ticks; m += old) {
                                k->ev[k->n] = DAB_EV(p + m, DAB_EV_LEN(e), DAB_EV_NOTE(e));
                                k->snd[k->n >> 3] &= (uint8_t)~(1u << (k->n & 7));
                                k->n++;
                            }
                    }
                } else if (need) st.full++;
            }
        }
        trk_off(t);
        k->len = (uint16_t)ticks; k->bars = (uint8_t)bars_of(ticks);
        trk_align(t);
        trk_tell(1);
    }
}

/* HOLD: pressed with keys down, the notes of those keys (the lead's, the left hand's, the bass line's) sound on after
 * they are let go, with their sound and tuning, until HOLD again: with other keys down, those are held instead; with
 * none, the held notes are let go. What is played meanwhile is not held. */
static void held_end(int m, int n)      /* a held note let go, as its key would have been */
{
    int t = KTRK(m), arp = BGET(heldArp, n) && arp_keys(m);
    BCLR(heldK[m], n);
    if (arp) BCLR(heldArp, n);
    if (m == DAB_NTRK) {
        trk_key(PART_LEFT, n, 0);
        lower_hold(n, 4, 0);
        if (accOn) left_after_off(); else release(R_KEY, PART_LEFT, n);
    } else if (SF_DRUM(tfam[t])) {
    } else if (t == PART_DRONE) {
        trk_key(t, n, 0);               /* (the drone stays on its note) */
    } else if (arp) {
        if (par[P_ARPLATCH] == 0.0f) { arp_remove(n); st.arp_n = (uint8_t)nArp; }
    } else {
        trk_key(t, n, 0);
        if (tfam[t] == SF_LEAD && t == leadTr) lead_off(n); else release(R_KEY, t, n);
    }
}
static void hold_release(void)          /* every held note let go */
{
    int m, n;
    for (m = 0; m < NMAN; m++) for (n = 0; n < 128; n++) if (BGET(heldK[m], n)) held_end(m, n);
    st.held_n = 0; st.hold = 0;
}
static void hold_press(void)            /* HOLD pressed: the keys down held (instead of what was); none down: let go */
{
    int m, n, c = 0;
    uint32_t any = 0;
    for (m = 0; m < NMAN; m++) for (n = 0; n < 4; n++) any |= keyDn[m][n];
    for (m = 0; m < NMAN; m++) for (n = 0; n < 128; n++)
        if (BGET(heldK[m], n) && (!any || !BGET(keyDn[m], n))) held_end(m, n);
    if (any)
        for (m = 0; m < NMAN; m++) for (n = 0; n < 128; n++)
            if (BGET(keyDn[m], n)) {
                BSET(heldK[m], n);
                if (arp_keys(m) && arpDown[n]) BSET(heldArp, n);
                c++;
            }
    st.held_n = (uint8_t)c; st.hold = c != 0; st.hold_ver++;
}

/* a track's sound changed: its notes end (held ones too), its loop goes on in the new sound (silent with a drum sound),
 * its steps play the new drums (silent with a note sound) */
static int8_t drnV[2];
static void track_sound(int t, int f, int x, int again)   /* (again: the same, as a new sound) */
{
    int i, n, held = 0;
    if (t < 0 || t >= DAB_NTRK || f < 0 || f >= SF_N) return;
    if (x < 0 || x >= dabke_family_size(f)) x = 0;
    if (tfam[t] == f && tidx[t] == x && !again) return;
    if (recTrk == t && !SF_DRUM(tfam[t]) != !SF_DRUM(f))   /* recording, and its sound of the other kind now (notes, drums):
                                                             * the recording ends here as REC would end it, what was played
                                                             * kept. (Ended later, it ended as the other kind: a note left
                                                             * with no loop around it, or a loop's length made up) */
        trk_stop();
    trk_off(t);
    for (i = 0; i < NV; i++) if (vo[i].used && vo[i].tr == t && vo[i].gate) vo[i].gate = 0;
    for (n = 0; n < 128; n++) {
        if (BGET(heldK[t], n)) BCLR(heldK[t], n);
        if (t == PART_LEFT && BGET(heldK[DAB_NTRK], n)) { BCLR(heldK[DAB_NTRK], n); lower_hold(n, 4, 0); }
    }
    if (t == PART_LEFT && accOn) stop_acc();
    if (t == PART_BASS && bassNote >= 0) bassNote = -1;
    if (t == PART_DRONE) drnV[0] = drnV[1] = -1;
    if (t == arpTr) arp_forget();
    if (t == leadTr) { nms = 0; monoNote = -1; }
    tfam[t] = (uint8_t)f; tidx[t] = (uint8_t)x;
    st.tfam[t] = (uint8_t)f; st.tidx[t] = (uint8_t)x;
    for (i = 0; i < NMAN; i++) for (n = 0; n < 128; n++) held += (int)BGET(heldK[i], n);
    st.held_n = (uint8_t)held; st.hold = held != 0;
    derive();
}

/* ---------------------------------------------------------------- messages --- */
static void post(int t, int a, int b, float f, float g, float h)
{
    uint32_t w = q_w, nx = (w + 1u) % QN;
    if (nx == q_r) return;                                  /* full: the audio side is not running; this one is lost */
    q[w].t = (uint8_t)t; q[w].a = (int16_t)a; q[w].b = (int16_t)b; q[w].f = f; q[w].g = g; q[w].h = h;
    q_w = nx;
}
static uint8_t echoStale = 1, revStale = 1, slapStale = 1;
static uint8_t lite;                /* out of time with the fewest notes: the ensemble and chorus are left out */
static volatile uint8_t load_pct;
static uint32_t shed_wait, took;
static void take(const msg_t *m)
{
    int i, k;
    if (dirty && m->t != M_PAR && m->t != M_TPAR && m->t != M_NTUNE && m->t != M_DRUM) { dirty = 0; derive(); }
    switch (m->t) {
    case M_KEY: if (m->b) key_on(m->a >> 8 & 15, m->a & 255, m->b, m->a >> 12 & 1); else key_off(m->a >> 8 & 15, m->a & 255); break;
    case M_PAR:
        if (m->a >= 0 && m->a < P_COUNT) {
            if (m->a == P_PRESSURE && m->b) pressTr = (uint8_t)m->b;   /* (eased off: the bend comes back on the same tracks) */
            if (m->a == P_ARPON && m->f == 0.0f && par[P_ARPON] != 0.0f) { par[P_ARPON] = 0.0f; arp_flush(); }
            if (m->a == P_ARPLATCH && m->f == 0.0f && par[P_ARPLATCH] != 0.0f) { par[P_ARPLATCH] = 0.0f; arp_latch_off(); }
            if (m->a == P_PLAY && m->f != par[P_PLAY]) { nms = 0; monoNote = -1; }
            if (m->a < DAB_NLK) {       /* a knob a lock can hold: its normal value, heard unless a lock holds it now */
                parN[m->a] = m->f;
                if (!lkAt[m->a]) par_to(m->a, m->f);
            } else { par[m->a] = m->f; dirty = 1; }
        }
        break;
    case M_TPAR:                        /* a track's own sound: a knob (its normal value, heard unless a lock holds it
                                         * now), a choice, or its tone as its instrument's own */
        if (m->b >= 0 && m->b < DAB_NTRK) {
            int t = m->b, s = sp_of(m->a);
            if (m->a == DAB_PLAIN) { tplain[t] = m->f != 0.0f; dirty = 1; }
            else if (s >= 0 && s < DAB_NSK) {
                k = DAB_NLK + t * DAB_NSK + s;
                parN[k] = m->f;
                if (!lkAt[k]) kn_to(k, m->f);
            } else if (s >= 0) {
                if (s == S_PLAY && t == leadTr && m->f != tpar[t][s]) { nms = 0; monoNote = -1; }
                tpar[t][s] = m->f; dirty = 1;
                if (!t) par[m->a] = m->f;   /* (track 1's: the whole instrument's of that name) */
            }
        }
        break;
    case M_NTUNE: if (m->a >= 0 && m->a < 12) ntune[m->a] = (int16_t)(m->b < -100 ? -100 : m->b > 100 ? 100 : m->b); break;
    case M_RHY: set_active(sh_rhy, sh_nrhy); dsteps_tell(); break;   /* (a bar as LENGTH counts it may change) */
    case M_RUN: fadeDir = 0; fadeG = 1.0f; set_run(m->a); break;
    case M_SYNC: syncOn = m->a ? 1 : 0; break;
    case M_ACC:
        accOn = m->a ? 1 : 0;
        if (!accOn) { stop_acc(); holding = 0; chordRoot = -1; st.left_root = -1; }
        break;
    case M_STYLE:
        backStyle = (uint8_t)m->a; bassStyle = (uint8_t)m->b; stop_acc(); holding = 0;
        if (accOn && chordRoot >= 0 && !running && nLower) hold_chord();
        break;
    case M_FILL: if (running && !fill) { fill = 1; fillType = (uint8_t)(m->a < 0 ? (int)par[P_FILLTYPE] : m->a); } break;
    case M_FADE:
        if (m->a > 0 && !running) { set_run(1); fadeG = 0.0f; fadeDir = 1; }
        else if (m->a < 0 && running) fadeDir = -1;
        break;
    case M_DRUM:
        if (m->a >= 0 && m->a < DAB_NDRUM) { dlv[m->a] = clampf(m->f, 0.0f, 1.0f); dtune[m->a] = clampf(m->g, 0.25f, 4.0f); ddec[m->a] = clampf(m->h, 0.2f, 5.0f); dirty = 1; }
        break;
    case M_OWN:
        if (m->a >= 0 && m->a < DAB_NOWN) {
            int s = m->a;
            ownLen[s] = (uint8_t)clampf((float)(m->b & 255), 2.0f, (float)DAB_RMAX);
            ownDiv[s] = (uint8_t)clampf((float)(m->b >> 8), 2.0f, 4.0f);
            for (k = 0; k < DAB_NDRUM; k++) for (i = 0; i < DAB_RMAX; i++) ownG[s][k][i] = (uint8_t)(sh_own[k * DAB_RMAX + i] & 3u);
            for (i = 0; i < nact; i++) if (active[i] == DAB_NRHYTHM + s) compile(active[i], i);
            st.steps = glen[0];
            dsteps_tell();
        }
        break;
    case M_TRKREC: trk_rec(m->a); break;
    case M_UNDO: trk_undo(m->a); break;
    case M_CLEARALL:                    /* every loop, every track's own steps, every lock */
        for (i = 0; i < DAB_NTRK; i++) trk_empty(i);
        recTrk = -1; recWait = 0; recOver = 0; dedit = 0; nSnap = 0; drumTake = 0; snapArm = -1;
        lock_clear(-1, 0);
        dsteps_tell(); trk_tell(1);
        break;
    case M_LOOPSET:                     /* a kept loop */
        if (m->a >= 0 && m->a < DAB_NTRK) {
            ktrack_t *kk = &kt[m->a];
            trk_off(m->a);
            kk->n = sh_n[m->a] > DAB_LOOP_N ? DAB_LOOP_N : sh_n[m->a];
            kk->len = sh_len[m->a] > 2047u ? 2047u : sh_len[m->a];
            kk->bars = sh_bars[m->a]; kk->takes = sh_ntakes[m->a] > DAB_TAKES ? DAB_TAKES : sh_ntakes[m->a];
            takeHand[m->a] = 0;         /* (kept takes: all as recordings) */
            kk->flen = sh_flen[m->a] > 2047u ? 0 : sh_flen[m->a];
            for (i = 0; i < kk->n; i++) kk->ev[i] = sh_ev[m->a][i] & 0x1FFFFFFFu;
            for (i = 0; i < DAB_TAKES; i++) kk->take[i] = sh_takes[m->a][i];
            if (!kk->n || !kk->len) { kk->n = 0; kk->len = 0; kk->bars = 0; kk->takes = 0; kk->flen = 0; }
            trk_align(m->a);
            trk_tell(1);
        }
        break;
    case M_STEP: dstep(m->a, m->b >> 8 & 127, m->b & 15, m->b >> 4 & 3); break;
    case M_DHIT: dhit(m->a, m->b & 255, m->b >> 8); break;
    case M_STEPSSET: {
        int t;
        for (t = 0; t < DAB_NTRK; t++) for (i = 0; i < DAB_TLANE; i++) for (k = 0; k < DAB_TPAT; k++)
            tpat[t][i][k] = sh_pat[t][i][k];
        dedit = sh_edit; tl2 = sh_two; nSnap = 0; drumTake = 0; snapArm = -1;
        for (t = 0; t < DAB_NTRK; t++) {
            dn[t] = sh_dn[t] > DAB_TSTEPS ? DAB_TSTEPS : sh_dn[t];
            if ((dedit >> t & 1u) && !dn[t]) {   /* (kept before a track had a length: its steps went round with the
                                                 * rhythm's bar; the same now, over a bar as LENGTH counts it) */
                int L = glen[0] < 1 ? 1 : glen[0], n = bar_steps(), s2;
                if (n > DAB_TSTEPS) n = DAB_TSTEPS;
                for (i = 0; i < DAB_TLANE; i++) for (s2 = L; s2 < DAB_TSTEPS; s2++) tp_put(t, i, s2, s2 < n ? (int)TP_GET(t, i, s2 % L) : 0);
                dn[t] = (uint8_t)n;
            }
        }
        dsteps_tell();
        break;
    }
    case M_NADD: nadd(m->a & 7, m->b, (int)m->f, m->a >> 4 & 127, (int)m->g, m->a >> 11 & 1); break;
    case M_NDEL: ndel(m->a, m->b, (int)m->f); break;
    case M_TLEN: tlen(m->a, m->b, (int)m->f); break;
    case M_LOCK: {                      /* a lock in its place: a = its low half, b = its high half, f = the place */
        uint32_t v = (uint32_t)(uint16_t)m->a | (uint32_t)(uint16_t)m->b << 16;
        if (v && DAB_LK_ID(v) < DAB_NLK) lkU[DAB_LK_ID(v)] = m->g;
        lock_put((int)m->f, v);
        break;
    }
    case M_LKCLEAR: lock_clear(m->a, m->b); break;
    case M_MUTE: muteM = (uint8_t)m->a; break;
    case M_TSND:                        /* a track's sound: what it played stops (its held notes too), the rest as it was */
        track_sound(m->a, m->b >> 8, m->b & 127, m->b & DAB_AGAIN);
        break;
    case M_HOLD:
        if (m->a) hold_press(); else { hold_release(); st.hold_ver++; }
        break;
    default:                                                /* panic */
        for (i = 0; i < NV; i++) vo[i].used = 0;
        for (i = 0; i < 128; i++) lowerHeld[i] = 0;
        for (i = 0; i < DAB_NTRK; i++) for (k = 0; k < DAB_LOOP_N / 8; k++) kt[i].snd[k] = 0;
        drnV[0] = drnV[1] = -1;
        for (k = 0; k < NMAN; k++) for (i = 0; i < 4; i++) { keyDn[k][i] = 0; heldK[k][i] = 0; }
        for (i = 0; i < 4; i++) heldArp[i] = 0;
        if (recWait) { recTrk = -1; recWait = 0; }
        st.held_n = 0; st.hold = 0;
        nLower = 0; nAcc = 0; bassNote = -1; holding = 0; nArp = 0; nArpSnd = 0; nDown = 0; nms = 0; monoNote = -1;
        for (i = 0; i < 128; i++) arpDown[i] = 0;
        running = 0; st.running = 0; fadeDir = 0; droneG = 0.0f; chordRoot = -1; st.left_root = -1;
        snapArm = -1;
        trk_runs(0);
        break;
    }
}

/* ------------------------------------------------- one note's share of a block --- */
static float vtmp[BS];
static int render_voice(voice_t *v, int n, float vibS)
{
    const tone_t *t = &TT[v->tr];
    float inN = 1.0f / (float)n, g0 = v->g, g1, a0, da, pitch, pp0, f, dt, dt2, sec = (float)n / SR, lpF = t->lpF;
    uint32_t d1, d2, p1 = v->ph1, p2 = v->ph2;
    int lead = v->fam == SF_LEAD, bassv = v->fam == SF_BASS, plucked = MD_PLUCKED(v->model) && !bassv, i, live;
    int leant = v->m == R_KEY && (pressTr >> v->tr & 1u);   /* a key's note on a track whose keys are leant on: the
                                         * pressure is its own (not a loop's notes, not another track's) */
    float s1 = v->s1, s2 = v->s2, s3 = v->s3, s4 = v->s4;
    /* the pitch: the key with its quarter tones, the glide, the ornament, vibrato and the bend */
    v->age += sec;
    if (v->gl != 0.0f) { v->gl -= v->gl * KT[v->tr].glideK; if (v->gl < 0.002f && v->gl > -0.002f) v->gl = 0.0f; }
    pitch = v->base + v->gl + pitchS[v->tr];
    {
        float kb = key_pitch(v->n) + (float)v->oct + (lead ? KT[v->tr].tune : K.tune);   /* (a key's tuning may change
                                         * while it sounds) */
        pitch += kb - v->base;
        v->base = kb;
    }
    if (lead) {                     /* (its track's own vibrato and bend range) */
        orn_block(v, n);
        if (v->age > KT[v->tr].vibDelay) v->vamt += (1.0f - v->vamt) * 0.04f;
        pitch += v->oo + vibST[v->tr] * v->vamt + bendST[v->tr];
    } else if (!bassv) pitch += vibS * 0.35f;
    if (leant) pitch += lead ? pbendST[v->tr] + vibPT[v->tr] : pbendS + vibP;
    f = 440.0f * fm_exp2f((pitch - 69.0f) * (1.0f / 12.0f));
    if (f > 16000.0f) f = 16000.0f;
    dt = f / SR; dt2 = dt * t->detB;
    if (!bassv && (v->model == MD_MIJWIZ || v->model == MD_ACCORDION || v->model == MD_LEAD || v->model == MD_STRINGS ||
                      v->model == MD_BRASS))
        dt *= t->detA;              /* (two of a size: the note between them; a weak second one stays above) */
    else dt2 = dt * t->detB * t->detB;
    d1 = (uint32_t)(dt * 4294967296.0f); d2 = (uint32_t)(dt2 * 4294967296.0f);
    /* the envelope */
    pp0 = v->pp;
    if (bassv) {                    /* the bass: plucked, holding at a part */
        if (v->gate) { g1 = g0 + (1.0f - g0) * K.bassAtk; v->pp = 0.45f + (v->pp - 0.45f) * K.bassDecN; }
        else g1 = g0 * K.bassRelN;
    } else if (plucked) {
        float rs = v->gate ? t->rings : (t->rings < 0.25f ? t->rings : 0.25f);
        g1 = g0 * fm_expf(-6.9f * sec / rs);
        v->kloss = fm_expf(-6.9f / (rs * f));
        if (v->gate && lead && tpar[v->tr][S_PICKING] != 0.0f && v->m == R_KEY) {   /* tremolo picking: struck again
                                         * and again */
            if ((v->kpick -= n) <= 0) { v->kpick = (int32_t)(SR / clampf(tpar[v->tr][S_ORNSPEED], 3.0f, 30.0f)); if (v->age > 0.01f) ks_pluck(v, v->vel * 0.7f); g1 = 1.0f; }
        }
    } else g1 = v->gate ? g0 + (1.0f - g0) * t->atkN : g0 * (v->m == R_BACK && backStyle != 1 ? K.stabRelN : t->relN);
    v->g = g1;
    live = g0 > 0.0003f || g1 > 0.0003f;
    if (!live) return v->gate;      /* (a string rung out while its key is held: kept, not worked) */
    a0 = g0 * v->vel * t->gain; da = (g1 - g0) * v->vel * t->gain * inN;
    if (bassv) { a0 = g0 * pp0 * v->vel * t->gain; da = (g1 * v->pp - g0 * pp0) * v->vel * t->gain * inN; }
    if (leant && par[P_PRESS] == 4.0f) lpF = clampf(lpF * (1.0f + pressS * 1.5f), 0.0f, t->lpTop);
    if (v->model == MD_BRASS && !bassv) {   /* the brass's filter opens as it starts */
        v->pp *= 0.94f;
        lpF = clampf(lpF * (1.0f + t->fenv * v->pp * (v->gate ? 1.0f : 0.0f)), 0.0f, t->lpTop);
    }
    if (bassv) {                    /* the bass's sounds */
        float bf = svf_f(180.0f + 1400.0f * v->pp * v->pp);
        for (i = 0; i < n; i++) {
            float s;
            if (t->model == 0) s = sin_ph(p1) + 0.25f * sin_ph(p1 << 1);
            else if (t->model == 1) { s = saw_s(p1, dt); s2 += bf * s3; s4 = s - s2 - 0.9f * s3; s3 += bf * s4 + DN; s = s2; }
            else if (t->model == 3) {   /* woody: a triangle, round, a little of the filter */
                float ph = PHF(p1);
                s = (ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph) * 1.2f; s2 += bf * s3; s4 = s - s2 - 1.2f * s3; s3 += bf * s4 + DN; s = s2;
            } else if (t->model == 4) { /* picked: a narrow pulse through a ringing filter */
                s = pulse_s(p1, dt, 0.18f) * 1.4f; s2 += bf * s3; s4 = s - s2 - 0.45f * s3; s3 += bf * s4 + DN; s = s2 * 0.9f;
            }
            else { s = clip3(pulse_s(p1, dt, 0.5f) * 1.6f) * 0.8f; s2 += bf * s3; s4 = s - s2 - 0.7f * s3; s3 += bf * s4 + DN; s = s2 * 1.2f; }
            p1 += d1;
            vtmp[i] = s * a0; a0 += da;
        }
    } else if (plucked) {           /* a string: its loop, the pick's noise into it, the body, the buzz */
        float *buf = ksbuf[v->slot], L = 44100.0f / f - 0.5f, loss = v->kloss, amp = t->gain;   /* (its loop decays it) */
        float br = t->bright, bpF = t->bpF, bq = t->bpQ, bm = t->bpMix, dr = t->drive, dn = t->driveN;
        uint32_t w = v->kw;
        while (L > (float)(KSN - 4)) L *= 0.5f;
        if (L < 2.0f) L = 2.0f;
        v->klen = L;
        for (i = 0; i < n; i++) {
            float rp = (float)w - L, fr, y, x;
            uint32_t ri;
            if (rp < 0.0f) rp += (float)KSN;
            ri = (uint32_t)rp; fr = rp - (float)ri;
            y = buf[ri & (KSN - 1u)] * (1.0f - fr) + buf[(ri + 1u) & (KSN - 1u)] * fr;
            x = (y + v->kprev) * 0.5f * loss;
            v->kprev = y;
            if (v->kexc > 0) { v->kexc--; v->kexl += (noise() * v->kexa - v->kexl) * br; x += v->kexl; }
            buf[w] = x + DN;
            w = (w + 1u) & (KSN - 1u);
            s1 += bpF * s2; s3 = y - s1 - bq * s2; s2 += bpF * s3 + DN;
            x = y + s2 * bm;
            x = clip3(x * dr) * dn;
            vtmp[i] = x * amp;
        }
        v->kw = w;
    } else {
        float lq = t->lpQ, bpF = t->bpF, bq = t->bpQ, bm = t->bpMix, dm = t->dryMix, dr = t->drive, dn = t->driveN, nz = t->noise, pw = t->pw;
        if (v->model == MD_NEY) bpF = svf_f(f * 2.0f);
        switch (v->model) {
        case MD_MIJWIZ:
        case MD_ACCORDION:
        case MD_VIOLIN:
        case MD_ZURNA:
            for (i = 0; i < n; i++) {
                float s, h;
                if (v->model == MD_MIJWIZ || v->model == MD_ACCORDION) s = (pulse_s(p1, dt, pw) + pulse_s(p2, dt2, pw)) * 0.5f;
                else s = saw_s(p1, dt) + (v->model == MD_ZURNA ? 0.35f : 0.3f) * saw_s(p2, dt2);
                p1 += d1; p2 += d2;
                if (nz != 0.0f) s += nz * noise();
                s1 += bpF * s2; h = s - s1 - bq * s2; s2 += bpF * h + DN;
                s = s * dm + s2 * bm;
                s = clip3(s * dr) * dn;
                s3 += lpF * s4; h = s - s3 - lq * s4; s4 += lpF * h + DN;
                vtmp[i] = s3 * a0; a0 += da;
            }
            break;
        case MD_NEY:
            for (i = 0; i < n; i++) {
                float s = sin_ph(p1) + 0.12f * sin_ph(p1 << 1) + 0.05f * sin_ph(p1 * 3u), h, b = noise();
                p1 += d1;
                s1 += bpF * s2; h = b - s1 - bq * s2; s2 += bpF * h + DN;   /* breath, tuned to the note */
                s += s2 * nz * 0.6f + b * nz * 0.04f;
                s3 += lpF * s4; h = s - s3 - lq * s4; s4 += lpF * h + DN;
                vtmp[i] = s3 * a0; a0 += da;
            }
            break;
        case MD_ORGAN:
            for (i = 0; i < n; i++) {
                float s = sin_ph(p1) + 0.55f * sin_ph(p1 << 1) + 0.3f * sin_ph(p1 * 3u) + 0.15f * sin_ph(p1 << 2);
                p1 += d1;
                if (dr > 1.05f) s = clip3(s * 0.5f * dr) * dn * 2.0f;
                vtmp[i] = s * a0; a0 += da;
            }
            break;
        case MD_SQUARE:
            for (i = 0; i < n; i++) {
                float s = pulse_s(p1, dt, pw) + 0.25f * pulse_s(p2, dt2, pw), h;
                p1 += d1; p2 += d2;
                s = clip3(s * dr * 0.7f) * dn;
                s3 += lpF * s4; h = s - s3 - lq * s4; s4 += lpF * h + DN;
                vtmp[i] = s3 * a0; a0 += da;
            }
            break;
        default:                    /* keyboard lead, strings, brass: two saws through the low-pass */
            for (i = 0; i < n; i++) {
                float s = saw_s(p1, dt) + 0.8f * saw_s(p2, dt2), h;
                p1 += d1; p2 += d2;
                if (nz != 0.0f) s += nz * noise();
                if (dr > 1.05f) s = clip3(s * 0.6f * dr) * dn;
                s3 += lpF * s4; h = s - s3 - lq * s4; s4 += lpF * h + DN;
                vtmp[i] = s3 * a0; a0 += da;
            }
            break;
        }
    }
    v->ph1 = p1; v->ph2 = p2;
    v->s1 = s1; v->s2 = s2; v->s3 = s3; v->s4 = s4;
    {
        float *o = bus[v->tr];
        for (i = 0; i < n; i++) o[i] += vtmp[i];
    }
    return 1;
}

static int8_t drnV[2] = {-1, -1};       /* the drone line's notes as voices (its track's sound not the drone's) */
static void drone_block(int n, float vibS)
{
    float target = 0.0f, g0 = droneG, dg, f, dt, f2 = 0.0f, dt2 = 0.0f, lpF = TD.lpF, lq = TD.lpQ, bpF = TD.bpF, bq = TD.bpQ;
    int mode = (int)par[P_DRONEMODE], i, pc, k;
    uint32_t d1, d2 = 0, p1 = DR.ph1, p2 = DR.ph2;
    float s1 = DR.s1, s2 = DR.s2, s3 = DR.s3, s4 = DR.s4, base;
    const uint32_t *kd = keyDn[PART_DRONE], *hd = heldK[PART_DRONE];
    if ((kd[0] | kd[1] | kd[2] | kd[3] | hd[0] | hd[1] | hd[2] | hd[3]) != 0u)
        droneIdle = 0;                  /* (its own keys, down or held: it sounds, switched off or not) */
    else if (droneIdle < 0x7FFFFFFFu) droneIdle += (uint32_t)n;
    if (!mode && droneIdle < (uint32_t)(SR * 0.6f)) mode = 1;
    if (mode) {                     /* it sounds while the lead plays (and 0.6 s after), or the rhythm runs */
        int on = running || leadIdle < (uint32_t)(SR * 0.6f) || (accOn && nLower > 0) || droneIdle < (uint32_t)(SR * 0.6f);
        target = on ? 1.0f : 0.0f;
    }
    if (tfam[PART_DRONE] != SF_DRONE) { /* another sound on its track: the line played as that sound's notes (a drum
                                         * sound: none) */
        int want[2] = {-1, -1};
        droneG = 0.0f;
        if (target > 0.0f && !SF_DRUM(tfam[PART_DRONE])) {
            pc = droneNote >= 0 ? droneNote % 12 : mode == 3 && st.left_root >= 0 ? st.left_root : (int)par[P_ROOT];
            want[0] = 36 + pc + ((int)par[P_DRONEOCT] ? 12 : 0);
            if (mode == 2) want[1] = want[0] + 7;
        }
        for (k = 0; k < 2; k++)
            if (drnV[k] != want[k]) {
                if (drnV[k] >= 0) release(R_DRN, PART_DRONE, drnV[k]);
                if (want[k] >= 0) start(R_DRN, PART_DRONE, want[k], 90);
                drnV[k] = (int8_t)want[k];
            }
        return;
    }
    if (drnV[0] >= 0 || drnV[1] >= 0) {   /* (back to the drone's own sound: its notes as voices end) */
        for (k = 0; k < 2; k++) if (drnV[k] >= 0) release(R_DRN, PART_DRONE, drnV[k]);
        drnV[0] = drnV[1] = -1;
    }
    droneG += (target - droneG) * (target > droneG ? 0.012f : 0.008f);
    if (droneG < 0.0002f && target == 0.0f) { droneG = 0.0f; return; }
    dg = (droneG - g0) / (float)n;
    pc = droneNote >= 0 ? droneNote % 12 : mode == 3 && st.left_root >= 0 ? st.left_root : (int)par[P_ROOT];   /* (its track's
                                         * note, the left key's, or the maqam's tonic) */
    base = 36.0f + (float)pc + ((int)par[P_DRONEOCT] ? 12.0f : 0.0f) + pitchS[PART_DRONE];
    f = 440.0f * fm_exp2f((base + (float)ntune[pc % 12] * 0.01f + K.tune - 69.0f + vibS * 0.1f) * (1.0f / 12.0f));
    dt = f / SR; d1 = (uint32_t)(dt * 4294967296.0f);
    if (mode == 2) {                /* and its fifth (the maqam's fifth key, as tuned) */
        k = (pc + 7) % 12;
        f2 = 440.0f * fm_exp2f((base + 7.0f + (float)ntune[k] * 0.01f + K.tune - 69.0f) * (1.0f / 12.0f));
        dt2 = f2 / SR; d2 = (uint32_t)(dt2 * 4294967296.0f);
    }
    switch (TD.model) {                 /* each sound its own loop: no choice made a sample at a time */
    case MD_ORGAN:
        for (i = 0; i < n; i++) {
            float s = sin_ph(p1) + 0.5f * sin_ph(p1 << 1) + (d2 ? 0.7f * sin_ph(p2) : 0.0f);
            p1 += d1; p2 += d2;
            bus[PART_DRONE][i] += s * (g0 + dg * (float)i) * 0.27f;   /* (each sound's level: as loud as the reed's) */
        }
        break;
    case MD_STRINGS:
    case MD_BRASS:                      /* saws through the low-pass (brass: brighter, a little rise at its corner) */
        for (i = 0; i < n; i++) {
            float s = saw_s(p1, dt) + (d2 ? 0.7f * saw_s(p2, dt2) : 0.0f), h;
            s3 += lpF * s4; h = s - s3 - lq * s4; s4 += lpF * h + DN;
            p1 += d1; p2 += d2;
            bus[PART_DRONE][i] += s3 * (g0 + dg * (float)i) * (TD.model == MD_BRASS ? 0.36f : 0.35f);
        }
        break;
    case MD_VIOLIN:
    case MD_ACCORDION:                  /* a saw (bowed) or a narrow pulse (the reeds), its body, the low-pass */
        for (i = 0; i < n; i++) {
            float s = TD.model == MD_VIOLIN ? saw_s(p1, dt) + (d2 ? 0.7f * saw_s(p2, dt2) : 0.0f)
                                            : pulse_s(p1, dt, TD.pw) + (d2 ? 0.7f * pulse_s(p2, dt2, TD.pw) : 0.0f), h;
            s1 += bpF * s2; h = s - s1 - bq * s2; s2 += bpF * h + DN;
            s = s * TD.dryMix + s2 * TD.bpMix;
            s3 += lpF * s4; h = s - s3 - lq * s4; s4 += lpF * h + DN;
            p1 += d1; p2 += d2;
            bus[PART_DRONE][i] += s3 * (g0 + dg * (float)i) * (TD.model == MD_VIOLIN ? 0.45f : 0.30f);
        }
        break;
    case MD_NEY: {                      /* a pure tone and its breath, the breath tuned to the note */
        float nf = svf_f(f), nz = TD.noise * 0.6f;
        for (i = 0; i < n; i++) {
            float s = sin_ph(p1) + (d2 ? 0.7f * sin_ph(p2) : 0.0f), h, b = noise();
            s1 += nf * s2; h = b - s1 - 0.12f * s2; s2 += nf * h + DN;
            s += s2 * nz;
            p1 += d1; p2 += d2;
            bus[PART_DRONE][i] += s * (g0 + dg * (float)i) * 0.30f;
        }
        break;
    }
    default:                            /* the reed: the arghul's long pipe */
        for (i = 0; i < n; i++) {
            float s = pulse_s(p1, dt, 0.46f) + (d2 ? 0.7f * pulse_s(p2, dt2, 0.46f) : 0.0f), h;
            s1 += bpF * s2; h = s - s1 - bq * s2; s2 += bpF * h + DN;
            s = clip3((s * 0.5f + s2 * 1.2f) * TD.drive) * TD.driveN;
            s3 += lpF * s4; h = s - s3 - lq * s4; s4 += lpF * h + DN;
            p1 += d1; p2 += d2;
            bus[PART_DRONE][i] += s3 * (g0 + dg * (float)i) * 0.35f;
        }
        break;
    }
    DR.ph1 = p1; DR.ph2 = p2; DR.s1 = s1; DR.s2 = s2; DR.s3 = s3; DR.s4 = s4;
}

/* reads a delay line of a power-of-two length, `d` samples back, between two points */
static inline float tap(const float *b, uint32_t w, uint32_t mask, float d)
{
    float rp = (float)w - d + (float)(mask + 1u) * 4.0f;
    uint32_t ri = (uint32_t)rp;
    float fr = rp - (float)ri;
    return b[ri & mask] * (1.0f - fr) + b[(ri + 1u) & mask] * fr;
}

static float volS[DAB_NTRK], sndS[DAB_NTRK];   /* each track's volume and share of the effects as the last block mixed them */
static uint32_t busWas;                 /* the tracks that sounded in the last block */
static void post_chain(int32_t *lr, int n, float gain, int lowAny, uint32_t busOn)
{
    float vol[DAB_NTRK], snd[DAB_NTRK], sq_sum = 0.0f, lsum = 0.0f, lvl;
    float gd = par[P_DRUMS] * 0.34f * fadeG;   /* (the drums all together; the bass with them in a fade) */
    uint8_t act[DAB_NTRK], grp[DAB_NTRK];
    int on[DAB_NTRK], nact2 = 0, drumAny;
    float hissAmt = par[P_HISS] * 0.012f, pa = par[P_PA];
    int brightOn = par[P_BRIGHT] < 0.999f, driveOn = par[P_DRIVE] > 0.001f, chorusOn = par[P_CHORUS] > 0.001f && !lite, ensOn;
    int wowOn = par[P_WOW] > 0.001f, ageOn = par[P_AGE] > 0.001f, paOn = pa > 0.001f;
    int echoOn = par[P_ECHO] > 0.001f, revOn = par[P_REVERB] > 0.001f, i, k;
    if (lowAny) ensLive = 2048;
    else if (ensLive > 0) ensLive -= n;
    for (k = 0; k < DAB_NTRK; k++) {     /* the mixer: which tracks' filters run this block */
        if (busOn >> k & 1u) partLive[k] = partLive[k] > 4096 ? partLive[k] : 4096;
        if (partLive[k] > 0) partLive[k] -= n;
        on[k] = partLive[k] > 0 && (sp[k].mode || sp[k].want || sp[k].g != 0.0f || sp[k].gS != 0.0f);
        if (partLive[k] <= 0) { partLive[k] = 0; strip_settle(&sp[k]); }
    }
    /* the drums, each into the track it was hit from, the kit's tone over each track's */
    drumAny = drumLive > 0;
    if (drumAny) {
        int m = n < drumLive ? n : (int)drumLive;
        for (i = 0; i < m; i++) drums(i);
        drumLive -= m;
        for (k = 0; k < DAB_NTRK; k++)   /* (each drum track: the kit's tone; the drums' volume: with its own, below) */
            if (partLive[k] > 0 && SF_DRUM(tfam[k])) {
                float *x = bus[k], lp = dlpT[k];
                busOn |= 1u << k;
                if (K.tone < 1.0f) {
                    for (i = 0; i < n; i++) { lp += K.tone * (x[i] - lp) + DN; x[i] = lp; }
                    dlpT[k] = lp;
                }
            }
    }
    /* each track: a bass sound's tone, its filter and bass, its volume and share of the effects; where it joins the
     * mix by its family (the lead's and the drone's through the chorus and drive, the left hand's through its ensemble,
     * the bass and the drums after them) */
    for (k = 0; k < DAB_NTRK; k++) {
        int f = tfam[k];
        vol[k] = muteM >> k & 1u ? 0.0f : par[PART_VOL[k]] * (f == SF_BASS ? fadeG : f == SF_DRONE && k == PART_DRONE ? droneLvl
                                                              : SF_DRUM(f) ? gd : 1.0f);
        snd[k] = vol[k] * par[P_SEND + k] * (f == SF_LEFT ? 1.1f : 1.0f);
        if (!(busOn >> k & 1u)) {       /* (silent: its next sound begins at its volume as it is then) */
            if (f == SF_BASS && fBassT[k].z1 != 0.0f) fBassT[k].z1 = fBassT[k].z2 = 0.0f;
            volS[k] = vol[k]; sndS[k] = snd[k];
            continue;
        }
        if (f == SF_BASS) for (i = 0; i < n; i++) bus[k][i] = bq_run(&fBassT[k], bus[k][i] * 0.6f);
        if (on[k]) strip_block(&sp[k], bus[k], n);
        grp[k] = (uint8_t)(f == SF_LEFT ? 1 : f == SF_BASS || SF_DRUM(f) ? 2 : 0);
        act[nact2++] = (uint8_t)k;
    }
    for (i = 0; i < n; i++) gm[0][i] = gm[1][i] = gm[2][i] = gm[3][i] = 0.0f;
    for (k = 0; k < nact2; k++) {        /* the tracks into their places in the mix, and the effects' share */
        int t = act[k];
        const float *x = bus[t];
        float *o = gm[grp[t]], *e = gm[3], v = vol[t], e2 = snd[t], v0 = volS[t], e0 = sndS[t];
        if (!(busWas >> t & 1u)) { v0 = v; e0 = e2; }
        if (v0 == v && e0 == e2) {
            if (tfam[t] == SF_LEAD)      /* (the lead's level: for the singer's mouth) */
                for (i = 0; i < n; i++) { float q = x[i] * v; o[i] += q; lsum += q * q; }
            else
                for (i = 0; i < n; i++) o[i] += x[i] * v;
            if (e2 != 0.0f)
                for (i = 0; i < n; i++) e[i] += x[i] * e2;
        } else {                         /* its volume or its share moved (a fader, a mute, a lock) while it sounds:
                                          * half the way in this block, along a line, so nothing clicks */
            float v1 = v0 + (v - v0) * 0.5f, e1 = e0 + (e2 - e0) * 0.5f, dv, de;
            if (v1 - v < 0.002f && v - v1 < 0.002f) v1 = v;
            if (e1 - e2 < 0.002f && e2 - e1 < 0.002f) e1 = e2;
            dv = (v1 - v0) / (float)n; de = (e1 - e0) / (float)n;
            for (i = 0; i < n; i++) {
                float q = x[i] * v0;
                o[i] += q; e[i] += x[i] * e0;
                if (tfam[t] == SF_LEAD) lsum += q * q;
                v0 += dv; e0 += de;
            }
            v = v1; e2 = e1;
        }
        volS[t] = v; sndS[t] = e2;
    }
    busWas = busOn;
    ensOn = ensLive > 0 && !lite;
    if (!echoOn) echoStale = 1;
    else if (echoStale) { for (i = 0; i < ECHO_N; i++) echoBuf[i] = 0; echoLp = 0.0f; echoStale = 0; }
    if (!revOn) revStale = 1;
    else if (revStale) {
        for (k = 0; k < 4; k++) { for (i = 0; i < (int)COMB_N[k]; i++) COMB[k][i] = 0.0f; combLp[k] = 0.0f; }
        for (i = 0; i < 225; i++) ap0[i] = 0.0f;
        for (i = 0; i < 556; i++) ap1[i] = 0.0f;
        revStale = 0;
    }
    if (!paOn) slapStale = 1;
    else if (slapStale) { for (i = 0; i < SLAP_N; i++) slapBuf[i] = 0; paH = paL = paB = paLp = 0.0f; slapStale = 0; }

    for (i = 0; i < n; i++) {
        float pre = gm[0][i], low = gm[1][i], post = gm[2][i], send = gm[3][i];
        float eL, eR, oL, oR, ew = 0.0f, wet = 0.0f, xL, xR, m;
        /* the left hand's sound through three swept taps: an ensemble */
        ensBuf[ensW] = low;
        ensPh += 0.6f / SR; WRAP(ensPh);
        ensPh2 += 5.3f / SR; WRAP(ensPh2);
        if (ensOn) {
            eL = tap(ensBuf, ensW, 1023u, (0.006f + 0.0028f * sin_turn(ensPh) + 0.0003f * sin_turn(ensPh2)) * SR);
            eR = tap(ensBuf, ensW, 1023u, (0.006f + 0.0028f * sin_turn(fracf(ensPh + 0.5f)) + 0.0003f * sin_turn(fracf(ensPh2 + 0.5f))) * SR);
        } else eL = eR = low;
        ensW = (ensW + 1u) & 1023u;
        oL = pre + low * 0.5f + eL * 0.6f;
        oR = pre + low * 0.5f + eR * 0.6f;
        chBuf[chW] = (oL + oR) * 0.5f;
        if (chorusOn) {
            float c = par[P_CHORUS];
            chPh += 0.45f / SR; WRAP(chPh);
            oL = oL * (1.0f - 0.25f * c) + tap(chBuf, chW, 1023u, (0.012f + 0.004f * sin_turn(chPh)) * SR) * c * 0.85f;
            oR = oR * (1.0f - 0.25f * c) + tap(chBuf, chW, 1023u, (0.012f + 0.004f * sin_turn(fracf(chPh + 0.5f))) * SR) * c * 0.85f;
        }
        chW = (chW + 1u) & 1023u;
        if (driveOn) { oL = clip3(oL * K.dg2) * K.dn2; oR = clip3(oR * K.dg2) * K.dn2; }
        if (echoOn) {               /* the echo; changing its time bends the pitch of what is in it, as a tape's */
            float erp, rd;
            uint32_t eri;
            echoD += (K.echoTarget - echoD) * 0.0004f;
            erp = (float)echoW - echoD; if (erp < 0.0f) erp += (float)ECHO_N;
            if (erp >= (float)ECHO_N) erp = 0.0f;
            eri = (uint32_t)erp;
            rd = ((float)echoBuf[eri] + ((float)echoBuf[eri + 1u >= ECHO_N ? 0u : eri + 1u] - (float)echoBuf[eri]) * (erp - (float)eri)) * (1.0f / 16384.0f);
            echoLp += (rd - echoLp) * 0.35f + DN;
            echoBuf[echoW] = (int16_t)clampf((send + echoLp * par[P_ECHOFB]) * 16384.0f, -32767.0f, 32767.0f);
            if (++echoW >= ECHO_N) echoW = 0;
            ew = rd * par[P_ECHO];
            oL += ew; oR += ew;
        }
        if (revOn) {                /* a hall: four combs, two all-passes; the drums a little in it */
            float in = send + ew;
            for (k = 0; k < 4; k++) {
                float *cb = COMB[k], y = cb[combI[k]];
                combLp[k] += (y - combLp[k]) * 0.3f;
                cb[combI[k]] = in + combLp[k] * 0.82f + DN;
                if (++combI[k] >= COMB_N[k]) combI[k] = 0;
                wet += y;
            }
            { float y = ap0[apI[0]], x = wet + y * 0.5f; ap0[apI[0]] = x; wet = y - x * 0.5f; if (++apI[0] >= 225u) apI[0] = 0; }
            { float y = ap1[apI[1]], x = wet + y * 0.5f; ap1[apI[1]] = x; wet = y - x * 0.5f; if (++apI[1] >= 556u) apI[1] = 0; }
            wet *= 0.12f * par[P_REVERB];
        }
        xL = oL + wet + post; xR = oR + wet + post;
        if (paOn) {                 /* the wedding's PA: no lows, a horn's honk, pushed into its limit, the hall's slap */
            float x = (xL + xR) * 0.5f, h, sl;
            paH += (x - paH) * paHk; x -= paH;
            paL += paF * paB; h = x - paL - 0.7f * paB; paB += paF * h + DN;
            {                       /* driven into its limit, then back down: as loud as before, but pushed and honking */
                float g = 1.0f + 6.0f * pa;
                x = clip3((x + paB * 1.3f * pa) * g) * (1.15f / g);
            }
            paLp += (x - paLp) * paLk + DN;
            sl = (float)slapBuf[slapW] * (1.0f / 16384.0f);
            slapBuf[slapW] = (int16_t)clampf(paLp * 16384.0f, -32767.0f, 32767.0f);
            if (++slapW >= SLAP_N) slapW = 0;
            x = paLp + sl * 0.3f;
            xL = xL * (1.0f - pa) + x * pa; xR = xR * (1.0f - pa) + x * pa;
        }
        if (wowOn) {                /* the cassette it was copied onto */
            float dl;
            wowPh += 0.55f / SR; WRAP(wowPh);
            wowPh2 += 6.3f / SR; WRAP(wowPh2);
            wowL[wowW] = xL; wowR[wowW] = xR;
            dl = (0.006f + par[P_WOW] * (0.0035f * sin_turn(wowPh) + 0.00012f * sin_turn(wowPh2))) * SR;
            xL = tap(wowL, wowW, 1023u, dl); xR = tap(wowR, wowW, 1023u, dl);
            wowW = (wowW + 1u) & 1023u;
        }
        if (ageOn) {
            xL = clip3(xL * K.ageG) * K.ageN; xR = clip3(xR * K.ageG) * K.ageN;
            agL += (xL - agL) * K.ageLp + DN; agR += (xR - agR) * K.ageLp + DN;
            ahL += (agL - ahL) * K.ageHp; ahR += (agR - ahR) * K.ageHp;
            xL = agL - ahL; xR = agR - ahR;
        }
        if (hissAmt != 0.0f) { xL += noise() * hissAmt; xR += noise() * hissAmt; }
        if (brightOn) { xL = bq_run(&fBrL, xL); xR = bq_run(&fBrR, xR); }
        xL = clip3(xL * 0.75f); xR = clip3(xR * 0.75f);
        m = (xL + xR) * 0.5f;
        sq_sum += m * m;
        if ((scopeDiv++ & 3u) == 0u) { scopeRing[scopeW] = (int16_t)(m * 32767.0f); scopeW = (scopeW + 1u) % DAB_SCOPE; }
        lr[2 * i] = (int32_t)(xL * gain);
        lr[2 * i + 1] = (int32_t)(xR * gain);
    }
    for (k = 0; k < nact2; k++)          /* the buses used, empty again for the next block */
        for (i = 0; i < n; i++) bus[act[k]][i] = 0.0f;
    for (k = 0; k < DAB_NTRK; k++)       /* (and any written to but not mixed: a track's tail as its sound changed) */
        if (!(busOn >> k & 1u) && bus[k][0] != 0.0f) for (i = 0; i < BS; i++) bus[k][i] = 0.0f;
    lvl = fm_sqrtf(sq_sum / (float)n) * 4.0f;
    if (lvl > 1.0f) lvl = 1.0f;
    st.level += (lvl - st.level) * (lvl > st.level ? 0.5f : 0.1f);
    if (st.level < 1e-6f) st.level = 0.0f;
    lvl = fm_sqrtf(lsum / (float)n) * 5.0f;
    if (lvl > 1.0f) lvl = 1.0f;
    st.lead += (lvl - st.lead) * (lvl > st.lead ? 0.5f : 0.08f);
    if (st.lead < 1e-6f) st.lead = 0.0f;
}

static void block(int32_t *lr, int n, float gain)
{
    float vibS, bendT;
    int i, k, leadOn = 0, lowAny = 0;
    uint32_t busOn = 0;
    if (glN) glide_block();
    if (running) {
        uint32_t whole;
        beatAcc += par[P_TEMPO] / 60.0f / SR * (float)n * (float)BEAT;
        whole = (uint32_t)beatAcc; beatFix += whole; beatAcc -= (float)whole;
        for (i = 0; i < nact; i++) if ((int32_t)(beatFix - nextt[i]) >= 0) fire(i);
        trk_run();
        if (recTrk >= 0 && !recWait && !recOver && !SF_DRUM(tfam[recTrk])) {   /* (a first take: the step it is at) */
            int32_t rel = (int32_t)(beatFix - recAnchor) * DAB_TPB / (int32_t)BEAT, sx = rel < 0 ? 0 : rel / step_ticks();
            st.tstep[recTrk] = (uint8_t)(sx > 127 ? 127 : sx);
        }
        if (dirty) { dirty = 0; derive(); }   /* (a lock moved a knob at this step: before the step's notes sound) */
    }
    if (nArp || nArpSnd) {
        uint32_t clock;
        if (!running) { uint32_t whole; uaAcc += par[P_TEMPO] / 60.0f / SR * (float)n * (float)BEAT; whole = (uint32_t)uaAcc; uaClock += whole; uaAcc -= (float)whole; }
        clock = running ? beatFix : uaClock;
        arp_step(clock, n);
    }
    /* vibrato, the bend, the keys' pressure */
    vibPh += par[P_VIBRATE] * (float)n / SR; WRAP(vibPh);
    pressS += (par[P_PRESSURE] - pressS) * 0.2f;
    {
        float depth = K.vib + par[P_MOD] * 0.4f, turn = sin_turn(vibPh);
        vibS = turn * depth;
        vibP = (int)par[P_PRESS] == 3 ? turn * pressS * 0.5f : 0.0f;
    }
    bendT = par[P_BEND] * par[P_BENDRANGE];
    bendS += (bendT - bendS) * 0.08f;
    if (bendS < 0.0005f && bendS > -0.0005f) bendS = 0.0f;
    bendT = (int)par[P_PRESS] == 1 ? pressS * par[P_BENDRANGE] : (int)par[P_PRESS] == 2 ? -pressS * par[P_BENDRANGE] : 0.0f;
    pbendS += (bendT - pbendS) * 0.08f;
    if (pbendS < 0.0005f && pbendS > -0.0005f) pbendS = 0.0f;
    st.bend = bendS + pbendS;
    for (k = 0; k < DAB_NTRK; k++) {     /* each track's own sound's vibrato and bend (track 1's: the same as the whole
                                         * instrument's above, which the sounds without their own follow) */
        const float *p = tpar[k];
        float turn;
        vibPhT[k] += p[S_VIBRATE] * (float)n / SR; WRAP(vibPhT[k]);
        turn = sin_turn(vibPhT[k]);
        vibST[k] = turn * (KT[k].vib + par[P_MOD] * 0.4f);
        vibPT[k] = (int)par[P_PRESS] == 3 ? turn * pressS * 0.5f : 0.0f;
        bendT = par[P_BEND] * p[S_BENDRANGE];
        bendST[k] += (bendT - bendST[k]) * 0.08f;
        if (bendST[k] < 0.0005f && bendST[k] > -0.0005f) bendST[k] = 0.0f;
        bendT = (int)par[P_PRESS] == 1 ? pressS * p[S_BENDRANGE] : (int)par[P_PRESS] == 2 ? -pressS * p[S_BENDRANGE] : 0.0f;
        pbendST[k] += (bendT - pbendST[k]) * 0.08f;
        if (pbendST[k] < 0.0005f && pbendST[k] > -0.0005f) pbendST[k] = 0.0f;
    }
    droneLvl = 1.0f;
    for (k = 0; k < DAB_NTRK; k++) {     /* each track's pitch: moved at once on what sounds, eased so it does not click */
        float d = par[P_PITCH + k] - pitchS[k];
        pitchS[k] = d > 0.002f || d < -0.002f ? pitchS[k] + d * 0.3f : par[P_PITCH + k];
    }
    (void)k;                             /* (the buses: left empty by the last block's mix) */
    for (i = 0; i < NV; i++) {
        voice_t *v = &vo[i];
        if (!v->used) continue;
        busOn |= 1u << v->tr;
        if (!render_voice(v, n, vibS)) v->used = 0;
        else {
            if (v->fam == SF_LEAD && v->m == R_KEY && v->gate) leadOn = 1;
            if (v->fam == SF_LEFT) lowAny = 1;
        }
    }
    st.lead_on = (uint8_t)leadOn;
    leadIdle = leadOn ? 0u : leadIdle + (uint32_t)n;
    if (leadIdle > 0x40000000u) leadIdle = 0x40000000u;
    drone_block(n, vibS);
    if (droneG > 0.0f) busOn |= 1u << PART_DRONE;
    post_chain(lr, n, gain, lowAny, busOn);
#ifdef DAB_TRACE
    trace_t += (uint64_t)n;
#endif
}

/* ---------------------------------------------------------------- the API --- */
/* The load's guard. What a render costs is worked out before it from what will sound, in units (a thousand of the
 * host's instructions in a render of 256 frames, as measured there): the effects that are on, the drums while they
 * ring, the drone, and each note (the bass's 11, the others' 26 to 28). A unit's time on the FM-1 is learned: each
 * render is timed, and the slowest a unit has been lately is kept (it falls back slowly, it rises at once). It starts
 * at 134 ticks of the 24 MHz clock, as if the FM-1 ran 1.04 million of those instructions in a render's 5.8 ms (half
 * an instruction a cycle at 360 MHz). The notes that may sound at once are then as many as fit in three quarters of a
 * render's time; a note beyond them takes the oldest one's place (a released one first), so a render is never late. */
#define VOICE_FLOOR 4
#define CPU_R0 134.0f
#define U_NOTE 28
static float cpuR = CPU_R0;
#ifdef PLAT_HOST
static int cpuScale = 100;              /* (the tests: the FM-1 as if this % as slow as first assumed; not learned) */
#endif
static int base_units(void)             /* what the render costs before its notes */
{
    int u = 67 + (drumLive > 0 ? 158 : 0) + (droneG > 0.0f ? 20 : 0);
    u += 16 * ((par[P_CHORUS] > 0.001f) + (par[P_DRIVE] > 0.001f) + (par[P_PA] > 0.001f) + (par[P_WOW] > 0.001f) +
               (par[P_AGE] > 0.001f) + (par[P_HISS] > 0.001f) + (par[P_ECHO] > 0.001f) + (par[P_BRIGHT] < 0.999f));
    return u;
}
static int render_units(void)
{
    int i, u = base_units();
    for (i = 0; i < NV; i++) if (vo[i].used) u += vo[i].fam == SF_BASS ? 11 : vo[i].fam == SF_LEFT ? 28 : 26;
    return u;
}
static void guard(uint32_t n)           /* before a render of n frames: the notes that fit; any beyond, gone now */
{
    float budget = (float)n * (24000000.0f / SR) * 0.75f, per = cpuR * (float)n * (1.0f / 256.0f);
    int cap = (int)((budget / per - (float)base_units()) / (float)U_NOTE), used;
#ifdef PLAT_HOST
    per = CPU_R0 * (float)cpuScale * 0.01f * (float)n * (1.0f / 256.0f);
    cap = (int)((budget / per - (float)base_units()) / (float)U_NOTE);
#endif
    guard_cap = cap < 0 ? 0 : cap > NV ? NV : cap;
    used = voices_used();
    while (used > guard_cap) {          /* the oldest go, a released one first */
        int i, best = -1;
        for (i = 0; i < NV; i++) if (vo[i].used && (best < 0 || (!vo[i].gate && vo[best].gate) || (vo[i].gate == vo[best].gate && vo[i].vage < vo[best].vage))) best = i;
        if (best < 0) break;
        vo[best].used = 0; used--; st.shed++;
    }
    st.voice_cap = (uint8_t)guard_cap;
}
static void shed_to(int cap)            /* the oldest notes go until cap are left, released ones first */
{
    int i, used = voices_used();
    while (used > cap) {
        int best = -1;
        for (i = 0; i < NV; i++) if (vo[i].used && (best < 0 || (!vo[i].gate && vo[best].gate) || (vo[i].gate == vo[best].gate && vo[i].vage < vo[best].vage))) best = i;
        if (best < 0) break;
        vo[best].used = 0; used--;
    }
}
void dabke_render(int32_t *lr, uint32_t n, float gain)
{
    uint32_t off, t0;
    took = 0;
    guard(n);                           /* (before the keys come in, and again before each block) */
    while (q_r != q_w) { take(&q[q_r]); q_r = (q_r + 1u) % QN; took++; }
    if (dirty) { dirty = 0; derive(); }
    t0 = plat_ticks24();                /* (the notes' and effects' time, not the messages') */
    shed_wait = shed_wait > n ? shed_wait - n : 0u;
    if (overloads != overloads_seen) {      /* a render ran out of time: three fewer notes at once, now */
        overloads_seen = overloads; calm = 0;
        voice_cap = voice_cap > VOICE_FLOOR + 3 ? voice_cap - 3 : VOICE_FLOOR;
        shed_to(voice_cap);
    } else if (load_pct >= 80u) {           /* nearly out of time, by the platform's reading: two fewer, before it is heard */
        calm = 0;
        if (!shed_wait) {
            int used = voices_used();
            if (used < voice_cap) voice_cap = used;
            voice_cap = voice_cap > VOICE_FLOOR + 2 ? voice_cap - 2 : VOICE_FLOOR;
            shed_to(voice_cap);
            shed_wait = 4410u;
            if (voice_cap == VOICE_FLOOR && load_pct >= 88u) lite = 1;
        }
    } else if (load_pct <= 50u) {           /* room again: one note back every five seconds */
        lite = 0;
        if (voice_cap < NV && (calm += n) > 44100u * 5u) { calm = 0; voice_cap++; }
    } else calm = 0;
    if (fadeDir) {
        fadeG += (float)fadeDir * (float)n / (4.0f * SR);
        if (fadeG >= 1.0f) { fadeG = 1.0f; fadeDir = 0; }
        else if (fadeG <= 0.0f) { fadeDir = 0; set_run(0); fadeG = 1.0f; }
    }
    for (off = 0; off < n; off += BS) {
        int m = (int)(n - off < BS ? n - off : BS);
        if (off) guard(n);
        block(lr + 2u * off, m, gain);
    }
    {                                   /* the time it took: what a unit costs on this FM-1 now */
        uint32_t ticks = plat_ticks24() - t0;
        float units = (float)render_units() * (float)n * (1.0f / 256.0f), r;
        st.voices = (uint8_t)voices_used();
        if ((float)ticks > (float)n * (24000000.0f / SR)) st.late++;
#ifdef PLAT_HOST
        (void)ticks; (void)units; (void)r;
        if (units * CPU_R0 * (float)cpuScale * 0.01f > (float)n * (24000000.0f / SR)) st.late++;
#else
        r = units > 1.0f ? (float)ticks / units : cpuR;
        cpuR = r > cpuR ? r : cpuR * 0.998f + r * 0.002f;
        if (cpuR < 20.0f) cpuR = 20.0f;
#endif
    }
}
void dabke_key_audio(int manual, int note, int vel) { if (vel) key_on(manual, note, vel, 0); else key_off(manual, note); }
void dabke_bend_audio(float bend) { par[P_BEND] = clampf(bend, -1.0f, 1.0f); }
void dabke_mod_audio(float mod) { par[P_MOD] = clampf(mod, 0.0f, 1.0f); }
void dabke_overload(void) { if (took < 16u) overloads++; }
void dabke_load(int pct) { load_pct = (uint8_t)(pct < 0 ? 0 : pct > 255 ? 255 : pct); }

void dabke_key(int manual, int note, int vel) { post(M_KEY, ((manual & 31) << 8) | (note & 255), vel < 0 ? 0 : vel > 127 ? 127 : vel, 0, 0, 0); }
void dabke_press_debug(int32_t o[4])
{
    o[0] = (int32_t)(par[P_PRESSURE] * 127.0f + 0.5f); o[1] = (int32_t)(pbendS * 100.0f); o[2] = pressTr; o[3] = (int32_t)par[P_PRESS];
}
void dabke_pressure(int tracks, float v) { post(M_PAR, P_PRESSURE, tracks & 255, v, 0, 0); }
void dabke_tpar(int track, int id, float v) { post(M_TPAR, id, track, v, 0, 0); }
void dabke_par(int id, float v) { post(M_PAR, id, 0, v, 0, 0); }   /* (a track's filter and its bass: -1 .. 1, worked
                                                                     * out on the audio side as they move) */
void dabke_ntune(int pc, int cents) { post(M_NTUNE, pc, cents, 0, 0, 0); }
void dabke_rhythms(const uint8_t *list, int n)
{
    int i;
    if (n > 4) n = 4;
    for (i = 0; i < n; i++) sh_rhy[i] = list[i];
    sh_nrhy = (uint8_t)(n < 0 ? 0 : n);
    post(M_RHY, 0, 0, 0, 0, 0);
}
void dabke_run(int go) { post(M_RUN, go, 0, 0, 0, 0); }
void dabke_sync(int go) { post(M_SYNC, go, 0, 0, 0, 0); }
void dabke_acc(int go) { post(M_ACC, go, 0, 0, 0, 0); }
void dabke_style(int backing, int bass) { post(M_STYLE, backing, bass, 0, 0, 0); }
void dabke_fill(int type) { post(M_FILL, type, 0, 0, 0, 0); }
void dabke_fade(int dir) { post(M_FADE, dir, 0, 0, 0, 0); }
void dabke_drum(int k, float level, float tune, float length) { post(M_DRUM, k, 0, level, tune, length); }
void dabke_own(int slot, const uint8_t *g, int len, int div, int bpm)
{
    int i;
    (void)bpm;
    for (i = 0; i < DAB_NDRUM * DAB_RMAX; i++) sh_own[i] = g[i];
    post(M_OWN, slot, (len & 255) | (div << 8), 0, 0, 0);
}
void dabke_track_rec(int track) { post(M_TRKREC, track, 0, 0, 0, 0); }
void dabke_take_undo(int track) { post(M_UNDO, track, 0, 0, 0, 0); }
void dabke_clear_all(void) { post(M_CLEARALL, 0, 0, 0, 0, 0); }
void dabke_loop_set(int track, const uint32_t *ev, int n, int len, int bars, const uint8_t *takes, int ntakes, int played)
{
    int i;
    if (track < 0 || track >= DAB_NTRK) return;
    if (n > DAB_LOOP_N) n = DAB_LOOP_N;
    if (n < 0) n = 0;
    for (i = 0; i < n; i++) sh_ev[track][i] = ev[i];
    for (i = 0; i < DAB_TAKES; i++) sh_takes[track][i] = takes ? takes[i] : 0;
    sh_n[track] = (uint16_t)n; sh_len[track] = (uint16_t)(len < 0 ? 0 : len); sh_bars[track] = (uint8_t)bars;
    sh_ntakes[track] = (uint8_t)ntakes;
    sh_flen[track] = (uint16_t)(played < 0 ? 0 : played);
    post(M_LOOPSET, track, 0, 0, 0, 0);
}
void dabke_step(int track, int step, int lane, int how) { post(M_STEP, track, ((step & 127) << 8) | ((how & 3) << 4) | (lane & 15), 0, 0, 0); }
void dabke_note_add(int track, int pos, int len, int note, int newlen, int alone)
{
    post(M_NADD, (track & 7) | ((note & 127) << 4) | (alone ? 1 << 11 : 0), pos, (float)len, (float)newlen, 0);
}
int dabke_sound_drum(int f, int lane) { return f >= SF_KICK && f < SF_N ? GDR[f - SF_KICK][(lane < 0 ? 0 : lane) % GN[f - SF_KICK]] : 0; }
void dabke_note_del(int track, int from, int to) { post(M_NDEL, track, from, (float)to, 0, 0); }
void dabke_track_len(int track, int two, int ticks) { post(M_TLEN, track, two, (float)ticks, 0, 0); }
void dabke_lock(int place, uint32_t lock, float unit) { post(M_LOCK, (int16_t)(lock & 0xFFFFu), (int16_t)(lock >> 16), (float)place, unit, 0); }
void dabke_lock_clear(int track, int take) { post(M_LKCLEAR, track, take, 0, 0, 0); }
void dabke_mute(int mask) { post(M_MUTE, mask & 255, 0, 0, 0, 0); }
void dabke_track_hit(int track, int lane, int record) { post(M_DHIT, track, (lane & 255) | (record ? 256 : 0), 0, 0, 0); }
void dabke_track_hit_audio(int track, int lane, int record) { dhit(track, lane, record); }
void dabke_track_sound(int track, int family, int index) { post(M_TSND, track, ((family & 255) << 8) | (index & 255), 0, 0, 0); }
int dabke_family_size(int f)
{
    return f == SF_LEAD || f == SF_LEFT ? MD_COUNT : f == SF_DRONE ? DAB_NDRONE : f == SF_BASS ? DAB_BASSV
           : f >= SF_KICK && f < SF_N ? DAB_DRUMV : 0;
}
int dabke_sound_drums(int f) { return f >= SF_KICK && f < SF_N ? GN[f - SF_KICK] : 0; }
#ifdef PLAT_HOST
int dabke_host_voices(int t)            /* (the tests: the voices of track t sounding with their key or hold) */
{
    int i, c = 0;
    for (i = 0; i < NV; i++) if (vo[i].used && vo[i].tr == t && vo[i].gate) c++;
    return c;
}
int dabke_host_drone(void) { return (int)(droneG * 1000.0f); }   /* (the drone's level now, x 1000) */
int dabke_host_cpu(int pct) { if (pct > 0) cpuScale = pct; return guard_cap; }
int dabke_host_orn(void)                /* (the lead's sounding voices with a trill or a shake going on) */
{
    int i, c = 0;
    for (i = 0; i < NV; i++)
        if (vo[i].used && vo[i].fam == SF_LEAD && vo[i].gate && (vo[i].orn == ORN_TRILL || vo[i].shake >= 0.0f)) c++;
    return c;
}
int dabke_host_drone_note(void) { return droneNote; }
uint32_t dabke_host_lock(int i) { return i >= 0 && i < DAB_NLOCK ? lk[i] : 0; }   /* (the lock in place i, as the audio side has it) */
int dabke_host_dsound(int k) { return k >= 0 && k < DAB_NDRUM ? dtr[k] * 100 + dvar[k] : -1; }   /* (the track drum k was
                                         * last hit from x 100 + the number of the sound that played it) */
int dabke_host_lock_sums(void)          /* (its counts in step with its table: 0; else which is off) */
{
    int n[DAB_NTRK] = {0}, h[DAB_NTRK] = {0}, i, bad = 0;
    for (i = 0; i < DAB_NLOCK; i++) if (lk[i]) n[DAB_LK_TRK(lk[i])]++;
    for (i = 0; i < DAB_NLKS; i++) if (lkAt[i]) { if (!lk[lkAt[i] - 1] || lk_knob(lk[lkAt[i] - 1]) != i) bad |= 4; else h[DAB_LK_TRK(lk[lkAt[i] - 1])]++; }
    for (i = 0; i < DAB_NTRK; i++) { if (n[i] != lkN[i]) bad |= 1; if (h[i] != lkHeld[i]) bad |= 2; }
    return bad;
}
int dabke_host_par(int id) { return id >= 0 && id < P_COUNT ? (int)(par[id] * 1000.0f + (par[id] < 0.0f ? -0.5f : 0.5f)) : 0; }   /* (a knob as it sounds now, x 1000) */
int dabke_host_tpar(int t, int id)      /* (a knob or choice of track t's own sound as it sounds now, x 1000; DAB_PLAIN) */
{
    int s = sp_of(id);
    float v;
    if (t < 0 || t >= DAB_NTRK) return 0;
    if (id == DAB_PLAIN) return tplain[t];
    if (s < 0) return 0;
    v = tpar[t][s];
    return (int)(v * 1000.0f + (v < 0.0f ? -0.5f : 0.5f));
}
int dabke_host_vfam(int t)              /* (the family of a note sounding on track t: -1 none) */
{
    int i;
    for (i = 0; i < NV; i++) if (vo[i].used && vo[i].tr == t && vo[i].gate) return vo[i].fam;
    return -1;
}
#endif
void dabke_steps_set(const uint8_t *lo, const uint8_t *hi, int edited, const uint8_t *steps, int two)
{
    int t, i, k;
    for (t = 0; t < DAB_NTRK; t++) {
        for (i = 0; i < DAB_TLANE; i++)
            for (k = 0; k < DAB_TPAT / 2; k++) {
                sh_pat[t][i][k] = lo ? lo[(t * DAB_TLANE + i) * (DAB_TPAT / 2) + k] : 0;
                sh_pat[t][i][k + DAB_TPAT / 2] = hi ? hi[(t * DAB_TLANE + i) * (DAB_TPAT / 2) + k] : 0;
            }
        sh_dn[t] = steps ? steps[t] : 0;
    }
    sh_edit = (uint8_t)edited; sh_two = (uint8_t)two;
    post(M_STEPSSET, 0, 0, 0, 0, 0);
}
void dabke_hold(int on) { post(M_HOLD, on, 0, 0, 0, 0); }
void dabke_panic(void) { post(M_PANIC, 0, 0, 0, 0, 0); }
int dabke_idle(void) { return q_r == q_w; }

const dabke_status_t *dabke_status(void) { return &st; }
const int16_t *dabke_scope(void)
{
    uint32_t w = scopeW, i;
    for (i = 0; i < DAB_SCOPE; i++) scopeOut[i] = scopeRing[(w + i) % DAB_SCOPE];
    return scopeOut;
}
int dabke_rhythm_count(void) { return DAB_NRHYTHM; }
const char *dabke_rhythm_name(int i) { return i >= 0 && i < DAB_NRHYTHM ? RHYTHMS[i].name : ""; }
const char *dabke_rhythm_group(int i) { return i >= 0 && i < DAB_NRHYTHM ? RHYTHMS[i].group : ""; }
int dabke_rhythm_bpm(int i) { return i >= 0 && i < DAB_NRHYTHM ? RHYTHMS[i].bpm : 120; }
int dabke_rhythm_grid(int i, uint8_t g[DAB_NLANE][DAB_RMAX], int *len, int *div)
{
    const rhythm_t *p;
    int k, s;
    if (i < 0 || i >= DAB_NRHYTHM) i = 0;
    p = &RHYTHMS[i];
    *len = p->len > DAB_RMAX ? DAB_RMAX : p->len;
    *div = p->div;
    for (k = 0; k < DAB_NLANE; k++)
        for (s = 0; s < DAB_RMAX; s++) {
            char c = p->lane[k] && s < *len ? p->lane[k][s] : '.';
            uint8_t v = 0;
            if (k < DAB_NDRUM) v = (uint8_t)(c == 'X' ? 3 : c == 'x' ? 2 : c == 'o' ? 1 : 0);
            else if (k == DAB_LBASS) v = (uint8_t)(c == 'r' ? 1 : c == 'f' ? 2 : c == 'o' ? 3 : c == 't' ? 4 : c == '4' ? 5 : c == '7' ? 6 : 0);
            else v = (uint8_t)(c == 'x' ? 1 : c == '_' ? 2 : 0);
            g[k][s] = v;
        }
    return *len;
}

void dabke_init(void)
{
    int i;
    for (i = 0; i <= 2048; i++) SINE[i] = fm_sinf((float)i * (TWO_PI / 2048.0f));
    for (i = 0; i < 12; i++) ntune[i] = 0;
    for (i = 0; i < 4; i++) {           /* (nothing held, no key down: a start) */
        int m;
        for (m = 0; m < NMAN; m++) { keyDn[m][i] = 0; heldK[m][i] = 0; }
        heldArp[i] = 0;
    }
    st.hold = 0; st.held_n = 0; droneIdle = 0x7FFFFFFFu;
    /* a start: nothing waits, nothing sounds, nothing runs, nothing records, no track has anything yet. (The FM-1's
     * memory is empty at power-on; the simulator's reboot keeps it, so it is said here) */
    q_r = q_w;
    for (i = 0; i < NV; i++) vo[i].used = 0;
    for (i = 0; i < 128; i++) { lowerHeld[i] = 0; arpDown[i] = 0; recOpen[i] = 0; }
    nLower = 0; nAcc = 0; bassNote = -1; holding = 0; chordRoot = -1; accId = -1;
    nArp = 0; nArpSnd = 0; nDown = 0; nms = 0; monoNote = -1; droneNote = -1; drnV[0] = drnV[1] = -1;
    arpPlayed = 0; arpSync = 1; st.arp_n = 0;
    running = 0; st.running = 0; fadeDir = 0; fadeG = 1.0f; droneG = 0.0f; drumLive = 0; fill = 0; inFill = 0; st.in_fill = 0;
    recTrk = -1; recWait = 0; recOver = 0; dedit = 0;
    for (i = 0; i < DAB_NTRK; i++) {
        int k;
        kt[i].n = 0; kt[i].len = 0; kt[i].bars = 0; kt[i].takes = 0; kt[i].flen = 0; kt[i].wait = 0;
        for (k = 0; k < DAB_LOOP_N / 8; k++) kt[i].snd[k] = 0;
        partLive[i] = 0; st.tstep[i] = 0;
    }
    for (i = 0; i < DAB_NLOCK; i++) lk[i] = 0;
    for (i = 0; i < DAB_NLKS; i++) { lkAt[i] = 0; st.lk_at[i] = 0; }
    for (i = 0; i < DAB_NLK; i++) lkU[i] = 0.0f;
    for (i = 0; i < DAB_NTRK; i++) { lkN[i] = 0; lkHeld[i] = 0; sndLk[i] = 0; dn[i] = 0; volS[i] = sndS[i] = 0.0f; }
    glN = 0; tl2 = 0; muteM = 0; busWas = 0; nSnap = 0; drumTake = 0; snapArm = -1;
    for (i = 0; i < DAB_NTRK; i++) { st.tfam[i] = tfam[i]; st.tidx[i] = tidx[i]; pitchS[i] = 0.0f; }
    for (i = 0; i < P_COUNT; i++) par[i] = 0.0f;
    par[P_LEAD] = 0.8f; par[P_LEFT] = 0.55f; par[P_BASS] = 0.75f; par[P_DRUMS] = 0.8f; par[P_DRONE] = 0.35f;
    par[P_TEMPO] = 140.0f;
    par[P_TONE] = 0.7f; par[P_BUZZ] = 0.45f; par[P_DETUNE] = 14.0f; par[P_BREATH] = 0.15f; par[P_BODY] = 0.55f;
    par[P_ATTACK] = 0.01f; par[P_RELEASE] = 0.05f; par[P_GLIDE] = 0.03f;
    par[P_VIBRATE] = 5.5f; par[P_VIBDEPTH] = 0.2f; par[P_VIBDELAY] = 0.25f; par[P_ORNSPEED] = 12.0f; par[P_ORNAMOUNT] = 0.4f;
    par[P_ECHOTIME] = 0.3f; par[P_ECHOFB] = 0.45f; par[P_REVERB] = 0.25f; par[P_BRIGHT] = 1.0f;
    par[P_VELAMT] = 0.5f; par[P_BASSTONE] = 600.0f; par[P_BASSDECAY] = 1.0f; par[P_ACCENT] = 0.5f;
    par[P_KITTUNE] = 1.0f; par[P_KITDECAY] = 1.0f; par[P_KITTONE] = 1.0f;
    par[P_GDRUM] = par[P_GKICK] = par[P_GCLAP] = par[P_GMETAL] = 1.0f;
    for (i = 0; i < DAB_NPART; i++) par[P_SEND + i] = i < PART_KICK ? (i == PART_BASS ? 0.0f : 1.0f) : 0.3f;
    clapV = -1;
    for (i = 0; i < DAB_NTRK; i++) st.trk_ev[i] = kt[i].ev;
    st.rec_track = -1;
    par[P_ARPRATE] = 4.0f; par[P_ARPOCT] = 0.0f; par[P_ARPGATE] = 0.6f; par[P_ARPREP] = 1.0f; par[P_ARPSWING] = -1.0f;
    par[P_LEFTMODEL] = (float)MD_STRINGS; par[P_BENDRANGE] = 1.0f; par[P_SCALE] = (float)0xAB5;
    for (i = 0; i < DAB_NDRUM; i++) { dlv[i] = 1.0f; dtune[i] = 1.0f; ddec[i] = 1.0f; }
    active[0] = 0; nact = 1;
    compile(0, 0);
    bq_set(&fTek, BP, 3400.0f, 2.2f); bq_set(&fKa, BP, 2600.0f, 1.8f); bq_set(&fClap, BP, 1250.0f, 1.4f);
    bq_set(&fSn, BP, 3200.0f, 0.8f); bq_set(&fRiq, BP, 900.0f, 1.0f);
    paF = svf_f(2500.0f); paHk = 1.0f - fm_expf(-TWO_PI * 170.0f / SR); paLk = 1.0f - fm_expf(-TWO_PI * 5200.0f / SR);
    st.left_root = -1; st.beats = 2; st.steps = glen[0];
    leadIdle = 0x40000000u;
    for (i = 0; i < DAB_NTRK; i++) {    /* each track's own sound as it starts: the whole instrument's (its vibrato's
                                         * phase with the whole instrument's) */
        int k;
        for (k = 0; k < NSP; k++) tpar[i][k] = par[sp_id(k)];
        tplain[i] = 0;
        vibPhT[i] = vibPh; bendST[i] = pbendST[i] = vibST[i] = vibPT[i] = 0.0f;
    }
    for (i = 0; i < DAB_NLK; i++) parN[i] = par[i];   /* (each knob's normal value: as it starts) */
    for (i = DAB_NLK; i < DAB_NLKS; i++) parN[i] = tpar[(i - DAB_NLK) / DAB_NSK][(i - DAB_NLK) % DAB_NSK];
    dsteps_tell();
    trk_tell(1);
    derive();
}
