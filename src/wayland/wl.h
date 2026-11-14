/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_WL_H
#define MERGEDOS_WL_H

/*
 * Wayland wire protocol and a minimal compositor state machine (docs/WAYLAND.md).
 * Portable and freestanding: no syscalls, no allocation, no libc beyond
 * memcpy/memset/memmove/strlen (the guest supplies those).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WL_MSG_MAX   4096       /* largest message accepted */
#define WL_IN_BUF    16384
#define WL_OUT_BUF   65536
#define WL_MAX_FDS   28

struct wl_conn {
	uint8_t in[WL_IN_BUF];
	size_t in_len;
	int in_fd[WL_MAX_FDS];
	int in_nfd;
	uint8_t out[WL_OUT_BUF];
	size_t out_len;
	int out_fd[WL_MAX_FDS];         /* sent with the first chunk; the platform closes them */
	int out_nfd;
	bool err;                       /* protocol error or overflow: drop the client */
};

/* ---- reader ---- */
struct wl_msg {
	uint32_t obj;
	uint16_t op, size;              /* size includes the 8-byte header */
	const uint8_t *p, *end;         /* read cursor over the arguments */
	struct wl_conn *c;
	bool bad;                       /* an argument ran past the message */
};

/* 1: a whole message is at the head of c->in (not consumed); 0: need more; -1: malformed. */
int wl_next(struct wl_conn *c, struct wl_msg *m);
void wl_pop(struct wl_conn *c, const struct wl_msg *m);        /* consume it */
/* Append received bytes and fds; false if they do not fit. */
bool wl_feed(struct wl_conn *c, const void *data, size_t n, const int *fds, int nfd);

uint32_t wl_u32(struct wl_msg *m);
int32_t  wl_i32(struct wl_msg *m);
/* NUL-terminated string inside the message (NULL and bad on error; empty for a null string). */
const char *wl_str(struct wl_msg *m);
const uint8_t *wl_arr(struct wl_msg *m, uint32_t *len);
int      wl_fd(struct wl_msg *m);       /* -1 and bad if none queued */

/* ---- writer ---- */
struct wl_out {
	struct wl_conn *c;
	size_t start;
	bool bad;
};

struct wl_out wl_begin(struct wl_conn *c, uint32_t obj, uint16_t op);
void wl_put_u32(struct wl_out *o, uint32_t v);
void wl_put_str(struct wl_out *o, const char *s);
void wl_put_arr(struct wl_out *o, const void *p, uint32_t len);
void wl_put_fd(struct wl_out *o, int fd);
void wl_end(struct wl_out *o);          /* fills the size; rolls back on overflow */
/* The platform sent n bytes (and, with the first chunk, all queued fds, now closed by it). */
void wl_sent(struct wl_conn *c, size_t n);

/* ---- compositor ---- */

#define WL_MAX_CLIENTS  4
#define WL_MAX_OBJ      1024
#define WL_MAX_SURF     64
#define WL_MAX_POOL     16
#define WL_MAX_BUF      64
#define WL_MAX_POS      8
#define WL_FRAMES       8

enum wl_iface {
	WI_NONE = 0, WI_DISPLAY, WI_REGISTRY, WI_CALLBACK, WI_COMPOSITOR, WI_SURFACE, WI_REGION,
	WI_SHM, WI_POOL, WI_BUFFER, WI_SUBCOMP, WI_SUBSURF, WI_WMBASE, WI_POSITIONER, WI_XSURF,
	WI_TOPLEVEL, WI_POPUP, WI_SEAT, WI_POINTER, WI_KEYBOARD, WI_OUTPUT, WI_XOUTPUT,
	WI_XOUTMGR, WI_DDM, WI_DSOURCE, WI_DDEV, WI_VPORTER, WI_VIEWPORT, WI_PCONS, WI_PLOCK,
	WI_RELMGR, WI_RELPTR, WI_IFACES
};

struct wl_obj {
	uint8_t iface;
	uint8_t ver;
	uint16_t idx;                   /* pool / buffer / surface / positioner index */
};

struct wl_pool {
	bool used;
	uint8_t *base;
	size_t size;
	int fd;                         /* kept open for wl_shm_pool.resize */
	int refs;                       /* the pool object plus its buffers */
};

struct wl_buffer {
	bool used;
	uint16_t pool;
	uint32_t id;
	uint32_t offset, stride, fmt;
	int32_t w, h;
	bool zombie;                    /* destroyed by the client while still shown */
};

struct wl_pos {
	bool used;
	int32_t w, h, ax, ay, aw, ah, ox, oy;
	uint32_t anchor, gravity;
};

struct wl_surface {
	bool used;
	uint32_t id;                    /* wl_surface object id */
	uint8_t role;                   /* 0 none, 1 toplevel, 2 popup, 3 subsurface */
	bool mapped;
	int32_t parent;                 /* surface index (subsurface/popup), -1 */
	int32_t x, y, px, py;           /* position in the parent (current / pending) */
	int32_t buf;                    /* pending attached buffer index, -1 none */
	int32_t cur;                    /* committed buffer index (held until replaced), -1 */
	bool attached;                  /* attach seen since the last commit */
	uint32_t frames[WL_FRAMES];
	int nframes;
	uint32_t xsurf, top, popup, subsurf, viewport;   /* role object ids */
	bool configured;
	int32_t gx, gy, gw, gh;         /* window geometry, 0 = whole buffer */
	int32_t dw, dh;                 /* viewport destination, 0 = none */
	int32_t cw, ch;                 /* last committed size (after viewport) */
	int win;                        /* host window handle (toplevel), 0 */
	char title[48];
};

struct wl_server;

struct wl_client {
	bool used;
	struct wl_server *srv;
	struct wl_conn conn;
	struct wl_obj obj[WL_MAX_OBJ];  /* indexed by object id */
	struct wl_pool pool[WL_MAX_POOL];
	struct wl_buffer buf[WL_MAX_BUF];
	struct wl_pos pos[WL_MAX_POS];
	struct wl_surface surf[WL_MAX_SURF];
	uint32_t serial;
	int ptr_surf, kbd_surf;         /* surface under the pointer / with keyboard focus, -1 */
	int ptr_x, ptr_y;
};

/* What the platform supplies. All hooks are NULL-checked. */
struct wl_host {
	void *ctx;
	uint8_t *(*map)(void *ctx, int fd, size_t size);       /* read-only shared mapping */
	void (*unmap)(void *ctx, uint8_t *p, size_t size);
	void (*close_fd)(void *ctx, int fd);
	int  (*win_create)(void *ctx, int w, int h);           /* handle > 0, or <= 0 on failure */
	void (*win_resize)(void *ctx, int win, int w, int h);
	void (*win_title)(void *ctx, int win, const char *title);
	void (*win_pixels)(void *ctx, int win, int x, int y, int w, int h,
			   const uint32_t *src, size_t stride_px);
	void (*win_destroy)(void *ctx, int win);
	int  (*keymap_fd)(void *ctx, uint32_t *size);          /* a new fd each call; -1 none */
	uint32_t (*now_ms)(void *ctx);
	int out_w, out_h;               /* the single output */
};

struct wl_server {
	struct wl_host host;
	struct wl_client cl[WL_MAX_CLIENTS];
	int focus_win;                  /* host window with keyboard focus, 0 */
	uint32_t mods;                  /* last modifier state sent (bit0 shift, bit2 ctrl) */
};

void wl_server_init(struct wl_server *s, const struct wl_host *h);
struct wl_client *wl_client_add(struct wl_server *s);          /* NULL when full */
/* Run every whole message in the client's input; false once the client must be dropped. */
bool wl_client_dispatch(struct wl_client *c);
void wl_client_drop(struct wl_client *c);       /* frees everything, closes queued fds */

/* Input from the window manager. Coordinates are relative to the window content. */
void wl_in_pointer_motion(struct wl_server *s, int win, int x, int y);
void wl_in_pointer_button(struct wl_server *s, int win, int x, int y, unsigned btn, bool down);
void wl_in_pointer_leave(struct wl_server *s);
void wl_in_mods(struct wl_server *s, unsigned mods);  /* bit0 shift, bit2 ctrl */
void wl_in_key(struct wl_server *s, unsigned evdev_code, bool down);
void wl_in_focus(struct wl_server *s, int win, bool focused);
void wl_in_close(struct wl_server *s, int win);
/* Console character (ASCII, or the KEY_UP.. values) to an evdev key + modifier bits, 0 if none. */
unsigned wl_ascii_to_key(int c, unsigned *mods);

extern const char wl_keymap_text[];
#define WL_BTN_LEFT 0x110
#define WL_BTN_RIGHT 0x111
#define WL_BTN_MIDDLE 0x112

#endif
