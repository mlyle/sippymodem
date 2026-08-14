// SPDX-License-Identifier: BSD-3-Clause

#include <v92_control_rx.h>

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

static bool parameters_match(
	const struct v92_suvd_parameters *left,
	const struct v92_suvd_parameters *right)
{
	return left->request_silence == right->request_silence &&
	       left->acknowledges_cpu == right->acknowledges_cpu;
}

static bool receive_bit(
	struct v92_control_rx *receiver, uint8_t bit,
	struct v92_control_event *event)
{
	CHECK(v92_control_rx_receive_bit(receiver, bit, event) ==
		      V92_CONTROL_RX_OK &&
	      v92_control_rx_is_valid(receiver),
	      "SUVd receiver rejected bit %u", bit);
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
		      "SUVd frame stopped at bit %zu", bit);
		if (event.type == V92_CONTROL_EVENT_NONE)
			continue;
		CHECK(observed.type == V92_CONTROL_EVENT_NONE,
		      "SUVd frame emitted multiple events");
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
			      event.frame_result == V92_SUVD_OK &&
			      parameters_match(&event.parameters, &parameters) &&
			      receiver.stage == V92_CONTROL_RX_SEARCH_SYNC &&
			      receiver.sync_one_count == 0u,
			      "SUVd flags 0x%x width %u did not decode",
			      flags, width);
		}
	}
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
	uint8_t frame[V92_SUVD_MAX_FRAME_BYTES];
	uint8_t corrupt[V92_SUVD_MAX_FRAME_BYTES];
	size_t bit_count = 0u;
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
	      event.type == V92_CONTROL_EVENT_REJECTED &&
	      event.frame_result == V92_SUVD_CRC_MISMATCH,
	      "corrupt SUVd did not emit a CRC rejection");
	CHECK(feed_frame(&receiver, frame, 0u, 31u, &event) &&
	      event.type == V92_CONTROL_EVENT_NONE &&
	      feed_frame(&receiver, frame, 0u, bit_count, &event) &&
	      event.type == V92_CONTROL_EVENT_SUVD &&
	      parameters_match(&event.parameters, &parameters),
	      "newer SUVd prefix did not replace a partial candidate");
	for (size_t bit = 0u; bit < 17u; ++bit)
		CHECK(receive_bit(&receiver, 1u, &event),
		      "non-status prefix stopped at bit %zu", bit);
	CHECK(receive_bit(&receiver, 0u, &event) &&
	      event.type == V92_CONTROL_EVENT_NONE &&
	      receive_bit(&receiver, 0u, &event) &&
	      event.type == V92_CONTROL_EVENT_REJECTED &&
	      event.frame_result == V92_SUVD_INVALID_FRAME &&
	      feed_frame(&receiver, frame, 0u, bit_count, &event) &&
	      event.type == V92_CONTROL_EVENT_SUVD,
	      "non-status control marker prevented recovery");
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
	      "invalid SUVd receiver initialization changed state");
	CHECK(v92_control_rx_init(&receiver, 17u) ==
		      V92_CONTROL_RX_OK,
	      "SUVd contract receiver did not initialize");
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
	      "invalid SUVd receiver call changed state or event");
	receiver.bit_index = receiver.frame_bit_count + 1u;
	before = receiver;
	CHECK(!v92_control_rx_is_valid(NULL) &&
	      !v92_control_rx_is_valid(&receiver) &&
	      v92_control_rx_receive_bit(&receiver, 0u, &event) ==
		      V92_CONTROL_RX_INVALID_STATE &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0 &&
	      memcmp(&event, &event_before, sizeof(event)) == 0,
	      "invalid SUVd receiver state was not transactional");
	for (size_t index = 0u; index < ARRAY_LENGTH(names); ++index)
		CHECK(strcmp(
			      v92_control_rx_result_name(
				      (enum v92_control_rx_result)index),
			      names[index]) == 0,
		      "SUVd receiver result name %zu changed", index);
	CHECK(strcmp(
		      v92_control_rx_result_name(
			      (enum v92_control_rx_result)-1),
		      "unknown") == 0,
	      "unknown SUVd receiver result name changed");
	return true;
}

int main(void)
{
	fprintf(stderr, "[ RUN      ] V.92 incremental SUVd receiver\n");
	if (!test_all_frame_widths() || !test_recovery_and_rejection() ||
	    !test_contracts()) {
		fprintf(stderr, "[  FAILED  ] V.92 incremental SUVd receiver\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] V.92 incremental SUVd receiver\n");
	return 0;
}
