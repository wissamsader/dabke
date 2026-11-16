/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 STUDIOMORPHIK
 * Copyright (C) 2026 Charles Vestal (the parts from fm1-base's template) */
/* DABKE: an Arabic wedding keyboard with quarter tones and a darbuka rhythm box, on fm1-base. This header is the app's
 * half of the contract with the platform (device/unity.c, host/plat_host.c), as fm1-base's template has it. The sound
 * is dabke.h's engine. Built from COMBO's app (same panel layout, same safety rules). */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "DABKE"
#define APP_SPLASH_RGB RGB(232, 64, 52)
#define APP_BLOCK 256u                   /* frames per audio block: 5.8 ms at 44.1 kHz */

/* the store: the settings (what is set now), the panel calibration (the platform's) and the kept sounds, and their
 * flash offsets (two 4 KiB sectors each). The same places as COMBO's: its records there are not read (another magic) */
enum { OBJ_SETTINGS, OBJ_PANEL, OBJ_KEPT, OBJ_SEQ, OBJ_NOBJ };
/* DABKE's settings and kept sounds have sectors of their own (0x9B000, 0x99000: two each), not COMBO's (0xFC000,
 * 0x97000), so the owner can switch between the two firmwares and each finds what it saved. The panel's calibration
 * (0xDC000) is the platform's and the same for both. The sequencer's part (each track's length, its steps past the
 * first bar, the locks) has the next two sectors after the settings (0x9D000), a new object: the first three are where
 * and what they were */
#define APP_STORE_MAP {0x9B000u, 0xDC000u, 0x99000u, 0x9D000u}

/* velocity and pressure from the keys (fm1-base lib/touch): KEY TOUCH is the velocity, PRESSURE the pressure: the keys are sensed for either */
#define APP_TOUCH

/* the contract: the platform calls these */
void app_init(void);                     /* boot: the engine, then the saved settings (or the starting sound) */
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);   /* audio interrupt: n stereo frames, 24-bit */
void app_panic(void);                    /* silence now (before an update rewrites the flash) */
void app_ui_init(void);                  /* the first screen, after the splash */
void app_frame(void);                    /* the UI: input and a redraw, about 60 times a second */
void app_input_only(void);               /* between frames: input only */
void app_say(const char *a, const char *b);   /* a short message on the screen and to a screen reader */
#define APP_OVERLOAD                     /* the render took over 90 % of its time: fewer notes at once */
void app_overload(void);
#define APP_SYSEX                        /* (for plat_usb_session: is a computer on the port; no SysEx of our own) */
int app_sysex(const uint8_t *p, uint32_t n);
#define APP_UPDATE_SAVE                  /* an update starts: what is not saved yet, saved now, whatever plays (the firmware
                                          * put on may be another; DABKE's work is there when it comes back) */
void app_update_save(void);
#define APP_LOG                          /* the keys' pressure in the development readout over USB (fm1_remote.py log) */
void app_log(void (*line)(const char *t));

/* the host simulator's checks (host/plat_host.c "expect"): page (0 home, 1 maqam, 2 fx, 3 sound, 4 ornament, 5
 * rhythm), save, row, cur (the maqam page's note), octave, sound (0.. a ready sound, 100 + N kept place N, -1 none),
 * rhythm, rhythm2 (-1 none), running, arp, sync, left (0 same, 1 lower, 2 bass), tempo, scene, dirty, kept, mixleds,
 * maqam, root, qdepth, centsN (note N's tuning), parN (knob N in its own steps), chN (choice N), keysdim, drum, own
 * (the rhythms of your own, a bit each), left_root, dlevel, dtune, dlen, pressure, mute (the tracks muted, a bit
 * each), track (the picked one: 0 all), the tracks' loops (rec_track, rec_waiting, overdub, trk_nT, trk_lenT,
 * trk_barsT, trk_takesT, trk_keyT_I, trk_posT_I, trk_durT_I, playedT, kept_nT, loop_plays, loop_last_trk), their steps
 * (dedit, hisT, tpatT_L_S, tnT, two, trigT_S, drum_take), the sequencer (seq, spage, sstep, pages, nsteps, page_steps,
 * tstepT, tapnT, tapT_I), the locks (locks, locksT, lockT_S_ID, lk_atID, eparID), the keys' lights (lit_keys,
 * lit_keyN, dim_keys, dim_keyN, mid_keys, mid_keyN), pitch_x10 (what sounds now), and more: app.c app_expect */
#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
#define APP_HOST_REBOOT                  /* a reboot forgets what RAM would: the settings come from the flash again */
void app_host_reboot(void);
#define APP_HOST_CMD                     /* the script's "scene N", "held TEXT" (the held step's line on the screen) */
int app_host_cmd(const char *cmd, const char *a, const char *b, const char *c, const char *d);
#define APP_HOST_FAILS                   /* the checks those commands made themselves that failed */
int app_host_fails(void);
