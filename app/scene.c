/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 STUDIOMORPHIK */
/* DABKE's home scenes: a wedding singer drawn from shapes (a red and white keffiyeh with its black agal, dark glasses,
 * a moustache, a white robe), deadpan: he barely moves and claps on the beat while everything around him moves with
 * the sound. Nothing bends or warps: whole pieces move, appear or change (the hands, the head as one piece, a glint,
 * the tassels, the lights, the dancers' legs, the confetti, the reels' spokes). Four scenes, HOME SCENE on the
 * ornaments page: the portrait (first), the stage, the dancers, the cassette. Included by app.c (its drawing helpers
 * and its ui). */

#define C_SKIN RGB(196, 140, 98)
#define C_SKIN2 RGB(156, 100, 66)
#define C_KEFW RGB(246, 242, 234)
#define C_KEFR RGB(206, 30, 40)
#define C_AGAL RGB(14, 12, 14)
#define C_GLASS RGB(16, 16, 22)
#define C_RIM RGB(84, 84, 96)
#define C_ROBE RGB(238, 236, 228)
#define C_ROBE2 RGB(196, 192, 184)
#define C_MUST RGB(36, 24, 20)
#define C_NIGHT RGB(24, 14, 38)
#define C_NIGHT2 RGB(44, 22, 58)
#define C_CARPET RGB(142, 22, 34)
#define C_GOLD RGB(232, 182, 62)
#define C_YEL RGB(250, 214, 120)         /* the tape label's yellow, and a deeper one */
#define C_YEL2 RGB(240, 190, 86)
#define C_RED2 RGB(168, 22, 34)
#define C_RED3 RGB(150, 20, 30)

static float sc_u = 1.0f;                /* the pattern's scale while a figure is drawn */

/* the shemagh's pattern: white with a red lattice, a red band near its hem */
static uint16_t kef_px(int32_t x, int32_t y, int32_t hem)
{
    int32_t p = sc_u >= 2.2f ? 6 : sc_u >= 1.3f ? 4 : 3, a = (x + y + 1200) % p, b = (x - y + 1200) % p;
    if (hem >= 0 && y == hem - 2)        /* a red line along its hem */
        return C_KEFR;
    return (a == 0 || b == 0) ? C_KEFR : C_KEFW;
}
static void kef_row(int32_t y, float xl, float xr, int32_t hem)
{
    int32_t x, x0 = ri(xl), x1 = ri(xr);
    if (y < clip_y0 || y >= clip_y1)
        return;
    for (x = x0; x < x1; x++)
        px(x, y, kef_px(x, y, hem));
}
/* rows from y0 to y1, the left and right edges going from l0, r0 to l1, r1: solid, or the pattern (pat) */
static void trap(float y0, float y1, float l0, float r0, float l1, float r1, uint16_t c, int pat, int32_t hem)
{
    int32_t y, ya = ri(y0), yb = ri(y1);
    for (y = ya; y < yb; y++) {
        float f = yb > ya ? (float)(y - ya) / (float)(yb - ya) : 0.0f, l = l0 + (l1 - l0) * f, r = r0 + (r1 - r0) * f;
        if (pat)
            kef_row(y, l, r, hem);
        else
            box(ri(l), y, ri(r) - ri(l), 1, c);
    }
}
static void kef_dome(float cx, float cy, float rx, float ry)   /* the upper half of an ellipse, patterned */
{
    int32_t y, y0 = ri(cy - ry);
    for (y = y0; y < ri(cy); y++) {
        float dy = ((float)y + 0.5f - cy) / ry, w2 = 1.0f - dy * dy;
        if (w2 > 0.0f)
            kef_row(y, cx - rx * fm_sqrtf(w2), cx + rx * fm_sqrtf(w2), -1);
    }
}
static void rrect(float x, float y, float w, float h, float r, uint16_t c)   /* a rounded box */
{
    boxf(x + r, y, w - 2.0f * r, h, c);
    boxf(x, y + r, w, h - 2.0f * r, c);
    disc(x + r, y + r, r, c);
    disc(x + w - r, y + r, r, c);
    disc(x + r, y + h - r, r, c);
    disc(x + w - r, y + h - r, r, c);
}

/* how he stands: still and deadpan (all 0), or a moment of a dance. Every value moves a whole piece. */
typedef struct {
    float bx, by;                        /* the whole of him: across, up (pixels; by < 0 a hop) */
    float hdx, hdy;                      /* his head on his shoulders: a slide across, a bob (units) */
    float shl, shr;                      /* each shoulder up (units) */
    int arms;                            /* 0 at his chest (the clap), 1 both up, 2 the right up, 3 the left up, 4 out */
    int kerchief;                        /* the handkerchief in a raised hand: where it is in its turn 0..3, -1 none */
    float must, brow;                    /* the moustache up, the eyebrows up (units) */
    int feet;                            /* 0 both down, 1 the left kicked out, 2 the right, 3 both up (a hop) */
} pose_t;
static const pose_t STILL;

/* the singer. (hx, hy) the middle of his face, u pixels a unit (u 1: the face 22 high), sc_wx times wider than tall.
 * clap: 0 the hands apart, 1 together; nod: the head down this many pixels; glint: 0..1 across the lens, < 0 none;
 * sing: 0 the mouth shut .. 1 open; tassel: -1, 0, 1; brow: units the eyebrows go up; hands_y: where the hands are (0:
 * at his chest; > 0 they come up from below, no arms); ps: his pose */
static float sc_wx = 1.0f;               /* his width over his height (the portrait: a little wider) */
static uint16_t sc_clapc = K_WH;         /* the clap's strokes */
static float sc_hem;                     /* > 0: his robe ends there and his feet show under it */
static void singer(float hx, float hy, float u, float clap, float nod, float glint, float sing, int tassel, float brow,
                   float hands_y, const pose_t *ps)
{
    float v = u * sc_wx;                     /* a unit across */
    float bot = sc_hem > 0.0f ? sc_hem : (float)clip_y1, hy2, gap, lhx, lhy, rhx, rhy, hh, h, sh, fl;
    int k, arms = ps->arms, below = hands_y > 0.0f;
    hx += ps->bx;
    hy += ps->by;
    if (sc_hem > 0.0f)
        bot += ps->by;
    hh = hx + ps->hdx * v;                   /* the head's middle */
    h = hy + nod + ps->hdy * u;
    sh = hy + 15.0f * u;
    fl = bot - sh > 4.0f ? 7.0f * v / (bot - sh) : 0.25f;   /* how the robe widens down (as it always did) */
    if (sc_hem <= 0.0f)
        bot += 40.0f * u;                    /* (a shoulder up: its half reaches below the scene still) */
    hy2 = below ? hands_y + ps->by : hy + 27.0f * u;
    gap = 3.6f * v + (1.0f - clap) * 12.0f * v;
    lhx = hx - gap; lhy = hy2; rhx = hx + gap; rhy = hy2;   /* the hands: where the pose puts them */
    if (arms == 1 || arms == 3) { lhx = hx - 21.0f * v; lhy = hy - (below ? 2.0f : 8.0f) * u - ps->shl * u; }
    if (arms == 1 || arms == 2) { rhx = hx + 21.0f * v; rhy = hy - (below ? 2.0f : 8.0f) * u - ps->shr * u; }
    if (arms == 2) { lhx = hx - 5.0f * v; lhy = below ? hy2 : hy + 24.0f * u; }
    if (arms == 3) { rhx = hx + 5.0f * v; rhy = below ? hy2 : hy + 24.0f * u; }
    if (arms == 4) { lhx = hx - 28.0f * v; rhx = hx + 28.0f * v; lhy = rhy = sh - 1.0f * u; }
    if (below && arms == 0) {                /* (the portrait: the hands ride on their shoulders) */
        lhy -= ps->shl * u;
        rhy -= ps->shr * u;
    }
    sc_u = u;
    /* the keffiyeh's back (with his head), then the robe over it: each half its own piece, its shoulder up or down */
    trap(h - nod + 2.0f * u, sh + 6.0f * u, hh - 14.0f * v, hh + 14.0f * v, hh - 21.0f * v, hh + 21.0f * v, 0, 1, -1);
    trap(sh - ps->shl * u, bot, hx - 20.0f * v, hx + 0.5f, hx - 20.0f * v - (bot - sh + ps->shl * u) * fl, hx + 0.5f, C_ROBE, 0, -1);
    trap(sh - ps->shr * u, bot, hx - 0.5f, hx + 20.0f * v, hx - 0.5f, hx + 20.0f * v + (bot - sh + ps->shr * u) * fl, C_ROBE, 0, -1);
    trap(sh + 1.0f * u, bot, hx - 1.5f * v, hx + 1.5f * v, hx - 1.0f * v, hx + 1.0f * v, C_ROBE2, 0, -1);   /* its placket */
    trap(sh - 1.0f * u, sh + 6.0f * u, hx - 5.0f * v, hx + 5.0f * v, hx - 0.5f * v, hx + 0.5f * v, C_SKIN2, 0, -1);   /* the neck */
    if (sc_hem > 0.0f) {                     /* his feet under the hem: down, one kicked out, or both up */
        float fy = bot + 1.0f * u, kl = ps->feet == 1 ? 1.0f : 0.0f, kr = ps->feet == 2 ? 1.0f : 0.0f;
        float up = ps->feet == 3 ? 1.5f * u : 0.0f;
        rrect(hx - 9.0f * v - kl * 7.0f * v, fy - kl * 3.0f * u - up, 7.0f * v, 2.6f * u, 1.0f * u, C_MUST);
        rrect(hx + 2.0f * v + kr * 7.0f * v, fy - kr * 3.0f * u - up, 7.0f * v, 2.6f * u, 1.0f * u, C_MUST);
    }
    /* the arms: sleeves from the shoulders to the hands (straight pieces), each its own; from below in the portrait
     * only when a hand is up or out */
    for (k = 0; k < 2; k++) {
        float ax = k ? hx + 18.0f * v : hx - 18.0f * v, ay = sh + 4.0f * u - (k ? ps->shr : ps->shl) * u;
        float tx = k ? rhx : lhx, ty = k ? rhy : lhy, wv = (u >= 2.0f ? 3.0f : 1.8f) * v;
        int raised = k ? (arms == 1 || arms == 2 || arms == 4) : (arms == 1 || arms == 3 || arms == 4);
        if (below && !raised)
            continue;
        if (below || raised) {               /* a wide sleeve: a slanted band, the hand at its top */
            if (ty < ay)
                trap(ty, ay + 2.0f * u, tx - wv, tx + wv, ax - wv, ax + wv, C_ROBE2, 0, -1);
            else
                trap(ay - 2.0f * u, ty + 1.0f * u, ax - wv, ax + wv, tx - wv, tx + wv, C_ROBE2, 0, -1);
        } else {
            seg(ax, ay, tx + (k ? 2.0f : -2.0f) * v, ty + 1.0f * u, C_ROBE2, u >= 2.0f ? 3 : 2);
            if (u >= 2.0f)
                seg(ax + (k ? -1.0f : 1.0f) * v, ay, tx + (k ? 1.0f : -1.0f) * v, ty, C_ROBE2, 3);
        }
    }
    /* the face */
    ellipse(hh, h + 1.0f * u, 9.0f * v, 11.0f * u, C_SKIN);
    /* the keffiyeh over the head and down both sides, the agal round it */
    kef_dome(hh, h - 3.0f * u, 13.0f * v, 11.0f * u);
    trap(h - 3.0f * u, h - nod + 15.0f * u, hh - 13.0f * v, hh - 8.5f * v, hh - 15.5f * v, hh - 9.0f * v, 0, 1, ri(h - nod + 15.0f * u));
    trap(h - 3.0f * u, h - nod + 15.0f * u, hh + 8.5f * v, hh + 13.0f * v, hh + 9.0f * v, hh + 15.5f * v, 0, 1, ri(h - nod + 15.0f * u));
    boxf(hh - 12.5f * v, h - 8.5f * u, 25.0f * v, 1.8f * u, C_AGAL);
    boxf(hh - 12.0f * v, h - 6.0f * u, 24.0f * v, 1.8f * u, C_AGAL);
    disc(hh - 12.5f * v, h - 7.6f * u, 1.0f * u, C_AGAL);
    disc(hh + 12.5f * v, h - 7.6f * u, 1.0f * u, C_AGAL);
    /* the tassels at the keffiyeh's front corners: short cords, a knot on each */
    for (k = 0; k < 3; k++) {
        float tx = (float)tassel * 1.2f * v, lx = hh - 14.0f * v + (float)k * 2.2f * v + tx, rx = hh + 9.6f * v + (float)k * 2.2f * v + tx;
        float ty = h - nod + 15.0f * u, kr = u < 1.2f ? 0.8f : 0.75f * u;
        seg(lx, ty, lx, ty + 1.6f * u, C_ROBE2, 1);
        seg(rx, ty, rx, ty + 1.6f * u, C_ROBE2, 1);
        disc(lx, ty + 1.6f * u + kr, kr, C_KEFR);
        disc(rx, ty + 1.6f * u + kr, kr, C_KEFR);
    }
    /* eyebrows, the dark glasses and their glint */
    brow += ps->brow;
    boxf(hh - 7.5f * v, h - 4.4f * u - brow * u, 5.5f * v, 1.3f * u, C_MUST);
    boxf(hh + 2.0f * v, h - 4.4f * u - brow * u, 5.5f * v, 1.3f * u, C_MUST);
    rrect(hh - 8.2f * v, h - 2.8f * u, 7.4f * v, 5.0f * u, 1.4f * u, C_RIM);
    rrect(hh + 0.8f * v, h - 2.8f * u, 7.4f * v, 5.0f * u, 1.4f * u, C_RIM);
    rrect(hh - 7.7f * v, h - 2.3f * u, 6.4f * v, 4.0f * u, 1.2f * u, C_GLASS);
    rrect(hh + 1.3f * v, h - 2.3f * u, 6.4f * v, 4.0f * u, 1.2f * u, C_GLASS);
    boxf(hh - 0.9f * v, h - 1.6f * u, 1.8f * v, 0.9f * u, C_RIM);
    if (glint >= 0.0f) {
        float gx = hh - 7.0f * v + glint * 13.5f * v;
        int32_t cx0 = clip_x0, cy0 = clip_y0, cx1 = clip_x1, cy1 = clip_y1;
        clip_set(ri(hh - 7.7f * v), ri(h - 2.3f * u), ri(15.4f * v), ri(4.0f * u));   /* (on the lenses only) */
        if (clip_x0 < cx0) clip_x0 = cx0;
        if (clip_y0 < cy0) clip_y0 = cy0;
        if (clip_x1 > cx1) clip_x1 = cx1;
        if (clip_y1 > cy1) clip_y1 = cy1;
        seg(gx, h + 1.8f * u, gx + 2.4f * v, h - 2.6f * u, K_WH, u >= 2.0f ? 3 : 2);
        seg(gx + 2.0f * v, h + 1.8f * u, gx + 3.2f * v, h - 0.6f * u, K_WH, 1);
        clip_x0 = cx0; clip_y0 = cy0; clip_x1 = cx1; clip_y1 = cy1;
    }
    /* the nose, the moustache (it jumps as one piece), the mouth */
    ellipse(hh, h + 3.2f * u, 1.6f * v, 1.6f * u, C_SKIN2);
    if (sing > 0.05f)                        /* he sings what is played */
        ellipse(hh, h + 8.8f * u, 1.8f * v, (0.5f + sing * 1.2f) * u, C_MUST);
    else
        boxf(hh - 1.8f * v, h + 8.2f * u, 3.6f * v, 0.6f * u, C_SKIN2);
    ellipse(hh - 2.7f * v, h + 5.3f * u - ps->must * u, 3.4f * v, 1.5f * u, C_MUST);
    ellipse(hh + 2.7f * v, h + 5.3f * u - ps->must * u, 3.4f * v, 1.5f * u, C_MUST);
    /* the hands */
    ellipse(lhx, lhy, 3.4f * v, 4.4f * u, C_SKIN);
    ellipse(rhx, rhy, 3.4f * v, 4.4f * u, C_SKIN);
    if (u >= 2.0f)                           /* big enough: the fingers */
        for (k = 0; k < 3; k++) {
            seg(lhx + 0.6f * v, lhy - 2.4f * u + (float)k * 1.5f * u, lhx + 3.0f * v, lhy - 2.4f * u + (float)k * 1.5f * u, C_SKIN2, 1);
            seg(rhx - 3.0f * v, rhy - 2.4f * u + (float)k * 1.5f * u, rhx - 0.6f * v, rhy - 2.4f * u + (float)k * 1.5f * u, C_SKIN2, 1);
        }
    if (ps->kerchief >= 0 && arms >= 1 && arms <= 3) {   /* the handkerchief, turning round the raised hand */
        static const float KX[4] = {0.0f, 1.0f, 0.0f, -1.0f}, KY[4] = {-1.0f, 0.0f, 1.0f, 0.0f};
        float kx = arms == 3 ? lhx : rhx, ky = (arms == 3 ? lhy : rhy) - 5.0f * u, r = 4.5f * u;
        int q = ps->kerchief & 3;
        seg(kx, ky + 2.0f * u, kx + KX[q] * r, ky + KY[q] * r, C_KEFW, u >= 2.0f ? 2 : 1);
        boxf(kx + KX[q] * r - 2.6f * u, ky + KY[q] * r - 2.6f * u, 5.2f * u, 5.2f * u, C_KEFR);
        boxf(kx + KX[q] * r - 2.6f * u, ky + KY[q] * r - 2.6f * u, 5.2f * u, 1.0f * u, C_KEFW);
    }
    if (clap > 0.97f && hands_y >= 0.0f && arms == 0) {   /* the clap itself: three short strokes */
        seg(hx - 4.0f * v, hy2 - 6.0f * u, hx - 6.5f * v, hy2 - 9.5f * u, sc_clapc, 1);
        seg(hx, hy2 - 6.5f * u, hx, hy2 - 10.5f * u, sc_clapc, 1);
        seg(hx + 4.0f * v, hy2 - 6.0f * u, hx + 6.5f * v, hy2 - 9.5f * u, sc_clapc, 1);
    }
}

/* the beat now: its phase 0..1 and whether a rhythm runs (or the keys are played: a slower pulse) */
static float sc_ph, sc_lv;
static int sc_run, sc_beat, sc_bar_new;
static float clap_of(float ph)           /* together on the beat, apart by its middle, closing again at its end */
{
    return ph < 0.14f ? 1.0f : ph < 0.42f ? 1.0f - (ph - 0.14f) / 0.28f : ph < 0.86f ? 0.0f : (ph - 0.86f) / 0.14f;
}
static float pump_of(int drum, float ms) { float a = (float)(ms_now - ui.hit_at[drum]); return a < ms ? 1.0f - a / ms : 0.0f; }

/* his dance, from the beat: a move a bar, picked by the bar's number so it does not go round the same two; more as
 * it gets louder and faster, a hop and the handkerchief on a fill, a shimmy on a trill, a lean on a bend. Each move
 * puts whole pieces in places that change with the beat and its halves. */
static float sc_bend;
static int sc_fill, sc_trill;
static void dance(pose_t *p, float u, int whole)
{
    int beats = dabke_status()->beats ? dabke_status()->beats : 4, bar = sc_beat / beats, b = sc_beat % beats, half = sc_ph >= 0.5f, on = sc_ph < 0.22f;
    uint32_t hs = (uint32_t)(bar + 7) * 2654435761u;
    int move = (int)((hs >> 13) % 7u);
    float e = fm_clampf(0.55f + sc_lv * 1.4f, 0.55f, 1.25f), s1 = (b & 1) ? 1.0f : -1.0f;
    *p = STILL;
    p->kerchief = -1;
    p->must = on ? 0.7f : 0.0f;              /* the moustache jumps on every beat */
    if (sc_trill) {                          /* a trill: the shoulders shimmy, the head small and fast */
        int q = (int)fm_floorf(t_now * 12.0f) & 1;
        p->shl = q ? 1.2f : 0.0f;
        p->shr = q ? 0.0f : 1.2f;
        p->hdx = q ? 0.6f : -0.6f;
        return;
    }
    if (fm_fabsf(sc_bend) > 0.25f) {         /* a bend: he leans with it, his head goes up, one arm out */
        float d = sc_bend > 0.0f ? 1.0f : -1.0f;
        p->bx = d * 5.0f * u;
        p->hdx = d * 1.5f;
        p->hdy = -fm_minf(2.0f, fm_fabsf(sc_bend));
        p->arms = d > 0.0f ? 2 : 3;
        return;
    }
    if (sc_fill) {                           /* a fill: a hop, both arms up, the handkerchief round */
        p->by = on || !half ? -4.0f * u * e : 0.0f;
        p->arms = (b & 1) ? 1 : 2;
        p->kerchief = (int)fm_floorf(sc_ph * 4.0f);
        p->feet = on ? 3 : 0;
        p->shl = p->shr = 0.8f;
        return;
    }
    switch (move) {
    case 0:                                  /* the dabke shoulders: up and down in turn on each half beat */
        p->shl = half ? 2.6f * e : 0.0f;
        p->shr = half ? 0.0f : 2.6f * e;
        p->hdy = on ? 0.8f : 0.0f;
        p->hdx = half ? -0.8f : 0.8f;
        break;
    case 1:                                  /* step and kick: across on the beats, a kick on the second and fourth */
        p->bx = s1 * 4.0f * u * e;
        p->feet = (b & 1) && !half ? (b == 1 ? 1 : 2) : 0;
        p->hdx = -s1 * 1.2f;
        p->brow = on ? 1.2f : 0.0f;
        break;
    case 2:                                  /* the neck slide: his head across and back, the rest of him still */
        p->hdx = (b == 0 ? -2.6f : b == 1 ? 0.0f : b == 2 ? 2.6f : 0.0f) * e;
        p->must = on ? 1.2f : 0.0f;
        p->brow = b & 1 ? 0.0f : 1.4f;
        break;
    case 3:                                  /* one arm up with the handkerchief going round, a sway */
        p->arms = (bar & 1) ? 3 : 2;
        p->kerchief = (int)fm_floorf(sc_ph * 4.0f) + b;
        p->bx = (half ? 2.5f : -2.5f) * u * e;
        p->hdx = half ? -0.8f : 0.8f;
        break;
    case 4:                                  /* the hop on the first beat, a stomp after, arms out on the hop */
        p->by = b == 0 && !half ? -6.0f * u * e : b == 2 && on ? 1.0f * u : 0.0f;
        p->feet = b == 0 && !half ? 3 : b == 2 && on ? 2 : 0;
        p->arms = b == 0 && !half ? 4 : 0;
        p->hdy = b == 0 && !half ? -1.0f : 0.0f;
        p->brow = b == 0 && !half ? 2.0f : 0.0f;
        break;
    case 5:                                  /* a hand up on each beat in turn, a sway the other way */
        p->arms = b & 1 ? 2 : 3;
        p->bx = (b & 1 ? -2.0f : 2.0f) * u * e;
        p->hdx = b & 1 ? 1.0f : -1.0f;
        p->must = on ? 1.0f : 0.0f;
        break;
    default:                                 /* both arms up, pumping on the half beats, a bounce */
        p->arms = 1;
        p->shl = p->shr = half ? 0.0f : 2.0f * e;
        p->by = half ? 0.0f : -2.0f * u * e;
        p->kerchief = (int)fm_floorf(sc_ph * 4.0f) + b;
        p->hdy = half ? 0.6f : 0.0f;
        break;
    }
    if (!whole && p->feet)                   /* (the portrait: no feet; a kick is a dip of the shoulder instead) */
        p->shl += p->feet == 1 ? 1.0f : 0.0f, p->shr += p->feet == 2 ? 1.0f : 0.0f;
}

static void confetti(float x, float y, float w, float h)
{
    static const uint16_t CC[5] = {RGB(232, 64, 52), RGB(255, 186, 64), RGB(80, 190, 120), RGB(244, 238, 226), RGB(90, 150, 230)};
    float age = (float)(ms_now - ui.fill_at) * 0.001f;
    int k;
    if (!ui.fill_at || age > 2.6f)
        return;
    for (k = 0; k < 36; k++) {
        float fx = (float)((k * 73 + 11) % 97) / 97.0f, sp = 40.0f + (float)((k * 37) % 50);
        float yy = y - 6.0f + fmodp(age * sp + (float)((k * 29) % 41), h + 8.0f);
        int flip = (int)fm_floorf(age * 6.0f + (float)k) & 1;          /* (a piece turning: tall, then wide) */
        boxf(x + fx * w, yy, flip ? 2.0f : 3.0f, flip ? 3.0f : 2.0f, CC[k % 5]);
    }
}

static void bulbs(float x, float y, float w, int beat, int flash)   /* flash: every bulb lit, bigger (the kick, the dum) */
{
    static const uint16_t BC[4] = {RGB(255, 186, 64), RGB(232, 64, 52), RGB(80, 190, 120), RGB(244, 238, 226)};
    int k;
    float i;
    pl_start(RGB(70, 60, 60), 1);
    for (i = 0.0f; i <= w; i += 6.0f)
        pl_to(x + i, y + 7.0f * fm_sinf(i / w * FM_PI));
    for (k = 0; k < 14; k++) {
        float bx = x + 8.0f + (float)k * (w - 16.0f) / 13.0f, by = y + 7.0f * fm_sinf((bx - x) / w * FM_PI) + 3.0f;
        int lit = flash || (sc_run ? ((k + beat) & 1) : (k % 3 == (int)fm_floorf(t_now * 1.2f) % 3));
        disc(bx, by, flash ? 2.8f : 2.2f, lit ? BC[k & 3] : RGB(60, 50, 50));
    }
}

/* a strand of the light curtain behind the stage: a cord hanging from the top, n bulbs on it */
static void strand(float x, float y, int n, int beat, int flash, int j)
{
    static const uint16_t SC[3] = {RGB(255, 214, 120), RGB(244, 238, 226), RGB(255, 170, 90)};
    int k;
    box(ri(x), ri(y), 1, n * 9, RGB(58, 46, 60));
    for (k = 0; k < n; k++) {
        int lit = flash || (sc_run ? ((k + j + beat) % 3 == 0) : ((k + j + (int)fm_floorf(t_now * 1.5f)) % 3 == 0));
        disc(x + 0.5f, y + 5.0f + (float)k * 9.0f, flash ? 2.2f : 1.6f, lit ? SC[(k + j) % 3] : RGB(80, 62, 64));
    }
}

static void half_disc_fill(float cx, float cy, float r, uint16_t c)   /* the upper half of a disc */
{
    int32_t y = ri(cy - r - 0.5f), y1 = ri(cy);
    for (; y < y1; y++) {
        float dy = (float)y + 0.5f - cy, hw2 = r * r - dy * dy, hw;
        if (hw2 < 0.0f)
            continue;
        hw = fm_sqrtf(hw2);
        box(ri(cx - hw), y, ri(cx + hw) - ri(cx - hw), 1, c);
    }
}

/* a dancer of the dabke line: pose 0 both feet down, 1 the right leg kicked forward, 2 the stomp, 3 the left back */
static void dancer(float x, float gy, int pose, int kind, float dip)
{
    float hy = gy - 34.0f + dip, sh = hy + 6.0f, hip = gy - 14.0f + dip;
    uint16_t body = kind == 0 ? C_ROBE : kind == 1 ? RGB(150, 30, 60) : RGB(40, 90, 150);
    static const float LEG[4][4] = {{-3, 0, 3, 0}, {-3, 0, 8, -6}, {-2, 0, 2, 0}, {-8, -4, 3, 0}};
    seg(x - 2.0f, hip, x + LEG[pose][0], gy + LEG[pose][1], RGB(60, 50, 50), 2);
    seg(x + 2.0f, hip, x + LEG[pose][2], gy + LEG[pose][3], RGB(60, 50, 50), 2);
    trap(sh, hip + 4.0f, x - 4.0f, x + 4.0f, x - 6.0f, x + 6.0f, body, 0, -1);
    disc(x, hy, 4.0f, C_SKIN);
    if (kind == 0) {                         /* a keffiyeh, small */
        sc_u = 0.8f;
        kef_dome(x, hy, 5.0f, 5.0f);
        boxf(x - 5.0f, hy - 2.5f, 10.0f, 1.0f, C_AGAL);
        trap(hy - 1.0f, hy + 6.0f, x - 5.0f, x - 3.5f, x - 5.5f, x - 3.5f, 0, 1, -1);
        trap(hy - 1.0f, hy + 6.0f, x + 3.5f, x + 5.0f, x + 3.5f, x + 5.5f, 0, 1, -1);
    } else
        half_disc_fill(x, hy - 0.5f, 4.6f, RGB(30, 20, 20));
}

static void scene(int32_t x0, int32_t y0, int32_t w0, int32_t h0)
{
    const dabke_status_t *s = dabke_status();
    float x = (float)x0, y = (float)y0, w = (float)w0, h = (float)h0, tempo = (float)set.par[P_TEMPO];
    float beat_len = 60000.0f / (tempo < 20.0f ? 20.0f : tempo), since = (float)(ms_now - ui.beat_at);
    float glint = -1.0f, sing, brow, clap, nod;
    int k, bi, tassel, moving;
    pose_t pz;
    sc_run = s->running;
    sc_lv = ui.level;
    if (sc_run) {
        sc_ph = fm_minf(0.999f, since / beat_len);
        bi = (int)ui.beat_count;
    } else {                                 /* no rhythm: keys played make a slow pulse, else he rests */
        sc_ph = fmodp(t_now * 2.0f, 1.0f);
        bi = (int)fm_floorf(t_now * 2.0f);
    }
    sc_beat = bi;
    {                                        /* a glint across his glasses: on a bar's first beat, 2.5 s apart at least */
        static uint32_t glint_at;
        uint32_t bar0 = sc_run && (ui.beat_count - 1u) % (s->beats ? s->beats : 4u) == 0u ? ui.beat_at : 0u;
        if (!sc_run && ms_now - glint_at > 4000u)
            glint_at = ms_now;
        else if (bar0 && bar0 != glint_at && ms_now - glint_at > 2500u)
            glint_at = bar0;
        if (ms_now - glint_at < 360u)
            glint = (float)(ms_now - glint_at) / 360.0f;
    }
    sc_bar_new = glint >= 0.0f;
    sing = fm_minf(1.0f, ui.lead * 2.5f);
    brow = fm_clampf(s->bend, 0.0f, 2.0f) * 0.9f;
    sc_bend = s->bend;
    sc_fill = s->in_fill || (ui.fill_at && ms_now - ui.fill_at < 500u);
    sc_trill = set.ch[C_ORN] == ORN_TRILL && ui.lead > 0.05f;
    clap = sc_run ? clap_of(sc_ph) : (sc_lv > 0.02f ? clap_of(sc_ph) * 0.6f : 0.85f);
    nod = sc_run && sc_ph < 0.16f ? 1.0f : 0.0f;
    tassel = sc_run ? ((bi & 1) ? 1 : -1) : 0;
    moving = sc_run || sc_lv > 0.02f;        /* music: he dances (his two scenes); silence: he stands, deadpan */
    ui.last_draw = ms_now;
    clip_set(x0, y0, w0, h0);
    switch (set.ch[C_SCENE]) {
    case 0: {                                /* the portrait: his face big and a little wide on the tape label's colours */
        float cx = x + w / 2.0f, cy = y + 48.0f;
        int32_t yy, xx, par = bi & 1, bnd = 7, f0 = y0 + bnd, f1 = y0 + h0 - bnd;
        for (yy = f0; yy < f1; yy++) {       /* the yellow field: sixteen rays in two yellows, swapping on each beat */
            float dy = (float)yy + 0.5f - cy;
            for (xx = x0; xx < x0 + w0; xx += 2) {   /* (the sign of Im((dx + i dy)^8), two pixels at a time) */
                float dx = (float)xx + 1.0f - cx, a = dx * dx - dy * dy, b = 2.0f * dx * dy, a2, b2;
                a2 = a * a - b * b; b2 = 2.0f * a * b;
                a = a2 * a2 - b2 * b2; b = 2.0f * a2 * b2;
                (void)a;
                box(xx, yy, 2, 1, ((b > 0.0f) ^ par) ? C_YEL2 : C_YEL);
            }
        }
        for (k = 0; k < 5; k++) {            /* the bars at each side, with the drums */
            static const uint8_t DR[5] = {D_DUM, D_TEK, D_TABL, D_CLAP, D_RIQ};
            float lv = fm_maxf(pump_of(DR[k], 220.0f), sc_lv * (0.6f + 0.1f * (float)k)), bh = 6.0f + lv * 46.0f;
            boxf(x + 4.0f + (float)k * 7.0f, (float)f1 - bh, 5.0f, bh, k & 1 ? C_RED2 : C_KEFR);
            boxf(x + w - 9.0f - (float)k * 7.0f, (float)f1 - bh, 5.0f, bh, k & 1 ? C_RED2 : C_KEFR);
        }
        {                                    /* him, between the bands */
            int32_t a0 = clip_y0, a1 = clip_y1;
            clip_y0 = f0;
            clip_y1 = f1;
            sc_wx = 1.22f;
            sc_clapc = C_KEFR;
            if (moving)
                dance(&pz, 2.9f, 0);
            else
                pz = STILL;
            singer(cx, cy, 2.9f, clap, nod * 2.0f, glint, sing, tassel, brow, (float)f1 - 13.0f, &pz);
            sc_wx = 1.0f;
            sc_clapc = K_WH;
            clip_y0 = a0;
            clip_y1 = a1;
        }
        box(x0, y0, w0, bnd, C_KEFR);        /* the red bands; their lamps: the beat going round above, the drums below */
        box(x0, f1, w0, bnd, C_KEFR);
        for (k = 0; k < 16; k++) {
            static const uint8_t DB[4] = {D_DUM, D_TEK, D_KA, D_TABL};
            int up = sc_run ? (k & 3) == (bi & 3) : (k & 3) == ((int)fm_floorf(t_now * 2.0f) & 3);
            int dn = pump_of(DB[k & 3], 160.0f) > 0.25f || (DB[k & 3] == D_DUM && pump_of(D_KICK, 160.0f) > 0.25f);
            box(x0 + 6 + k * 15, y0 + 2, 3, 3, up ? C_KEFW : C_RED3);
            box(x0 + 6 + k * 15, f1 + 2, 3, 3, dn ? C_YEL : C_RED3);
        }
        break;
    }
    case 1: {                                /* the stage: string lights and a light curtain, flashing on the kick and the
                                              * dum; the carpet, its edge lit by the dum */
        float pump = fm_maxf(pump_of(D_KICK, 160.0f), pump_of(D_DUM, 160.0f));
        int flash = pump > 0.35f;
        box(x0, y0, w0, h0, C_NIGHT);
        box(x0, y0 + 60, w0, h0 - 60, C_NIGHT2);
        for (k = 0; k < 3; k++) {            /* the light curtain at each side */
            strand(x + 8.0f + (float)k * 12.0f, y + 12.0f + (float)(k & 1) * 4.0f, 8 - (k & 1), bi, flash, k);
            strand(x + w - 9.0f - (float)k * 12.0f, y + 12.0f + (float)(k & 1) * 4.0f, 8 - (k & 1), bi, flash, k + 1);
        }
        bulbs(x, y + 2.0f, w, bi, flash);
        trap(y + h - 14.0f, y + h, x + 30.0f, x + w - 30.0f, x, x + w, C_CARPET, 0, -1);
        box(x0 + 30, y0 + h0 - 14, w0 - 60, pump > 0.2f ? 2 : 1, pump > 0.2f ? RGB(255, 236, 160) : C_GOLD);
        if (moving)
            dance(&pz, 1.55f, 1);
        else
            pz = STILL;
        sc_hem = y + h - 9.0f;               /* (his feet show: he steps and kicks) */
        singer(x + w / 2.0f, y + 40.0f, 1.55f, clap, nod, glint, sing, tassel, brow, 0.0f, &pz);
        sc_hem = 0.0f;
        seg(x + w / 2.0f + 52.0f, y + h, x + w / 2.0f + 52.0f, y + 74.0f, RGB(120, 120, 132), 2);   /* the mic stand */
        seg(x + w / 2.0f + 52.0f, y + 74.0f, x + w / 2.0f + 32.0f, y + 64.0f, RGB(120, 120, 132), 2);
        ellipse(x + w / 2.0f + 30.0f, y + 62.0f, 3.2f, 4.6f, RGB(70, 70, 82));                   /* the mic */
        ellipse(x + w / 2.0f + 30.0f, y + 60.0f, 2.4f, 2.6f, RGB(150, 150, 160));
        confetti(x, y, w, h);
        break;
    }
    case 2: {                                /* the dabke line: dusk, the singer on his riser, six dancers stepping */
        int pose = sc_run || sc_lv > 0.02f ? bi & 3 : 0, i;
        float gy = y + h - 10.0f, slide = sc_run ? (float)((bi / 4) & 1) * 6.0f : 0.0f;
        box(x0, y0, w0, 40, RGB(54, 26, 62));
        box(x0, y0 + 40, w0, 36, RGB(110, 46, 60));
        box(x0, y0 + 76, w0, h0 - 76, RGB(48, 30, 30));
        disc(x + w - 26.0f, y + 16.0f, 8.0f, RGB(250, 230, 170));
        disc(x + w - 22.0f, y + 13.0f, 7.0f, RGB(54, 26, 62));                                  /* the crescent */
        for (k = 0; k < 9; k++)
            if (((k + (sc_run ? bi : (int)t_now)) % 4) != 0)
                box(x0 + 12 + k * 23, y0 + 5 + (k * 13) % 26, 1, 1, K_WH);
        box(x0, y0 + h0 - 10, w0, 10, RGB(80, 52, 40));
        box(x0 + 4, y0 + 62, 50, h0 - 62, RGB(90, 30, 36));                                       /* the riser */
        box(x0 + 4, y0 + 62, 50, 2, C_GOLD);
        singer(x + 29.0f, y + 24.0f, 0.85f, clap, nod, glint, sing, tassel, brow, 0.0f, &STILL);
        for (i = 0; i < 6; i++) {
            float dx = x + 76.0f + (float)i * 27.0f + slide, dip = pose == 2 ? 1.0f : 0.0f;
            dancer(dx, gy, pose, i % 3 == 1 ? 1 : i % 3 == 2 ? 2 : 0, dip);
            if (i < 5)                       /* hands held: the line */
                seg(dx + 4.0f, gy - 26.0f + dip, dx + 23.0f, gy - 26.0f + dip, C_SKIN, 1);
        }
        {                                    /* the leader waves his handkerchief */
            float lx = x + 76.0f + 5.0f * 27.0f + slide, up = (bi & 1) ? 1.0f : 0.0f;
            seg(lx + 4.0f, gy - 26.0f, lx + 9.0f, gy - 34.0f - up * 4.0f, C_SKIN, 1);
            boxf(lx + 8.0f, gy - 40.0f - up * 4.0f, 5.0f, 6.0f, C_KEFR);
        }
        confetti(x, y, w, h);
        break;
    }
    default: {                               /* the cassette: the wedding tape, its reels turning with the tempo */
        static float spin;
        static uint32_t spun_at;
        float dt = spun_at ? fm_minf(0.1f, (float)(ms_now - spun_at) * 0.001f) : 0.0f, cyl = y + 72.0f;
        char b[16], t[8];
        spin += dt * (sc_run ? tempo / 60.0f * FM_PI : sc_lv > 0.02f ? 2.5f : 0.0f);
        spin = fmodp(spin, FM_TWO_PI);
        spun_at = ms_now;
        box(x0, y0, w0, h0, RGB(58, 36, 26));
        for (k = 0; k < 5; k++)
            box(x0, y0 + 10 + k * 22, w0, 1, RGB(72, 46, 34));
        rrect(x + 18.0f, y + 6.0f, w - 36.0f, h - 10.0f, 6.0f, RGB(214, 204, 184));
        for (k = 0; k < 4; k++)
            disc(k & 1 ? x + w - 26.0f : x + 26.0f, k & 2 ? y + h - 12.0f : y + 14.0f, 2.2f, RGB(150, 140, 124));
        box(x0 + 30, y0 + 12, w0 - 60, 50, RGB(246, 240, 226));          /* the label */
        box(x0 + 30, y0 + 12, w0 - 60, 6, C_KEFR);
        box(x0 + 30, y0 + 56, w0 - 60, 6, C_KEFR);
        T("SIDE A", 92, y0 + 31, 9, RGB(60, 40, 40), 0, 0);
        T("WEDDING LIVE", 92, y0 + 46, 13, C_KEFR, 0, 110);
        b[0] = 0;
        text_cat(b, "BAR ");
        int_text((int32_t)(s->bar % 1000u), t);
        text_cat(b, t);
        T(b, x0 + w0 - 34, y0 + 31, 9, RGB(60, 40, 40), 2, 0);
        {                                    /* his portrait on it */
            int32_t a0 = clip_x0, a1 = clip_y0, a2 = clip_x1, a3 = clip_y1;
            clip_set(x0 + 30, y0 + 18, 56, 38);
            box(x0 + 30, y0 + 18, 56, 38, C_YEL);
            singer(x + 58.0f, y + 32.0f, 0.95f, clap, 0.0f, glint, sing, tassel, brow, -1.0f, &STILL);
            clip_x0 = a0; clip_y0 = a1; clip_x1 = a2; clip_y1 = a3;
        }
        rrect(x + 64.0f, cyl - 12.0f, w - 128.0f, 24.0f, 5.0f, RGB(40, 34, 32));   /* the window */
        for (k = 0; k < 2; k++) {
            float rx = k ? x + w - 84.0f : x + 84.0f;
            int j;
            disc(rx, cyl, 9.0f + (k ? 0.0f : 2.0f), RGB(80, 56, 40));          /* the tape wound on it */
            disc(rx, cyl, 5.0f, RGB(236, 232, 222));
            for (j = 0; j < 6; j++) {
                float a = spin + (float)j * 1.0472f;
                seg(rx + fm_cosf(a) * 2.0f, cyl + fm_sinf(a) * 2.0f, rx + fm_cosf(a) * 4.6f, cyl + fm_sinf(a) * 4.6f, RGB(80, 70, 60), 1);
            }
        }
        {                                    /* the VU meter */
            float mx = x + w - 56.0f, my = y + h - 14.0f, a = -2.5f + fm_minf(1.0f, sc_lv * 1.4f) * 1.8f;
            box(ri(mx - 16.0f), ri(my - 18.0f), 32, 20, RGB(244, 226, 170));
            arc(mx, my, 13.0f, -2.6f, -0.55f, RGB(90, 70, 50), 1);
            seg(mx, my, mx + fm_cosf(a) * 14.0f, my + fm_sinf(a) * 14.0f, sc_lv > 0.6f ? K_RED : RGB(40, 30, 30), 1);
        }
        trap(y + h - 18.0f, y + h - 4.0f, x + 70.0f, x + w - 70.0f, x + 60.0f, x + w - 60.0f, RGB(190, 180, 160), 0, -1);
        disc(x + 92.0f, y + h - 11.0f, 3.0f, RGB(58, 36, 26));
        disc(x + w - 92.0f, y + h - 11.0f, 3.0f, RGB(58, 36, 26));
        break;
    }
    }
    clip_all();
}
