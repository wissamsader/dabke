/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 STUDIOMORPHIK */
/* DABKE's home page under the singer, and the step view, on RAGA MEOW's paper. Included by app.c after scene.c and the
 * label's pieces (the drawing helpers, the paper's inks, the dots and the glasses).
 *
 * The home page, under RAGA MEOW's top line and DABKE's scene: the eight tracks as lanes of steps, all at once, small
 * (six rows each): each lane its number (on red: the picked one), a short bar by it when what it plays is his, its steps
 * over its whole length (a block: something of his begins there; a low faint one: the rhythm's; a blue line on top: a
 * lock; red: the step playing, and a lane recording), faint when it is silent, its steps counted at the right. Under
 * them the rhythm (and the one added), the home page's rows as dots, the tempo with a metronome ticking on the beat.
 *
 * The step view (step mode on the home page): a label of its own, STEPS on its ribbon. The picked track alone: what its
 * steps play (a drum sound: its drums lane by lane; a note sound: its notes by pitch, as long as they sound), the row of
 * steps as the white keys have them (red: his; outlined: the rhythm's; dotted: empty; a padlock on a locked one; the
 * step playing underlined; the held one framed), its bars at the left; the track and whose it is, its length; what a
 * tap adds, its locks counted; the page shown. On a knob page in step mode the same row of steps and words sit under
 * the knobs' pictures.
 *
 * Drawn in bands, each redrawn alone when what it shows moves. */

#define HM_SCENE_Y 17                    /* the home page: the scene, rows 17..128 */
#define HM_LANE_Y 131                    /* its lanes, six rows each: 130..178 */
#define HM_FOOT_Y 184                    /* the rhythm, the dots, the tempo: 179..190 */
#define PN_DIM K_DIM
#define PN_LIVE K_RED                    /* what is picked, what is his in the step view */
#define SQ_X 34                          /* the step view (inside the frame): its steps from here */
#define SQ_W 192
#define SQ_ROLL_Y 50                     /* what its steps play */
#define SQ_ROLL_H 46
#define SQ_ROW_Y 102                     /* its row of steps (the white keys) */
#define SQ_ROW_H 22
#define SQ_INFO_Y 143                    /* its lines of words */

/* boxes with their corners cut (a pixel each), filled or drawn; dotted for a thing that is off */
static void pbox(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    box(x + 1, y, w - 2, 1, c);
    box(x, y + 1, w, h - 2, c);
    box(x + 1, y + h - 1, w - 2, 1, c);
}
static void pframe(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    box(x + 1, y, w - 2, 1, c);
    box(x + 1, y + h - 1, w - 2, 1, c);
    box(x, y + 1, 1, h - 2, c);
    box(x + w - 1, y + 1, 1, h - 2, c);
}
static void pdots(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    int32_t i;
    for (i = x + 1; i < x + w - 1; i += 2) {
        box(i, y, 1, 1, c);
        box(i, y + h - 1, 1, 1, c);
    }
    for (i = y + 2; i < y + h - 1; i += 2) {
        box(x, i, 1, 1, c);
        box(x + w - 1, i, 1, 1, c);
    }
}

/* ---- K1 to K4 on the home page with ALL picked: the four parts' volumes, each said as it turns ---- */
enum { PT_LEAD, PT_DRONE, PT_LEFT, PT_DRUMS, PT_N };
static const char *const PT_NAME[PT_N] = {"LEAD", "DRONE", "LEFT HAND", "DRUMS"};
#define MUTED(k) (set.mute >> (k) & 1u)
static int left_bass(void) { return set.ch[C_LEFT] == 2; }
static int part_muted(int p)             /* muted on the mixer (the left hand: all it plays; the drums: all four) */
{
    switch (p) {
    case PT_LEAD: return MUTED(PART_LEAD);
    case PT_DRONE: return MUTED(PART_DRONE);
    case PT_LEFT: return MUTED(PART_LEFT) && (!left_bass() || MUTED(PART_BASS));
    default: return MUTED(PART_KICK) && MUTED(PART_DRUMS) && MUTED(PART_CLAPS) && MUTED(PART_METAL);
    }
}
static void part_turn(int p, int n)      /* a part's volume, and nothing else: "LEAD 76" (muted: "LEAD 76, MUTED") */
{
    int32_t d = n * 4, v;
    char t[40] = "", x[8];
    switch (p) {
    case PT_LEAD:
        ctl_turn(P_LEAD, ctl_base(P_LEAD) + d);
        v = ctl_base(P_LEAD);
        break;
    case PT_DRONE:
        ctl_turn(P_DRONE, ctl_base(P_DRONE) + d);
        drone_up(n);
        v = ctl_base(P_DRONE);
        break;
    case PT_LEFT:                        /* the left hand's own sound (track 3), and the bass line with it while the
                                          * left keys play the bass and backing */
        ctl_turn(P_LEFT, ctl_base(P_LEFT) + d);
        if (left_bass())
            ctl_turn(P_BASS, ctl_base(P_BASS) + d);
        v = ctl_base(P_LEFT);
        break;
    default:
        set_par(P_DRUMS, set.par[P_DRUMS] + d);
        v = set.par[P_DRUMS];
    }
    text_cat(t, PT_NAME[p]);
    text_cat(t, " ");
    int_text(v, x);
    text_cat(t, x);
    if (part_muted(p))
        text_cat(t, ", MUTED");
    say(t);
}

/* a track's kind of sound, drawn small (10 x 11, its left top at x, y) */
static void track_icon(int t, int32_t x, int32_t y, uint16_t c)
{
    float fx = (float)x, fy = (float)y;
    switch (t) {
    case PART_LEAD:                      /* a pipe: its body, its holes, its bell */
        seg(fx + 1.0f, fy + 10.0f, fx + 8.0f, fy + 2.0f, c, 2);
        trap(fy, fy + 3.0f, fx + 6.0f, fx + 10.0f, fx + 7.0f, fx + 9.0f, c, 0, -1);
        break;
    case PART_DRONE:                     /* a long tone: a line and its wave over it */
        box(x, y + 8, 10, 2, c);
        seg(fx, fy + 4.0f, fx + 3.0f, fy + 2.0f, c, 1);
        seg(fx + 3.0f, fy + 2.0f, fx + 6.0f, fy + 5.0f, c, 1);
        seg(fx + 6.0f, fy + 5.0f, fx + 10.0f, fy + 2.0f, c, 1);
        break;
    case PART_LEFT:                      /* a chord: three notes stacked */
        box(x + 1, y + 1, 4, 2, c);
        box(x + 4, y + 5, 4, 2, c);
        box(x + 1, y + 9, 4, 2, c);
        box(x + 8, y, 1, 11, c);
        break;
    case PART_BASS:                      /* a low note: a round head on a long stem down */
        disc(fx + 3.5f, fy + 8.0f, 2.6f, c);
        box(x + 5, y, 1, 8, c);
        box(x + 5, y, 4, 1, c);
        break;
    case PART_KICK:                      /* a big drum: its face */
        disc(fx + 5.0f, fy + 5.5f, 5.0f, c);
        disc(fx + 5.0f, fy + 5.5f, 1.6f, c == K_BLACK ? K_WH : K_BLACK);
        break;
    case PART_DRUMS:                     /* a darbuka: its head, its goblet */
        ellipse(fx + 5.0f, fy + 1.5f, 4.5f, 1.5f, c);
        trap(fy + 2.0f, fy + 6.0f, fx + 1.0f, fx + 9.0f, fx + 4.0f, fx + 6.0f, c, 0, -1);
        trap(fy + 6.0f, fy + 11.0f, fx + 4.0f, fx + 6.0f, fx + 2.0f, fx + 8.0f, c, 0, -1);
        break;
    case PART_CLAPS:                     /* two hands meeting */
        seg(fx + 1.0f, fy + 10.0f, fx + 4.0f, fy + 1.0f, c, 2);
        seg(fx + 9.0f, fy + 10.0f, fx + 6.0f, fy + 1.0f, c, 2);
        break;
    default: {                           /* a riq: its thin ring, broken by its cymbals' slots */
        uint16_t bg = c == K_BLACK ? K_WH : K_BLACK;
        disc(fx + 5.0f, fy + 5.5f, 5.0f, c);
        disc(fx + 5.0f, fy + 5.5f, 3.8f, bg);
        box(x + 4, y, 3, 2, bg);
        box(x + 4, y + 9, 3, 2, bg);
        box(x, y + 4, 2, 3, bg);
        box(x + 8, y + 4, 2, 3, bg);
        break;
    }
    }
}

/* the lanes' numbers: small digits, 3 x 5 (too small for a face), a row of three bits each, top to bottom */
static const uint16_t DIG35[10] = {0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249, 0x7BEF, 0x7BCF};
static void digits(int32_t x, int32_t y, int v, int right, uint16_t c)   /* v from x (right: ending at x), its top at y */
{
    char t[12];
    int n, i, r, k;
    int_text(v, t);
    for (n = 0; t[n]; n++)
        ;
    if (right)
        x -= 4 * n - 1;
    for (i = 0; i < n; i++, x += 4) {
        uint16_t g = t[i] >= '0' && t[i] <= '9' ? DIG35[t[i] - '0'] : 0;
        for (r = 0; r < 5; r++)
            for (k = 0; k < 3; k++)
                if (g >> (14 - 3 * r - k) & 1u)
                    box(x + k, y + r, 1, 1, c);
    }
}

/* ---- the lanes: the eight tracks, each the bar of its steps that is playing ---- */
static void lane(int t, int32_t y)       /* a track's lane, five rows from y: its number (on red: picked), a short bar by
                                          * it when what it plays is his, its steps over its whole length (a block: his; a
                                          * low faint one: the rhythm's; a blue line on top: a lock; red: the step
                                          * playing), faint while it is silent, red while it records; its steps counted */
{
    const dabke_status_t *s = dabke_status();
    int P = page_steps(), n = trk_nsteps(t), pk = picked() == t, his = trk_his(t), q = step_q();
    int rec = s->rec_track == t, mute = mute_eff(t), cur = s->running ? (int)s->tstep[t] : -1, i;
    int div = gview_div < 1 ? 4 : gview_div;
    uint32_t has[11] = {0}, lk[11] = {0};
    uint16_t ink = mute ? K_DIM : rec ? K_RED : pk ? K_INK : mixc(K_INK, K_PAPER, 0.3f);
    uint16_t soft = mute ? mixc(K_DIM, K_PAPER, 0.4f) : pk ? K_GR : mixc(K_GR, K_PAPER, 0.3f);
    if (n > 352)
        n = 352;
    if (pk)
        box(1, y - 1, 9, 7, K_RED);
    digits(4, y, t + 1, 0, pk ? K_PAPER : mute ? K_DIM : rec ? K_RED : K_INK);
    if (his)                             /* what it plays is his: a short bar by its number */
        box(11, y, 1, 5, rec ? K_RED : K_INK);
    if (trk_loop(t)) {                   /* (a loop: its notes' steps, in one pass) */
        for (i = 0; i < s->trk_n[t] && i < DAB_LOOP_N; i++) {
            int p = (int)DAB_EV_POS(s->trk_ev[t][i]);
            if (p < s->trk_len[t] && p / q < n)
                has[p / q >> 5] |= 1u << (p / q & 31);
        }
    }
    for (i = 0; i < DAB_NLOCK; i++) {
        uint32_t l = sq.lock[i];
        if (l && DAB_LK_TRK(l) == t && (int)DAB_LK_STEP(l) < n)
            lk[DAB_LK_STEP(l) >> 5] |= 1u << (DAB_LK_STEP(l) & 31);
    }
    for (i = 0; i < n; i++) {
        int tr = trk_loop(t) ? (int)(has[i >> 5] >> (i & 31) & 1u) : step_trig(t, i);
        int32_t x0 = 14 + (i * 210) / n, cw = 14 + ((i + 1) * 210) / n - x0 - (n <= 64 ? 1 : 0);
        if (cw < 1)
            cw = 1;
        if (i && i % P == 0)             /* (a bar begins) */
            box(x0 - 1, y, 1, 5, K_DIM);
        if (tr == 1)
            box(x0, y, cw, 5, i == cur ? K_RED : ink);
        else if (tr == 2)
            box(x0, y + 1, cw, 3, i == cur ? K_RED : soft);
        else if (i == cur)
            box(x0, y + 2, cw, 2, K_RED);
        else if (n <= 32 || i % div == 0)
            box(x0 + cw / 2, y + 2, 1, 1, i % div == 0 ? K_GR : K_DIM);
        if (lk[i >> 5] >> (i & 31) & 1u)     /* a lock on it */
            box(x0, y, cw, 1, K_LK);
    }
    digits(238, y, trk_nsteps(t), 1, pk ? K_INK : K_GR);   /* its length */
}
static void lanes(void)
{
    int t;
    for (t = 0; t < DAB_NTRK; t++)
        lane(t, HM_LANE_Y + t * 6);
}

/* ---- under the lanes: the rhythm (and the one added) at the left, the home page's rows as dots, the tempo at the
 * right, its metronome's arm left and right on the beats ---- */
static void home_foot(void)
{
    const dabke_status_t *s = dabke_status();
    char b[56], t[24];
    float mx, by = (float)(HM_FOOT_Y + 4), sw;
    if (!in_strip(HM_FOOT_Y - 5, HM_FOOT_Y + 7))
        return;
    rhythm_name_copy(b, ui.r0);
    if (ui.r1 >= 0) {
        text_cat(b, " + ");
        rhythm_name_copy(t, ui.r1);
        text_cat(b, t);
    }
    TXF(b, 6, HM_FOOT_Y + 4, 0, s->running ? K_INK : K_FAINT, 0, 98);
    row_dots(3, ui.row[PG_HOME] % 3, 111.0f, (float)HM_FOOT_Y);
    int_text(set.par[P_TEMPO], t);
    TX(t, 234, HM_FOOT_Y + 4, 0, K_INK, 2, 0);
    mx = (float)(234 - text_wf(t, 0)) - 9.0f;   /* the metronome: its body narrowing up, its arm out of it */
    sw = s->running ? ((ui.beat_count & 1u) ? 4.0f : -4.0f) : 3.0f;
    a_seg(mx - 4.5f, by, mx + 4.5f, by, 1.0f, K_INK);
    a_seg(mx - 4.5f, by, mx - 1.8f, by - 8.0f, 1.0f, K_INK);
    a_seg(mx + 4.5f, by, mx + 1.8f, by - 8.0f, 1.0f, K_INK);
    a_seg(mx - 1.8f, by - 8.0f, mx + 1.8f, by - 8.0f, 1.0f, K_INK);
    a_seg(mx, by - 1.0f, mx + sw, by - 9.0f, 1.2f, s->running ? K_RED : K_INK);
    a_ell(mx + sw * 0.6f, by - 5.8f, 1.3f, 1.3f, s->running ? K_RED : K_INK);
}

/* the lanes' moving part (the steps playing) and the metronome's: a change, and each is drawn again */
static uint32_t panel_body_sig(void)
{
    const dabke_status_t *s = dabke_status();
    uint32_t h = s->running;
    int t;
    if (s->running)
        for (t = 0; t < DAB_NTRK; t++)
            h = h * 131u + s->tstep[t];
    return h;
}
static uint32_t panel_head_sig(void) { return dabke_status()->running ? 1u + (ui.beat_count & 1u) : 0u; }

static void home_panel(void)             /* (not "panel": the device platform has a variable of that name) */
{
    if (in_strip(HM_LANE_Y - 1, HM_LANE_Y + 47))
        lanes();
    home_foot();
}

/* ================================================================ the step view ===== */
static void note_word(int note, char *b) /* a key by name: "D4", "F#3" */
{
    char o[4];
    b[0] = 0;
    text_cat(b, NOTE_NAME[((note % 12) + 12) % 12]);
    int_text(note / 12 - 1, o);
    text_cat(b, o);
}
static void seq_whose(int t, int32_t y)  /* a line: the track and whose it is ("TRACK 3: YOURS"), its length at the right */
{
    const dabke_status_t *s = dabke_status();
    char b[48];
    int rec = s->rec_track == t;
    trk_name(t, b);
    text_cat(b, rec ? (s->rec_waiting ? ": A KEY STARTS IT" : ": RECORDING") : trk_his(t) ? ": YOURS" : trk_rhythm(t) ? ": THE RHYTHM'S" : ": EMPTY");
    if ((set.mute >> t & 1u) && !rec)
        text_cat(b, ", MUTED");
    TXF(b, 14, y, 1, rec ? K_RED : K_INK, 0, 150);
    int_text(trk_nsteps(t), b);
    text_cat(b, " STEPS");
    TX(b, 226, y, 0, K_FAINT, 2, 0);
}
static void seq_locks(int t, int32_t y)  /* its locks counted, at the right: "3 LOCKS" */
{
    char b[16];
    int nl = lk_count(t, -1);
    if (!nl)
        return;
    int_text(nl, b);
    text_cat(b, nl == 1 ? " LOCK" : " LOCKS");
    TX(b, 226, y, 0, K_LK, 2, 0);
}
static void seq_page(int t, int32_t y)   /* the page of steps shown: "PAGE 1 OF 2" */
{
    char b[24] = "PAGE ", x[8];
    int_text(ui.spage + 1, x);
    text_cat(b, x);
    text_cat(b, " OF ");
    int_text(trk_pages(t), x);
    text_cat(b, x);
    TX(b, 14, y, 0, K_FAINT, 0, 120);
}
static void seq_drums(int t, int from, int cols, float cw)   /* a drum sound's steps: its drums lane by lane */
{
    int f = set.tsnd[t][0], gn = dabke_sound_drums(f), nl = gn, j, w, st, his = trk_his(t), n = trk_nsteps(t);
    int32_t rh, top;
    if (!his && t >= PART_KICK) {        /* (the rhythm's: the lanes of its track's own drums) */
        for (nl = 0, j = 0; j < DAB_NDRUM; j++)
            nl += HOME_DRUM[j] == t;
    } else if (his) {                    /* (his: any lane he has something in) */
        for (j = gn; j < DAB_TLANE; j++)
            for (st = 0; st < n; st++)
                if (step_lane(t, st, j))
                    nl = j + 1;
    }
    if (nl < 1)
        nl = 1;
    rh = SQ_ROLL_H / nl > 14 ? 14 : SQ_ROLL_H / nl;
    top = SQ_ROLL_Y + (SQ_ROLL_H - nl * rh) / 2;
    for (j = 0; j < nl; j++) {
        int32_t y = top + j * rh, bh = rh - 3;
        TX(DRUM_SHORT[dabke_sound_drum(f, j)], 13, y + bh / 2 + 4, 0, K_GR, 0, 20);
        for (w = 0; w < cols; w++) {
            int v = step_lane(t, from + w, j);
            int32_t bx = ri((float)SQ_X + (float)w * cw), bw = ri((float)SQ_X + (float)(w + 1) * cw) - bx - 2;
            int32_t by = v == 1 ? y + bh / 2 : y, hh = v == 1 ? bh - bh / 2 : bh;   /* (soft: half as tall) */
            if (!v)
                box(bx + bw / 2, y + bh / 2, 1, 1, PN_DIM);
            else if (his)
                pbox(bx, by, bw, hh, PN_LIVE);
            else
                pframe(bx, by, bw, hh, K_GR);
            if (v == 3)                  /* an accent: a mark in it */
                box(bx + 2, y + 1, bw - 4, 2, his ? K_PAPER : K_GR);
        }
    }
}
static void seq_roll(int t, int from, int cols, float cw)   /* a note sound's steps: its notes by pitch, as long as they
                                                              * sound; the automatic bass line and backing: the rhythm's */
{
    const dabke_status_t *s = dabke_status();
    int q = step_q(), i, lo = 128, hi = -1, w, t0 = from * q, t1 = (from + cols) * q, span, base, div = gview_div < 1 ? 4 : gview_div;
    const int H = SQ_ROLL_H - 10;        /* (the notes' heights: from the lowest up to the highest) */
    char b[8];
    for (w = 0; w < cols; w++)           /* (the steps' places: a dot each, the beats' darker) */
        box(ri((float)SQ_X + ((float)w + 0.5f) * cw) - 1, SQ_ROLL_Y + SQ_ROLL_H - 2, 1, 1, w % div == 0 ? K_GR : PN_DIM);
    if (!trk_loop(t)) {
        for (w = 0; w < cols; w++)
            if (step_trig(t, from + w) == 2) {
                int32_t bx = ri((float)SQ_X + (float)w * cw), bw = ri((float)SQ_X + (float)(w + 1) * cw) - bx - 2;
                pframe(bx, SQ_ROLL_Y + SQ_ROLL_H / 2 - 3, bw, 6, K_GR);
            }
        return;
    }
    for (i = 0; i < s->trk_n[t] && i < DAB_LOOP_N; i++) {
        int p = (int)DAB_EV_POS(s->trk_ev[t][i]), nt = (int)DAB_EV_NOTE(s->trk_ev[t][i]);
        if (p >= t0 && p < t1 && p < s->trk_len[t]) {
            if (nt < lo)
                lo = nt;
            if (nt > hi)
                hi = nt;
        }
    }
    if (hi < 0)
        return;
    span = hi - lo < 7 ? 7 : hi - lo;    /* (the page's notes from its lowest up to its highest; a few: in the middle) */
    base = SQ_ROLL_Y + 2 + (span < hi - lo + 1 ? H : H / 2 + ((hi - lo) * H / span) / 2);
    for (i = 0; i < s->trk_n[t] && i < DAB_LOOP_N; i++) {
        uint32_t e = s->trk_ev[t][i];
        int p = (int)DAB_EV_POS(e), nt = (int)DAB_EV_NOTE(e);
        float x, wd;
        if (p < t0 || p >= t1 || p >= s->trk_len[t])
            continue;
        x = (float)SQ_X + (float)(p - t0) * cw / (float)q;
        wd = (float)DAB_EV_LEN(e) * cw / (float)q - 2.0f;
        if (x + wd > (float)(SQ_X + SQ_W))
            wd = (float)(SQ_X + SQ_W) - x;
        boxf(x, (float)(base - ((nt - lo) * H) / span), wd < 3.0f ? 3.0f : wd, 4.0f, PN_LIVE);
    }
    note_word(hi, b);                    /* (the highest and the lowest note on the page, by name, each at its height; in
                                          * the face that has a small b for a flat) */
    TX(b, 13, base - ((hi - lo) * H) / span + 7, 1, K_GR, 0, 21);
    if (lo != hi) {
        note_word(lo, b);
        TX(b, 13, base + 7, 1, K_GR, 0, 21);
    }
}
static void seq_row(int t, int from, int cols, float cw, int32_t ry)   /* the steps as the white keys have them, the
                                                                        * row's top at ry */
{
    const dabke_status_t *s = dabke_status();
    int w, P = page_steps(), cur = s->running ? (int)s->tstep[t] : -1, play = cur >= 0 ? cur - from : -1;
    int div = gview_div < 1 ? 4 : gview_div, pages = trk_pages(t), i;
    for (i = 0; i < pages && i < 8 && pages > 1; i++)   /* its bars, at the left: the one shown ink, the one playing red */
        box(14 + (i & 1) * 7, ry + 1 + (i >> 1) * 6, 5, 4, i == ui.spage ? K_INK : cur >= 0 && cur / P == i ? PN_LIVE : PN_DIM);
    for (w = 0; w < cols; w++) {
        int st = from + w, tr = step_trig(t, st);
        int32_t bx = ri((float)SQ_X + (float)w * cw), bw = ri((float)SQ_X + (float)(w + 1) * cw) - bx - 2;
        char n[4];
        if (tr == 1)
            pbox(bx, ry, bw, SQ_ROW_H, PN_LIVE);
        else if (tr == 2)
            pframe(bx, ry, bw, SQ_ROW_H, K_GR);
        else
            pdots(bx, ry, bw, SQ_ROW_H, PN_DIM);
        if (ui.m_lock >> w & 1u)         /* a lock on it: a padlock (on an empty step: a lock only step) */
            plock(bx + (bw - 5) / 2, ry + 3, K_LK);
        if (w % div == 0) {              /* the beats' steps numbered */
            int_text(st + 1, n);
            TX(n, bx + bw / 2, ry + SQ_ROW_H - 4, 0, tr == 1 ? K_PAPER : K_GR, 1, 0);
        }
        if (step_held() && ui.skey == w)     /* the step held */
            pframe(bx - 1, ry - 1, bw + 2, SQ_ROW_H + 2, K_INK);
        if (w == play)                   /* the step playing */
            box(bx, ry + SQ_ROW_H + 2, bw, 3, K_INK);
    }
}
static void seq_info(int t)              /* the lines of words: the track and whose it is, its length; what a tap adds, its
                                          * locks counted; the page. A step held (its locks are named in the top line):
                                          * the rest of them, when one line did not hold them */
{
    char a[72], b2[72], w[8];
    int i, n;
    seq_whose(t, SQ_INFO_Y);
    if (step_held()) {
        step_words(t, ui.sstep, a, b2);
        if (b2[0])
            TXF(b2, 14, SQ_INFO_Y + 14, 0, K_RED, 0, 212);
        else
            seq_locks(t, SQ_INFO_Y + 14);
    } else {
        a[0] = 0;
        text_cat(a, "A TAP ADDS ");
        if (trk_drum(t)) {
            int f = set.tsnd[t][0];
            n = dabke_sound_drums(f);
            text_cat(a, DRUM_NAME[dabke_sound_drum(f, ui.tdrum[t] % (n ? n : 1))]);
        } else {
            uint8_t notes[4];
            n = chord_for(t, notes);
            for (i = 0; i < n; i++) {
                note_word(notes[i], w);
                text_cat(a, i ? " " : "");
                text_cat(a, w);
            }
        }
        TX(a, 14, SQ_INFO_Y + 15, 1, K_GR, 0, 150);
        seq_locks(t, SQ_INFO_Y + 15);
    }
    seq_page(t, SQ_INFO_Y + 28);
}
static void steps_bottom(void)           /* step mode on a knob page: the picked track's steps of the page shown, as the
                                          * white keys have them (locks marked, the step playing, the step held), under
                                          * the knobs' pictures; the track and whose it is, its length; the page, its locks */
{
    int t = picked(), P = page_steps(), n = trk_nsteps(t), from = ui.spage * P, cols = n - from < P ? n - from : P;
    if (cols < 0)
        cols = 0;
    if (gview_of != ui.r0)
        grid_view();
    if (in_strip(SQ_ROW_Y - 2, SQ_ROW_Y + SQ_ROW_H + 6))
        seq_row(t, from, cols, (float)SQ_W / (float)P, SQ_ROW_Y);
    seq_whose(t, SQ_INFO_Y);
    seq_page(t, SQ_INFO_Y + 16);
    seq_locks(t, SQ_INFO_Y + 16);
}
static uint32_t seq_sig(void)            /* its moving parts: the step playing, the locks holding the strip's knobs */
{
    const dabke_status_t *s = dabke_status();
    int t = picked(), k;
    uint32_t h = s->running ? 1u + s->tstep[t] : 0u;
    for (k = 0; k < 4; k++)
        h = h * 131u + s->lk_at[strip_id(STRIP_ROW[k]) % DAB_NLK];
    return h;
}
static void seq_view(void)               /* (inside the step view's frame) */
{
    int t = picked(), P = page_steps(), n = trk_nsteps(t), from = ui.spage * P, cols = n - from < P ? n - from : P;
    float cw = (float)SQ_W / (float)P;
    if (cols < 0)
        cols = 0;
    if (gview_of != ui.r0)
        grid_view();
    if (in_strip(SQ_ROLL_Y - 2, SQ_ROLL_Y + SQ_ROLL_H + 2)) {
        if (trk_drum(t))
            seq_drums(t, from, cols, cw);
        else
            seq_roll(t, from, cols, cw);
    }
    if (in_strip(SQ_ROW_Y - 2, SQ_ROW_Y + SQ_ROW_H + 6))
        seq_row(t, from, cols, cw, SQ_ROW_Y);
    if (in_strip(SQ_INFO_Y - 14, SQ_INFO_Y + 32))
        seq_info(t);
}
