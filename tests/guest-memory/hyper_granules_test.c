// SPDX-FileCopyrightText: 2026 roolrz
// SPDX-License-Identifier: GPL-2.0-only

/* Exercise the production registry and retirement code with real Linux locks,
 * workqueues and resource claims. Only foreign-page import and its potentially
 * blocking release are substituted: no test maps or touches physical memory.
 * The HypeR broker tests separately exercise actual page and DMA references.
 */
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/ioport.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/memremap.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sched/task.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>

#include "../../modules/guest-memory/granules.h"

#define TEST_RECORDS (HYPER_GRANULE_RETIRE_BATCH * 4 + 5)
#define TEST_BASE (1ULL << 40)
#define TEST_FIRST_INDEX (TEST_BASE / SZ_2M)
#define TEST_TIMEOUT msecs_to_jiffies(10000)

struct test_record {
	atomic_t imported;
	atomic_t imports;
	atomic_t releases;
	bool hold;
	struct completion entered;
	struct completion proceed;
};

static struct test_record records[TEST_RECORDS];
static atomic_t callback_errors = ATOMIC_INIT(0);
static int fail_import = -1;

static struct test_record *record_for(struct dev_pagemap *pgmap)
{
	u64 offset = pgmap->range.start - TEST_BASE;

	if (pgmap->range.start < TEST_BASE || offset / SZ_2M >= TEST_RECORDS ||
	    offset % SZ_2M || pgmap->nr_range != 1 ||
	    pgmap->range.end != pgmap->range.start + SZ_2M - 1) {
		atomic_inc(&callback_errors);
		return NULL;
	}
	return &records[offset / SZ_2M];
}

static void *test_memremap_pages(struct dev_pagemap *pgmap, int nid)
{
	struct test_record *record = record_for(pgmap);

	(void)nid;
	if (!record)
		return ERR_PTR(-EINVAL);
	if (record - records == fail_import)
		return ERR_PTR(-ENOMEM);
	if (atomic_cmpxchg(&record->imported, 0, 1)) {
		atomic_inc(&callback_errors);
		return ERR_PTR(-EBUSY);
	}
	atomic_inc(&record->imports);
	return pgmap;
}

static void test_memunmap_pages(struct dev_pagemap *pgmap)
{
	struct test_record *record = record_for(pgmap);

	if (!record)
		return;
	complete(&record->entered);
	if (record->hold)
		wait_for_completion(&record->proceed);
	if (atomic_cmpxchg(&record->imported, 1, 0) != 1)
		atomic_inc(&callback_errors);
	atomic_inc(&record->releases);
}

/* Include the implementation, not a copy of its scheduling/ownership model.
 * Headers were included above so the substitutions affect only its calls.
 */
#define memremap_pages test_memremap_pages
#define memunmap_pages test_memunmap_pages
#include "../../modules/guest-memory/granules.c"
#undef memunmap_pages
#undef memremap_pages

struct release_job {
	struct task_struct *task;
	struct completion done;
	unsigned int count;
	bool released;
	struct hyper_granule *items[TEST_RECORDS];
};

static bool check(bool condition, const char *message)
{
	if (!condition)
		pr_err("HypeR granule retirement test: %s\n", message);
	return condition;
}

static void reset_records(void)
{
	unsigned int i;

	fail_import = -1;
	atomic_set(&callback_errors, 0);
	for (i = 0; i < TEST_RECORDS; ++i) {
		atomic_set(&records[i].imported, 0);
		atomic_set(&records[i].imports, 0);
		atomic_set(&records[i].releases, 0);
		records[i].hold = false;
		init_completion(&records[i].entered);
		init_completion(&records[i].proceed);
	}
}

static struct release_job *acquire_job(unsigned int first, unsigned int count)
{
	struct release_job *job = kzalloc(sizeof(*job), GFP_KERNEL);
	unsigned int i;

	if (!job)
		return NULL;
	init_completion(&job->done);
	for (i = 0; i < count; ++i) {
		struct hyper_granule *granule =
			hyper_granule_get(TEST_FIRST_INDEX + first + i);

		if (IS_ERR(granule)) {
			hyper_granules_put(job->items, job->count);
			kfree(job);
			return NULL;
		}
		job->items[job->count++] = granule;
	}
	return job;
}

static int granules_test_release_thread(void *argument)
{
	struct release_job *job = argument;

	hyper_granules_put(job->items, job->count);
	job->released = true;
	complete(&job->done);
	return 0;
}

static bool start_job(struct release_job *job)
{
	job->task = kthread_create(granules_test_release_thread, job, "hyper-retire-test");
	if (IS_ERR(job->task)) {
		job->task = NULL;
		return false;
	}
	/* Keep the task valid even when its function completes before our join. */
	get_task_struct(job->task);
	wake_up_process(job->task);
	return true;
}

static bool consumed(const struct release_job *job)
{
	unsigned int i;

	for (i = 0; i < job->count; ++i)
		if (!check(!job->items[i], "retirement left an owned array entry"))
			return false;
	return true;
}

static void destroy_job(struct release_job *job)
{
	if (!job)
		return;
	if (job->task) {
		kthread_stop(job->task);
		put_task_struct(job->task);
	}
	if (!job->released) {
		hyper_granules_put(job->items, job->count);
	}
	kfree(job);
}

static bool released_range(unsigned int first, unsigned int count)
{
	unsigned int i;
	bool success = check(!atomic_read(&callback_errors), "invalid import/release callback");

	for (i = first; i < first + count; ++i) {
		success &= check(!atomic_read(&records[i].imported), "import still live after join");
		success &= check(atomic_read(&records[i].imports) == 1,
				 "resource imported more than once while shared");
		success &= check(atomic_read(&records[i].releases) == 1,
				 "resource not released exactly once");
	}
	return success;
}

static bool test_sizes(void)
{
	const unsigned int sizes[] = {
		0, 1, HYPER_GRANULE_RETIRE_BATCH,
		HYPER_GRANULE_RETIRE_BATCH + 1, TEST_RECORDS,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(sizes); ++i) {
		struct release_job *job;
		bool success;

		reset_records();
		job = acquire_job(0, sizes[i]);
		if (!check(job != NULL, "size test admission failed"))
			return false;
		hyper_granules_put(job->items, job->count);
		success = consumed(job) && released_range(0, sizes[i]);
		kfree(job);
		if (!success)
			return false;
	}
	return true;
}

static bool test_join_and_admission(void)
{
	struct release_job *job;
	struct hyper_granule *replacement = NULL;
	bool success;

	reset_records();
	job = acquire_job(0, HYPER_GRANULE_RETIRE_BATCH * 2 + 5);
	if (!check(job != NULL, "join test admission failed"))
		return false;
	records[0].hold = true;
	success = check(start_job(job), "cannot start retirement thread");
	if (!success)
		goto out;
	success = check(wait_for_completion_timeout(&records[0].entered, TEST_TIMEOUT),
			"retirement callback did not begin");
	if (!success)
		goto out;
	success &= check(!completion_done(&job->done), "release returned before the held callback");
	success &= check(wait_for_completion_timeout(&records[1].entered, TEST_TIMEOUT),
			 "independent teardown did not overlap the held callback");
	success &= check(!completion_done(&records[HYPER_GRANULE_RETIRE_BATCH].entered),
			 "next batch began before joining the current batch");
	/* The registry entry is detached, but the real physical resource claim
	 * must remain until the blocking callback completes. */
	replacement = hyper_granule_get(TEST_FIRST_INDEX);
	success &= check(IS_ERR(replacement) && PTR_ERR(replacement) == -EBUSY,
			 "admission reused a granule during retirement");
out:
	complete_all(&records[0].proceed);
	if (replacement && !IS_ERR(replacement))
		hyper_granules_put(&replacement, 1);
	if (job->task) {
		success &= check(wait_for_completion_timeout(&job->done, TEST_TIMEOUT),
				 "retirement did not finish after releasing its callback");
		/* Join before reading the array or allowing any test-owned storage to die. */
		kthread_stop(job->task);
		put_task_struct(job->task);
		job->task = NULL;
		success &= consumed(job);
		success &= released_range(0, job->count);
	}
	destroy_job(job);
	if (!success)
		return false;
	replacement = hyper_granule_get(TEST_FIRST_INDEX);
	if (!check(!IS_ERR(replacement), "admission failed after completed retirement"))
		return false;
	hyper_granules_put(&replacement, 1);
	return check(atomic_read(&records[0].imports) == 2 &&
		     atomic_read(&records[0].releases) == 2,
		     "reopened granule did not receive a fresh lifetime");
}

static bool test_shared(void)
{
	struct release_job *first, *second;
	bool success;

	reset_records();
	first = acquire_job(0, 1);
	second = acquire_job(0, 1);
	if (!check(first && second, "shared admission failed")) {
		destroy_job(first);
		destroy_job(second);
		return false;
	}
	success = check(first->items[0] == second->items[0], "shared granule has two owners");
	hyper_granules_put(first->items, first->count);
	success &= consumed(first);
	success &= check(atomic_read(&records[0].imported) == 1 &&
			 !atomic_read(&records[0].releases),
			 "first client removed a surviving client's granule");
	hyper_granules_put(second->items, second->count);
	success &= consumed(second);
	success &= released_range(0, 1);
	kfree(first);
	kfree(second);
	return success;
}

static bool test_concurrent_callers(void)
{
	const unsigned int count = HYPER_GRANULE_RETIRE_BATCH + 5;
	const unsigned int second_first = HYPER_GRANULE_RETIRE_BATCH / 2;
	const unsigned int last = second_first + count - 1;
	struct release_job *first, *second;
	unsigned int i;
	bool success;

	reset_records();
	first = acquire_job(0, count);
	second = acquire_job(second_first, count);
	if (!check(first && second, "concurrent admission failed")) {
		destroy_job(first);
		destroy_job(second);
		return false;
	}
	records[0].hold = true;
	records[last].hold = true;
	success = check(start_job(first), "cannot start first caller");
	if (success)
		success = check(wait_for_completion_timeout(&records[0].entered, TEST_TIMEOUT),
				 "first caller did not begin retirement");
	if (success)
		success = check(start_job(second), "cannot start second caller");
	if (!success)
		goto out;
	success &= check(wait_for_completion_timeout(&records[last].entered, TEST_TIMEOUT),
			 "independent caller could not progress while first caller waited");
	success &= check(!completion_done(&first->done) && !completion_done(&second->done),
			 "concurrent caller returned with its callback still held");
	/* The first caller consumed these non-final entries before it blocked;
	 * the second has now freed them. Its predecessor must skip the NULLs. */
	for (i = second_first; i < HYPER_GRANULE_RETIRE_BATCH; ++i)
		success &= check(atomic_read(&records[i].releases) == 1,
				 "concurrent last user did not retire the shared prefix");
out:
	complete_all(&records[0].proceed);
	complete_all(&records[last].proceed);
	if (first->task)
		success &= check(wait_for_completion_timeout(&first->done, TEST_TIMEOUT),
				 "first concurrent release did not finish");
	if (second->task)
		success &= check(wait_for_completion_timeout(&second->done, TEST_TIMEOUT),
				 "second concurrent release did not finish");
	destroy_job(first);
	destroy_job(second);
	return success && released_range(0, last + 1);
}

static bool test_failed_import(void)
{
	const unsigned int count = HYPER_GRANULE_RETIRE_BATCH + 4;
	struct release_job *job;
	struct hyper_granule *retry;
	unsigned int i;
	bool success = true;

	reset_records();
	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!check(job != NULL, "cannot allocate rollback fixture"))
		return false;
	fail_import = count - 1;
	for (i = 0; i < count; ++i) {
		struct hyper_granule *granule = hyper_granule_get(TEST_FIRST_INDEX + i);

		if (IS_ERR(granule)) {
			success &= check(i == count - 1 && PTR_ERR(granule) == -ENOMEM,
					 "import failed outside the injected position");
			break;
		}
		job->items[job->count++] = granule;
	}
	success &= check(job->count == count - 1, "injected import failure was missed");
	/* Exactly the acquired prefix is still owned after a failed preparation. */
	hyper_granules_put(job->items, job->count);
	success &= consumed(job);
	success &= released_range(0, job->count);
	kfree(job);
	fail_import = -1;
	retry = hyper_granule_get(TEST_FIRST_INDEX + count - 1);
	if (!check(!IS_ERR(retry), "failed import leaked its resource claim"))
		return false;
	hyper_granules_put(&retry, 1);
	return success && released_range(count - 1, 1);
}

static int __init hyper_granules_test_init(void)
{
	struct resource *window;
	int error;
	bool success;

	/* Claim-check a deliberately unmapped, high address window first. Actual
	 * imports are stubbed; request_mem_region still tests exclusion and ABA. */
	window = request_mem_region(TEST_BASE, TEST_RECORDS * SZ_2M, "hyper-retirement-test");
	if (!window)
		return -EBUSY;
	release_mem_region(TEST_BASE, TEST_RECORDS * SZ_2M);
	error = hyper_granules_init();
	if (error)
		return error;
	success = test_sizes() && test_shared() && test_join_and_admission() &&
		  test_concurrent_callers() && test_failed_import();
	hyper_granules_exit();
	window = request_mem_region(TEST_BASE, TEST_RECORDS * SZ_2M, "hyper-retirement-test");
	success &= check(window != NULL, "retirement leaked a physical resource claim");
	if (window)
		release_mem_region(TEST_BASE, TEST_RECORDS * SZ_2M);
	if (!success)
		return -EINVAL;
	pr_info("HypeR granule retirement tests: PASS\n");
	return 0;
}

static void __exit hyper_granules_test_exit(void)
{
}

module_init(hyper_granules_test_init);
module_exit(hyper_granules_test_exit);
MODULE_DESCRIPTION("HypeR granule registry and synchronous retirement regressions");
MODULE_LICENSE("GPL");
