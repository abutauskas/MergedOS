/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_PKG_INTERNAL_H
#define MERGEDOS_PKG_INTERNAL_H

#include "pkg_translate/pkg.h"
#include "pkg_translate/tar.h"

/* ---- inflate.c ------------------------------------------------------------
 * Decode one gzip member. Returns 0 when the member decoded completely (CRC
 * verified), 1 when `out` filled up first (the prefix in `out` is valid), or
 * -MERR_INVAL on a corrupt stream. `consumed` is only set on full success.
 */
int gz_decode(const u8 *in, size_t inlen, u8 *out, size_t outcap, size_t *consumed,
	      size_t *produced);
/* Same results for a bare DEFLATE stream and for a zlib (RFC 1950) stream,
 * whose Adler-32 is verified. */
int inflate_raw(const u8 *in, size_t inlen, u8 *out, size_t outcap, size_t *consumed,
		size_t *produced);
int zlib_decode(const u8 *in, size_t inlen, u8 *out, size_t outcap, size_t *consumed,
		size_t *produced);
u32 pkg_crc32(u32 crc, const u8 *p, size_t n);  /* CRC-32 (IEEE), start with 0 */

/* ---- xz.c, zstd.c -----------------------------------------------------------
 * Same results as gz_decode; all concatenated streams/frames are decoded.
 * `work` is caller memory of at least XZ_WORK_SIZE / ZSTD_WORK_SIZE bytes
 * (8-byte aligned). -MERR_NOTSUP: a valid stream using a feature left out
 * (xz BCJ/delta filters, zstd dictionaries).
 */
#define XZ_WORK_SIZE   28288u
#define ZSTD_WORK_SIZE 9232u
int xz_decode(const u8 *in, size_t inlen, u8 *out, size_t outcap, size_t *consumed,
	      size_t *produced, void *work, size_t worklen);
int zstd_decode(const u8 *in, size_t inlen, u8 *out, size_t outcap, size_t *consumed,
		size_t *produced, void *work, size_t worklen);
u64 pkg_xxh64(const u8 *p, size_t n);           /* XXH64, seed 0 */

/* ---- containers.c --------------------------------------------------------- */
struct ar_member {
	char name[32];
	const u8 *data;
	u64 size;
};

/* Iterate a System V / GNU `ar` archive: *off = 0 first; 1 entry, 0 end, <0 error. */
int ar_next(const u8 *buf, size_t len, size_t *off, struct ar_member *m);

struct cpio_entry {
	const char *name;       /* not NUL-normalised beyond the archive's own NUL */
	u32 mode;
	u64 size;
	const u8 *data;
};

/* Iterate an SVR4 "newc" cpio archive (the RPM payload format). */
int cpio_next(const u8 *buf, size_t len, size_t *off, struct cpio_entry *e);

enum pkg_codec pkg_codec_sniff(const u8 *p, size_t len);

/*
 * Decode a gzip, xz or zstd stream into buf; xz and zstd take their work
 * area from the tail of buf. Results as gz_decode.
 */
int pkg_decode(enum pkg_codec codec, const u8 *in, size_t inlen, u8 *buf, size_t cap,
	       size_t *consumed, size_t *produced);

/*
 * Make a possibly-compressed member readable. Stored data is returned in
 * place; gzip, xz and zstd are decoded into the scratch buffer. Returns 0, 1 (prefix only:
 * scratch was too small for the whole member) or a negative error.
 */
int pkg_unpack(enum pkg_codec codec, const u8 *in, size_t inlen, struct pkg_scratch *s,
	       const u8 **out, size_t *outlen);

/* ---- metadata.c ----------------------------------------------------------- */
int deb_control_parse(const char *txt, size_t len, struct pkg_manifest *m);
int pkginfo_parse(const char *txt, size_t len, struct pkg_manifest *m, bool *is_alpine);
int rpm_parse(const u8 *img, size_t len, struct pkg_manifest *m);

/* ---- depmap.c -------------------------------------------------------------- */
void pkg_add_dep(struct pkg_manifest *m, const char *name, const char *constraint);
void pkg_add_dylib_dep(struct pkg_manifest *m, const char *install_name);
void pkg_set_arch(struct pkg_manifest *m, const char *foreign);
const char *pkg_family(enum pkg_format f);
int merged_translate(const u8 *img, size_t len, struct pkg_manifest *m);

/* ---- bundles.c: AppImage, snap, flatpak ------------------------------------ */
int bundle_translate(const u8 *img, size_t len, struct pkg_scratch *s, struct pkg_manifest *m);
int bundle_exec(const u8 *img, size_t len, const struct pkg_manifest *m, struct pkg_scratch *s,
		const u8 **exe, size_t *exe_len, char *path, size_t path_cap);

#endif /* MERGEDOS_PKG_INTERNAL_H */
