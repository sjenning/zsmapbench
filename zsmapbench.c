/*
 * zsmapbench.c
 *
 * This is a microbenchmark for zsmalloc allocation mapping
*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/sched.h>

#include "../drivers/staging/zsmalloc/zsmalloc.h"

struct mapdata {
	struct task_struct *kthread;
	struct zs_pool *pool;
	int size;
	int cpu;
};


static int zsmb_kthread(void *ptr)
{
	struct mapdata *data = ptr;
	unsigned long *handles, start, end, dt;
	size_t handles_nr;
	/* index of the spanned handle that will need mapping */ 
	int spanned_index; 
	int i, err;
	char *buf;
	atomic_t completed;

	pr_info("starting zsmb_kthread\n");

	if (!data)
		return -EINVAL;

	handles_nr = (PAGE_SIZE * 2) / data->size;
	handles = (unsigned long *)kmalloc(handles_nr * sizeof(unsigned long),
					GFP_KERNEL);
	if (!handles) {
		pr_info("kmalloc failed\n");
		return -ENOMEM;
	}
	memset(handles, 0, sizeof(unsigned long) * handles_nr);
	spanned_index = handles_nr / 2;

	for (i = 0; i < handles_nr; i++) {
		handles[i] = zs_malloc(data->pool, data->size);
		if(!handles[i]) {
			pr_err("zs_malloc failed\n");
			err = -ENOMEM;
			goto free;
		}
	}

	rdtscll(start);

	while (unlikely(!kthread_should_stop())) {
		buf = zs_map_object(data->pool, handles[spanned_index]);
		if (unlikely(!buf)) {
			pr_err("zs_map_object failed\n");
			err = -EINVAL;
			goto free;
		}
		zs_unmap_object(data->pool, handles[spanned_index]);
		atomic_inc(&completed);
		cond_resched();
	}

	rdtscll(end);

	dt = end - start;
	pr_info("%lu cycles\n",dt);
	pr_info("%d mappings\n",atomic_read(&completed));
	pr_info("%lu cycles/map\n",dt/(unsigned long)atomic_read(&completed));

	pr_info("stopping zsmb_kthread\n");
	err = 0;

free:
	for (i = 0; i < handles_nr; i++)
		if (handles[i])
			zs_free(data->pool, handles[i]);
	if (handles)
		kfree(handles);
	return err;		
}

/*
 *This benchmark isn't made to handle changes in the
 * cpu online mask. Please don't hotplug while the
 * benchmark runs
*/
DEFINE_PER_CPU(struct mapdata, pcpu_data);
int single_threaded = 1;

static int __init zsmb_init(void)
{
	struct mapdata *data;
	int cpu;

	pr_info("zstest init\n");

	/*
	 * This size is roughly 40% of PAGE_SIZE an results in an
	 * underlying zspage size of 2 pages.  See the
	 * get_pages_per_zspage() function in zsmalloc for details.
	 * The third allocation in this class will span two pages.
	*/
	for_each_online_cpu(cpu) {
		data = per_cpu_ptr(&pcpu_data, cpu);
		data->size = 1632;
		data->pool = zs_create_pool("zsmb", GFP_NOIO | __GFP_HIGHMEM);
		if (!data->pool)
			return -ENOMEM;
		data->cpu = cpu;
		data->kthread =
			kthread_create(zsmb_kthread, data, "zsmb_kthread");
		if (IS_ERR(data->kthread))
			return IS_ERR(data->kthread);
		kthread_bind(data->kthread, cpu);
		if (single_threaded)
			break;
	}

	for_each_online_cpu(cpu) {
		data = per_cpu_ptr(&pcpu_data, cpu);
		wake_up_process(data->kthread);
		if (single_threaded)
			break;
	}

	msleep(1000);

	for_each_online_cpu(cpu) {
		data = per_cpu_ptr(&pcpu_data, cpu);
		kthread_stop(data->kthread);
		if (single_threaded)
			break;
	}

	return 0;
}

static void __exit zsmb_exit(void)
{
	pr_info("zstest exit\n");
}

module_init(zsmb_init);
module_exit(zsmb_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Seth Jennings <sjenning@linux.vnet.ibm.com");
MODULE_DESCRIPTION("Microbenchmark for zsmalloc mapping methods");
