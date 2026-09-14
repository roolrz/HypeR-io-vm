// SPDX-FileCopyrightText: 2026 roolrz
// SPDX-License-Identifier: Apache-2.0
#ifndef HYPER_IO_SESSION_H
#define HYPER_IO_SESSION_H
#include <stdint.h>
/* Native owner chooses binding identities. Retired epochs are never reusable. */
struct hyper_io_session { uint64_t binding, epoch; int retired; };
/* 1 starts a fresh session; 0 stays in the current session; -1 rejects it.
 * Caller invokes this only after validating framing and exact HELLO length. */
static inline int hyper_io_session_admit(struct hyper_io_session *s,
 uint64_t binding, uint64_t epoch, int hello, int idle)
{
 if (!binding || !epoch || epoch > UINT32_MAX) return -1;
 if (s->binding != binding) {
  if (!hello || !idle || epoch <= s->epoch) return -1;
  s->binding = binding; s->epoch = epoch; s->retired = 0; return 1;
 }
 if (epoch < s->epoch) return -1;
 return 0;
}
static inline int hyper_io_session_preparable(const struct hyper_io_session *s) { return s->binding && !s->retired; }
static inline void hyper_io_session_retire(struct hyper_io_session *s) { s->retired = 1; }
#endif
