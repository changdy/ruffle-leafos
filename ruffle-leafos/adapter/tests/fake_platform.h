#ifndef RUFFLE_FAKE_PLATFORM_H
#define RUFFLE_FAKE_PLATFORM_H
#include <stddef.h>
#define RTLD_NEXT ((void *)-1)
#define CLOCK_MONOTONIC 1
struct timespec { long tv_sec, tv_nsec; };
int clock_gettime(int, struct timespec *);
void *dlsym(void *, const char *);
int dprintf(int, const char *, ...);
char *getenv(const char *);
int strcmp(const char *, const char *);
char *strstr(const char *, const char *);
void *memset(void *, int, size_t);
void _exit(int);
#endif
