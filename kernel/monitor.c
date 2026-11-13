/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * In-kernel msh front end.
 *
 * Until there is a VFS and a user-space shell, this is the terminal: it
 * hosts the portable msh core (src/msh) and wires its builtins to kernel
 * services. Windows, PowerShell and POSIX command names resolve to the same
 * builtins, both path notations name the same files, and programs of all
 * four binary formats start from the same prompt.
 */
#include <mergedos/kernel.h>
#include <mergedos/console.h>
#include <mergedos/initrd.h>
#include <mergedos/mm.h>
#include <mergedos/monitor.h>
#include <mergedos/sched.h>
#include <mergedos/task.h>
#include "abi_dispatch/binfmt.h"
#include "de_selector/de_selector.h"
#include "filetype/filetype.h"
#include "mergedfence/fence.h"
#include "pkg_translate/pkg.h"
#include "src/msh/msh.h"

#define LINE_MAX_LEN 160
#define MAX_ARGS     12
#define SCRATCH_SIZE (96 * 1024)

static u8 scratch_buf[SCRATCH_SIZE];
static struct pkg_scratch scratch = { scratch_buf, sizeof(scratch_buf) };
static struct pkg_manifest manifest;
static char text_buf[2048];

/* ---- program lookup and launch ------------------------------------------- */

/* Resolve a program name in either path notation against the ramdisk. */
static int find_program(const char *arg, char *path, size_t cap, const u8 **img, size_t *len)
{
	static const char *const patterns[] = { "%s", "bin/%s", "bin/%s.exe" };
	char native[128];
	const char *rel = native;

	if (msh_path_to_native(arg, native, sizeof(native)) < 0)
		return -MERR_RANGE;
	while (rel[0] == '.' && rel[1] == '/')
		rel += 2;
	while (*rel == '/')
		rel++;
	for (unsigned i = 0; i < ARRAY_SIZE(patterns); i++) {
		ksnprintf(path, cap, patterns[i], rel);
		if (!initrd_find(path, img, len))
			return 0;
	}
	return -MERR_NOENT;
}

static const char *load_error(int rc)
{
	switch (-rc) {
	case MERR_NOEXEC: return "not an executable this system recognises";
	case MERR_NOTSUP: return "recognised, but needs support that is not built yet "
				 "(dynamic linking, codec or foreign CPU)";
	case MERR_NOMEM:  return "out of memory";
	case MERR_NOENT:  return "no such file";
	case MERR_RANGE:  return "too large for the decode buffer";
	default:          return "failed";
	}
}

static int launch(const char *name, const u8 *img, size_t len)
{
	struct exec_report rep;
	int rc = task_exec(name, img, len, &rep);

	if (rc) {
		kprintf("msh: %s: %s (%s %s)\n", name, load_error(rc),
			binfmt_format_name(rep.format), abi_personality_name(rep.personality));
		return rc;
	}
	if (rep.exit_code)
		kprintf("msh: %s exited with status %d\n", name, rep.exit_code);
	return rep.exit_code;
}

static int cmd_run(const char *arg)
{
	char path[128];
	const u8 *img;
	size_t len;
	int rc = find_program(arg, path, sizeof(path), &img, &len);

	if (rc) {
		kprintf("msh: %s: command or program not found\n", arg);
		return rc;
	}
	return launch(path, img, len);
}

/* ---- packages and bundles -------------------------------------------------- */

static bool is_app_bundle(const char *path)
{
	return mos_has_suffix(path, ".app") || mos_has_suffix(path, ".app/");
}

/* Translate a package file or .app bundle; optionally run its program. */
static int cmd_pkg(const char *arg, bool inspect, bool run)
{
	char native[128], path[192];
	const char *rel = native;
	const u8 *img, *exe = NULL;
	size_t len, exe_len = 0, n;
	int rc;

	if (msh_path_to_native(arg, native, sizeof(native)) < 0)
		return -MERR_RANGE;
	while (*rel == '/')
		rel++;
	n = strlen(rel);
	while (n && rel[n - 1] == '/')
		native[(rel - native) + --n] = '\0';

	if (is_app_bundle(rel)) {
		const u8 *plist;
		size_t plist_len;

		ksnprintf(path, sizeof(path), "%s/Contents/Info.plist", rel);
		if (initrd_find(path, &plist, &plist_len)) {
			kprintf("mpkg: %s: not a bundle (no Contents/Info.plist)\n", arg);
			return -MERR_NOENT;
		}
		/* First pass names the executable, second pass inspects it. */
		rc = pkg_translate_app(plist, plist_len, NULL, 0, &manifest);
		if (!rc) {
			ksnprintf(path, sizeof(path), "%s/%s", rel, manifest.executable);
			rc = initrd_find(path, &exe, &exe_len);
		}
		if (!rc)
			rc = pkg_translate_app(plist, plist_len, exe, exe_len, &manifest);
	} else {
		if (initrd_find(rel, &img, &len)) {
			kprintf("mpkg: %s: no such file\n", arg);
			return -MERR_NOENT;
		}
		rc = pkg_translate(img, len, rel, &scratch, &manifest);
		if (!rc && run)
			rc = pkg_payload_exec(img, len, &manifest, &scratch, &exe, &exe_len, path,
					      sizeof(path));
	}
	if (rc) {
		kprintf("mpkg: %s: %s", arg, load_error(rc));
		if (manifest.format != PKG_UNKNOWN)
			kprintf(" [%s]", pkg_format_name(manifest.format));
		kprintf("\n");
		return rc;
	}
	if (inspect) {
		pkg_manifest_print(&manifest, text_buf, sizeof(text_buf));
		kprintf("%s:\n", arg);
		console_write(text_buf, strlen(text_buf));
	}
	if (!run)
		return 0;
	kprintf("[mpkg] %s %s (%s) -> %s personality, running %s\n", manifest.name,
		manifest.version, pkg_format_name(manifest.format),
		abi_personality_name(manifest.personality), path);
	return launch(path, exe, exe_len);
}

/* ---- builtins -------------------------------------------------------------- */

static void describe(const u8 *data, size_t size, const char *name, char *out, size_t cap)
{
	struct binfmt_info bi;
	enum pkg_format pf;

	if (!binfmt_probe(data, size, &bi) && bi.format != BINFMT_SCRIPT) {
		ksnprintf(out, cap, "%s program, %s ABI", binfmt_format_name(bi.format),
			  abi_personality_name(bi.personality));
		return;
	}
	pf = pkg_probe(data, size, name);
	if (pf != PKG_UNKNOWN)
		ksnprintf(out, cap, "%s", pkg_format_name(pf));
	else
		ksnprintf(out, cap, "data");
}

static int cmd_ls(const char *arg)
{
	char native[128], kind[64];
	const char *dir = native;
	struct tar_entry e;
	size_t off = 0, dlen;

	if (msh_path_to_native(arg ? arg : "/", native, sizeof(native)) < 0)
		return -MERR_RANGE;
	while (*dir == '/')
		dir++;
	dlen = strlen(dir);
	if (!initrd_is_dir(dir)) {
		kprintf("ls: %s: no such directory\n", arg ? arg : "/");
		return -MERR_NOENT;
	}

	while (initrd_next(&off, &e) > 0) {
		const char *rest = e.name;
		size_t rlen;

		if (dlen) {
			if (strncmp(e.name, dir, dlen) || e.name[dlen] != '/')
				continue;
			rest = e.name + dlen + 1;
		}
		rlen = strlen(rest);
		if (rlen && rest[rlen - 1] == '/')
			rlen--;
		if (!rlen || memchr(rest, '/', rlen))
			continue;                       /* the directory itself, or deeper */
		if (e.type == '5') {
			kprintf("  %8s  %-34s directory\n", "<DIR>", rest);
		} else {
			describe(e.data, (size_t)e.size, e.name, kind, sizeof(kind));
			kprintf("  %8llu  %-34s %s\n", (unsigned long long)e.size, rest, kind);
		}
	}
	return 0;
}

static int cmd_cat(const char *arg)
{
	char native[128];
	const u8 *data;
	size_t size;

	if (!arg || msh_path_to_native(arg, native, sizeof(native)) < 0 ||
	    initrd_find(native, &data, &size)) {
		kprintf("cat: %s: no such file\n", arg ? arg : "(missing operand)");
		return -MERR_NOENT;
	}
	if (memchr(data, '\0', MOS_MIN(size, (size_t)512))) {
		kprintf("cat: %s: binary file, not shown\n", arg);
		return -MERR_INVAL;
	}
	console_write((const char *)data, size);
	return 0;
}

static int cmd_ps(void)
{
	static struct exec_report hist[TASK_HISTORY_MAX];
	u32 n = task_history(hist, TASK_HISTORY_MAX);

	sched_print();
	kprintf("  PID  ABI       FORMAT      SYSCALLS  PAGES  EXIT  NAME\n");
	kprintf("    0  native    -                  -      -     -  kernel + msh (running)\n");
	for (u32 i = 0; i < n; i++)
		kprintf("  %3d  %-8s  %-10s  %8llu  %5u  %4d  %s\n", hist[i].pid,
			abi_personality_name(hist[i].personality),
			binfmt_format_name(hist[i].format), (unsigned long long)hist[i].syscalls,
			hist[i].pages, hist[i].exit_code, hist[i].name);
	return 0;
}

/* ---- any file: type, Viewer window or text preview ------------------------ */

/* Resolve a path in either notation to a ramdisk file. */
static int find_file(const char *who, const char *arg, char *native, size_t cap,
		     const u8 **data, size_t *size)
{
	if (!arg) {
		kprintf("usage: %s <path>\n", who);
		return -MERR_INVAL;
	}
	if (msh_path_to_native(arg, native, cap) < 0)
		return -MERR_RANGE;
	if (initrd_find(native, data, size)) {
		kprintf("%s: %s: %s\n", who, arg, initrd_is_dir(native[0] == '/' ? native + 1 :
				native) ? "directory" : "no such file");
		return -MERR_NOENT;
	}
	return 0;
}

static int cmd_file(int argc, char **argv)
{
	int rc = argc < 2 ? -MERR_INVAL : 0;

	if (argc < 2)
		kprintf("usage: file <path>...\n");
	for (int i = 1; i < argc; i++) {
		char native[128];
		const u8 *data;
		size_t size;
		struct ft_type t;
		struct ft_meta m;
		char facts[48];

		if (find_file("file", argv[i], native, sizeof(native), &data, &size)) {
			rc = -MERR_NOENT;
			continue;
		}
		ft_identify(data, size, native, &t);
		ft_meta_read(data, size, &t, &m);
		facts[0] = '\0';
		if (m.width && m.height)
			ksnprintf(facts, sizeof(facts), ", %u x %u", (unsigned)m.width,
				  (unsigned)m.height);
		else if (m.duration_ms)
			ksnprintf(facts, sizeof(facts), ", %u ch %u Hz, %llu ms", (unsigned)m.channels,
				  (unsigned)m.rate, (unsigned long long)m.duration_ms);
		kprintf("%s: %s%s%s%s [%s]\n", argv[i], t.name, t.detail[0] ? ", " : "", t.detail,
			facts, t.mime);
	}
	return rc;
}

/*
 * open: .app bundles, packages and programs start as before; any other file
 * goes to the Viewer window when the desktop runs, else its info card and
 * a text or hex preview are printed here.
 */
static int cmd_open(const char *arg)
{
	static struct ft_doc doc;
	char native[128], line[FT_LINE_MAX];
	const u8 *data;
	size_t size;
	struct binfmt_info bi;
	unsigned view;
	int rc, rows;

	if (arg && msh_path_to_native(arg, native, sizeof(native)) >= 0 && is_app_bundle(native))
		return cmd_pkg(arg, false, true);
	rc = find_file("open", arg, native, sizeof(native), &data, &size);
	if (rc)
		return rc;
	ft_doc_open(&doc, data, size, native, scratch_buf, sizeof(scratch_buf));
	if (doc.type.kind == FT_PACKAGE)
		return cmd_pkg(arg, false, true);
	if (!binfmt_probe(data, size, &bi) && bi.format != BINFMT_SCRIPT)
		return cmd_run(arg);
	if (desktop_view(native)) {
		kprintf("open: %s: %s, shown in the Viewer window\n", arg, doc.type.name);
		return 0;
	}
	if (doc.views & FT_VIEW_IMAGE) {
		u32 w, h;

		rc = desktop_view_decode(data, size, &w, &h);
		if (rc)
			ksnprintf(doc.note, sizeof(doc.note), "image not shown: %s", ft_image_error(rc));
		else
			ksnprintf(doc.note, sizeof(doc.note), "image decodes: %u x %u RGB",
				  (unsigned)w, (unsigned)h);
	}
	for (int i = 0; ft_doc_line(&doc, FT_VIEW_INFO, i, line, sizeof(line)); i++)
		kprintf("  %s\n", line);
	view = doc.views & FT_VIEW_TEXT ? FT_VIEW_TEXT : FT_VIEW_HEX;
	rows = view == FT_VIEW_TEXT ? 12 : 8;
	if (size)
		kprintf("  -- %s preview --\n", view == FT_VIEW_TEXT ? "text" : "hex");
	for (int i = 0; i < rows && ft_doc_line(&doc, view, i, line, sizeof(line)); i++)
		kprintf("  %s\n", line);
	return 0;
}

static int cmd_help(void)
{
	size_t count;
	const struct msh_cmd *cmds = msh_commands(&count);

	kprintf("Commands (every alias on a line is the same builtin):\n");
	for (size_t i = 0; i < count; i++) {
		char names[96] = "";

		for (int a = 0; a < MSH_MAX_ALIASES && cmds[i].alias[a].name; a++) {
			if (a)
				mos_strlcat(names, ", ", sizeof(names));
			mos_strlcat(names, cmds[i].alias[a].name, sizeof(names));
		}
		kprintf("  %-46s %s%s\n", names, cmds[i].help,
			cmds[i].needs_vfs ? " [needs VFS]" : "");
	}
	kprintf("Programs: type a name or path in either notation, e.g. hello.exe, "
		"./bin/hello.linux, C:\\bin\\hello.macho\n");
	return 0;
}

static int cmd_which(const char *arg)
{
	enum msh_dialect d;
	const struct msh_cmd *cmd;
	char path[128];
	const u8 *img;
	size_t len;

	if (!arg)
		return -MERR_INVAL;
	cmd = msh_resolve(arg, &d);
	if (cmd) {
		kprintf("%s: %s name for builtin `%s`\n", arg, msh_dialect_name(d), cmd->canon);
		return 0;
	}
	if (!find_program(arg, path, sizeof(path), &img, &len)) {
		char kind[64];

		describe(img, len, path, kind, sizeof(kind));
		kprintf("%s: /%s (%s)\n", arg, path, kind);
		return 0;
	}
	kprintf("%s: not found\n", arg);
	return -MERR_NOENT;
}

static int cmd_de(const char *arg)
{
	struct de_recommendation rec;
	int id;

	if (!arg) {
		de_recommend(system_hw(), &rec);
		de_print_table(system_hw(), &rec);
		kprintf("   current: %s\n", de_profile(de_session_current())->title);
		return 0;
	}
	id = de_lookup(arg);
	if (id < 0) {
		kprintf("de: unknown desktop '%s' (taskbar, dock, tiling, retro)\n", arg);
		return -MERR_INVAL;
	}
	de_session_start((enum de_id)id, system_hw());
	return 0;
}

/* ---- Mergedfence ------------------------------------------------------------ */

#define FENCE_LIST_PATH "etc/mergedfence/default.merged"

static struct fence fence;
static bool fence_ready;

/* First use: verify the shipped .merged blocklist and load its payload. */
static int fence_setup(void)
{
	struct merged_pkg pkg;
	const u8 *img;
	size_t len;
	int rc;

	if (fence_ready)
		return 0;
	fence_init(&fence);
	if (initrd_find(FENCE_LIST_PATH, &img, &len)) {
		kprintf("mergedfence: /%s: no such file\n", FENCE_LIST_PATH);
		return -MERR_NOENT;
	}
	rc = merged_parse(img, len, &pkg);
	if (rc) {
		kprintf("mergedfence: /%s: %s\n", FENCE_LIST_PATH, load_error(rc));
		return rc;
	}
	if (pkg.type != MERGED_TYPE_BLOCKLIST) {
		kprintf("mergedfence: /%s: type %s, not a blocklist\n", FENCE_LIST_PATH,
			merged_type_name(pkg.type));
		return -MERR_NOEXEC;
	}
	rc = fence_load(&fence, (const char *)pkg.payload, pkg.payload_len);
	if (rc < 0)
		kprintf("mergedfence: rule table full, %u rules dropped\n", fence.st.dropped);
	kprintf("mergedfence: loaded %s %s (%u rules)\n", pkg.name, pkg.version, fence.st.rules);
	fence_ready = true;
	return 0;
}

const struct fence *monitor_fence(void)
{
	return fence_ready ? &fence : NULL;
}

static int cmd_fence(int argc, char **argv)
{
	const struct fence_stats *st;
	int rc = fence_setup();

	if (rc)
		return rc;
	st = fence_stats(&fence);
	if (argc == 1 || (argc == 2 && !strcmp(argv[1], "status"))) {
		kprintf("mergedfence: %s\n", fence.enabled ? "enabled" : "disabled");
		kprintf("  rules %u (%u exceptions), ignored lines %u, dropped %u\n", st->rules,
			st->exceptions, st->ignored, st->dropped);
		kprintf("  checks %u, blocked %u, allowed by exception %u\n", st->checks,
			st->blocked, st->excepted);
		kprintf("  no network stack yet: verdicts are not applied on the wire\n");
		return 0;
	}
	if (argc == 3 && !strcmp(argv[1], "check")) {
		kprintf("%s: %s\n", argv[2], fence_verdict_name(fence_check(&fence, argv[2])));
		return 0;
	}
	if (argc == 2 && (!strcmp(argv[1], "on") || !strcmp(argv[1], "off"))) {
		fence_enable(&fence, argv[1][1] == 'n');
		kprintf("mergedfence: %s\n", fence.enabled ? "enabled" : "disabled");
		return 0;
	}
	kprintf("usage: mergedfence status | check <domain> | on | off\n");
	return -MERR_INVAL;
}

int monitor_exec(const char *line)
{
	char buf[LINE_MAX_LEN];
	char *argv[MAX_ARGS + 1] = { 0 };
	const struct msh_cmd *cmd;
	int argc;

	mos_strlcpy(buf, line, sizeof(buf));
	argc = msh_tokenize(buf, argv, MAX_ARGS);
	if (!argc)
		return 0;

	cmd = msh_resolve(argv[0], NULL);
	if (!cmd)
		return cmd_run(argv[0]);        /* not a builtin: try it as a program */
	if (cmd->needs_vfs) {
		kprintf("msh: `%s` is `%s`: recognised, but it needs the writable VFS "
			"(roadmap M3)\n", argv[0], cmd->canon);
		return -MERR_NOSYS;
	}

	switch (cmd->id) {
	case MSH_HELP:
		return cmd_help();
	case MSH_LS:
		return cmd_ls(argv[1]);
	case MSH_CAT:
		return cmd_cat(argv[1]);
	case MSH_ECHO:
		for (int i = 1; i < argc; i++)
			kprintf("%s%s", i > 1 ? " " : "", argv[i]);
		kprintf("\n");
		return 0;
	case MSH_CLEAR:
		console_clear();
		return 0;
	case MSH_UNAME:
		kprintf("MergedOS %s x86_64\n", MERGEDOS_VERSION);
		for (int p = ABI_LINUX; p < ABI_COUNT; p++) {
			const struct abi_identity *id = abi_identity((enum abi_personality)p);

			kprintf("  %-8s programs are told: %s %s (%s)\n",
				abi_personality_name((enum abi_personality)p), id->sysname, id->release,
				id->version);
		}
		return 0;
	case MSH_FREE:
		system_mem_report();
		return 0;
	case MSH_PS:
		return cmd_ps();
	case MSH_SYSINFO:
		hwprobe_print(system_hw());
		return 0;
	case MSH_WHICH:
		return cmd_which(argv[1]);
	case MSH_RUN:
		return argv[1] ? cmd_run(argv[1]) : -MERR_INVAL;
	case MSH_OPEN:
		return cmd_open(argv[1]);
	case MSH_FILE:
		return cmd_file(argc, argv);
	case MSH_PKG:
		if (argc == 3 && !strcmp(argv[1], "inspect"))
			return cmd_pkg(argv[2], true, false);
		if (argc == 3 && !strcmp(argv[1], "run"))
			return cmd_pkg(argv[2], false, true);
		kprintf("usage: mpkg inspect|run <package file or .app bundle>\n");
		return -MERR_INVAL;
	case MSH_DE:
		return cmd_de(argv[1]);
	case MSH_FENCE:
		return cmd_fence(argc, argv);
	case MSH_SHUTDOWN:
		kprintf("Powering off.\n");
		machine_poweroff(0x10);
	default:
		return -MERR_NOSYS;
	}
}

void monitor_run(void)
{
	char line[LINE_MAX_LEN];

	monitor_exec("cat /etc/motd");
	for (;;) {
		size_t n = 0;

		kprintf("msh:/> ");
		for (;;) {
			int c = console_getc();

			if (c == '\n') {
				console_putc('\n');
				break;
			}
			if (c == '\b') {
				if (n) {
					n--;
					console_write("\b \b", 3);
				}
			} else if (c >= ' ' && c < 0x7F && n + 1 < sizeof(line)) {
				line[n++] = (char)c;
				console_putc((char)c);
			}
		}
		line[n] = '\0';
		monitor_exec(line);
	}
}

/* ---- boot self-test --------------------------------------------------------- */

int monitor_selftest(void)
{
	/* Each line must succeed (expect 0) or fail in the documented way (expect != 0). */
	static const struct {
		const char *line;
		bool expect_ok;
	} script[] = {
		{ "dir C:\\bin", true },                        /* CMD name, Windows path   */
		{ "ls /pkg", true },                            /* POSIX name, POSIX path   */
		{ "Get-ChildItem /Applications/Hello.app/Contents", true },
		{ "hello.exe", true },                          /* PE32+, NT personality    */
		{ "./bin/hello.linux", true },                  /* ELF, Linux personality   */
		{ "C:\\bin\\hello.freebsd", true },             /* ELF, FreeBSD personality */
		{ "run /bin/hello.macho", true },               /* Mach-O, XNU personality  */
		{ "open /Applications/Hello.app", true },       /* .app bundle              */
		{ "file /home/samples/sample.png /home/samples/sample.zip", true },
		{ "file /home/samples/random.bin", true },
		{ "Get-ItemProperty C:\\home\\samples\\sample.wav", true },
		{ "open /home/samples/sample.png", true },      /* info card + decode check */
		{ "open /home/samples/sample.zip", true },      /* listing                  */
		{ "open /home/samples/sample.csv", true },      /* text preview             */
		{ "file /home/samples/no-such-file", false },
		{ "mpkg inspect /pkg/hello_1.0-1_amd64.deb", true },
		{ "mpkg inspect /pkg/hello-1.0-1.x86_64.rpm", true },
		{ "mpkg inspect /Applications/Hello.app", true },
		{ "mpkg run /pkg/hello_1.0-1_amd64.deb", true },
		{ "mpkg run /pkg/hello-1.0-1.x86_64.rpm", true },
		{ "mpkg run /pkg/hello-1.0-r0.apk", true },
		{ "mpkg run /pkg/hello-1.0-1-x86_64.pkg.tar.gz", true },
		{ "where Get-Process", true },
		{ "tasklist", true },
		{ "free", true },
		{ "mergedfence check tracker.example", true },
		{ "mergedfence status", true },
		{ "mpkg inspect /etc/mergedfence/default.merged", true },
		{ "del C:\\bin\\hello.exe", false },            /* mapped, needs the VFS    */
		{ "no-such-program", false },
		{ "run /etc/motd", false },                     /* not an executable        */
	};
	int failures = 0;

	kprintf("\n== shell self-test: %u commands ==\n", (unsigned)ARRAY_SIZE(script));
	for (unsigned i = 0; i < ARRAY_SIZE(script); i++) {
		int rc;

		kprintf("msh:/> %s\n", script[i].line);
		rc = monitor_exec(script[i].line);
		if ((rc == 0) != script[i].expect_ok) {
			kprintf("SELFTEST FAIL: `%s` returned %d\n", script[i].line, rc);
			failures++;
		}
	}
	return failures;
}
