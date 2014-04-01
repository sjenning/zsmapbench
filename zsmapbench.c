/*
 * zsmapbench.c
 *
 * Microbenchmark for zsmalloc allocation mapping
 *
 * This is x86 only because it uses rdtscll to get the number of elapsed cycles
*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/sched.h>

#include "linux/zsmalloc.h"

#if !defined(CONFIG_X86) && !defined(CONFIG_CPU_V6)
#error "CPU not supported by zsmapbench"
#endif

#ifdef CONFIG_CPU_V6

#define ARMV6_PMCR_ENABLE		(1 << 0)
#define ARMV6_PMCR_CCOUNT_RESET		(1 << 2)
#define ARMV6_PMCR_CCOUNT_OVERFLOW	(1 << 10)

static inline unsigned long
armv6_pmcr_read(void)
{
	unsigned long val;
	asm volatile("mrc   p15, 0, %0, c15, c12, 0" : "=r"(val));
	return val;
}

static inline void
armv6_pmcr_write(unsigned long val)
{
	asm volatile("mcr   p15, 0, %0, c15, c12, 0" : : "r"(val));
}

static inline unsigned long
armv6_pmcr_ccnt_has_overflowed(unsigned long pmcr)
{
	return !(!(pmcr & ARMV6_PMCR_CCOUNT_OVERFLOW));
}

static inline unsigned long 
armv6pmu_read_ccnt(void)
{
	unsigned long value = 0;
	asm volatile("mrc   p15, 0, %0, c15, c12, 1" : "=r"(value));
	return value;
}

static inline void
armv6pmu_write_ccnt(u32 value)
{
	asm volatile("mcr   p15, 0, %0, c15, c12, 1" : : "r"(value));
}

#endif /* CONFIG_CPU_V6 */

static int zsmb_kthread(void *ptr)
{
	struct zs_pool *pool;
	unsigned long *handles, start, end, dt, completed = 0;
	int i, err;
	char *buf;
	/*
	 * This size is roughly 40% of PAGE_SIZE an results in an
	 * underlying zspage size of 2 pages.  See the
	 * get_pages_per_zspage() function in zsmalloc for details.
	 * The third allocation in this class will span two pages.
	*/
	int size = 1632;
	int handles_nr = 3;
	int spanned_index = handles_nr - 1;
#ifdef CONFIG_CPU_V6
	unsigned long pmcr;
#endif

	pr_info("starting zsmb_kthread\n");

	pool = zs_create_pool(GFP_NOIO | __GFP_HIGHMEM);
	if (!pool)
		return -ENOMEM;

	handles = (unsigned long *)kmalloc(handles_nr * sizeof(unsigned long),
					GFP_KERNEL);
	if (!handles) {
		pr_info("kmalloc failed\n");
		return -ENOMEM;
	}
	memset(handles, 0, sizeof(unsigned long) * handles_nr);

	for (i = 0; i < handles_nr; i++) {
		handles[i] = zs_malloc(pool, size);
		if(!handles[i]) {
			pr_err("zs_malloc failed\n");
			err = -ENOMEM;
			goto free;
		}
	}

#ifdef CONFIG_CPU_V6
	pmcr = armv6_pmcr_read();
	pmcr |= ARMV6_PMCR_ENABLE | ARMV6_PMCR_CCOUNT_RESET;
	armv6_pmcr_write(pmcr);
	armv6pmu_write_ccnt(0);
	start = 0;
#else
	rdtscll(start);
#endif

	while (unlikely(!kthread_should_stop())) {
		buf = zs_map_object(pool, handles[spanned_index], ZS_MM_RW);
		if (unlikely(!buf)) {
			pr_err("zs_map_object failed\n");
			err = -EINVAL;
			goto free;
		}
		zs_unmap_object(pool, handles[spanned_index]);
		completed++;
		cond_resched();
	}

#ifdef CONFIG_CPU_V6
	end = armv6pmu_read_ccnt();
	pmcr = armv6_pmcr_read();
	pmcr &= ~ARMV6_PMCR_ENABLE;
	armv6_pmcr_write(pmcr);
#else
	rdtscll(end);
#endif

	dt = end - start;
	pr_info("%lu cycles\n",dt);
	pr_info("%lu mappings\n",completed);
	pr_info("%lu cycles/map\n",dt/completed);

	pr_info("stopping zsmb_kthread\n");
	err = 0;

free:
	for (i = 0; i < handles_nr; i++)
		if (handles[i])
			zs_free(pool, handles[i]);
	if (handles)
		kfree(handles);
	zs_destroy_pool(pool);
	return err;		
}

/*
 * This benchmark isn't made to handle changes in the cpu online mask.
 * Please don't hotplug while the benchmark runs.
*/
static DEFINE_PER_CPU(struct task_struct *, pcpu_kthread);
static bool single_threaded;
module_param(single_threaded, bool, 0);

static int __init zsmb_init(void)
{
	struct task_struct **kthread;
	int cpu;

	pr_info("running zsmapbench...\n");

	for_each_online_cpu(cpu) {
		kthread = per_cpu_ptr(&pcpu_kthread, cpu);
		*kthread =
			kthread_create(zsmb_kthread, NULL, "zsmb_kthread");
		if (IS_ERR(*kthread))
			return IS_ERR(*kthread);
		kthread_bind(*kthread, cpu);
		if (single_threaded)
			break;
	}

	for_each_online_cpu(cpu) {
		kthread = per_cpu_ptr(&pcpu_kthread, cpu);
		wake_up_process(*kthread);
		if (single_threaded)
			break;
	}

	/* Run for about one second */
	msleep(1000);

	for_each_online_cpu(cpu) {
		kthread = per_cpu_ptr(&pcpu_kthread, cpu);
		kthread_stop(*kthread);
		if (single_threaded)
			break;
	}

	pr_info("zsmapbench complete\n");

	return 0;
}

static void __exit zsmb_exit(void)
{
	pr_info("unloading zsmapbench\n");
}

module_init(zsmb_init);
module_exit(zsmb_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Seth Jennings <sjenning@linux.vnet.ibm.com");
MODULE_DESCRIPTION("Microbenchmark for zsmalloc mapping methods");
