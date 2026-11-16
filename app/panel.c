/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 STUDIOMORPHIK */
/* DABKE's home page under the singer, and the step view. Included by app.c after scene.c (its drawing helpers, its ui,
 * the scenes' trap).
 *
 * The home page: a top line (the picked track's sound, the maqam on its root, the tempo's metronome), the singer
 * (scene.c), then the eight tracks as lanes of steps, all at once: each lane its number (yellow: the picked one; red:
 * recording), a small bar by it when what it plays is his, its steps over its whole length (a block: something of his
 * begins there; a low grey one: the rhythm's; a blue strip: a lock; amber: the step playing), bright when it is picked,
 * and its steps counted at the right. A bottom line: the rhythm and the switches that are not tracks.
 *
 * The step view (SEQ): the picked track alone, large. Its top line: "STEPS" and the page shown. What its steps play (a
 * drum sound: its drums lane by lane; a note sound: its notes by pitch, as long as they sound). The row of steps as
 * the white keys have them: filled, outlined (the rhythm's), empty; a blue padlock on a locked one; the step playing
 * underlined; the held one framed. Two lines of words: the track and whose it is, its length; what a tap adds, its
 * locks counted. The picked track's strip on K1 to K4, small. On a knob page in step mode the same row of steps and
 * words sit under the knobs.
 *
 * Drawn in bands, each redrawn alone when what it shows moves. */

static void knob_boxes(const uint8_t *row, int with_icons, int32_t top);   /* (app.c, below this file) */
static void plock(int32_t x, int32_t y, uint16_t c);

#define PN_Y 144                         /* the lanes: rows 144..221 */
#define PN_FOOT_Y 222                    /* the bottom line: 222..239 */
#define PN_DIM RGB(66, 60, 58)
#define PN_LIVE C_YEL                    /* what is picked, what is his in the step view: the portrait's yellow */
#define SQ_X 30                          /* the step view: its steps from here to the right edge */
#define SQ_W 208
#define SQ_ROW_Y 120                     /* its row of steps (the white keys) */
#define SQ_ROW_H 26
#define SQ_KNOB_Y 192                    /* its four knobs, small */
#define SQ_BOT_Y 164                     /* the same row of steps at the bottom of a knob page, in step mode */

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

/* a word in a box: lit (white, the word black), or not (a dotted box, the word grey). Returns where it ends */
static int32_t ptoggle(int32_t x, int32_t y, const char *s, int on)
{
    int32_t w = text_w(&FONT_T, s) + 7;
    if (on)
        pbox(x, y, w, 13, K_WH);
    else
        pdots(x, y, w, 13, PN_DIM);
    T(s, x + 4, y + 11, 9, on ? K_BLACK : K_GR, 0, 0);
    return x + w;
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

/* ---- the home page's top line: the picked track's sound, the maqam on its root, the tempo (its metronome ticks with
 * the beat) ---- */
static void panel_head(void)
{
    const dabke_status_t *s = dabke_status();
    const int32_t y0 = 8, yb = y0 + 12;
    char b[40], t[8];
    int32_t x, w, flat = set.ntune[set.ch[C_ROOT] % 12] < 0, ix = set.track ? 14 : 0;
    if (!in_strip(0, 32))
        return;
    b[0] = 0;                            /* the picked track's sound, black on its box (all: the whole sound) */
    if (set.track) {                     /* (its own name: the sign in front says its family; a lead sound's: its own) */
        int f = set.tsnd[set.track - 1][0];
        if (f == SF_LEAD)
            lead_name(set.track - 1, b);
        else
            text_cat(b, FAM_LIST[f][set.tsnd[set.track - 1][1] % fam_size(f)]);
    } else
        sound_name(b);
    w = text_w(&FONT_XS, b);
    if (w > 80 - ix)
        w = 80 - ix;
    pbox(2, y0, w + 8 + ix, 15, K_WH);
    if (set.track)
        track_icon(set.tsnd[set.track - 1][0], 6, y0 + 2, K_BLACK);
    T(b, 6 + ix, yb, 11, K_BLACK, 0, 80 - ix);
    x = 2 + w + 8 + ix + 6;              /* the maqam, its root in a box */
    maqam_name(b, 0);
    x = T(b, x, yb, 11, K_WH, 0, 194 - x - 24);
    w = text_w(&FONT_XS, NOTE_NAME[set.ch[C_ROOT] % 12]) + (flat ? 8 : 0) + 8;
    pframe(x + 4, y0, w, 15, K_WH);
    {
        int32_t e = T(NOTE_NAME[set.ch[C_ROOT] % 12], x + 8, yb, 11, K_WH, 0, 0);
        if (flat)
            halfflat(e + 3, yb - 1, 10, K_WH);
    }
    if (!maqam_as_set() && x + w + 10 < 170)
        T("CHANGED", x + w + 10, yb - 1, 9, K_AMB, 0, 194 - (x + w + 10));
    {                                    /* the tempo: a metronome, its arm and weight left and right on the beats */
        float mx = 207.0f, by = (float)yb - 1.0f, sw = s->running ? ((ui.beat_count & 1u) ? 5.0f : -5.0f) : 4.0f;
        uint16_t ac = s->running ? PN_LIVE : K_WH;
        pframe(197, y0, 41, 15, K_WH);
        box(ri(mx) - 5, ri(by), 11, 1, K_WH);    /* its body: a box narrowing up */
        seg(mx - 5.0f, by, mx - 2.0f, by - 8.0f, K_WH, 1);
        seg(mx + 5.0f, by, mx + 2.0f, by - 8.0f, K_WH, 1);
        box(ri(mx) - 2, ri(by) - 8, 5, 1, K_WH);
        seg(mx, by - 1.0f, mx + sw, by - 10.0f, ac, 1);   /* the arm out of it, its weight */
        box(ri(mx + sw * 0.6f) - 1, ri(by - 7.0f), 3, 2, ac);
        int_text(set.par[P_TEMPO], t);
        T(t, 234, yb, 11, K_WH, 2, 0);
    }
}

/* ---- the lanes: the eight tracks, each the bar of its steps that is playing ---- */
static void lane(int t, int32_t y)       /* a track's lane: its number (yellow: picked; red: recording), a small bar by
                                          * it when what it plays is his, its steps over its whole length (a block: his;
                                          * a low grey one: the rhythm's; a blue strip on top: a lock; amber: the step
                                          * playing), bright when it is picked (ALL: every lane), and its steps counted
                                          * at the right */
{
    const dabke_status_t *s = dabke_status();
    int P = page_steps(), n = trk_nsteps(t), pk = set.track == t + 1, his = trk_his(t), q = step_q();
    int rec = s->rec_track == t, mute = (int)MUTED(t), cur = s->running ? (int)s->tstep[t] : -1, bright = pk || !set.track;
    int div = gview_div < 1 ? 4 : gview_div, i;
    uint32_t has[11] = {0}, lk[11] = {0};
    uint16_t ink = mute ? PN_DIM : rec ? K_RED : bright ? K_WH : mixc(K_WH, K_BLACK, 0.4f);
    uint16_t soft = mute ? PN_DIM : bright ? K_GR : mixc(K_GR, K_BLACK, 0.35f);
    char num[8] = {(char)('1' + t), 0};
    if (n > 352)
        n = 352;
    if (rec)
        pbox(1, y, 11, 9, K_RED);
    else if (pk)
        pbox(1, y, 11, 9, PN_LIVE);
    T(num, 4, y + 8, 9, rec ? K_WH : pk ? K_BLACK : mute ? PN_DIM : K_GR, 0, 0);
    if (his)                             /* what it plays is his: a small bar by its number */
        box(13, y + 1, 2, 7, rec ? K_RED : PN_LIVE);
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
        int32_t x0 = 17 + (i * 202) / n, cw = 17 + ((i + 1) * 202) / n - x0 - (n <= 64 ? 1 : 0);
        if (cw < 1)
            cw = 1;
        if (i && i % P == 0)             /* (a bar begins) */
            box(x0 - 1, y + 1, 1, 7, K_DIM);
        if (tr == 1)
            box(x0, y + 1, cw, 7, i == cur ? K_AMB : ink);
        else if (tr == 2)
            box(x0, y + 3, cw, 3, i == cur ? K_AMB : soft);
        else if (i == cur)
            box(x0, y + 4, cw, 2, K_AMB);
        else if (n <= 32 || i % div == 0)
            box(x0 + cw / 2, y + 4, 1, 1, i % div == 0 ? K_GR : PN_DIM);
        if (lk[i >> 5] >> (i & 31) & 1u)     /* a lock on it */
            box(x0, y + 1, cw, 2, K_LK);
    }
    int_text(trk_nsteps(t), num);        /* its length */
    T(num, 239, y + 8, 9, pk ? K_WH : K_GR, 2, 0);
}
static void lanes(void)
{
    int t;
    for (t = 0; t < DAB_NTRK; t++)
        lane(t, PN_Y + 1 + (t * 19) / 2);
}

/* ---- the bottom line: the rhythm (or the track recording); the switches that are not tracks ---- */
static void panel_foot(void)
{
    const dabke_status_t *s = dabke_status();
    char b[48], t[24];
    int32_t x, w;
    int o = set.ch[C_ORN];
    const char *on = o && o != ORN_AUTO ? N_ORN[o] : "AUTO";
    const char *pm = N_PLAY[set.ch[C_PLAY] % 3];
    int rec = s->rec_track >= 0, armed = rec && s->rec_waiting;
    if (!in_strip(PN_FOOT_Y, 240))
        return;
    box(0, PN_FOOT_Y, 240, 1, PN_DIM);
    w = text_w(&FONT_T, pm) + text_w(&FONT_T, on) + text_w(&FONT_T, "ARP") + text_w(&FONT_T, "SYNC") + 4 * 7 + 3 * 3 +
        (rec ? text_w(&FONT_T, "REC") + 10 : 0) + (set.ch[C_ARPLATCH] ? 7 : 0) + (s->hold ? 15 : 0);
    x = 238 - w;
    if (s->hold) {                       /* HOLD: keys held down under a bar, one a held note (three at most) */
        int i, n = s->held_n > 3 ? 3 : s->held_n;
        box(x, 225, 11, 2, PN_LIVE);
        for (i = 0; i < 3; i++)
            box(x + i * 4, i < n ? 229 : 228, 3, i < n ? 9 : 10, i < n ? PN_LIVE : PN_DIM);
        x += 15;
    }
    {                                    /* how the lead plays: a mode, always one of three (framed, not a switch) */
        int32_t pw = text_w(&FONT_T, pm) + 7;
        pframe(x, 225, pw, 13, K_WH);
        T(pm, x + 4, 236, 9, K_WH, 0, 0);
        x += pw + 3;
    }
    x = ptoggle(x, 225, on, o != 0) + 3;
    {                                    /* the arpeggio: lit when on; latched, a small padlock in it */
        int32_t ax = ptoggle(x, 225, "ARP", set.arp);
        if (set.ch[C_ARPLATCH]) {
            uint16_t lc = set.arp ? K_BLACK : K_GR;
            pbox(ax - 1, 225, 8, 13, set.arp ? K_WH : K_BLACK);
            box(ax, 230, 5, 5, lc);
            box(ax, 228, 1, 2, lc);
            box(ax + 4, 228, 1, 2, lc);
            box(ax + 1, 227, 3, 1, lc);
            ax += 7;
        }
        x = ax + 3;
    }
    if (rec) {                           /* recording: red; waiting for its first key: framed red */
        int32_t bw = text_w(&FONT_T, "REC") + 7;
        if (armed)
            pframe(x, 225, bw, 13, K_RED);
        else
            pbox(x, 225, bw, 13, K_RED);
        T("REC", x + 4, 236, 9, armed ? K_RED : K_WH, 0, 0);
        x += bw + 3;
    }
    ptoggle(x, 225, "SYNC", set.ch[C_SYNC]);
    x = 238 - w;
    if (rec) {                           /* recording: which track, said here, in red */
        int rt = s->rec_track & 7;
        b[0] = 0;
        trk_name(rt, b);
        if (armed)
            text_cat(b, ": PLAY TO START");
        else if (SF_DRUM(set.tsnd[rt][0]))
            text_cat(b, ": RECORDING");
        else if (s->overdub)
            text_cat(b, ": ADDING");
        else {
            text_cat(b, ": BAR ");
            int_text(s->trk_bars[rt], t);
            text_cat(b, t);
        }
        T(b, 3, 236, 9, K_RED, 0, x - 7);
        return;
    }
    ellipse(8.0f, 228.0f, 5.0f, 1.6f, K_WH);   /* the rhythm: a small darbuka, its name (and the one added) */
    trap(229.0f, 233.0f, 3.0f, 13.0f, 6.0f, 10.0f, K_GR, 0, -1);
    trap(233.0f, 237.0f, 6.0f, 10.0f, 4.0f, 12.0f, K_GR, 0, -1);
    rhythm_name_copy(b, ui.r0);
    if (ui.r1 >= 0) {
        text_cat(b, " + ");
        rhythm_name_copy(t, ui.r1);
        text_cat(b, t);
    }
    T(b, 17, 236, 11, s->running ? K_WH : K_GR, 0, x - 21);
}

/* the lanes' moving part (the steps playing) and the top line's (the metronome): a change, and each is drawn again */
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
    if (in_strip(PN_Y, PN_FOOT_Y))
        lanes();
    panel_foot();
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
static void seq_head(void)               /* its top line: "STEPS", and the page shown ("PAGE 1 OF 2") */
{
    char b[24] = "PAGE ", n[8];
    if (!in_strip(0, 32))
        return;
    T("STEPS", 8, 22, 16, K_WH, 0, 120);
    int_text(ui.spage + 1, n);
    text_cat(b, n);
    text_cat(b, " OF ");
    int_text(trk_pages(picked()), n);
    text_cat(b, n);
    T(b, 232, 21, 11, K_GR, 2, 0);
}
static void seq_whose(int t, int32_t y)  /* a line: the track and whose it is ("TRACK 3: YOURS"), its length at the right */
{
    const dabke_status_t *s = dabke_status();
    char b[48];
    int rec = s->rec_track == t;
    trk_name(t, b);
    text_cat(b, rec ? (s->rec_waiting ? ": PLAY TO START" : ": RECORDING") : trk_his(t) ? ": YOURS" : trk_rhythm(t) ? ": THE RHYTHM'S" : ": EMPTY");
    if ((set.mute >> t & 1u) && !rec)
        text_cat(b, ", MUTED");
    T(b, 4, y, 11, rec ? K_RED : K_WH, 0, 160);
    int_text(trk_nsteps(t), b);
    text_cat(b, " STEPS");
    T(b, 236, y, 11, K_GR, 2, 0);
}
static void seq_locks(int t, int32_t y)  /* its locks counted, at the right: "3 LOCKS" */
{
    char b[16];
    int nl = lk_count(t, -1);
    if (!nl)
        return;
    int_text(nl, b);
    text_cat(b, nl == 1 ? " LOCK" : " LOCKS");
    T(b, 236, y, 11, K_LK, 2, 0);
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
    rh = 80 / nl > 20 ? 20 : 80 / nl;
    top = 36 + (80 - nl * rh) / 2;
    for (j = 0; j < nl; j++) {
        int32_t y = top + j * rh, bh = rh - 3;
        T(DRUM_SHORT[dabke_sound_drum(f, j)], 3, y + bh / 2 + 4, 9, K_GR, 0, 25);
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
                box(bx + 2, y + 2, bw - 4, 2, his ? K_BLACK : K_GR);
        }
    }
}
static void seq_roll(int t, int from, int cols, float cw)   /* a note sound's steps: its notes by pitch, as long as they
                                                              * sound; the automatic bass line and backing: the rhythm's */
{
    const dabke_status_t *s = dabke_status();
    int q = step_q(), i, lo = 128, hi = -1, w, t0 = from * q, t1 = (from + cols) * q, span, base, div = gview_div < 1 ? 4 : gview_div;
    char b[8];
    for (w = 0; w < cols; w++)           /* (the steps' places: a dot each, the beats' brighter) */
        box(ri((float)SQ_X + ((float)w + 0.5f) * cw) - 1, 114, 1, 1, w % div == 0 ? K_GR : PN_DIM);
    if (!trk_loop(t)) {
        for (w = 0; w < cols; w++)
            if (step_trig(t, from + w) == 2) {
                int32_t bx = ri((float)SQ_X + (float)w * cw), bw = ri((float)SQ_X + (float)(w + 1) * cw) - bx - 2;
                pframe(bx, 72, bw, 6, K_GR);
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
    base = 74 + (span < hi - lo + 1 ? 34 : ((hi - lo) * 68 / span) / 2);
    for (i = 0; i < s->trk_n[t] && i < DAB_LOOP_N; i++) {
        uint32_t e = s->trk_ev[t][i];
        int p = (int)DAB_EV_POS(e), nt = (int)DAB_EV_NOTE(e);
        float x, wd;
        if (p < t0 || p >= t1 || p >= s->trk_len[t])
            continue;
        x = (float)SQ_X + (float)(p - t0) * cw / (float)q;
        wd = (float)DAB_EV_LEN(e) * cw / (float)q - 2.0f;
        if (x + wd > 238.0f)
            wd = 238.0f - x;
        boxf(x, (float)(base - ((nt - lo) * 68) / span), wd < 3.0f ? 3.0f : wd, 4.0f, PN_LIVE);
    }
    note_word(hi, b);                    /* (the highest and the lowest note on the page, by name, each at its height;
                                          * in the size that has a small b for a flat) */
    T(b, 2, base - ((hi - lo) * 68) / span + 6, 11, K_GR, 0, 27);
    if (lo != hi) {
        note_word(lo, b);
        T(b, 2, base + 6, 11, K_GR, 0, 27);
    }
}
static void seq_row(int t, int from, int cols, float cw, int32_t ry)   /* the steps as the white keys have them, the
                                                                        * row's top at ry */
{
    const dabke_status_t *s = dabke_status();
    int w, P = page_steps(), cur = s->running ? (int)s->tstep[t] : -1, play = cur >= 0 ? cur - from : -1;
    int div = gview_div < 1 ? 4 : gview_div, pages = trk_pages(t), i;
    for (i = 0; i < pages && i < 8 && pages > 1; i++)   /* its bars, at the left: the one shown white, the one playing
                                                         * yellow */
        box(4 + (i & 1) * 7, ry + 1 + (i >> 1) * 6, 5, 4, i == ui.spage ? K_WH : cur >= 0 && cur / P == i ? PN_LIVE : PN_DIM);
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
            T(n, bx + bw / 2, ry + 22, 9, tr == 1 ? K_BLACK : K_GR, 1, 0);
        }
        if (step_held() && ui.skey == w)     /* the step held */
            pframe(bx - 1, ry - 1, bw + 2, SQ_ROW_H + 2, K_WH);
        if (w == play)                   /* the step playing */
            box(bx, ry + SQ_ROW_H + 2, bw, 3, K_WH);
    }
}
static void seq_info(int t)              /* two lines of words: the track and whose it is, its length; what a tap adds,
                                          * its locks counted. A step held (its locks are named in the top line): the
                                          * rest of them, when one line did not hold them */
{
    char a[72], b2[72], w[8];
    int i, n;
    seq_whose(t, 167);
    if (step_held()) {
        step_words(t, ui.sstep, a, b2);
        if (b2[0])
            T(b2, 4, 183, 11, K_AMB, 0, 232);
        else
            seq_locks(t, 183);
        return;
    }
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
    T(a, 4, 183, 11, K_GR, 0, 170);
    seq_locks(t, 183);
}
static void steps_bottom(void)           /* step mode on a knob page: the picked track's steps of the page shown, as the
                                          * white keys have them (locks marked, the step playing, the step held), under
                                          * the knobs he turns; the track and whose it is, its length; the page, its locks */
{
    int t = picked(), P = page_steps(), n = trk_nsteps(t), from = ui.spage * P, cols = n - from < P ? n - from : P;
    char b[24] = "PAGE ", x[8];
    if (cols < 0)
        cols = 0;
    if (gview_of != ui.r0)
        grid_view();
    seq_row(t, from, cols, (float)SQ_W / (float)P, SQ_BOT_Y);
    seq_whose(t, 213);
    int_text(ui.spage + 1, x);
    text_cat(b, x);
    text_cat(b, " OF ");
    int_text(trk_pages(t), x);
    text_cat(b, x);
    T(b, 4, 231, 11, K_GR, 0, 120);
    seq_locks(t, 231);
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
static void seq_view(void)
{
    int t = picked(), P = page_steps(), n = trk_nsteps(t), from = ui.spage * P, cols = n - from < P ? n - from : P;
    float cw = (float)SQ_W / (float)P;
    if (cols < 0)
        cols = 0;
    if (gview_of != ui.r0)
        grid_view();
    if (in_strip(32, 118)) {
        if (trk_drum(t))
            seq_drums(t, from, cols, cw);
        else
            seq_roll(t, from, cols, cw);
    }
    if (in_strip(SQ_ROW_Y - 2, SQ_ROW_Y + SQ_ROW_H + 6))
        seq_row(t, from, cols, cw, SQ_ROW_Y);
    if (in_strip(152, 190))
        seq_info(t);
    if (in_strip(SQ_KNOB_Y, 240))
        knob_boxes(STRIP_ROW, 0, SQ_KNOB_Y + 1);
}
