// SPDX-License-Identifier: BSD-3-Clause

#include <constellation.h>
#include <trellis_acquirer.h>

#include "test.h"

#include <string.h>

enum {
	KNOWN_SYMBOLS = 128,
	PAYLOAD_SYMBOLS = 96,
};

static struct qam_point multiply(
	struct qam_point left,
	struct qam_point right)
{
	return (struct qam_point){
		.in_phase = left.in_phase * right.in_phase -
			    left.quadrature * right.quadrature,
		.quadrature = left.in_phase * right.quadrature +
			      left.quadrature * right.in_phase,
	};
}

static bool encode_point(
	struct v32_symbol_encoder *framing,
	struct v32_trellis_encoder *trellis, uint8_t input,
	struct qam_point *point)
{
	uint8_t scrambled;
	uint8_t symbol;

	return v32_scramble_group(
		       &framing->scrambler, input,
		       trellis->data_bits_per_symbol, &scrambled) ==
		       V32_FRAMING_OK &&
	       v32_trellis_encode(trellis, scrambled, &symbol) ==
		       V32_TRELLIS_OK &&
	       v32_trellis_constellation_point(
		       trellis->data_bits_per_symbol, symbol, point) ==
		       V32_CONSTELLATION_OK;
}

static bool check_phase(uint8_t phase, uint8_t data_bits)
{
	static const struct qam_point channel = { 0.73, -0.31 };
	struct v32_symbol_encoder framing_encoder;
	struct v32_symbol_decoder framing_decoder;
	struct v32_trellis_encoder trellis_encoder;
	struct v32_trellis_acquirer acquirer;
	struct qam_point reference;
	uint8_t payload[PAYLOAD_SYMBOLS];
	uint8_t previous_output;
	uint8_t mask = (uint8_t)((UINT8_C(1) << data_bits) - 1u);

	CHECK(v32_symbol_encoder_init(
		      &framing_encoder, V32_ANSWERING_TRANSMITTER,
		      phase) == V32_FRAMING_OK &&
	      v32_symbol_decoder_init(
		      &framing_decoder, V32_ANSWERING_TRANSMITTER,
		      0) == V32_FRAMING_OK &&
	      v32_phase_to_differential_output(
		      phase, &previous_output) == V32_FRAMING_OK &&
	      v32_trellis_encoder_init(
		      &trellis_encoder, data_bits, previous_output) ==
		      V32_TRELLIS_OK &&
	      v32_4800_constellation_point(phase, &reference) ==
		      V32_CONSTELLATION_OK,
	      "V.32bis phase %u/%u fixture did not initialize", phase,
	      data_bits);
	reference = multiply(channel, reference);
	CHECK(v32_trellis_acquirer_init(
		      &acquirer, &framing_decoder, &reference, data_bits) ==
		      V32_TRELLIS_ACQUIRER_OK,
	      "V.32bis phase %u/%u acquisition did not initialize", phase,
	      data_bits);
	for (size_t index = 0; index < KNOWN_SYMBOLS; ++index) {
		struct qam_point point;

		CHECK(encode_point(
			      &framing_encoder, &trellis_encoder, mask, &point),
		      "V.32bis phase %u/%u known symbol %zu generation failed",
		      phase, data_bits, index);
		point = multiply(channel, point);
		CHECK(
		      v32_trellis_acquirer_track_known(
			      &acquirer, &point, mask) ==
			      V32_TRELLIS_ACQUIRER_OK,
		      "V.32bis phase %u/%u known symbol %zu failed", phase,
		      data_bits, index);
	}
	CHECK(v32_trellis_acquirer_select(&acquirer) ==
		      V32_TRELLIS_ACQUIRER_OK &&
	      acquirer.selected && acquirer.selected_phase == phase,
	      "V.32bis phase %u/%u selected hypothesis %u", phase,
	      data_bits, acquirer.selected_phase);
	for (size_t index = 0; index < PAYLOAD_SYMBOLS; ++index) {
		struct qam_point point;
		uint8_t output = UINT8_MAX;
		bool ready = true;

		payload[index] = (uint8_t)((index * 37u + 11u) & mask);
		CHECK(encode_point(
			      &framing_encoder, &trellis_encoder, payload[index],
			      &point),
		      "V.32bis phase %u/%u payload generation failed at %zu",
		      phase, data_bits, index);
		point = multiply(channel, point);
		CHECK(v32_trellis_acquirer_decode(
			      &acquirer, &point, &output, &ready) ==
			      V32_TRELLIS_ACQUIRER_OK,
		      "V.32bis phase %u/%u payload decode failed at %zu", phase,
		      data_bits, index);
		if (index + 1u < V32_TRELLIS_RECEIVER_DELAY) {
			CHECK(!ready && output == UINT8_MAX,
			      "V.32bis phase acquisition exposed delayed payload");
		} else {
			size_t expected = index + 1u -
					  V32_TRELLIS_RECEIVER_DELAY;

			CHECK(ready && output == payload[expected],
			      "V.32bis phase %u/%u decoded %u instead of %u",
			      phase, data_bits, output, payload[expected]);
		}
	}
	return true;
}

static bool test_generated_phase_acquisition(void)
{
	static const uint8_t widths[] = {
		V32_7200_DATA_BITS,
		V32_9600_TRELLIS_DATA_BITS,
		V32_12000_DATA_BITS,
		V32_14400_DATA_BITS,
	};

	for (uint8_t phase = 0; phase < QPSK_QUADRANT_COUNT;
	     ++phase)
		for (size_t width = 0; width < ARRAY_LENGTH(widths); ++width)
			if (!check_phase(phase, widths[width]))
				return false;
	return true;
}

static bool test_api_contract(void)
{
	struct v32_symbol_decoder framing;
	struct v32_trellis_acquirer acquirer;
	struct v32_trellis_acquirer saved;
	struct qam_point point = { 1.0, 1.0 };
	uint8_t output = UINT8_MAX;
	bool ready = true;

	CHECK(v32_symbol_decoder_init(
		      &framing, V32_CALLING_TRANSMITTER, 0) ==
		      V32_FRAMING_OK &&
	      v32_trellis_acquirer_init(
		      &acquirer, &framing, &point,
		      V32_7200_DATA_BITS) ==
		      V32_TRELLIS_ACQUIRER_OK,
	      "V.32bis acquisition API fixture did not initialize");
	saved = acquirer;
	CHECK(v32_trellis_acquirer_select(&acquirer) ==
		      V32_TRELLIS_ACQUIRER_NOT_READY &&
	      v32_trellis_acquirer_decode(
		      &acquirer, &point, &output, &ready) ==
		      V32_TRELLIS_ACQUIRER_NOT_READY &&
	      v32_trellis_acquirer_track_known(
		      &acquirer, NULL, 7) ==
		      V32_TRELLIS_ACQUIRER_INVALID_ARGUMENT &&
	      memcmp(&acquirer, &saved, sizeof(acquirer)) == 0 &&
	      output == UINT8_MAX && ready,
	      "V.32bis acquisition API failure changed state or output");
	CHECK(strcmp(
		      v32_trellis_acquirer_result_name(
			      V32_TRELLIS_ACQUIRER_NOT_READY),
		      "not ready") == 0 &&
	      !v32_trellis_acquirer_is_valid(NULL),
	      "V.32bis acquisition API names or validation changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated phase acquisition", test_generated_phase_acquisition },
		{ "API contract", test_api_contract },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (!tests[index].run()) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
