/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_PKG_H
#define MERGEDOS_PKG_H

/*
 * Package-to-native translation layer.
 *
 * Foreign packages are not converted or repacked. They are *read in their
 * own format* and described by one native manifest:
 *
 *   identity      name, version, architecture (normalised)
 *   personality   which ABI personality its programs need (abi_dispatch/)
 *   dependencies  distro package names mapped to distro-neutral capabilities
 *   store prefix  where the payload lives: /pkg/<family>/<name>/<version>
 *   path view     how the paths the package expects (/usr/bin, /usr/lib64,
 *                 /usr/lib/x86_64-linux-gnu, ...) resolve inside that prefix
 *
 * At run time the launcher uses the manifest to pick the personality and to
 * give the process the file-system view its distribution would have given
 * it. A macOS .app bundle goes through the same manifest, built from its
 * Info.plist and the load commands of its main executable.
 *
 * Everything here is allocation-free and works on caller-supplied buffers,
 * so the same code runs in the kernel, in user space and in host tools.
 */
#include "abi_dispatch/abi.h"

enum pkg_format {
	PKG_UNKNOWN = 0,
	PKG_DEB,                /* Debian, Ubuntu, Mint, ...             */
	PKG_RPM,                /* Fedora, RHEL, openSUSE, ...           */
	PKG_APK,                /* Alpine                                */
	PKG_PACMAN,             /* Arch, Manjaro (.pkg.tar.*)            */
	PKG_APPIMAGE,           /* distro-independent, ELF + squashfs    */
	PKG_SNAP,               /* squashfs image                        */
	PKG_FLATPAK,            /* OSTree bundle                         */
	PKG_NIX_NAR,            /* Nix archive                           */
	PKG_MACOS_APP,          /* .app bundle (directory)               */
	PKG_MACOS_DMG,          /* UDIF disk image                       */
	PKG_MACOS_XAR,          /* .pkg installer (xar)                  */
	PKG_MERGED,             /* native .merged package                */
	PKG_FORMAT_COUNT
};

enum pkg_codec {
	PKG_CODEC_NONE = 0,     /* stored */
	PKG_CODEC_GZIP,
	PKG_CODEC_XZ,
	PKG_CODEC_ZSTD,
	PKG_CODEC_BZIP2,
	PKG_CODEC_LZ4,          /* squashfs blocks only */
	PKG_CODEC_UNKNOWN
};

#define PKG_MAX_DEPS 16

struct pkg_dep {
	char name[64];          /* as written in the package              */
	char constraint[24];    /* e.g. ">= 2.34", or ""                  */
	char capability[80];    /* distro-neutral name it resolves to     */
	bool base;              /* provided by a MergedOS base runtime or
				   by the bundle itself, not another package */
	u16 nimports;           /* .app: symbols the executable binds from this
				   library (counted for libraries not provided)  */
	char imports[72];       /* ... the first few, space separated            */
};

#define PKG_F_SCRIPTS      0x1  /* has install-time maintainer scripts            */
#define PKG_F_PAYLOAD_OK   0x2  /* payload can be read by the built-in codecs     */
#define PKG_F_RELOCATABLE  0x4  /* self-contained: runs from wherever it is       */

struct pkg_manifest {
	enum pkg_format format;
	char name[64];
	char version[48];
	char arch[16];                  /* normalised: x86_64, aarch64, noarch */
	char foreign_arch[16];          /* as written: amd64, x86_64, any ...  */
	char summary[96];
	char bundle_id[96];             /* .app: CFBundleIdentifier; flatpak id */
	char min_os[16];                /* .app: LSMinimumSystemVersion        */
	char executable[128];           /* .app, AppImage, snap, flatpak:
					   program path inside the bundle      */
	enum abi_personality personality;
	char libc[12];                  /* glibc, musl, libSystem              */
	enum pkg_codec meta_codec, payload_codec;
	u64 payload_off, payload_size;  /* location of the payload in the file */
	u32 ndeps, deps_total;
	struct pkg_dep deps[PKG_MAX_DEPS];
	u32 flags;
	char store_prefix[160];
};

/* Decode buffer supplied by the caller (metadata needs a few KiB). */
struct pkg_scratch {
	u8 *buf;
	size_t cap;
};

/* Identify a package file. `name_hint` (file name, may be NULL) breaks ties. */
enum pkg_format pkg_probe(const u8 *img, size_t len, const char *name_hint);

/*
 * Build the native manifest for a package file.
 *   0             success
 *  -MERR_NOEXEC   not a package / corrupt
 *  -MERR_NOTSUP   recognised, but needs a codec or backend this build lacks;
 *                 `m->format` and the codec fields say which
 */
int pkg_translate(const u8 *img, size_t len, const char *name_hint,
		  struct pkg_scratch *scratch, struct pkg_manifest *m);

/*
 * Build the manifest for a macOS .app bundle from its Contents/Info.plist
 * and, optionally, its main executable (used for dependencies).
 */
int pkg_translate_app(const u8 *plist, size_t plist_len, const u8 *exe, size_t exe_len,
		      struct pkg_manifest *m);

/*
 * Locate the package's program (first regular file in a bin directory)
 * inside the payload, decoding into `scratch` if needed.
 */
int pkg_payload_exec(const u8 *img, size_t len, const struct pkg_manifest *m,
		     struct pkg_scratch *scratch, const u8 **exe, size_t *exe_len,
		     char *path, size_t path_cap);

/* Path view: where a path the package refers to lives in the native store. */
int pkg_store_path(const struct pkg_manifest *m, const char *foreign_path, char *out,
		   size_t cap);

/* Render a manifest as text. Returns the length written. */
int pkg_manifest_print(const struct pkg_manifest *m, char *out, size_t cap);

const char *pkg_format_name(enum pkg_format f);

/*
 * Native .merged package (merged.c). Layout, all integers little-endian:
 *
 *   0   "MRGD"
 *   4   u32 format version (MERGED_VERSION)
 *   8   u32 manifest length
 *   12  u32 payload length
 *   16  manifest: UTF-8 "key: value" lines (name, version, type, [summary])
 *   ..  payload
 *   ..  32-byte SHA-256 over every byte before it
 */
#define MERGED_MAGIC        "MRGD"
#define MERGED_VERSION      1u
#define MERGED_HDR_LEN      16u
#define MERGED_HASH_LEN     32u
#define MERGED_MANIFEST_MAX 4096u
#define MERGED_PAYLOAD_MAX  (16u << 20)

enum merged_type {
	MERGED_TYPE_APP = 1,
	MERGED_TYPE_BLOCKLIST,
};

struct merged_pkg {
	char name[64];
	char version[48];
	char summary[96];
	enum merged_type type;
	const u8 *payload;      /* points into the caller's image */
	size_t payload_len;
};

bool merged_probe(const u8 *img, size_t len);
/*
 * Parse and verify. 0 on success; -MERR_NOEXEC for a bad magic, truncation,
 * bad hash or bad manifest; -MERR_NOTSUP for an unknown format version;
 * -MERR_RANGE for oversized sections.
 */
int merged_parse(const u8 *img, size_t len, struct merged_pkg *out);
const char *merged_type_name(enum merged_type t);
const char *pkg_codec_name(enum pkg_codec c);

#endif /* MERGEDOS_PKG_H */
