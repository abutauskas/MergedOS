/* SPDX-License-Identifier: BSD-2-Clause */
/* Command alias table and tokenizer for the unified shell. */
#include "src/msh/msh.h"

#define P(n)  { n, MSH_POSIX }
#define C(n)  { n, MSH_CMD }
#define PS(n) { n, MSH_POWERSHELL }
#define N(n)  { n, MSH_NATIVE }

static const struct msh_cmd commands[] = {
	{ MSH_HELP, "help", "list commands and their aliases", false,
	  { P("help"), P("man"), C("/?"), PS("Get-Help") } },
	{ MSH_LS, "ls", "list a directory", false,
	  { P("ls"), C("dir"), PS("Get-ChildItem"), PS("gci") } },
	{ MSH_CAT, "cat", "print a text file", false,
	  { P("cat"), C("type"), PS("Get-Content"), PS("gc") } },
	{ MSH_ECHO, "echo", "print arguments", false,
	  { P("echo"), PS("Write-Output"), PS("Write-Host") } },
	{ MSH_CLEAR, "clear", "clear the screen", false,
	  { P("clear"), C("cls"), PS("Clear-Host") } },
	{ MSH_UNAME, "uname", "kernel name and version", false,
	  { P("uname"), C("ver") } },
	{ MSH_FREE, "free", "memory usage", false,
	  { P("free"), C("mem") } },
	{ MSH_PS, "ps", "tasks that have run, with their ABI personality", false,
	  { P("ps"), C("tasklist"), PS("Get-Process"), PS("gps") } },
	{ MSH_SYSINFO, "sysinfo", "hardware inventory", false,
	  { N("sysinfo"), P("lscpu"), P("lspci"), C("systeminfo"), PS("Get-ComputerInfo") } },
	{ MSH_WHICH, "which", "show how a command name resolves", false,
	  { P("which"), C("where"), PS("Get-Command"), PS("gcm") } },
	{ MSH_RUN, "run", "execute a program of any supported format", false,
	  { N("run"), P("exec"), PS("Start-Process") } },
	{ MSH_OPEN, "open", "open any file: Viewer window, program, .app or package", false,
	  { P("open"), P("xdg-open"), C("start"), PS("Invoke-Item"), PS("ii"), N("view") } },
	{ MSH_FILE, "file", "name a file's type and MIME type, like Unix file", false,
	  { P("file"), C("assoc"), PS("Get-ItemProperty"), PS("gp"), N("filetype") } },
	{ MSH_PKG, "mpkg", "inspect or run a foreign package: mpkg inspect|run <file>", false,
	  { N("mpkg") } },
	{ MSH_DE, "de", "show or switch the desktop profile: de [name]", false,
	  { N("de") } },
	{ MSH_FENCE, "mergedfence", "domain blocker: mergedfence status|check <domain>|on|off",
	  false, { N("mergedfence"), N("fence") } },
	{ MSH_SHUTDOWN, "poweroff", "power the machine off", false,
	  { P("poweroff"), P("halt"), P("exit"), C("shutdown"), PS("Stop-Computer") } },

	{ MSH_CD, "cd", "change directory", true,
	  { P("cd"), C("chdir"), PS("Set-Location"), PS("sl") } },
	{ MSH_PWD, "pwd", "print working directory", true,
	  { P("pwd"), PS("Get-Location"), PS("gl") } },
	{ MSH_CP, "cp", "copy files", true,
	  { P("cp"), C("copy"), C("xcopy"), PS("Copy-Item"), PS("cpi") } },
	{ MSH_MV, "mv", "move or rename files", true,
	  { P("mv"), C("move"), C("ren"), PS("Move-Item"), PS("mi") } },
	{ MSH_RM, "rm", "remove files", true,
	  { P("rm"), C("del"), C("erase"), PS("Remove-Item"), PS("ri") } },
	{ MSH_MKDIR, "mkdir", "create a directory", true,
	  { P("mkdir"), C("md"), PS("New-Item") } },
	{ MSH_KILL, "kill", "terminate a process", true,
	  { P("kill"), C("taskkill"), PS("Stop-Process"), PS("spps") } },
	{ MSH_GREP, "grep", "search text", true,
	  { P("grep"), C("findstr"), PS("Select-String"), PS("sls") } },
	{ MSH_DATE, "date", "show date and time", true,
	  { P("date"), C("time"), PS("Get-Date") } },
};

const struct msh_cmd *msh_commands(size_t *count)
{
	*count = ARRAY_SIZE(commands);
	return commands;
}

const char *msh_dialect_name(enum msh_dialect d)
{
	switch (d) {
	case MSH_CMD:        return "cmd";
	case MSH_POWERSHELL: return "powershell";
	case MSH_NATIVE:     return "native";
	default:             return "posix";
	}
}

const struct msh_cmd *msh_resolve(const char *word, enum msh_dialect *dialect)
{
	for (size_t i = 0; i < ARRAY_SIZE(commands); i++) {
		for (int a = 0; a < MSH_MAX_ALIASES && commands[i].alias[a].name; a++) {
			const struct msh_alias *al = &commands[i].alias[a];
			bool match = al->dialect == MSH_CMD || al->dialect == MSH_POWERSHELL
					? !mos_strcasecmp(word, al->name)
					: !strcmp(word, al->name);

			if (match) {
				if (dialect)
					*dialect = al->dialect;
				return &commands[i];
			}
		}
	}
	return NULL;
}

int msh_tokenize(char *line, char **argv, int max_args)
{
	int argc = 0;
	char *p = line;

	while (*p && argc < max_args) {
		char *out;

		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p)
			break;
		argv[argc++] = out = p;
		while (*p && *p != ' ' && *p != '\t') {
			if (*p == '"' || *p == '\'') {
				char quote = *p++;

				while (*p && *p != quote)
					*out++ = *p++;
				if (*p)
					p++;
			} else {
				*out++ = *p++;
			}
		}
		if (*p)
			p++;
		*out = '\0';
	}
	return argc;
}
