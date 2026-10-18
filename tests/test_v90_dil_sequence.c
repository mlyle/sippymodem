// SPDX-License-Identifier: BSD-3-Clause

#include "test.h"

#include "v90_dil_sequence.h"
#include "pcm_dil_receiver.h"

#include <stdbool.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static struct v90_dil_parameters parameters(void)
{
	struct v90_dil_parameters generated = {
		.sign_pattern = { 0x05 },
		.training_pattern = { 0x06 },
		.segment_count = 2,
		.sign_pattern_bits = 3,
		.training_pattern_bits = 4,
	};

	generated.training_ucodes[0] = 17;
	generated.training_ucodes[1] = 112;
	generated.reference_ucodes[1] = 16;
	generated.reference_ucodes[7] = 120;
	generated.segment_length_codes[7] = 1;
	return generated;
}

static bool packed_bit(const uint8_t *pattern, size_t length, size_t index)
{
	index %= length;
	return ((pattern[index / 8u] >> (index % 8u)) & 1u) != 0u;
}

static bool symbols_are_equal(
	const struct v90_pcm_symbol *left,
	const struct v90_pcm_symbol *right)
{
	return left->ucode == right->ucode &&
	       left->positive == right->positive &&
	       left->octet == right->octet &&
	       left->sample == right->sample;
}

static bool expected_symbol(
	const struct v90_dil_parameters *dil, enum g711_law law,
	size_t cycle_symbol, struct v90_pcm_symbol *symbol)
{
	size_t segment = 0;
	size_t within = cycle_symbol;
	uint8_t training;
	uint8_t chord;
	size_t length;

	for (;;) {
		training = dil->training_ucodes[segment];
		chord = training >> 4;
		length = (size_t)(dil->segment_length_codes[chord] + 1u) * 6u;
		if (within < length)
			break;
		within -= length;
		segment++;
	}
	return v90_pcm_symbol_make(
		symbol, law,
		packed_bit(
			dil->training_pattern, dil->training_pattern_bits,
			within) ? training : dil->reference_ucodes[chord],
		packed_bit(dil->sign_pattern, dil->sign_pattern_bits, within));
}

static bool test_exact_cycles(void)
{
	struct v90_dil_parameters dil = parameters();

	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		struct v90_dil_sequence sequence = { 0 };

		CHECK(v90_dil_sequence_init(&sequence, &dil, law) ==
			      V90_DIL_SEQUENCE_OK &&
		      v90_dil_sequence_is_valid(&sequence) &&
		      v90_dil_sequence_at_segment_boundary(&sequence),
		      "DIL sequence did not initialize for law %d", law);
		for (size_t index = 0; index < 36u; ++index) {
			struct v90_pcm_symbol expected;
			struct v90_pcm_symbol actual = { 0 };

			CHECK(expected_symbol(&dil, law, index % 18u, &expected) &&
			      v90_dil_sequence_next(&sequence, &actual) ==
				      V90_DIL_SEQUENCE_OK &&
			      symbols_are_equal(&actual, &expected) &&
			      v90_dil_sequence_is_valid(&sequence),
			      "DIL symbol %zu differed for law %d", index, law);
			CHECK(v90_dil_sequence_at_segment_boundary(&sequence) ==
				      (index == 5u || index == 17u ||
				       index == 23u || index == 35u),
			      "DIL boundary %zu differed for law %d", index, law);
		}
	}
	return true;
}

static bool test_bounded_finish(void)
{
	struct v90_dil_parameters dil = parameters();
	struct v90_dil_sequence sequence = { 0 };
	struct v90_pcm_symbol symbol = { 0 };
	struct v90_pcm_symbol saved;

	CHECK(v90_dil_sequence_init(&sequence, &dil, G711_LAW_PCMU) ==
		      V90_DIL_SEQUENCE_OK,
	      "finish fixture did not initialize");
	for (size_t index = 0; index < 8u; ++index)
		CHECK(v90_dil_sequence_next(&sequence, &symbol) ==
			      V90_DIL_SEQUENCE_OK,
		      "finish fixture stopped at %zu", index);
	CHECK(v90_dil_sequence_request_finish(&sequence) ==
		      V90_DIL_SEQUENCE_OK,
	      "finish request was rejected");
	for (size_t index = 0; index < 10u; ++index)
		CHECK(v90_dil_sequence_next(&sequence, &symbol) ==
			      V90_DIL_SEQUENCE_OK,
		      "finish segment stopped at %zu", index);
	saved = symbol;
	CHECK(v90_dil_sequence_is_complete(&sequence) &&
	      v90_dil_sequence_at_segment_boundary(&sequence) &&
	      v90_dil_sequence_next(&sequence, &symbol) ==
		      V90_DIL_SEQUENCE_COMPLETE &&
	      symbols_are_equal(&symbol, &saved) &&
	      v90_dil_sequence_request_finish(&sequence) ==
		      V90_DIL_SEQUENCE_COMPLETE,
	      "DIL did not stop transactionally at the segment boundary");

	CHECK(v90_dil_sequence_init(&sequence, &dil, G711_LAW_PCMU) ==
		      V90_DIL_SEQUENCE_OK &&
	      v90_dil_sequence_request_finish(&sequence) ==
		      V90_DIL_SEQUENCE_OK &&
	      v90_dil_sequence_is_complete(&sequence),
	      "boundary finish emitted an unnecessary segment");
	return true;
}

static bool test_maximum_segment(void)
{
	struct v90_dil_parameters dil = parameters();
	struct v90_dil_sequence sequence = { 0 };
	struct v90_pcm_symbol symbol;

	dil.segment_count = 1;
	dil.training_ucodes[0] = 127;
	dil.training_ucodes[1] = 0;
	dil.segment_length_codes[7] = 127;
	CHECK(v90_dil_sequence_init(&sequence, &dil, G711_LAW_PCMA) ==
		      V90_DIL_SEQUENCE_OK,
	      "maximum-segment fixture did not initialize");
	for (size_t index = 0; index < V90_DIL_MAX_SEGMENT_SYMBOLS; ++index)
		CHECK(v90_dil_sequence_next(&sequence, &symbol) ==
			      V90_DIL_SEQUENCE_OK,
		      "maximum segment stopped at %zu", index);
	CHECK(v90_dil_sequence_at_segment_boundary(&sequence) &&
	      !v90_dil_sequence_is_complete(&sequence),
	      "maximum segment did not wrap exactly");
	return true;
}

static bool test_rejected_segment_transition(void)
{
	for (unsigned damage = 0; damage < 3u; ++damage) {
		struct v90_dil_parameters dil = parameters();
		struct v90_dil_sequence sequence;
		struct v90_dil_sequence saved;
		struct v90_pcm_symbol symbol = { 0 };
		struct v90_pcm_symbol saved_symbol;

		CHECK(v90_dil_sequence_init(&sequence, &dil, G711_LAW_PCMU) ==
			      V90_DIL_SEQUENCE_OK,
		      "segment transition fixture did not initialize");
		for (unsigned index = 0; index < 5u; ++index)
			CHECK(v90_dil_sequence_next(&sequence, &symbol) ==
				      V90_DIL_SEQUENCE_OK,
			      "segment transition fixture stopped early");
		if (damage == 0u)
			sequence.parameters.training_ucodes[1] = 128u;
		else if (damage == 1u)
			sequence.parameters.reference_ucodes[7] = 128u;
		else
			sequence.parameters.segment_length_codes[7] = 128u;
		saved = sequence;
		saved_symbol = symbol;
		CHECK(v90_dil_sequence_next(&sequence, &symbol) ==
			      V90_DIL_SEQUENCE_INVALID_STATE &&
		      memcmp(&sequence, &saved, sizeof(sequence)) == 0 &&
		      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0,
		      "invalid next segment changed the cursor or emitted a symbol");
		CHECK(v90_dil_sequence_request_finish(&sequence) ==
			      V90_DIL_SEQUENCE_OK &&
		      v90_dil_sequence_next(&sequence, &symbol) ==
			      V90_DIL_SEQUENCE_OK &&
		      v90_dil_sequence_is_complete(&sequence) &&
		      sequence.segment == 0u && sequence.segment_symbol == 6u,
		      "finishing the current segment inspected an unused segment");
	}
	return true;
}

static bool test_contracts(void)
{
	struct v90_dil_parameters dil = parameters();
	struct v90_dil_parameters zero = {
		.sign_pattern_bits = 1,
		.training_pattern_bits = 1,
	};
	struct v90_dil_sequence sequence;
	struct v90_dil_sequence saved;
	struct v90_pcm_symbol symbol = { 0 };

	memset(&sequence, 0xa5, sizeof(sequence));
	saved = sequence;
	CHECK(v90_dil_sequence_init(NULL, &dil, G711_LAW_PCMU) ==
		      V90_DIL_SEQUENCE_INVALID_ARGUMENT &&
	      v90_dil_sequence_init(&sequence, NULL, G711_LAW_PCMU) ==
		      V90_DIL_SEQUENCE_INVALID_ARGUMENT &&
	      v90_dil_sequence_init(&sequence, &zero, G711_LAW_PCMU) ==
		      V90_DIL_SEQUENCE_INVALID_ARGUMENT &&
	      v90_dil_sequence_init(
		      &sequence, &dil, (enum g711_law)99) ==
		      V90_DIL_SEQUENCE_INVALID_ARGUMENT &&
	      memcmp(&sequence, &saved, sizeof(sequence)) == 0,
	      "invalid initialization changed the DIL sequence");
	memset(&sequence, 0, sizeof(sequence));
	CHECK(v90_dil_sequence_next(&sequence, &symbol) ==
		      V90_DIL_SEQUENCE_INVALID_STATE &&
	      v90_dil_sequence_next(NULL, &symbol) ==
		      V90_DIL_SEQUENCE_INVALID_ARGUMENT &&
	      v90_dil_sequence_next(&sequence, NULL) ==
		      V90_DIL_SEQUENCE_INVALID_ARGUMENT &&
	      v90_dil_sequence_request_finish(NULL) ==
		      V90_DIL_SEQUENCE_INVALID_ARGUMENT &&
	      strcmp(v90_dil_sequence_result_name(V90_DIL_SEQUENCE_OK), "ok") ==
		      0 &&
	      strcmp(v90_dil_sequence_result_name(
			     (enum v90_dil_sequence_result)99), "unknown") == 0,
	      "DIL sequence contracts changed");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

static uint8_t impaired_octet(uint8_t octet, unsigned pattern, unsigned phase)
{
	if (pattern && phase == 4u)
		octet |= 1u;
	if (pattern == 2u && phase == 1u)
		octet &= (uint8_t)~1u;
	return octet;
}

static bool test_measured_levels(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
	for (unsigned pattern = 0; pattern < 3u; ++pattern)
	for (unsigned rotation = 0; rotation < 6u; ++rotation)
	for (unsigned restored = 0; restored < 2u; ++restored)
	for (unsigned inverted = 0; inverted < 2u; ++inverted)
	for (unsigned raw_inverted = 0; raw_inverted < 2u; ++raw_inverted) {
		struct v90_dil_parameters parameters;
		struct v90_dil_sequence sequence;
		struct pcm_dil_receiver receiver;
		struct pcm_dil_result result;
		uint32_t length;

		CHECK(v90_dil_profile_init(&parameters, V90_DIL_PROFILE_CONVENTIONAL) == V90_DIL_OK &&
		      v90_dil_training_symbol_count(&parameters, &length) &&
		      v90_dil_sequence_init(&sequence, &parameters, law) == V90_DIL_SEQUENCE_OK &&
		      pcm_dil_receiver_init(&receiver, &parameters, law, 90u), "DIL measurement init");
		for (uint32_t index = 0; index < length; ++index) {
			struct v90_pcm_symbol symbol;
			CHECK(v90_dil_sequence_next(&sequence, &symbol) == V90_DIL_SEQUENCE_OK, "DIL symbol failed");
			uint8_t octet = impaired_octet(symbol.octet, pattern, (index + rotation) % 6u);
			/* Even perfect conditioning cannot make an altered D/A level
			 * safe to advertise as an identical transmit/codec point. */
			uint8_t sign = inverted ? 0x80u : 0u;
			CHECK(pcm_dil_receiver_receive(&receiver, (restored ? symbol.octet : octet) ^ sign, octet ^ (raw_inverted ? 0x80u : 0u)),
			      "DIL observation failed");
		}
		CHECK(pcm_dil_receiver_finish(&receiver, &result) && result.measured &&
		      result.interval_model && result.symbols == length && result.polarity_inverted == (inverted != 0u),
		      "DIL did not qualify the interval model and line polarity");
		struct pcm_constellation common = result.usable[0];
		for (unsigned interval = 0; interval < 6u; ++interval) {
			CHECK(pcm_constellation_count(&result.usable[interval]) >= 40u, "too few measured levels");
			for (unsigned byte = 0; byte < sizeof(common.ucode_mask); ++byte)
				common.ucode_mask[byte] &= result.usable[interval].ucode_mask[byte];
			for (uint8_t code = 2u; code <= 108u; ++code) {
				struct v90_pcm_symbol symbol;
				CHECK(v90_pcm_symbol_make(&symbol, law, code, true), "invalid code");
				CHECK(pcm_constellation_contains(&result.usable[interval], code) ==
				      (symbol.octet == impaired_octet(symbol.octet, pattern, (interval + rotation) % 6u)),
				      "incorrect qualified level: law %u phase %u code %u", law, interval, code);
			}
		}
		CHECK(pattern != 2u || pcm_constellation_is_empty(&common), "opposite robbed bits retained a common set");
		struct v90_pcm_symbol nominal;
		CHECK(v90_pcm_symbol_make(&nominal, law, 90u, true), "invalid training level");
		double energy = 0;
		for (unsigned interval = 0; interval < 6u; ++interval) {
			double value = g711_decode(law, impaired_octet(nominal.octet, pattern, interval));
			energy += value * value;
		}
		double ratio = nominal.sample / sqrt(energy / 6.0);
		CHECK(fabs(result.trn1d_ratio_q3_13 / 8192.0 - ratio) <= 0.5 / 8192.0,
		      "TRN1d ratio did not use codec-side UINFO RMS");
		if (!pattern)
			CHECK(result.gain_q3_13 == 8192u && result.rejected_levels == 0u,
			      "transparent DIL gain or rejection changed");
		struct pcm_dil_receiver before = receiver;
		CHECK(!pcm_dil_receiver_receive(&receiver, 0, 0) && memcmp(&receiver, &before, sizeof(receiver)) == 0,
		      "DIL overrun changed the receiver");
	}
	return true;
}

static bool test_measured_evidence(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
	for (unsigned unknown = 0; unknown < 2u; ++unknown) {
		struct v90_dil_parameters parameters;
		struct v90_dil_sequence sequence;
		struct pcm_dil_receiver receiver;
		struct pcm_dil_result result;
		uint32_t length;

		CHECK(v90_dil_profile_init(&parameters, V90_DIL_PROFILE_CONVENTIONAL) == V90_DIL_OK &&
		      v90_dil_training_symbol_count(&parameters, &length) &&
		      v90_dil_sequence_init(&sequence, &parameters, law) == V90_DIL_SEQUENCE_OK &&
		      pcm_dil_receiver_init(&receiver, &parameters, law, 91u), "evidence fixture failed");
		struct pcm_dil_receiver saved = receiver;
		struct pcm_dil_result untouched = { .symbols = 123u };
		result = untouched;
		CHECK(!pcm_dil_receiver_init(&receiver, &parameters, law, 128u) &&
		      memcmp(&receiver, &saved, sizeof(saved)) == 0 &&
		      !pcm_dil_receiver_finish(&receiver, &result) &&
		      memcmp(&result, &untouched, sizeof(result)) == 0, "rejected evidence changed output");
		for (uint32_t index = 0; index < length; ++index) {
			struct v90_pcm_symbol symbol;
			CHECK(v90_dil_sequence_next(&sequence, &symbol) == V90_DIL_SEQUENCE_OK, "evidence symbol failed");
			uint8_t raw = symbol.octet, conditioned = raw;
			if (symbol.ucode == 47u && index % 6u == 3u) {
				conditioned ^= 2u; /* Outside every supported LSB model. */
				if (unknown)
					raw = conditioned;
			}
			CHECK(pcm_dil_receiver_receive(&receiver, conditioned, raw), "evidence observation failed");
			if (index == 71u) {
				saved = receiver;
				CHECK(pcm_dil_receiver_finish(&receiver, &result) && result.measured &&
				      !result.interval_model && result.trn1d_ratio_q3_13 == 8192u &&
				      memcmp(&receiver, &saved, sizeof(saved)) == 0,
				      "short traversal fabricated an interval model or changed continuation");
			}
		}
		CHECK(pcm_dil_receiver_finish(&receiver, &result) &&
		      result.interval_model == !unknown && result.trn1d_ratio_q3_13 == 8192u,
		      "conditioned samples contaminated the raw substitution model");
		for (unsigned interval = 0; interval < 6u; ++interval) {
			CHECK(pcm_constellation_contains(&result.usable[interval], 47u) == (!unknown && interval != 3u),
			      "unqualified cell survived interval or conservative common selection");
			CHECK(pcm_constellation_contains(&result.usable[interval], 80u), "unchanged evidence was lost");
			CHECK(!pcm_constellation_contains(&result.usable[interval], 120u), "unobserved level was qualified");
		}
	}
	return true;
}

static bool test_polarity_evidence(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
	for (unsigned damage = 0; damage < 4u; ++damage) {
		struct v90_dil_parameters parameters;
		struct v90_dil_sequence sequence;
		struct pcm_dil_receiver receiver;
		struct pcm_dil_result result;
		uint32_t length;
		uint8_t changed_code = 0;

		CHECK(v90_dil_profile_init(&parameters, V90_DIL_PROFILE_CONVENTIONAL) == V90_DIL_OK &&
		      v90_dil_training_symbol_count(&parameters, &length) &&
		      v90_dil_sequence_init(&sequence, &parameters, law) == V90_DIL_SEQUENCE_OK &&
		      pcm_dil_receiver_init(&receiver, &parameters, law, 90u),
		      "polarity evidence fixture failed");
		if (damage == 3u) {
			struct v90_pcm_symbol discarded;
			CHECK(v90_dil_sequence_next(&sequence, &discarded) == V90_DIL_SEQUENCE_OK,
			      "shifted DIL fixture failed");
		}
		for (uint32_t index = 0; index < length; ++index) {
			struct v90_pcm_symbol symbol;
			CHECK(v90_dil_sequence_next(&sequence, &symbol) == V90_DIL_SEQUENCE_OK,
			      "polarity evidence symbol failed");
			uint8_t octet = symbol.octet;
			if ((damage == 0u && index % 2u) || (damage == 2u && index == 18u)) {
				octet ^= 0x80u;
				changed_code = symbol.ucode;
			}
			if (damage == 1u)
				octet = g711_encode(law, 0);
			CHECK(pcm_dil_receiver_receive(&receiver, octet, octet),
			      "polarity evidence observation failed");
		}
		CHECK(pcm_dil_receiver_finish(&receiver, &result), "polarity evidence finish failed");
		for (unsigned interval = 0; interval < 6u; ++interval)
			if (damage == 2u)
				CHECK(result.interval_model && !result.polarity_inverted &&
				      pcm_constellation_contains(&result.usable[interval], changed_code) == (interval != 0u) &&
				      pcm_constellation_contains(&result.usable[interval], 80u),
				      "one early wrong sign locked the polarity or retained its damaged cell");
			else
				CHECK(pcm_constellation_is_empty(&result.usable[interval]),
				      "changing polarity, silence or shifted DIL qualified a level");
	}
	return true;
}

int main(void)
{
	static const struct test_case tests[] = {
		{ "measured DIL levels", test_measured_levels },
		{ "polarity evidence and rejection", test_polarity_evidence },
		{ "measured evidence and conservative fallback", test_measured_evidence },
		{ "exact repeated cycles", test_exact_cycles },
		{ "bounded segment finish", test_bounded_finish },
		{ "maximum segment", test_maximum_segment },
		{ "rejected segment transition", test_rejected_segment_transition },
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
