/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * PS/2 mouse on the i8042 auxiliary port, polled.
 *
 * The controller shares one output buffer between keyboard and mouse and
 * tags mouse bytes in the status register, so console.c reads the port and
 * hands aux bytes to mouse_feed(). Standard 3-byte packets only (no wheel).
 */
#include <mergedos/kernel.h>
#include <mergedos/gfx.h>
#include <mergedos/io.h>

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_CMD     0x64
#define PS2_ACK     0xFA
#define SPIN_LIMIT  200000

static struct mouse_state state;
static u8 packet[3];
static int packet_len;

const struct mouse_state *mouse_get(void)
{
	return &state;
}

static void wait_input_clear(void)
{
	for (int i = 0; i < SPIN_LIMIT && (inb(PS2_STATUS) & 0x02); i++)
		cpu_relax();
}

static int read_data(void)
{
	for (int i = 0; i < SPIN_LIMIT; i++) {
		if (inb(PS2_STATUS) & 0x01)
			return inb(PS2_DATA);
		cpu_relax();
	}
	return -1;
}

static void controller_cmd(u8 cmd)
{
	wait_input_clear();
	outb(PS2_CMD, cmd);
}

static bool mouse_cmd(u8 cmd)
{
	controller_cmd(0xD4);                   /* next data byte goes to the aux device */
	wait_input_clear();
	outb(PS2_DATA, cmd);
	return read_data() == PS2_ACK;
}

void mouse_init(void)
{
	const struct gfx_info *g = gfx_get();
	int config;

	while (inb(PS2_STATUS) & 0x01)          /* drop stale bytes */
		(void)inb(PS2_DATA);

	controller_cmd(0xA8);                   /* enable the aux port */
	controller_cmd(0x20);                   /* read the configuration byte */
	config = read_data();
	if (config < 0)
		return;
	controller_cmd(0x60);                   /* write it back with the aux clock running */
	wait_input_clear();
	outb(PS2_DATA, (u8)(config & ~0x20));

	if (!mouse_cmd(0xF6) || !mouse_cmd(0xF4))       /* defaults, then enable reporting */
		return;
	state.present = true;
	state.x = (int)g->width / 2;
	state.y = (int)g->height / 2;
	gfx_cursor_set(state.x, state.y);
}

void mouse_feed(u8 byte)
{
	const struct gfx_info *g = gfx_get();
	int dx, dy;

	if (!state.present)
		return;
	/* Byte 0 always has bit 3 set: use it to resynchronise after a lost byte. */
	if (packet_len == 0 && !(byte & 0x08))
		return;
	packet[packet_len++] = byte;
	if (packet_len < 3)
		return;
	packet_len = 0;
	if (packet[0] & 0xC0)                   /* overflow: discard the movement */
		return;

	dx = packet[1] - ((packet[0] & 0x10) ? 256 : 0);
	dy = packet[2] - ((packet[0] & 0x20) ? 256 : 0);
	state.x += dx;
	state.y -= dy;                          /* PS/2 y grows upwards */
	if (state.x < 0)
		state.x = 0;
	if (state.y < 0)
		state.y = 0;
	if (g->active && state.x >= (int)g->width)
		state.x = (int)g->width - 1;
	if (g->active && state.y >= (int)g->height)
		state.y = (int)g->height - 1;
	if ((packet[0] & MOUSE_LEFT) && !(state.buttons & MOUSE_LEFT)) {
		state.presses++;
		state.press_x = state.x;
		state.press_y = state.y;
	} else if (!(packet[0] & MOUSE_LEFT) && (state.buttons & MOUSE_LEFT)) {
		state.releases++;
		state.release_x = state.x;
		state.release_y = state.y;
	}
	state.buttons = packet[0] & 0x07;
	state.packets++;
	gfx_cursor_set(state.x, state.y);
}
