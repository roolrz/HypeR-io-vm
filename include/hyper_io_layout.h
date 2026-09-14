// SPDX-FileCopyrightText: 2026 roolrz
// SPDX-License-Identifier: Apache-2.0
#ifndef HYPER_IO_LAYOUT_H
#define HYPER_IO_LAYOUT_H
#ifdef __KERNEL__
#include <linux/types.h>
typedef u64 hyper_layout_word;
#else
#include <stdint.h>
typedef uint64_t hyper_layout_word;
#endif
static inline int hyper_io_extent_valid(hyper_layout_word cursor, hyper_layout_word total,
 hyper_layout_word alias, hyper_layout_word offset, hyper_layout_word length)
{
 return cursor <= total && offset == cursor && length && !(length & 4095) &&
  !(alias & 4095) && alias <= ~(hyper_layout_word)0 - length && length <= total - cursor;
}
static inline int hyper_io_page_aperture(hyper_layout_word alias,
 hyper_layout_word start, hyper_layout_word end)
{
 const hyper_layout_word mask = (2 * 1024 * 1024) - 1;
 hyper_layout_word base = alias & ~mask;
 return !(alias & 4095) && base >= start && base <= end && end - base >= mask;
}
#endif
