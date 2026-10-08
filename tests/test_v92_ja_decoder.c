// SPDX-License-Identifier: BSD-3-Clause

#include <v92_ja_decoder.h>
#include <v92_pcm_phase3_trn1u.h>

#include "test.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct sign_source {
	struct v34_scrambler scrambler;
	uint8_t previous_sign;
};

static bool source_init(struct sign_source *source)
{
	struct sign_source initialized = { 0 };

	if (source == NULL ||
	    v34_scrambler_init(
		    &initialized.scrambler, V34_POLYNOMIAL_GPA) !=
		    V34_FRAMING_OK)
		return false;
	*source = initialized;
	return true;
}

static bool source_trn1u(struct sign_source *source, uint8_t *sign)
{
	if (v34_scramble_group(
		    &source->scrambler, 1u, 1u, sign) != V34_FRAMING_OK)
		return false;
	source->previous_sign = *sign;
	return true;
}

static bool source_ja(
	struct sign_source *source, uint8_t bit, uint8_t *sign)
{
	uint8_t scrambled;

	if (v34_scramble_group(
		    &source->scrambler, bit, 1u, &scrambled) !=
	    V34_FRAMING_OK)
		return false;
	source->previous_sign ^= scrambled;
	*sign = source->previous_sign;
	return true;
}

static uint8_t descriptor_bit(
	const uint8_t *descriptor, size_t bit_index)
{
	return (uint8_t)((descriptor[bit_index / 8u] >>
			  (7u - bit_index % 8u)) &
			  1u);
}

static bool prepare_decoder(
	struct v92_ja_decoder *decoder, struct sign_source *source)
{
	struct v92_phase3_sync sync;

	if (!source_init(source) ||
	    v92_phase3_sync_init(&sync) != V92_PHASE3_SYNC_OK)
		return false;
	for (size_t index = 0;
	     index < V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS; ++index) {
		uint8_t sign;
		enum v92_phase3_sync_result result;

		if (!source_trn1u(source, &sign))
			return false;
		if (index < V92_PHASE3_SYNC_MIN_TRAINING_SYMBOLS) {
			result = v92_phase3_sync_train(&sync, sign);
		} else {
			if (index == V92_PHASE3_SYNC_MIN_TRAINING_SYMBOLS &&
			    v92_phase3_sync_begin(&sync) != V92_PHASE3_SYNC_OK)
				return false;
			result = v92_phase3_sync_receive(&sync, sign);
		}
		if (result != V92_PHASE3_SYNC_OK)
			return false;
	}
	for (size_t index = 0; index < V92_PHASE3_MARKER_BITS; ++index) {
		uint8_t bit = index + 1u < V92_PHASE3_MARKER_BITS;
		uint8_t sign;
		enum v92_phase3_sync_result expected =
			index + 1u == V92_PHASE3_MARKER_BITS ?
				V92_PHASE3_SYNC_SYNCHRONIZED : V92_PHASE3_SYNC_OK;

		if (!source_ja(source, bit, &sign) ||
		    v92_phase3_sync_receive(&sync, sign) != expected)
			return false;
	}
	return v92_ja_decoder_init(decoder, &sync) == V92_JA_DECODER_OK;
}

static enum v92_ja_decoder_result feed_descriptor(
	struct v92_ja_decoder *decoder, struct sign_source *source,
	const uint8_t *descriptor, size_t first_bit, size_t corrupt_bit)
{
	enum v92_ja_decoder_result result = V92_JA_DECODER_OK;

	for (size_t bit = first_bit;
	     bit < V92_PCM_DIL_ZERO_DESCRIPTOR_BITS; ++bit) {
		uint8_t sign;

		if (!source_ja(source, descriptor_bit(descriptor, bit), &sign))
			return V92_JA_DECODER_INVALID_STATE;
		if (bit == corrupt_bit)
			sign ^= 1u;
		result = v92_ja_decoder_receive(decoder, sign);
		if (bit + 1u < V92_PCM_DIL_ZERO_DESCRIPTOR_BITS &&
		    result != V92_JA_DECODER_OK)
			return V92_JA_DECODER_INVALID_STATE;
	}
	return result;
}

static bool check_mask(uint32_t expected_mask)
{
	struct v92_ja_decoder decoder;
	struct sign_source source;
	uint8_t descriptor[V92_PCM_DIL_ZERO_DESCRIPTOR_BYTES];
	struct v92_pcm_dil_parameters decoded = { 0 };

	CHECK(prepare_decoder(&decoder, &source) &&
	      v92_pcm_dil_encode_zero(
		      expected_mask, descriptor, sizeof(descriptor)) ==
		      V92_PCM_DIL_OK &&
	      feed_descriptor(
		      &decoder, &source, descriptor,
		      V92_PHASE3_FRAME_SYNC_BITS + 1u, SIZE_MAX) ==
		      V92_JA_DECODER_DETECTED,
	      "Ja descriptor fixture failed for mask 0x%05x",
	      (unsigned int)expected_mask);
	CHECK(v92_ja_decoder_is_complete(&decoder) &&
	      v92_ja_decoder_parameters(&decoder, &decoded) &&
	      decoded.upstream_rate_mask == expected_mask,
	      "Ja descriptor decoded 0x%05x instead of 0x%05x",
	      (unsigned int)decoded.upstream_rate_mask, (unsigned int)expected_mask);
	{
		struct v92_ja_decoder before = decoder;

		CHECK(v92_ja_decoder_receive(&decoder, 0u) ==
			      V92_JA_DECODER_COMPLETE &&
		      memcmp(&decoder, &before, sizeof(decoder)) == 0,
		      "complete Ja descriptor decoder consumed another sign");
	}
	return true;
}

static bool test_generated_masks(void)
{
	CHECK(check_mask(0u), "zero Ja rate mask failed");
	CHECK(check_mask(UINT32_C(0x52491)), "sparse Ja rate mask failed");
	CHECK(check_mask(V92_PCM_DIL_RATE_MASK_ALL),
	      "full Ja rate mask failed");
	return true;
}

static bool test_repetition_recovery(void)
{
	struct v92_ja_decoder decoder;
	struct sign_source source;
	uint8_t descriptor[V92_PCM_DIL_ZERO_DESCRIPTOR_BYTES];
	const uint32_t expected_mask = UINT32_C(0x4567);
	struct v92_pcm_dil_parameters decoded = { 0 };

	CHECK(prepare_decoder(&decoder, &source) &&
	      v92_pcm_dil_encode_zero(
		      expected_mask, descriptor, sizeof(descriptor)) ==
		      V92_PCM_DIL_OK,
	      "Ja repetition fixture did not initialize");
	CHECK(feed_descriptor(
		      &decoder, &source, descriptor,
		      V92_PHASE3_FRAME_SYNC_BITS + 1u, 80u) ==
		      V92_JA_DECODER_REJECTED &&
	      !v92_ja_decoder_is_complete(&decoder) &&
	      decoder.bit_index == 0u,
	      "damaged Ja descriptor was not rejected at its boundary");
	CHECK(feed_descriptor(
		      &decoder, &source, descriptor, 0u, SIZE_MAX) ==
		      V92_JA_DECODER_DETECTED &&
	      v92_ja_decoder_parameters(&decoder, &decoded) &&
	      decoded.upstream_rate_mask == expected_mask,
	      "Ja decoder did not recover on the next repetition");
	return true;
}

static bool test_variable_descriptors(void)
{
	for (unsigned int segments = 1; segments <= 255; ++segments) {
		struct v92_ja_decoder decoder;
		struct sign_source source;
		struct v92_pcm_dil_parameters parameters = {
			.upstream_rate_mask = 0xf,
		};
		struct v92_pcm_dil_parameters decoded;
		uint8_t descriptor[V92_PCM_DIL_MAX_DESCRIPTOR_BYTES];
		size_t bits;

		CHECK(v90_dil_profile_init(&parameters.downstream,
			      V90_DIL_PROFILE_CONVENTIONAL) == V90_DIL_OK,
		      "variable Ja parameters failed");
		parameters.downstream.segment_count = (uint8_t)segments;
		memset(parameters.downstream.training_ucodes, 0,
		       sizeof(parameters.downstream.training_ucodes));
		for (unsigned int index = 0; index < segments; ++index)
			parameters.downstream.training_ucodes[index] = index % 128u;
		CHECK(prepare_decoder(&decoder, &source) &&
		      v92_pcm_dil_encode(&parameters, descriptor,
			      sizeof(descriptor), &bits) == V92_PCM_DIL_OK,
		      "variable Ja fixture failed for N=%u", segments);
		for (size_t bit = 18; bit < bits; ++bit) {
			uint8_t sign;
			CHECK(source_ja(&source, descriptor_bit(descriptor, bit), &sign) &&
			      v92_ja_decoder_receive(&decoder, sign) ==
				      (bit + 1u == bits ? V92_JA_DECODER_DETECTED :
							V92_JA_DECODER_OK),
			      "variable Ja failed for N=%u at bit %zu", segments, bit);
		}
		CHECK(v92_ja_decoder_parameters(&decoder, &decoded) &&
		      decoded.upstream_rate_mask == parameters.upstream_rate_mask &&
		      memcmp(&decoded.downstream, &parameters.downstream,
			     sizeof(parameters.downstream)) == 0,
		      "variable Ja handoff changed N=%u parameters", segments);
	}
	return true;
}

static bool test_length_recovery(void)
{
	struct v92_ja_decoder decoder;
	struct sign_source source;
	struct v92_pcm_dil_parameters decoded;
	uint8_t descriptor[V92_PCM_DIL_ZERO_DESCRIPTOR_BYTES];

	CHECK(prepare_decoder(&decoder, &source) &&
	      v92_pcm_dil_encode_zero(0xf, descriptor, sizeof(descriptor)) ==
		      V92_PCM_DIL_OK, "length recovery fixture failed");
	/* Corrupt N from zero to 255, without corrupting the coding epoch.
	 * The declared length now includes the following intact repetition. */
	for (size_t bit = 18; bit < 276; ++bit) {
		uint8_t sign;
		uint8_t value = bit < 26 ? 1u : descriptor_bit(descriptor, bit);
		CHECK(source_ja(&source, value, &sign) &&
		      v92_ja_decoder_receive(&decoder, sign) == V92_JA_DECODER_OK,
		      "corrupt length failed before the next sync at %zu", bit);
	}
	CHECK(decoder.descriptor_bits > decoder.bit_index,
	      "corrupt length did not create an incomplete candidate");
	for (size_t bit = 0; bit < 276; ++bit) {
		uint8_t sign;
		enum v92_ja_decoder_result expected = bit == 17 ?
			V92_JA_DECODER_REJECTED : bit == 275 ?
			V92_JA_DECODER_DETECTED : V92_JA_DECODER_OK;
		CHECK(source_ja(&source, descriptor_bit(descriptor, bit), &sign) &&
		      v92_ja_decoder_receive(&decoder, sign) == expected,
		      "length recovery lost the next descriptor at %zu", bit);
	}
	CHECK(v92_ja_decoder_parameters(&decoder, &decoded) &&
	      decoded.downstream.segment_count == 0 &&
	      decoded.upstream_rate_mask == 0xf,
	      "length recovery did not hand off the next repetition");
	return true;
}

static bool test_contracts(void)
{
	struct v92_ja_decoder decoder;
	struct v92_ja_decoder before;
	struct v92_phase3_sync sync;
	struct v92_pcm_dil_parameters sentinel = {
		.upstream_rate_mask = UINT32_C(0xa5a5a5a5),
	};
	struct v92_pcm_dil_parameters parameters = sentinel;

	memset(&decoder, 0xa5, sizeof(decoder));
	before = decoder;
	CHECK(v92_ja_decoder_init(NULL, NULL) ==
		      V92_JA_DECODER_INVALID_ARGUMENT &&
	      v92_phase3_sync_init(&sync) == V92_PHASE3_SYNC_OK &&
	      v92_ja_decoder_init(&decoder, &sync) ==
		      V92_JA_DECODER_INVALID_ARGUMENT &&
	      memcmp(&decoder, &before, sizeof(decoder)) == 0 &&
	      !v92_ja_decoder_is_complete(NULL) &&
	      !v92_ja_decoder_parameters(&decoder, &parameters) &&
	      memcmp(&parameters, &sentinel, sizeof(parameters)) == 0,
	      "invalid Ja decoder initialization/getter changed output");
	{
		struct sign_source source;

		CHECK(prepare_decoder(&decoder, &source),
		      "Ja decoder contract fixture did not initialize");
	}
	before = decoder;
	CHECK(v92_ja_decoder_receive(NULL, 0u) ==
		      V92_JA_DECODER_INVALID_ARGUMENT &&
	      v92_ja_decoder_receive(&decoder, 2u) ==
		      V92_JA_DECODER_INVALID_ARGUMENT &&
	      memcmp(&decoder, &before, sizeof(decoder)) == 0,
	      "invalid Ja decoder call changed state");
	decoder.bit_index = V92_PCM_DIL_MAX_DESCRIPTOR_BITS + 1u;
	before = decoder;
	CHECK(!v92_ja_decoder_is_valid(&decoder) &&
	      v92_ja_decoder_receive(&decoder, 0u) ==
		      V92_JA_DECODER_INVALID_STATE &&
	      memcmp(&decoder, &before, sizeof(decoder)) == 0,
	      "corrupt Ja decoder state was not rejected transactionally");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated masks", test_generated_masks },
		{ "repetition recovery", test_repetition_recovery },
		{ "variable descriptors", test_variable_descriptors },
		{ "corrupt length recovery", test_length_recovery },
		{ "contracts", test_contracts },
	};
	unsigned int failures = 0u;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		else {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0u ? 0 : 1;
}
