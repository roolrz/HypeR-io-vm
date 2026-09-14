// SPDX-FileCopyrightText: 2026 roolrz
// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include "hyper_io_session.h"
#include "hyper_io_layout.h"
int main(void) {
 struct hyper_io_session s = {0};
 assert(hyper_io_session_admit(&s, 1, 1, 0, 1) == -1); /* must HELLO */
 assert(hyper_io_session_admit(&s, 1, 1, 1, 1) == 1);
 assert(hyper_io_session_admit(&s, 1, 1, 0, 0) == 0);
 assert(hyper_io_session_admit(&s, 2, 2, 1, 0) == -1); /* live memory */
 assert(hyper_io_session_admit(&s, 2, 1, 1, 1) == -1); /* reused epoch */
 assert(hyper_io_session_admit(&s, 2, 2, 0, 1) == -1); /* no HELLO */
 hyper_io_session_retire(&s);
 assert(!hyper_io_session_preparable(&s));
 assert(hyper_io_session_admit(&s, 1, 1, 0, 1) == 0); /* cached release may replay */
 assert(!hyper_io_session_preparable(&s));
 assert(hyper_io_session_admit(&s, 2, 2, 1, 1) == 1);
 assert(hyper_io_session_preparable(&s)); /* released slot */
 assert(hyper_io_session_admit(&s, 1, 1, 1, 1) == -1); /* stale owner */
 assert(hyper_io_session_admit(&s, 2, 1, 0, 1) == -1); /* stale queue */
 assert(hyper_io_session_admit(&s, 2, 2, 0, 1) == 0); /* replay candidate */
 assert(hyper_io_session_admit(&s, 0, 3, 1, 1) == -1);
 assert(hyper_io_session_admit(&s, 3, (uint64_t)UINT32_MAX + 1, 1, 1) == -1);
 assert(s.binding == 2 && s.epoch == 2);
 assert(hyper_io_extent_valid(0, 8192, 0x100000, 0, 4096));
 assert(hyper_io_extent_valid(4096, 8192, 0x900000, 4096, 4096));
 assert(!hyper_io_extent_valid(0, 8192, 0x100000, 4096, 4096)); /* gap */
 assert(!hyper_io_extent_valid(4096, 8192, 0x100000, 0, 4096)); /* overlap */
 assert(!hyper_io_extent_valid(0, 8192, UINT64_MAX - 4095, 0, 4096));
 assert(!hyper_io_extent_valid(8193, 8192, 0, 8193, 4096));
 assert(!hyper_io_extent_valid(0, 8192, 1, 0, 4096));
 assert(hyper_io_page_aperture(0x201000, 0x200000, 0x3fffff));
 assert(!hyper_io_page_aperture(0x401000, 0x200000, 0x3fffff)); /* subtraction underflow */
 assert(!hyper_io_page_aperture(0x201000, 0x201000, 0x3fffff)); /* partial metadata granule */
 assert(!hyper_io_page_aperture(0x201001, 0x200000, 0x3fffff));
 return 0;
}
