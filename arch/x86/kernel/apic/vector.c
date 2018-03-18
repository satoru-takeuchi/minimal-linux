/*
 * Local APIC related interfaces to support IOAPIC, MSI, etc.
 *
 * Copyright (C) 1997, 1998, 1999, 2000, 2009 Ingo Molnar, Hajnalka Szabo
 *	Moved from arch/x86/kernel/apic/io_apic.c.
 * Jiang Liu <jiang.liu@linux.intel.com>
 *	Enable support of hierarchical irqdomains
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/interrupt.h>
#include <linux/seq_file.h>
#include <linux/init.h>
#include <linux/compiler.h>
#include <linux/slab.h>
#include <asm/irqdomain.h>
#include <asm/hw_irq.h>
#include <asm/apic.h>
#include <asm/i8259.h>
#include <asm/desc.h>
#include <asm/irq_remapping.h>

#include <asm/trace/irq_vectors.h>

struct apic_chip_data {
	struct irq_cfg		hw_irq_cfg;
	unsigned int		vector;
	unsigned int		prev_vector;
	unsigned int		cpu;
	unsigned int		prev_cpu;
	unsigned int		irq;
	struct hlist_node	clist;
	unsigned int		move_in_progress	: 1,
				is_managed		: 1,
				can_reserve		: 1,
				has_reserved		: 1;
};

struct irq_domain *x86_vector_domain;
EXPORT_SYMBOL_GPL(x86_vector_domain);
static DEFINE_RAW_SPINLOCK(vector_lock);
static cpumask_var_t vector_searchmask;
static struct irq_chip lapic_controller;
static struct irq_matrix *vector_matrix;

void lock_vector_lock(void)
{
	/* Used to the online set of cpus does not change
	 * during assign_irq_vector.
	 */
	raw_spin_lock(&vector_lock);
}

void unlock_vector_lock(void)
{
	raw_spin_unlock(&vector_lock);
}

void init_irq_alloc_info(struct irq_alloc_info *info,
			 const struct cpumask *mask)
{
	memset(info, 0, sizeof(*info));
	info->mask = mask;
}

void copy_irq_alloc_info(struct irq_alloc_info *dst, struct irq_alloc_info *src)
{
	if (src)
		*dst = *src;
	else
		memset(dst, 0, sizeof(*dst));
}

static struct apic_chip_data *apic_chip_data(struct irq_data *irqd)
{
	if (!irqd)
		return NULL;

	while (irqd->parent_data)
		irqd = irqd->parent_data;

	return irqd->chip_data;
}

struct irq_cfg *irqd_cfg(struct irq_data *irqd)
{
	struct apic_chip_data *apicd = apic_chip_data(irqd);

	return apicd ? &apicd->hw_irq_cfg : NULL;
}
EXPORT_SYMBOL_GPL(irqd_cfg);

struct irq_cfg *irq_cfg(unsigned int irq)
{
	return irqd_cfg(irq_get_irq_data(irq));
}

static struct apic_chip_data *alloc_apic_chip_data(int node)
{
	struct apic_chip_data *apicd;

	apicd = kzalloc_node(sizeof(*apicd), GFP_KERNEL, node);
	if (apicd)
		INIT_HLIST_NODE(&apicd->clist);
	return apicd;
}

static void free_apic_chip_data(struct apic_chip_data *apicd)
{
	kfree(apicd);
}

static void apic_update_irq_cfg(struct irq_data *irqd, unsigned int vector,
				unsigned int cpu)
{
	struct apic_chip_data *apicd = apic_chip_data(irqd);

	lockdep_assert_held(&vector_lock);

	apicd->hw_irq_cfg.vector = vector;
	apicd->hw_irq_cfg.dest_apicid = apic->calc_dest_apicid(cpu);
	irq_data_update_effective_affinity(irqd, cpumask_of(cpu));
	trace_vector_config(irqd->irq, vector, cpu,
			    apicd->hw_irq_cfg.dest_apicid);
}

static void apic_update_vector(struct irq_data *irqd, unsigned int newvec,
			       unsigned int newcpu)
{
	struct apic_chip_data *apicd = apic_chip_data(irqd);
	struct irq_desc *desc = irq_data_to_desc(irqd);

	lockdep_assert_held(&vector_lock);

	trace_vector_update(irqd->irq, newvec, newcpu, apicd->vector,
			    apicd->cpu);

	/* Setup the vector move, if required  */
	if (apicd->vector && cpu_online(apicd->cpu)) {
		apicd->move_in_progress = true;
		apicd->prev_vector = apicd->vector;
		apicd->prev_cpu = apicd->cpu;
	} else {
		apicd->prev_vector = 0;
	}

	apicd->vector = newvec;
	apicd->cpu = newcpu;
	BUG_ON(!IS_ERR_OR_NULL(per_cpu(vector_irq, newcpu)[newvec]));
	per_cpu(vector_irq, newcpu)[newvec] = desc;
}

static void vector_assign_managed_shutdown(struct irq_data *irqd)
{
	unsigned int cpu = cpumask_first(cpu_online_mask);

	apic_update_irq_cfg(irqd, MANAGED_IRQ_SHUTDOWN_VECTOR, cpu);
}

static int reserve_managed_vector(struct irq_data *irqd)
{
	const struct cpumask *affmsk = irq_data_get_affinity_mask(irqd);
	struct apic_chip_data *apicd = apic_chip_data(irqd);
	unsigned long flags;
	int ret;

	raw_spin_lock_irqsave(&vector_lock, flags);
	apicd->is_managed = true;
	ret = irq_matrix_reserve_managed(vector_matrix, affmsk);
	raw_spin_unlock_irqrestore(&vector_lock, flags);
	trace_vector_reserve_managed(irqd->irq, ret);
	return ret;
}

static void reserve_irq_vector_locked(struct irq_data *irqd)
{
	struct apic_chip_data *apicd = apic_chip_data(irqd);

	irq_matrix_reserve(vector_matrix);
	apicd->can_reserve = true;
	apicd->has_reserved = true;
	irqd_set_can_reserve(irqd);
	trace_vector_reserve(irqd->irq, 0);
	vector_assign_managed_shutdown(irqd);
}

static int reserve_irq_vector(struct irq_data *irqd)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&vector_lock, flags);
	reserve_irq_vector_locked(irqd);
	raw_spin_unlock_irqrestore(&vector_lock, flags);
	return 0;
}

static int allocate_vector(struct irq_data *irqd, const struct cpumask *dest)
{
	struct apic_chip_data *apicd = apic_chip_data(irqd);
	bool resvd = apicd->has_reserved;
	unsigned int cpu = apicd->cpu;
	int vector = apicd->vector;

	lockdep_assert_held(&vector_lock);

	/*
	 * If the current target CPU is online and in the new requested
	 * affinity mask, there is no point in moving the interrupt from
	 * one CPU to another.
	 */
	if (vector && cpu_online(cpu) && cpumask_test_cpu(cpu, dest))
		return 0;

	vector = irq_matrix_alloc(vector_matrix, dest, resvd, &cpu);
	if (vector > 0)
		apic_update_vector(irqd, vector, cpu);
	trace_vector_alloc(irqd->irq, vector, resvd, vector);
	return vector;
}

static int assign_vector_locked(struct irq_data *irqd,
				const struct cpumask *dest)
{
	struct apic_chip_data *apicd = apic_chip_data(irqd);
	int vector = allocate_vector(irqd, dest);

	if (vector < 0)
		return vector;

	apic_update_irq_cfg(irqd, apicd->vector, apicd->cpu);
	return 0;
}

static int assign_irq_vector(struct irq_data *irqd, const struct cpumask *dest)
{
	unsigned long flags;
	int ret;

	raw_spin_lock_irqsave(&vector_lock, flags);
	cpumask_and(vector_searchmask, dest, cpu_online_mask);
	ret = assign_vector_locked(irqd, vector_searchmask);
	raw_spin_unlock_irqrestore(&vector_lock, flags);
	return ret;
}

static int assign_irq_vector_any_locked(struct irq_data *irqd)
{
	/* Get the affinity mask - either irq_default_affinity or (user) set */
	const struct cpumask *affmsk = irq_data_get_affinity_mask(irqd);
	int node = irq_data_get_node(irqd);

	if (node == NUMA_NO_NODE)
		goto all;
	/* Try the intersection of @affmsk and node mask */
	cpumask_and(vector_searchmask, cpumask_of_node(node), affmsk);
	if (!assign_vector_locked(irqd, vector_searchmask))
		return 0;
	/* Try the node mask */
	if (!assign_vector_locked(irqd, cpumask_of_node(node)))
		return 0;
all:
	/* Try the full affinity mask */
	cpumask_and(vector_searchmask, affmsk, cpu_online_mask);
	if (!assign_vector_locked(irqd, vector_searchmask))
		return 0;
	/* Try the full online mask */
	return assign_vector_locked(irqd, cpu_online_mask);
}

static int
assign_irq_vector_policy(struct irq_data *irqd, struct irq_alloc_info *info)
{
	if (irqd_affinity_is_managed(irqd))
		return reserve_managed_vector(irqd);
	if (info->mask)
		return assign_irq_vector(irqd, info->mask);
	/*
	 * Make only a global reservation with no guarantee. A real vector
	 * is associated at activation time.
	 */
	return reserve_irq_vector(irqd);
}

static int
assign_managed_vector(struct irq_data *irqd, const struct cpumask *dest)
{
	const struct cpumask *affmsk = irq_data_get_affinity_mask(irqd);
	struct apic_chip_data *apicd = apic_chip_data(irqd);
	int vector, cpu;

	cpumask_and(vector_searchmask, vector_searchmask, affmsk);
	cpu = cpumask_first(vector_searchmask);
	if (cpu >= nr_cpu_ids)
		return -EINVAL;
	/* set_affinity might call here for nothing */
	if (apicd->vector && cpumask_test_cpu(apicd->cpu, vector_searchmask))
		return 0;
	vector = irq_matrix_alloc_managed(vector_matrix, cpu);
	trace_vector_alloc_managed(irqd->irq, vector, vector);
	if (vector < 0)
		return vector;
	apic_update_vector(irqd, vector, cpu);
	apic_update_irq_cfg(irqd, vector, cpu);
	return 0;
}

static void clear_irq_vector(struct irq_data *irqd)
{
	struct apic_chip_data *apicd = apic_chip_data(irqd);
	bool managed = irqd_affinity_is_managed(irqd);
	unsigned int vector = apicd->vector;

	lockdep_assert_held(&vector_lock);

	if (!vector)
		return;

	trace_vector_clear(irqd->irq, vector, apicd->cpu, apicd->prev_vector,
			   apicd->prev_cpu);

	per_cpu(vector_irq, apicd->cpu)[vector] = VECTOR_UNUSED;
	irq_matrix_free(vector_matrix, apicd->cpu, vector, managed);
	apicd->vector = 0;

	/* Clean up move in progress */
	vector = apicd->prev_vector;
	if (!vector)
		return;

	per_cpu(vector_irq, apicd->prev_cpu)[vector] = VECTOR_UNUSED;
	irq_matrix_free(vector_matrix, apicd->prev_cpu, vector, managed);
	apicd->prev_vector = 0;
	apicd->move_in_progress = 0;
	hlist_del_init(&apicd->clist);
}

static void x86_vector_deactivate(struct irq_domain *dom, struct irq_data *irqd)
{
	struct apic_chip_data *apicd = apic_chip_data(irqd);
	unsigned long flags;

	trace_vector_deactivate(irqd->irq, apicd->is_managed,
				apicd->can_reserve, false);

	/* Regular fixed assigned interrupt */
	if (!apicd->is_managed && !apicd->can_reserve)
		return;
	/* If the interrupt has a global reservation, nothing to do */
	if (apicd->has_reserved)
		return;

	raw_spin_lock_irqsave(&vector_lock, flags);
	clear_irq_vector(irqd);
	if (apicd->can_reserve)
		reserve_irq_vector_locked(irqd);
	else
		vector_assign_managed_shutdown(irqd);
	raw_spin_unlock_irqrestore(&vector_lock, flags);
}

static int activate_reserved(struct irq_data *irqd)
{
	struct apic_chip_data *apicd = apic_chip_data(irqd);
	int ret;

	ret = assign_irq_vector_any_locked(irqd);
	if (!ret) {
		apicd->has_reserved = false;
		/*
		 * Core might have disabled reservation mode after
		 * allocating the irq descriptor. Ideally this should
		 * happen before allocation time, but that would require
		 * completely convoluted ways of transporting that
		 * information.
		 */
		if (!irqd_can_reserve(irqd))
			apicd->can_reserve = false;
	}
	return ret;
}

static int activate_managed(struct irq_data *irqd)
{
	const struct cpumask *dest = irq_data_get_affinity_mask(irqd);
	int ret;

	cpumask_and(vector_searchmask, dest, cpu_online_mask);
	if (WARN_ON_ONCE(cpumask_empty(vector_searchmask))) {
		/* Something in the core code broke! Survive gracefully */
		pr_err("Managed startup for irq %u, but no CPU\n", irqd->irq);
		return EINVAL;
	}

	ret = assign_managed_vector(irqd, vector_searchmask);
	/*
	 * This should not happen. The vector reservation got buggered.  Handle
	 * it gracefully.
	 */
	if (WARN_ON_ONCE(ret < 0)) {
		pr_err("Managed startup irq %u, no vector available\n",
		       irqd->irq);
	}
       return ret;
}

static int x86_vector_activate(struct irq_domain *dom, struct irq_data *irqd,
			       bool reserve)
{
	struct apic_chip_data *apicd = apic_chip_data(irqd);
	unsigned long flags;
	int ret = 0;

	trace_vector_activate(irqd->irq, apicd->is_managed,
			      apicd->can_reserve, reserve);

	/* Nothing to do for fixed assigned vectors */
	if (!apicd->can_reserve && !apicd->is_managed)
		return 0;

	raw_spin_lock_irqsave(&vector_lock, flags);
	if (reserve || irqd_is_managed_and_shutdown(irqd))
		vector_assign_managed_shutdown(irqd);
	else if (apicd->is_managed)
		ret = activate_managed(irqd);
	else if (apicd->has_reserved)
		ret = activate_reserved(irqd);
	raw_spin_unlock_irqrestore(&vector_lock, flags);
	return ret;
}

static void vector_free_reserved_and_managed(struct irq_data *irqd)
{
	const struct cpumask *dest = irq_data_get_affinity_mask(irqd);
	struct apic_chip_data *apicd = apic_chip_data(irqd);

	trace_vector_teardown(irqd->irq, apicd->is_managed,
			      apicd->has_reserved);

	if (apicd->has_reserved)
		irq_matrix_remove_reserved(vector_matrix);
	if (apicd->is_managed)
		irq_matrix_remove_managed(vector_matrix, dest);
}

static void x86_vector_free_irqs(struct irq_domain *domain,
				 unsigned int virq, unsigned int nr_irqs)
{
	struct apic_chip_data *apicd;
	struct irq_data *irqd;
	unsigned long flags;
	int i;

	for (i = 0; i < nr_irqs; i++) {
		irqd = irq_domain_get_irq_data(x86_vector_domain, virq + i);
		if (irqd && irqd->chip_data) {
			raw_spin_lock_irqsave(&vector_lock, flags);
			clear_irq_vector(irqd);
			vector_free_reserved_and_managed(irqd);
			apicd = irqd->chip_data;
			irq_domain_reset_irq_data(irqd);
			raw_spin_unlock_irqrestore(&vector_lock, flags);
			free_apic_chip_data(apicd);
		}
	}
}

static bool vector_configure_legacy(unsigned int virq, struct irq_data *irqd,
				    struct apic_chip_data *apicd)
{
	unsigned long flags;
	bool realloc = false;

	apicd->vector = ISA_IRQ_VECTOR(virq);
	apicd->cpu = 0;

	raw_spin_lock_irqsave(&vector_lock, flags);
	/*
	 * If the interrupt is activated, then it must stay at this vector
	 * position. That's usually the timer interrupt (0).
	 */
	if (irqd_is_activated(irqd)) {
		trace_vector_setup(virq, true, 0);
		apic_update_irq_cfg(irqd, apicd->vector, apicd->cpu);
	} else {
		/* Release the vector */
		apicd->can_reserve = true;
		irqd_set_can_reserve(irqd);
		clear_irq_vector(irqd);
		realloc = true;
	}
	raw_spin_unlock_irqrestore(&vector_lock, flags);
	return realloc;
}

static int x86_vector_alloc_irqs(struct irq_domain *domain, unsigned int virq,
				 unsigned int nr_irqs, void *arg)
{
	struct irq_alloc_info *info = arg;
	struct apic_chip_data *apicd;
	struct irq_data *irqd;
	int i, err, node;

	if (disable_apic)
		return -ENXIO;

	/* Currently vector allocator can't guarantee contiguous allocations */
	if ((info->flags & X86_IRQ_ALLOC_CONTIGUOUS_VECTORS) && nr_irqs > 1)
		return -ENOSYS;

	for (i = 0; i < nr_irqs; i++) {
		irqd = irq_domain_get_irq_data(domain, virq + i);
		BUG_ON(!irqd);
		node = irq_data_get_node(irqd);
		WARN_ON_ONCE(irqd->chip_data);
		apicd = alloc_apic_chip_data(node);
		if (!apicd) {
			err = -ENOMEM;
			goto error;
		}

		apicd->irq = virq + i;
		irqd->chip = &lapic_controller;
		irqd->chip_data = apicd;
		irqd->hwirq = virq + i;
		irqd_set_single_target(irqd);
		/*
		 * Legacy vectors are already assigned when the IOAPIC
		 * takes them over. They stay on the same vector. This is
		 * required for check_timer() to work correctly as it might
		 * switch back to legacy mode. Only update the hardware
		 * config.
		 */
		if (info->flags & X86_IRQ_ALLOC_LEGACY) {
			if (!vector_configure_legacy(virq + i, irqd, apicd))
				continue;
		}

		err = assign_irq_vector_policy(irqd, info);
		trace_vector_setup(virq + i, false, err);
		if (err) {
			irqd->chip_data = NULL;
			free_apic_chip_data(apicd);
			goto error;
		}
	}

	return 0;

error:
	x86_vector_free_irqs(domain, virq, i);
	return err;
}

#ifdef CONFIG_GENERIC_IRQ_DEBUGFS
static void x86_vector_debug_show(struct seq_file *m, struct irq_domain *d,
				  struct irq_data *irqd, int ind)
{
	unsigned int cpu, vector, prev_cpu, prev_vector;
	struct apic_chip_data *apicd;
	unsigned long flags;
	int irq;

	if (!irqd) {
		irq_matrix_debug_show(m, vector_matrix, ind);
		return;
	}

	irq = irqd->irq;
	if (irq < nr_legacy_irqs() && !test_bit(irq, &io_apic_irqs)) {
		seq_printf(m, "%*sVector: %5d\n", ind, "", ISA_IRQ_VECTOR(irq));
		seq_printf(m, "%*sTarget: Legacy PIC all CPUs\n", ind, "");
		return;
	}

	apicd = irqd->chip_data;
	if (!apicd) {
		seq_printf(m, "%*sVector: Not assigned\n", ind, "");
		return;
	}

	raw_spin_lock_irqsave(&vector_lock, flags);
	cpu = apicd->cpu;
	vector = apicd->vector;
	prev_cpu = apicd->prev_cpu;
	prev_vector = apicd->prev_vector;
	raw_spin_unlock_irqrestore(&vector_lock, flags);
	seq_printf(m, "%*sVector: %5u\n", ind, "", vector);
	seq_printf(m, "%*sTarget: %5u\n", ind, "", cpu);
	if (prev_vector) {
		seq_printf(m, "%*sPrevious vector: %5u\n", ind, "", prev_vector);
		seq_printf(m, "%*sPrevious target: %5u\n", ind, "", prev_cpu);
	}
}
#endif

static const struct irq_domain_ops x86_vector_domain_ops = {
	.alloc		= x86_vector_alloc_irqs,
	.free		= x86_vector_free_irqs,
	.activate	= x86_vector_activate,
	.deactivate	= x86_vector_deactivate,
#ifdef CONFIG_GENERIC_IRQ_DEBUGFS
	.debug_show	= x86_vector_debug_show,
#endif
};

int __init arch_probe_nr_irqs(void)
{
	int nr;

	if (nr_irqs > (NR_VECTORS * nr_cpu_ids))
		nr_irqs = NR_VECTORS * nr_cpu_ids;

	nr = (gsi_top + nr_legacy_irqs()) + 8 * nr_cpu_ids;
#if defined(CONFIG_PCI_MSI)
	/*
	 * for MSI and HT dyn irq
	 */
	if (gsi_top <= NR_IRQS_LEGACY)
		nr +=  8 * nr_cpu_ids;
	else
		nr += gsi_top * 16;
#endif
	if (nr < nr_irqs)
		nr_irqs = nr;

	/*
	 * We don't know if PIC is present at this point so we need to do
	 * probe() to get the right number of legacy IRQs.
	 */
	return legacy_pic->probe();
}

void lapic_assign_legacy_vector(unsigned int irq, bool replace)
{
	/*
	 * Use assign system here so it wont get accounted as allocated
	 * and moveable in the cpu hotplug check and it prevents managed
	 * irq reservation from touching it.
	 */
	irq_matrix_assign_system(vector_matrix, ISA_IRQ_VECTOR(irq), replace);
}

void __init lapic_assign_system_vectors(void)
{
	unsigned int i, vector = 0;

	for_each_set_bit_from(vector, system_vectors, NR_VECTORS)
		irq_matrix_assign_system(vector_matrix, vector, false);

	if (nr_legacy_irqs() > 1)
		lapic_assign_legacy_vector(PIC_CASCADE_IR, false);

	/* System vectors are reserved, online it */
	irq_matrix_online(vector_matrix);

	/* Mark the preallocated legacy interrupts */
	for (i = 0; i < nr_legacy_irqs(); i++) {
		if (i != PIC_CASCADE_IR)
			irq_matrix_assign(vector_matrix, ISA_IRQ_VECTOR(i));
	}
}

int __init arch_early_irq_init(void)
{
	struct fwnode_handle *fn;

	fn = irq_domain_alloc_named_fwnode("VECTOR");
	BUG_ON(!fn);
	x86_vector_domain = irq_domain_create_tree(fn, &x86_vector_domain_ops,
						   NULL);
	BUG_ON(x86_vector_domain == NULL);
	irq_domain_free_fwnode(fn);
	irq_set_default_host(x86_vector_domain);

	arch_init_msi_domain(x86_vector_domain);

	BUG_ON(!alloc_cpumask_var(&vector_searchmask, GFP_KERNEL));

	/*
	 * Allocate the vector matrix allocator data structure and limit the
	 * search area.
	 */
	vector_matrix = irq_alloc_matrix(NR_VECTORS, FIRST_EXTERNAL_VECTOR,
					 FIRST_SYSTEM_VECTOR);
	BUG_ON(!vector_matrix);

	return arch_early_ioapic_init();
}

# define apic_set_affinity	NULL

static int apic_retrigger_irq(struct irq_data *irqd)
{
	struct apic_chip_data *apicd = apic_chip_data(irqd);
	unsigned long flags;

	raw_spin_lock_irqsave(&vector_lock, flags);
	apic->send_IPI(apicd->cpu, apicd->vector);
	raw_spin_unlock_irqrestore(&vector_lock, flags);

	return 1;
}

void apic_ack_edge(struct irq_data *irqd)
{
	irq_complete_move(irqd_cfg(irqd));
	irq_move_irq(irqd);
	ack_APIC_irq();
}

static struct irq_chip lapic_controller = {
	.name			= "APIC",
	.irq_ack		= apic_ack_edge,
	.irq_set_affinity	= apic_set_affinity,
	.irq_retrigger		= apic_retrigger_irq,
};

static void __init print_APIC_field(int base)
{
	int i;

	printk(KERN_DEBUG);

	for (i = 0; i < 8; i++)
		pr_cont("%08x", apic_read(base + i*0x10));

	pr_cont("\n");
}

static void __init print_local_APIC(void *dummy)
{
	unsigned int i, v, ver, maxlvt;
	u64 icr;

	pr_debug("printing local APIC contents on CPU#%d/%d:\n",
		 smp_processor_id(), hard_smp_processor_id());
	v = apic_read(APIC_ID);
	pr_info("... APIC ID:      %08x (%01x)\n", v, read_apic_id());
	v = apic_read(APIC_LVR);
	pr_info("... APIC VERSION: %08x\n", v);
	ver = GET_APIC_VERSION(v);
	maxlvt = lapic_get_maxlvt();

	v = apic_read(APIC_TASKPRI);
	pr_debug("... APIC TASKPRI: %08x (%02x)\n", v, v & APIC_TPRI_MASK);

	/* !82489DX */
	if (APIC_INTEGRATED(ver)) {
		if (!APIC_XAPIC(ver)) {
			v = apic_read(APIC_ARBPRI);
			pr_debug("... APIC ARBPRI: %08x (%02x)\n",
				 v, v & APIC_ARBPRI_MASK);
		}
		v = apic_read(APIC_PROCPRI);
		pr_debug("... APIC PROCPRI: %08x\n", v);
	}

	/*
	 * Remote read supported only in the 82489DX and local APIC for
	 * Pentium processors.
	 */
	if (!APIC_INTEGRATED(ver) || maxlvt == 3) {
		v = apic_read(APIC_RRR);
		pr_debug("... APIC RRR: %08x\n", v);
	}

	v = apic_read(APIC_LDR);
	pr_debug("... APIC LDR: %08x\n", v);
	if (!x2apic_enabled()) {
		v = apic_read(APIC_DFR);
		pr_debug("... APIC DFR: %08x\n", v);
	}
	v = apic_read(APIC_SPIV);
	pr_debug("... APIC SPIV: %08x\n", v);

	pr_debug("... APIC ISR field:\n");
	print_APIC_field(APIC_ISR);
	pr_debug("... APIC TMR field:\n");
	print_APIC_field(APIC_TMR);
	pr_debug("... APIC IRR field:\n");
	print_APIC_field(APIC_IRR);

	/* !82489DX */
	if (APIC_INTEGRATED(ver)) {
		/* Due to the Pentium erratum 3AP. */
		if (maxlvt > 3)
			apic_write(APIC_ESR, 0);

		v = apic_read(APIC_ESR);
		pr_debug("... APIC ESR: %08x\n", v);
	}

	icr = apic_icr_read();
	pr_debug("... APIC ICR: %08x\n", (u32)icr);
	pr_debug("... APIC ICR2: %08x\n", (u32)(icr >> 32));

	v = apic_read(APIC_LVTT);
	pr_debug("... APIC LVTT: %08x\n", v);

	if (maxlvt > 3) {
		/* PC is LVT#4. */
		v = apic_read(APIC_LVTPC);
		pr_debug("... APIC LVTPC: %08x\n", v);
	}
	v = apic_read(APIC_LVT0);
	pr_debug("... APIC LVT0: %08x\n", v);
	v = apic_read(APIC_LVT1);
	pr_debug("... APIC LVT1: %08x\n", v);

	if (maxlvt > 2) {
		/* ERR is LVT#3. */
		v = apic_read(APIC_LVTERR);
		pr_debug("... APIC LVTERR: %08x\n", v);
	}

	v = apic_read(APIC_TMICT);
	pr_debug("... APIC TMICT: %08x\n", v);
	v = apic_read(APIC_TMCCT);
	pr_debug("... APIC TMCCT: %08x\n", v);
	v = apic_read(APIC_TDCR);
	pr_debug("... APIC TDCR: %08x\n", v);

	if (boot_cpu_has(X86_FEATURE_EXTAPIC)) {
		v = apic_read(APIC_EFEAT);
		maxlvt = (v >> 16) & 0xff;
		pr_debug("... APIC EFEAT: %08x\n", v);
		v = apic_read(APIC_ECTRL);
		pr_debug("... APIC ECTRL: %08x\n", v);
		for (i = 0; i < maxlvt; i++) {
			v = apic_read(APIC_EILVTn(i));
			pr_debug("... APIC EILVT%d: %08x\n", i, v);
		}
	}
	pr_cont("\n");
}

static void __init print_local_APICs(int maxcpu)
{
	int cpu;

	if (!maxcpu)
		return;

	preempt_disable();
	for_each_online_cpu(cpu) {
		if (cpu >= maxcpu)
			break;
		smp_call_function_single(cpu, print_local_APIC, NULL, 1);
	}
	preempt_enable();
}

static void __init print_PIC(void)
{
	unsigned int v;
	unsigned long flags;

	if (!nr_legacy_irqs())
		return;

	pr_debug("\nprinting PIC contents\n");

	raw_spin_lock_irqsave(&i8259A_lock, flags);

	v = inb(0xa1) << 8 | inb(0x21);
	pr_debug("... PIC  IMR: %04x\n", v);

	v = inb(0xa0) << 8 | inb(0x20);
	pr_debug("... PIC  IRR: %04x\n", v);

	outb(0x0b, 0xa0);
	outb(0x0b, 0x20);
	v = inb(0xa0) << 8 | inb(0x20);
	outb(0x0a, 0xa0);
	outb(0x0a, 0x20);

	raw_spin_unlock_irqrestore(&i8259A_lock, flags);

	pr_debug("... PIC  ISR: %04x\n", v);

	v = inb(0x4d1) << 8 | inb(0x4d0);
	pr_debug("... PIC ELCR: %04x\n", v);
}

static int show_lapic __initdata = 1;
static __init int setup_show_lapic(char *arg)
{
	int num = -1;

	if (strcmp(arg, "all") == 0) {
		show_lapic = CONFIG_NR_CPUS;
	} else {
		get_option(&arg, &num);
		if (num >= 0)
			show_lapic = num;
	}

	return 1;
}
__setup("show_lapic=", setup_show_lapic);

static int __init print_ICs(void)
{
	if (apic_verbosity == APIC_QUIET)
		return 0;

	print_PIC();

	/* don't print out if apic is not there */
	if (!boot_cpu_has(X86_FEATURE_APIC) && !apic_from_smp_config())
		return 0;

	print_local_APICs(show_lapic);
	print_IO_APICs();

	return 0;
}

late_initcall(print_ICs);
