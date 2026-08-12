// SPDX-License-Identifier: BSD-3-Clause

#include "v34_caller_training.h"

#include <stddef.h>

static bool stage_is_valid(
	const struct v34_caller_training *training)
{
	bool phase3_complete =
		training->phase3.stage == V34_PHASE3_TRANSMITTER_COMPLETE;

	switch (training->stage) {
	case V34_CALLER_TRAINING_PHASE3:
		return !phase3_complete && training->phase4_trn_symbols == 0;
	case V34_CALLER_TRAINING_J_PRIME:
		return phase3_complete && training->phase4_trn_symbols == 0 &&
		       v34_phase4_j_prime_sequence_is_valid(
			       &training->phase4.j_prime) &&
		       !v34_phase4_j_prime_sequence_is_complete(
			       &training->phase4.j_prime);
	case V34_CALLER_TRAINING_PHASE4_TRN:
		return phase3_complete &&
		       v34_phase4_trn_is_valid(&training->phase4.trn) &&
		       training->phase4.trn.role == V34_CALLING_STATION &&
		       training->phase4.trn.constellation ==
			       training->phase3.remote_constellation &&
		       training->phase4.trn.has_last_symbol ==
			       (training->phase4_trn_symbols != 0);
	case V34_CALLER_TRAINING_PHASE4_MP:
		return phase3_complete &&
		       training->phase4_trn_symbols >=
			       V34_PHASE4_TRN_MIN_SYMBOLS &&
		       v34_phase4_mp_is_valid(&training->phase4.mp) &&
		       training->phase4.mp.role == V34_CALLING_STATION &&
		       training->phase4.mp.constellation ==
			       training->phase3.remote_constellation;
	case V34_CALLER_TRAINING_PHASE4_E:
		return phase3_complete &&
		       training->phase4_trn_symbols >=
			       V34_PHASE4_TRN_MIN_SYMBOLS &&
		       v34_phase4_e_sequence_is_valid(&training->phase4.e) &&
		       training->phase4.e.role == V34_CALLING_STATION &&
		       training->phase4.e.constellation ==
			       training->phase3.remote_constellation;
	}
	return false;
}

bool v34_caller_training_is_valid(
	const struct v34_caller_training *training)
{
	return training != NULL &&
	       v34_training_line_is_valid(&training->line) &&
	       training->line.profile.local_role == V34_CALLING_STATION &&
	       v34_phase3_transmitter_is_valid(&training->phase3) &&
	       stage_is_valid(training);
}

static enum v34_caller_training_result line_result(
	enum v34_training_line_result result)
{
	switch (result) {
	case V34_TRAINING_LINE_OK:
		return V34_CALLER_TRAINING_OK;
	case V34_TRAINING_LINE_OUT_OF_RANGE:
		return V34_CALLER_TRAINING_OUT_OF_RANGE;
	case V34_TRAINING_LINE_CLIPPING_RISK:
		return V34_CALLER_TRAINING_CLIPPING_RISK;
	case V34_TRAINING_LINE_NUMERIC_FAILURE:
		return V34_CALLER_TRAINING_NUMERIC_FAILURE;
	case V34_TRAINING_LINE_INVALID_ARGUMENT:
		break;
	}
	return V34_CALLER_TRAINING_INVALID_ARGUMENT;
}

enum v34_caller_training_result v34_caller_training_init(
	struct v34_caller_training *training,
	const struct v34_phase3_profile *profile,
	enum v34_constellation_size remote_constellation,
	uint32_t sample_rate, int16_t nominal_peak_amplitude)
{
	struct v34_caller_training initialized = { 0 };
	enum v34_training_line_result result;

	if (training == NULL || profile == NULL ||
	    profile->local_role != V34_CALLING_STATION)
		return V34_CALLER_TRAINING_INVALID_ARGUMENT;
	result = v34_training_line_init(
		&initialized.line, profile, sample_rate,
		nominal_peak_amplitude);
	if (result != V34_TRAINING_LINE_OK)
		return line_result(result);
	if (v34_phase3_transmitter_init(
		    &initialized.phase3, remote_constellation) !=
		    V34_PHASE3_TRANSMITTER_OK)
		return V34_CALLER_TRAINING_INVALID_ARGUMENT;
	initialized.stage = V34_CALLER_TRAINING_PHASE3;
	if (!v34_caller_training_is_valid(&initialized))
		return V34_CALLER_TRAINING_NUMERIC_FAILURE;
	*training = initialized;
	return V34_CALLER_TRAINING_OK;
}

enum v34_caller_training_result v34_caller_training_start_j(
	struct v34_caller_training *training)
{
	struct v34_caller_training next;
	enum v34_phase3_transmitter_result result;

	if (!v34_caller_training_is_valid(training))
		return V34_CALLER_TRAINING_INVALID_ARGUMENT;
	if (training->stage != V34_CALLER_TRAINING_PHASE3)
		return V34_CALLER_TRAINING_INVALID_STATE;
	next = *training;
	result = v34_phase3_transmitter_start_j(&next.phase3);
	if (result == V34_PHASE3_TRANSMITTER_INVALID_STATE)
		return V34_CALLER_TRAINING_INVALID_STATE;
	if (result != V34_PHASE3_TRANSMITTER_OK ||
	    !v34_caller_training_is_valid(&next))
		return V34_CALLER_TRAINING_NUMERIC_FAILURE;
	*training = next;
	return V34_CALLER_TRAINING_OK;
}

enum v34_caller_training_result v34_caller_training_finish_j(
	struct v34_caller_training *training)
{
	struct v34_caller_training next;
	struct v34_phase4_j_prime_sequence j_prime;
	enum v34_phase3_transmitter_result result;

	if (!v34_caller_training_is_valid(training))
		return V34_CALLER_TRAINING_INVALID_ARGUMENT;
	if (training->stage != V34_CALLER_TRAINING_PHASE3)
		return V34_CALLER_TRAINING_INVALID_STATE;
	next = *training;
	result = v34_phase3_transmitter_finish_j(&next.phase3);
	if (result == V34_PHASE3_TRANSMITTER_INVALID_STATE)
		return V34_CALLER_TRAINING_INVALID_STATE;
	if (result != V34_PHASE3_TRANSMITTER_OK ||
	    v34_phase3_transmitter_start_j_prime(
		    &next.phase3, &j_prime) != V34_PHASE3_TRANSMITTER_OK)
		return V34_CALLER_TRAINING_NUMERIC_FAILURE;
	next.phase4 = (union v34_caller_phase4_state){ 0 };
	next.phase4.j_prime = j_prime;
	next.stage = V34_CALLER_TRAINING_J_PRIME;
	if (!v34_caller_training_is_valid(&next))
		return V34_CALLER_TRAINING_NUMERIC_FAILURE;
	*training = next;
	return V34_CALLER_TRAINING_OK;
}

static enum v34_caller_training_result mp_result(
	enum v34_phase4_mp_result result)
{
	switch (result) {
	case V34_PHASE4_MP_OK:
		return V34_CALLER_TRAINING_OK;
	case V34_PHASE4_MP_RECORD_COMPLETE:
	case V34_PHASE4_MP_RECORD_IN_PROGRESS:
		return V34_CALLER_TRAINING_INVALID_STATE;
	case V34_PHASE4_MP_INVALID_PARAMETERS:
		return V34_CALLER_TRAINING_INVALID_PARAMETERS;
	case V34_PHASE4_MP_INVALID_ARGUMENT:
		break;
	}
	return V34_CALLER_TRAINING_NUMERIC_FAILURE;
}

static enum v34_caller_training_result start_mp(
	struct v34_caller_training *training,
	const struct v34_mp_parameters *type0,
	const struct v34_mp_type1_parameters *type1)
{
	struct v34_caller_training next;
	enum v34_phase4_mp_result result;

	if (!v34_caller_training_is_valid(training) ||
	    (type0 == NULL) == (type1 == NULL))
		return V34_CALLER_TRAINING_INVALID_ARGUMENT;
	next = *training;
	if (next.stage == V34_CALLER_TRAINING_PHASE4_TRN) {
		struct v34_phase4_mp mp;

		if (!v34_caller_training_phase4_trn_ready(&next))
			return V34_CALLER_TRAINING_INVALID_STATE;
		result = type0 != NULL ?
			v34_phase4_mp_init_type0_from_trn(
				&mp, &next.phase4.trn, type0) :
			v34_phase4_mp_init_type1_from_trn(
				&mp, &next.phase4.trn, type1);
		if (result != V34_PHASE4_MP_OK)
			return mp_result(result);
		next.phase4 = (union v34_caller_phase4_state){ 0 };
		next.phase4.mp = mp;
		next.stage = V34_CALLER_TRAINING_PHASE4_MP;
	} else if (next.stage == V34_CALLER_TRAINING_PHASE4_MP) {
		result = type0 != NULL ?
			v34_phase4_mp_start_next_type0_record(
				&next.phase4.mp, type0) :
			v34_phase4_mp_start_next_type1_record(
				&next.phase4.mp, type1);
		if (result != V34_PHASE4_MP_OK)
			return mp_result(result);
	} else {
		return V34_CALLER_TRAINING_INVALID_STATE;
	}
	if (!v34_caller_training_is_valid(&next))
		return V34_CALLER_TRAINING_NUMERIC_FAILURE;
	*training = next;
	return V34_CALLER_TRAINING_OK;
}

enum v34_caller_training_result v34_caller_training_start_mp_type0(
	struct v34_caller_training *training,
	const struct v34_mp_parameters *parameters)
{
	return start_mp(training, parameters, NULL);
}

enum v34_caller_training_result v34_caller_training_start_mp_type1(
	struct v34_caller_training *training,
	const struct v34_mp_type1_parameters *parameters)
{
	return start_mp(training, NULL, parameters);
}

enum v34_caller_training_result v34_caller_training_start_e(
	struct v34_caller_training *training)
{
	struct v34_caller_training next;
	struct v34_phase4_e_sequence e;

	if (!v34_caller_training_is_valid(training))
		return V34_CALLER_TRAINING_INVALID_ARGUMENT;
	if (!v34_caller_training_phase4_mp_record_complete(training))
		return V34_CALLER_TRAINING_INVALID_STATE;
	next = *training;
	if (v34_phase4_e_sequence_init_from_mp(
		    &e, &next.phase4.mp) != V34_PHASE4_E_SEQUENCE_OK)
		return V34_CALLER_TRAINING_NUMERIC_FAILURE;
	next.phase4 = (union v34_caller_phase4_state){ 0 };
	next.phase4.e = e;
	next.stage = V34_CALLER_TRAINING_PHASE4_E;
	if (!v34_caller_training_is_valid(&next))
		return V34_CALLER_TRAINING_NUMERIC_FAILURE;
	*training = next;
	return V34_CALLER_TRAINING_OK;
}

static enum v34_caller_training_result next_phase3_point(
	struct v34_caller_training *training, struct qam_point *point)
{
	return v34_phase3_transmitter_next(&training->phase3, point) ==
		       V34_PHASE3_TRANSMITTER_OK ?
		       V34_CALLER_TRAINING_OK :
		       V34_CALLER_TRAINING_NUMERIC_FAILURE;
}

static enum v34_caller_training_result next_j_prime_point(
	struct v34_caller_training *training, struct qam_point *point)
{
	struct v34_phase4_trn trn;
	uint8_t rotation;

	if (v34_phase4_j_prime_sequence_next(
		    &training->phase4.j_prime, point, &rotation) !=
	    V34_PHASE4_J_PRIME_SEQUENCE_OK)
		return V34_CALLER_TRAINING_NUMERIC_FAILURE;
	if (!v34_phase4_j_prime_sequence_is_complete(
		    &training->phase4.j_prime))
		return V34_CALLER_TRAINING_OK;
	if (v34_phase4_trn_init(
		    &trn, V34_CALLING_STATION,
		    training->phase3.remote_constellation) != V34_PHASE4_TRN_OK)
		return V34_CALLER_TRAINING_NUMERIC_FAILURE;
	training->phase4 = (union v34_caller_phase4_state){ 0 };
	training->phase4.trn = trn;
	training->stage = V34_CALLER_TRAINING_PHASE4_TRN;
	return V34_CALLER_TRAINING_OK;
}

static enum v34_caller_training_result next_phase4_trn_point(
	struct v34_caller_training *training, struct qam_point *point)
{
	uint8_t quarter_point;
	uint8_t rotation;

	if (training->phase4_trn_symbols == UINT32_MAX)
		return V34_CALLER_TRAINING_OUT_OF_RANGE;
	if (v34_phase4_trn_next(
		    &training->phase4.trn, point, &quarter_point, &rotation) !=
	    V34_PHASE4_TRN_OK)
		return V34_CALLER_TRAINING_NUMERIC_FAILURE;
	training->phase4_trn_symbols++;
	return V34_CALLER_TRAINING_OK;
}

static enum v34_caller_training_result next_mp_point(
	struct v34_caller_training *training, struct qam_point *point)
{
	uint8_t quarter_point;
	uint8_t rotation;
	bool complete;
	enum v34_phase4_mp_result result = v34_phase4_mp_next(
		&training->phase4.mp, point, &quarter_point, &rotation,
		&complete);

	if (result == V34_PHASE4_MP_RECORD_COMPLETE)
		return V34_CALLER_TRAINING_INVALID_STATE;
	return result == V34_PHASE4_MP_OK ?
		       V34_CALLER_TRAINING_OK :
		       V34_CALLER_TRAINING_NUMERIC_FAILURE;
}

static enum v34_caller_training_result next_e_point(
	struct v34_caller_training *training, struct qam_point *point)
{
	uint8_t quarter_point;
	uint8_t rotation;
	enum v34_phase4_e_sequence_result result =
		v34_phase4_e_sequence_next(
			&training->phase4.e, point, &quarter_point, &rotation);

	if (result == V34_PHASE4_E_SEQUENCE_COMPLETE)
		return V34_CALLER_TRAINING_INVALID_STATE;
	return result == V34_PHASE4_E_SEQUENCE_OK ?
		       V34_CALLER_TRAINING_OK :
		       V34_CALLER_TRAINING_NUMERIC_FAILURE;
}

static enum v34_caller_training_result next_point(
	struct v34_caller_training *training, struct qam_point *point)
{
	switch (training->stage) {
	case V34_CALLER_TRAINING_PHASE3:
		return next_phase3_point(training, point);
	case V34_CALLER_TRAINING_J_PRIME:
		return next_j_prime_point(training, point);
	case V34_CALLER_TRAINING_PHASE4_TRN:
		return next_phase4_trn_point(training, point);
	case V34_CALLER_TRAINING_PHASE4_MP:
		return next_mp_point(training, point);
	case V34_CALLER_TRAINING_PHASE4_E:
		return next_e_point(training, point);
	}
	return V34_CALLER_TRAINING_NUMERIC_FAILURE;
}

enum v34_caller_training_result v34_caller_training_sample(
	struct v34_caller_training *training, int16_t *sample)
{
	struct v34_caller_training next;
	struct qam_point point;
	const struct qam_point *point_pointer = NULL;
	int16_t next_sample;
	bool accepting;
	bool accepted;
	enum v34_caller_training_result result;
	enum v34_training_line_result waveform_result;

	if (!v34_caller_training_is_valid(training) || sample == NULL)
		return V34_CALLER_TRAINING_INVALID_ARGUMENT;
	next = *training;
	accepting = v34_training_line_needs_point(&next.line);
	if (accepting) {
		point_pointer = &point;
		result = next_point(&next, &point);
		if (result != V34_CALLER_TRAINING_OK)
			return result;
	}
	waveform_result = v34_training_line_sample(
		&next.line, point_pointer, &next_sample, &accepted);
	if (waveform_result != V34_TRAINING_LINE_OK)
		return line_result(waveform_result);
	if (accepted != accepting || !v34_caller_training_is_valid(&next))
		return V34_CALLER_TRAINING_NUMERIC_FAILURE;
	*training = next;
	*sample = next_sample;
	return V34_CALLER_TRAINING_OK;
}

bool v34_caller_training_phase4_trn_ready(
	const struct v34_caller_training *training)
{
	return v34_caller_training_is_valid(training) &&
	       training->stage == V34_CALLER_TRAINING_PHASE4_TRN &&
	       training->phase4_trn_symbols >= V34_PHASE4_TRN_MIN_SYMBOLS;
}

bool v34_caller_training_phase4_mp_record_complete(
	const struct v34_caller_training *training)
{
	return v34_caller_training_is_valid(training) &&
	       training->stage == V34_CALLER_TRAINING_PHASE4_MP &&
	       v34_phase4_mp_record_is_complete(&training->phase4.mp);
}

bool v34_caller_training_phase4_e_complete(
	const struct v34_caller_training *training)
{
	return v34_caller_training_is_valid(training) &&
	       training->stage == V34_CALLER_TRAINING_PHASE4_E &&
	       v34_phase4_e_sequence_is_complete(&training->phase4.e);
}

const char *v34_caller_training_result_name(
	enum v34_caller_training_result result)
{
	switch (result) {
	case V34_CALLER_TRAINING_OK:
		return "ok";
	case V34_CALLER_TRAINING_INVALID_ARGUMENT:
		return "invalid argument";
	case V34_CALLER_TRAINING_INVALID_STATE:
		return "invalid state";
	case V34_CALLER_TRAINING_INVALID_PARAMETERS:
		return "invalid parameters";
	case V34_CALLER_TRAINING_OUT_OF_RANGE:
		return "out of range";
	case V34_CALLER_TRAINING_CLIPPING_RISK:
		return "clipping risk";
	case V34_CALLER_TRAINING_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
