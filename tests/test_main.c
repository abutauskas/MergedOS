/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Host-side unit tests for the portable parts of MergedOS. Built with
 * AddressSanitizer + UBSan, so the corruption/truncation loops double as a
 * memory-safety check on every parser that touches untrusted bytes.
 */
#include "tests/test.h"
#include "abi_dispatch/abi.h"
#include "ui/ui.h"

int test_checks, test_failures;

u8 *load_fixture_opt(const char *rel, size_t *len)
{
	char path[512];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", FIXTURE_DIR, rel);
	f = fopen(path, "rb");
	if (!f)
		return NULL;
	fclose(f);
	return load_fixture(rel, len);
}

u8 *load_fixture(const char *rel, size_t *len)
{
	char path[512];
	FILE *f;
	long size;
	u8 *buf;

	snprintf(path, sizeof(path), "%s/%s", FIXTURE_DIR, rel);
	f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "missing fixture %s (run `make test-host`)\n", path);
		exit(2);
	}
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc((size_t)size + 1);
	if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
		fprintf(stderr, "cannot read %s\n", path);
		exit(2);
	}
	fclose(f);
	*len = (size_t)size;
	return buf;
}

/* ---- mock native layer: the link seam abi_dispatch/abi.h describes --------- */

static bool mock_range_ok(struct task *t, u64 addr, size_t len)
{
	return addr >= t->base && len <= MOCK_MEM_SIZE && addr - t->base <= MOCK_MEM_SIZE - len;
}

int uaccess_copy_in(struct task *t, void *dst, u64 uaddr, size_t len)
{
	if (!mock_range_ok(t, uaddr, len))
		return -MERR_FAULT;
	memcpy(dst, t->mem + (uaddr - t->base), len);
	return 0;
}

int uaccess_copy_out(struct task *t, u64 uaddr, const void *src, size_t len)
{
	if (!mock_range_ok(t, uaddr, len))
		return -MERR_FAULT;
	memcpy(t->mem + (uaddr - t->base), src, len);
	return 0;
}

long msys_write(struct task *t, int fd, u64 ubuf, u64 len)
{
	if (fd != 1 && fd != 2)
		return -MERR_BADF;
	if (len > sizeof(t->out) - t->out_len)
		return -MERR_RANGE;
	if (uaccess_copy_in(t, t->out + t->out_len, ubuf, (size_t)len))
		return -MERR_FAULT;
	t->out_len += (size_t)len;
	return (long)len;
}

long msys_getpid(struct task *t)
{
	return t->pid;
}

void msys_exit(struct task *t, int status)
{
	t->exited = true;
	t->exit_code = status;
}

/* ---- recording draw back end for ui/ ---------------------------------------- */

char mock_draw_log[8192];
size_t mock_draw_len;
int mock_fill_count, mock_blend_count;
int mock_x0, mock_y0, mock_x1, mock_y1;

void mock_bounds_reset(void)
{
	mock_x0 = mock_y0 = 1 << 30;
	mock_x1 = mock_y1 = -(1 << 30);
}

/* Grow the bounding box of everything drawn since the last reset. */
static void mock_touch(int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0)
		return;
	mock_x0 = x < mock_x0 ? x : mock_x0;
	mock_y0 = y < mock_y0 ? y : mock_y0;
	mock_x1 = x + w > mock_x1 ? x + w : mock_x1;
	mock_y1 = y + h > mock_y1 ? y + h : mock_y1;
}

void ui_draw_blend(int x, int y, u32 rgb, u8 alpha)
{
	(void)rgb;
	mock_blend_count += alpha != 0;
	if (alpha)
		mock_touch(x, y, 1, 1);
}

void ui_draw_fill(int x, int y, int w, int h, u32 rgb)
{
	(void)rgb;
	mock_fill_count++;
	mock_touch(x, y, w, h);
}

void ui_draw_text(int x, int y, const char *s, u32 rgb, int scale)
{
	size_t n = strlen(s);

	(void)rgb;
	/* The raster of a character: 6 x 10 dots, starting two dot rows above y. */
	mock_touch(x, y - 2 * scale, ui_text_cells(s) * UI_GLYPH_W * scale,
		   UI_RASTER_ROWS * scale);
	if (mock_draw_len + n + 2 > sizeof(mock_draw_log))
		mock_draw_len = 0;                      /* wrap: only recent text matters */
	memcpy(mock_draw_log + mock_draw_len, s, n);
	mock_draw_len += n;
	mock_draw_log[mock_draw_len++] = '\n';
	mock_draw_log[mock_draw_len] = '\0';
}

int main(void)
{
	static const struct {
		const char *name;
		void (*fn)(void);
	} suites[] = {
		{ "binfmt (ELF / PE / Mach-O probes and load plans)", test_binfmt },
		{ "PE32+ imports + base relocations (NT loader input)", test_pe },
		{ "abi dispatch (Linux / FreeBSD / XNU / NT)", test_dispatch },
		{ "desktop selector policy", test_de_policy },
		{ "msh (aliases, tokenizer, paths)", test_msh },
		{ "xz + zstd decoders (vectors, prefixes, fuzz)", test_codec },
		{ "squashfs (4 compressors, fragments, walk, fuzz)", test_squashfs },
		{ "package translation (deb / rpm / apk / pacman / .app)", test_pkg },
		{ "ui toolkit + setup wizard (hit-test, focus, i18n, hashing)", test_ui },
		{ "storage (MBR/GPT scan, safe allocation, config sector)", test_storage },
		{ "desktop window manager (4 layouts, z-order, tiling, clip)", test_desktop },
		{ "mergedfence + .merged (format, rules, matching)", test_fence },
		{ "file types + viewers (identify, PNG/BMP, zip/tar, fuzz)", test_filetype },
		{ "ramfs + process system calls (Linux / FreeBSD routing)", test_ramfs },
		{ "wayland (wire codec, compositor, socketpair client, fuzz)", test_wayland },
		{ "Darwin: XNU calls, Mach-O dynamic plans, imports, .app report", test_darwin },
		{ "network stack (ARP/IP/UDP/TCP/DHCP, lossy wire, fuzz)", test_net },
	};

	for (size_t i = 0; i < ARRAY_SIZE(suites); i++) {
		int before = test_failures, checks = test_checks;

		suites[i].fn();
		printf("%-58s %4d checks  %s\n", suites[i].name, test_checks - checks,
		       test_failures == before ? "ok" : "FAILED");
	}
	printf("%d checks, %d failures\n", test_checks, test_failures);
	return test_failures ? 1 : 0;
}
