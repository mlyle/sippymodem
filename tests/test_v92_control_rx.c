// SPDX-License-Identifier: BSD-3-Clause

#include <v92_control_rx.h>
#include <packed_bits.h>

#include "test.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static uint8_t frame_bit(const uint8_t *frame, size_t bit)
{
	return (uint8_t)(
		(frame[bit / 8u] >> (7u - bit % 8u)) & 1u);
}

static void flip_bit(uint8_t *frame, size_t bit)
{
	frame[bit / 8u] ^= (uint8_t)(1u << (7u - bit % 8u));
}

static bool suvd_parameters_match(
	const struct v92_suvd_parameters *left,
	const struct v92_suvd_parameters *right)
{
	return left->request_silence == right->request_silence &&
	       left->acknowledges_cpu == right->acknowledges_cpu;
}

static struct v92_cpd_parameters cpd_selection(unsigned int sections)
{
	struct v92_cpd_parameters parameters = {
		.four_times_gain_q0_16 = UINT16_C(0x2345),
		.data_rate_number = 3u,
		.trellis = V92_CPD_TRELLIS_32_STATE,
		.extend_e2u = true,
		.acknowledges_cpu = true,
		.modulus_present = (sections & 1u) != 0u,
		.filters_present = (sections & 2u) != 0u,
		.constellations_present = (sections & 4u) != 0u,
	};

	if (parameters.modulus_present) {
		for (size_t index = 0u;
		     index < V92_CPD_MODULUS_COUNT; ++index)
			parameters.modulus[index] = (uint8_t)(index + 1u);
	}
	if (parameters.filters_present) {
		parameters.filter[V92_CPD_PRECODER_FEED_FORWARD].count = 1u;
		parameters.filter[V92_CPD_PRECODER_FEED_FORWARD]
			.coefficient[0] = INT16_MAX;
		parameters.filter[V92_CPD_PREFILTER_FEED_FORWARD].count = 1u;
		parameters.filter[V92_CPD_PREFILTER_FEED_FORWARD]
			.coefficient[0] = INT16_MIN;
	}
	if (parameters.constellations_present) {
		parameters.constellation[0].count = 3u;
		parameters.constellation[0].positive_linear[0] = 128;
		parameters.constellation[0].positive_linear[1] = 512;
		parameters.constellation[0].positive_linear[2] = 2048;
	}
	return parameters;
}

static bool cpd_parameters_match(
	const struct v92_cpd_parameters *left,
	const struct v92_cpd_parameters *right)
{
	if (left->four_times_gain_q0_16 != right->four_times_gain_q0_16 ||
	    left->data_rate_number != right->data_rate_number ||
	    left->trellis != right->trellis ||
	    left->extend_e2u != right->extend_e2u ||
	    left->acknowledges_cpu != right->acknowledges_cpu ||
	    left->modulus_present != right->modulus_present ||
	    left->filters_present != right->filters_present ||
	    left->constellations_present != right->constellations_present ||
	    memcmp(left->modulus, right->modulus, sizeof(left->modulus)) != 0 ||
	    memcmp(left->frame_constellation, right->frame_constellation,
		   sizeof(left->frame_constellation)) != 0)
		return false;
	for (size_t section = 0u;
	     section < V92_CPD_FILTER_SECTION_COUNT; ++section) {
		if (left->filter[section].count != right->filter[section].count ||
		    memcmp(left->filter[section].coefficient,
			   right->filter[section].coefficient,
			   sizeof(left->filter[section].coefficient)) != 0)
			return false;
	}
	for (size_t set = 0u; set < V92_CPD_MAX_CONSTELLATIONS; ++set) {
		if (left->constellation[set].count !=
			    right->constellation[set].count ||
		    memcmp(left->constellation[set].positive_linear,
			   right->constellation[set].positive_linear,
			   sizeof(left->constellation[set].positive_linear)) != 0)
			return false;
	}
	return true;
}

static bool receive_bit(
	struct v92_control_rx *receiver, uint8_t bit,
	struct v92_control_event *event)
{
	CHECK(v92_control_rx_receive_bit(receiver, bit, event) ==
		      V92_CONTROL_RX_OK &&
	      v92_control_rx_is_valid(receiver),
	      "control receiver rejected bit %u", bit);
	return true;
}

static bool feed_frame(
	struct v92_control_rx *receiver,
	const uint8_t *frame, size_t first_bit, size_t bit_count,
	struct v92_control_event *last_event)
{
	struct v92_control_event observed = { 0 };

	for (size_t bit = first_bit; bit < bit_count; ++bit) {
		struct v92_control_event event;

		CHECK(receive_bit(receiver, frame_bit(frame, bit), &event),
		      "control frame stopped at bit %zu", bit);
		if (event.type == V92_CONTROL_EVENT_NONE)
			continue;
		CHECK(observed.type == V92_CONTROL_EVENT_NONE,
		      "control frame emitted multiple events");
		observed = event;
	}
	*last_event = observed;
	return true;
}

static bool test_all_frame_widths(void)
{
	for (uint8_t width = V92_STATUS_UPDATE_DOWNSTREAM_FRAME_MIN_BITS;
	     width <= V92_STATUS_UPDATE_DOWNSTREAM_FRAME_MAX_BITS; ++width) {
		for (unsigned int flags = 0u; flags < 4u; ++flags) {
			struct v92_suvd_parameters parameters = {
				.request_silence = (flags & 1u) != 0u,
				.acknowledges_cpu = (flags & 2u) != 0u,
			};
			struct v92_control_rx receiver;
			struct v92_control_event event;
			uint8_t frame[V92_SUVD_MAX_FRAME_BYTES];
			size_t bit_count = 0u;

			CHECK(v92_suvd_encode(
				      &parameters, width, frame, sizeof(frame),
				      &bit_count) == V92_SUVD_OK &&
			      v92_control_rx_init(&receiver, width) ==
				      V92_CONTROL_RX_OK,
			      "SUVd width %u fixture did not initialize", width);
			for (size_t bit = 0u; bit < 47u; ++bit)
				CHECK(receive_bit(&receiver, 1u, &event) &&
				      event.type == V92_CONTROL_EVENT_NONE,
				      "TRN2d suffix emitted an event");
			CHECK(feed_frame(
				      &receiver, frame, 0u, bit_count, &event) &&
			      event.type == V92_CONTROL_EVENT_SUVD &&
			      event.record.suvd.frame_result == V92_SUVD_OK &&
			      suvd_parameters_match(
				      &event.record.suvd.parameters, &parameters) &&
			      receiver.stage == V92_CONTROL_RX_SEARCH_SYNC &&
			      receiver.sync_one_count == 0u,
			      "SUVd flags 0x%x width %u did not decode",
			      flags, width);
		}
	}
	return true;
}

static bool test_cpd_matrix(void)
{
	unsigned int cases = 0u;

	for (uint8_t width = V92_CPD_DOWNSTREAM_FRAME_MIN_BITS;
	     width <= V92_CPD_DOWNSTREAM_FRAME_MAX_BITS; ++width) {
		for (unsigned int sections = 0u; sections < 8u; ++sections) {
			struct v92_cpd_parameters parameters =
				cpd_selection(sections);
			struct v92_control_rx receiver;
			struct v92_control_event event;
			uint8_t frame[V92_CPD_MAX_FRAME_BYTES];
			size_t bit_count = 0u;

			CHECK(v92_cpd_encode(
				      &parameters, width, frame, sizeof(frame),
				      &bit_count) == V92_CPD_OK &&
			      v92_control_rx_init(&receiver, width) ==
				      V92_CONTROL_RX_OK,
			      "CPd sections 0x%x width %u did not initialize",
			      sections, width);
			for (size_t bit = 0u; bit < 47u; ++bit)
				CHECK(receive_bit(&receiver, 1u, &event) &&
				      event.type == V92_CONTROL_EVENT_NONE,
				      "TRN2d suffix emitted a CPd event");
			CHECK(feed_frame(
				      &receiver, frame, 0u, bit_count, &event) &&
			      event.type == V92_CONTROL_EVENT_CPD &&
			      event.record.cpd.frame_result == V92_CPD_OK &&
			      cpd_parameters_match(
				      &event.record.cpd.parameters, &parameters) &&
			      receiver.stage == V92_CONTROL_RX_SEARCH_SYNC &&
			      receiver.frame_bit_count == 0u &&
			      receiver.sync_one_count == 0u,
			      "CPd sections 0x%x width %u did not decode",
			      sections, width);
			cases++;
		}
	}
	CHECK(cases == 176u, "generated %u CPd receiver cases", cases);
	return true;
}

static bool test_recovery_and_rejection(void)
{
	const struct v92_suvd_parameters parameters = {
		.request_silence = true,
		.acknowledges_cpu = true,
	};
	struct v92_control_rx receiver;
	struct v92_control_event event;
	struct v92_cpd_parameters valid_cpd = cpd_selection(7u);
	struct v92_cpd_parameters cpd;
	uint8_t frame[V92_SUVD_MAX_FRAME_BYTES];
	uint8_t corrupt[V92_SUVD_MAX_FRAME_BYTES];
	uint8_t cpd_frame[V92_CPD_MAX_FRAME_BYTES];
	uint8_t cpd_corrupt[V92_CPD_MAX_FRAME_BYTES];
	size_t bit_count = 0u;
	size_t cpd_bits = 0u;
	size_t valid_cpd_bits;
	const uint8_t width = 17u;

	CHECK(v92_suvd_encode(
		      &parameters, width, frame, sizeof(frame), &bit_count) ==
		      V92_SUVD_OK &&
	      v92_control_rx_init(&receiver, width) ==
		      V92_CONTROL_RX_OK,
	      "SUVd recovery fixture did not initialize");
	memcpy(corrupt, frame, sizeof(corrupt));
	flip_bit(corrupt, 35u);
	CHECK(feed_frame(&receiver, corrupt, 0u, bit_count, &event) &&
	      event.type == V92_CONTROL_EVENT_SUVD_REJECTED &&
	      event.record.suvd.frame_result == V92_SUVD_CRC_MISMATCH,
	      "corrupt SUVd did not emit a CRC rejection");
	CHECK(feed_frame(&receiver, frame, 0u, 31u, &event) &&
	      event.type == V92_CONTROL_EVENT_NONE &&
	      feed_frame(&receiver, frame, 0u, bit_count, &event) &&
	      event.type == V92_CONTROL_EVENT_SUVD &&
	      suvd_parameters_match(
		      &event.record.suvd.parameters, &parameters),
	      "newer SUVd prefix did not replace a partial candidate");
	for (size_t bit = 0u; bit < 17u; ++bit)
		CHECK(receive_bit(&receiver, 1u, &event),
		      "non-status prefix stopped at bit %zu", bit);
	CHECK(receive_bit(&receiver, 0u, &event) &&
	      event.type == V92_CONTROL_EVENT_NONE &&
	      receive_bit(&receiver, 0u, &event) &&
	      event.type == V92_CONTROL_EVENT_NONE &&
	      feed_frame(&receiver, frame, 0u, bit_count, &event) &&
	      event.type == V92_CONTROL_EVENT_SUVD,
	      "partial CPd candidate prevented SUVd recovery");

	CHECK(v92_cpd_encode(
		      &valid_cpd, width, cpd_frame, sizeof(cpd_frame),
		      &cpd_bits) == V92_CPD_OK,
	      "CPd recovery fixture did not encode");
	valid_cpd_bits = cpd_bits;
	memcpy(cpd_corrupt, cpd_frame, sizeof(cpd_corrupt));
	flip_bit(cpd_corrupt, 22u);
	CHECK(feed_frame(
		      &receiver, cpd_corrupt, 0u, cpd_bits, &event) &&
	      event.type == V92_CONTROL_EVENT_CPD_REJECTED &&
	      event.record.cpd.frame_result == V92_CPD_CRC_MISMATCH,
	      "corrupt CPd did not emit a CRC rejection");
	cpd = cpd_selection(2u);
	CHECK(v92_cpd_encode(
		      &cpd, width, cpd_corrupt, sizeof(cpd_corrupt),
		      &cpd_bits) == V92_CPD_OK,
	      "CPd bound fixture did not encode");
	packed_msb_write_lsb_field(
		cpd_corrupt, V92_CPD_FIXED_BITS + 1u, 9u,
		V92_CPD_MAX_SECTION_COEFFICIENTS + 1u);
	CHECK(feed_frame(
		      &receiver, cpd_corrupt, 0u, cpd_bits, &event) &&
	      event.type == V92_CONTROL_EVENT_CPD_REJECTED &&
	      event.record.cpd.frame_result == V92_CPD_INVALID_FRAME &&
	      feed_frame(
		      &receiver, cpd_frame, 0u, valid_cpd_bits, &event) &&
	      event.type == V92_CONTROL_EVENT_CPD &&
	      cpd_parameters_match(
		      &event.record.cpd.parameters, &valid_cpd),
	      "malformed CPd prevented control-stream recovery");
	return true;
}

static bool test_contracts(void)
{
	struct v92_control_rx receiver;
	struct v92_control_rx before;
	struct v92_control_event event;
	struct v92_control_event event_before;
	static const char *const names[] = {
		"ok", "invalid argument", "invalid parameters",
		"invalid state", "internal error",
	};

	memset(&receiver, 0xa5, sizeof(receiver));
	before = receiver;
	CHECK(v92_control_rx_init(NULL, 17u) ==
		      V92_CONTROL_RX_INVALID_ARGUMENT &&
	      v92_control_rx_init(&receiver, 8u) ==
		      V92_CONTROL_RX_INVALID_PARAMETERS &&
	      v92_control_rx_init(&receiver, 31u) ==
		      V92_CONTROL_RX_INVALID_PARAMETERS &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0,
	      "invalid control receiver initialization changed state");
	CHECK(v92_control_rx_init(&receiver, 17u) ==
		      V92_CONTROL_RX_OK,
	      "control receiver did not initialize");
	before = receiver;
	memset(&event, 0xa5, sizeof(event));
	event_before = event;
	CHECK(v92_control_rx_receive_bit(NULL, 0u, &event) ==
		      V92_CONTROL_RX_INVALID_ARGUMENT &&
	      v92_control_rx_receive_bit(&receiver, 0u, NULL) ==
		      V92_CONTROL_RX_INVALID_ARGUMENT &&
	      v92_control_rx_receive_bit(&receiver, 2u, &event) ==
		      V92_CONTROL_RX_INVALID_ARGUMENT &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0 &&
	      memcmp(&event, &event_before, sizeof(event)) == 0,
	      "invalid control receiver call changed state or event");
	receiver.bit_index = receiver.frame_bit_count + 1u;
	before = receiver;
	CHECK(!v92_control_rx_is_valid(NULL) &&
	      !v92_control_rx_is_valid(&receiver) &&
	      v92_control_rx_receive_bit(&receiver, 0u, &event) ==
		      V92_CONTROL_RX_INVALID_STATE &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0 &&
	      memcmp(&event, &event_before, sizeof(event)) == 0,
	      "invalid control receiver state was not transactional");
	for (size_t index = 0u; index < ARRAY_LENGTH(names); ++index)
		CHECK(strcmp(
			      v92_control_rx_result_name(
				      (enum v92_control_rx_result)index),
			      names[index]) == 0,
		      "control receiver result name %zu changed", index);
	CHECK(strcmp(
		      v92_control_rx_result_name(
			      (enum v92_control_rx_result)-1),
		      "unknown") == 0,
	      "unknown control receiver result name changed");
	return true;
}

int main(void)
{
	fprintf(stderr, "[ RUN      ] V.92 downstream control receiver\n");
	if (!test_all_frame_widths() || !test_cpd_matrix() ||
	    !test_recovery_and_rejection() ||
	    !test_contracts()) {
		fprintf(stderr, "[  FAILED  ] V.92 downstream control receiver\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] V.92 downstream control receiver\n");
	return 0;
}
