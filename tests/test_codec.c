/* SPDX-License-Identifier: BSD-2-Clause */
/* xz / zstd decoders and the squashfs reader against vectors from the host tools. */
#include "tests/test.h"
#include "pkg_translate/pkg_internal.h"
#include "pkg_translate/squashfs.h"

typedef int (*decoder)(const u8 *, size_t, u8 *, size_t, size_t *, size_t *, void *, size_t);

static u8 work[XZ_WORK_SIZE] __attribute__((aligned(8)));
static u8 out[512 * 1024];

/* zstd -19 --check of 20 verses of "N bottles of beer" (1040 bytes): FSE + Huffman. */
static const u8 beer_zst[] = {
	0x28, 0xb5, 0x2f, 0xfd, 0x64, 0x10, 0x03, 0x1d, 0x03, 0x00, 0xb2, 0x84,
	0x0f, 0x11, 0xa0, 0x3d, 0xb0, 0xf2, 0x68, 0xeb, 0x53, 0xd5, 0x41, 0x44,
	0x4f, 0x96, 0xc2, 0x86, 0x3f, 0x9b, 0x30, 0xbb, 0xcc, 0xdd, 0x63, 0x10,
	0x52, 0x8a, 0x31, 0xe7, 0xb8, 0xbf, 0xbf, 0xef, 0x32, 0x77, 0x8f, 0x41,
	0x48, 0x29, 0xc6, 0x9c, 0xe3, 0xfe, 0x40, 0x68, 0x68, 0x0d, 0x4d, 0x55,
	0x4c, 0xaa, 0x7b, 0x0a, 0x95, 0x4a, 0xba, 0x9e, 0x46, 0xd5, 0xa4, 0x7c,
	0x49, 0xff, 0x03, 0x27, 0xa8, 0x11, 0xa0, 0x33, 0xd6, 0xff, 0x0d, 0xf0,
	0x35, 0x03, 0x11, 0xfc, 0x4b, 0x74, 0xe1, 0x67, 0xfc, 0xa4, 0x56, 0x4d,
	0x62, 0x0c, 0xa0, 0xf4, 0xf4, 0x19, 0x70, 0x33, 0x7f, 0x0c, 0x20, 0xa9,
	0x01, 0xba, 0xb9, 0x59, 0xdb,
};

/*
 * Decode, decode into a short buffer (valid prefix), then truncate and flip
 * bits. With a content check (`checked`), damage must never decode to
 * different bytes with success.
 */
static void vector(decoder fn, const u8 *z, size_t zl, const u8 *raw, size_t rl, bool checked,
		   bool concat)
{
	size_t c = 0, p = 0, step = rl < 4096 ? 1 : zl / 300 + 1;
	u8 *copy = malloc(zl + 1);
	int rc;

	CHECK_EQ(fn(z, zl, out, sizeof(out), &c, &p, work, sizeof(work)), 0);
	CHECK_EQ(p, rl);
	CHECK_EQ(c, zl);
	CHECK(p == rl && !memcmp(out, raw, rl));
	if (rl > 3) {
		CHECK_EQ(fn(z, zl, out, rl / 3, &c, &p, work, sizeof(work)), 1);
		CHECK(p == rl / 3 && !memcmp(out, raw, rl / 3));
	}
	CHECK_EQ(fn(z, zl, out, sizeof(out), &c, &p, work, 64), -MERR_NOMEM);
	for (size_t n = 0; n < zl; n += step) {
		memcpy(copy, z, n);
		rc = fn(copy, n, out, sizeof(out), &c, &p, work, sizeof(work));
		CHECK(rc != 0 || concat);
	}
	for (size_t i = 0; i < zl; i += step) {
		memcpy(copy, z, zl);
		copy[i] ^= (u8)(1u << (i % 8));
		rc = fn(copy, zl, out, sizeof(out), &c, &p, work, sizeof(work));
		if (checked && rc == 0 && (p != rl || memcmp(out, raw, rl))) {
			printf("  undetected corruption at byte %zu\n", i);
			CHECK(0);
		}
	}
	free(copy);
}

static void vector_files(decoder fn, const char *zname, const char *rawname, bool checked)
{
	size_t zl, rl;
	u8 *z = load_fixture_opt(zname, &zl), *raw;

	if (!z) {
		printf("  (skipped %s: host tool missing)\n", zname);
		return;
	}
	raw = load_fixture(rawname, &rl);
	vector(fn, z, zl, raw, rl, checked, strstr(zname, "-tiny") != NULL);
	free(z);
	free(raw);
}

void test_codec(void)
{
	static const char *const names[] = { "text", "elf", "zeros", "tiny", "random", "mixed" };
	char a[64], b[64];
	size_t c, p;

	for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
		snprintf(b, sizeof(b), "codec/%s.bin", names[i]);
		snprintf(a, sizeof(a), "codec/%s.xz", names[i]);
		vector_files(xz_decode, a, b, true);
		snprintf(a, sizeof(a), "codec/%s.zst", names[i]);
		vector_files(zstd_decode, a, b, true);
	}
	vector_files(xz_decode, "codec/text.crc32.xz", "codec/text.bin", true);
	vector_files(xz_decode, "codec/text.sha256.xz", "codec/text.bin", true);
	vector_files(xz_decode, "codec/text.none.xz", "codec/text.bin", false);
	vector_files(xz_decode, "codec/elf.lc4.xz", "codec/elf.bin", true);
	vector_files(xz_decode, "codec/text-tiny.xz", "codec/text-tiny.bin", true);
	vector_files(zstd_decode, "codec/text-tiny.zst", "codec/text-tiny.bin", true);
	vector_files(zstd_decode, "codec/mixed.fast.zst", "codec/mixed.bin", false);
	vector_files(zstd_decode, "codec/mixed.ultra.zst", "codec/mixed.bin", true);
	vector_files(zstd_decode, "codec/mixed.fastest.zst", "codec/mixed.bin", true);

	/* Embedded vector: runs without the zstd tool. */
	{
		char beer[1100];
		size_t n = 0;

		for (int i = 99; i > 79; i--)
			n += (size_t)snprintf(beer + n, sizeof(beer) - n,
					      "%d bottles of beer on the wall, %d bottles of beer.\n",
					      i, i);
		CHECK_EQ(n, 1040);
		vector(zstd_decode, beer_zst, sizeof(beer_zst), (const u8 *)beer, n, true, false);
	}
	/* XXH64 reference values (seed 0). */
	CHECK(pkg_xxh64((const u8 *)"", 0) == 0xEF46DB3751D8E999ull);
	CHECK(pkg_xxh64((const u8 *)"a", 1) == 0xD24EC4F1A98C6E5Bull);
	/* A dictionary-compressed frame is reported as unsupported, not corrupt. */
	{
		static const u8 dict_frame[] = { 0x28, 0xb5, 0x2f, 0xfd, 0x21, 0x01, 0x05,
						 0x01, 0x00, 0x00, 0x41 };

		CHECK_EQ(zstd_decode(dict_frame, sizeof(dict_frame), out, sizeof(out), &c, &p, work,
				     sizeof(work)), -MERR_NOTSUP);
	}
	/* Not a stream at all. */
	CHECK(xz_decode((const u8 *)"hello", 5, out, sizeof(out), &c, &p, work, sizeof(work)) < 0);
	CHECK(zstd_decode((const u8 *)"hello", 5, out, sizeof(out), &c, &p, work, sizeof(work)) < 0);
}

/* ---- squashfs ---------------------------------------------------------------- */

static u8 sq_work[64 * 1024] __attribute__((aligned(8)));

struct walk_stats {
	int files, dirs, links;
	size_t bytes;
};

static int count_cb(const char *path, const struct sqfs_inode *ino, void *arg)
{
	struct walk_stats *st = arg;

	(void)path;
	st->files += ino->kind == SQFS_FILE;
	st->dirs += ino->kind == SQFS_DIR;
	st->links += ino->kind == SQFS_SYMLINK;
	return 0;
}

static struct sqfs *fuzz_fs;
static int read_cb(const char *path, const struct sqfs_inode *ino, void *arg)
{
	static u8 buf[32 * 1024];
	size_t got;

	(void)arg;
	/* Whole files outside many/, prefixes inside: keeps the fuzz loop quick. */
	if (ino->kind == SQFS_FILE)
		(void)sqfs_read(fuzz_fs, ino, buf, strncmp(path, "many/", 5) ? sizeof(buf) : 2,
				&got);
	return 0;
}

static void sq_torture(const u8 *img, size_t len, size_t step)
{
	u8 *copy = malloc(len);
	struct sqfs fs;

	fuzz_fs = &fs;
	for (size_t n = 0; n < len; n += step * 13 + 1) {
		memcpy(copy, img, n);
		if (!sqfs_open(&fs, copy, n, sq_work, sizeof(sq_work)))
			(void)sqfs_walk(&fs, read_cb, NULL);
	}
	for (size_t i = 0; i < len; i += step) {
		memcpy(copy, img, len);
		copy[i] ^= (u8)(1u << (i % 8));
		if (!sqfs_open(&fs, copy, len, sq_work, sizeof(sq_work)))
			(void)sqfs_walk(&fs, read_cb, NULL);
	}
	free(copy);
}

static void check_file(struct sqfs *fs, const char *path)
{
	struct sqfs_inode ino;
	char name[96];
	size_t len, got = 0;
	u8 *ref;

	snprintf(name, sizeof(name), "sq/src/%s", path);
	ref = load_fixture(name, &len);
	CHECK_EQ(sqfs_lookup(fs, path, &ino), 0);
	CHECK_EQ(ino.kind, SQFS_FILE);
	CHECK_EQ(ino.size, len);
	CHECK_EQ(sqfs_read(fs, &ino, out, sizeof(out), &got), 0);
	CHECK(got == len && !memcmp(out, ref, len));
	if (got != len)
		printf("  %s: got %zu of %zu\n", path, got, len);
	if (len > 10 && fs->tmp_len > 4096) {               /* a short buffer: valid prefix */
		size_t cut = len > 5000 ? 5000 : len - 3;

		CHECK_EQ(sqfs_read(fs, &ino, out, cut, &got), 1);
		CHECK(got == cut && !memcmp(out, ref, cut));
	}
	free(ref);
}

void test_squashfs(void)
{
	static const char *const comps[] = { "gzip", "xz", "zstd", "lz4", "raw" };

	for (size_t i = 0; i < ARRAY_SIZE(comps); i++) {
		struct walk_stats st = { 0 };
		struct sqfs fs;
		struct sqfs_inode ino;
		char name[64];
		size_t len;
		u8 *img;

		snprintf(name, sizeof(name), "sq/%s.sqfs", comps[i]);
		img = load_fixture_opt(name, &len);
		if (!img) {
			printf("  (skipped %s: mksquashfs missing)\n", name);
			return;
		}
		CHECK_EQ(sqfs_find(img, len), 0);
		CHECK_EQ(sqfs_open(&fs, img, len, sq_work, 100), -MERR_NOMEM);
		/* Minimal work area: no spare buffer, fragments decode in place. */
		CHECK_EQ(sqfs_open(&fs, img, len, sq_work, sqfs_work_size(img, len)), 0);
		CHECK_EQ(fs.block_size, 4096);
		for (int pass = 0; pass < 2; pass++) {
			if (pass)
				CHECK_EQ(sqfs_open(&fs, img, len, sq_work, sizeof(sq_work)), 0);
			check_file(&fs, "usr/bin/hello");
			check_file(&fs, "big.bin");             /* blocks + fragment */
			check_file(&fs, "README");
			check_file(&fs, "empty");
			check_file(&fs, "zeros.bin");           /* sparse blocks */
			check_file(&fs, "a/b/c/d/deep.txt");
			check_file(&fs, "many/f299");           /* past the first dir header */
		}
		CHECK_EQ(sqfs_lookup(&fs, "link", &ino), 0);
		CHECK_EQ(ino.kind, SQFS_SYMLINK);
		CHECK_EQ(sqfs_readlink(&fs, &ino, name, sizeof(name)), 13);
		CHECK_STR(name, "usr/bin/hello");
		CHECK_EQ(sqfs_lookup(&fs, "many/f300", &ino), -MERR_NOENT);
		CHECK_EQ(sqfs_lookup(&fs, "README/x", &ino), -MERR_INVAL);
		CHECK_EQ(sqfs_walk(&fs, count_cb, &st), 0);
		CHECK_EQ(st.files, 306);
		CHECK_EQ(st.dirs, 7);
		CHECK_EQ(st.links, 1);
		/* With a spare buffer, a fragment tail fits even when `out` is tight. */
		CHECK_EQ(sqfs_open(&fs, img, len, sq_work, sizeof(sq_work)), 0);
		CHECK_EQ(sqfs_lookup(&fs, "README", &ino), 0);
		{
			size_t got;

			CHECK_EQ(sqfs_read(&fs, &ino, out, 6, &got), 1);
			CHECK(got == 6 && !memcmp(out, "squash", 6));
		}
		if (i < 3)
			sq_torture(img, len, len / 200 + 1);
		free(img);
	}
}
