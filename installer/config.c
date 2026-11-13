/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Saved setup: one 512-byte sector at the start of the MergedOS partition.
 *
 *   0   8  magic "MRGDCFG1"
 *   8   4  format version (2; version 1 had no desktop byte and reads as "recommend")
 *   12  4  language
 *   16  4  PBKDF2 iterations (0 = no password)
 *   20  1  password login enabled
 *   21  1  desktop: enum de_id, or 0xFF = recommend for the hardware (version 2)
 *   24  16 salt
 *   40  32 password hash
 *   72  64 user name, UTF-8, NUL-padded
 *   508 4  CRC-32 of bytes 0..507
 * All integers little-endian. The password itself is never stored.
 */
#include "installer/installer.h"
#include "storage/storage.h"

#define CONFIG_MAGIC   "MRGDCFG1"
#define CONFIG_VERSION 2

static void put32(u8 *p, u32 v)
{
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
	p[2] = (u8)(v >> 16);
	p[3] = (u8)(v >> 24);
}

void config_pack(const struct install_config *c, u8 s[SECTOR_SIZE])
{
	memset(s, 0, SECTOR_SIZE);
	memcpy(s, CONFIG_MAGIC, 8);
	put32(s + 8, CONFIG_VERSION);
	put32(s + 12, (u32)c->lang);
	put32(s + 16, c->iterations);
	s[20] = c->password_enabled;
	s[21] = c->desktop;
	memcpy(s + 24, c->salt, PW_SALT_LEN);
	memcpy(s + 40, c->hash, SHA256_LEN);
	mos_strlcpy((char *)s + 72, c->name, 64);
	put32(s + 508, mos_crc32(0, s, 508));
}

int config_unpack(const u8 s[SECTOR_SIZE], struct install_config *c)
{
	u32 version = rd32le(s + 8);

	memset(c, 0, sizeof(*c));
	if (memcmp(s, CONFIG_MAGIC, 8) || version < 1 || version > CONFIG_VERSION ||
	    rd32le(s + 508) != mos_crc32(0, s, 508) || rd32le(s + 12) >= LANG_COUNT || s[20] > 1 ||
	    (version >= 2 && s[21] >= DE_COUNT && s[21] != DESKTOP_AUTO))
		return -MERR_INVAL;
	c->desktop = version >= 2 ? s[21] : DESKTOP_AUTO;
	c->lang = (enum wiz_lang)rd32le(s + 12);
	c->iterations = rd32le(s + 16);
	c->password_enabled = s[20];
	memcpy(c->salt, s + 24, PW_SALT_LEN);
	memcpy(c->hash, s + 40, SHA256_LEN);
	mos_fieldcpy(c->name, sizeof(c->name), (const char *)s + 72, 63);
	return 0;
}
