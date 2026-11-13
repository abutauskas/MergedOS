/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_MSH_H
#define MERGEDOS_MSH_H

/*
 * msh core: one command namespace for three vocabularies.
 *
 * `dir`, `ls` and `Get-ChildItem` are the same builtin; `C:\bin\hello.exe`
 * and `/bin/hello.exe` are the same file. This module is the portable part
 * (alias table, tokenizer, path translation); the front end that executes
 * builtins lives with whoever hosts the shell (kernel/monitor.c today, a
 * user-space msh process after the VFS milestone).
 */
#include <mergedos/compat.h>

enum msh_builtin {
	MSH_NONE = 0,
	MSH_HELP, MSH_LS, MSH_CAT, MSH_ECHO, MSH_CLEAR, MSH_UNAME, MSH_FREE, MSH_PS,
	MSH_SYSINFO, MSH_WHICH, MSH_RUN, MSH_OPEN, MSH_PKG, MSH_DE, MSH_FENCE, MSH_SHUTDOWN,
	/* recognised and mapped, but they need a writable VFS to do anything: */
	MSH_CD, MSH_PWD, MSH_CP, MSH_MV, MSH_RM, MSH_MKDIR, MSH_KILL, MSH_GREP, MSH_DATE,
};

enum msh_dialect { MSH_POSIX = 0, MSH_CMD, MSH_POWERSHELL, MSH_NATIVE };

#define MSH_MAX_ALIASES 8

struct msh_alias {
	const char *name;
	enum msh_dialect dialect;
};

struct msh_cmd {
	enum msh_builtin id;
	const char *canon;              /* canonical (POSIX-flavoured) name */
	const char *help;
	bool needs_vfs;
	struct msh_alias alias[MSH_MAX_ALIASES];
};

/*
 * Resolve the first word of a command line. CMD and PowerShell names are
 * case-insensitive, as they are on Windows; POSIX names are case-sensitive.
 */
const struct msh_cmd *msh_resolve(const char *word, enum msh_dialect *dialect);
const struct msh_cmd *msh_commands(size_t *count);
const char *msh_dialect_name(enum msh_dialect d);

/*
 * Split a line in place. Double and single quotes group words. Backslash is
 * an ordinary character so that Windows paths survive. Returns argc.
 */
int msh_tokenize(char *line, char **argv, int max_args);

/*
 * Translate a path in either notation into the native namespace:
 *   C:\Users\me\a.txt   -> /Users/me/a.txt      (C: is the system volume)
 *   D:\data             -> /vol/d/data
 *   \\server\share\x    -> /net/server/share/x
 *   dir\file            -> dir/file
 *   /usr/bin/env        -> /usr/bin/env         (unchanged)
 * Returns the output length, or -MERR_RANGE if it does not fit.
 */
int msh_path_to_native(const char *in, char *out, size_t cap);

#endif /* MERGEDOS_MSH_H */
