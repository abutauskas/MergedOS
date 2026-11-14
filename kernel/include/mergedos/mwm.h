/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_MWM_H
#define MERGEDOS_MWM_H

/*
 * /dev/mwm: window-manager device for the Wayland compositor (docs/WAYLAND.md).
 * One opener at a time (EEXIST otherwise); ENOENT while the desktop is not
 * running (text mode). Plain C types so guests can include this header.
 *
 * write(): one command per call, struct mwm_cmd followed by `len` payload bytes
 * (the write size must be exactly sizeof(cmd) + len). Windows are named by a
 * small handle (1..255) the client picks. Commands are applied by the desktop
 * thread within ~20 ms; pixels are copied at once.
 *
 *   MWM_CREATE  win=handle a=content width b=content height (<= 1280 x 800)
 *   MWM_DESTROY win
 *   MWM_TITLE   win, payload = title bytes (cut at 47)
 *   MWM_PIXELS  win, a,b = x,y  c,d = w,h, payload = w*h XRGB8888, rows packed
 *   MWM_RESIZE  win, a,b = new width,height (content is cleared)
 *
 * read(): whole struct mwm_event records (32 bytes each), blocking unless the
 * descriptor is O_NONBLOCK (then EAGAIN when empty). Event queue holds 64;
 * the oldest are dropped when full. Positions are relative to the window content.
 */
#include <stdint.h>

#define MWM_PATH      "/dev/mwm"
#define MWM_MAX_WIN   4
#define MWM_MAX_W     1280
#define MWM_MAX_H     800

enum mwm_op { MWM_CREATE = 1, MWM_DESTROY, MWM_TITLE, MWM_PIXELS, MWM_RESIZE };

struct mwm_cmd {
	uint32_t op, win;
	int32_t a, b, c, d;
	uint32_t len;
};

enum mwm_ev_type {
	MWM_EV_MOTION = 1,      /* x, y                                          */
	MWM_EV_BUTTON,          /* x, y, code 0 left 1 right 2 middle, state 1 down */
	MWM_EV_KEY,             /* code = console character (ASCII, or 0x110001..4 up/down/left/right), state 1 */
	MWM_EV_FOCUS,           /* state 1 gained, 0 lost                        */
	MWM_EV_CLOSE,           /* the user clicked the close button             */
	MWM_EV_LEAVE,           /* the pointer left the window                   */
};

struct mwm_event {
	uint32_t type, win;
	int32_t x, y;
	uint32_t code, state, time_ms, pad;
};

#endif /* MERGEDOS_MWM_H */
