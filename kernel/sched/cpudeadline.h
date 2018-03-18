/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CPUDL_H
#define _LINUX_CPUDL_H

#include <linux/sched.h>
#include <linux/sched/deadline.h>

#define IDX_INVALID     -1

struct cpudl_item {
	u64 dl;
	int cpu;
	int idx;
};

struct cpudl {
	raw_spinlock_t lock;
	int size;
	cpumask_var_t free_cpus;
	struct cpudl_item *elements;
};

#endif /* _LINUX_CPUDL_H */
