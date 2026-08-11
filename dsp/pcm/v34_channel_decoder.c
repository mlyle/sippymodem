// SPDX-License-Identifier: BSD-3-Clause

#include "v34_channel_decoder.h"

#include "v34_branches.h"

#include <stddef.h>

static enum sippymodem_v34_precoder_grid_step expected_grid_step(
	const struct sippymodem_v34_mapping_parameters *parameters)
{
	return parameters->mapping_frame_bits < 56u ?
		SIPPYMODEM_V34_PRECODER_GRID_STEP_2 :
		SIPPYMODEM_V34_PRECODER_GRID_STEP_4;
}

bool sippymodem_v34_channel_decoder_is_valid(
	const struct sippymodem_v34_channel_decoder *decoder)
{
	return decoder != NULL &&
	       sippymodem_v34_mapping_parameters_is_valid(
		       &decoder->parameters) &&
	       sippymodem_v34_survivor_is_valid(&decoder->survivor) &&
	       sippymodem_v34_precoder_is_valid(&decoder->inverse_precoder) &&
	       decoder->inverse_precoder.grid_step ==
		       expected_grid_step(&decoder->parameters);
}

enum sippymodem_v34_channel_decoder_result
sippymodem_v34_channel_decoder_init(
	struct sippymodem_v34_channel_decoder *decoder,
	const struct sippymodem_v34_mapping_parameters *parameters,
	const struct sippymodem_v34_q14_coefficient
		coefficients[SIPPYMODEM_V34_PRECODER_TAP_COUNT],
	enum sippymodem_v34_trellis_selection selection)
{
	struct sippymodem_v34_channel_decoder initialized = { 0 };

	if (decoder == NULL ||
	    !sippymodem_v34_mapping_parameters_is_valid(parameters))
		return SIPPYMODEM_V34_CHANNEL_DECODER_INVALID_ARGUMENT;
	initialized.parameters = *parameters;
	if (sippymodem_v34_survivor_init(
		    &initialized.survivor, selection) !=
		    SIPPYMODEM_V34_SURVIVOR_OK ||
	    sippymodem_v34_precoder_init(
		    &initialized.inverse_precoder, parameters, coefficients) !=
		    SIPPYMODEM_V34_PRECODER_OK ||
	    !sippymodem_v34_channel_decoder_is_valid(&initialized))
		return SIPPYMODEM_V34_CHANNEL_DECODER_INVALID_ARGUMENT;
	*decoder = initialized;
	return SIPPYMODEM_V34_CHANNEL_DECODER_OK;
}

static enum sippymodem_v34_channel_decoder_result lattice_result(
	enum sippymodem_v34_lattice_result result)
{
	return result == SIPPYMODEM_V34_LATTICE_NUMERIC_FAILURE ?
		SIPPYMODEM_V34_CHANNEL_DECODER_NUMERIC_FAILURE :
		SIPPYMODEM_V34_CHANNEL_DECODER_INVALID_ARGUMENT;
}

static enum sippymodem_v34_channel_decoder_result branches_result(
	enum sippymodem_v34_branches_result result)
{
	return result == SIPPYMODEM_V34_BRANCHES_NUMERIC_FAILURE ?
		SIPPYMODEM_V34_CHANNEL_DECODER_NUMERIC_FAILURE :
		SIPPYMODEM_V34_CHANNEL_DECODER_INVALID_ARGUMENT;
}

static enum sippymodem_v34_channel_decoder_result path_result(
	enum sippymodem_v34_path_decoder_result result)
{
	return result == SIPPYMODEM_V34_PATH_DECODER_OUT_OF_RANGE ?
		SIPPYMODEM_V34_CHANNEL_DECODER_OUT_OF_RANGE :
		SIPPYMODEM_V34_CHANNEL_DECODER_INVALID_ARGUMENT;
}

enum sippymodem_v34_channel_decoder_result
sippymodem_v34_channel_decoder_advance(
	struct sippymodem_v34_channel_decoder *decoder,
	const struct sippymodem_qam_point
		estimates[SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL],
	uint8_t inversion_bit,
	struct sippymodem_v34_survivor_decision *current,
	struct sippymodem_v34_decided_4d_interval *decided,
	bool *decided_ready)
{
	struct sippymodem_v34_subset_slice slices[2];
	struct sippymodem_v34_branch_set branches;
	struct sippymodem_v34_channel_decoder next;
	struct sippymodem_v34_survivor_decision generated_current;
	struct sippymodem_v34_survivor_decision traceback;
	struct sippymodem_v34_decided_4d_interval generated_decided;
	bool generated_ready;

	if (!sippymodem_v34_channel_decoder_is_valid(decoder) ||
	    estimates == NULL || inversion_bit > 1u || current == NULL ||
	    decided == NULL || decided_ready == NULL)
		return SIPPYMODEM_V34_CHANNEL_DECODER_INVALID_ARGUMENT;
	for (uint8_t symbol = 0;
	     symbol < SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
		enum sippymodem_v34_lattice_result result =
			sippymodem_v34_slice_grid_subsets(
				&estimates[symbol], &slices[symbol]);

		if (result != SIPPYMODEM_V34_LATTICE_OK)
			return lattice_result(result);
	}
	{
		enum sippymodem_v34_branches_result result =
			sippymodem_v34_build_branch_set(
				slices, inversion_bit, decoder->survivor.selection,
				&branches);

		if (result != SIPPYMODEM_V34_BRANCHES_OK)
			return branches_result(result);
	}
	next = *decoder;
	{
		enum sippymodem_v34_survivor_result result =
			sippymodem_v34_survivor_advance(
				&next.survivor, &branches, &generated_current,
				&traceback, &generated_ready);

		if (result != SIPPYMODEM_V34_SURVIVOR_OK)
			return result == SIPPYMODEM_V34_SURVIVOR_NUMERIC_FAILURE ?
				SIPPYMODEM_V34_CHANNEL_DECODER_NUMERIC_FAILURE :
				SIPPYMODEM_V34_CHANNEL_DECODER_INVALID_ARGUMENT;
	}
	if (generated_ready) {
		enum sippymodem_v34_path_decoder_result result =
			sippymodem_v34_decode_survivor_path(
				&next.inverse_precoder, &traceback,
				&generated_decided);

		if (result != SIPPYMODEM_V34_PATH_DECODER_OK)
			return path_result(result);
	}
	if (!sippymodem_v34_channel_decoder_is_valid(&next))
		return SIPPYMODEM_V34_CHANNEL_DECODER_INVALID_ARGUMENT;
	*decoder = next;
	*current = generated_current;
	*decided_ready = generated_ready;
	if (generated_ready)
		*decided = generated_decided;
	return SIPPYMODEM_V34_CHANNEL_DECODER_OK;
}

const char *sippymodem_v34_channel_decoder_result_name(
	enum sippymodem_v34_channel_decoder_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_CHANNEL_DECODER_OK:
		return "ok";
	case SIPPYMODEM_V34_CHANNEL_DECODER_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V34_CHANNEL_DECODER_NUMERIC_FAILURE:
		return "numeric failure";
	case SIPPYMODEM_V34_CHANNEL_DECODER_OUT_OF_RANGE:
		return "out of range";
	}
	return "unknown";
}
