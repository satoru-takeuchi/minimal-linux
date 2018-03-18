/*
 *  linux/mm/vmstat.c
 *
 *  Manages VM statistics
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *
 *  zoned VM statistics
 *  Copyright (C) 2006 Silicon Graphics, Inc.,
 *		Christoph Lameter <christoph@lameter.com>
 *  Copyright (C) 2008-2014 Christoph Lameter
 */
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/vmstat.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/debugfs.h>
#include <linux/sched.h>
#include <linux/math64.h>
#include <linux/writeback.h>
#include <linux/compaction.h>
#include <linux/mm_inline.h>
#include <linux/page_ext.h>
#include <linux/page_owner.h>

#include "internal.h"

#define NUMA_STATS_THRESHOLD (U16_MAX - 2)

#ifdef CONFIG_NUMA
int sysctl_vm_numa_stat = ENABLE_NUMA_STAT;

/* zero numa counters within a zone */
static void zero_zone_numa_counters(struct zone *zone)
{
	int item, cpu;

	for (item = 0; item < NR_VM_NUMA_STAT_ITEMS; item++) {
		atomic_long_set(&zone->vm_numa_stat[item], 0);
		for_each_online_cpu(cpu)
			per_cpu_ptr(zone->pageset, cpu)->vm_numa_stat_diff[item]
						= 0;
	}
}

/* zero numa counters of all the populated zones */
static void zero_zones_numa_counters(void)
{
	struct zone *zone;

	for_each_populated_zone(zone)
		zero_zone_numa_counters(zone);
}

/* zero global numa counters */
static void zero_global_numa_counters(void)
{
	int item;

	for (item = 0; item < NR_VM_NUMA_STAT_ITEMS; item++)
		atomic_long_set(&vm_numa_stat[item], 0);
}

static void invalid_numa_statistics(void)
{
	zero_zones_numa_counters();
	zero_global_numa_counters();
}

static DEFINE_MUTEX(vm_numa_stat_lock);

int sysctl_vm_numa_stat_handler(struct ctl_table *table, int write,
		void __user *buffer, size_t *length, loff_t *ppos)
{
	int ret, oldval;

	mutex_lock(&vm_numa_stat_lock);
	if (write)
		oldval = sysctl_vm_numa_stat;
	ret = proc_dointvec_minmax(table, write, buffer, length, ppos);
	if (ret || !write)
		goto out;

	if (oldval == sysctl_vm_numa_stat)
		goto out;
	else if (sysctl_vm_numa_stat == ENABLE_NUMA_STAT) {
		static_branch_enable(&vm_numa_stat_key);
		pr_info("enable numa statistics\n");
	} else {
		static_branch_disable(&vm_numa_stat_key);
		invalid_numa_statistics();
		pr_info("disable numa statistics, and clear numa counters\n");
	}

out:
	mutex_unlock(&vm_numa_stat_lock);
	return ret;
}
#endif

#ifdef CONFIG_VM_EVENT_COUNTERS
DEFINE_PER_CPU(struct vm_event_state, vm_event_states) = {{0}};
EXPORT_PER_CPU_SYMBOL(vm_event_states);

static void sum_vm_events(unsigned long *ret)
{
	int cpu;
	int i;

	memset(ret, 0, NR_VM_EVENT_ITEMS * sizeof(unsigned long));

	for_each_online_cpu(cpu) {
		struct vm_event_state *this = &per_cpu(vm_event_states, cpu);

		for (i = 0; i < NR_VM_EVENT_ITEMS; i++)
			ret[i] += this->event[i];
	}
}

/*
 * Accumulate the vm event counters across all CPUs.
 * The result is unavoidably approximate - it can change
 * during and after execution of this function.
*/
void all_vm_events(unsigned long *ret)
{
	get_online_cpus();
	sum_vm_events(ret);
	put_online_cpus();
}
EXPORT_SYMBOL_GPL(all_vm_events);

/*
 * Fold the foreign cpu events into our own.
 *
 * This is adding to the events on one processor
 * but keeps the global counts constant.
 */
void vm_events_fold_cpu(int cpu)
{
	struct vm_event_state *fold_state = &per_cpu(vm_event_states, cpu);
	int i;

	for (i = 0; i < NR_VM_EVENT_ITEMS; i++) {
		count_vm_events(i, fold_state->event[i]);
		fold_state->event[i] = 0;
	}
}

#endif /* CONFIG_VM_EVENT_COUNTERS */

/*
 * Manage combined zone based / global counters
 *
 * vm_stat contains the global counters
 */
atomic_long_t vm_zone_stat[NR_VM_ZONE_STAT_ITEMS] __cacheline_aligned_in_smp;
atomic_long_t vm_numa_stat[NR_VM_NUMA_STAT_ITEMS] __cacheline_aligned_in_smp;
atomic_long_t vm_node_stat[NR_VM_NODE_STAT_ITEMS] __cacheline_aligned_in_smp;
EXPORT_SYMBOL(vm_zone_stat);
EXPORT_SYMBOL(vm_numa_stat);
EXPORT_SYMBOL(vm_node_stat);

#ifdef CONFIG_NUMA
void __inc_numa_state(struct zone *zone,
				 enum numa_stat_item item)
{
	struct per_cpu_pageset __percpu *pcp = zone->pageset;
	u16 __percpu *p = pcp->vm_numa_stat_diff + item;
	u16 v;

	v = __this_cpu_inc_return(*p);

	if (unlikely(v > NUMA_STATS_THRESHOLD)) {
		zone_numa_state_add(v, zone, item);
		__this_cpu_write(*p, 0);
	}
}

/*
 * Determine the per node value of a stat item. This function
 * is called frequently in a NUMA machine, so try to be as
 * frugal as possible.
 */
unsigned long sum_zone_node_page_state(int node,
				 enum zone_stat_item item)
{
	struct zone *zones = NODE_DATA(node)->node_zones;
	int i;
	unsigned long count = 0;

	for (i = 0; i < MAX_NR_ZONES; i++)
		count += zone_page_state(zones + i, item);

	return count;
}

/*
 * Determine the per node value of a numa stat item. To avoid deviation,
 * the per cpu stat number in vm_numa_stat_diff[] is also included.
 */
unsigned long sum_zone_numa_state(int node,
				 enum numa_stat_item item)
{
	struct zone *zones = NODE_DATA(node)->node_zones;
	int i;
	unsigned long count = 0;

	for (i = 0; i < MAX_NR_ZONES; i++)
		count += zone_numa_state_snapshot(zones + i, item);

	return count;
}

/*
 * Determine the per node value of a stat item.
 */
unsigned long node_page_state(struct pglist_data *pgdat,
				enum node_stat_item item)
{
	long x = atomic_long_read(&pgdat->vm_stat[item]);
	return x;
}
#endif

#ifdef CONFIG_COMPACTION

struct contig_page_info {
	unsigned long free_pages;
	unsigned long free_blocks_total;
	unsigned long free_blocks_suitable;
};

/*
 * Calculate the number of free pages in a zone, how many contiguous
 * pages are free and how many are large enough to satisfy an allocation of
 * the target size. Note that this function makes no attempt to estimate
 * how many suitable free blocks there *might* be if MOVABLE pages were
 * migrated. Calculating that is possible, but expensive and can be
 * figured out from userspace
 */
static void fill_contig_page_info(struct zone *zone,
				unsigned int suitable_order,
				struct contig_page_info *info)
{
	unsigned int order;

	info->free_pages = 0;
	info->free_blocks_total = 0;
	info->free_blocks_suitable = 0;

	for (order = 0; order < MAX_ORDER; order++) {
		unsigned long blocks;

		/* Count number of free blocks */
		blocks = zone->free_area[order].nr_free;
		info->free_blocks_total += blocks;

		/* Count free base pages */
		info->free_pages += blocks << order;

		/* Count the suitable free blocks */
		if (order >= suitable_order)
			info->free_blocks_suitable += blocks <<
						(order - suitable_order);
	}
}

/*
 * A fragmentation index only makes sense if an allocation of a requested
 * size would fail. If that is true, the fragmentation index indicates
 * whether external fragmentation or a lack of memory was the problem.
 * The value can be used to determine if page reclaim or compaction
 * should be used
 */
static int __fragmentation_index(unsigned int order, struct contig_page_info *info)
{
	unsigned long requested = 1UL << order;

	if (WARN_ON_ONCE(order >= MAX_ORDER))
		return 0;

	if (!info->free_blocks_total)
		return 0;

	/* Fragmentation index only makes sense when a request would fail */
	if (info->free_blocks_suitable)
		return -1000;

	/*
	 * Index is between 0 and 1 so return within 3 decimal places
	 *
	 * 0 => allocation would fail due to lack of memory
	 * 1 => allocation would fail due to fragmentation
	 */
	return 1000 - div_u64( (1000+(div_u64(info->free_pages * 1000ULL, requested))), info->free_blocks_total);
}

/* Same as __fragmentation index but allocs contig_page_info on stack */
int fragmentation_index(struct zone *zone, unsigned int order)
{
	struct contig_page_info info;

	fill_contig_page_info(zone, order, &info);
	return __fragmentation_index(order, &info);
}
#endif

#if defined(CONFIG_PROC_FS) || defined(CONFIG_SYSFS) || defined(CONFIG_NUMA)
#ifdef CONFIG_ZONE_DMA
#define TEXT_FOR_DMA(xx) xx "_dma",
#else
#define TEXT_FOR_DMA(xx)
#endif

#ifdef CONFIG_ZONE_DMA32
#define TEXT_FOR_DMA32(xx) xx "_dma32",
#else
#define TEXT_FOR_DMA32(xx)
#endif

#ifdef CONFIG_HIGHMEM
#define TEXT_FOR_HIGHMEM(xx) xx "_high",
#else
#define TEXT_FOR_HIGHMEM(xx)
#endif

#define TEXTS_FOR_ZONES(xx) TEXT_FOR_DMA(xx) TEXT_FOR_DMA32(xx) xx "_normal", \
					TEXT_FOR_HIGHMEM(xx) xx "_movable",

const char * const vmstat_text[] = {
	/* enum zone_stat_item countes */
	"nr_free_pages",
	"nr_zone_inactive_anon",
	"nr_zone_active_anon",
	"nr_zone_inactive_file",
	"nr_zone_active_file",
	"nr_zone_unevictable",
	"nr_zone_write_pending",
	"nr_mlock",
	"nr_page_table_pages",
	"nr_kernel_stack",
	"nr_bounce",
#if IS_ENABLED(CONFIG_ZSMALLOC)
	"nr_zspages",
#endif
	"nr_free_cma",

	/* enum numa_stat_item counters */
#ifdef CONFIG_NUMA
	"numa_hit",
	"numa_miss",
	"numa_foreign",
	"numa_interleave",
	"numa_local",
	"numa_other",
#endif

	/* Node-based counters */
	"nr_inactive_anon",
	"nr_active_anon",
	"nr_inactive_file",
	"nr_active_file",
	"nr_unevictable",
	"nr_slab_reclaimable",
	"nr_slab_unreclaimable",
	"nr_isolated_anon",
	"nr_isolated_file",
	"workingset_refault",
	"workingset_activate",
	"workingset_nodereclaim",
	"nr_anon_pages",
	"nr_mapped",
	"nr_file_pages",
	"nr_dirty",
	"nr_writeback",
	"nr_writeback_temp",
	"nr_shmem",
	"nr_shmem_hugepages",
	"nr_shmem_pmdmapped",
	"nr_anon_transparent_hugepages",
	"nr_unstable",
	"nr_vmscan_write",
	"nr_vmscan_immediate_reclaim",
	"nr_dirtied",
	"nr_written",

	/* enum writeback_stat_item counters */
	"nr_dirty_threshold",
	"nr_dirty_background_threshold",

#ifdef CONFIG_VM_EVENT_COUNTERS
	/* enum vm_event_item counters */
	"pgpgin",
	"pgpgout",
	"pswpin",
	"pswpout",

	TEXTS_FOR_ZONES("pgalloc")
	TEXTS_FOR_ZONES("allocstall")
	TEXTS_FOR_ZONES("pgskip")

	"pgfree",
	"pgactivate",
	"pgdeactivate",
	"pglazyfree",

	"pgfault",
	"pgmajfault",
	"pglazyfreed",

	"pgrefill",
	"pgsteal_kswapd",
	"pgsteal_direct",
	"pgscan_kswapd",
	"pgscan_direct",
	"pgscan_direct_throttle",

#ifdef CONFIG_NUMA
	"zone_reclaim_failed",
#endif
	"pginodesteal",
	"slabs_scanned",
	"kswapd_inodesteal",
	"kswapd_low_wmark_hit_quickly",
	"kswapd_high_wmark_hit_quickly",
	"pageoutrun",

	"pgrotated",

	"drop_pagecache",
	"drop_slab",
	"oom_kill",

#ifdef CONFIG_NUMA_BALANCING
	"numa_pte_updates",
	"numa_huge_pte_updates",
	"numa_hint_faults",
	"numa_hint_faults_local",
	"numa_pages_migrated",
#endif
#ifdef CONFIG_MIGRATION
	"pgmigrate_success",
	"pgmigrate_fail",
#endif
#ifdef CONFIG_COMPACTION
	"compact_migrate_scanned",
	"compact_free_scanned",
	"compact_isolated",
	"compact_stall",
	"compact_fail",
	"compact_success",
	"compact_daemon_wake",
	"compact_daemon_migrate_scanned",
	"compact_daemon_free_scanned",
#endif

#ifdef CONFIG_HUGETLB_PAGE
	"htlb_buddy_alloc_success",
	"htlb_buddy_alloc_fail",
#endif
	"unevictable_pgs_culled",
	"unevictable_pgs_scanned",
	"unevictable_pgs_rescued",
	"unevictable_pgs_mlocked",
	"unevictable_pgs_munlocked",
	"unevictable_pgs_cleared",
	"unevictable_pgs_stranded",

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	"thp_fault_alloc",
	"thp_fault_fallback",
	"thp_collapse_alloc",
	"thp_collapse_alloc_failed",
	"thp_file_alloc",
	"thp_file_mapped",
	"thp_split_page",
	"thp_split_page_failed",
	"thp_deferred_split_page",
	"thp_split_pmd",
#ifdef CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD
	"thp_split_pud",
#endif
	"thp_zero_page_alloc",
	"thp_zero_page_alloc_failed",
	"thp_swpout",
	"thp_swpout_fallback",
#endif
#ifdef CONFIG_MEMORY_BALLOON
	"balloon_inflate",
	"balloon_deflate",
#ifdef CONFIG_BALLOON_COMPACTION
	"balloon_migrate",
#endif
#endif /* CONFIG_MEMORY_BALLOON */
#ifdef CONFIG_DEBUG_TLBFLUSH
	"nr_tlb_local_flush_all",
	"nr_tlb_local_flush_one",
#endif /* CONFIG_DEBUG_TLBFLUSH */

#ifdef CONFIG_DEBUG_VM_VMACACHE
	"vmacache_find_calls",
	"vmacache_find_hits",
	"vmacache_full_flushes",
#endif
#ifdef CONFIG_SWAP
	"swap_ra",
	"swap_ra_hit",
#endif
#endif /* CONFIG_VM_EVENTS_COUNTERS */
};
#endif /* CONFIG_PROC_FS || CONFIG_SYSFS || CONFIG_NUMA */

#if (defined(CONFIG_DEBUG_FS) && defined(CONFIG_COMPACTION)) || \
     defined(CONFIG_PROC_FS)
static void *frag_start(struct seq_file *m, loff_t *pos)
{
	pg_data_t *pgdat;
	loff_t node = *pos;

	for (pgdat = first_online_pgdat();
	     pgdat && node;
	     pgdat = next_online_pgdat(pgdat))
		--node;

	return pgdat;
}

static void *frag_next(struct seq_file *m, void *arg, loff_t *pos)
{
	pg_data_t *pgdat = (pg_data_t *)arg;

	(*pos)++;
	return next_online_pgdat(pgdat);
}

static void frag_stop(struct seq_file *m, void *arg)
{
}

/*
 * Walk zones in a node and print using a callback.
 * If @assert_populated is true, only use callback for zones that are populated.
 */
static void walk_zones_in_node(struct seq_file *m, pg_data_t *pgdat,
		bool assert_populated, bool nolock,
		void (*print)(struct seq_file *m, pg_data_t *, struct zone *))
{
	struct zone *zone;
	struct zone *node_zones = pgdat->node_zones;
	unsigned long flags;

	for (zone = node_zones; zone - node_zones < MAX_NR_ZONES; ++zone) {
		if (assert_populated && !populated_zone(zone))
			continue;

		if (!nolock)
			spin_lock_irqsave(&zone->lock, flags);
		print(m, pgdat, zone);
		if (!nolock)
			spin_unlock_irqrestore(&zone->lock, flags);
	}
}
#endif

#ifdef CONFIG_PROC_FS
static void frag_show_print(struct seq_file *m, pg_data_t *pgdat,
						struct zone *zone)
{
	int order;

	seq_printf(m, "Node %d, zone %8s ", pgdat->node_id, zone->name);
	for (order = 0; order < MAX_ORDER; ++order)
		seq_printf(m, "%6lu ", zone->free_area[order].nr_free);
	seq_putc(m, '\n');
}

/*
 * This walks the free areas for each zone.
 */
static int frag_show(struct seq_file *m, void *arg)
{
	pg_data_t *pgdat = (pg_data_t *)arg;
	walk_zones_in_node(m, pgdat, true, false, frag_show_print);
	return 0;
}

static void pagetypeinfo_showfree_print(struct seq_file *m,
					pg_data_t *pgdat, struct zone *zone)
{
	int order, mtype;

	for (mtype = 0; mtype < MIGRATE_TYPES; mtype++) {
		seq_printf(m, "Node %4d, zone %8s, type %12s ",
					pgdat->node_id,
					zone->name,
					migratetype_names[mtype]);
		for (order = 0; order < MAX_ORDER; ++order) {
			unsigned long freecount = 0;
			struct free_area *area;
			struct list_head *curr;

			area = &(zone->free_area[order]);

			list_for_each(curr, &area->free_list[mtype])
				freecount++;
			seq_printf(m, "%6lu ", freecount);
		}
		seq_putc(m, '\n');
	}
}

/* Print out the free pages at each order for each migatetype */
static int pagetypeinfo_showfree(struct seq_file *m, void *arg)
{
	int order;
	pg_data_t *pgdat = (pg_data_t *)arg;

	/* Print header */
	seq_printf(m, "%-43s ", "Free pages count per migrate type at order");
	for (order = 0; order < MAX_ORDER; ++order)
		seq_printf(m, "%6d ", order);
	seq_putc(m, '\n');

	walk_zones_in_node(m, pgdat, true, false, pagetypeinfo_showfree_print);

	return 0;
}

static void pagetypeinfo_showblockcount_print(struct seq_file *m,
					pg_data_t *pgdat, struct zone *zone)
{
	int mtype;
	unsigned long pfn;
	unsigned long start_pfn = zone->zone_start_pfn;
	unsigned long end_pfn = zone_end_pfn(zone);
	unsigned long count[MIGRATE_TYPES] = { 0, };

	for (pfn = start_pfn; pfn < end_pfn; pfn += pageblock_nr_pages) {
		struct page *page;

		page = pfn_to_online_page(pfn);
		if (!page)
			continue;

		/* Watch for unexpected holes punched in the memmap */
		if (!memmap_valid_within(pfn, page, zone))
			continue;

		if (page_zone(page) != zone)
			continue;

		mtype = get_pageblock_migratetype(page);

		if (mtype < MIGRATE_TYPES)
			count[mtype]++;
	}

	/* Print counts */
	seq_printf(m, "Node %d, zone %8s ", pgdat->node_id, zone->name);
	for (mtype = 0; mtype < MIGRATE_TYPES; mtype++)
		seq_printf(m, "%12lu ", count[mtype]);
	seq_putc(m, '\n');
}

/* Print out the number of pageblocks for each migratetype */
static int pagetypeinfo_showblockcount(struct seq_file *m, void *arg)
{
	int mtype;
	pg_data_t *pgdat = (pg_data_t *)arg;

	seq_printf(m, "\n%-23s", "Number of blocks type ");
	for (mtype = 0; mtype < MIGRATE_TYPES; mtype++)
		seq_printf(m, "%12s ", migratetype_names[mtype]);
	seq_putc(m, '\n');
	walk_zones_in_node(m, pgdat, true, false,
		pagetypeinfo_showblockcount_print);

	return 0;
}

/*
 * Print out the number of pageblocks for each migratetype that contain pages
 * of other types. This gives an indication of how well fallbacks are being
 * contained by rmqueue_fallback(). It requires information from PAGE_OWNER
 * to determine what is going on
 */
static void pagetypeinfo_showmixedcount(struct seq_file *m, pg_data_t *pgdat)
{
#ifdef CONFIG_PAGE_OWNER
	int mtype;

	if (!static_branch_unlikely(&page_owner_inited))
		return;

	drain_all_pages(NULL);

	seq_printf(m, "\n%-23s", "Number of mixed blocks ");
	for (mtype = 0; mtype < MIGRATE_TYPES; mtype++)
		seq_printf(m, "%12s ", migratetype_names[mtype]);
	seq_putc(m, '\n');

	walk_zones_in_node(m, pgdat, true, true,
		pagetypeinfo_showmixedcount_print);
#endif /* CONFIG_PAGE_OWNER */
}

/*
 * This prints out statistics in relation to grouping pages by mobility.
 * It is expensive to collect so do not constantly read the file.
 */
static int pagetypeinfo_show(struct seq_file *m, void *arg)
{
	pg_data_t *pgdat = (pg_data_t *)arg;

	/* check memoryless node */
	if (!node_state(pgdat->node_id, N_MEMORY))
		return 0;

	seq_printf(m, "Page block order: %d\n", pageblock_order);
	seq_printf(m, "Pages per block:  %lu\n", pageblock_nr_pages);
	seq_putc(m, '\n');
	pagetypeinfo_showfree(m, pgdat);
	pagetypeinfo_showblockcount(m, pgdat);
	pagetypeinfo_showmixedcount(m, pgdat);

	return 0;
}

static const struct seq_operations fragmentation_op = {
	.start	= frag_start,
	.next	= frag_next,
	.stop	= frag_stop,
	.show	= frag_show,
};

static int fragmentation_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &fragmentation_op);
}

static const struct file_operations buddyinfo_file_operations = {
	.open		= fragmentation_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release,
};

static const struct seq_operations pagetypeinfo_op = {
	.start	= frag_start,
	.next	= frag_next,
	.stop	= frag_stop,
	.show	= pagetypeinfo_show,
};

static int pagetypeinfo_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &pagetypeinfo_op);
}

static const struct file_operations pagetypeinfo_file_operations = {
	.open		= pagetypeinfo_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release,
};

static bool is_zone_first_populated(pg_data_t *pgdat, struct zone *zone)
{
	int zid;

	for (zid = 0; zid < MAX_NR_ZONES; zid++) {
		struct zone *compare = &pgdat->node_zones[zid];

		if (populated_zone(compare))
			return zone == compare;
	}

	return false;
}

static void zoneinfo_show_print(struct seq_file *m, pg_data_t *pgdat,
							struct zone *zone)
{
	int i;
	seq_printf(m, "Node %d, zone %8s", pgdat->node_id, zone->name);
	if (is_zone_first_populated(pgdat, zone)) {
		seq_printf(m, "\n  per-node stats");
		for (i = 0; i < NR_VM_NODE_STAT_ITEMS; i++) {
			seq_printf(m, "\n      %-12s %lu",
				vmstat_text[i + NR_VM_ZONE_STAT_ITEMS +
				NR_VM_NUMA_STAT_ITEMS],
				node_page_state(pgdat, i));
		}
	}
	seq_printf(m,
		   "\n  pages free     %lu"
		   "\n        min      %lu"
		   "\n        low      %lu"
		   "\n        high     %lu"
		   "\n        spanned  %lu"
		   "\n        present  %lu"
		   "\n        managed  %lu",
		   zone_page_state(zone, NR_FREE_PAGES),
		   min_wmark_pages(zone),
		   low_wmark_pages(zone),
		   high_wmark_pages(zone),
		   zone->spanned_pages,
		   zone->present_pages,
		   zone->managed_pages);

	seq_printf(m,
		   "\n        protection: (%ld",
		   zone->lowmem_reserve[0]);
	for (i = 1; i < ARRAY_SIZE(zone->lowmem_reserve); i++)
		seq_printf(m, ", %ld", zone->lowmem_reserve[i]);
	seq_putc(m, ')');

	/* If unpopulated, no other information is useful */
	if (!populated_zone(zone)) {
		seq_putc(m, '\n');
		return;
	}

	for (i = 0; i < NR_VM_ZONE_STAT_ITEMS; i++)
		seq_printf(m, "\n      %-12s %lu", vmstat_text[i],
				zone_page_state(zone, i));

#ifdef CONFIG_NUMA
	for (i = 0; i < NR_VM_NUMA_STAT_ITEMS; i++)
		seq_printf(m, "\n      %-12s %lu",
				vmstat_text[i + NR_VM_ZONE_STAT_ITEMS],
				zone_numa_state_snapshot(zone, i));
#endif

	seq_printf(m, "\n  pagesets");
	for_each_online_cpu(i) {
		struct per_cpu_pageset *pageset;

		pageset = per_cpu_ptr(zone->pageset, i);
		seq_printf(m,
			   "\n    cpu: %i"
			   "\n              count: %i"
			   "\n              high:  %i"
			   "\n              batch: %i",
			   i,
			   pageset->pcp.count,
			   pageset->pcp.high,
			   pageset->pcp.batch);
	}
	seq_printf(m,
		   "\n  node_unreclaimable:  %u"
		   "\n  start_pfn:           %lu",
		   pgdat->kswapd_failures >= MAX_RECLAIM_RETRIES,
		   zone->zone_start_pfn);
	seq_putc(m, '\n');
}

/*
 * Output information about zones in @pgdat.  All zones are printed regardless
 * of whether they are populated or not: lowmem_reserve_ratio operates on the
 * set of all zones and userspace would not be aware of such zones if they are
 * suppressed here (zoneinfo displays the effect of lowmem_reserve_ratio).
 */
static int zoneinfo_show(struct seq_file *m, void *arg)
{
	pg_data_t *pgdat = (pg_data_t *)arg;
	walk_zones_in_node(m, pgdat, false, false, zoneinfo_show_print);
	return 0;
}

static const struct seq_operations zoneinfo_op = {
	.start	= frag_start, /* iterate over all zones. The same as in
			       * fragmentation. */
	.next	= frag_next,
	.stop	= frag_stop,
	.show	= zoneinfo_show,
};

static int zoneinfo_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &zoneinfo_op);
}

static const struct file_operations zoneinfo_file_operations = {
	.open		= zoneinfo_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release,
};

enum writeback_stat_item {
	NR_DIRTY_THRESHOLD,
	NR_DIRTY_BG_THRESHOLD,
	NR_VM_WRITEBACK_STAT_ITEMS,
};

static void *vmstat_start(struct seq_file *m, loff_t *pos)
{
	unsigned long *v;
	int i, stat_items_size;

	if (*pos >= ARRAY_SIZE(vmstat_text))
		return NULL;
	stat_items_size = NR_VM_ZONE_STAT_ITEMS * sizeof(unsigned long) +
			  NR_VM_NUMA_STAT_ITEMS * sizeof(unsigned long) +
			  NR_VM_NODE_STAT_ITEMS * sizeof(unsigned long) +
			  NR_VM_WRITEBACK_STAT_ITEMS * sizeof(unsigned long);

#ifdef CONFIG_VM_EVENT_COUNTERS
	stat_items_size += sizeof(struct vm_event_state);
#endif

	v = kmalloc(stat_items_size, GFP_KERNEL);
	m->private = v;
	if (!v)
		return ERR_PTR(-ENOMEM);
	for (i = 0; i < NR_VM_ZONE_STAT_ITEMS; i++)
		v[i] = global_zone_page_state(i);
	v += NR_VM_ZONE_STAT_ITEMS;

#ifdef CONFIG_NUMA
	for (i = 0; i < NR_VM_NUMA_STAT_ITEMS; i++)
		v[i] = global_numa_state(i);
	v += NR_VM_NUMA_STAT_ITEMS;
#endif

	for (i = 0; i < NR_VM_NODE_STAT_ITEMS; i++)
		v[i] = global_node_page_state(i);
	v += NR_VM_NODE_STAT_ITEMS;

	global_dirty_limits(v + NR_DIRTY_BG_THRESHOLD,
			    v + NR_DIRTY_THRESHOLD);
	v += NR_VM_WRITEBACK_STAT_ITEMS;

#ifdef CONFIG_VM_EVENT_COUNTERS
	all_vm_events(v);
	v[PGPGIN] /= 2;		/* sectors -> kbytes */
	v[PGPGOUT] /= 2;
#endif
	return (unsigned long *)m->private + *pos;
}

static void *vmstat_next(struct seq_file *m, void *arg, loff_t *pos)
{
	(*pos)++;
	if (*pos >= ARRAY_SIZE(vmstat_text))
		return NULL;
	return (unsigned long *)m->private + *pos;
}

static int vmstat_show(struct seq_file *m, void *arg)
{
	unsigned long *l = arg;
	unsigned long off = l - (unsigned long *)m->private;

	seq_puts(m, vmstat_text[off]);
	seq_put_decimal_ull(m, " ", *l);
	seq_putc(m, '\n');
	return 0;
}

static void vmstat_stop(struct seq_file *m, void *arg)
{
	kfree(m->private);
	m->private = NULL;
}

static const struct seq_operations vmstat_op = {
	.start	= vmstat_start,
	.next	= vmstat_next,
	.stop	= vmstat_stop,
	.show	= vmstat_show,
};

static int vmstat_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &vmstat_op);
}

static const struct file_operations vmstat_file_operations = {
	.open		= vmstat_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release,
};
#endif /* CONFIG_PROC_FS */

struct workqueue_struct *mm_percpu_wq;

void __init init_mm_internals(void)
{
	int ret __maybe_unused;

	mm_percpu_wq = alloc_workqueue("mm_percpu_wq", WQ_MEM_RECLAIM, 0);

#ifdef CONFIG_PROC_FS
	proc_create("buddyinfo", 0444, NULL, &buddyinfo_file_operations);
	proc_create("pagetypeinfo", 0444, NULL, &pagetypeinfo_file_operations);
	proc_create("vmstat", 0444, NULL, &vmstat_file_operations);
	proc_create("zoneinfo", 0444, NULL, &zoneinfo_file_operations);
#endif
}

#if defined(CONFIG_DEBUG_FS) && defined(CONFIG_COMPACTION)

/*
 * Return an index indicating how much of the available free memory is
 * unusable for an allocation of the requested size.
 */
static int unusable_free_index(unsigned int order,
				struct contig_page_info *info)
{
	/* No free memory is interpreted as all free memory is unusable */
	if (info->free_pages == 0)
		return 1000;

	/*
	 * Index should be a value between 0 and 1. Return a value to 3
	 * decimal places.
	 *
	 * 0 => no fragmentation
	 * 1 => high fragmentation
	 */
	return div_u64((info->free_pages - (info->free_blocks_suitable << order)) * 1000ULL, info->free_pages);

}

static void unusable_show_print(struct seq_file *m,
					pg_data_t *pgdat, struct zone *zone)
{
	unsigned int order;
	int index;
	struct contig_page_info info;

	seq_printf(m, "Node %d, zone %8s ",
				pgdat->node_id,
				zone->name);
	for (order = 0; order < MAX_ORDER; ++order) {
		fill_contig_page_info(zone, order, &info);
		index = unusable_free_index(order, &info);
		seq_printf(m, "%d.%03d ", index / 1000, index % 1000);
	}

	seq_putc(m, '\n');
}

/*
 * Display unusable free space index
 *
 * The unusable free space index measures how much of the available free
 * memory cannot be used to satisfy an allocation of a given size and is a
 * value between 0 and 1. The higher the value, the more of free memory is
 * unusable and by implication, the worse the external fragmentation is. This
 * can be expressed as a percentage by multiplying by 100.
 */
static int unusable_show(struct seq_file *m, void *arg)
{
	pg_data_t *pgdat = (pg_data_t *)arg;

	/* check memoryless node */
	if (!node_state(pgdat->node_id, N_MEMORY))
		return 0;

	walk_zones_in_node(m, pgdat, true, false, unusable_show_print);

	return 0;
}

static const struct seq_operations unusable_op = {
	.start	= frag_start,
	.next	= frag_next,
	.stop	= frag_stop,
	.show	= unusable_show,
};

static int unusable_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &unusable_op);
}

static const struct file_operations unusable_file_ops = {
	.open		= unusable_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release,
};

static void extfrag_show_print(struct seq_file *m,
					pg_data_t *pgdat, struct zone *zone)
{
	unsigned int order;
	int index;

	/* Alloc on stack as interrupts are disabled for zone walk */
	struct contig_page_info info;

	seq_printf(m, "Node %d, zone %8s ",
				pgdat->node_id,
				zone->name);
	for (order = 0; order < MAX_ORDER; ++order) {
		fill_contig_page_info(zone, order, &info);
		index = __fragmentation_index(order, &info);
		seq_printf(m, "%d.%03d ", index / 1000, index % 1000);
	}

	seq_putc(m, '\n');
}

/*
 * Display fragmentation index for orders that allocations would fail for
 */
static int extfrag_show(struct seq_file *m, void *arg)
{
	pg_data_t *pgdat = (pg_data_t *)arg;

	walk_zones_in_node(m, pgdat, true, false, extfrag_show_print);

	return 0;
}

static const struct seq_operations extfrag_op = {
	.start	= frag_start,
	.next	= frag_next,
	.stop	= frag_stop,
	.show	= extfrag_show,
};

static int extfrag_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &extfrag_op);
}

static const struct file_operations extfrag_file_ops = {
	.open		= extfrag_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release,
};

static int __init extfrag_debug_init(void)
{
	struct dentry *extfrag_debug_root;

	extfrag_debug_root = debugfs_create_dir("extfrag", NULL);
	if (!extfrag_debug_root)
		return -ENOMEM;

	if (!debugfs_create_file("unusable_index", 0444,
			extfrag_debug_root, NULL, &unusable_file_ops))
		goto fail;

	if (!debugfs_create_file("extfrag_index", 0444,
			extfrag_debug_root, NULL, &extfrag_file_ops))
		goto fail;

	return 0;
fail:
	debugfs_remove_recursive(extfrag_debug_root);
	return -ENOMEM;
}

module_init(extfrag_debug_init);
#endif
