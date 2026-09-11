// SPDX-License-Identifier: BSD-3-Clause

#include "v92_upstream_control.h"

#include "memory_region.h"

#include <string.h>

static bool silence_requested(const struct v92_upstream_control *control)
{
	return control->renegotiation &&
		(control->suvu.request_silence || control->peer_requests_silence);
}

bool v92_upstream_control_is_valid(const struct v92_upstream_control *control)
{
	if (control == NULL || !v92_suvu_parameters_are_valid(&control->suvu) ||
	    control->suvu.acknowledges_cpd || !v92_cpu_parameters_are_valid(&control->cpu) ||
	    control->cpu.acknowledges_cpd ||
	    control->cpu_sent != (control->completed_cpu_records != 0u) ||
	    control->retry < V92_UPSTREAM_RETRY_IDLE || control->retry > V92_UPSTREAM_RETRY_ACTIVE ||
	    (control->retry != V92_UPSTREAM_RETRY_IDLE &&
	     (!control->cpu_sent || control->peer_confirmation_received || control->cpu_pending)) ||
	    (!control->received_suvd && control->cpu_pending))
		return false;
	bool silence = silence_requested(control);
	if ((silence && (control->received_cpd || control->cpu_sent || control->cpu_pending ||
		control->retry != V92_UPSTREAM_RETRY_IDLE)) ||
	    (!(silence ? control->received_suvd : control->received_cpd) &&
	     (control->acknowledgment_sent || control->current_record_acknowledges)))
		return false;
	switch (control->record) {
	case V92_UPSTREAM_CONTROL_NONE:
		return control->record_complete && !control->cpu_sent &&
			!control->acknowledgment_sent && !control->current_record_acknowledges;
	case V92_UPSTREAM_CONTROL_CPU:
		return !silence && control->received_suvd && !control->cpu_pending;
	case V92_UPSTREAM_CONTROL_SUVU:
		return true;
	}
	return false;
}

bool v92_upstream_control_init(struct v92_upstream_control *control,
	const struct v92_suvu_parameters *suvu, const struct v92_cpu_parameters *cpu)
{
	if (control == NULL || suvu == NULL || cpu == NULL)
		return false;
	struct v92_upstream_control initialized = {
		.suvu = *suvu, .cpu = *cpu, .record_complete = true,
	};
	if (!v92_upstream_control_is_valid(&initialized))
		return false;
	*control = initialized;
	return true;
}

bool v92_upstream_control_can_finish(const struct v92_upstream_control *control)
{
	return v92_upstream_control_is_valid(control) &&
		(silence_requested(control) ? control->received_suvd : control->cpu_sent) &&
		control->acknowledgment_sent && control->peer_confirmation_received;
}

static bool accepts_events(const struct v92_upstream_control *control)
{
	return v92_upstream_control_is_valid(control) &&
		!(control->record_complete && v92_upstream_control_can_finish(control));
}

static void confirm(struct v92_upstream_control *control, bool ack)
{
	control->peer_confirmation_received |= ack;
	if ((control->cpu_sent || silence_requested(control)) && control->peer_confirmation_received) {
		control->cpu_pending = false;
		control->retry = V92_UPSTREAM_RETRY_IDLE;
	} else if (control->retry == V92_UPSTREAM_RETRY_ELIGIBLE) {
		/* Called only for complete validated CPd/SUVd, or the final Ed.
		 * A partial or rejected record never authorizes a retry. */
		control->retry = V92_UPSTREAM_RETRY_ACTIVE;
	}
}

bool v92_upstream_control_note_suvd(struct v92_upstream_control *control,
	const struct v92_suvd_parameters *suvd)
{
	if (!accepts_events(control) || suvd == NULL)
		return false;
	/* The receive owner supplies an already validated, CRC-checked record. */
	struct v92_suvd_parameters received = *suvd;
	/* A request belongs to the first status exchange, not a late change
	 * after CPu was committed. The resumed exchange starts with fresh flags. */
	if (control->renegotiation &&
	    ((control->received_suvd && received.request_silence != control->peer_requests_silence) ||
	     (received.request_silence && (control->received_cpd || control->cpu_sent ||
		control->record == V92_UPSTREAM_CONTROL_CPU))))
		return false;
	control->peer_requests_silence |= received.request_silence;
	if (!control->received_suvd && !control->cpu_sent &&
	    control->record != V92_UPSTREAM_CONTROL_CPU)
		control->cpu_pending = !silence_requested(control);
	control->received_suvd = true;
	confirm(control, received.acknowledges_cpu);
	return true;
}

bool v92_upstream_control_note_cpd(struct v92_upstream_control *control, bool ack)
{
	if (!accepts_events(control) || silence_requested(control))
		return false;
	control->received_cpd = true;
	confirm(control, ack);
	return true;
}

bool v92_upstream_control_note_ed(struct v92_upstream_control *control)
{
	if (!accepts_events(control))
		return false;
	confirm(control, true);
	return true;
}

bool v92_upstream_control_expire_cpu_wait(struct v92_upstream_control *control)
{
	if (!accepts_events(control) || !control->cpu_sent ||
	    control->peer_confirmation_received || control->cpu_pending ||
	    control->record != V92_UPSTREAM_CONTROL_SUVU || control->retry != V92_UPSTREAM_RETRY_IDLE)
		return false;
	control->retry = V92_UPSTREAM_RETRY_ELIGIBLE;
	return true;
}

bool v92_upstream_control_start(struct v92_upstream_control *control,
	unsigned int frame_bits, uint8_t *frame, size_t capacity, size_t *bit_count)
{
	uint8_t encoded[V92_CPU_MAX_FRAME_BYTES];
	size_t bits;
	if (!v92_upstream_control_is_valid(control) || !control->record_complete ||
	    v92_upstream_control_can_finish(control) || frame == NULL || bit_count == NULL ||
	    (control->renegotiation && frame_bits != 24u && frame_bits != 36u) ||
	    memory_regions_overlap(control, sizeof(*control), frame, capacity) ||
	    memory_regions_overlap(control, sizeof(*control), bit_count, sizeof(*bit_count)) ||
	    memory_regions_overlap(frame, capacity, bit_count, sizeof(*bit_count)))
		return false;
	bool cpu = control->record != V92_UPSTREAM_CONTROL_NONE &&
		(control->cpu_pending || control->retry == V92_UPSTREAM_RETRY_ACTIVE);
	bool ack = silence_requested(control) ?
		control->record != V92_UPSTREAM_CONTROL_NONE && control->received_suvd : control->received_cpd;
	if (cpu) {
		struct v92_cpu_parameters parameters = control->cpu;
		parameters.acknowledges_cpd = ack;
		if (v92_cpu_encode(&parameters, frame_bits, encoded, sizeof(encoded), &bits) != V92_CPU_OK)
			return false;
	} else {
		struct v92_suvu_parameters parameters = control->suvu;
		parameters.acknowledges_cpd = ack;
		if (v92_suvu_encode(&parameters, frame_bits, encoded, sizeof(encoded), &bits) != V92_SUVU_OK)
			return false;
	}
	size_t bytes = (bits + 7u) / 8u;
	if (capacity < bytes)
		return false;
	memcpy(frame, encoded, bytes);
	*bit_count = bits;
	control->record = cpu ? V92_UPSTREAM_CONTROL_CPU : V92_UPSTREAM_CONTROL_SUVU;
	control->record_complete = false;
	control->current_record_acknowledges = ack;
	if (cpu)
		control->cpu_pending = false;
	return true;
}

bool v92_upstream_control_complete(struct v92_upstream_control *control)
{
	if (!v92_upstream_control_is_valid(control) || control->record_complete ||
	    (control->record == V92_UPSTREAM_CONTROL_CPU && control->completed_cpu_records == UINT64_MAX))
		return false;
	if (control->record == V92_UPSTREAM_CONTROL_CPU) {
		control->completed_cpu_records++;
		control->cpu_sent = true;
	}
	control->acknowledgment_sent |= control->current_record_acknowledges;
	control->record_complete = true;
	return true;
}
