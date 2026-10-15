/*
 * pghdr_alloc.h - bridge HdrHistogram_c's allocator to PostgreSQL's palloc.
 *
 * The vendored HdrHistogram_c sources include this header (via the
 * HDR_MALLOC_INCLUDE macro, overridden in the Makefile) instead of their own
 * hdr_malloc.h, so that hdr_malloc / hdr_calloc / hdr_realloc / hdr_free map
 * onto palloc / palloc0 / repalloc / pfree.  This puts all histogram memory
 * into PostgreSQL memory contexts, so it is reclaimed automatically - even on
 * mid-query errors.  The implementations live in hdr_histogram.c.
 */

#ifndef PGHDR_ALLOC_H
#define PGHDR_ALLOC_H

#include <stddef.h>

void *pghdr_alloc(size_t size);
void *pghdr_calloc(size_t count, size_t size);
void *pghdr_realloc(void *ptr, size_t size);
void pghdr_free(void *ptr);

#define hdr_malloc(sz)        pghdr_alloc(sz)
#define hdr_calloc(n, sz)     pghdr_calloc((n), (sz))
#define hdr_realloc(ptr, sz)  pghdr_realloc((ptr), (sz))
#define hdr_free(ptr)         pghdr_free(ptr)

#endif
