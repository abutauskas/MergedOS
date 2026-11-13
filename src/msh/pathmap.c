/* SPDX-License-Identifier: BSD-2-Clause */
/* Windows-notation -> native path translation for the unified shell. */
#include "src/msh/msh.h"

static bool is_sep(char c)
{
	return c == '/' || c == '\\';
}

static bool is_drive_letter(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

int msh_path_to_native(const char *in, char *out, size_t cap)
{
	char prefix[16] = "";
	size_t n;

	if (in[0] == '\\' && in[1] == '\\' && in[2]) {
		/* UNC: \\server\share\path */
		mos_strlcpy(prefix, "/net/", sizeof(prefix));
		in += 2;
	} else if (is_drive_letter(in[0]) && in[1] == ':' && (in[2] == '\0' || is_sep(in[2]))) {
		/* Drive letter: C: is the system volume, i.e. the root itself. */
		if (mos_tolower(in[0]) == 'c')
			mos_strlcpy(prefix, "/", sizeof(prefix));
		else
			mos_snprintf(prefix, sizeof(prefix), "/vol/%c/", mos_tolower(in[0]));
		in += 2;
		while (is_sep(*in))
			in++;
	}

	n = mos_strlcpy(out, prefix, cap);
	for (; *in; in++, n++)
		if (n + 1 < cap)
			out[n] = *in == '\\' ? '/' : *in;
	if (n >= cap)
		return -MERR_RANGE;
	out[n] = '\0';

	/* "/vol/d/" -> "/vol/d": no trailing slash except on the root. */
	while (n > 1 && out[n - 1] == '/')
		out[--n] = '\0';
	return (int)n;
}
