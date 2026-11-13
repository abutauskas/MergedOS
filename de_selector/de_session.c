/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Session start for the selected desktop.
 *
 * PoC scope: there is no graphics stack yet, so each profile is rendered as
 * an 80x25 text-mode mock-up of its layout, and the kernel shell runs inside
 * the "window" that layout leaves free. What is real is the decision and the
 * session descriptor (compositor backend, memory budget) derived from it.
 */
#include <mergedos/kernel.h>
#include <mergedos/console.h>
#include "de_selector/de_selector.h"

static enum de_id current = DE_TILING;

enum de_id de_session_current(void)
{
	return current;
}

static void center(int row, const char *s, u8 attr)
{
	vga_write_at(row, (VGA_COLS - (int)strlen(s)) / 2, s, attr);
}

void de_session_start(enum de_id id, const struct hw_profile *hw)
{
	const struct de_profile *p = de_profile(id);
	const bool gpu = hw->gpu_tier >= HW_GPU_PARAVIRT && id != DE_TILING && id != DE_RETRO;
	u8 bar, body;
	char status[VGA_COLS + 1];

	current = id;
	console_set_region(0, VGA_ROWS - 1);
	/* With a framebuffer the real graphical shell takes over; the text-mode
	 * mock-ups below remain for gfx=off. */
	if (desktop_start(id))
		goto report;
	ksnprintf(status, sizeof(status), "%u CPU | %u MiB | %s", hw->cpus, hw->ram_mib,
		  hw_gpu_tier_name(hw->gpu_tier));

	switch (id) {
	case DE_TASKBAR:
		bar = VGA_ATTR(VGA_WHITE, VGA_BLUE);
		body = VGA_ATTR(VGA_LGRAY, VGA_BLACK);
		console_set_attr(body);
		console_clear();
		vga_fill_row(VGA_ROWS - 1, ' ', bar);
		vga_write_at(VGA_ROWS - 1, 0, " [Start] | msh |", bar);
		vga_write_at(VGA_ROWS - 1, VGA_COLS - (int)strlen(status) - 1, status, bar);
		console_set_region(0, VGA_ROWS - 2);
		break;
	case DE_DOCK:
		bar = VGA_ATTR(VGA_BLACK, VGA_LGRAY);
		body = VGA_ATTR(VGA_WHITE, VGA_BLACK);
		console_set_attr(body);
		console_clear();
		vga_fill_row(0, ' ', bar);
		vga_write_at(0, 1, "MergedOS  File  Edit  View  Window  Help", bar);
		vga_write_at(0, VGA_COLS - (int)strlen(status) - 1, status, bar);
		vga_fill_row(VGA_ROWS - 1, ' ', body);
		center(VGA_ROWS - 1, "[ Files ] [ Terminal ] [ Packages ] [ Settings ]", bar);
		console_set_region(1, VGA_ROWS - 2);
		break;
	case DE_RETRO:
		bar = VGA_ATTR(VGA_WHITE, VGA_BLUE);
		body = VGA_ATTR(VGA_BLUE, VGA_LGRAY);
		console_set_attr(body);
		console_clear();
		vga_fill_row(0, ' ', bar);
		center(0, "MergedOS Retro - 640x480, 16 colours", bar);
		console_set_region(1, VGA_ROWS - 1);
		break;
	case DE_TILING:
	default:
		bar = VGA_ATTR(VGA_BLACK, VGA_GREEN);
		body = VGA_ATTR(VGA_LGREEN, VGA_BLACK);
		console_set_attr(body);
		console_clear();
		vga_fill_row(0, ' ', bar);
		vga_write_at(0, 0, " [1:msh] 2 3 4", bar);
		vga_write_at(0, VGA_COLS - (int)strlen(status) - 1, status, bar);
		console_set_region(1, VGA_ROWS - 1);
		break;
	}

report:
	kprintf("[de] session: %s (%s), budget %u MiB\n", p->title, p->style, p->resident_mib);
	kprintf("[de] compositor: %s; rendering: %s\n", p->compositor, gpu ? "GPU" : "software");
}
