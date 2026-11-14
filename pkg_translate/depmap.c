/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Dependency translation: distro package names -> distro-neutral capabilities.
 *
 * "libc6" (Debian), "glibc" (Fedora, Arch) and the automatic RPM dependency
 * "libc.so.6(GLIBC_2.34)(64bit)" all mean the same thing: the GNU C runtime
 * for the Linux personality. Mapping them onto one capability name is what
 * lets packages from different distributions share one installed runtime.
 *
 *   runtime:<name>   a MergedOS base runtime           (base = true)
 *   bundle:<file>    shipped inside the bundle itself  (base = true)
 *   soname:<lib>     any package providing that shared object
 *   framework:<name> a macOS framework
 *   cmd:<name>       any package providing that command
 *   pkg:<family>/<n> a package of the same family (resolved by the package DB)
 */
#include "pkg_translate/pkg_internal.h"

static const struct {
	const char *name;
	const char *capability;
} base_map[] = {
	{ "libc6",          "runtime:linux-glibc" },
	{ "glibc",          "runtime:linux-glibc" },
	{ "libgcc-s1",      "runtime:linux-gcc" },
	{ "libgcc1",        "runtime:linux-gcc" },
	{ "libgcc",         "runtime:linux-gcc" },
	{ "gcc-libs",       "runtime:linux-gcc" },
	{ "libstdc++6",     "runtime:linux-gcc" },
	{ "libstdc++",      "runtime:linux-gcc" },
	{ "musl",           "runtime:linux-musl" },
	{ "/bin/sh",        "runtime:posix-sh" },
	{ "sh",             "runtime:posix-sh" },
	{ "bash",           "runtime:posix-sh" },
	{ "busybox",        "runtime:posix-sh" },
	{ "dash",           "runtime:posix-sh" },
};

/* Shared objects that belong to a base runtime, matched by soname prefix. */
static const struct {
	const char *soname;
	const char *capability;
} soname_map[] = {
	{ "libc.so.6",      "runtime:linux-glibc" },
	{ "libm.so.6",      "runtime:linux-glibc" },
	{ "libpthread.so.0", "runtime:linux-glibc" },
	{ "libdl.so.2",     "runtime:linux-glibc" },
	{ "ld-linux-x86-64.so.2", "runtime:linux-glibc" },
	{ "libc.musl-",     "runtime:linux-musl" },
	{ "libgcc_s.so.1",  "runtime:linux-gcc" },
	{ "libstdc++.so.6", "runtime:linux-gcc" },
};

const char *pkg_family(enum pkg_format f)
{
	switch (f) {
	case PKG_DEB:       return "deb";
	case PKG_RPM:       return "rpm";
	case PKG_APK:       return "apk";
	case PKG_PACMAN:    return "pacman";
	case PKG_MACOS_APP: return "app";
	case PKG_MERGED:    return "merged";
	case PKG_APPIMAGE:  return "appimage";
	case PKG_SNAP:      return "snap";
	case PKG_FLATPAK:   return "flatpak";
	default:            return "misc";
	}
}

void pkg_set_arch(struct pkg_manifest *m, const char *foreign)
{
	static const struct {
		const char *foreign, *native;
	} map[] = {
		{ "amd64", "x86_64" }, { "x86_64", "x86_64" }, { "x86-64", "x86_64" },
		{ "arm64", "aarch64" }, { "aarch64", "aarch64" },
		{ "i386", "i386" }, { "i486", "i386" }, { "i586", "i386" }, { "i686", "i386" },
		{ "x86", "i386" },
		{ "all", "noarch" }, { "any", "noarch" }, { "noarch", "noarch" },
	};

	mos_strlcpy(m->foreign_arch, foreign, sizeof(m->foreign_arch));
	mos_strlcpy(m->arch, foreign[0] ? foreign : "unknown", sizeof(m->arch));
	for (size_t i = 0; i < ARRAY_SIZE(map); i++)
		if (!mos_strcasecmp(foreign, map[i].foreign))
			mos_strlcpy(m->arch, map[i].native, sizeof(m->arch));
}

static struct pkg_dep *new_dep(struct pkg_manifest *m, const char *name, const char *constraint)
{
	struct pkg_dep *d;

	m->deps_total++;
	if (m->ndeps == PKG_MAX_DEPS)
		return NULL;
	d = &m->deps[m->ndeps++];
	memset(d, 0, sizeof(*d));
	mos_strlcpy(d->name, name, sizeof(d->name));
	mos_strlcpy(d->constraint, constraint ? constraint : "", sizeof(d->constraint));
	return d;
}

static bool map_soname(struct pkg_dep *d, const char *soname)
{
	char lib[64];
	size_t n = 0;

	/* Strip RPM decorations: "libc.so.6(GLIBC_2.34)(64bit)" -> "libc.so.6". */
	while (soname[n] && soname[n] != '(' && n < sizeof(lib) - 1)
		n++;
	mos_fieldcpy(lib, sizeof(lib), soname, n);

	for (size_t i = 0; i < ARRAY_SIZE(soname_map); i++) {
		if (mos_has_prefix(lib, soname_map[i].soname)) {
			mos_strlcpy(d->capability, soname_map[i].capability, sizeof(d->capability));
			d->base = true;
			return true;
		}
	}
	mos_snprintf(d->capability, sizeof(d->capability), "soname:%s", lib);
	return false;
}

void pkg_add_dep(struct pkg_manifest *m, const char *name, const char *constraint)
{
	struct pkg_dep *d = new_dep(m, name, constraint);

	if (!d)
		return;
	for (size_t i = 0; i < ARRAY_SIZE(base_map); i++) {
		if (!strcmp(name, base_map[i].name)) {
			mos_strlcpy(d->capability, base_map[i].capability, sizeof(d->capability));
			d->base = true;
			goto libc;
		}
	}
	if (mos_has_prefix(name, "so:")) {                      /* apk shared-object dependency */
		map_soname(d, name + 3);
	} else if (mos_has_prefix(name, "cmd:")) {              /* apk command dependency */
		mos_strlcpy(d->capability, name, sizeof(d->capability));
	} else if (mos_memstr(name, strlen(name), ".so")) {     /* rpm automatic dependency */
		map_soname(d, name);
	} else if (name[0] == '/') {                            /* rpm file dependency */
		mos_snprintf(d->capability, sizeof(d->capability), "file:%s", name);
	} else {
		mos_snprintf(d->capability, sizeof(d->capability), "pkg:%s/%s",
			     pkg_family(m->format), name);
	}
libc:
	/* The C library a package links against decides its runtime flavour. */
	if (!strcmp(d->capability, "runtime:linux-musl"))
		mos_strlcpy(m->libc, "musl", sizeof(m->libc));
	else if (!strcmp(d->capability, "runtime:linux-glibc"))
		mos_strlcpy(m->libc, "glibc", sizeof(m->libc));
}

/*
 * macOS install names:
 *   /usr/lib/libSystem.B.dylib                           -> runtime:darwin-libsystem
 *   /System/Library/Frameworks/AppKit.framework/...      -> framework:AppKit
 *   @rpath/libfoo.dylib, @executable_path/../Frameworks/ -> bundle:libfoo.dylib
 */
void pkg_add_dylib_dep(struct pkg_manifest *m, const char *install_name)
{
	struct pkg_dep *d = new_dep(m, install_name, "");
	const char *base = install_name, *fw;

	if (!d)
		return;
	for (const char *p = install_name; *p; p++)
		if (*p == '/')
			base = p + 1;

	fw = mos_memstr(install_name, strlen(install_name), ".framework/");
	if (install_name[0] == '@') {
		mos_snprintf(d->capability, sizeof(d->capability), "bundle:%s", base);
		d->base = true;
	} else if (fw) {
		const char *start = fw;

		while (start > install_name && start[-1] != '/')
			start--;
		mos_strlcpy(d->capability, "framework:", sizeof(d->capability));
		mos_fieldcpy(d->capability + 10, sizeof(d->capability) - 10, start,
			     (size_t)(fw - start));
	} else if (mos_has_prefix(base, "libSystem")) {
		mos_strlcpy(d->capability, "runtime:darwin-libsystem", sizeof(d->capability));
		d->base = true;
	} else {
		mos_snprintf(d->capability, sizeof(d->capability), "dylib:%s", base);
	}
}
