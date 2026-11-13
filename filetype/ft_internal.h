/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_FT_INTERNAL_H
#define MERGEDOS_FT_INTERNAL_H

#include "filetype/filetype.h"

/* Length of the UTF-8 sequence at p (1..4), or 0 if malformed; a sequence cut
 * off by the end of the n bytes counts as valid when `tail_ok`. */
int ft_utf8_len(const u8 *p, size_t n, bool tail_ok);
const char *ft_base_name(const char *path);

#endif /* MERGEDOS_FT_INTERNAL_H */
