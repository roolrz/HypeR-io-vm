/* SPDX-FileCopyrightText: 2026 roolrz */
/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HYPER_GUEST_MEMORY_GRANULES_H
#define HYPER_GUEST_MEMORY_GRANULES_H

#define HYPER_GRANULE_RETIRE_BATCH 32

struct hyper_granule;

int hyper_granules_init(void);
void hyper_granules_exit(void);
struct hyper_granule *hyper_granule_get(unsigned long index);
/* Consume unique, acquired entries; return only after all final users retire. */
void hyper_granules_put(struct hyper_granule **granules, unsigned int count);

#endif
