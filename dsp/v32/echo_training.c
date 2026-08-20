// SPDX-License-Identifier: BSD-3-Clause

#include "echo_training.h"

#include "memory_region.h"

#include <stddef.h>

static const double V32_ECHO_COARSE_ADAPTATION = 0.65;
static const double V32_ECHO_FINE_ADAPTATION = 0.065;

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

bool v32_echo_training_is_valid(
	const struct v32_echo_training *training)
{
	return training != NULL && role_is_valid(training->role) &&
	       training->symbols <= symbol_limit(training->role) &&
	       (!training->complete ||
		training->symbols == symbol_limit(training->role));
}

bool v32_echo_training_is_complete(
	const struct v32_echo_training *training)
{
	return v32_echo_training_is_valid(training) && training->complete;
}

enum v32_echo_training_result v32_echo_training_init(
	struct v32_echo_training *training,
	enum v32_transmitter_role role)
{
	struct v32_echo_training initialized = {
		.role = role,
	};

	if (training == NULL || !role_is_valid(role))
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

bool v32_echo_training_path_is_valid(
	const struct v32_echo_training *training,
	const struct v32_receive_path *path)
{
	uint16_t coarse_end = V32_ECHO_TRAINING_PREFILL_SYMBOLS +
		V32_ECHO_TRAINING_COARSE_SYMBOLS;

	if (!v32_echo_training_is_valid(training) ||
	    !v32_receive_path_is_valid(path))
		return false;
	if (training->complete)
		return !path->echo_training && !path->echo.echo.adapting;
	if (training->symbols == 0)
		return !path->echo_training && !path->echo.echo.adapting;
	if (!path->echo_training)
		return false;
	if (training->symbols <= V32_ECHO_TRAINING_PREFILL_SYMBOLS)
		return !path->echo.echo.adapting;
	if (!path->echo.echo.adapting)
		return false;
	return adaptation_matches(
		path->echo.echo.adaptation,
		training->symbols <= coarse_end ?
			V32_ECHO_COARSE_ADAPTATION :
			V32_ECHO_FINE_ADAPTATION);
}

static bool arguments_are_separate(
	const struct v32_echo_training *training,
	const struct v32_receive_path *path)
{
	return training != NULL && path != NULL &&
	       !memory_regions_overlap(
		       training, sizeof(*training), path, sizeof(*path));
}

enum v32_echo_training_result v32_echo_training_begin_symbol(
	struct v32_echo_training *training,
	struct v32_receive_path *path)
{
	struct v32_echo_training next_training;
	struct v32_receive_path next_path;
	enum v32_receive_path_result result = V32_RECEIVE_PATH_OK;
	uint16_t coarse_end = V32_ECHO_TRAINING_PREFILL_SYMBOLS +
		V32_ECHO_TRAINING_COARSE_SYMBOLS;

	if (!arguments_are_separate(training, path))
		return V32_ECHO_TRAINING_INVALID_ARGUMENT;
	if (!v32_echo_training_path_is_valid(training, path) ||
	    training->complete ||
	    training->symbols == symbol_limit(training->role))
		return V32_ECHO_TRAINING_INVALID_STATE;
	next_training = *training;
	next_path = *path;
	if (next_training.symbols == 0)
		result = v32_receive_path_begin_echo_training(&next_path);
	else if (next_training.symbols ==
		 V32_ECHO_TRAINING_PREFILL_SYMBOLS)
		result = v32_receive_path_start_echo_adaptation(
			&next_path, V32_ECHO_COARSE_ADAPTATION);
	else if (next_training.symbols == coarse_end)
		result = v32_receive_path_start_echo_adaptation(
			&next_path, V32_ECHO_FINE_ADAPTATION);
	if (result != V32_RECEIVE_PATH_OK)
		return result == V32_RECEIVE_PATH_INVALID_ARGUMENT ?
			V32_ECHO_TRAINING_INVALID_ARGUMENT :
			V32_ECHO_TRAINING_INVALID_STATE;
	next_training.symbols++;
	if (!v32_echo_training_path_is_valid(&next_training, &next_path))
		return V32_ECHO_TRAINING_INVALID_STATE;
	*training = next_training;
	*path = next_path;
	return V32_ECHO_TRAINING_OK;
}

enum v32_echo_training_result v32_echo_training_finish(
	struct v32_echo_training *training,
	struct v32_receive_path *path)
{
	struct v32_echo_training next_training;
	struct v32_receive_path next_path;

	if (!arguments_are_separate(training, path))
		return V32_ECHO_TRAINING_INVALID_ARGUMENT;
	if (!v32_echo_training_path_is_valid(training, path) ||
	    training->complete ||
	    training->symbols != symbol_limit(training->role))
		return V32_ECHO_TRAINING_INVALID_STATE;
	next_training = *training;
	next_path = *path;
	if (v32_receive_path_finish_echo_training(&next_path) !=
	    V32_RECEIVE_PATH_OK)
		return V32_ECHO_TRAINING_INVALID_STATE;
	next_training.complete = true;
	if (!v32_echo_training_path_is_valid(&next_training, &next_path))
		return V32_ECHO_TRAINING_INVALID_STATE;
	*training = next_training;
	*path = next_path;
	return V32_ECHO_TRAINING_COMPLETE;
}
