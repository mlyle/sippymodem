// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PATH_STATE_FIXTURE_H
#define V90_PATH_STATE_FIXTURE_H

#include <v90_analog_path.h>
#include <v90_digital_path.h>
#include "v34_state_fixture.h"

/* Normalize snapshot copies only. Keep every named field's exact bytes and
 * compare retained buffers in full; C padding and retired union storage are
 * not modem state. Do not make undefined named fields appear initialized. */
static inline bool v90_snapshot_analog_path(struct v90_analog_path *path)
{
	struct v34_data_stream_preparation_job *job =
		&path->upstream_preparation;
	size_t upstream_size;

	switch (path->upstream_stage) {
	case V90_ANALOG_PATH_UPSTREAM_CP:
		qam_snapshot_channel(&path->upstream.cp.line.carrier.channel);
		upstream_size = sizeof(path->upstream.cp);
		break;
	case V90_ANALOG_PATH_UPSTREAM_E:
		qam_snapshot_channel(&path->upstream.e.line.carrier.channel);
		upstream_size = sizeof(path->upstream.e);
		break;
	case V90_ANALOG_PATH_UPSTREAM_DATA: {
		struct v34_data_stream *stream = &path->upstream.data;
		unsigned char bytes[sizeof(*stream)] = { 0 };

#define COPY(field) memcpy(bytes + offsetof(struct v34_data_stream, field), \
	&stream->field, sizeof(stream->field))
		COPY(points);
		COPY(line);
		COPY(delayed_primary_bits);
		COPY(primary_delay_bits);
		COPY(b1_active);
#undef COPY
		memcpy(stream, bytes, sizeof(bytes));
		v34_snapshot_points(&stream->points);
		qam_snapshot_channel(&stream->line.carrier.channel);
		upstream_size = sizeof(*stream);
		break;
	}
	default:
		/* These tests do not compare the RRN-return waveform. */
		return false;
	}
	memset((unsigned char *)&path->upstream + upstream_size, 0,
	       sizeof(path->upstream) - upstream_size);
	v34_snapshot_geometry(&job->preparation.mapping.schedule.geometry);
	v34_snapshot_parameters(&job->preparation.mapping.encoder.parameters);
	v34_snapshot_parameters(&job->preparation.mapping.normalizer.parameters);
	if (job->calibrating_line) {
		v34_snapshot_points(&job->calibration.line.points);
		qam_snapshot_channel(&job->calibration.line.carrier.channel);
	} else {
		v34_snapshot_geometry(
			&job->calibration.mapping.geometry);
		v34_snapshot_geometry(&job->calibration.mapping.schedule.geometry);
		v34_snapshot_parameters(
			&job->calibration.mapping.parameters);
		v34_snapshot_parameters(
			&job->calibration.mapping.encoder.parameters);
	}
	return true;
}

static inline bool v90_analog_path_states_equal(
	const struct v90_analog_path *left, const struct v90_analog_path *right)
{
	struct v90_analog_path a, b;

	memcpy(&a, left, sizeof(a));
	memcpy(&b, right, sizeof(b));
	return v90_snapshot_analog_path(&a) && v90_snapshot_analog_path(&b) &&
	       memcmp(&a, &b, sizeof(a)) == 0;
}

static inline bool v90_digital_path_states_equal(
	const struct v90_digital_path *left, const struct v90_digital_path *right)
{
	struct v90_digital_path snapshots[2];

	memcpy(&snapshots[0], left, sizeof(*left));
	memcpy(&snapshots[1], right, sizeof(*right));
	for (size_t i = 0; i < 2; ++i) {
		struct v90_digital_path *path = &snapshots[i];

		if (path->upstream_stage == V90_DIGITAL_PATH_UPSTREAM_STARTUP) {
			struct v90_upstream_rx *rx = &path->upstream.startup;

			v34_snapshot_geometry(&rx->data_mapping.schedule.geometry);
			v34_snapshot_parameters(&rx->data_mapping.encoder.parameters);
			v34_snapshot_parameters(&rx->data_mapping.normalizer.parameters);
			v34_snapshot_geometry(
				&rx->calibration.geometry);
			v34_snapshot_geometry(&rx->calibration.schedule.geometry);
			v34_snapshot_parameters(
				&rx->calibration.parameters);
			v34_snapshot_parameters(&rx->calibration.encoder.parameters);
			if (rx->stage == V90_UPSTREAM_RX_STAGE_B1 ||
			    rx->stage == V90_UPSTREAM_RX_STAGE_COMPLETE)
				v34_snapshot_data_receiver(&rx->state.data);
		} else if (path->upstream_stage == V90_DIGITAL_PATH_UPSTREAM_DATA) {
			v34_snapshot_data_receiver(&path->upstream.data);
			memset((unsigned char *)&path->upstream +
				       sizeof(path->upstream.data), 0,
			       sizeof(path->upstream) - sizeof(path->upstream.data));
		} else {
			return false;
		}
	}
	return memcmp(&snapshots[0], &snapshots[1], sizeof(*left)) == 0;
}

#endif
