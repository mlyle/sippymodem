// SPDX-License-Identifier: BSD-3-Clause

#include <v34_phase2_dpsk.h>

#include "test.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_AMPLITUDE = 12000,
	TEST_BIT_COUNT = 49,
	TEST_DECODED_CAPACITY = 96,
	TEST_SYNC_BITS = 12,
};

static const double TEST_PI = 3.1415926535897932384626433832795;

static double tone_projection(
	const int16_t *samples, size_t count, uint32_t frequency_hz)
{
	double in_phase = 0.0;
	double quadrature = 0.0;

	for (size_t index = 0; index < count; ++index) {
		double angle = 2.0 * TEST_PI * (double)frequency_hz *
			       (double)index /
			       V34_PHASE2_SAMPLE_RATE;

		in_phase += samples[index] * cos(angle);
		quadrature += samples[index] * sin(angle);
	}
	return in_phase * in_phase + quadrature * quadrature;
}

static bool render_station(
	enum v34_station_role role,
	int16_t samples[V34_PHASE2_SAMPLE_RATE],
	unsigned int *accepted_count)
{
	struct v34_phase2_dpsk_transmitter transmitter;

	CHECK(v34_phase2_dpsk_transmitter_init(
		      &transmitter, role, TEST_AMPLITUDE) ==
		      V34_PHASE2_DPSK_OK,
	      "Phase 2 transmitter did not initialize");
	*accepted_count = 0;
	for (size_t index = 0;
	     index < V34_PHASE2_SAMPLE_RATE; ++index) {
		bool accepted;

		CHECK(v34_phase2_dpsk_transmitter_sample(
			      &transmitter, 0, &samples[index], &accepted) ==
			      V34_PHASE2_DPSK_OK,
		      "Phase 2 transmitter failed at sample %zu", index);
		if (accepted)
			(*accepted_count)++;
	}
	CHECK(!transmitter.initial_point_pending,
	      "Phase 2 transmitter omitted its reference point");
	if (role == V34_CALLING_STATION) {
		CHECK(transmitter.carrier.amplitude == TEST_AMPLITUDE &&
			      transmitter.guard_amplitude == 0,
		      "calling transmitter changed its nominal level");
	} else {
		CHECK(transmitter.carrier.amplitude >= 10690 &&
			      transmitter.carrier.amplitude <= 10700 &&
			      transmitter.guard_amplitude >= 5355 &&
			      transmitter.guard_amplitude <= 5365,
		      "answering transmitter changed its relative levels");
	}
	return true;
}

static bool test_carriers_and_cadence(void)
{
	int16_t calling[V34_PHASE2_SAMPLE_RATE];
	int16_t answering[V34_PHASE2_SAMPLE_RATE];
	const size_t skip = V34_PHASE2_SAMPLE_RATE / 4;
	const size_t count = ARRAY_LENGTH(calling) - skip;
	unsigned int calling_accepted;
	unsigned int answering_accepted;

	CHECK(render_station(
		      V34_CALLING_STATION, calling,
		      &calling_accepted) &&
		      render_station(
			      V34_ANSWERING_STATION,
			      answering, &answering_accepted),
	      "Phase 2 waveform fixture failed");
	CHECK(calling_accepted ==
		      V34_PHASE2_DPSK_BIT_RATE - 1 &&
		      answering_accepted ==
			      V34_PHASE2_DPSK_BIT_RATE - 1,
	      "Phase 2 symbol clock drifted over one second");
	CHECK(tone_projection(
		      calling + skip, count,
		      V34_PHASE2_CALLING_CARRIER_HZ) >
		      tone_projection(
			      calling + skip, count,
			      V34_PHASE2_ANSWERING_CARRIER_HZ) *
			      1000.0,
	      "calling transmitter did not retain its carrier");
	CHECK(tone_projection(
		      answering + skip, count,
		      V34_PHASE2_ANSWERING_CARRIER_HZ) >
		      tone_projection(
			      answering + skip, count,
			      V34_PHASE2_CALLING_CARRIER_HZ) *
			      1000.0 &&
		      tone_projection(
			      answering + skip, count,
			      V34_PHASE2_ANSWERING_GUARD_HZ) >
			      tone_projection(
				      answering + skip, count,
				      V34_PHASE2_CALLING_CARRIER_HZ) *
				      1000.0,
	      "answering transmitter omitted its carrier or guard tone");
	return true;
}

static uint32_t fixture_random(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static void build_bits(uint8_t bits[TEST_BIT_COUNT])
{
	uint32_t random = UINT32_C(0x63c97a51);
	static const uint8_t info_prefix[TEST_SYNC_BITS] = {
		1, 1, 1, 1, 0, 1, 1, 1, 0, 0, 1, 0,
	};

	memcpy(bits, info_prefix, sizeof(info_prefix));
	for (size_t index = TEST_SYNC_BITS; index < TEST_BIT_COUNT; ++index)
		bits[index] = (uint8_t)(fixture_random(&random) >> 31);
}

static size_t find_pattern(
	const uint8_t *haystack, size_t haystack_count,
	const uint8_t *needle, size_t needle_count)
{
	if (needle_count > haystack_count)
		return SIZE_MAX;
	for (size_t offset = 0; offset <= haystack_count - needle_count;
	     ++offset)
		if (memcmp(haystack + offset, needle, needle_count) == 0)
			return offset;
	return SIZE_MAX;
}

static bool check_loopback(
	enum v34_station_role transmit_role,
	size_t leading_silence, unsigned int noise_peak)
{
	enum v34_station_role receive_role =
		transmit_role == V34_CALLING_STATION ?
			V34_ANSWERING_STATION :
			V34_CALLING_STATION;
	struct v34_phase2_dpsk_transmitter transmitter;
	struct v34_phase2_dpsk_receiver receiver;
	struct v34_phase2_dpsk_measurement measurement;
	uint8_t bits[TEST_BIT_COUNT];
	uint8_t decoded[TEST_DECODED_CAPACITY];
	size_t accepted_count = 0;
	size_t decoded_count = 0;
	size_t sync_offset;
	uint32_t noise = UINT32_C(0xd7a41c29);
	const size_t sample_limit =
		(TEST_BIT_COUNT + 24u) *
		V34_PHASE2_SAMPLE_RATE /
		V34_PHASE2_DPSK_BIT_RATE;

	build_bits(bits);
	CHECK(v34_phase2_dpsk_transmitter_init(
		      &transmitter, transmit_role, TEST_AMPLITUDE) ==
		      V34_PHASE2_DPSK_OK &&
		      v34_phase2_dpsk_receiver_init(
			      &receiver, receive_role) ==
			      V34_PHASE2_DPSK_OK,
	      "Phase 2 loopback endpoints did not initialize");
	for (size_t index = 0; index < leading_silence; ++index) {
		uint8_t received_bit;
		bool ready;

		CHECK(v34_phase2_dpsk_receiver_sample(
			      &receiver, 0, &received_bit, &ready) ==
			      V34_PHASE2_DPSK_OK &&
			      !ready,
		      "Phase 2 receiver emitted a bit during leading silence");
	}
	for (size_t index = 0; index < sample_limit; ++index) {
		uint8_t transmit_bit = accepted_count < TEST_BIT_COUNT ?
					       bits[accepted_count] :
					       0;
		uint8_t received_bit;
		int16_t sample;
		bool accepted;
		bool ready;

		CHECK(v34_phase2_dpsk_transmitter_sample(
			      &transmitter, transmit_bit, &sample, &accepted) ==
			      V34_PHASE2_DPSK_OK,
		      "Phase 2 transmitter failed at sample %zu", index);
		if (noise_peak != 0) {
			int32_t perturbation = (int32_t)(
				fixture_random(&noise) % (2u * noise_peak + 1u)) -
				(int32_t)noise_peak;

			sample = (int16_t)((int32_t)sample + perturbation);
		}
		CHECK(v34_phase2_dpsk_receiver_sample(
				      &receiver, sample, &received_bit, &ready) ==
				      V34_PHASE2_DPSK_OK,
		      "Phase 2 loopback failed at sample %zu", index);
		if (accepted && accepted_count < TEST_BIT_COUNT)
			accepted_count++;
		if (ready) {
			CHECK(decoded_count < ARRAY_LENGTH(decoded),
			      "Phase 2 receiver produced unbounded bits");
			decoded[decoded_count++] = received_bit;
		}
	}
	CHECK(accepted_count == TEST_BIT_COUNT,
	      "Phase 2 transmitter did not consume its bit fixture");
	sync_offset = find_pattern(
		decoded, decoded_count, bits, TEST_SYNC_BITS);
	CHECK(sync_offset != SIZE_MAX &&
		      sync_offset + TEST_BIT_COUNT <= decoded_count &&
		      memcmp(decoded + sync_offset, bits, TEST_BIT_COUNT) == 0,
	      "Phase 2 receiver did not recover one short INFO sequence");
	measurement = v34_phase2_dpsk_receiver_measure(&receiver);
	CHECK(measurement.timing_locked && measurement.line_power > 1.0 &&
		      measurement.symbol_power > 1.0,
	      "Phase 2 receiver did not retain carrier measurements");
	return true;
}

static bool test_bidirectional_loopback(void)
{
	return check_loopback(
		       V34_CALLING_STATION, 0, 0) &&
	       check_loopback(
		       V34_ANSWERING_STATION, 0, 0);
}

static bool test_receive_tolerance(void)
{
	return check_loopback(
		       V34_CALLING_STATION, 7, 350) &&
	       check_loopback(
		       V34_ANSWERING_STATION, 11, 350);
}

static bool test_validation(void)
{
	struct v34_phase2_dpsk_transmitter transmitter;
	struct v34_phase2_dpsk_receiver receiver;
	unsigned char snapshot[sizeof(transmitter)];
	struct v34_phase2_dpsk_measurement measurement;
	int16_t sample = 123;
	uint8_t bit = 7;
	bool ready = true;

	CHECK(v34_phase2_dpsk_transmitter_init(
		      NULL, V34_CALLING_STATION,
		      TEST_AMPLITUDE) ==
		      V34_PHASE2_DPSK_INVALID_ARGUMENT &&
		      v34_phase2_dpsk_transmitter_init(
			      &transmitter,
			      (enum v34_station_role)99,
			      TEST_AMPLITUDE) ==
			      V34_PHASE2_DPSK_INVALID_ARGUMENT &&
		      v34_phase2_dpsk_transmitter_init(
			      &transmitter,
			      V34_ANSWERING_STATION,
			      30000) ==
			      V34_PHASE2_DPSK_INVALID_ARGUMENT,
	      "Phase 2 transmitter accepted an invalid configuration");
	CHECK(v34_phase2_dpsk_transmitter_init(
		      &transmitter, V34_CALLING_STATION,
		      TEST_AMPLITUDE) == V34_PHASE2_DPSK_OK,
	      "Phase 2 validation fixture did not initialize");
	memcpy(snapshot, &transmitter, sizeof(snapshot));
	CHECK(v34_phase2_dpsk_transmitter_sample(
		      &transmitter, 2, &sample, &ready) ==
		      V34_PHASE2_DPSK_INVALID_ARGUMENT &&
		      memcmp(snapshot, &transmitter, sizeof(snapshot)) == 0 &&
		      sample == 123 && ready,
	      "Phase 2 transmitter mutated state after invalid input");
	CHECK(v34_phase2_dpsk_receiver_init(
		      NULL, V34_CALLING_STATION) ==
		      V34_PHASE2_DPSK_INVALID_ARGUMENT &&
		      v34_phase2_dpsk_receiver_init(
			      &receiver,
			      V34_CALLING_STATION) ==
			      V34_PHASE2_DPSK_OK &&
		      v34_phase2_dpsk_receiver_sample(
			      &receiver, 0, NULL, &ready) ==
			      V34_PHASE2_DPSK_INVALID_ARGUMENT &&
		      v34_phase2_dpsk_receiver_reset(&receiver) ==
			      V34_PHASE2_DPSK_OK,
	      "Phase 2 receiver validation or reset failed");
	measurement = v34_phase2_dpsk_receiver_measure(&receiver);
	CHECK(!measurement.timing_locked && measurement.line_power == 0.0 &&
		      measurement.symbol_power == 0.0,
	      "Phase 2 receiver reset retained measurements");
	receiver.detector_output = NAN;
	measurement = v34_phase2_dpsk_receiver_measure(&receiver);
	CHECK(v34_phase2_dpsk_receiver_sample(
		      &receiver, 0, &bit, &ready) ==
		      V34_PHASE2_DPSK_INVALID_ARGUMENT &&
		      !measurement.timing_locked && measurement.line_power == 0.0,
	      "Phase 2 receiver accepted corrupt state");
	CHECK(strcmp(v34_phase2_dpsk_result_name(
			     V34_PHASE2_DPSK_INVALID_ARGUMENT),
			     "invalid argument") == 0,
	      "Phase 2 transmitter returned an unstable result name");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "carriers and cadence", test_carriers_and_cadence },
		{ "bidirectional loopback", test_bidirectional_loopback },
		{ "receive tolerance", test_receive_tolerance },
		{ "validation", test_validation },
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
