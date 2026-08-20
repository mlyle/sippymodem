// SPDX-License-Identifier: BSD-3-Clause

#include "echo_training.h"

#include "memory_region.h"

#include <stddef.h>

static const double V32_ECHO_COARSE_ADAPTATION = 0.65;
static const double V32_ECHO_FINE_ADAPTATION = 0.065;

enum {
	V32_SIGNAL_A = 0,
	V32_SIGNAL_B = 1,
};

_Static_assert(
	V32_ECHO_TRAINING_PREFILL_SYMBOLS +
		V32_ECHO_TRAINING_COARSE_SYMBOLS ==
		V32_CONDITIONING_TRN_MIN_SYMBOLS,
	"V.32 echo acquisition must reach the minimum TRN interval");
_Static_assert(
	(int)V32_CALLING_ECHO_TRAINING_SYMBOLS <=
		(int)V32_CONDITIONING_TRN_MAX_SYMBOLS &&
	(int)V32_ANSWERING_ECHO_TRAINING_SYMBOLS <=
		(int)V32_CONDITIONING_TRN_MAX_SYMBOLS,
	"V.32 echo training must stay inside the TRN interval");

static bool role_is_valid(enum v32_transmitter_role role)
{
	return role == V32_CALLING_TRANSMITTER ||
	       role == V32_ANSWERING_TRANSMITTER;
}

static uint16_t symbol_limit(enum v32_transmitter_role role)
{
	return role == V32_CALLING_TRANSMITTER ?
		V32_CALLING_ECHO_TRAINING_SYMBOLS :
		V32_ANSWERING_ECHO_TRAINING_SYMBOLS;
}

static enum v32_transmitter_role training_role(
	const struct v32_echo_training *training)
{
	return training->conditioning.trn_encoder.scrambler.transmitter_role;
}

static bool s_extension_is_valid(
	const struct v32_echo_training *training,
	enum v32_transmitter_role role)
{
	return training->s_extension_symbols <=
		       V32_ECHO_MAX_ROUND_TRIP_SYMBOLS &&
	       (training->s_extension_symbols & 1u) == 0 &&
	       (role == V32_CALLING_TRANSMITTER ||
		training->s_extension_symbols == 0) &&
	       training->s_extension_emitted <=
		       training->s_extension_symbols &&
	       (training->s_extension_emitted ==
			training->s_extension_symbols ||
		training->conditioning.emitted_symbols == 0) &&
	       (training->conditioning.emitted_symbols == 0 ||
		training->s_extension_emitted ==
			training->s_extension_symbols);
}

static uint16_t trn_symbols_emitted(
	const struct v32_echo_training *training)
{
	uint16_t fixed_symbols = V32_CONDITIONING_S_SYMBOLS +
		V32_CONDITIONING_S_BAR_SYMBOLS;

	return training->conditioning.emitted_symbols > fixed_symbols ?
		(uint16_t)(training->conditioning.emitted_symbols -
			fixed_symbols) :
		0;
}

bool v32_echo_training_is_valid(
	const struct v32_echo_training *training)
{
	enum v32_transmitter_role role;

	if (training == NULL ||
	    !v32_conditioning_transmitter_is_valid(
		    &training->conditioning))
		return false;
	role = training_role(training);
	return role_is_valid(role) &&
	       s_extension_is_valid(training, role) &&
	       training->conditioning.trn_symbols == symbol_limit(role) &&
	       (!training->complete ||
		(training->s_extension_emitted ==
			 training->s_extension_symbols &&
		 v32_conditioning_transmitter_is_complete(
			 &training->conditioning)));
}

bool v32_echo_training_is_complete(
	const struct v32_echo_training *training)
{
	return v32_echo_training_is_valid(training) && training->complete;
}

enum v32_echo_training_result v32_echo_training_init(
	struct v32_echo_training *training,
	enum v32_transmitter_role role, uint16_t s_extension_symbols)
{
	struct v32_echo_training initialized = {
		.s_extension_symbols = s_extension_symbols,
	};

	if (training == NULL || !role_is_valid(role) ||
	    s_extension_symbols > V32_ECHO_MAX_ROUND_TRIP_SYMBOLS ||
	    (s_extension_symbols & 1u) != 0 ||
	    (role == V32_ANSWERING_TRANSMITTER &&
	     s_extension_symbols != 0) ||
	    v32_conditioning_transmitter_init(
		    &initialized.conditioning, role,
		    symbol_limit(role)) != V32_CONDITIONING_OK)
		return V32_ECHO_TRAINING_INVALID_ARGUMENT;
	if (!v32_echo_training_is_valid(&initialized))
		return V32_ECHO_TRAINING_INVALID_STATE;
	*training = initialized;
	return V32_ECHO_TRAINING_OK;
}

static bool adaptation_matches(double actual, double expected)
{
	return actual == expected;
}

static bool gain_lifecycle_matches(
	const struct v32_echo_training *training,
	const struct v32_receive_path *path)
{
	if (training->complete ||
	    training_role(training) == V32_ANSWERING_TRANSMITTER)
		return path->line.gain.gain_mode == AGC_GAIN_TRACKING;
	return path->line.gain.gain_mode == AGC_GAIN_FROZEN &&
	       path->line.gain.gain_mantissa_q15 != 0 &&
	       path->line.gain.signal_present;
}

bool v32_echo_training_path_is_valid(
	const struct v32_echo_training *training,
	const struct v32_receive_path *path)
{
	uint16_t coarse_end = V32_ECHO_TRAINING_PREFILL_SYMBOLS +
		V32_ECHO_TRAINING_COARSE_SYMBOLS;
	uint16_t symbols;

	if (!v32_echo_training_is_valid(training) ||
	    !v32_receive_path_is_valid(path) ||
	    !gain_lifecycle_matches(training, path))
		return false;
	if (training->complete)
		return !path->echo_training && !path->echo.echo.adapting;
	if (training->s_extension_emitted == 0 &&
	    training->conditioning.emitted_symbols == 0)
		return !path->echo_training && !path->echo.echo.adapting;
	if (!path->echo_training)
		return false;
	symbols = trn_symbols_emitted(training);
	if (symbols <= V32_ECHO_TRAINING_PREFILL_SYMBOLS)
		return !path->echo.echo.adapting;
	if (!path->echo.echo.adapting)
		return false;
	return adaptation_matches(
		path->echo.echo.adaptation,
		symbols <= coarse_end ?
			V32_ECHO_COARSE_ADAPTATION :
			V32_ECHO_FINE_ADAPTATION);
}

static bool arguments_are_separate(
	const struct v32_echo_training *training,
	const struct v32_receive_path *path, const uint8_t *quadrant)
{
	return training != NULL && path != NULL && quadrant != NULL &&
	       !memory_regions_overlap(
		       training, sizeof(*training), path, sizeof(*path)) &&
	       !memory_regions_overlap(
		       training, sizeof(*training), quadrant,
		       sizeof(*quadrant)) &&
	       !memory_regions_overlap(
		       path, sizeof(*path), quadrant, sizeof(*quadrant));
}

static enum v32_echo_training_result transition_path(
	struct v32_receive_path *path, uint16_t symbol)
{
	enum v32_receive_path_result result = V32_RECEIVE_PATH_OK;
	uint16_t coarse_end = V32_ECHO_TRAINING_PREFILL_SYMBOLS +
		V32_ECHO_TRAINING_COARSE_SYMBOLS;

	if (symbol == V32_ECHO_TRAINING_PREFILL_SYMBOLS)
		result = v32_receive_path_start_echo_adaptation(
			path, V32_ECHO_COARSE_ADAPTATION);
	else if (symbol == coarse_end)
		result = v32_receive_path_start_echo_adaptation(
			path, V32_ECHO_FINE_ADAPTATION);
	if (result == V32_RECEIVE_PATH_OK)
		return V32_ECHO_TRAINING_OK;
	return result == V32_RECEIVE_PATH_INVALID_ARGUMENT ?
		V32_ECHO_TRAINING_INVALID_ARGUMENT :
		V32_ECHO_TRAINING_INVALID_STATE;
}

static enum v32_echo_training_result finish_training(
	struct v32_echo_training *training,
	struct v32_receive_path *path)
{
	struct v32_echo_training next_training;
	struct v32_receive_path next_path;

	next_training = *training;
	next_path = *path;
	if (v32_receive_path_finish_echo_training(&next_path) !=
	    V32_RECEIVE_PATH_OK)
		return V32_ECHO_TRAINING_INVALID_STATE;
	if (training_role(&next_training) == V32_CALLING_TRANSMITTER &&
	    v32_receive_path_reacquire_gain(&next_path) !=
		    V32_RECEIVE_PATH_OK)
		return V32_ECHO_TRAINING_INVALID_STATE;
	next_training.complete = true;
	if (!v32_echo_training_path_is_valid(&next_training, &next_path))
		return V32_ECHO_TRAINING_INVALID_STATE;
	*training = next_training;
	*path = next_path;
	return V32_ECHO_TRAINING_COMPLETE;
}

enum v32_echo_training_result v32_echo_training_next(
	struct v32_echo_training *training,
	struct v32_receive_path *path, uint8_t *quadrant)
{
	struct v32_echo_training next_training;
	struct v32_receive_path next_path;
	uint16_t symbol;
	uint8_t next_quadrant;
	bool path_changes;

	if (!arguments_are_separate(training, path, quadrant))
		return V32_ECHO_TRAINING_INVALID_ARGUMENT;
	if (!v32_echo_training_path_is_valid(training, path))
		return V32_ECHO_TRAINING_INVALID_STATE;
	if (training->complete)
		return V32_ECHO_TRAINING_COMPLETE;
	if (training->s_extension_emitted <
	    training->s_extension_symbols) {
		next_training = *training;
		path_changes = next_training.s_extension_emitted == 0;
		if (path_changes) {
			next_path = *path;
			if (v32_receive_path_begin_echo_training(&next_path) !=
			    V32_RECEIVE_PATH_OK)
				return V32_ECHO_TRAINING_INVALID_STATE;
		}
		next_quadrant =
			(next_training.s_extension_emitted & 1u) != 0 ?
				V32_SIGNAL_B : V32_SIGNAL_A;
		next_training.s_extension_emitted++;
		if (!v32_echo_training_path_is_valid(
			    &next_training,
			    path_changes ? &next_path : path))
			return V32_ECHO_TRAINING_INVALID_STATE;
		*training = next_training;
		if (path_changes)
			*path = next_path;
		*quadrant = next_quadrant;
		return V32_ECHO_TRAINING_OK;
	}
	if (v32_conditioning_transmitter_is_complete(
		    &training->conditioning))
		return finish_training(training, path);

	next_training = *training;
	symbol = trn_symbols_emitted(&next_training);
	path_changes = (next_training.s_extension_emitted == 0 &&
			next_training.conditioning.emitted_symbols == 0) ||
		(next_training.conditioning.emitted_symbols >=
			 V32_CONDITIONING_S_SYMBOLS +
			 V32_CONDITIONING_S_BAR_SYMBOLS &&
		 (symbol == V32_ECHO_TRAINING_PREFILL_SYMBOLS ||
		  symbol == V32_ECHO_TRAINING_PREFILL_SYMBOLS +
			  V32_ECHO_TRAINING_COARSE_SYMBOLS));
	if (path_changes) {
		enum v32_echo_training_result result;

		next_path = *path;
		result = next_training.conditioning.emitted_symbols == 0 ?
			(v32_receive_path_begin_echo_training(&next_path) ==
					 V32_RECEIVE_PATH_OK ?
				 V32_ECHO_TRAINING_OK :
				 V32_ECHO_TRAINING_INVALID_STATE) :
			transition_path(&next_path, symbol);
		if (result != V32_ECHO_TRAINING_OK)
			return result;
	}
	if (v32_conditioning_transmitter_next(
		    &next_training.conditioning, &next_quadrant) !=
	    V32_CONDITIONING_OK ||
	    !v32_echo_training_path_is_valid(
		    &next_training, path_changes ? &next_path : path))
		return V32_ECHO_TRAINING_INVALID_STATE;
	*training = next_training;
	if (path_changes)
		*path = next_path;
	*quadrant = next_quadrant;
	return V32_ECHO_TRAINING_OK;
}

enum v32_echo_training_result v32_echo_training_framing_state(
	const struct v32_echo_training *training,
	struct v32_symbol_encoder *encoder)
{
	if (training == NULL || encoder == NULL ||
	    memory_regions_overlap(
		    training, sizeof(*training), encoder, sizeof(*encoder)))
		return V32_ECHO_TRAINING_INVALID_ARGUMENT;
	if (!v32_echo_training_is_complete(training))
		return V32_ECHO_TRAINING_INVALID_STATE;
	return v32_conditioning_transmitter_framing_state(
		       &training->conditioning, encoder) == V32_CONDITIONING_OK ?
		       V32_ECHO_TRAINING_OK :
		       V32_ECHO_TRAINING_INVALID_STATE;
}
