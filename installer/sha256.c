/* SPDX-License-Identifier: BSD-2-Clause */
/* SHA-256 (FIPS 180-4) and PBKDF2-HMAC-SHA256 (RFC 8018), written from the specifications. */
#include "installer/sha256.h"

static const u32 K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
	0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
	0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
	0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
	0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
	0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
	0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
	0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
	0xc67178f2,
};

static inline u32 rotr(u32 x, int n)
{
	return (x >> n) | (x << (32 - n));
}

static void compress(u32 h[8], const u8 block[64])
{
	u32 w[64], s[8];

	for (int i = 0; i < 16; i++)
		w[i] = rd32be(block + i * 4);
	for (int i = 16; i < 64; i++) {
		u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
		u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);

		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	memcpy(s, h, sizeof(s));
	for (int i = 0; i < 64; i++) {
		u32 S1 = rotr(s[4], 6) ^ rotr(s[4], 11) ^ rotr(s[4], 25);
		u32 ch = (s[4] & s[5]) ^ (~s[4] & s[6]);
		u32 t1 = s[7] + S1 + ch + K[i] + w[i];
		u32 S0 = rotr(s[0], 2) ^ rotr(s[0], 13) ^ rotr(s[0], 22);
		u32 maj = (s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]);
		u32 t2 = S0 + maj;

		s[7] = s[6];
		s[6] = s[5];
		s[5] = s[4];
		s[4] = s[3] + t1;
		s[3] = s[2];
		s[2] = s[1];
		s[1] = s[0];
		s[0] = t1 + t2;
	}
	for (int i = 0; i < 8; i++)
		h[i] += s[i];
}

void sha256_init(struct sha256 *c)
{
	static const u32 iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
				   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };

	memcpy(c->h, iv, sizeof(iv));
	c->len = 0;
	c->fill = 0;
}

void sha256_update(struct sha256 *c, const void *data, size_t n)
{
	const u8 *p = data;

	c->len += n;
	while (n) {
		size_t take = MOS_MIN(n, (size_t)(64 - c->fill));

		memcpy(c->buf + c->fill, p, take);
		c->fill += (u32)take;
		p += take;
		n -= take;
		if (c->fill == 64) {
			compress(c->h, c->buf);
			c->fill = 0;
		}
	}
}

void sha256_final(struct sha256 *c, u8 out[SHA256_LEN])
{
	u64 bits = c->len * 8;

	c->buf[c->fill++] = 0x80;
	if (c->fill > 56) {
		memset(c->buf + c->fill, 0, 64 - c->fill);
		compress(c->h, c->buf);
		c->fill = 0;
	}
	memset(c->buf + c->fill, 0, 56 - c->fill);
	for (int i = 0; i < 8; i++)
		c->buf[56 + i] = (u8)(bits >> (56 - 8 * i));
	compress(c->h, c->buf);
	for (int i = 0; i < 8; i++) {
		out[i * 4] = (u8)(c->h[i] >> 24);
		out[i * 4 + 1] = (u8)(c->h[i] >> 16);
		out[i * 4 + 2] = (u8)(c->h[i] >> 8);
		out[i * 4 + 3] = (u8)c->h[i];
	}
}

void secure_wipe(void *p, size_t n)
{
	volatile u8 *v = p;

	while (n--)
		*v++ = 0;
}

void pbkdf2_hmac_sha256(const void *password, size_t pw_len, const u8 *salt, size_t salt_len,
			u32 iterations, u8 out[SHA256_LEN])
{
	static const u8 block_one[4] = { 0, 0, 0, 1 };  /* only one 32-byte block is derived */
	struct sha256 inner, outer, c;
	u8 key[64] = { 0 }, pad[64], u[SHA256_LEN];

	if (pw_len > 64) {
		sha256_init(&c);
		sha256_update(&c, password, pw_len);
		sha256_final(&c, key);
	} else {
		memcpy(key, password, pw_len);
	}
	/* Hash the two HMAC pads once; every iteration then costs two compressions. */
	for (int i = 0; i < 64; i++)
		pad[i] = key[i] ^ 0x36;
	sha256_init(&inner);
	sha256_update(&inner, pad, 64);
	for (int i = 0; i < 64; i++)
		pad[i] = key[i] ^ 0x5C;
	sha256_init(&outer);
	sha256_update(&outer, pad, 64);

	c = inner;
	sha256_update(&c, salt, salt_len);
	sha256_update(&c, block_one, 4);
	sha256_final(&c, u);
	c = outer;
	sha256_update(&c, u, SHA256_LEN);
	sha256_final(&c, u);
	memcpy(out, u, SHA256_LEN);

	for (u32 i = 1; i < iterations; i++) {
		c = inner;
		sha256_update(&c, u, SHA256_LEN);
		sha256_final(&c, u);
		c = outer;
		sha256_update(&c, u, SHA256_LEN);
		sha256_final(&c, u);
		for (int k = 0; k < SHA256_LEN; k++)
			out[k] ^= u[k];
	}
	secure_wipe(key, sizeof(key));
	secure_wipe(pad, sizeof(pad));
	secure_wipe(&inner, sizeof(inner));
	secure_wipe(&outer, sizeof(outer));
	secure_wipe(&c, sizeof(c));
}
