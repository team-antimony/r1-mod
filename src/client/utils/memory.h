/* Fault-tolerant reads of game memory. */
#ifndef R1_MEMORY_H
#define R1_MEMORY_H

#include <stddef.h>

/* copy a C string from p; 0 on an access fault */
int safe_str(const void *p, char *dst, size_t n);
/* read a pointer at p; 0 on an access fault */
int safe_ptr(const void *p, void **out);

#endif
