// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase4_line_receiver.h"

#include <math.h>
#include <stddef.h>

_Static_assert(
	V34_PHASE4_TIMING_REFINEMENT_SAMPLES <= UINT16_MAX,
	"Phase 4 timing-refinement count must fit its state field");

enum {
	/* Timing refinement corrects a sample-boundary ambiguity after the
	 * qualified S/S-bar marker.  Its search width is independent of the
	 * marker's companding-error budget and must remain bounded in the
	 * real-time media callback. */
	V34_PHASE4_MAX_TRN_ALIGNMENT_SYMBOLS = 2,
};

_Static_assert(
	(unsigned int)V34_PHASE4_MAX_TRN_ALIGNMENT_SYMBOLS <=
		(unsigned int)V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS,
	"TRN alignment search must fit the qualified marker uncertainty");

static enum v34_station_role transmitting_role(
	const struct v34_phase4_equalizer_receiver *control)
{
	return control->mp_receiver.transmitting_role;
}

bool v34_phase4_line_receiver_is_valid(
	const struct v34_phase4_line_receiver *receiver)
{
	return receiver != NULL &&
	       (receiver->timing_refining ?
		(receiver->timing_refinement_sample_count <
			 V34_PHASE4_TIMING_REFINEMENT_SAMPLES &&
		 !receiver->failed &&
		 receiver->control.stage ==
			 V34_PHASE4_EQUALIZER_RECEIVER_STAGE_TRN) :
		receiver->timing_refinement_sample_count == 0) &&
	       v34_phase3_profile_is_valid(
		       &receiver->transmitting_profile) &&
	       qam_demodulator_is_valid(&receiver->demodulator) &&
	       qam_demodulator_measure(&receiver->demodulator).timing_locked &&
	       v34_data_carrier_channel_matches(
		       &receiver->transmitting_profile,
		       &receiver->demodulator.channel) &&
	       v34_phase4_equalizer_receiver_is_valid(&receiver->control) &&
	       receiver->transmitting_profile.local_role ==
		       transmitting_role(&receiver->control);
}

enum v34_phase4_line_receiver_result
v34_phase4_line_receiver_init(
	struct v34_phase4_line_receiver *receiver,
	const struct qam_demodulator *demodulator,
	const struct qam_equalizer *equalizer,
	const struct v34_phase3_profile *transmitting_profile,
	enum v34_constellation_size constellation, double adaptation)
{
	struct v34_phase4_line_receiver initialized = { 0 };

	if (receiver == NULL ||
	    !v34_phase3_profile_is_valid(transmitting_profile) ||
	    !qam_demodulator_is_valid(demodulator) ||
	    !qam_demodulator_measure(demodulator).timing_locked ||
	    !v34_data_carrier_channel_matches(
		    transmitting_profile, &demodulator->channel) ||
	    v34_phase4_equalizer_receiver_init(
		    &initialized.control, equalizer,
		    transmitting_profile->local_role, constellation,
		    adaptation) != V34_PHASE4_EQUALIZER_RECEIVER_OK)
		return V34_PHASE4_LINE_RECEIVER_INVALID_ARGUMENT;
	initialized.demodulator = *demodulator;
	initialized.transmitting_profile = *transmitting_profile;
	if (!v34_phase4_line_receiver_is_valid(&initialized))
		return V34_PHASE4_LINE_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_PHASE4_LINE_RECEIVER_OK;
}

bool v34_phase4_line_receiver_has_failed(
	const struct v34_phase4_line_receiver *receiver)
{
	return v34_phase4_line_receiver_is_valid(receiver) &&
	       receiver->failed;
}

static enum v34_phase4_line_receiver_result control_result(
	enum v34_phase4_equalizer_receiver_result result)
{
	switch (result) {
	case V34_PHASE4_EQUALIZER_RECEIVER_OK:
	case V34_PHASE4_EQUALIZER_RECEIVER_WARMING:
		return V34_PHASE4_LINE_RECEIVER_OK;
	case V34_PHASE4_EQUALIZER_RECEIVER_SYMBOL:
		return V34_PHASE4_LINE_RECEIVER_SYMBOL;
	case V34_PHASE4_EQUALIZER_RECEIVER_S_DETECTED:
		return V34_PHASE4_LINE_RECEIVER_S_DETECTED;
	case V34_PHASE4_EQUALIZER_RECEIVER_RECORD_READY:
		return V34_PHASE4_LINE_RECEIVER_RECORD_READY;
	case V34_PHASE4_EQUALIZER_RECEIVER_E_COMPLETE:
		return V34_PHASE4_LINE_RECEIVER_E_COMPLETE;
	case V34_PHASE4_EQUALIZER_RECEIVER_COMPLETE:
		return V34_PHASE4_LINE_RECEIVER_COMPLETE;
	case V34_PHASE4_EQUALIZER_RECEIVER_INVALID_STATE:
		return V34_PHASE4_LINE_RECEIVER_INVALID_STATE;
	case V34_PHASE4_EQUALIZER_RECEIVER_NUMERIC_FAILURE:
		return V34_PHASE4_LINE_RECEIVER_NUMERIC_FAILURE;
	case V34_PHASE4_EQUALIZER_RECEIVER_SEQUENCE_MISMATCH:
		return V34_PHASE4_LINE_RECEIVER_SEQUENCE_MISMATCH;
	case V34_PHASE4_EQUALIZER_RECEIVER_INVALID_ARGUMENT:
		break;
	}
	return V34_PHASE4_LINE_RECEIVER_NUMERIC_FAILURE;
}

static bool is_terminal(enum v34_phase4_line_receiver_result result)
{
	return result == V34_PHASE4_LINE_RECEIVER_NUMERIC_FAILURE ||
	       result == V34_PHASE4_LINE_RECEIVER_SEQUENCE_MISMATCH;
}

static enum v34_phase4_line_receiver_result update_control(
	struct v34_phase4_line_receiver *receiver,
	enum v34_phase4_equalizer_receiver_result (*update)(
		struct v34_phase4_equalizer_receiver *))
{
	struct v34_phase4_line_receiver next;
	enum v34_phase4_line_receiver_result result;

	if (!v34_phase4_line_receiver_is_valid(receiver))
		return V34_PHASE4_LINE_RECEIVER_INVALID_ARGUMENT;
	if (receiver->failed)
		return V34_PHASE4_LINE_RECEIVER_FAILED;
	next = *receiver;
	result = control_result(update(&next.control));
	if (result != V34_PHASE4_LINE_RECEIVER_OK)
		return result;
	if (!v34_phase4_line_receiver_is_valid(&next))
		return V34_PHASE4_LINE_RECEIVER_INVALID_STATE;
	*receiver = next;
	return V34_PHASE4_LINE_RECEIVER_OK;
}

enum v34_phase4_line_receiver_result
v34_phase4_line_receiver_continue_mp(
	struct v34_phase4_line_receiver *receiver)
{
	return update_control(
		receiver, v34_phase4_equalizer_receiver_continue_mp);
}

enum v34_phase4_line_receiver_result
v34_phase4_line_receiver_start_e(
	struct v34_phase4_line_receiver *receiver)
{
	return update_control(
		receiver, v34_phase4_equalizer_receiver_start_e);
}

enum v34_phase4_line_receiver_result
v34_phase4_line_receiver_expect_e(
	struct v34_phase4_line_receiver *receiver)
{
	return update_control(
		receiver, v34_phase4_equalizer_receiver_expect_e);
}

static enum v34_phase4_line_receiver_result receive_sample_direct(
	struct v34_phase4_line_receiver *receiver, int16_t sample,
	struct v34_mp_receive_event *event)
{
	struct v34_phase4_line_receiver next;
	struct v34_mp_receive_event emitted = { 0 };
	struct qam_point point;
	enum v34_phase4_line_receiver_result result;
	bool ready;

	if (!v34_phase4_line_receiver_is_valid(receiver) || event == NULL)
		return V34_PHASE4_LINE_RECEIVER_INVALID_ARGUMENT;
	if (receiver->failed)
		return V34_PHASE4_LINE_RECEIVER_FAILED;
	if (receiver->control.record_pending)
		return V34_PHASE4_LINE_RECEIVER_INVALID_STATE;
	if (receiver->control.stage ==
	    V34_PHASE4_EQUALIZER_RECEIVER_STAGE_COMPLETE)
		return V34_PHASE4_LINE_RECEIVER_COMPLETE;
	next = *receiver;
	if (qam_demodulator_sample(
		    &next.demodulator, sample, &point, &ready) != QAM_OK) {
		receiver->failed = true;
		return V34_PHASE4_LINE_RECEIVER_NUMERIC_FAILURE;
	}
	if (!ready) {
		if (!v34_phase4_line_receiver_is_valid(&next)) {
			receiver->failed = true;
			return V34_PHASE4_LINE_RECEIVER_NUMERIC_FAILURE;
		}
		*receiver = next;
		return V34_PHASE4_LINE_RECEIVER_OK;
	}
	result = control_result(v34_phase4_equalizer_receive_point(
		&next.control, &point, &emitted));
	if (is_terminal(result)) {
		next.failed = true;
		if (!v34_phase4_line_receiver_is_valid(&next))
			receiver->failed = true;
		else
			*receiver = next;
		return result;
	}
	if (result == V34_PHASE4_LINE_RECEIVER_INVALID_STATE ||
	    !v34_phase4_line_receiver_is_valid(&next)) {
		receiver->failed = true;
		return V34_PHASE4_LINE_RECEIVER_NUMERIC_FAILURE;
	}
	*receiver = next;
	if (result == V34_PHASE4_LINE_RECEIVER_RECORD_READY)
		*event = emitted;
	return result;
}

static bool refinement_result_is_valid(
	enum v34_phase4_line_receiver_result result)
{
	return result == V34_PHASE4_LINE_RECEIVER_OK ||
	       result == V34_PHASE4_LINE_RECEIVER_SYMBOL;
}

static uint64_t phase_distance(
	uint64_t left, uint64_t right, uint64_t period)
{
	uint64_t forward = left >= right ? left - right : period - (right - left);
	uint64_t backward = right >= left ? right - left : period - (left - right);

	return forward < backward ? forward : backward;
}

static bool score_timing_phase(
	const struct v34_phase4_line_receiver *source, uint64_t phase,
	struct v34_phase4_line_receiver *candidate, double *average_error)
{
	struct v34_mp_receive_event ignored = { 0 };
	double initial_squared_error;
	uint16_t initial_error_symbols;
	uint16_t scored_symbols;

	*candidate = *source;
	initial_squared_error = candidate->control.trn_squared_error;
	initial_error_symbols = candidate->control.trn_error_symbols;
	candidate->timing_refining = false;
	candidate->timing_refinement_sample_count = 0;
	if (qam_demodulator_rebase_timing(
		    &candidate->demodulator,
		    phase % candidate->demodulator.symbol_period_units) != QAM_OK)
		return false;
	for (size_t index = 0;
	     index < source->timing_refinement_sample_count; ++index) {
		enum v34_phase4_line_receiver_result result =
			receive_sample_direct(
				candidate,
				source->timing_refinement_samples[index],
				&ignored);

		if (!refinement_result_is_valid(result))
			return false;
	}
	if (candidate->control.trn_error_symbols < initial_error_symbols ||
	    candidate->control.trn_squared_error < initial_squared_error)
		return false;
	scored_symbols = candidate->control.trn_error_symbols -
		initial_error_symbols;
	if (scored_symbols < V34_PHASE4_TIMING_REFINEMENT_MIN_SYMBOLS)
		return false;
	*average_error =
		(candidate->control.trn_squared_error - initial_squared_error) /
		scored_symbols;
	return isfinite(*average_error) &&
	       v34_phase4_line_receiver_is_valid(candidate);
}

static bool advance_trn_alignment(
	struct v34_phase4_line_receiver *receiver)
{
	struct qam_point ignored_point;
	uint8_t ignored_quarter_point;
	uint8_t ignored_quarter_turns;

	if (receiver->control.stage !=
		    V34_PHASE4_EQUALIZER_RECEIVER_STAGE_TRN ||
	    receiver->control.trn_symbols >= V34_PHASE4_TRN_MIN_SYMBOLS ||
	    v34_phase4_trn_next(
		    &receiver->control.expected_trn, &ignored_point,
		    &ignored_quarter_point, &ignored_quarter_turns) !=
		    V34_PHASE4_TRN_OK)
		return false;
	receiver->control.trn_symbols++;
	return v34_phase4_equalizer_receiver_is_valid(&receiver->control);
}

static enum v34_phase4_line_receiver_result refine_symbol_timing(
	struct v34_phase4_line_receiver *receiver)
{
	static const double REQUIRED_IMPROVEMENT = 0.98;
	struct v34_phase4_line_receiver reference;
	struct v34_phase4_line_receiver best;
	struct v34_phase4_line_receiver aligned;
	double reference_error;
	double best_error;
	uint64_t reference_phase = receiver->demodulator.locked_phase_units;
	uint64_t timing_reference_phase =
		receiver->demodulator.timing[
			receiver->demodulator.timing_reference_index].phase_units;
	uint64_t best_phase = reference_phase;
	uint64_t fine_step;
	unsigned int best_alignment = 0;
	bool gross_timing_error;

	if (!score_timing_phase(
		    receiver, reference_phase, &reference, &reference_error))
		return V34_PHASE4_LINE_RECEIVER_NUMERIC_FAILURE;
	best = reference;
	best_error = reference_error;
	/* The energy tracker remains the better clock when its recovered phase is
	 * already within one coarse bin. Searching every TRN phase in that case
	 * can overfit the four-point training sequence and damage dense B1 data.
	 * Use TRN to repair only a gross S/S-bar timing lock. */
	gross_timing_error = phase_distance(
		reference_phase, timing_reference_phase,
		receiver->demodulator.symbol_period_units) >
		receiver->demodulator.timing_step_units;
	aligned = *receiver;
	for (unsigned int alignment = 0;
	     alignment <= V34_PHASE4_MAX_TRN_ALIGNMENT_SYMBOLS;
	     ++alignment) {
		if (alignment != 0 && !advance_trn_alignment(&aligned))
			return V34_PHASE4_LINE_RECEIVER_NUMERIC_FAILURE;
		if (!gross_timing_error && alignment == 0)
			continue;
		for (size_t index = 0;
		     index < receiver->demodulator.timing_candidate_count;
		     ++index) {
			struct v34_phase4_line_receiver candidate;
			double error;
			uint64_t phase =
				index * receiver->demodulator.timing_step_units;

			if (score_timing_phase(
				    &aligned, phase, &candidate, &error) &&
			    error < best_error) {
				best = candidate;
				best_error = error;
				best_phase = phase;
				best_alignment = alignment;
			}
		}
	}
	/* Known TRN supplies an exact error metric. Refine one half of a coarse
	 * timing bin on either side of the best candidate. */
	fine_step = (gross_timing_error || best_alignment != 0) ?
		receiver->demodulator.timing_step_units / 8u : 0;
	if (fine_step != 0) {
		aligned = *receiver;
		for (unsigned int alignment = 0;
		     alignment < best_alignment; ++alignment)
			if (!advance_trn_alignment(&aligned))
				return V34_PHASE4_LINE_RECEIVER_NUMERIC_FAILURE;
		for (int step = -4; step <= 4; ++step) {
			struct v34_phase4_line_receiver candidate;
			double error;
			int64_t shifted = (int64_t)best_phase +
				(int64_t)step * (int64_t)fine_step;
			uint64_t phase;

			while (shifted < 0)
				shifted += (int64_t)receiver->demodulator
					.symbol_period_units;
			phase = (uint64_t)shifted %
				receiver->demodulator.symbol_period_units;
			if (score_timing_phase(
				    &aligned, phase, &candidate, &error) &&
			    error < best_error) {
				best = candidate;
				best_error = error;
			}
		}
	}
	if (best_error >= reference_error * REQUIRED_IMPROVEMENT)
		best = reference;
	if (!v34_phase4_line_receiver_is_valid(&best))
		return V34_PHASE4_LINE_RECEIVER_NUMERIC_FAILURE;
	*receiver = best;
	return V34_PHASE4_LINE_RECEIVER_SYMBOL;
}

enum v34_phase4_line_receiver_result
v34_phase4_line_receiver_sample(
	struct v34_phase4_line_receiver *receiver, int16_t sample,
	struct v34_mp_receive_event *event)
{
	struct v34_phase4_line_receiver next;
	enum v34_phase4_line_receiver_result result;

	if (!v34_phase4_line_receiver_is_valid(receiver) || event == NULL)
		return V34_PHASE4_LINE_RECEIVER_INVALID_ARGUMENT;
	if (!receiver->timing_refining) {
		result = receive_sample_direct(receiver, sample, event);
		if (result != V34_PHASE4_LINE_RECEIVER_S_DETECTED)
			return result;
		next = *receiver;
		next.timing_refining = true;
		if (!v34_phase4_line_receiver_is_valid(&next)) {
			receiver->failed = true;
			return V34_PHASE4_LINE_RECEIVER_NUMERIC_FAILURE;
		}
		*receiver = next;
		return result;
	}
	next = *receiver;
	next.timing_refinement_samples[
		next.timing_refinement_sample_count++] = sample;
	if (next.timing_refinement_sample_count <
	    V34_PHASE4_TIMING_REFINEMENT_SAMPLES) {
		if (!v34_phase4_line_receiver_is_valid(&next)) {
			receiver->failed = true;
			return V34_PHASE4_LINE_RECEIVER_NUMERIC_FAILURE;
		}
		*receiver = next;
		return V34_PHASE4_LINE_RECEIVER_OK;
	}
	result = refine_symbol_timing(&next);
	if (result != V34_PHASE4_LINE_RECEIVER_SYMBOL) {
		next.timing_refining = false;
		next.timing_refinement_sample_count = 0;
		next.failed = true;
		if (v34_phase4_line_receiver_is_valid(&next))
			*receiver = next;
		else
			receiver->failed = true;
		return result;
	}
	*receiver = next;
	return result;
}

enum v34_phase4_line_receiver_result
v34_phase4_line_receiver_start_data(
	const struct v34_phase4_line_receiver *receiver,
	struct v34_data_line_receiver *data_receiver,
	const struct v34_data_line_receiver_configuration *configuration)
{
	struct v34_equalizer_handoff handoff;

	if (!v34_phase4_line_receiver_is_valid(receiver) ||
	    data_receiver == NULL || configuration == NULL)
		return V34_PHASE4_LINE_RECEIVER_INVALID_ARGUMENT;
	if (receiver->failed)
		return V34_PHASE4_LINE_RECEIVER_FAILED;
	if (receiver->control.stage !=
	    V34_PHASE4_EQUALIZER_RECEIVER_STAGE_COMPLETE)
		return V34_PHASE4_LINE_RECEIVER_INVALID_STATE;
	if (v34_phase4_equalizer_receiver_handoff(
		    &receiver->control, &handoff) !=
		    V34_PHASE4_EQUALIZER_RECEIVER_OK)
		return V34_PHASE4_LINE_RECEIVER_INVALID_STATE;
	if (v34_data_line_receiver_init(
		    data_receiver, &receiver->demodulator, &handoff,
		    configuration) != V34_DATA_LINE_RECEIVER_OK)
		return V34_PHASE4_LINE_RECEIVER_INVALID_ARGUMENT;
	return V34_PHASE4_LINE_RECEIVER_OK;
}

const char *v34_phase4_line_receiver_result_name(
	enum v34_phase4_line_receiver_result result)
{
	switch (result) {
	case V34_PHASE4_LINE_RECEIVER_OK:
		return "ok";
	case V34_PHASE4_LINE_RECEIVER_SYMBOL:
		return "symbol";
	case V34_PHASE4_LINE_RECEIVER_S_DETECTED:
		return "S detected";
	case V34_PHASE4_LINE_RECEIVER_RECORD_READY:
		return "record ready";
	case V34_PHASE4_LINE_RECEIVER_E_COMPLETE:
		return "E complete";
	case V34_PHASE4_LINE_RECEIVER_COMPLETE:
		return "complete";
	case V34_PHASE4_LINE_RECEIVER_FAILED:
		return "failed";
	case V34_PHASE4_LINE_RECEIVER_INVALID_ARGUMENT:
		return "invalid argument";
	case V34_PHASE4_LINE_RECEIVER_INVALID_STATE:
		return "invalid state";
	case V34_PHASE4_LINE_RECEIVER_NUMERIC_FAILURE:
		return "numeric failure";
	case V34_PHASE4_LINE_RECEIVER_SEQUENCE_MISMATCH:
		return "sequence mismatch";
	}
	return "unknown";
}
