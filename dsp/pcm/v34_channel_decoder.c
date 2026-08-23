// SPDX-License-Identifier: BSD-3-Clause

#include "v34_channel_decoder.h"

#include "memory_region.h"
#include "v34_branches.h"

#include <stddef.h>

static enum v34_precoder_grid_step expected_grid_step(
	const struct v34_mapping_parameters *parameters)
{
	return parameters->mapping_frame_bits < 56u ?
		V34_PRECODER_GRID_STEP_2 :
		V34_PRECODER_GRID_STEP_4;
}

static bool init_regions_overlap(
	const struct v34_channel_decoder *decoder,
	const struct v34_frame_geometry *geometry,
	const struct v34_q14_coefficient *coefficients)
{
	return memory_regions_overlap(
		       decoder, sizeof(*decoder), geometry, sizeof(*geometry)) ||
	       memory_regions_overlap(
		       decoder, sizeof(*decoder), coefficients,
		       sizeof(*coefficients) * V34_PRECODER_TAP_COUNT);
}

bool v34_channel_decoder_is_valid(
	const struct v34_channel_decoder *decoder)
{
	struct v34_mapping_parameters expected;

	return decoder != NULL &&
	       v34_frame_geometry_is_valid(&decoder->geometry) &&
	       v34_mapping_parameters_init(
		       &expected, &decoder->geometry,
		       decoder->parameters.expanded_shaping) ==
		       V34_MAPPING_PARAMETERS_OK &&
	       v34_mapping_parameters_are_equal(
		       &decoder->parameters, &expected) &&
	       v34_survivor_is_valid(&decoder->survivor) &&
	       v34_precoder_is_valid(&decoder->inverse_precoder) &&
	       decoder->inverse_precoder.grid_step ==
		       expected_grid_step(&decoder->parameters);
}

enum v34_channel_decoder_result
v34_channel_decoder_init(
	struct v34_channel_decoder *decoder,
	const struct v34_frame_geometry *geometry,
	bool expanded_shaping,
	const struct v34_q14_coefficient
		coefficients[V34_PRECODER_TAP_COUNT],
	enum v34_trellis_selection selection)
{
	struct v34_channel_decoder initialized = { 0 };

	if (decoder == NULL || geometry == NULL || coefficients == NULL ||
	    init_regions_overlap(decoder, geometry, coefficients) ||
	    !v34_frame_geometry_is_valid(geometry) ||
	    !v34_trellis_selection_is_valid(selection))
		return V34_CHANNEL_DECODER_INVALID_ARGUMENT;
	initialized.geometry = *geometry;
	if (v34_mapping_parameters_init(
		    &initialized.parameters, geometry, expanded_shaping) !=
		    V34_MAPPING_PARAMETERS_OK ||
	    v34_survivor_init(&initialized.survivor, selection) !=
		    V34_SURVIVOR_OK ||
	    v34_precoder_init(
		    &initialized.inverse_precoder, &initialized.parameters,
		    coefficients) !=
		    V34_PRECODER_OK ||
	    !v34_channel_decoder_is_valid(&initialized))
		return V34_CHANNEL_DECODER_INVALID_ARGUMENT;
	*decoder = initialized;
	return V34_CHANNEL_DECODER_OK;
}

static enum v34_channel_decoder_result lattice_result(
	enum v34_lattice_result result)
{
	return result == V34_LATTICE_NUMERIC_FAILURE ?
		V34_CHANNEL_DECODER_NUMERIC_FAILURE :
		V34_CHANNEL_DECODER_INVALID_ARGUMENT;
}

static enum v34_channel_decoder_result branches_result(
	enum v34_branches_result result)
{
	return result == V34_BRANCHES_NUMERIC_FAILURE ?
		V34_CHANNEL_DECODER_NUMERIC_FAILURE :
		V34_CHANNEL_DECODER_INVALID_ARGUMENT;
}

static enum v34_channel_decoder_result path_result(
	enum v34_path_decoder_result result)
{
	return result == V34_PATH_DECODER_OUT_OF_RANGE ?
		V34_CHANNEL_DECODER_OUT_OF_RANGE :
		V34_CHANNEL_DECODER_INVALID_ARGUMENT;
}

static enum v34_channel_decoder_result advance_path(
	struct v34_channel_decoder *decoder,
	const struct qam_point estimates[V34_2D_SYMBOLS_PER_4D_INTERVAL],
	uint8_t inversion_bit, struct v34_survivor_decision *current,
	struct v34_survivor_decision *traceback, bool *ready)
{
	struct v34_subset_slice slices[V34_2D_SYMBOLS_PER_4D_INTERVAL];
	struct v34_branch_set branches;

	for (uint8_t symbol = 0;
	     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
		enum v34_lattice_result result = v34_slice_grid_subsets(
			&estimates[symbol], &slices[symbol]);

		if (result != V34_LATTICE_OK)
			return lattice_result(result);
	}
	{
		enum v34_branches_result result = v34_build_branch_set(
			slices, inversion_bit, decoder->survivor.selection,
			&branches);

		if (result != V34_BRANCHES_OK)
			return branches_result(result);
	}
	{
		enum v34_survivor_result result = v34_survivor_advance(
			&decoder->survivor, &branches, current, traceback,
			ready);

		if (result != V34_SURVIVOR_OK)
			return result == V34_SURVIVOR_NUMERIC_FAILURE ?
				V34_CHANNEL_DECODER_NUMERIC_FAILURE :
				V34_CHANNEL_DECODER_INVALID_ARGUMENT;
	}
	return V34_CHANNEL_DECODER_OK;
}

static bool advance_regions_overlap(
	const struct v34_channel_decoder *decoder,
	const struct qam_point *estimates,
	const struct v34_survivor_decision *current,
	const struct v34_decided_4d_interval *decided, const bool *ready)
{
	const size_t estimates_size =
		sizeof(*estimates) * V34_2D_SYMBOLS_PER_4D_INTERVAL;

	return memory_regions_overlap(
		       decoder, sizeof(*decoder), estimates, estimates_size) ||
	       memory_regions_overlap(
		       decoder, sizeof(*decoder), current, sizeof(*current)) ||
	       memory_regions_overlap(
		       decoder, sizeof(*decoder), decided, sizeof(*decided)) ||
	       memory_regions_overlap(
		       decoder, sizeof(*decoder), ready, sizeof(*ready)) ||
	       memory_regions_overlap(
		       estimates, estimates_size, current, sizeof(*current)) ||
	       memory_regions_overlap(
		       estimates, estimates_size, decided, sizeof(*decided)) ||
	       memory_regions_overlap(
		       estimates, estimates_size, ready, sizeof(*ready)) ||
	       memory_regions_overlap(
		       current, sizeof(*current), decided, sizeof(*decided)) ||
	       memory_regions_overlap(
		       current, sizeof(*current), ready, sizeof(*ready)) ||
	       memory_regions_overlap(
		       decided, sizeof(*decided), ready, sizeof(*ready));
}

enum v34_channel_decoder_result
v34_channel_decoder_advance(
	struct v34_channel_decoder *decoder,
	const struct qam_point
		estimates[V34_2D_SYMBOLS_PER_4D_INTERVAL],
	uint8_t inversion_bit,
	struct v34_survivor_decision *current,
	struct v34_decided_4d_interval *decided,
	bool *decided_ready)
{
	struct v34_channel_decoder next;
	struct v34_survivor_decision generated_current;
	struct v34_survivor_decision traceback;
	struct v34_decided_4d_interval generated_decided;
	bool generated_ready;

	if (decoder == NULL || estimates == NULL || current == NULL ||
	    decided == NULL || decided_ready == NULL || inversion_bit > 1u ||
	    advance_regions_overlap(
		    decoder, estimates, current, decided, decided_ready) ||
	    !v34_channel_decoder_is_valid(decoder))
		return V34_CHANNEL_DECODER_INVALID_ARGUMENT;
	next = *decoder;
	{
		enum v34_channel_decoder_result result =
			advance_path(
				&next, estimates, inversion_bit,
				&generated_current, &traceback,
				&generated_ready);

		if (result != V34_CHANNEL_DECODER_OK)
			return result;
	}
	if (generated_ready) {
		enum v34_path_decoder_result result =
			v34_decode_survivor_path(
				&next.inverse_precoder, &traceback,
				&generated_decided);

		if (result != V34_PATH_DECODER_OK)
			return path_result(result);
	}
	if (!v34_channel_decoder_is_valid(&next))
		return V34_CHANNEL_DECODER_INVALID_ARGUMENT;
	*decoder = next;
	*current = generated_current;
	*decided_ready = generated_ready;
	if (generated_ready)
		*decided = generated_decided;
	return V34_CHANNEL_DECODER_OK;
}
