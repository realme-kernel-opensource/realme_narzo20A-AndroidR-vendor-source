#define pr_fmt(fmt) "boostpool: " fmt

#include <asm/page.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/scatterlist.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/sizes.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/proc_fs.h>
#include <linux/vmstat.h>
#include <linux/oom.h>
#include <linux/poll.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <uapi/linux/sched/types.h>
#include <../../../../kernel/sched/sched.h>

#include "ion.h"
#include "ion_system_heap.h"
#include "oplus_ion_boost_pool.h"

#define MAX_BOOST_POOL_HIGH (901 * 256)

static bool boost_pool_enable = true;
static wait_queue_head_t kcrit_scene_wait;
static int kcrit_scene_flag = 0;
struct kmem_cache *boost_ion_info_cachep;
bool create_kmemcache_ion_info_success = false;

static inline unsigned int order_to_size(int order)
{
	return PAGE_SIZE << order;
}

static int page_pool_nr_pages(struct ion_page_pool *pool)
{
	if (NULL == pool) {
		pr_err("%s: pool is NULL!\n", __func__);
		return 0;
	}
	return (pool->low_count + pool->high_count) << pool->order;
}

static int boost_pool_nr_pages(struct ion_boost_pool *pool)
{
	int i;
	int count = 0;

	if (NULL == pool) {
		pr_err("%s: pool is NULL!\n", __func__);
		return 0;
	}

	for (i = 0; i < NUM_ORDERS; i++)
		count += page_pool_nr_pages(pool->pools[i]);

	return count;
}

static int fill_boost_page_pool(struct ion_page_pool *pool)
{
	struct page *page;
	struct device *dev = pool->dev;

	if (NULL == pool) {
		pr_err("%s: pool is NULL!\n", __func__);
		return -ENOENT;
	}
	page = ion_page_pool_alloc_pages(pool);
	if (NULL == page)
		return -ENOMEM;

	ion_pages_sync_for_device(dev, page,
				  PAGE_SIZE << pool->order,
				  DMA_BIDIRECTIONAL);

	ion_page_pool_free(pool, page);

	return 0;
}

void boost_pool_dump(struct ion_boost_pool *pool)
{
	int i;

	pr_info("Name:%s: %dMib, low: %dMib, high:%dMib\n",
		pool->name,
		boost_pool_nr_pages(pool) >> 8,
		pool->low >> 8,
		pool->high >> 8);
	for (i = 0; i < NUM_ORDERS; i++) {
		struct ion_page_pool *page_pool = pool->pools[i];

		pr_info("%d order %u highmem pages in boost pool = %lu total\n",
			page_pool->high_count, page_pool->order,
			(PAGE_SIZE << page_pool->order) * page_pool->high_count);
		pr_info("%d order %u lowmem pages in boost pool = %lu total\n",
			page_pool->low_count, page_pool->order,
			(PAGE_SIZE << page_pool->order) * page_pool->low_count);
	}
}
extern int direct_vm_swappiness;
static int boost_pool_kworkthread(void *p)
{
	int i;
	struct ion_boost_pool *pool;
	int ret;

	if (NULL == p) {
		pr_err("%s: p is NULL!\n", __func__);
		return 0;
	}
    printk(KERN_EMERG"%s[start]：now :%d Kbytes，fill to %d Kbytes\n",
		__func__, boost_pool_nr_pages(pool), ((pool->high) << 12));
	pool = (struct ion_boost_pool *)p;
	while (true) {
		ret = wait_event_interruptible(pool->waitq,
					       (pool->wait_flag == 1));
		if (ret < 0)
			continue;

		pool->wait_flag = 0;
		direct_vm_swappiness = 20;
		for (i = 0; i < NUM_ORDERS; i++) {
			while (boost_pool_nr_pages(pool) < pool->high) {
				if (fill_boost_page_pool(pool->pools[i]) < 0)
					break;
			}
		}

		for (i = 1; i < NUM_ORDERS; i++) {
			while (page_pool_nr_pages(pool->pools[i]) <
			       LOWORDER_WATER_MASK) {
				if (fill_boost_page_pool(pool->pools[i]) < 0)
					break;
			}
		}
		direct_vm_swappiness = 60;
	}
    printk(KERN_EMERG"%s[end]：now :%d Kbytes，amid to %d Kbytes\n",
		__func__, boost_pool_nr_pages(pool), ((pool->high) << 12));
	return 0;
}

struct page_info *boost_pool_allocate(struct ion_boost_pool *pool,
				      unsigned long size,
				      unsigned int max_order)
{
	int i;
	struct page *page;
	struct page_info *info;
	bool from_pool = true;

	if (NULL == pool) {
		pr_err("%s: pool is NULL!\n", __func__);
		return NULL;
	}
	if (likely(create_kmemcache_ion_info_success)) {
		info = kmem_cache_zalloc(boost_ion_info_cachep, GFP_ATOMIC);
		if (!info)
			info = kmem_cache_zalloc(boost_ion_info_cachep, GFP_KERNEL);
		if (unlikely(!info))
			return ERR_PTR(-ENOMEM);
		info->from_boost_kmem_cache = true; 
	} else {
		info = kmalloc(sizeof(*info), GFP_KERNEL);
		if (unlikely(!info))
			return ERR_PTR(-ENOMEM);
		info->from_boost_kmem_cache = false; 	
	}

	if (!info)
		return NULL;
	for (i = 0; i < NUM_ORDERS; i++) {
		if (size < order_to_size(orders[i]))
			continue;
		if (max_order < orders[i])
			continue;
		page = ion_page_pool_alloc(pool->pools[i], &from_pool);
		if (IS_ERR(page))
			continue;

		info->page = page;
		info->order = orders[i];

		info->from_pool = true;
		INIT_LIST_HEAD(&info->list);
		return info;
	}
	kfree(info);

	return NULL;
}

void boost_pool_wakeup_process(struct ion_boost_pool *pool)
{
	if (!boost_pool_enable)
		return;

	if (NULL == pool) {
		pr_err("%s: boost_pool is NULL!\n", __func__);
		return;
	}

	pool->wait_flag = 1;
	wake_up_interruptible(&pool->waitq);
}

static void boost_pool_all_free(struct ion_boost_pool *pool, gfp_t gfp_mask,
				int nr_to_scan)
{
	int i;

	if (NULL == pool) {
		pr_err("%s: boost_pool is NULL!\n", __func__);
		return;
	}

	for (i = 0; i < NUM_ORDERS; i++)
		ion_page_pool_shrink(pool->pools[i], gfp_mask, nr_to_scan);
}

static bool boost_pool_is_full(struct ion_boost_pool *boost_pool)
{
	return boost_pool_nr_pages(boost_pool) >= boost_pool->gr_pool_should_protect;
}

int boost_pool_free(struct ion_boost_pool *pool, struct page *page,
		    int order)
{
	if (!boost_pool_enable) {
		boost_pool_all_free(pool, __GFP_HIGHMEM, MAX_POOL_SIZE);
		return -1;
	}

	if (order == 0 && pool->usage != ION_FLAG_GRAPHIC_BUFFER)
		return -1;

	if ((NULL == pool) || (NULL == page)) {
		pr_err("%s: pool/page is NULL!\n", __func__);
		return -1;
	}

	if (!((order == orders[0]) || (order == orders[1])
	      || (order == orders[2]))) {
		pr_err("%s: order:%d is error!\n", __func__, order);
		return -1;
	}
	
	if (!boost_pool_is_full(pool)) {
		ion_page_pool_free(pool->pools[order_to_index(order)], page);
		return 0;
	}

	return -1;
}

int boost_pool_shrink(struct ion_boost_pool *boost_pool,
		      struct ion_page_pool *pool, gfp_t gfp_mask,
		      int nr_to_scan)
{
	int nr_max_free;
	int nr_to_free;
	int nr_total = 0;
	int other_free = 0, other_file = 0;
	int pool_max_shrink = 1024;

	other_free = global_zone_page_state(NR_FREE_PAGES);
	other_file = global_node_page_state(NR_ACTIVE_FILE) + global_node_page_state(NR_INACTIVE_FILE);

	if ((NULL == boost_pool) || (NULL == pool)) {
		pr_err("%s: boostpool/pool is NULL!\n", __func__);
		return 0;
	}

	if (boost_pool_is_full(boost_pool)) {
		int pool_max_shrink = boost_pool_nr_pages(boost_pool) - boost_pool->gr_pool_should_protect;
		
		if (pool_max_shrink <= 0) {
			if (other_free + other_file > (totalram_pages >> 3))
				return 0;
			pool_max_shrink = 1024;
		}
	} else {
		if (other_free + other_file > (totalram_pages >> 3))
			return 0;
	}

	if (nr_to_scan == 0) {
		nr_to_scan = pool_max_shrink;
		return ion_page_pool_shrink(pool, gfp_mask, 0);
	}

	nr_max_free = boost_pool_nr_pages(boost_pool) -
		(boost_pool->high + LOWORDER_WATER_MASK);
	nr_to_free = min(nr_max_free, nr_to_scan);

	if (nr_to_free <= 0)
		return 0;

	nr_total = ion_page_pool_shrink(pool, gfp_mask, nr_to_free);
	return nr_total;
}

void boost_pool_dec_high(struct ion_boost_pool *pool, int nr_pages)
{
	if (unlikely(nr_pages < 0))
		return;

	pool->high = max(pool->low,
			 pool->high - nr_pages);
	if (pool->high <= pool->low) {
		pool->gr_pool_should_protect = pool->low;
	}
}

static int bind_task_min_cap_cpus(struct task_struct *tsk)
{
	struct cpumask mask;
	int i, ret;
	int end_cpu = 6;

	cpumask_clear(&mask);
	for (i = 0; i < end_cpu; i++)
		cpumask_set_cpu(i, &mask);
	/* TODO FIX this */
	ret = sched_setaffinity(tsk->pid, &mask);
	pr_info("bind %s on cpu[0-%d].\n", tsk->comm, end_cpu);

	return ret;
}

static int boost_pool_proc_show(struct seq_file *s, void *v)
{
	struct ion_boost_pool *boost_pool = s->private;
	int i;

	seq_printf(s, "Name:%s: %dMib, low: %dMib high: %dMib\n",
		   boost_pool->name,
		   boost_pool_nr_pages(boost_pool) >> 8,
		   boost_pool->low >> 8,
		   boost_pool->high >> 8);

	for (i = 0; i < NUM_ORDERS; i++) {
		struct ion_page_pool *pool = boost_pool->pools[i];

		seq_printf(s, "%d order %u highmem pages in boost pool = %lu total\n",
			   pool->high_count, pool->order,
			   (PAGE_SIZE << pool->order) * pool->high_count);
		seq_printf(s, "%d order %u lowmem pages in boost pool = %lu total\n",
			   pool->low_count, pool->order,
			   (PAGE_SIZE << pool->order) * pool->low_count);
	}
	return 0;
}

static int boost_pool_proc_open(struct inode *inode, struct file *file)
{
	struct ion_boost_pool *data = PDE_DATA(inode);
	return single_open(file, boost_pool_proc_show, data);
}
static ssize_t boost_pool_proc_write(struct file *file,
				     const char __user *buf,
				     size_t count, loff_t *ppos)
{
	char buffer[13];
	int err, nr_pages;
	struct ion_boost_pool *boost_pool = PDE_DATA(file_inode(file));

	if (IS_ERR(boost_pool)) {
		pr_err("%s: boost pool is NULL.\n");
		return -EFAULT;
	}

	memset(buffer, 0, sizeof(buffer));
	if (count > sizeof(buffer) - 1)
		count = sizeof(buffer) - 1;
	if (copy_from_user(buffer, buf, count))
		return -EFAULT;

	err = kstrtoint(strstrip(buffer), 0, &nr_pages);
	if(err)
		return err;

	if (nr_pages < 0 || nr_pages > MAX_BOOST_POOL_HIGH)
		return -EINVAL;

	if (nr_pages == 0) {
		boost_pool->gr_pool_should_protect = boost_pool->high = boost_pool->low;
		return count;
	}
	pr_info("%s: open camera, set high wm => %dMib.\n", __func__, nr_pages >> 8);
	kcrit_scene_flag = 1;
	wake_up_interruptible(&kcrit_scene_wait);

	boost_pool->gr_pool_should_protect = nr_pages;
	boost_pool->high = nr_pages;
	boost_pool_wakeup_process(boost_pool);

	return count;
}

static const struct file_operations boost_pool_proc_ops = {
	.owner          = THIS_MODULE,
	.open           = boost_pool_proc_open,
	.read           = seq_read,
	.write          = boost_pool_proc_write,
	.llseek         = seq_lseek,
	.release        = single_release,
};

struct ion_boost_pool *boost_pool_create(struct ion_system_heap *heap,
					 unsigned int ion_flag,
					 unsigned int nr_pages,
					 struct proc_dir_entry *root_dir,
					 char *name)
{
	bool graphic_buffer_flag = true;
	struct task_struct *tsk;
	struct ion_boost_pool *boost_pool;

	if (NULL == root_dir) {
		pr_err("boost_pool dir not exits.\n");
		return NULL;
	}

	boost_pool = kzalloc(sizeof(struct ion_boost_pool) +
			     sizeof(struct ion_page_pool *) * NUM_ORDERS,
			     GFP_KERNEL);

	if (NULL == boost_pool) {
		pr_err("%s: boost_pool is NULL!\n", __func__);
		return NULL;
	}

	if (ion_system_heap_create_pools(heap, boost_pool->pools,
					 false, graphic_buffer_flag))
		goto free_heap;

	boost_pool->high = boost_pool->low = nr_pages;
	if (nr_pages == 256 * 256)
		boost_pool->low = 0x8000;
	boost_pool->name = name;
	boost_pool->usage = ion_flag;

	boost_pool->proc_info = proc_create_data(name, 0666,
						 root_dir,
						 &boost_pool_proc_ops,
						 boost_pool);
	if (IS_ERR(boost_pool->proc_info)) {
		pr_info("Unable to initialise /proc/%s\n",
			name);
		goto destroy_pools;
	} else {
		pr_info("procfs entry /proc/%s allocated. data=%p\n",
			name, boost_pool);
	}

	init_waitqueue_head(&boost_pool->waitq);
	tsk = kthread_run(boost_pool_kworkthread, boost_pool,
			  "%s", name);
	if (IS_ERR(tsk)) {
		pr_err("%s: kthread_create failed!\n", __func__);
		goto destroy_proc_info;
	}

	bind_task_min_cap_cpus(tsk);
	boost_pool_wakeup_process(boost_pool);

	return boost_pool;
destroy_proc_info:
	proc_remove(boost_pool->proc_info);
destroy_pools:
	ion_system_heap_destroy_pools(boost_pool->pools);
free_heap:
	kfree(boost_pool);
	boost_pool = NULL;
	return NULL;
}

static unsigned int kcrit_scene_proc_poll(struct file *file, poll_table *table)
{
	int mask = 0;

	poll_wait(file, &kcrit_scene_wait, table);

	if (kcrit_scene_flag == 1) {
		mask |= POLLIN | POLLRDNORM;
		kcrit_scene_flag = 0;
	}
	return mask;
}

static int kcrit_scene_proc_open(struct inode *inode, struct file *file)
{
	return nonseekable_open(inode, file);
}

static int kcrit_scene_proc_release(struct inode *inode, struct file *file)
{
	return 0;
}


/* TODO add read ops. */
static const struct file_operations kcrit_scene_proc_fops = {
	.owner = THIS_MODULE,
	.open = kcrit_scene_proc_open,
	.release = kcrit_scene_proc_release,
	.poll = kcrit_scene_proc_poll,
};

static void init_once(void *foo)
{
	;
}

static __init int kcrit_scene_init(void)
{
	init_waitqueue_head(&kcrit_scene_wait);
	proc_create("kcritical_scene", S_IRUGO, NULL,
		    &kcrit_scene_proc_fops);
	
	boost_ion_info_cachep = kmem_cache_create("boost_ion_info_cachep",
								sizeof(struct page_info), 0,
								0, init_once);
	if (boost_ion_info_cachep != NULL)
		create_kmemcache_ion_info_success = true;
	else
		pr_err("boost_ion_info_cachep create failed\n");
	return 0;
}
fs_initcall(kcrit_scene_init);
module_param_named(debug_boost_pool_enable, boost_pool_enable, bool, 0644);
