/* SPDX-License-Identifier: BSD-2-Clause */
/* Prototypes for the libc subset exported by MergedOS's libSystem.B.dylib. */
#ifndef MINI_H
#define MINI_H
#include <stdarg.h>
#include <stddef.h>

typedef struct sfile FILE;
extern FILE *__stdinp, *__stdoutp, *__stderrp;
#define stdin  __stdinp
#define stdout __stdoutp
#define stderr __stderrp
#ifndef NULL
#define NULL ((void *)0)
#endif

int printf(const char *, ...);
int fprintf(FILE *, const char *, ...);
int snprintf(char *, size_t, const char *, ...);
int puts(const char *);
int fputs(const char *, FILE *);
FILE *fopen(const char *, const char *);
int fclose(FILE *);
char *fgets(char *, int, FILE *);
size_t fread(void *, size_t, size_t, FILE *);
size_t fwrite(const void *, size_t, size_t, FILE *);
int fflush(FILE *);
void *malloc(size_t);
void free(void *);
void *memset(void *, int, size_t);
void *memcpy(void *, const void *, size_t);
size_t strlen(const char *);
int strcmp(const char *, const char *);
char *strcpy(char *, const char *);
char *getenv(const char *);
int unlink(const char *);
int stat(const char *, void *);
void exit(int);
int *__error(void);
#define errno (*__error())
#endif
