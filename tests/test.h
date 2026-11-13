/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_TEST_H
#define MERGEDOS_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mergedos/compat.h>

extern int test_checks, test_failures;

#define CHECK(cond)                                                            \
	do {                                                                   \
		test_checks++;                                                 \
		if (!(cond)) {                                                 \
			test_failures++;                                       \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}                                                              \
	} while (0)

#define CHECK_EQ(a, b)                                                         \
	do {                                                                   \
		long long a_ = (long long)(a), b_ = (long long)(b);            \
		test_checks++;                                                 \
		if (a_ != b_) {                                                \
			test_failures++;                                       \
			printf("  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, \
			       #a, #b, a_, b_);                                \
		}                                                              \
	} while (0)

#define CHECK_STR(a, b)                                                        \
	do {                                                                   \
		const char *a_ = (a), *b_ = (b);                               \
		test_checks++;                                                 \
		if (strcmp(a_, b_)) {                                          \
			test_failures++;                                       \
			printf("  FAIL %s:%d: \"%s\" vs \"%s\"\n", __FILE__, __LINE__, a_, b_); \
		}                                                              \
	} while (0)

/* Mock task for the dispatcher tests: a window of fake user memory at `base`. */
#define MOCK_MEM_SIZE 4096
struct task {
	int pid;
	u64 base;
	u8 mem[MOCK_MEM_SIZE];
	char out[512];
	size_t out_len;
	bool exited;
	int exit_code;
};

/* Load build/fixtures/<rel>; aborts the run if it is missing. Caller frees. */
u8 *load_fixture(const char *rel, size_t *len);
/* Same, but NULL when absent (fixtures that need optional host tools). */
u8 *load_fixture_opt(const char *rel, size_t *len);

void test_binfmt(void);
void test_pe(void);
void test_dispatch(void);
void test_de_policy(void);
void test_msh(void);
void test_pkg(void);
void test_codec(void);
void test_squashfs(void);
void test_ui(void);
void test_storage(void);
void test_desktop(void);
void test_fence(void);
void test_filetype(void);
void test_ramfs(void);
void test_darwin(void);

/* Recording back end for the ui/ toolkit (test_main.c). */
extern char mock_draw_log[8192];
extern size_t mock_draw_len;
extern int mock_fill_count, mock_blend_count;
/* Bounding box of all drawing since mock_bounds_reset() (x1/y1 exclusive). */
extern int mock_x0, mock_y0, mock_x1, mock_y1;
void mock_bounds_reset(void);

#endif /* MERGEDOS_TEST_H */
