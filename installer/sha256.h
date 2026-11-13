/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_SHA256_H
#define MERGEDOS_SHA256_H

/* SHA-256 and PBKDF2-HMAC-SHA256 (sha256.c). Portable: kernel, tests, tools. */
#include <mergedos/compat.h>

#define SHA256_LEN 32

struct sha256 {
	u32 h[8];
	u64 len;
	u8 buf[64];
	u32 fill;
};

void sha256_init(struct sha256 *c);
void sha256_update(struct sha256 *c, const void *data, size_t n);
void sha256_final(struct sha256 *c, u8 out[SHA256_LEN]);
void pbkdf2_hmac_sha256(const void *password, size_t pw_len, const u8 *salt, size_t salt_len,
			u32 iterations, u8 out[SHA256_LEN]);
void secure_wipe(void *p, size_t n);            /* zeroing the compiler may not drop */

#endif /* MERGEDOS_SHA256_H */
