// SPDX-FileCopyrightText: 2026 roolrz
// SPDX-License-Identifier: GPL-2.0-only

/* Shared device-page metadata. Grant authorization remains per 4 KiB page. */
#include <linux/ioport.h>
#include <linux/memremap.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>
#include "granules.h"

struct hyper_granule {
	unsigned int users;
	struct work_struct retire_work;
	struct dev_pagemap pgmap;
};

static DEFINE_MUTEX(granules_lock);
static DEFINE_XARRAY(granules);
static struct workqueue_struct *retirement_queue;

static void granule_retire(struct work_struct *work)
{
	struct hyper_granule *granule =
		container_of(work, struct hyper_granule, retire_work);

	/* The resource claim excludes new admission while Linux waits for every
	 * page reference and RCU reader. No owner/registry lock is needed here.
	 */
	memunmap_pages(&granule->pgmap);
	release_mem_region(granule->pgmap.range.start, SZ_2M);
	/* The joining caller retains the work object until flush_work returns. */
}

struct hyper_granule *hyper_granule_get(unsigned long index)
{
	struct hyper_granule *granule;
	void *mapping;
	int result;

	mutex_lock(&granules_lock);
	granule = xa_load(&granules, index);
	if (granule) {
		++granule->users;
		goto out;
	}
	granule = kzalloc(sizeof(*granule), GFP_KERNEL);
	if (!granule) {
		granule = ERR_PTR(-ENOMEM);
		goto out;
	}
	INIT_WORK(&granule->retire_work, granule_retire);
	granule->pgmap.type = MEMORY_DEVICE_GENERIC;
	granule->pgmap.owner = &granules;
	granule->pgmap.nr_range = 1;
	granule->pgmap.range.start = (u64)index * SZ_2M;
	granule->pgmap.range.end = granule->pgmap.range.start + SZ_2M - 1;
	if (!request_mem_region(granule->pgmap.range.start, SZ_2M,
				"hyper-guest-granule")) {
		result = -EBUSY;
		goto free;
	}
	mapping = memremap_pages(&granule->pgmap, NUMA_NO_NODE);
	if (IS_ERR(mapping)) {
		result = PTR_ERR(mapping);
		goto release;
	}
	result = xa_err(xa_store(&granules, index, granule, GFP_KERNEL));
	if (result) {
		memunmap_pages(&granule->pgmap);
		goto release;
	}
	granule->users = 1;
	goto out;
release:
	release_mem_region(granule->pgmap.range.start, SZ_2M);
free:
	kfree(granule);
	granule = ERR_PTR(result);
out:
	mutex_unlock(&granules_lock);
	return granule;
}

void hyper_granules_put(struct hyper_granule **owned, unsigned int count)
{
	unsigned int first = 0;

	while (first < count) {
		unsigned int end = first + min_t(unsigned int, count - first,
						HYPER_GRANULE_RETIRE_BATCH);
		unsigned int i;

		/* Detach only final users. A shared granule remains discoverable and
		 * usable by its other grants. The consumed array owns detached ones.
		 */
		mutex_lock(&granules_lock);
		for (i = first; i < end; ++i) {
			struct hyper_granule *granule = owned[i];

			if (--granule->users) {
				owned[i] = NULL;
				continue;
			}
			xa_erase(&granules, granule->pgmap.range.start / SZ_2M);
		}
		mutex_unlock(&granules_lock);
		for (i = first; i < end; ++i)
			if (owned[i])
				queue_work(retirement_queue, &owned[i]->retire_work);
		/* Overlap sleeping RCU waits, but never acknowledge release or free
		 * the caller's array while Linux still owns a page or work callback.
		 * Final detach queues each freshly initialized work item once.
		 */
		for (i = first; i < end; ++i) {
			if (!owned[i])
				continue;
			flush_work(&owned[i]->retire_work);
			kfree(owned[i]);
			owned[i] = NULL;
		}
		first = end;
	}
}

int hyper_granules_init(void)
{
	/* The current non-NUMA appliances run at most 32 callbacks concurrently.
	 * A reclaim rescuer can complete it serially under memory pressure.
	 */
	retirement_queue = alloc_workqueue("hyper-memory-retire",
			WQ_UNBOUND | WQ_MEM_RECLAIM, HYPER_GRANULE_RETIRE_BATCH);
	return retirement_queue ? 0 : -ENOMEM;
}

void hyper_granules_exit(void)
{
	destroy_workqueue(retirement_queue);
	retirement_queue = NULL;
}
