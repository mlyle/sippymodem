// SPDX-License-Identifier: BSD-3-Clause

#include "v92_upstream_control.h"

#include "memory_region.h"

#include <string.h>

bool v92_upstream_control_is_valid(const struct v92_upstream_control *control)
{
	if (control == NULL || !v92_suvu_parameters_are_valid(&control->suvu) ||
	    control->suvu.acknowledges_cpd || !v92_cpu_parameters_are_valid(&control->cpu) ||
	    control->cpu.acknowledges_cpd ||
	    control->cpu_sent != (control->completed_cpu_records != 0u) ||
	    (!control->received_suvd && control->cpu_pending) ||
	    (!control->received_cpd && (control->acknowledgment_sent ||
		control->current_record_acknowledges_cpd)))
		return false;
	switch (control->record) {
	case V92_UPSTREAM_CONTROL_NONE:
		return control->record_complete && !control->cpu_sent &&
			!control->acknowledgment_sent && !control->current_record_acknowledges_cpd;
	case V92_UPSTREAM_CONTROL_CPU:
		return control->received_suvd && !control->cpu_pending;
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
	return v92_upstream_control_is_valid(control) && control->cpu_sent &&
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
	if (control->cpu_sent && control->peer_confirmation_received)
		control->cpu_pending = false;
}

bool v92_upstream_control_note_suvd(struct v92_upstream_control *control,
	const struct v92_suvd_parameters *suvd)
{
	if (!accepts_events(control) || suvd == NULL)
		return false;
	/* The receive owner supplies an already validated, CRC-checked record. */
	struct v92_suvd_parameters received = *suvd;
	if (!control->received_suvd && !control->cpu_sent &&
	    control->record != V92_UPSTREAM_CONTROL_CPU)
		control->cpu_pending = true;
	control->received_suvd = true;
	confirm(control, received.acknowledges_cpu);
	control->peer_requests_silence |= received.request_silence;
	return true;
}

bool v92_upstream_control_note_cpd(struct v92_upstream_control *control, bool ack)
{
	if (!accepts_events(control))
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

bool v92_upstream_control_repeat_cpu(struct v92_upstream_control *control)
{
	if (!accepts_events(control) || !control->cpu_sent ||
	    control->peer_confirmation_received || control->cpu_pending ||
	    control->record != V92_UPSTREAM_CONTROL_SUVU)
		return false;
	control->cpu_pending = true;
	return true;
}

bool v92_upstream_control_start(struct v92_upstream_control *control,
	unsigned int frame_bits, uint8_t *frame, size_t capacity, size_t *bit_count)
{
	uint8_t encoded[V92_CPU_MAX_FRAME_BYTES];
	size_t bits;
	if (!v92_upstream_control_is_valid(control) || !control->record_complete ||
	    v92_upstream_control_can_finish(control) || frame == NULL || bit_count == NULL ||
	    memory_regions_overlap(control, sizeof(*control), frame, capacity) ||
	    memory_regions_overlap(control, sizeof(*control), bit_count, sizeof(*bit_count)) ||
	    memory_regions_overlap(frame, capacity, bit_count, sizeof(*bit_count)))
		return false;
	bool cpu = control->record != V92_UPSTREAM_CONTROL_NONE && control->cpu_pending;
	if (cpu) {
		struct v92_cpu_parameters parameters = control->cpu;
		parameters.acknowledges_cpd = control->received_cpd;
		if (v92_cpu_encode(&parameters, frame_bits, encoded, sizeof(encoded), &bits) != V92_CPU_OK)
			return false;
	} else {
		struct v92_suvu_parameters parameters = control->suvu;
		parameters.acknowledges_cpd = control->received_cpd;
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
	control->current_record_acknowledges_cpd = control->received_cpd;
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
	control->acknowledgment_sent |= control->current_record_acknowledges_cpd;
	control->record_complete = true;
	return true;
}
