// SPDX-FileCopyrightText: 2026 roolrz
// SPDX-License-Identifier: Apache-2.0
#ifndef HYPER_IO_SESSION_H
#define HYPER_IO_SESSION_H
#include <stdint.h>
/* Binding and transactions belong to the client; epochs belong to devices.
 * Network reset must not invalidate the unchanged storage queue generation. */
struct hyper_io_session {
	uint64_t binding, epoch[2];
	int retired;
};
static inline int hyper_io_session_admit(struct hyper_io_session *s,
					 uint64_t binding, uint64_t epoch,
					 int hello, int idle, unsigned device)
{
	if (!binding || !epoch || epoch > UINT32_MAX || device > 1)
		return -1;
	if (s->binding != binding) {
		if (!hello || !idle || binding <= s->binding)
			return -1;
		s->binding = binding;
		s->epoch[0] = s->epoch[1] = 0;
		s->epoch[device] = epoch;
		s->retired = 0;
		return 1;
	}
	if (epoch < s->epoch[device])
		return -1;
	return 0;
}
static inline int hyper_io_session_preparable(const struct hyper_io_session *s)
{
	return s->binding && !s->retired;
}
static inline void hyper_io_session_retire(struct hyper_io_session *s)
{
	s->retired = 1;
}
#endif
