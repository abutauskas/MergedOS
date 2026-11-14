/* SPDX-License-Identifier: BSD-2-Clause */
#include "tests/test.h"
#include "pkg_translate/pkg_internal.h"
#include "pkg_translate/squashfs.h"

static u8 scratch_buf[256 * 1024];
static struct pkg_scratch scratch = { scratch_buf, sizeof(scratch_buf) };

static const struct pkg_dep *find_dep(const struct pkg_manifest *m, const char *name)
{
	for (u32 i = 0; i < m->ndeps; i++)
		if (!strcmp(m->deps[i].name, name))
			return &m->deps[i];
	return NULL;
}

/* The payload program of every Linux package must be the original ELF, bit for bit. */
static void check_payload(const u8 *img, size_t len, const struct pkg_manifest *m)
{
	size_t elf_len, exe_len = 0;
	u8 *elf = load_fixture("bin/hello.linux", &elf_len);
	const u8 *exe = NULL;
	char path[128] = "";

	CHECK(m->flags & PKG_F_PAYLOAD_OK);
	CHECK_EQ(pkg_payload_exec(img, len, m, &scratch, &exe, &exe_len, path, sizeof(path)), 0);
	CHECK_EQ(exe_len, elf_len);
	CHECK(exe && exe_len == elf_len && !memcmp(exe, elf, elf_len));
	CHECK(strstr(path, "bin/hello") != NULL);
	free(elf);
}

/* Truncations and bit flips of a package must never crash a parser. */
static void torture(const u8 *img, size_t len, const char *hint)
{
	struct pkg_manifest m;
	const u8 *exe;
	size_t exe_len;
	char path[64];
	u8 *copy = malloc(len);

	for (size_t n = 0; n <= len; n += (n < 256 ? 1 : 7)) {
		u8 *exact = malloc(n ? n : 1);

		memcpy(exact, img, n);
		if (!pkg_translate(exact, n, hint, &scratch, &m))
			(void)pkg_payload_exec(exact, n, &m, &scratch, &exe, &exe_len, path,
					       sizeof(path));
		free(exact);
	}
	for (size_t i = 0; i < len; i++) {
		memcpy(copy, img, len);
		copy[i] ^= (u8)(1u << (i % 8));
		if (!pkg_translate(copy, len, hint, &scratch, &m))
			(void)pkg_payload_exec(copy, len, &m, &scratch, &exe, &exe_len, path,
					       sizeof(path));
	}
	free(copy);
}

static void test_gzip(void)
{
	static const char *const cases[] = { "gz/stored", "gz/fixed", "gz/dynamic" };

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		char name[64];
		size_t gz_len, raw_len, consumed = 0, produced = 0;
		u8 *gz, *raw;

		snprintf(name, sizeof(name), "%s.gz", cases[i]);
		gz = load_fixture(name, &gz_len);
		snprintf(name, sizeof(name), "%s.bin", cases[i]);
		raw = load_fixture(name, &raw_len);

		CHECK_EQ(gz_decode(gz, gz_len, scratch.buf, scratch.cap, &consumed, &produced), 0);
		CHECK_EQ(produced, raw_len);
		CHECK_EQ(consumed, gz_len);
		CHECK(produced == raw_len && !memcmp(scratch.buf, raw, raw_len));
		/* A too-small buffer yields a correct prefix, not an error. */
		CHECK_EQ(gz_decode(gz, gz_len, scratch.buf, 100, &consumed, &produced), 1);
		CHECK(produced == 100 && !memcmp(scratch.buf, raw, 100));
		/* A flipped payload bit must be caught (CRC-32), not returned as data. */
		gz[gz_len / 2] ^= 0x10;
		CHECK(gz_decode(gz, gz_len, scratch.buf, scratch.cap, &consumed, &produced) != 0);
		free(gz);
		free(raw);
	}
}

static void test_app(void)
{
	static const char *const dylibs[] = {
		"/usr/lib/libSystem.B.dylib",
		"/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit",
		"@rpath/libfoo.dylib",
	};
	static const u8 bplist[] = "bplist00\x01\x02\x03\x04";
	static const char no_exec[] = "<?xml version=\"1.0\"?><plist><dict>"
				      "<key>CFBundleName</key><string>X</string></dict></plist>";
	static const char evil[] = "<plist><dict><key>CFBundleExecutable</key>"
				   "<string>../../bin/sh</string></dict></plist>";
	struct pkg_manifest m;
	size_t plist_len, exe_len, off = 32;
	u8 *plist = load_fixture("Applications/Hello.app/Contents/Info.plist", &plist_len);
	u8 *exe = load_fixture("Applications/Hello.app/Contents/MacOS/hello", &exe_len);
	u8 macho[512] = { 0 };
	const struct pkg_dep *d;
	char path[256];

	CHECK_EQ(pkg_probe(plist, plist_len, "Hello.app/Contents/Info.plist"), PKG_MACOS_APP);
	CHECK_EQ(pkg_probe(plist, plist_len, NULL), PKG_MACOS_APP);
	CHECK_EQ(pkg_translate_app(plist, plist_len, exe, exe_len, &m), 0);
	CHECK_EQ(m.format, PKG_MACOS_APP);
	CHECK_EQ(m.personality, ABI_XNU);
	CHECK_STR(m.name, "Hello");
	CHECK_STR(m.version, "1.0");
	CHECK_STR(m.bundle_id, "org.mergedos.fixtures.Hello");
	CHECK_STR(m.min_os, "10.13");
	CHECK_STR(m.executable, "Contents/MacOS/hello");
	CHECK_STR(m.arch, "x86_64");
	CHECK_STR(m.store_prefix, "/pkg/app/org.mergedos.fixtures.Hello/1.0");
	CHECK(m.flags & PKG_F_RELOCATABLE);
	CHECK_EQ(m.ndeps, 0);                                   /* static test binary */
	CHECK(pkg_store_path(&m, "Contents/Resources/icon.icns", path, sizeof(path)) > 0);
	CHECK_STR(path, "/pkg/app/org.mergedos.fixtures.Hello/1.0/Contents/Resources/icon.icns");

	/* A synthetic dynamically linked Mach-O: dependencies come from LC_LOAD_DYLIB. */
	memcpy(macho, "\xCF\xFA\xED\xFE\x07\x00\x00\x01\x03\x00\x00\x00\x02\x00\x00\x00", 16);
	for (size_t i = 0; i < ARRAY_SIZE(dylibs); i++) {
		u32 cmdsize = (u32)ALIGN_UP(24 + strlen(dylibs[i]) + 1, 8);
		u32 cmd[3] = { 0xC, cmdsize, 24 };

		memcpy(macho + off, cmd, sizeof(cmd));
		strcpy((char *)macho + off + 24, dylibs[i]);
		off += cmdsize;
	}
	macho[16] = (u8)ARRAY_SIZE(dylibs);                     /* ncmds      */
	macho[20] = (u8)(off - 32);                             /* sizeofcmds */
	CHECK_EQ(pkg_translate_app(plist, plist_len, macho, sizeof(macho), &m), 0);
	CHECK_EQ(m.ndeps, 3);
	d = find_dep(&m, dylibs[0]);
	CHECK(d && d->base && !strcmp(d->capability, "runtime:darwin-libsystem"));
	d = find_dep(&m, dylibs[1]);
	CHECK(d && !d->base && !strcmp(d->capability, "framework:AppKit"));
	d = find_dep(&m, dylibs[2]);
	CHECK(d && d->base && !strcmp(d->capability, "bundle:libfoo.dylib"));

	CHECK_EQ(pkg_translate_app(bplist, sizeof(bplist), NULL, 0, &m), -MERR_NOTSUP);
	CHECK_EQ(pkg_translate_app((const u8 *)no_exec, sizeof(no_exec), NULL, 0, &m), -MERR_NOEXEC);
	/* The executable name may not escape the bundle. */
	CHECK_EQ(pkg_translate_app((const u8 *)evil, sizeof(evil), NULL, 0, &m), -MERR_NOEXEC);
	/* An "executable" that is not a Mach-O is rejected. */
	CHECK_EQ(pkg_translate_app(plist, plist_len, plist, plist_len, &m), -MERR_NOEXEC);
	for (size_t n = 0; n <= plist_len; n += 5)
		(void)pkg_translate_app(plist, n, NULL, 0, &m);
	free(plist);
	free(exe);
}

/* deb / rpm / pacman with xz and zstd payloads: same manifest, same program. */
static void test_modern(void)
{
	static const struct {
		const char *file;
		enum pkg_format format;
		enum pkg_codec codec;
	} cases[] = {
		{ "pkg/hello_1.0-1_amd64.xz.deb", PKG_DEB, PKG_CODEC_XZ },
		{ "pkg/hello_1.0-1_amd64.zst.deb", PKG_DEB, PKG_CODEC_ZSTD },
		{ "pkg/hello-1.0-1.x86_64.xz.rpm", PKG_RPM, PKG_CODEC_XZ },
		{ "pkg/hello-1.0-1.x86_64.zst.rpm", PKG_RPM, PKG_CODEC_ZSTD },
		{ "pkg/hello-1.0-1-x86_64.pkg.tar.xz", PKG_PACMAN, PKG_CODEC_XZ },
		{ "pkg/hello-1.0-1-x86_64.pkg.tar.zst", PKG_PACMAN, PKG_CODEC_ZSTD },
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		struct pkg_manifest m;
		const char *hint = strrchr(cases[i].file, '/') + 1;
		size_t len;
		u8 *img = load_fixture_opt(cases[i].file, &len);

		if (!img) {
			printf("  (skipped %s: host tool missing)\n", cases[i].file);
			continue;
		}
		CHECK_EQ(pkg_translate(img, len, hint, &scratch, &m), 0);
		CHECK_EQ(m.format, cases[i].format);
		CHECK_EQ(m.payload_codec, cases[i].codec);
		CHECK_STR(m.name, "hello");
		CHECK_STR(m.version, "1.0-1");
		CHECK_STR(m.arch, "x86_64");
		CHECK(find_dep(&m, "glibc") || find_dep(&m, "libc6"));
		check_payload(img, len, &m);
		/* The kernel's 96 KiB scratch buffer is enough. */
		{
			static u8 small[96 * 1024];
			struct pkg_scratch s = { small, sizeof(small) };
			const u8 *exe;
			size_t exe_len;
			char path[64];

			CHECK_EQ(pkg_translate(img, len, hint, &s, &m), 0);
			CHECK_EQ(pkg_payload_exec(img, len, &m, &s, &exe, &exe_len, path, sizeof(path)), 0);
		}
		if (i % 2 == 0 || i == 5)
			torture(img, len, hint);
		free(img);
	}
}

/* AppImage, snap and flatpak: self-contained bundles. */
static void test_bundles(void)
{
	struct pkg_manifest m;
	const struct pkg_dep *d;
	char text[1024];
	size_t len;
	u8 *img;

	img = load_fixture_opt("pkg/Hello-1.0-x86_64.AppImage", &len);
	if (img) {
		CHECK_EQ(pkg_probe(img, len, NULL), PKG_APPIMAGE);
		CHECK(sqfs_find(img, len) > 0);
		CHECK_EQ(pkg_translate(img, len, NULL, &scratch, &m), 0);
		CHECK_STR(m.name, "Hello");
		CHECK_STR(m.version, "1.0");
		CHECK_STR(m.summary, "AppImage test bundle");
		CHECK_STR(m.arch, "x86_64");
		CHECK_STR(m.executable, "AppRun");
		CHECK_EQ(m.payload_codec, PKG_CODEC_ZSTD);
		CHECK(m.flags & PKG_F_RELOCATABLE);
		CHECK_STR(m.store_prefix, "/pkg/appimage/Hello/1.0");
		check_payload(img, len, &m);                    /* AppRun -> usr/bin/hello */
		CHECK(pkg_manifest_print(&m, text, sizeof(text)) > 0);
		CHECK(strstr(text, "executable   AppRun") != NULL);
		torture(img, len, NULL);
		free(img);
	}
	img = load_fixture_opt("pkg/hello_1.0_amd64.snap", &len);
	if (img) {
		CHECK_EQ(pkg_probe(img, len, NULL), PKG_SNAP);
		CHECK_EQ(pkg_translate(img, len, NULL, &scratch, &m), 0);
		CHECK_STR(m.name, "hello");
		CHECK_STR(m.version, "1.0");
		CHECK_STR(m.summary, "Snap test bundle");
		CHECK_STR(m.foreign_arch, "amd64");
		CHECK_STR(m.arch, "x86_64");
		CHECK_STR(m.executable, "bin/hello");
		CHECK_EQ(m.payload_codec, PKG_CODEC_XZ);
		d = find_dep(&m, "core22");
		CHECK(d && !strcmp(d->capability, "pkg:snap/core22"));
		CHECK_STR(m.store_prefix, "/pkg/snap/hello/1.0");
		check_payload(img, len, &m);
		torture(img, len, NULL);
		free(img);
	}
	img = load_fixture("pkg/org.example.Hello.flatpak", &len);
	CHECK_EQ(pkg_probe(img, len, "org.example.Hello.flatpak"), PKG_FLATPAK);
	CHECK_EQ(pkg_translate(img, len, "org.example.Hello.flatpak", &scratch, &m), 0);
	CHECK_STR(m.name, "org.example.Hello");
	CHECK_STR(m.bundle_id, "org.example.Hello");
	CHECK_STR(m.version, "stable");
	CHECK_STR(m.arch, "x86_64");
	CHECK_STR(m.executable, "hello");
	CHECK(!(m.flags & PKG_F_PAYLOAD_OK));                   /* OSTree objects: not decoded */
	d = find_dep(&m, "org.freedesktop.Platform/x86_64/23.08");
	CHECK(d != NULL);
	torture(img, len, "x.flatpak");
	free(img);
}

void test_pkg(void)
{
	struct pkg_manifest m;
	const struct pkg_dep *d;
	char path[256];
	size_t len;
	u8 *img;

	test_gzip();

	/* ---- .deb ------------------------------------------------------------ */
	img = load_fixture("pkg/hello_1.0-1_amd64.deb", &len);
	CHECK_EQ(pkg_probe(img, len, NULL), PKG_DEB);
	CHECK_EQ(pkg_translate(img, len, NULL, &scratch, &m), 0);
	CHECK_STR(m.name, "hello");
	CHECK_STR(m.version, "1.0-1");
	CHECK_STR(m.foreign_arch, "amd64");
	CHECK_STR(m.arch, "x86_64");
	CHECK_STR(m.libc, "glibc");
	CHECK_EQ(m.personality, ABI_LINUX);
	CHECK_EQ(m.meta_codec, PKG_CODEC_GZIP);
	CHECK(m.flags & PKG_F_SCRIPTS);
	CHECK_STR(m.store_prefix, "/pkg/deb/hello/1.0-1");
	CHECK_EQ(m.ndeps, 3);
	d = find_dep(&m, "libc6");
	CHECK(d && d->base && !strcmp(d->constraint, ">= 2.34") &&
	      !strcmp(d->capability, "runtime:linux-glibc"));
	d = find_dep(&m, "libgcc-s1");                          /* first of "a | b" */
	CHECK(d && d->base);
	CHECK(find_dep(&m, "libgcc1") == NULL);
	d = find_dep(&m, "zlib1g");
	CHECK(d && !d->base && !strcmp(d->capability, "pkg:deb/zlib1g"));
	check_payload(img, len, &m);
	/* Debian multiarch library path -> unified store layout. */
	CHECK(pkg_store_path(&m, "/usr/lib/x86_64-linux-gnu/libz.so.1", path, sizeof(path)) > 0);
	CHECK_STR(path, "/pkg/deb/hello/1.0-1/lib/libz.so.1");
	CHECK(pkg_store_path(&m, "./usr/bin/hello", path, sizeof(path)) > 0);
	CHECK_STR(path, "/pkg/deb/hello/1.0-1/bin/hello");
	CHECK(pkg_store_path(&m, "/opt/hello/data", path, sizeof(path)) > 0);
	CHECK_STR(path, "/pkg/deb/hello/1.0-1/root/opt/hello/data");
	CHECK_EQ(pkg_store_path(&m, "/usr/bin/hello", path, 10), -MERR_RANGE);
	CHECK(pkg_manifest_print(&m, path, sizeof(path)) < (int)sizeof(path));
	torture(img, len, NULL);
	free(img);

	/* ---- .rpm ------------------------------------------------------------ */
	img = load_fixture("pkg/hello-1.0-1.x86_64.rpm", &len);
	CHECK_EQ(pkg_translate(img, len, NULL, &scratch, &m), 0);
	CHECK_EQ(m.format, PKG_RPM);
	CHECK_STR(m.name, "hello");
	CHECK_STR(m.version, "1.0-1");                          /* version-release */
	CHECK_STR(m.arch, "x86_64");
	CHECK_EQ(m.payload_codec, PKG_CODEC_GZIP);
	CHECK_EQ(m.deps_total, 3);                              /* rpmlib() filtered out */
	d = find_dep(&m, "glibc");
	CHECK(d && d->base && !strcmp(d->constraint, ">= 2.34"));
	d = find_dep(&m, "libc.so.6(GLIBC_2.2.5)(64bit)");
	CHECK(d && d->base && !strcmp(d->capability, "runtime:linux-glibc"));
	d = find_dep(&m, "/bin/sh");
	CHECK(d && !strcmp(d->capability, "runtime:posix-sh"));
	check_payload(img, len, &m);
	/* Same library, Fedora's lib64 layout: lands in the same store directory. */
	CHECK(pkg_store_path(&m, "/usr/lib64/libz.so.1", path, sizeof(path)) > 0);
	CHECK_STR(path, "/pkg/rpm/hello/1.0-1/lib/libz.so.1");
	torture(img, len, NULL);
	free(img);

	/* ---- .apk: three gzip members, musl ---------------------------------- */
	img = load_fixture("pkg/hello-1.0-r0.apk", &len);
	CHECK_EQ(pkg_probe(img, len, NULL), PKG_APK);           /* by content alone */
	CHECK_EQ(pkg_translate(img, len, "hello-1.0-r0.apk", &scratch, &m), 0);
	CHECK_EQ(m.format, PKG_APK);
	CHECK_STR(m.version, "1.0-r0");
	CHECK_STR(m.libc, "musl");
	d = find_dep(&m, "so:libc.musl-x86_64.so.1");
	CHECK(d && d->base && !strcmp(d->capability, "runtime:linux-musl"));
	CHECK(m.payload_off > 0);                               /* payload = a later member */
	check_payload(img, len, &m);
	torture(img, len, "x.apk");
	free(img);

	/* ---- pacman ---------------------------------------------------------- */
	img = load_fixture("pkg/hello-1.0-1-x86_64.pkg.tar.gz", &len);
	CHECK_EQ(pkg_probe(img, len, NULL), PKG_PACMAN);
	CHECK_EQ(pkg_translate(img, len, NULL, &scratch, &m), 0);
	CHECK_EQ(m.format, PKG_PACMAN);
	CHECK_STR(m.libc, "glibc");
	d = find_dep(&m, "gcc-libs");
	CHECK(d && d->base && !strcmp(d->constraint, ">=13"));
	check_payload(img, len, &m);
	/* A decode buffer too small for the payload is reported, not overrun. */
	{
		struct pkg_scratch tiny = { scratch_buf, 2048 };
		const u8 *exe;
		size_t exe_len;

		CHECK_EQ(pkg_payload_exec(img, len, &m, &tiny, &exe, &exe_len, path, sizeof(path)),
			 -MERR_RANGE);
	}
	torture(img, len, NULL);
	free(img);

	test_modern();
	test_bundles();

	/* ---- recognised, but damaged ------------------------------------------ */
	img = load_fixture("neg/hello_modern.deb", &len);       /* garbage control.tar.xz */
	CHECK_EQ(pkg_translate(img, len, NULL, &scratch, &m), -MERR_NOEXEC);
	CHECK_EQ(m.format, PKG_DEB);
	CHECK_EQ(m.meta_codec, PKG_CODEC_XZ);
	free(img);
	img = load_fixture("neg/hello-1.0-1-x86_64.pkg.tar.zst", &len);
	CHECK_EQ(pkg_translate(img, len, "hello-1.0-1-x86_64.pkg.tar.zst", &scratch, &m),
		 -MERR_NOEXEC);
	CHECK_EQ(m.format, PKG_PACMAN);
	CHECK_EQ(m.meta_codec, PKG_CODEC_ZSTD);
	free(img);
	img = load_fixture("neg/hello.snap", &len);
	CHECK_EQ(pkg_probe(img, len, NULL), PKG_SNAP);
	CHECK_EQ(pkg_translate(img, len, NULL, &scratch, &m), -MERR_NOEXEC);
	free(img);
	img = load_fixture("neg/Hello-x86_64.AppImage", &len);  /* runtime, no squashfs */
	CHECK_EQ(pkg_probe(img, len, NULL), PKG_APPIMAGE);
	CHECK_EQ(pkg_translate(img, len, NULL, &scratch, &m), -MERR_NOEXEC);
	free(img);

	/* ---- not packages / damaged ------------------------------------------ */
	img = load_fixture("neg/not-a-package.txt", &len);
	CHECK_EQ(pkg_translate(img, len, NULL, &scratch, &m), -MERR_NOEXEC);
	free(img);
	img = load_fixture("neg/truncated.deb", &len);
	CHECK(pkg_translate(img, len, NULL, &scratch, &m) < 0);
	free(img);
	img = load_fixture("neg/corrupt.apk", &len);
	CHECK(pkg_translate(img, len, "corrupt.apk", &scratch, &m) < 0);
	free(img);
	img = load_fixture("bin/hello.linux", &len);            /* an ELF is not a package */
	CHECK_EQ(pkg_probe(img, len, NULL), PKG_UNKNOWN);
	free(img);

	test_app();
}
