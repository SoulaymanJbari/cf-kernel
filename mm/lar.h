#ifndef _MM_LAR_H
#define _MM_LAR_H

#include <linux/mmzone.h>

#ifdef CONFIG_ZONE_LAR

extern unsigned long lar_zone_start_pfn;

struct page *alloc_lar_page(gfp_t gfp_mask, int preferred_nid);
void free_lar_page(struct page *page);
void init_lar_zone(struct zone *zone, unsigned long size);

int get_subarray_idx(struct page *page);
struct page *alloc_same_subarray(struct page *old_page, unsigned long subarray_idx);

#else

static inline struct page *alloc_lar_page(gfp_t gfp_mask, int preferred_nid) { return NULL; }
static inline void free_lar_page(struct page *page) { }
static inline void init_lar_zone(struct zone *zone, unsigned long size) { }
static inline int get_subarray_idx(struct page *page) { return -1; }

#endif

#endif