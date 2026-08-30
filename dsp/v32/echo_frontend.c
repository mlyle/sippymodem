// SPDX-License-Identifier: BSD-3-Clause

#include "echo_frontend.h"

#include "memory_region.h"
#include "phase.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

/* Fixed 9-phase, 40-tap Q15 voice-band profile for 8 kHz to 7.2 kHz. */
static const int16_t line_filter_coefficients[
	V32_ECHO_FRONTEND_FILTER_PHASES *
	V32_ECHO_FRONTEND_FILTER_TAPS] = {
	84, 85, 83, 76, 66, 54, 40, 26, 13, 4,
	0, 0, 7, 21, 41, 67, 96, 127, 157, 185,
	207, 221, 227, 223, 210, 187, 157, 121, 84, 48,
	17, -4, -16, -15, 0, 27, 68, 118, 175, 233,
	287, 334, 368, 386, 386, 366, 326, 270, 200, 123,
	43, -30, -94, -141, -166, -167, -142, -93, -23, 62,
	156, 251, 337, 407, 452, 467, 448, 394, 308, 194,
	59, -87, -233, -369, -483, -565, -610, -611, -569,
	-487, -371, -231, -80, 68, 200, 300, 358, 364, 313,
	206, 46, -157, -391, -640, -884, -1107, -1289, -1417,
	-1480, -1472, -1393, -1248, -1052, -819, -571, -332, -124,
	29, 110, 107, 11, -175, -445, -783, -1165, -1565,
	-1953, -2299, -2575, -2758, -2831, -2785, -2623, -2355,
	-2002, -1593, -1163, -751, -395, -132, 6, 1, -159,
	-473, -927, -1491, -2129, -2794, -3435, -3999, -4438,
	-4710, -4784, -4645, -4294, -3749, -3046, -2236, -1382,
	-556, 169, 722, 1039, 1074, 798, 203, -691, -1840,
	-3175, -4607, -6029, -7325, -8376, -9068, -9300, -8992,
	-8089, -6571, -4451, -1779, 1360, 4853, 8555, 12309,
	15944, 19291, 22189, 24499, 26105, 26929, 26929, 26105,
	24499, 22189, 19291, 15944, 12309, 8555, 4853, 1360,
	-1779, -4451, -6571, -8089, -8992, -9300, -9068, -8376,
	-7325, -6029, -4607, -3175, -1840, -691, 203, 798,
	1074, 1039, 722, 169, -556, -1382, -2236, -3046, -3749,
	-4294, -4645, -4784, -4710, -4438, -3999, -3435, -2794,
	-2129, -1491, -927, -473, -159, 1, 6, -132, -395,
	-751, -1163, -1593, -2002, -2355, -2623, -2785, -2831,
	-2758, -2575, -2299, -1953, -1565, -1165, -783, -445,
	-175, 11, 107, 110, 29, -124, -332, -571, -819,
	-1052, -1248, -1393, -1472, -1480, -1417, -1289, -1107,
	-884, -640, -391, -157, 46, 206, 313, 364, 358,
	300, 200, 68, -80, -231, -371, -487, -569, -611,
	-610, -565, -483, -369, -233, -87, 59, 194, 308,
	394, 448, 467, 452, 407, 337, 251, 156, 62,
	-23, -93, -142, -167, -166, -141, -94, -30, 43,
	123, 200, 270, 326, 366, 386, 386, 368, 334,
	287, 233, 175, 118, 68, 27, 0, -15, -16,
	-4, 17, 48, 84, 121, 157, 187, 210, 223,
	227, 221, 207, 185, 157, 127, 96, 67, 41,
	21, 7, 0, 0, 4, 13, 26, 40, 54,
	66, 76, 8, 85, 84,
};

_Static_assert(
	sizeof(line_filter_coefficients) /
		sizeof(line_filter_coefficients[0]) ==
		V32_ECHO_FRONTEND_FILTER_PHASES *
		V32_ECHO_FRONTEND_FILTER_TAPS,
	"V.32 line filter has the wrong shape");

static bool echo_header_is_valid(
	const struct v32_echo_canceller *echo)
{
	size_t oldest;

	if (echo == NULL ||
	    echo->near_delay_symbols > V32_ECHO_MAX_NEAR_DELAY_SYMBOLS ||
	    echo->round_trip_symbols < V32_ECHO_MIN_ROUND_TRIP_SYMBOLS ||
	    echo->round_trip_symbols > V32_ECHO_MAX_ROUND_TRIP_SYMBOLS)
		return false;
	oldest = (size_t)echo->near_delay_symbols +
		 (size_t)echo->round_trip_symbols +
		 V32_ECHO_WINDOW_SYMBOLS - 1u;
	return oldest < V32_ECHO_REFERENCE_CAPACITY &&
	       echo->reference_write < V32_ECHO_REFERENCE_CAPACITY &&
	       echo->reference_fill <= V32_ECHO_REFERENCE_CAPACITY &&
	       (echo->reference_fill == V32_ECHO_REFERENCE_CAPACITY ||
		echo->reference_write == echo->reference_fill) &&
	       echo->phase < V32_ECHO_PHASE_COUNT &&
	       qam_adaptation_is_valid(echo->adaptation) &&
	       (!echo->adapting || echo->adaptation > 0.0);
}

static bool frontend_header_is_valid(
	const struct v32_echo_frontend *frontend)
{
	const struct v32_echo_frontend_filter *filter;
	const struct v32_echo_canceller *echo;

	if (frontend == NULL)
		return false;
	filter = &frontend->filter;
	echo = &frontend->echo;
	return echo_header_is_valid(echo) &&
	       filter->history_write < V32_ECHO_FRONTEND_FILTER_TAPS &&
	       filter->phase < V32_ECHO_FRONTEND_FILTER_PHASES &&
	       filter->inputs_needed >= 1 && filter->inputs_needed <= 2 &&
	       (filter->phase == 0 || filter->inputs_needed == 1) &&
	       echo->phase == filter->phase % V32_ECHO_PHASE_COUNT &&
	       echo->reference_pending == (echo->phase != 0);
}

bool v32_echo_frontend_is_valid(
	const struct v32_echo_frontend *frontend)
{
	return frontend_header_is_valid(frontend) &&
	       v32_echo_canceller_is_valid(&frontend->echo);
}

enum v32_echo_result v32_echo_reference_init(
	struct v32_echo_reference *reference,
	const struct qam_point *mapped, uint32_t carrier_phase)
{
	struct v32_echo_reference initialized;
	double radians;
	double cosine;
	double power;
	double sine;

	if (reference == NULL || !point_is_finite(mapped))
		return V32_ECHO_INVALID_ARGUMENT;
	radians = phase_radians_u32(carrier_phase);
	cosine = cos(radians);
	sine = sin(radians);
	initialized.point.in_phase =
		mapped->in_phase * cosine - mapped->quadrature * sine;
	initialized.point.quadrature =
		mapped->in_phase * sine + mapped->quadrature * cosine;
	power = initialized.point.in_phase * initialized.point.in_phase +
		initialized.point.quadrature * initialized.point.quadrature;
	if (!point_is_finite(&initialized.point) || !isfinite(power))
		return V32_ECHO_INVALID_ARGUMENT;
	*reference = initialized;
	return V32_ECHO_OK;
}

enum v32_echo_result v32_echo_frontend_init(
	struct v32_echo_frontend *frontend,
	uint16_t near_delay_symbols, uint16_t round_trip_symbols)
{
	struct v32_echo_frontend initialized;
	enum v32_echo_result result;

	if (frontend == NULL)
		return V32_ECHO_INVALID_ARGUMENT;
	memset(&initialized, 0, sizeof(initialized));
	result = v32_echo_canceller_init(
		&initialized.echo, near_delay_symbols, round_trip_symbols);
	if (result != V32_ECHO_OK)
		return result;
	initialized.filter.phase = V32_ECHO_FRONTEND_INITIAL_PHASE;
	initialized.filter.inputs_needed = 1;
	if (!v32_echo_frontend_is_valid(&initialized))
		return V32_ECHO_INVALID_STATE;
	*frontend = initialized;
	return V32_ECHO_OK;
}

enum v32_echo_result v32_echo_frontend_start_adaptation(
	struct v32_echo_frontend *frontend, double adaptation)
{
	if (!v32_echo_frontend_is_valid(frontend))
		return V32_ECHO_INVALID_STATE;
	return v32_echo_canceller_start_adaptation(
		&frontend->echo, adaptation);
}

enum v32_echo_result v32_echo_frontend_freeze(
	struct v32_echo_frontend *frontend)
{
	if (!v32_echo_frontend_is_valid(frontend))
		return V32_ECHO_INVALID_STATE;
	return v32_echo_canceller_freeze(&frontend->echo);
}

static int16_t arithmetic_shift_q15(uint32_t bits)
{
	int64_t value = bits <= INT32_MAX ?
			(int64_t)bits :
			(int64_t)bits - (INT64_C(1) << 32);
	int64_t shifted = value >= 0 ?
			  value / (INT64_C(1) << 15) :
			  -((-value + (INT64_C(1) << 15) - 1) /
			    (INT64_C(1) << 15));
	uint16_t low = (uint16_t)(uint64_t)shifted;

	return low <= INT16_MAX ?
		       (int16_t)low : (int16_t)((int32_t)low - 65536);
}

static int16_t filter_output(
	const struct v32_echo_frontend_filter *filter)
{
	uint32_t accumulator = 0;
	size_t history = filter->history_write;

	for (size_t tap = 0; tap < V32_ECHO_FRONTEND_FILTER_TAPS; ++tap) {
		int32_t product =
			(int32_t)filter->history[history] *
			(int32_t)line_filter_coefficients[
				filter->phase +
				tap * V32_ECHO_FRONTEND_FILTER_PHASES];

		accumulator += (uint32_t)product;
		history = history == 0 ?
				  V32_ECHO_FRONTEND_FILTER_TAPS - 1u :
				  history - 1u;
	}
	return arithmetic_shift_q15(accumulator);
}

static bool filter_sample(
	struct v32_echo_frontend_filter *filter, int16_t received,
	int16_t *filtered)
{
	unsigned int next_phase;

	filter->history_write++;
	if (filter->history_write == V32_ECHO_FRONTEND_FILTER_TAPS)
		filter->history_write = 0;
	filter->history[filter->history_write] = received;
	filter->inputs_needed--;
	if (filter->inputs_needed != 0)
		return false;
	*filtered = filter_output(filter);
	next_phase = filter->phase + V32_ECHO_FRONTEND_FILTER_STEP;
	filter->inputs_needed =
		(uint8_t)(next_phase / V32_ECHO_FRONTEND_FILTER_PHASES);
	filter->phase =
		(uint8_t)(next_phase % V32_ECHO_FRONTEND_FILTER_PHASES);
	return true;
}

static void restore_reference_push(
	struct v32_echo_canceller *echo, size_t write, size_t fill,
	const struct qam_point *overwritten)
{
	echo->reference[write] = *overwritten;
	echo->reference_write = write;
	echo->reference_fill = fill;
	echo->reference_pending = false;
}

enum v32_echo_result v32_echo_frontend_sample(
	struct v32_echo_frontend *frontend, int16_t received,
	const struct v32_echo_reference *accepted_reference,
	double *cleaned, bool *cleaned_ready)
{
	struct v32_echo_frontend_filter next_filter;
	struct qam_point overwritten = { 0 };
	double next_cleaned;
	int16_t filtered = 0;
	size_t saved_write = 0;
	size_t saved_fill = 0;
	bool produces;
	bool expects_reference;
	bool pushed = false;
	enum v32_echo_result result;

	if (!frontend_header_is_valid(frontend))
		return V32_ECHO_INVALID_STATE;
	if (cleaned == NULL || cleaned_ready == NULL ||
	    memory_regions_overlap(cleaned, sizeof(*cleaned),
			   frontend, sizeof(*frontend)) ||
	    memory_regions_overlap(cleaned_ready, sizeof(*cleaned_ready),
			   frontend, sizeof(*frontend)) ||
	    memory_regions_overlap(cleaned, sizeof(*cleaned),
			   cleaned_ready, sizeof(*cleaned_ready)) ||
	    (accepted_reference != NULL &&
	     (memory_regions_overlap(
		      accepted_reference, sizeof(*accepted_reference),
			     frontend, sizeof(*frontend)) ||
	      memory_regions_overlap(
		      accepted_reference, sizeof(*accepted_reference),
			     cleaned, sizeof(*cleaned)) ||
	      memory_regions_overlap(
		      accepted_reference, sizeof(*accepted_reference),
			     cleaned_ready, sizeof(*cleaned_ready)))))
		return V32_ECHO_INVALID_ARGUMENT;
	produces = frontend->filter.inputs_needed == 1;
	expects_reference = produces && frontend->echo.phase == 0;
	if ((accepted_reference != NULL) != expects_reference)
		return V32_ECHO_INVALID_STATE;
	if (accepted_reference != NULL &&
	    !point_is_finite(&accepted_reference->point))
		return V32_ECHO_INVALID_ARGUMENT;

	next_filter = frontend->filter;
	if (filter_sample(&next_filter, received, &filtered) != produces)
		return V32_ECHO_INVALID_STATE;
	if (!produces) {
		frontend->filter = next_filter;
		*cleaned_ready = false;
		return V32_ECHO_OK;
	}
	if (accepted_reference != NULL) {
		saved_write = frontend->echo.reference_write;
		saved_fill = frontend->echo.reference_fill;
		overwritten = frontend->echo.reference[saved_write];
		result = v32_echo_canceller_push_reference(
			&frontend->echo, &accepted_reference->point);
		if (result != V32_ECHO_OK)
			return result;
		pushed = true;
	}
	next_cleaned = (double)filtered;
	result = v32_echo_canceller_process(
		&frontend->echo, &next_cleaned, &next_cleaned, 1);
	if (result != V32_ECHO_OK) {
		if (pushed)
			restore_reference_push(
				&frontend->echo, saved_write, saved_fill,
				&overwritten);
		return result;
	}
	frontend->filter = next_filter;
	*cleaned = next_cleaned;
	*cleaned_ready = true;
	return V32_ECHO_OK;
}
