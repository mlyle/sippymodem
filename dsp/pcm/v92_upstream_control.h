// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_UPSTREAM_CONTROL_H
#define V92_UPSTREAM_CONTROL_H

#include "v92_cpu.h"
#include "v92_suvd.h"
#include "v92_suvu.h"

enum v92_upstream_control_record {
	V92_UPSTREAM_CONTROL_NONE,
	V92_UPSTREAM_CONTROL_SUVU,
	V92_UPSTREAM_CONTROL_CPU,
};

enum v92_upstream_retry {
	V92_UPSTREAM_RETRY_IDLE,
	V92_UPSTREAM_RETRY_ELIGIBLE,
	V92_UPSTREAM_RETRY_ACTIVE,
};

/* V.92 9.6.2 acknowledgment/retry policy, also used by 9.9 fast exchange.
 * This owner neither modulates nor clocks records. The caller starts a record
 * only at an aligned boundary, finishes its entire wire tail before complete(),
 * and owns the initial 100 ms + RTD wait. Expiry only enables a retry: a
 * complete subsequent CPd/SUVd decides whether repeated CPu is required.
 * Training uses 24/36-bit alignment;
 * fast exchange uses the preceding data frame width, not the newly offered CPd.
 * Each operation is serialized; rejected operations leave state/output intact. */
struct v92_upstream_control {
	struct v92_suvu_parameters suvu;
	struct v92_cpu_parameters cpu;
	uint64_t completed_cpu_records;
	enum v92_upstream_control_record record;
	enum v92_upstream_retry retry;
	bool record_complete;
	bool received_suvd;
	bool received_cpd;
	bool peer_confirmation_received;
	bool peer_requests_silence;
	bool cpu_pending;
	bool cpu_sent;
	bool acknowledgment_sent;
	bool current_record_acknowledges_cpd;
};

bool v92_upstream_control_init(struct v92_upstream_control *control,
	const struct v92_suvu_parameters *suvu, const struct v92_cpu_parameters *cpu);
bool v92_upstream_control_is_valid(const struct v92_upstream_control *control);
/* Mutual acknowledgment may precede the end of the current record. */
bool v92_upstream_control_can_finish(const struct v92_upstream_control *control);
bool v92_upstream_control_note_suvd(struct v92_upstream_control *control,
	const struct v92_suvd_parameters *suvd);
bool v92_upstream_control_note_cpd(struct v92_upstream_control *control, bool ack);
bool v92_upstream_control_note_ed(struct v92_upstream_control *control);
/* Notify expiry after the first complete CPu wire tail. Do not call for
 * subsequent repeats: once qualified by a non-ACK record, repetition stays
 * active until peer acknowledgement. Expiry alone queues no record. */
bool v92_upstream_control_expire_cpu_wait(struct v92_upstream_control *control);
/* Always start with SUVu, even if SUVd arrived before the first local record.
 * Records are packed MSB first, including CRC and alignment fill. */
bool v92_upstream_control_start(struct v92_upstream_control *control,
	unsigned int frame_bits, uint8_t *frame, size_t capacity, size_t *bit_count);
bool v92_upstream_control_complete(struct v92_upstream_control *control);

#endif
