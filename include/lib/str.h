#ifndef __STR_H__
#define __STR_H__

#include "common.h"

void* memset(void *dst, int c, uint32 n);
int memcmp(const void *v1, const void *v2, uint32 n);
void* memmove(void *dst, const void *src, uint32 n);

void* memcpy(void *dst, const void *src, uint32 n);
int strncmp(const char *p, const char *q, uint32 n);
char* strncpy(char *s, const char *t, int n);

char* safestrcpy(char *s, const char *t, int n);
int strlen(const char *s);

#endif