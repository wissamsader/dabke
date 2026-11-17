/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 STUDIOMORPHIK */
/* DABKE's maqamat, each written as its notes from its tonic, the way a musician writes them: the letters C D E F G A B,
 * each with 'b' a flat, '#' a sharp or 'h' a half flat (a quarter tone flat: "Eh" is E a quarter tone down), one space
 * between notes. The first note is the tonic, and the root the MAQAM page puts the maqam on. DABKE turns each line into
 * the keys of its scale and the keys a quarter tone flat when it starts (dabke.c mq_make). The simulator's tests read
 * every line (app/tests/maqam.txt): a note they cannot read, or two notes on one key, fails there, before a build ever
 * reaches an FM-1.
 *
 * The saved settings keep a maqam by its place in this list. A new maqam goes at the END only: never move, insert or
 * remove one above it, or a saved song comes back in another maqam. */
static const maqam_text_t MAQAMAT[] = {
    {"Plain", "C D E F G A B"},              /* no quarter tones: the twelve as they are */
    {"Rast", "C D Eh F G A Bh"},             /* the third and the seventh half flat */
    {"Bayati", "D Eh F G A Bb C"},
    {"Saba", "D Eh F Gb A Bb C"},
    {"Sikah", "Eh F G A Bh C D"},
    {"Huzam", "Eh F G Ab B C D"},
    {"Iraq", "Bh C D Eh F G A"},
    {"Bayati Shuri", "D Eh F G Ab B C"},     /* Bayati with Hijaz on G */
    {"Hijaz", "D Eb F# G A Bb C"},
    {"Hijaz Kar", "C Db E F G Ab B"},
    {"Kurd", "D Eb F G A Bb C"},
    {"Nahawand", "C D Eb F G Ab B"},
    {"Ajam", "Bb C D Eb F G A"},
    {"Nakriz", "C D Eb F# G A Bb"},
    {"Saba Zamzam", "D Eb F Gb A Bb C"},
};
