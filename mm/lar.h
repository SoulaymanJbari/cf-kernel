#ifndef _MM_LAR_H
#define _MM_LAR_H

#include <linux/mmzone.h>

#ifdef CONFIG_ZONE_LAR

extern unsigned long lar_zone_start_pfn;

/* ==================== DRAM TOPOLOGY ==================== */

static inline int lar_pfn_to_subarray_idx(unsigned long pfn)
{
	unsigned long rel_pfn = pfn - lar_zone_start_pfn;
	phys_addr_t pa = (phys_addr_t)rel_pfn << PAGE_SHIFT;
#if defined(CONFIG_LAR_MAP_ROBARACOCH)
	unsigned int ra = (pa >> RA_SHIFT_PA) & ((1 << RA_BITS) - 1);
	unsigned int ba = (pa >> BA_SHIFT_PA) & ((1 << BA_BITS) - 1);
	unsigned int ch = (pa >> CH_SHIFT_PA) & ((1 << CH_BITS) - 1);
	unsigned int sa_in_bank = (pa >> SA_IN_BANK_SHIFT_PA) & ((1 << SA_IN_BANK_BITS) - 1);

	/* Empilement sur 13 bits (0 à 8191) : SA(6) | CH(2) | BA(4) | RA(1) */
	return (sa_in_bank << (CH_BITS + BA_BITS + RA_BITS)) |
	       (ch << (BA_BITS + RA_BITS))                   |
	       (ba << RA_BITS)                               |
	       ra;
#elif defined(CONFIG_LAR_MAP_CHRABAROCO)
	return (int)(pa >> SA_IN_BANK_SHIFT_PA);
#endif
}

static inline unsigned int lar_pfn_to_row_idx(unsigned long pfn)
{
	unsigned long rel_pfn = pfn - lar_zone_start_pfn;
	phys_addr_t pa = (phys_addr_t)rel_pfn << PAGE_SHIFT;
	return (unsigned int)((pa >> ROW_IN_SA_SHIFT_PA) & ((1 << ROW_IN_SA_BITS) - 1));
}

static inline unsigned long lar_sub_row_to_pfn(unsigned int sa_idx, unsigned int row_idx)
{
#if defined(CONFIG_LAR_MAP_ROBARACOCH)
	/* Déballage des champs depuis sa_idx */
	unsigned int ra = sa_idx & ((1 << RA_BITS) - 1);
	unsigned int ba = (sa_idx >> RA_BITS) & ((1 << BA_BITS) - 1);
	unsigned int ch = (sa_idx >> (RA_BITS + BA_BITS)) & ((1 << CH_BITS) - 1);
	unsigned int sa_in_bank = sa_idx >> (RA_BITS + BA_BITS + CH_BITS);

	/* Reconstruction de l'adresse physique avec les décalages mis à jour */
	phys_addr_t pa = ((phys_addr_t)sa_in_bank << SA_IN_BANK_SHIFT_PA) |
	                 ((phys_addr_t)row_idx    << ROW_IN_SA_SHIFT_PA)  |
	                 ((phys_addr_t)ch         << CH_SHIFT_PA)         |
	                 ((phys_addr_t)ba         << BA_SHIFT_PA)         |
	                 ((phys_addr_t)ra         << RA_SHIFT_PA);
#elif defined(CONFIG_LAR_MAP_CHRABAROCO)
	phys_addr_t pa = ((phys_addr_t)sa_idx << SA_IN_BANK_SHIFT_PA)     |
	                 ((phys_addr_t)row_idx << ROW_IN_SA_SHIFT_PA);
#endif
	return lar_zone_start_pfn + (unsigned long)(pa >> PAGE_SHIFT);
}

/* ==================== LAR ALLOCATOR ==================== */

struct page *alloc_lar_page(gfp_t gfp_mask, int preferred_nid);
void free_lar_page(struct page *page);
void init_lar_zone(struct zone *zone, unsigned long size);

int get_subarray_idx(struct page *page);
struct page *alloc_same_subarray(struct page *old_page, unsigned long subarray_idx);

int remap_user_page(unsigned long user_vaddr, struct page *cache_page);
int remap_kernel_page(struct page *user_page, struct address_space *mapping, pgoff_t index);

/* ==================== ROWCLONE LOG ==================== */

#define RC_RING_SIZE 32768

struct rowclone_entry {
    unsigned long src;
    unsigned long dst;
};

struct rowclone_ring {
    struct rowclone_entry entries[RC_RING_SIZE];
    unsigned int head;
};

DECLARE_PER_CPU_ALIGNED(struct rowclone_ring, rowclone_rings);

static inline void log_rowclone_fast(unsigned long src, unsigned long dst)
{
    struct rowclone_ring *ring = this_cpu_ptr(&rowclone_rings);
    unsigned int idx = ring->head++ & (RC_RING_SIZE - 1);

    ring->entries[idx].src = src;
    ring->entries[idx].dst = dst;
}

enum rowclone_stat_type {
    ROWCLONE_STAT_READ,
    ROWCLONE_STAT_WRITE,
	ROWCLONE_STAT_COW,

	ROWCLONE_STAT_ORDER_GT_0,
    ROWCLONE_STAT_ORDER_GT_0_CYCLES,
	ROWCLONE_STAT_ORDER_GE_5,
    ROWCLONE_STAT_ORDER_GE_5_CYCLES,

    ROWCLONE_STAT_ALIGNED_READ,
    ROWCLONE_STAT_ALIGNED_WRITE,

    ROWCLONE_STAT_ROWCLONE_READ,
    ROWCLONE_STAT_ROWCLONE_WRITE,
	ROWCLONE_STAT_ROWCLONE_COW,

	ROWCLONE_ERR_USER_INVALID_SUBARRAY,
    ROWCLONE_ERR_USER_VMA_NOT_FOUND,
    ROWCLONE_ERR_USER_FOLLOW_PAGE,
    ROWCLONE_ERR_USER_ISOLATE_LRU,
    ROWCLONE_ERR_USER_MIGRATION,

	ROWCLONE_ERR_KERNEL_INVALID_SUBARRAY,
    ROWCLONE_ERR_KERNEL_PAGE_MAPPED,
    ROWCLONE_ERR_KERNEL_ISOLATE_LRU,
    ROWCLONE_ERR_KERNEL_MIGRATION,
    ROWCLONE_ERR_KERNEL_ALLOC,
	ROWCLONE_ERR_KERNEL_ADD_PAGE_CACHE,

    NR_ROWCLONE_STATS
};
DECLARE_PER_CPU_ALIGNED(unsigned long[NR_ROWCLONE_STATS], rowclone_stats_pcpu);

static inline void rowclone_inc_stat(enum rowclone_stat_type type)
{
    this_cpu_inc(rowclone_stats_pcpu[type]);
}
static inline void rowclone_add_stat(enum rowclone_stat_type type, unsigned long val)
{
    this_cpu_add(rowclone_stats_pcpu[type], val);
}

/* ==================== RECLAIM LAR ==================== */

static inline bool lar_kswapd_should_giveup(pg_data_t* pgdat, int highest_zoneidx, int priority)
{
	struct zone* zone;
	if (highest_zoneidx != ZONE_LAR)
		return false;
	zone = &pgdat->node_zones[ZONE_LAR];
	
	if (priority <= 3) {
		unsigned long free_pages = zone_page_state(zone, NR_FREE_PAGES);

		if (free_pages < wmark_pages(zone, WMARK_LOW)) {
			WRITE_ONCE(zone->lar_spillover, true);
			WRITE_ONCE(zone->lar_kswapd_giveup, true);
			return true;
		}
	}
	return false;
}

static inline bool lar_zone_is_balanced(pg_data_t* pgdat, int order, int highest_zoneidx)
{
	struct zone* zone;
	if (highest_zoneidx != ZONE_LAR)
		return true;
	
	zone = &pgdat->node_zones[ZONE_LAR];
	if (READ_ONCE(zone->lar_kswapd_giveup))
		return true;
	
	return zone_watermark_ok_safe(zone, order, high_wmark_pages(zone), ZONE_LAR);
}

#else

static inline struct page *alloc_lar_page(gfp_t gfp_mask, int preferred_nid) { return NULL; }
static inline void free_lar_page(struct page *page) { }
static inline void init_lar_zone(struct zone *zone, unsigned long size) { }
static inline int get_subarray_idx(struct page *page) { return -1; }
static inline struct page *alloc_same_subarray(struct page *old_page, unsigned long subarray_idx) { return NULL; }

static inline int remap_user_page(unsigned long user_vaddr, struct page *cache_page) { return 0; }
static inline int remap_kernel_page(struct page *user_page, struct address_space *mapping, pgoff_t index) { return 0; }

static inline void log_rowclone_fast(unsigned long src, unsigned long dst) { }
static inline void rowclone_inc_stat(int type) { }
static inline void rowclone_add_stat(int type, unsigned long val) { }
static inline bool lar_kswapd_should_giveup(pg_data_t* pgdat, int highest_zoneidx, int priority) { return false; }
static inline bool lar_zone_is_balanced(pg_data_t *pgdat, int order, int highest_zoneidx) { return true; }

#endif

#endif