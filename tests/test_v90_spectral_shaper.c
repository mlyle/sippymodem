// SPDX-License-Identifier: BSD-3-Clause

#include <v90_spectral_shaper.h>

#include "test.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

struct oracle_filter {
	double x;
	double y;
	double v;
};

struct oracle_shaper {
	struct v90_spectral_frame
		pending[V90_SPECTRAL_MAX_BUFFERED_FRAMES];
	struct oracle_filter filter;
	enum g711_law law;
	int8_t a1;
	int8_t a2;
	int8_t b1;
	int8_t b2;
	uint8_t redundancy;
	uint8_t lookahead;
	uint8_t pending_count;
	bool trellis_state;
	bool finishing;
};

static void add_ucode(
	struct pcm_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static bool make_mapping(
	struct v90_pcm_mapping *mapping, enum g711_law law,
	uint8_t redundancy, uint8_t lookahead)
{
	struct v90_cp_parameters parameters = {
		.kind = V90_CP_TRAINING,
		.law = law,
		.shaping_a1_q1_6 = 23,
		.shaping_a2_q1_6 = -19,
		.shaping_b1_q1_6 = 41,
		.shaping_b2_q1_6 = -11,
		.selected_rate_index = 4,
		.sign_redundancy = redundancy,
		.spectral_lookahead = lookahead,
		.constellation_count = 1,
	};

	for (uint8_t ucode = 20; ucode < 24; ++ucode)
		add_ucode(&parameters.transmit_constellations[0], ucode);
	return v90_pcm_mapping_init(mapping, &parameters) ==
	       V90_PCM_MAPPING_OK;
}

static size_t oracle_frame_length(const struct oracle_shaper *oracle)
{
	return V90_CP_FRAME_INTERVALS / oracle->redundancy;
}

static uint8_t oracle_apply_rule(
	uint8_t sign, size_t position, enum v90_spectral_rule rule)
{
	bool invert = rule == V90_SPECTRAL_RULE_B ||
		      (rule == V90_SPECTRAL_RULE_C &&
		       position % 2u == 0) ||
		      (rule == V90_SPECTRAL_RULE_D &&
		       position % 2u != 0);

	return sign ^ (uint8_t)invert;
}

static double oracle_filter_frame(
	const struct oracle_shaper *oracle,
	const struct v90_spectral_frame *frame,
	enum v90_spectral_rule rule,
	struct oracle_filter *filter)
{
	double metric = 0.0;
	double a1 = oracle->a1 / 64.0;
	double a2 = oracle->a2 / 64.0;
	double b1 = oracle->b1 / 64.0;
	double b2 = oracle->b2 / 64.0;

	for (size_t position = 0;
	     position < oracle_frame_length(oracle); ++position) {
		struct v90_pcm_symbol magnitude;
		bool positive = oracle_apply_rule(
			frame->initial_signs[position], position, rule) != 0;
		double x;
		double y;
		double v;

		if (!v90_pcm_symbol_make(
			    &magnitude, oracle->law,
			    frame->ucodes[position], true))
			return INFINITY;
		x = positive ? magnitude.sample : -(double)magnitude.sample;
		y = x - b1 * filter->x + a1 * filter->y;
		v = y - b2 * filter->y + a2 * filter->v;
		metric += v * v;
		filter->x = x;
		filter->y = y;
		filter->v = v;
	}
	return metric;
}

struct oracle_search {
	double metric;
	enum v90_spectral_rule first_rule;
};

static void oracle_search_rules(
	const struct oracle_shaper *oracle, size_t frame,
	size_t horizon, bool state, struct oracle_filter filter,
	double metric, enum v90_spectral_rule first_rule,
	struct oracle_search *best)
{
	if (frame == horizon) {
		if (metric < best->metric) {
			best->metric = metric;
			best->first_rule = first_rule;
		}
		return;
	}
	for (unsigned int branch = 0; branch < 2; ++branch) {
		enum v90_spectral_rule rule =
			(enum v90_spectral_rule)(
				(state ? V90_SPECTRAL_RULE_C :
					 V90_SPECTRAL_RULE_A) + branch);
		struct oracle_filter next_filter = filter;
		double next_metric = metric + oracle_filter_frame(
			oracle, &oracle->pending[frame], rule,
			&next_filter);

		oracle_search_rules(
			oracle, frame + 1u, horizon,
			(rule & 1u) != 0, next_filter, next_metric,
			frame == 0 ? rule : first_rule, best);
	}
}

static void oracle_emit(
	struct oracle_shaper *oracle, size_t horizon,
	struct v90_spectral_output *output)
{
	struct oracle_search best = {
		.metric = DBL_MAX,
	};
	struct v90_spectral_output generated = { 0 };

	oracle_search_rules(
		oracle, 0, horizon, oracle->trellis_state,
		oracle->filter, 0.0, V90_SPECTRAL_RULE_A, &best);
	generated.rule = best.first_rule;
	generated.symbol_count = (uint8_t)oracle_frame_length(oracle);
	(void)oracle_filter_frame(
		oracle, &oracle->pending[0], best.first_rule,
		&oracle->filter);
	for (size_t position = 0;
	     position < generated.symbol_count; ++position) {
		(void)v90_pcm_symbol_make(
			&generated.symbols[position], oracle->law,
			oracle->pending[0].ucodes[position],
			oracle_apply_rule(
				oracle->pending[0].initial_signs[position],
				position, best.first_rule) != 0);
	}
	oracle->trellis_state = (best.first_rule & 1u) != 0;
	oracle->pending_count--;
	memmove(&oracle->pending[0], &oracle->pending[1],
		oracle->pending_count * sizeof(oracle->pending[0]));
	memset(&oracle->pending[oracle->pending_count], 0,
	       sizeof(oracle->pending[0]));
	*output = generated;
}

static void oracle_init(
	struct oracle_shaper *oracle,
	const struct v90_pcm_mapping *mapping)
{
	struct oracle_shaper initialized = {
		.law = mapping->law,
		.a1 = mapping->shaping_a1_q1_6,
		.a2 = mapping->shaping_a2_q1_6,
		.b1 = mapping->shaping_b1_q1_6,
		.b2 = mapping->shaping_b2_q1_6,
		.redundancy = mapping->sign_redundancy,
		.lookahead = mapping->spectral_lookahead,
	};

	*oracle = initialized;
}

static enum v90_spectral_shaper_result oracle_push(
	struct oracle_shaper *oracle, const uint8_t *ucodes,
	const uint8_t *signs, struct v90_spectral_output *output)
{
	size_t length = oracle_frame_length(oracle);
	struct v90_spectral_frame frame = { 0 };

	memcpy(frame.ucodes, ucodes, length);
	memcpy(frame.initial_signs, signs, length);
	oracle->pending[oracle->pending_count++] = frame;
	if (oracle->pending_count <= oracle->lookahead)
		return V90_SPECTRAL_SHAPER_BUFFERING;
	oracle_emit(oracle, oracle->pending_count, output);
	return V90_SPECTRAL_SHAPER_FRAME_READY;
}

static enum v90_spectral_shaper_result oracle_finish(
	struct oracle_shaper *oracle,
	struct v90_spectral_output *output)
{
	oracle->finishing = true;
	if (oracle->pending_count == 0)
		return V90_SPECTRAL_SHAPER_COMPLETE;
	oracle_emit(oracle, oracle->pending_count, output);
	return V90_SPECTRAL_SHAPER_FRAME_READY;
}

static bool outputs_equal(
	const struct v90_spectral_output *actual,
	const struct v90_spectral_output *expected)
{
	if (actual->rule != expected->rule ||
	    actual->symbol_count != expected->symbol_count)
		return false;
	for (size_t index = 0;
	     index < V90_SPECTRAL_MAX_FRAME_SYMBOLS; ++index) {
		const struct v90_pcm_symbol *left = &actual->symbols[index];
		const struct v90_pcm_symbol *right = &expected->symbols[index];

		if (left->ucode != right->ucode ||
		    left->positive != right->positive ||
		    left->octet != right->octet ||
		    left->sample != right->sample)
			return false;
	}
	return true;
}

static bool test_generated_all_modes(void)
{
	uint8_t seen_rules = 0;

	for (unsigned int law_index = 0; law_index < 2; ++law_index) {
		enum g711_law law = law_index == 0 ?
				    G711_LAW_PCMU : G711_LAW_PCMA;

		for (uint8_t redundancy = 1; redundancy <= 3; ++redundancy) {
			for (uint8_t lookahead = 0;
			     lookahead <= V90_SPECTRAL_MAX_LOOKAHEAD;
			     ++lookahead) {
				struct v90_pcm_mapping mapping;
				struct v90_spectral_shaper shaper;
				struct oracle_shaper oracle;
				size_t length = V90_CP_FRAME_INTERVALS /
						redundancy;

				CHECK(make_mapping(
					      &mapping, law, redundancy,
					      lookahead) &&
				      v90_spectral_shaper_init(
					      &shaper, &mapping) ==
					      V90_SPECTRAL_SHAPER_OK,
				      "mode %u/%u/%u did not initialize",
				      law_index, redundancy, lookahead);
				oracle_init(&oracle, &mapping);
				for (size_t frame = 0; frame < 24; ++frame) {
					uint8_t ucodes[6] = { 0 };
					uint8_t signs[6] = { 0 };
					struct v90_spectral_output actual;
					struct v90_spectral_output expected;
					enum v90_spectral_shaper_result actual_result;
					enum v90_spectral_shaper_result expected_result;

					for (size_t position = 0;
					     position < length; ++position) {
						ucodes[position] = (uint8_t)(
							(frame * 29u + position * 17u +
							 redundancy * 11u +
							 lookahead * 7u) % 128u);
						signs[position] = (uint8_t)(
							(frame * 5u + position * 3u +
							 law_index + redundancy) & 1u);
					}
					actual_result = v90_spectral_shaper_push(
						&shaper, ucodes, signs, length,
						&actual);
					expected_result = oracle_push(
						&oracle, ucodes, signs, &expected);
					CHECK(actual_result == expected_result,
					      "mode %u/%u/%u frame %zu returned %s",
					      law_index, redundancy, lookahead,
					      frame,
					      v90_spectral_shaper_result_name(
						      actual_result));
					if (actual_result ==
					    V90_SPECTRAL_SHAPER_FRAME_READY) {
						CHECK(outputs_equal(&actual, &expected),
						      "mode %u/%u/%u frame %zu chose a non-minimal rule",
						      law_index, redundancy,
						      lookahead, frame);
						seen_rules |= (uint8_t)(
							1u << actual.rule);
					}
				}
				for (;;) {
					struct v90_spectral_output actual;
					struct v90_spectral_output expected;
					enum v90_spectral_shaper_result actual_result =
						v90_spectral_shaper_finish(
							&shaper, &actual);
					enum v90_spectral_shaper_result expected_result =
						oracle_finish(&oracle, &expected);

					CHECK(actual_result == expected_result,
					      "mode %u/%u/%u tail result differed",
					      law_index, redundancy, lookahead);
					if (actual_result ==
					    V90_SPECTRAL_SHAPER_COMPLETE)
						break;
					CHECK(outputs_equal(&actual, &expected),
					      "mode %u/%u/%u tail frame differed",
					      law_index, redundancy, lookahead);
					seen_rules |= (uint8_t)(1u << actual.rule);
				}
				CHECK(v90_spectral_shaper_is_valid(&shaper) &&
				      fabs(shaper.previous_x - oracle.filter.x) <
					      1e-9 &&
				      fabs(shaper.previous_y - oracle.filter.y) <
					      1e-9 &&
				      fabs(shaper.previous_v - oracle.filter.v) <
					      1e-9 &&
				      shaper.trellis_state == oracle.trellis_state,
				      "mode %u/%u/%u committed different state",
				      law_index, redundancy, lookahead);
			}
		}
	}
	CHECK(seen_rules == 0x0fu,
	      "generated cases did not exercise every inversion rule (0x%02x)",
	      seen_rules);
	return true;
}

static bool test_rule_ties_and_tail(void)
{
	struct v90_pcm_mapping mapping;
	struct v90_spectral_shaper shaper;
	struct v90_spectral_output output;
	struct v90_spectral_output saved;
	uint8_t ucodes[6] = { 0 };
	uint8_t signs[6] = { 0, 1, 1, 0, 1, 0 };

	CHECK(make_mapping(&mapping, G711_LAW_PCMU, 1, 0),
	      "tie mapping did not initialize");
	mapping.shaping_a1_q1_6 = 0;
	mapping.shaping_a2_q1_6 = 0;
	mapping.shaping_b1_q1_6 = 0;
	mapping.shaping_b2_q1_6 = 0;
	CHECK(v90_spectral_shaper_init(&shaper, &mapping) ==
		      V90_SPECTRAL_SHAPER_OK &&
	      v90_spectral_shaper_push(
		      &shaper, ucodes, signs, 6, &output) ==
		      V90_SPECTRAL_SHAPER_FRAME_READY &&
	      output.rule == V90_SPECTRAL_RULE_A,
	      "state zero did not resolve an exact tie to rule A");
	for (size_t position = 0; position < 6; ++position) {
		CHECK(output.symbols[position].positive ==
			      (signs[position] != 0),
		      "rule A changed sign %zu", position);
	}
	CHECK(shaper.previous_x == 0.0 && shaper.previous_y == 0.0 &&
	      shaper.previous_v == 0.0,
	      "PCMU negative zero entered the spectral metric");
	CHECK(v90_spectral_shaper_init(&shaper, &mapping) ==
		      V90_SPECTRAL_SHAPER_OK,
	      "state-one fixture did not initialize");
	shaper.trellis_state = true;
	CHECK(v90_spectral_shaper_is_valid(&shaper) &&
	      v90_spectral_shaper_push(
		      &shaper, ucodes, signs, 6, &output) ==
		      V90_SPECTRAL_SHAPER_FRAME_READY &&
	      output.rule == V90_SPECTRAL_RULE_C,
	      "state one did not resolve an exact tie to rule C");
	for (size_t position = 0; position < 6; ++position) {
		bool expected = (signs[position] ^
				 (uint8_t)(position % 2u == 0)) != 0;

		CHECK(output.symbols[position].positive == expected,
		      "rule C changed sign %zu incorrectly", position);
	}

	CHECK(make_mapping(&mapping, G711_LAW_PCMA, 1, 3) &&
	      v90_spectral_shaper_init(&shaper, &mapping) ==
		      V90_SPECTRAL_SHAPER_OK,
	      "tail fixture did not initialize");
	memset(&output, 0xa5, sizeof(output));
	saved = output;
	for (size_t frame = 0; frame < 3; ++frame) {
		CHECK(v90_spectral_shaper_push(
			      &shaper, ucodes, signs, 6, &output) ==
			      V90_SPECTRAL_SHAPER_BUFFERING &&
		      memcmp(&output, &saved, sizeof(output)) == 0,
		      "look-ahead frame %zu changed buffered output", frame);
	}
	for (size_t frame = 0; frame < 3; ++frame) {
		CHECK(v90_spectral_shaper_finish(&shaper, &output) ==
			      V90_SPECTRAL_SHAPER_FRAME_READY,
		      "tail frame %zu was lost", frame);
	}
	memset(&output, 0xa5, sizeof(output));
	saved = output;
	CHECK(v90_spectral_shaper_finish(&shaper, &output) ==
		      V90_SPECTRAL_SHAPER_COMPLETE &&
	      v90_spectral_shaper_finish(&shaper, &output) ==
		      V90_SPECTRAL_SHAPER_COMPLETE &&
	      memcmp(&output, &saved, sizeof(output)) == 0 &&
	      v90_spectral_shaper_push(
		      &shaper, ucodes, signs, 6, &output) ==
		      V90_SPECTRAL_SHAPER_INVALID_STATE,
	      "completed tail did not latch its lifecycle");
	return true;
}

static bool test_contracts(void)
{
	struct v90_pcm_mapping mapping;
	struct v90_spectral_shaper shaper;
	struct v90_spectral_shaper saved_shaper;
	struct v90_spectral_output output;
	struct v90_spectral_output saved_output;
	uint8_t ucodes[6] = { 1, 2, 3, 4, 5, 6 };
	uint8_t signs[6] = { 0, 1, 0, 1, 0, 1 };

	CHECK(make_mapping(&mapping, G711_LAW_PCMU, 0, 0) &&
	      v90_spectral_shaper_init(&shaper, &mapping) ==
		      V90_SPECTRAL_SHAPER_INVALID_PARAMETERS,
	      "shaper accepted a mapping without spectral signs");
	CHECK(make_mapping(&mapping, G711_LAW_PCMU, 1, 0) &&
	      v90_spectral_shaper_init(&shaper, &mapping) ==
		      V90_SPECTRAL_SHAPER_OK,
	      "contract fixture did not initialize");
	memset(&output, 0xa5, sizeof(output));
	saved_output = output;
	saved_shaper = shaper;
	ucodes[2] = 128;
	CHECK(v90_spectral_shaper_push(
		      &shaper, ucodes, signs, 6, &output) ==
		      V90_SPECTRAL_SHAPER_OUT_OF_RANGE &&
	      memcmp(&shaper, &saved_shaper, sizeof(shaper)) == 0 &&
	      memcmp(&output, &saved_output, sizeof(output)) == 0,
	      "out-of-range Ucode changed state or output");
	ucodes[2] = 3;
	signs[4] = 2;
	CHECK(v90_spectral_shaper_push(
		      &shaper, ucodes, signs, 6, &output) ==
		      V90_SPECTRAL_SHAPER_OUT_OF_RANGE &&
	      memcmp(&shaper, &saved_shaper, sizeof(shaper)) == 0 &&
	      memcmp(&output, &saved_output, sizeof(output)) == 0,
	      "non-binary sign changed state or output");
	signs[4] = 0;
	CHECK(v90_spectral_shaper_push(
		      &shaper, ucodes, signs, 5, &output) ==
		      V90_SPECTRAL_SHAPER_OUT_OF_RANGE &&
	      v90_spectral_shaper_push(
		      NULL, ucodes, signs, 6, &output) ==
		      V90_SPECTRAL_SHAPER_INVALID_ARGUMENT &&
	      v90_spectral_shaper_finish(NULL, &output) ==
		      V90_SPECTRAL_SHAPER_INVALID_ARGUMENT,
	      "invalid call contract changed");

	shaper.pending[3].ucodes[0] = 1;
	CHECK(!v90_spectral_shaper_is_valid(&shaper) &&
	      v90_spectral_shaper_push(
		      &shaper, ucodes, signs, 6, &output) ==
		      V90_SPECTRAL_SHAPER_INVALID_STATE,
	      "non-canonical queue state was accepted");
	CHECK(v90_spectral_shaper_init(&shaper, &mapping) ==
		      V90_SPECTRAL_SHAPER_OK,
	      "numeric fixture did not initialize");
	shaper.shaping_a2_q1_6 = 64;
	shaper.previous_v = DBL_MAX;
	saved_shaper = shaper;
	CHECK(v90_spectral_shaper_is_valid(&shaper) &&
	      v90_spectral_shaper_push(
		      &shaper, ucodes, signs, 6, &output) ==
		      V90_SPECTRAL_SHAPER_NUMERIC_FAILURE &&
	      memcmp(&shaper, &saved_shaper, sizeof(shaper)) == 0 &&
	      memcmp(&output, &saved_output, sizeof(output)) == 0,
	      "numeric failure was not transactional");
	CHECK(make_mapping(&mapping, G711_LAW_PCMU, 1, 3) &&
	      v90_spectral_shaper_init(&shaper, &mapping) ==
		      V90_SPECTRAL_SHAPER_OK,
	      "full-lookahead numeric fixture did not initialize");
	for (size_t frame = 0; frame < 3; ++frame) {
		CHECK(v90_spectral_shaper_push(
			      &shaper, ucodes, signs, 6, &output) ==
			      V90_SPECTRAL_SHAPER_BUFFERING,
		      "full-lookahead fixture did not buffer frame %zu", frame);
	}
	shaper.shaping_a2_q1_6 = 64;
	shaper.previous_v = DBL_MAX;
	saved_shaper = shaper;
	memset(&output, 0xa5, sizeof(output));
	saved_output = output;
	CHECK(v90_spectral_shaper_is_valid(&shaper) &&
	      v90_spectral_shaper_push(
		      &shaper, ucodes, signs, 6, &output) ==
		      V90_SPECTRAL_SHAPER_NUMERIC_FAILURE &&
	      memcmp(&shaper, &saved_shaper, sizeof(shaper)) == 0 &&
	      memcmp(&output, &saved_output, sizeof(output)) == 0 &&
	      v90_spectral_shaper_finish(&shaper, &output) ==
		      V90_SPECTRAL_SHAPER_NUMERIC_FAILURE &&
	      memcmp(&shaper, &saved_shaper, sizeof(shaper)) == 0 &&
	      memcmp(&output, &saved_output, sizeof(output)) == 0,
	      "full-lookahead numeric failure changed state or output");
	mapping.shaping_b1_q1_6 = 65;
	CHECK(v90_spectral_shaper_init(&shaper, &mapping) ==
		      V90_SPECTRAL_SHAPER_INVALID_PARAMETERS &&
	      !v90_spectral_shaper_is_valid(NULL) &&
	      strcmp(v90_spectral_shaper_result_name(
			     V90_SPECTRAL_SHAPER_FRAME_READY),
		     "frame ready") == 0 &&
	      strcmp(v90_spectral_shaper_result_name(
			     (enum v90_spectral_shaper_result)99),
		     "unknown") == 0,
	      "initialization or result-name contract changed");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "generated all modes", test_generated_all_modes },
		{ "rule ties and tail", test_rule_ties_and_tail },
		{ "contracts", test_contracts },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		printf("[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			printf("[       OK ] %s\n", tests[index].name);
		else {
			printf("[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
