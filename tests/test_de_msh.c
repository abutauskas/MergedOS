/* SPDX-License-Identifier: BSD-2-Clause */
#include "tests/test.h"
#include "de_selector/de_selector.h"
#include "src/msh/msh.h"

static enum de_id pick(u32 ram_mib, enum hw_gpu_tier tier, u32 cpus)
{
	struct hw_profile hw = { .ram_mib = ram_mib, .gpu_tier = tier, .cpus = cpus,
				 .ngpu = tier != HW_GPU_NONE };
	struct de_recommendation rec;

	de_recommend(&hw, &rec);
	return rec.best;
}

void test_de_policy(void)
{
	struct hw_profile tiny = { .ram_mib = 8, .gpu_tier = HW_GPU_BASIC, .cpus = 1 };
	struct de_recommendation rec;
	enum hw_gpu_tier tier;
	const char *name;

	/* Very low memory: the 16-colour profile is all that fits. */
	CHECK_EQ(pick(24, HW_GPU_BASIC, 1), DE_RETRO);
	CHECK_EQ(pick(40, HW_GPU_BASIC, 1), DE_RETRO);
	/* Low memory: ultra-light tiling. */
	CHECK_EQ(pick(48, HW_GPU_BASIC, 1), DE_TILING);
	CHECK_EQ(pick(63, HW_GPU_BASIC, 1), DE_TILING);         /* QEMU -m 64 */
	CHECK_EQ(pick(256, HW_GPU_BASIC, 1), DE_TILING);
	CHECK_EQ(pick(512, HW_GPU_BASIC, 1), DE_TILING);
	/* Mid-range: a conventional taskbar desktop. */
	CHECK_EQ(pick(1024, HW_GPU_PARAVIRT, 2), DE_TASKBAR);
	CHECK_EQ(pick(2048, HW_GPU_INTEGRATED, 4), DE_TASKBAR);
	/* Plenty of RAM and a real GPU: the composited desktop. */
	CHECK_EQ(pick(4096, HW_GPU_INTEGRATED, 4), DE_DOCK);
	CHECK_EQ(pick(8192, HW_GPU_DISCRETE, 8), DE_DOCK);
	CHECK_EQ(pick(32768, HW_GPU_DISCRETE, 16), DE_DOCK);
	/* RAM alone does not buy compositing: no acceleration, no dock. */
	CHECK_EQ(pick(16384, HW_GPU_BASIC, 8), DE_TASKBAR);
	/* Nor does a GPU on a single core. */
	CHECK_EQ(pick(4096, HW_GPU_DISCRETE, 1), DE_TASKBAR);
	/* Headless machine: only the profile that needs no display controller. */
	CHECK_EQ(pick(8192, HW_GPU_NONE, 4), DE_RETRO);

	/* Below every minimum: nothing is eligible, retro is the stated fallback. */
	de_recommend(&tiny, &rec);
	CHECK_EQ(rec.best, DE_RETRO);
	for (int i = 0; i < DE_COUNT; i++) {
		CHECK(!rec.score[i].eligible);
		CHECK(rec.score[i].reason[0] != '\0');
	}

	/* Scores are bounded by the profile weight and monotonic in RAM. */
	for (int id = 0; id < DE_COUNT; id++) {
		u32 prev = 0;

		for (u32 ram = 16; ram <= 16384; ram *= 2) {
			struct hw_profile hw = { .ram_mib = ram, .gpu_tier = HW_GPU_DISCRETE,
						 .cpus = 8 };

			de_recommend(&hw, &rec);
			CHECK(rec.score[id].score <= de_profile((enum de_id)id)->weight);
			CHECK(rec.score[id].score >= prev);
			prev = rec.score[id].score;
		}
		CHECK_EQ(prev, de_profile((enum de_id)id)->weight);
	}

	CHECK_EQ(de_lookup("DOCK"), DE_DOCK);
	CHECK_EQ(de_lookup("tiling"), DE_TILING);
	CHECK_EQ(de_lookup("gnome"), -1);
	CHECK(de_profile(DE_COUNT) == NULL);

	hw_gpu_classify(0x10DE, 0x2684, &tier, &name);
	CHECK_EQ(tier, HW_GPU_DISCRETE);
	hw_gpu_classify(0x8086, 0x9A49, &tier, &name);
	CHECK_EQ(tier, HW_GPU_INTEGRATED);
	hw_gpu_classify(0x8086, 0x56A0, &tier, &name);          /* Arc A770 */
	CHECK_EQ(tier, HW_GPU_DISCRETE);
	hw_gpu_classify(0x1AF4, 0x1050, &tier, &name);
	CHECK_EQ(tier, HW_GPU_PARAVIRT);
	hw_gpu_classify(0x1234, 0x1111, &tier, &name);
	CHECK_EQ(tier, HW_GPU_BASIC);
}

static void check_path(const char *in, const char *expect)
{
	char out[128];

	CHECK(msh_path_to_native(in, out, sizeof(out)) >= 0);
	CHECK_STR(out, expect);
}

void test_msh(void)
{
	enum msh_dialect d;
	const struct msh_cmd *ls = msh_resolve("ls", &d);
	char line[] = "copy \"C:\\Program Files\\a b.txt\" 'x y'   z";
	char *argv[8];
	char small[8];

	/* Three vocabularies, one builtin. */
	CHECK(ls && ls->id == MSH_LS && d == MSH_POSIX);
	CHECK(msh_resolve("dir", &d) == ls && d == MSH_CMD);
	CHECK(msh_resolve("Get-ChildItem", &d) == ls && d == MSH_POWERSHELL);
	CHECK(msh_resolve("gci", NULL) == ls);
	CHECK(msh_resolve("tasklist", NULL) == msh_resolve("ps", NULL));
	CHECK(msh_resolve("Get-Process", NULL) == msh_resolve("ps", NULL));
	CHECK(msh_resolve("type", NULL) == msh_resolve("cat", NULL));
	CHECK(msh_resolve("cls", NULL) == msh_resolve("clear", NULL));
	CHECK(msh_resolve("del", NULL) == msh_resolve("rm", NULL));
	CHECK(msh_resolve("start", NULL) == msh_resolve("open", NULL));
	/* Windows names are case-insensitive, POSIX names are not. */
	CHECK(msh_resolve("DIR", NULL) == ls);
	CHECK(msh_resolve("get-childitem", NULL) == ls);
	CHECK(msh_resolve("LS", NULL) == NULL);
	CHECK(msh_resolve("frobnicate", NULL) == NULL);
	CHECK(msh_resolve("rm", NULL)->needs_vfs);
	CHECK(!ls->needs_vfs);

	/* Quotes group, backslashes survive. */
	CHECK_EQ(msh_tokenize(line, argv, 8), 4);
	CHECK_STR(argv[0], "copy");
	CHECK_STR(argv[1], "C:\\Program Files\\a b.txt");
	CHECK_STR(argv[2], "x y");
	CHECK_STR(argv[3], "z");
	{
		char empty[] = "   ", many[] = "a b c d";

		CHECK_EQ(msh_tokenize(empty, argv, 8), 0);
		CHECK_EQ(msh_tokenize(many, argv, 2), 2);        /* capped, no overflow */
	}

	check_path("C:\\Users\\me\\a.txt", "/Users/me/a.txt");
	check_path("c:/bin/hello.exe", "/bin/hello.exe");
	check_path("C:\\", "/");
	check_path("C:", "/");
	check_path("D:\\data\\x", "/vol/d/data/x");
	check_path("D:", "/vol/d");
	check_path("\\\\server\\share\\x", "/net/server/share/x");
	check_path("dir\\file", "dir/file");
	check_path("/usr/bin/env", "/usr/bin/env");
	check_path("./bin/hello.linux", "./bin/hello.linux");
	check_path("file:name", "file:name");                   /* not a drive letter */
	CHECK_EQ(msh_path_to_native("C:\\a\\long\\path", small, sizeof(small)), -MERR_RANGE);
}
