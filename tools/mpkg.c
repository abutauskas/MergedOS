/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * mpkg - host build of the MergedOS package translation layer.
 *
 *   mpkg inspect <package file | Foo.app directory>
 *   mpkg extract <package file> <output file>     write the package's program
 *
 * It links exactly the code the kernel links (pkg_translate/), which makes
 * it a convenient way to try real-world packages against the translators.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "pkg_translate/pkg.h"

static u8 scratch_buf[64 * 1024 * 1024];
static struct pkg_scratch scratch = { scratch_buf, sizeof(scratch_buf) };

/* The portable code is linked whole; these are its only external references. */
long msys_write(struct task *t, int fd, u64 ubuf, u64 len) { (void)t; (void)fd; (void)ubuf; (void)len; return -MERR_NOSYS; }
long msys_getpid(struct task *t) { (void)t; return 0; }
void msys_exit(struct task *t, int status) { (void)t; (void)status; }
int uaccess_copy_in(struct task *t, void *d, u64 a, size_t n) { (void)t; (void)d; (void)a; (void)n; return -MERR_FAULT; }
int uaccess_copy_out(struct task *t, u64 a, const void *s, size_t n) { (void)t; (void)a; (void)s; (void)n; return -MERR_FAULT; }

static u8 *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	long size;
	u8 *buf;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc((size_t)size + 1);
	if (buf && fread(buf, 1, (size_t)size, f) != (size_t)size) {
		free(buf);
		buf = NULL;
	}
	fclose(f);
	*len = (size_t)size;
	return buf;
}

static const char *explain(int rc)
{
	switch (-rc) {
	case MERR_NOEXEC: return "not a package this tool recognises, or it is damaged";
	case MERR_NOTSUP: return "recognised, but it needs a codec or backend that is not built in";
	case MERR_RANGE:  return "payload larger than the decode buffer";
	case MERR_NOENT:  return "no program found in the payload";
	default:          return "failed";
	}
}

static int translate(const char *path, struct pkg_manifest *m, u8 **img, size_t *len)
{
	struct stat st;
	int rc;

	if (stat(path, &st)) {
		perror(path);
		return -MERR_NOENT;
	}
	if (S_ISDIR(st.st_mode)) {                      /* a .app bundle */
		char sub[4096];
		size_t plist_len, exe_len = 0;
		u8 *plist, *exe = NULL;

		snprintf(sub, sizeof(sub), "%s/Contents/Info.plist", path);
		plist = slurp(sub, &plist_len);
		if (!plist) {
			fprintf(stderr, "%s: not a bundle (no Contents/Info.plist)\n", path);
			return -MERR_NOENT;
		}
		rc = pkg_translate_app(plist, plist_len, NULL, 0, m);
		if (!rc) {
			snprintf(sub, sizeof(sub), "%s/%s", path, m->executable);
			exe = slurp(sub, &exe_len);
			if (exe)
				rc = pkg_translate_app(plist, plist_len, exe, exe_len, m);
		}
		free(exe);
		free(plist);
		*img = NULL;
		return rc;
	}
	*img = slurp(path, len);
	if (!*img) {
		perror(path);
		return -MERR_NOENT;
	}
	return pkg_translate(*img, *len, path, &scratch, m);
}

int main(int argc, char **argv)
{
	static struct pkg_manifest m;
	static char text[8192];
	u8 *img = NULL;
	size_t len = 0;
	int rc;

	if (argc < 3 || (strcmp(argv[1], "inspect") && strcmp(argv[1], "extract")) ||
	    (!strcmp(argv[1], "extract") && argc != 4)) {
		fprintf(stderr, "usage: mpkg inspect <package|Foo.app>\n"
				"       mpkg extract <package> <output file>\n");
		return 2;
	}

	rc = translate(argv[2], &m, &img, &len);
	if (rc && m.format == PKG_UNKNOWN) {
		fprintf(stderr, "%s: %s\n", argv[2], explain(rc));
		return 1;
	}
	if (rc) {
		fprintf(stderr, "%s: %s [%s; metadata %s, payload %s]\n", argv[2], explain(rc),
			pkg_format_name(m.format), pkg_codec_name(m.meta_codec),
			pkg_codec_name(m.payload_codec));
		return 1;
	}

	if (!strcmp(argv[1], "inspect")) {
		pkg_manifest_print(&m, text, sizeof(text));
		printf("%s:\n%s", argv[2], text);
	} else {
		const u8 *exe;
		size_t exe_len;
		char path[256];
		FILE *out;

		rc = img ? pkg_payload_exec(img, len, &m, &scratch, &exe, &exe_len, path, sizeof(path))
			 : -MERR_NOTSUP;
		if (rc) {
			fprintf(stderr, "%s: %s\n", argv[2], explain(rc));
			return 1;
		}
		out = fopen(argv[3], "wb");
		if (!out || fwrite(exe, 1, exe_len, out) != exe_len) {
			perror(argv[3]);
			return 1;
		}
		fclose(out);
		chmod(argv[3], 0755);
		printf("%s: %s (%zu bytes) -> %s\n", argv[2], path, exe_len, argv[3]);
	}
	free(img);
	return 0;
}
