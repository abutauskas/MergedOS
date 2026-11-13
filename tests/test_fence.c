/* SPDX-License-Identifier: BSD-2-Clause */
/* .merged package format and the Mergedfence blocking engine. */
#include "tests/test.h"
#include "pkg_translate/pkg.h"
#include "installer/sha256.h"
#include "mergedfence/fence.h"

static struct fence fence;              /* ~33 KiB: keep it off the stack */

/* Build a .merged image in `out` (heap, exact size). */
static u8 *build_merged(const char *manifest, const char *payload, size_t *len)
{
	size_t ml = strlen(manifest), pl = strlen(payload);
	size_t body = MERGED_HDR_LEN + ml + pl;
	u8 *img = malloc(body + MERGED_HASH_LEN);
	struct sha256 h;

	memcpy(img, "MRGD", 4);
	for (int i = 0; i < 4; i++) {
		img[4 + i] = (u8)(MERGED_VERSION >> (8 * i));
		img[8 + i] = (u8)(ml >> (8 * i));
		img[12 + i] = (u8)(pl >> (8 * i));
	}
	memcpy(img + MERGED_HDR_LEN, manifest, ml);
	memcpy(img + MERGED_HDR_LEN + ml, payload, pl);
	sha256_init(&h);
	sha256_update(&h, img, body);
	sha256_final(&h, img + body);
	*len = body + MERGED_HASH_LEN;
	return img;
}

static int parse_built(const char *manifest, const char *payload, struct merged_pkg *p)
{
	size_t len;
	u8 *img = build_merged(manifest, payload, &len);
	int rc = merged_parse(img, len, p);

	free(img);
	return rc;
}

static void test_format(void)
{
	static u8 scratch_buf[4096];
	struct pkg_scratch scratch = { scratch_buf, sizeof(scratch_buf) };
	struct pkg_manifest m;
	struct merged_pkg p;
	char text[1024];
	size_t len;
	u8 *img = load_fixture("etc/mergedfence/default.merged", &len);

	/* Round trip of the shipped list. */
	CHECK(merged_probe(img, len));
	CHECK_EQ(pkg_probe(img, len, NULL), PKG_MERGED);
	CHECK_EQ(merged_parse(img, len, &p), 0);
	CHECK_STR(p.name, "mergedfence-default");
	CHECK_STR(p.version, "1.0");
	CHECK_EQ(p.type, MERGED_TYPE_BLOCKLIST);
	CHECK(strstr(p.summary, "fictional") != NULL);
	CHECK(p.payload > img && p.payload + p.payload_len + MERGED_HASH_LEN == img + len);
	CHECK(mos_memstr((const char *)p.payload, p.payload_len, "||tracker.example^") != NULL);

	CHECK_EQ(pkg_translate(img, len, "default.merged", &scratch, &m), 0);
	CHECK_EQ(m.format, PKG_MERGED);
	CHECK_STR(m.store_prefix, "/pkg/merged/mergedfence-default/1.0");
	CHECK_EQ(m.payload_size, p.payload_len);
	pkg_manifest_print(&m, text, sizeof(text));
	CHECK(strstr(text, "merged (MergedOS native)") != NULL);
	CHECK(strstr(text, "personality  native") != NULL);
	CHECK(strstr(text, "[blocklist]") != NULL);

	/* Every truncation, in an exactly sized buffer so ASan sees over-reads. */
	for (size_t n = 0; n < len; n++) {
		u8 *cut = malloc(n ? n : 1);

		memcpy(cut, img, n);
		CHECK(merged_parse(cut, n, &p) != 0);
		free(cut);
	}
	/* Trailing garbage. */
	{
		u8 *big = malloc(len + 1);

		memcpy(big, img, len);
		big[len] = 0;
		CHECK(merged_parse(big, len + 1, &p) != 0);
		free(big);
	}
	/* A flipped byte anywhere: header, manifest, payload or hash. */
	for (size_t i = 0; i < len; i++) {
		img[i] ^= 0x01;
		CHECK(merged_parse(img, len, &p) != 0);
		img[i] ^= 0x01;
	}
	CHECK_EQ(merged_parse(img, len, &p), 0);
	free(img);

	/* Valid hashes, bad manifests. */
	CHECK_EQ(parse_built("name: a\nversion: 1\ntype: app\n", "", &p), 0);
	CHECK_EQ(p.type, MERGED_TYPE_APP);
	CHECK_EQ(p.payload_len, 0);
	CHECK_EQ(parse_built("name: a\r\nversion: 1\r\ntype: blocklist", "x", &p), 0);
	CHECK_EQ(parse_built("version: 1\ntype: app\n", "", &p), -MERR_NOEXEC);
	CHECK_EQ(parse_built("name: a\ntype: app\n", "", &p), -MERR_NOEXEC);
	CHECK_EQ(parse_built("name: a\nversion: 1\n", "", &p), -MERR_NOEXEC);
	CHECK_EQ(parse_built("name: a\nversion: 1\ntype: driver\n", "", &p), -MERR_NOEXEC);
	CHECK_EQ(parse_built("name: a\nversion: 1\ntype: \n", "", &p), -MERR_NOEXEC);
	CHECK_EQ(parse_built("name: a\nname: b\nversion: 1\ntype: app\n", "", &p), -MERR_NOEXEC);
	CHECK_EQ(parse_built("name: ../x\nversion: 1\ntype: app\n", "", &p), -MERR_NOEXEC);
	CHECK_EQ(parse_built("name: a/b\nversion: 1\ntype: app\n", "", &p), -MERR_NOEXEC);
	CHECK_EQ(parse_built("garbage line\nname: a\nversion: 1\ntype: app\n", "", &p),
		 -MERR_NOEXEC);
	CHECK_EQ(parse_built("name: a\nversion: 1\ntype: app\nlicense: BSD\n", "", &p), 0);

	/* Header limits. */
	{
		u8 *b = build_merged("name: a\nversion: 1\ntype: app\n", "", &len);

		b[4] = 2;                       /* future format version */
		CHECK_EQ(merged_parse(b, len, &p), -MERR_NOTSUP);
		b[4] = 1;
		b[11] = 0x7F;                   /* manifest length far too large */
		CHECK_EQ(merged_parse(b, len, &p), -MERR_RANGE);
		b[11] = 0;
		b[15] = 0xFF;                   /* payload length far too large */
		CHECK_EQ(merged_parse(b, len, &p), -MERR_RANGE);
		free(b);
	}
	CHECK(!merged_probe((const u8 *)"MRG", 3));
	CHECK(merged_parse(NULL, 0, &p) != 0);
}

static const char *load_str(const char *txt)
{
	fence_init(&fence);
	CHECK(fence_load(&fence, txt, strlen(txt)) >= 0);
	return txt;
}

static void test_rules(void)
{
	const struct fence_stats *st;

	/* Each syntax. */
	load_str("# comment\n"
		 "! adblock comment\n"
		 "\n"
		 "   \t\n"
		 "0.0.0.0 hosts-zero.example\n"
		 "127.0.0.1\thosts-loop.example   # trailing note\n"
		 "0.0.0.0 multi-a.example multi-b.example\n"
		 "127.0.0.1 localhost\n"
		 "||adblock.example^\n"
		 "@@||ok.adblock.example^\n"
		 "example.com##.banner\n"
		 "||opts.example^$third-party\n"
		 "/ads/*\n"
		 "||bad_dom ain^\n"
		 "||^\n"
		 "0.0.0.0\n");
	st = fence_stats(&fence);
	CHECK_EQ(st->rules, 6);
	CHECK_EQ(st->exceptions, 1);
	CHECK_EQ(st->ignored, 7);
	CHECK_EQ(fence_check(&fence, "hosts-zero.example"), FENCE_BLOCKED);
	CHECK_EQ(fence_check(&fence, "hosts-loop.example"), FENCE_BLOCKED);
	CHECK_EQ(fence_check(&fence, "multi-a.example"), FENCE_BLOCKED);
	CHECK_EQ(fence_check(&fence, "multi-b.example"), FENCE_BLOCKED);
	CHECK_EQ(fence_check(&fence, "localhost"), FENCE_ALLOWED);
	CHECK_EQ(fence_check(&fence, "adblock.example"), FENCE_BLOCKED);
	CHECK_EQ(fence_check(&fence, "opts.example"), FENCE_ALLOWED);
	CHECK_EQ(fence_check(&fence, "ok.adblock.example"), FENCE_EXCEPTED);

	/* Suffix matching on label boundaries only. */
	load_str("||example.com^\n");
	CHECK_EQ(fence_check(&fence, "example.com"), FENCE_BLOCKED);
	CHECK_EQ(fence_check(&fence, "ads.example.com"), FENCE_BLOCKED);
	CHECK_EQ(fence_check(&fence, "a.b.c.example.com"), FENCE_BLOCKED);
	CHECK_EQ(fence_check(&fence, "example.com."), FENCE_BLOCKED);
	CHECK_EQ(fence_check(&fence, "badexample.com"), FENCE_ALLOWED);
	CHECK_EQ(fence_check(&fence, "example.com.evil"), FENCE_ALLOWED);
	CHECK_EQ(fence_check(&fence, "xample.com"), FENCE_ALLOWED);
	CHECK_EQ(fence_check(&fence, "com"), FENCE_ALLOWED);
	CHECK_EQ(fence_check(&fence, ""), FENCE_ALLOWED);
	CHECK_EQ(fence_check(&fence, NULL), FENCE_ALLOWED);

	/* Case-insensitive, both in rules and in queries. */
	load_str("||Tracker.EXAMPLE^\n");
	CHECK_EQ(fence_check(&fence, "tracker.example"), FENCE_BLOCKED);
	CHECK_EQ(fence_check(&fence, "CDN.TRACKER.Example"), FENCE_BLOCKED);

	/* Exceptions win, in either order, and only where a block applies. */
	load_str("@@||help.tracker.example^\n||tracker.example^\n");
	CHECK_EQ(fence_check(&fence, "help.tracker.example"), FENCE_EXCEPTED);
	CHECK_EQ(fence_check(&fence, "x.help.tracker.example"), FENCE_EXCEPTED);
	CHECK_EQ(fence_check(&fence, "tracker.example"), FENCE_BLOCKED);
	CHECK_EQ(fence_check(&fence, "unrelated.example"), FENCE_ALLOWED);
	st = fence_stats(&fence);
	CHECK_EQ(st->checks, 4);
	CHECK_EQ(st->blocked, 1);
	CHECK_EQ(st->excepted, 2);

	/* Disabled engine allows everything. */
	fence_enable(&fence, false);
	CHECK_EQ(fence_check(&fence, "tracker.example"), FENCE_ALLOWED);
	fence_enable(&fence, true);
	CHECK_EQ(fence_check(&fence, "tracker.example"), FENCE_BLOCKED);

	/* Duplicates do not take table slots. */
	load_str("||dup.example^\n0.0.0.0 dup.example\n||DUP.example^\n");
	CHECK_EQ(fence_stats(&fence)->rules, 1);

	/* Over-long names are ignored, not truncated into a different rule. */
	load_str("||aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.example^\n");
	CHECK_EQ(fence_stats(&fence)->rules, 0);
	CHECK_EQ(fence_stats(&fence)->ignored, 1);
}

static void test_capacity(void)
{
	static char big[(FENCE_MAX_RULES + 10) * 32];
	size_t n = 0;

	for (int i = 0; i < FENCE_MAX_RULES + 10; i++)
		n += (size_t)snprintf(big + n, sizeof(big) - n, "||host%d.example^\n", i);
	fence_init(&fence);
	CHECK_EQ(fence_load(&fence, big, n), -MERR_RANGE);
	CHECK_EQ(fence.nrules, FENCE_MAX_RULES);
	CHECK_EQ(fence_stats(&fence)->dropped, 10);
	CHECK_EQ(fence_check(&fence, "host0.example"), FENCE_BLOCKED);
	CHECK_EQ(fence_check(&fence, "host511.example"), FENCE_BLOCKED);
	CHECK_EQ(fence_check(&fence, "host512.example"), FENCE_ALLOWED);
	CHECK_EQ(fence_load(&fence, NULL, 0), -MERR_INVAL);
}

/* The shipped list, end to end: .merged -> engine. */
static void test_default_list(void)
{
	struct merged_pkg p;
	size_t len;
	u8 *img = load_fixture("etc/mergedfence/default.merged", &len);

	CHECK_EQ(merged_parse(img, len, &p), 0);
	fence_init(&fence);
	CHECK(fence_load(&fence, (const char *)p.payload, p.payload_len) >= 20);
	CHECK_EQ(fence_stats(&fence)->exceptions, 1);
	CHECK_EQ(fence_stats(&fence)->ignored, 0);
	CHECK_EQ(fence_check(&fence, "tracker.example"), FENCE_BLOCKED);
	CHECK_EQ(fence_check(&fence, "cdn.pixel.example"), FENCE_BLOCKED);
	CHECK_EQ(fence_check(&fence, "malvert.invalid"), FENCE_BLOCKED);
	CHECK_EQ(fence_check(&fence, "help.tracker.example"), FENCE_EXCEPTED);
	CHECK_EQ(fence_check(&fence, "mergedos.example"), FENCE_ALLOWED);
	free(img);
}

void test_fence(void)
{
	test_format();
	test_rules();
	test_capacity();
	test_default_list();
}
