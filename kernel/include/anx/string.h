/*
 * anx/string.h — Kernel string and memory functions.
 */

#ifndef ANX_STRING_H
#define ANX_STRING_H

#include <anx/types.h>

/* Memory operations */
void *anx_memcpy(void *dst, const void *src, size_t n);
void *anx_memset(void *dst, int val, size_t n);
int   anx_memcmp(const void *a, const void *b, size_t n);
void *anx_memmove(void *dst, const void *src, size_t n);

/* String operations */
size_t anx_strlen(const char *s);
int    anx_strcmp(const char *a, const char *b);
int    anx_strncmp(const char *a, const char *b, size_t n);
size_t anx_strlcpy(char *dst, const char *src, size_t dstsize);

#endif /* ANX_STRING_H */
