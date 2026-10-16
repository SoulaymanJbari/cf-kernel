#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/percpu.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/migrate.h>
#include <linux/pagemap.h>
#include <linux/swap.h>
#include <linux/io.h>

#include "internal.h"
#include "lar.h"

unsigned long lar_zone_start_pfn = 0;
EXPORT_SYMBOL(lar_zone_start_pfn);

DEFINE_PER_CPU_ALIGNED(unsigned long[NR_ROWCLONE_STATS], rowclone_stats_pcpu);
EXPORT_PER_CPU_SYMBOL(rowclone_stats_pcpu);

DEFINE_PER_CPU_ALIGNED(struct rowclone_ring, rowclone_rings);
EXPORT_PER_CPU_SYMBOL(rowclone_rings);

static bool rowclone_debug_mode = false;

int get_subarray_idx(struct page *page)
{
	struct zone *zone = page_zone(page);
	if (zone_idx(zone) != ZONE_LAR)
		return -1;
	return lar_pfn_to_subarray_idx(page_to_pfn(page));
}

void free_lar_page(struct page *page)
{
	unsigned long pfn = page_to_pfn(page);
	unsigned long flags;
	struct zone *zone = page_zone(page);
	struct subarray *sa;
	int subarray_idx;
	unsigned long row_idx;

	subarray_idx = get_subarray_idx(page);
	if (subarray_idx < 0)
		return;

	sa = &zone->subarrays[subarray_idx];
	row_idx = lar_pfn_to_row_idx(pfn);

	spin_lock_irqsave(&sa->lock, flags);
	__clear_bit(row_idx, sa->bitmap);
	sa->count++;
	if (sa->count == (LAR_RESERVED_PAGES + 1))
		clear_bit(subarray_idx, zone->full_subarrays_bitmap);
	spin_unlock_irqrestore(&sa->lock, flags);

	__mod_zone_page_state(zone, NR_FREE_PAGES, 1);
}

void init_lar_zone(struct zone *zone, unsigned long size)
{
	unsigned int sa_idx;
	struct subarray *sa;

	zone->zone_end_pfn = zone->zone_start_pfn + size;
	zone->num_subarrays = size / SUBARRAY_PAGES;
	spin_lock_init(&zone->rr_lock);
	zone->rr_cursor = 0;
	zone->lar_spillover = false;
    zone->lar_kswapd_giveup = false;
	bitmap_fill(zone->full_subarrays_bitmap, zone->num_subarrays);

	for (sa_idx = 0; sa_idx < zone->num_subarrays; sa_idx++) {
		sa = &zone->subarrays[sa_idx];
		bitmap_fill(sa->bitmap, SUBARRAY_PAGES);
		spin_lock_init(&sa->lock);
		sa->count = 0;
	}
}

struct page *alloc_lar_page(gfp_t gfp_mask, int preferred_nid)
{
	struct page *page;
	struct zone *lar_zone;
	struct subarray *sa;
	unsigned long start_idx, subarray_idx, row_idx;
	unsigned long flags;
	unsigned long lar_free_pages;

	lar_zone = &NODE_DATA(preferred_nid)->node_zones[ZONE_LAR];
	lar_free_pages = zone_page_state(lar_zone, NR_FREE_PAGES);
	if (unlikely(lar_zone->lar_spillover)) {
		if (lar_free_pages >= wmark_pages(lar_zone, WMARK_HIGH)) {
			WRITE_ONCE(lar_zone->lar_spillover, false);
            WRITE_ONCE(lar_zone->lar_kswapd_giveup, false);
        } else
			return NULL;
	} else if (unlikely(lar_free_pages <= wmark_pages(lar_zone, WMARK_LOW))) {
		if (!READ_ONCE(lar_zone->lar_kswapd_giveup))
			wakeup_kswapd(lar_zone, gfp_mask, 0, ZONE_LAR);
        if (lar_free_pages <= wmark_pages(lar_zone, WMARK_MIN)) {
			lar_zone->lar_spillover = true;
			return NULL;
		}
	}
	spin_lock_irqsave(&lar_zone->rr_lock, flags);
	start_idx = (lar_zone->rr_cursor + 1) % lar_zone->num_subarrays;
	subarray_idx = find_next_zero_bit(lar_zone->full_subarrays_bitmap,
                                      lar_zone->num_subarrays,
                                      start_idx);
	if (subarray_idx >= lar_zone->num_subarrays) {
		subarray_idx = find_first_zero_bit(lar_zone->full_subarrays_bitmap, start_idx);
		if (subarray_idx >= start_idx) {
			spin_unlock_irqrestore(&lar_zone->rr_lock, flags);
            WRITE_ONCE(lar_zone->lar_spillover, true);
            return NULL;
        }
	}
	lar_zone->rr_cursor = subarray_idx;
	spin_unlock_irqrestore(&lar_zone->rr_lock, flags);
	sa = &lar_zone->subarrays[subarray_idx];
	spin_lock_irqsave(&sa->lock, flags);
	if (sa->count > LAR_RESERVED_PAGES) {
		row_idx = find_first_zero_bit(sa->bitmap, SUBARRAY_PAGES);
		if (row_idx < SUBARRAY_PAGES) {
			unsigned long pfn;
			__set_bit(row_idx, sa->bitmap);
			sa->count--;
			if (sa->count == LAR_RESERVED_PAGES) {
				set_bit(subarray_idx, lar_zone->full_subarrays_bitmap);
			}
			spin_unlock_irqrestore(&sa->lock, flags);
			pfn = lar_sub_row_to_pfn(subarray_idx, row_idx);
			page = pfn_to_page(pfn);
			__mod_zone_page_state(lar_zone, NR_FREE_PAGES, -1);
			post_alloc_hook(page, 0, gfp_mask);
			return page;

		}
	}
    spin_unlock_irqrestore(&sa->lock, flags);

    return NULL;
}

struct page *alloc_same_subarray(struct page *old_page, 
                                    unsigned long subarray_idx)
{
    struct page *page;
    struct zone *lar_zone;
    struct subarray *sa;
    unsigned long row_idx;
    unsigned long flags;
    lar_zone = &NODE_DATA(0)->node_zones[ZONE_LAR];
    sa = &lar_zone->subarrays[subarray_idx];
    spin_lock_irqsave(&sa->lock, flags);
    if (sa->count > 0) {
        row_idx = find_first_zero_bit(sa->bitmap, SUBARRAY_PAGES);
        if (row_idx < SUBARRAY_PAGES) {
			unsigned long pfn;
            __set_bit(row_idx, sa->bitmap);
            sa->count--;
            spin_unlock_irqrestore(&sa->lock, flags);
			pfn = lar_sub_row_to_pfn(subarray_idx, row_idx);
			page = pfn_to_page(pfn);
            __mod_zone_page_state(lar_zone, NR_FREE_PAGES, -1);
            ClearPageReserved(page);
            post_alloc_hook(page, 0, GFP_HIGHUSER_MOVABLE);
            return page;
        }
    }
    spin_unlock_irqrestore(&sa->lock, flags);
    return NULL;
}

int remap_user_page(unsigned long user_vaddr, struct page *cache_page)
{
    int ret = 0;
    struct list_head list;
    struct mm_struct *mm = current->mm;
    struct vm_area_struct *vma;
    struct page *page;
    phys_addr_t user_paddr;
    int subarray_idx = get_subarray_idx(cache_page);
    if (subarray_idx < 0) {
		rowclone_inc_stat(ROWCLONE_ERR_USER_INVALID_SUBARRAY);
        return -EINVAL;
    }
    mmap_read_lock(mm);
    vma = find_vma(mm, untagged_addr(user_vaddr));
    if (!vma || user_vaddr < vma->vm_start) {
        mmap_read_unlock(mm);
		rowclone_inc_stat(ROWCLONE_ERR_USER_VMA_NOT_FOUND);
        return -EINVAL;
    }
    page = follow_page(vma, user_vaddr, 0);
    if (page && !IS_ERR(page) && subarray_idx == get_subarray_idx(page)) {
        user_paddr = page_to_phys(page) + (user_vaddr & ~PAGE_MASK);
        mmap_read_unlock(mm);
        rowclone_inc_stat(ROWCLONE_STAT_ROWCLONE_READ);
        log_rowclone_fast(page_to_phys(cache_page), user_paddr);
        return 0;
    }
    mmap_read_unlock(mm);
    if (!page || IS_ERR(page)) {
        rowclone_inc_stat(ROWCLONE_ERR_USER_FOLLOW_PAGE);
        return -EINVAL;
    }
    ret = isolate_lru_page(page);
    if (ret) {
        lru_add_drain();
        ret = isolate_lru_page(page);
        if (ret) {
			rowclone_inc_stat(ROWCLONE_ERR_USER_ISOLATE_LRU);
            return ret;
        }
    }
    INIT_LIST_HEAD(&list);
    list_add_tail(&page->lru, &list);

    ret = migrate_pages(&list, alloc_same_subarray, NULL,
        (unsigned long)subarray_idx, MIGRATE_SYNC_NO_COPY, MR_SYSCALL);
    if (ret == 0) {
        mmap_read_lock(mm);
        page = follow_page(vma, user_vaddr, 0);
        if (page && !IS_ERR(page)) {
            user_paddr = page_to_phys(page) + (user_vaddr & ~PAGE_MASK);
            rowclone_inc_stat(ROWCLONE_STAT_ROWCLONE_READ);
            log_rowclone_fast(page_to_phys(cache_page), user_paddr);
        }
        mmap_read_unlock(mm);
    } else {
        putback_movable_pages(&list);
        rowclone_inc_stat(ROWCLONE_ERR_USER_MIGRATION);
    }
    return ret;
}

int remap_kernel_page(struct page *user_page, struct address_space *mapping, pgoff_t index)
{
    int subarray_idx = get_subarray_idx(user_page);
    struct page *cache_page;
    int ret = 0;
    if (subarray_idx < 0) {
		rowclone_inc_stat(ROWCLONE_ERR_KERNEL_INVALID_SUBARRAY);
        return -EINVAL;
    }
    cache_page = find_get_page(mapping, index);
    if (cache_page) {
        if (get_subarray_idx(cache_page) == subarray_idx) {
            rowclone_inc_stat(ROWCLONE_STAT_ROWCLONE_WRITE);
            log_rowclone_fast(page_to_phys(user_page), page_to_phys(cache_page));
            put_page(cache_page);
            return 0;
        }
        if (page_mapped(cache_page)) {
			rowclone_inc_stat(ROWCLONE_ERR_KERNEL_PAGE_MAPPED);
            put_page(cache_page);
            return 0; 
        }
        ret = isolate_lru_page(cache_page);
        put_page(cache_page);
        if (ret == 0) {
            LIST_HEAD(page_list);
            list_add_tail(&cache_page->lru, &page_list);
            ret = migrate_pages(&page_list, alloc_same_subarray, NULL,
                                (unsigned long)subarray_idx,
                                MIGRATE_SYNC_NO_COPY, MR_SYSCALL);
            if (ret != 0) {
                putback_movable_pages(&page_list);
                rowclone_inc_stat(ROWCLONE_ERR_KERNEL_MIGRATION);
            } else {
				struct page* new_cache_page = find_get_page(mapping, index);
				if (new_cache_page) {
					rowclone_inc_stat(ROWCLONE_STAT_ROWCLONE_WRITE);
                	log_rowclone_fast(page_to_phys(user_page), page_to_phys(new_cache_page));
					put_page(new_cache_page);
				} 
            }
        } else {
            rowclone_inc_stat(ROWCLONE_ERR_KERNEL_ISOLATE_LRU);
        }
    } else {
        struct page *new_page = alloc_same_subarray(NULL, subarray_idx);
        if (!new_page) {
            rowclone_inc_stat(ROWCLONE_ERR_KERNEL_ALLOC);
            return -ENOMEM;
        }
        ret = add_to_page_cache_lru(new_page, mapping, index, GFP_KERNEL);
        if (ret) {
            free_unref_page(new_page);
			rowclone_inc_stat(ROWCLONE_ERR_KERNEL_ADD_PAGE_CACHE);
            return ret;
        }
        rowclone_inc_stat(ROWCLONE_STAT_ROWCLONE_WRITE);
		log_rowclone_fast(page_to_phys(user_page), page_to_phys(new_page));
        unlock_page(new_page);
        put_page(new_page);
    }
    return ret;
}

static int rowclone_proc_show(struct seq_file *m, void *v)
{
    unsigned long total[NR_ROWCLONE_STATS] = {0};
    int cpu, i;

    for_each_possible_cpu(cpu) {
        for (i = 0; i < NR_ROWCLONE_STATS; i++) {
            total[i] += per_cpu(rowclone_stats_pcpu, cpu)[i];
        }
    }

    if (!rowclone_debug_mode) {
        /* === AFFICHAGE NORMAL : ESSENTIEL UNIQUEMENT === */
        seq_printf(m, "=== ROWCLONE SUMMARY ===\n");
        seq_printf(m, "Read:     %lu (RowClone: %lu)\n", total[ROWCLONE_STAT_ALIGNED_READ], total[ROWCLONE_STAT_ROWCLONE_READ]);
        seq_printf(m, "Write:    %lu (RowClone: %lu)\n", total[ROWCLONE_STAT_ALIGNED_WRITE], total[ROWCLONE_STAT_ROWCLONE_WRITE]);
        seq_printf(m, "CoW:      %lu (RowClone: %lu)\n", total[ROWCLONE_STAT_COW], total[ROWCLONE_STAT_ROWCLONE_COW]);
        seq_printf(m, "Order > 0:  %lu (%lu cycles)\n", total[ROWCLONE_STAT_ORDER_GT_0], total[ROWCLONE_STAT_ORDER_GT_0_CYCLES]);
        seq_printf(m, "Order >= 5: %lu (%lu cycles)\n", total[ROWCLONE_STAT_ORDER_GE_5], total[ROWCLONE_STAT_ORDER_GE_5_CYCLES]);
    } else {
        /* === AFFICHAGE DEBUG UNIQUEMENT === */
        seq_printf(m, "=== DEBUG: BASELINE METRICS ===\n");
        seq_printf(m, "Read count:  %lu\n", total[ROWCLONE_STAT_READ]);
        seq_printf(m, "Write count: %lu\n", total[ROWCLONE_STAT_WRITE]);
        seq_printf(m, "CoW count:   %lu\n", total[ROWCLONE_STAT_COW]);

        seq_printf(m, "\n=== DEBUG: ALIGNMENT METRICS ===\n");
        seq_printf(m, "Read count:  %lu\n", total[ROWCLONE_STAT_ALIGNED_READ]);
        seq_printf(m, "Write count: %lu\n", total[ROWCLONE_STAT_ALIGNED_WRITE]);

        seq_printf(m, "\n=== DEBUG: ROWCLONE METRICS ===\n");
        seq_printf(m, "Read count:  %lu\n", total[ROWCLONE_STAT_ROWCLONE_READ]);
        seq_printf(m, "Write count: %lu\n", total[ROWCLONE_STAT_ROWCLONE_WRITE]);
        seq_printf(m, "CoW count:   %lu\n", total[ROWCLONE_STAT_ROWCLONE_COW]);

        seq_printf(m, "\n=== DEBUG: REMAP USER FAILURES ===\n");
        seq_printf(m, "Invalid subarray: %lu\n", total[ROWCLONE_ERR_USER_INVALID_SUBARRAY]);
        seq_printf(m, "VMA not found:    %lu\n", total[ROWCLONE_ERR_USER_VMA_NOT_FOUND]);
        seq_printf(m, "Follow page err:  %lu\n", total[ROWCLONE_ERR_USER_FOLLOW_PAGE]);
        seq_printf(m, "Isolate LRU err:  %lu\n", total[ROWCLONE_ERR_USER_ISOLATE_LRU]);
        seq_printf(m, "Migration err:    %lu\n", total[ROWCLONE_ERR_USER_MIGRATION]);

        seq_printf(m, "\n=== DEBUG: REMAP KERNEL FAILURES ===\n");
        seq_printf(m, "Invalid subarray: %lu\n", total[ROWCLONE_ERR_KERNEL_INVALID_SUBARRAY]);
        seq_printf(m, "Page mapped err:  %lu\n", total[ROWCLONE_ERR_KERNEL_PAGE_MAPPED]);
        seq_printf(m, "Isolate LRU err:  %lu\n", total[ROWCLONE_ERR_KERNEL_ISOLATE_LRU]);
        seq_printf(m, "Migration err:    %lu\n", total[ROWCLONE_ERR_KERNEL_MIGRATION]);
        seq_printf(m, "Alloc sa err:     %lu\n", total[ROWCLONE_ERR_KERNEL_ALLOC]);
        seq_printf(m, "Add pagecache err:%lu\n", total[ROWCLONE_ERR_KERNEL_ADD_PAGE_CACHE]);

        seq_printf(m, "\n=== DEBUG: ALLOCATION TIMINGS ===\n");
        seq_printf(m, "Order > 0 count:     %lu\n", total[ROWCLONE_STAT_ORDER_GT_0]);
        seq_printf(m, "Order > 0 cycles:    %lu\n", total[ROWCLONE_STAT_ORDER_GT_0_CYCLES]);
        seq_printf(m, "Order >= 5 count:    %lu\n", total[ROWCLONE_STAT_ORDER_GE_5]);
        seq_printf(m, "Order >= 5 cycles:   %lu\n", total[ROWCLONE_STAT_ORDER_GE_5_CYCLES]);
    }
    
    return 0;
}

static int rowclone_proc_open(struct inode *inode, struct file *file)
{
    return single_open(file, rowclone_proc_show, NULL);
}

static ssize_t rowclone_proc_write(struct file *file, const char __user *buffer,
                                   size_t count, loff_t *ppos)
{
    char kbuf[32];
    size_t len = min(count, sizeof(kbuf) - 1);
    int cpu;

    if (copy_from_user(kbuf, buffer, len))
        return -EFAULT;
    kbuf[len] = '\0';

    if (sysfs_streq(kbuf, "debug") || sysfs_streq(kbuf, "1")) {
        rowclone_debug_mode = true;
    } else if (sysfs_streq(kbuf, "normal") || sysfs_streq(kbuf, "0")) {
        rowclone_debug_mode = false;
    } else {
        for_each_possible_cpu(cpu) {
            memset(per_cpu(rowclone_stats_pcpu, cpu), 0, sizeof(unsigned long) * NR_ROWCLONE_STATS);
        }
    }

    return count;
}

static const struct proc_ops rowclone_proc_ops = {
    .proc_open    = rowclone_proc_open,
    .proc_read    = seq_read,
    .proc_write   = rowclone_proc_write,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
};

static int rowclone_ring_proc_show(struct seq_file *m, void *v)
{
    int cpu;
    unsigned int i, count;

    seq_printf(m, "# CPU | SRC_PHYS_ADDR | DST_PHYS_ADDR\n");

    for_each_possible_cpu(cpu) {
        struct rowclone_ring *ring = &per_cpu(rowclone_rings, cpu);
        count = min_t(unsigned int, ring->head, RC_RING_SIZE);
        
        for (i = 0; i < count; i++) {
            seq_printf(m, "%d 0x%lx 0x%lx\n",
                       cpu,
                       ring->entries[i].src,
                       ring->entries[i].dst);
        }
    }
    return 0;
}

static int rowclone_ring_proc_open(struct inode *inode, struct file *file)
{
    return single_open(file, rowclone_ring_proc_show, NULL);
}

static ssize_t rowclone_ring_proc_write(struct file *file, const char __user *buffer,
                                        size_t count, loff_t *ppos)
{
    int cpu;

    for_each_possible_cpu(cpu) {
        struct rowclone_ring *ring = &per_cpu(rowclone_rings, cpu);
        ring->head = 0;
        memset(ring->entries, 0, sizeof(ring->entries));
    }
    return count;
}

static const struct proc_ops rowclone_ring_proc_ops = {
    .proc_open    = rowclone_ring_proc_open,
    .proc_read    = seq_read,
    .proc_write   = rowclone_ring_proc_write,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
};

static int __init rowclone_init_procfs(void)
{
    proc_create("rowclone_stats", 0666, NULL, &rowclone_proc_ops);
    proc_create("rowclone_ring", 0666, NULL, &rowclone_ring_proc_ops);
    return 0;
}
subsys_initcall(rowclone_init_procfs);