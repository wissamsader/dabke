/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 STUDIOMORPHIK */
/* DABKE's rhythms: the traditional patterns of each iqa', written step by step. Each lane is one drum, then the bass
 * and the backing; one character a step: '.' nothing, 'o' soft, 'x' a hit, 'X' an accent. Bass: 'r' the root, 'f' the
 * fifth, 'o' the octave, 't' the maqam's third, '4' its fourth, '7' its seventh. Backing: 'x' a stab, '_' held on.
 * The skeleton of each (dum D, tek T, ka k) is the common teaching notation: Maqsum D T . T D . T ., Baladi
 * D D . T D . T ., Saidi D T . D D . T ., Malfuf D . . T . . T ., Ayoub D . k D T . k ., Fallahi a fast Maqsum in two,
 * Karachi T . k . T . D ., Zaffa D . T k T . T . D . T . T . . ., Khaliji D . . D . . T T (with the hand claps),
 * Sama'i Thaqil 10/8 D . . T . D D T . ., Masmudi 8/4 D . D . . . T . D . . . T . T .
 * Lanes: dum, tek, ka, slap, tabl, tabl stick, riq, daf, sagat, kick, clap, snare, shaker, bass, backing. */
#define DAB_NRHYTHM 21
static const rhythm_t RHYTHMS[DAB_NRHYTHM] = {
    {"Dabke Syria", "Levant", 4, 140, 8, {"x..x.x..", "..x...x.", ".o..o..o", 0, "x...x...", 0, 0, 0, 0, "x...x...",
     "....X...", 0, "xoxoxoxo", "r..r.f..", "..x...x."}},
    {"Dabke Lebanon", "Levant", 4, 116, 16, {"x.....x.x.......", "....x.......x...", 0, 0, "x.....x.x.......",
     "..x.x.....x.x.x.", 0, 0, 0, 0, "....x.......x...", 0, 0, "r.....r.r...f...", "....x.......x..."}},
    {"Dabke 3-3-2", "Levant", 4, 132, 16, {"x..x..x.x..x..x.", "....x.x.....x.x.", ".o.......o......", 0, 0, 0,
     "x.x.x.x.x.x.x.x.", 0, 0, "x..x..x.x..x..x.", "....x.......x...", 0, 0, "r..r..f.r..r..o.", "....x.......x..."}},
    {"Tabl Zurna", "Levant", 4, 120, 8, {0, 0, 0, 0, "x..x..x.", ".xx.xx.x", 0, 0, 0, 0, 0, 0, 0, "r..r..f.", 0}},
    {"Maqsum", "Egypt", 4, 100, 16, {"x.......x.......", "..x...x.....x...", "....o.....o...o.", 0, 0, 0,
     "x.o.x.o.x.o.x.o.", 0, 0, "x.......x.......", 0, 0, 0, "r.......r.....f.", "..x...x.....x..."}},
    {"Baladi", "Egypt", 4, 96, 16, {"x.x.....x.......", "......x.....x...", "....o.....o...o.", 0, 0, 0,
     "x.o.x.o.x.o.x.o.", 0, 0, "x.x.....x.......", 0, 0, 0, "r.r.....r.....f.", "......x.....x..."}},
    {"Saidi", "Egypt", 4, 112, 16, {"x.....x.x.......", "..x.........x...", "....o.....o...o.", 0, "x.....x.x.......",
     "..x.x.....x.x...", 0, 0, 0, 0, 0, 0, 0, "r.....r.r.....f.", "..x.........x..."}},
    {"Wahda", "Egypt", 4, 84, 16, {"x...............", "......x.....x...", "...o......o...o.", 0, 0, 0,
     "x...x...x...x...", "x.......x.......", 0, 0, 0, 0, 0, "r.......f.......", "......x.....x..."}},
    {"Malfuf", "Egypt", 4, 120, 8, {"x.......", "...x..x.", ".o..o..o", 0, 0, 0, "x.x.x.x.", 0, 0, "x.......", 0, 0, 0,
     "r..f..f.", "...x..x."}},
    {"Ayoub", "Egypt", 4, 116, 8, {"x..x....", "....x...", "..o...o.", 0, "x..x....", 0, 0, 0, 0, 0, 0, 0, 0,
     "r..r....", "....x..."}},
    {"Fallahi", "Egypt", 4, 132, 8, {"x...x...", ".x.x..x.", "..o..o.o", 0, 0, 0, "xoxoxoxo", 0, 0, 0, 0, 0, 0,
     "r...f...", ".x.x..x."}},
    {"Karachi", "Egypt", 4, 116, 8, {"......x.", "x...x...", "..o....o", 0, 0, 0, "x.x.x.x.", 0, 0, 0, 0, 0, 0,
     "......r.", "x...x..."}},
    {"Zaffa", "Egypt", 4, 100, 16, {"x.......x.......", "..x.x.x...x.x...", "...o............", 0, "x.......x.......", 0,
     0, 0, "x.x.x.x.x.x.x.x.", 0, 0, 0, 0, "r.......f.......", "x.......x......."}},
    {"Shaabi", "Egypt", 4, 112, 16, {"x.......x.......", "..x...x.....x...", "...o.o....o.o..o", 0, 0, 0, 0, 0,
     "x.x.x.x.x.x.x.x.", "x.......x.......", "......x.....x...", 0, 0, "r.....r.r.....f.", "..x...x.....x..."}},
    {"Mahragan", "Egypt", 4, 128, 16, {"x.....x.x.......", "..x.x.....x.x.x.", 0, 0, 0, 0, 0, 0, 0, "x...x...x...x...",
     "....x.......x.x.", "....x.......x...", "oxoxoxoxoxoxoxox", "r..r..r.r..r..f.", "..x...x...x...x."}},
    {"Khaliji", "Gulf", 4, 88, 8, {"x..x....", "......x.", ".......o", 0, "x..x....", 0, 0, 0, 0, 0, "......Xx", 0,
     "x.x.x.x.", "r..r....", "......x."}},
    {"Chobi", "Iraq", 4, 138, 8, {"x..x....", "....x.x.", ".o...o.o", 0, "x..x....", "..x...x.", 0, 0, 0, 0, "....x.x.",
     0, 0, "r..r..f.", "....x.x."}},
    {"Chaabi 6/8", "Maghreb", 3, 104, 6, {"x..x..", "..x.xx", ".o....", 0, 0, 0, "xoxxox", "x..x..", 0, 0, "...x..", 0,
     0, "r..f..", "..x.x."}},
    {"Samai 10/8", "Classic", 2, 80, 10, {"x....xx...", "...x...x..", ".o..o...o.", 0, 0, 0, "x.x.x.x.x.",
     "x....xx...", 0, 0, 0, 0, 0, "r....ff...", "...x...x.."}},
    {"Laz 7/8", "Classic", 2, 100, 7, {"x...x..", "..x..x.", ".o....o", 0, 0, 0, "x.x.x.x", 0, 0, 0, 0, 0, 0, "r...f..",
     "..x..x."}},
    {"Masmudi 8/4", "Classic", 2, 92, 16, {"x.x.....x.......", "......x.....x.x.", "...o.......o....", 0, 0, 0, 0,
     "x.x.....x.......", 0, 0, 0, 0, 0, "r.r.....f.......", "......x.....x.x."}},
};
