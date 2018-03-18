// SPDX-License-Identifier: GPL-2.0
/*
 * Check for extended topology enumeration cpuid leaf 0xb and if it
 * exists, use it for populating initial_apicid and cpu topology
 * detection.
 */

#include <linux/cpu.h>
#include <asm/apic.h>
#include <asm/pat.h>
#include <asm/processor.h>

/* leaf 0xb SMT level */
#define SMT_LEVEL	0

/* leaf 0xb sub-leaf types */
#define INVALID_TYPE	0
#define SMT_TYPE	1
#define CORE_TYPE	2

#define LEAFB_SUBTYPE(ecx)		(((ecx) >> 8) & 0xff)
#define BITS_SHIFT_NEXT_LEVEL(eax)	((eax) & 0x1f)
#define LEVEL_MAX_SIBLINGS(ebx)		((ebx) & 0xffff)

/*
 * Check for extended topology enumeration cpuid leaf 0xb and if it
 * exists, use it for populating initial_apicid and cpu topology
 * detection.
 */
void detect_extended_topology(struct cpuinfo_x86 *c)
{
}
