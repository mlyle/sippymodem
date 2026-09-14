// SPDX-License-Identifier: BSD-3-Clause

#include <phase2_info1_exchange.h>
#include <g711.h>

#include "test.h"

#include <string.h>

static int16_t quantize(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMU ?
		g711_decode_pcmu(g711_encode_pcmu(sample)) :
		g711_decode_pcma(g711_encode_pcma(sample));
}

static bool check_last_sample(
	const struct phase2_info1_exchange *receiver, int16_t input)
{
	struct phase2_info1_exchange last = *receiver, rejected;
	struct phase2_info1_event event;
	int16_t output;

	CHECK(phase2_info1_sample(&last, input, &output, &event) ==
		      PHASE2_INFO1_OK &&
	      event.kind == PHASE2_INFO1_EVENT_PEER_FRAME_PENDING &&
	      phase2_info1_reject_peer(&last) == PHASE2_INFO1_OK &&
	      last.phase == PHASE2_INFO1_WAITING_FOR_PEER &&
	      last.short_receive_budget_samples ==
		      receiver->short_receive_budget_samples &&
	      last.peer_deadline_elapsed_samples ==
		      receiver->peer_deadline_elapsed_samples + 1u,
	      "rejected INFO1a reset or failed to charge the original budget");
	last = *receiver;
	last.short_receive_budget_samples =
		(uint16_t)(last.peer_deadline_elapsed_samples + 1u);
	CHECK(phase2_info1_sample(&last, input, &output, &event) ==
		      PHASE2_INFO1_OK && output == 0 &&
	      event.kind == PHASE2_INFO1_EVENT_PEER_FRAME_PENDING,
	      "valid INFO1a on the last sample lost to the deadline");
	rejected = last;
	CHECK(phase2_info1_reject_peer(&rejected) == PHASE2_INFO1_OK &&
	      rejected.phase == PHASE2_INFO1_FAILED &&
	      rejected.failure == PHASE2_INFO1_PEER_TIMEOUT &&
	      phase2_info1_accept_peer(&last) == PHASE2_INFO1_OK &&
	      last.phase == PHASE2_INFO1_SILENCE,
	      "pending INFO1a acceptance/rejection lost the original deadline");
	rejected = last;
	CHECK(phase2_info1_accept_peer(&last) == PHASE2_INFO1_INVALID_ARGUMENT &&
	      phase2_info1_reject_peer(&last) == PHASE2_INFO1_INVALID_ARGUMENT &&
	      memcmp(&last, &rejected, sizeof(last)) == 0,
	      "accepted INFO1a was resolved more than once");
	return true;
}

static bool check_one_way_exchange(
	enum g711_law law, unsigned delay, unsigned tone_samples)
{
	/* Independent transport bytes, including leading/trailing zero bits.
	 * Selection semantics belong to the protocol owner, not this transport. */
	const uint8_t payload[] = { 0x02, 0x83, 0x94, 0xb0, 0x04 };
	struct v34_phase2_tone_transmitter tone;
	struct phase2_info1_exchange analog, digital;
	int16_t line[257] = { 0 };
	size_t sent = SIZE_MAX, complete = SIZE_MAX;
	bool received = false;

	CHECK(v34_phase2_tone_transmitter_init(
		      &tone, V34_ANSWERING_STATION, 12000) == V34_PHASE2_TONE_OK,
	      "Tone A did not initialize");
	for (unsigned i = 0; i < tone_samples; ++i) {
		int16_t sample;

		CHECK(v34_phase2_tone_transmitter_sample(&tone, &sample) ==
			      V34_PHASE2_TONE_OK, "Tone A did not advance");
	}
	CHECK(phase2_info1_init_short_signal_a(
		      &analog, &tone, payload, 40) == PHASE2_INFO1_OK &&
	      phase2_info1_init_short_signal_b(
		      &digital, 19000, 40) == PHASE2_INFO1_OK &&
	      analog.transmitter.modulation.carrier.carrier_phase ==
		      tone.carrier_phase - UINT32_C(0x40000000) &&
	      analog.transmitter.modulation.guard_phase == tone.guard_phase,
	      "short INFO1a did not retain Tone A phase");
	for (size_t i = 0; i < 10000; ++i) {
		struct phase2_info1_event ae, de;
		struct phase2_info1_exchange before = digital;
		int16_t ao, d_out, input = line[i % delay];

		CHECK(phase2_info1_sample(&analog, 0, &ao, &ae) == PHASE2_INFO1_OK &&
		      phase2_info1_sample(&digital, input, &d_out, &de) ==
			      PHASE2_INFO1_OK,
		      "one-way INFO1 sample %zu failed", i);
		CHECK(d_out == 0 &&
		      ae.kind != PHASE2_INFO1_EVENT_LOCAL_FIRST_SENT &&
		      de.kind != PHASE2_INFO1_EVENT_LOCAL_FIRST_SENT,
		      "one-way exchange emitted a digital record");
		if (ae.kind == PHASE2_INFO1_EVENT_LOCAL_SECOND_SENT)
			sent = i;
		if (analog.phase == PHASE2_INFO1_COMPLETE && complete == SIZE_MAX)
			complete = i;
		if (de.kind == PHASE2_INFO1_EVENT_PEER_FRAME_PENDING) {
			CHECK(!received && de.frame_event.payload_bit_count == 40 &&
			      memcmp(de.frame_event.payload, payload, sizeof(payload)) == 0,
			      "short INFO1a payload mismatch");
			CHECK(check_last_sample(&before, input) &&
			      phase2_info1_accept_peer(&digital) == PHASE2_INFO1_OK,
			      "short INFO1a could not hand off");
			received = true;
		}
		line[i % delay] = quantize(law, ao);
		if (analog.phase == PHASE2_INFO1_COMPLETE &&
		    digital.phase == PHASE2_INFO1_COMPLETE)
			break;
	}
	CHECK(sent != SIZE_MAX && complete - sent == 560 && received &&
	      analog.phase == PHASE2_INFO1_COMPLETE &&
	      digital.phase == PHASE2_INFO1_COMPLETE,
	      "short INFO1a missed completion or its 70 ms quiet interval");
	return true;
}

static bool test_one_way_exchange(void)
{
	const unsigned delays[] = { 1, 257 }, phases[] = { 13, 619 };

	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (size_t d = 0; d < ARRAY_LENGTH(delays); ++d)
			for (size_t p = 0; p < ARRAY_LENGTH(phases); ++p)
				CHECK(check_one_way_exchange(law, delays[d], phases[p]),
				      "law %u delay %u phase %u failed",
				      law, delays[d], phases[p]);
	return true;
}

static bool test_receive_budget(void)
{
	const uint16_t budgets[] = { 1, 73, 19381 };

	for (size_t n = 0; n < ARRAY_LENGTH(budgets); ++n) {
		struct phase2_info1_exchange digital, before;
		struct phase2_info1_event event;
		int16_t output;

		CHECK(phase2_info1_init_short_signal_b(
			      &digital, budgets[n], 40) == PHASE2_INFO1_OK,
		      "short receive budget did not initialize");
		for (unsigned i = 1; i <= budgets[n]; ++i) {
			CHECK(phase2_info1_sample(&digital, 0, &output, &event) ==
				      PHASE2_INFO1_OK && output == 0 &&
			      digital.phase == (i < budgets[n] ?
				      PHASE2_INFO1_WAITING_FOR_PEER : PHASE2_INFO1_FAILED),
			      "remaining budget %u failed at %u", budgets[n], i);
		}
		CHECK(event.kind == PHASE2_INFO1_EVENT_FAILED &&
		      event.failure == PHASE2_INFO1_PEER_TIMEOUT,
		      "expired short budget did not report its timeout");
		before = digital;
		CHECK(phase2_info1_sample(&digital, INT16_MAX, &output, &event) ==
			      PHASE2_INFO1_OK && output == 0 &&
		      event.kind == PHASE2_INFO1_EVENT_NONE &&
		      memcmp(&before, &digital, sizeof(digital)) == 0,
		      "expired short exchange was not stable");
		CHECK(phase2_info1_recover(&digital) == PHASE2_INFO1_INVALID_ARGUMENT &&
		      phase2_info1_repeat_signal_b(&digital) ==
			      PHASE2_INFO1_INVALID_ARGUMENT &&
		      memcmp(&before, &digital, sizeof(digital)) == 0,
		      "short timeout reused full-startup INFO recovery");
	}
	return true;
}

static bool test_corrupt_frame_budget(void)
{
	const uint8_t payload[] = { 0x02, 0x83, 0x94, 0xb0, 0x04 };
	const uint16_t budget = 2000;

	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		struct v34_phase2_info_generator frame;
		struct v34_phase2_dpsk_transmitter transmitter;
		struct phase2_info1_exchange digital;
		unsigned rejected = 0;

		CHECK(v34_phase2_info_generator_init(&frame, payload, 40) ==
			      V34_PHASE2_INFO_OK &&
		      v34_phase2_dpsk_transmitter_init(&transmitter,
			      V34_ANSWERING_STATION, 12000) == V34_PHASE2_DPSK_OK &&
		      phase2_info1_init_short_signal_b(&digital, budget, 40) ==
			      PHASE2_INFO1_OK, "bad-CRC wire did not initialize");
		for (unsigned i = 0; i < budget; ++i) {
			uint8_t bit = 0;
			bool accepted, complete;
			int16_t sample, output;
			struct phase2_info1_event event;

			if (v34_phase2_dpsk_transmitter_can_accept_bit(&transmitter) &&
			    frame.bit_index < v34_phase2_info_frame_bit_count(40)) {
				size_t index = frame.bit_index;

				CHECK(v34_phase2_info_next_bit(&frame, &bit, &complete) ==
					      V34_PHASE2_INFO_OK, "bad-CRC source bit failed");
				/* Corrupt the emitted CRC bit, not receiver state. */
				if (index == V34_PHASE2_INFO_PREFIX_BITS + 40u)
					bit ^= 1u;
			}
			CHECK(v34_phase2_dpsk_transmitter_sample(&transmitter, bit,
				      &sample, &accepted) == V34_PHASE2_DPSK_OK &&
			      phase2_info1_sample(&digital, quantize(law, sample),
				      &output, &event) == PHASE2_INFO1_OK && output == 0,
			      "bad-CRC sample failed");
			CHECK(event.kind != PHASE2_INFO1_EVENT_PEER_FRAME_PENDING &&
			      digital.peer_deadline_elapsed_samples == i + 1u &&
			      digital.short_receive_budget_samples == budget,
			      "bad CRC advanced the exchange or renewed its budget");
			rejected += event.kind == PHASE2_INFO1_EVENT_PEER_FRAME_REJECTED;
		}
		CHECK(rejected == 1 && digital.phase == PHASE2_INFO1_FAILED &&
		      digital.failure == PHASE2_INFO1_PEER_TIMEOUT,
		      "bad CRC did not expire at the original deadline");
	}
	return true;
}

static bool test_validation(void)
{
	const uint8_t payload[] = { 0 };
	struct v34_phase2_tone_transmitter tone;
	struct phase2_info1_exchange exchange = { 0 }, before, invalid;

	CHECK(v34_phase2_tone_transmitter_init(
		      &tone, V34_ANSWERING_STATION, 12000) == V34_PHASE2_TONE_OK,
	      "validation tone did not initialize");
	before = exchange;
	CHECK(phase2_info1_init_short_signal_b(&exchange, 0, 40) ==
		      PHASE2_INFO1_INVALID_ARGUMENT &&
	      phase2_info1_init_short_signal_b(&exchange, 12, 0) ==
		      PHASE2_INFO1_INVALID_ARGUMENT &&
	      phase2_info1_init_short_signal_b(NULL, 12, 40) ==
		      PHASE2_INFO1_INVALID_ARGUMENT &&
	      phase2_info1_init_short_signal_a(&exchange, &tone, payload, 0) ==
		      PHASE2_INFO1_INVALID_ARGUMENT &&
	      phase2_info1_init_short_signal_a(&exchange, &tone, NULL, 1) ==
		      PHASE2_INFO1_INVALID_ARGUMENT &&
	      phase2_info1_init_short_signal_a(&exchange, NULL, payload, 1) ==
		      PHASE2_INFO1_INVALID_ARGUMENT &&
	      memcmp(&before, &exchange, sizeof(exchange)) == 0,
	      "invalid short initialization changed output");
	CHECK(phase2_info1_init_short_signal_b(&exchange, 12, 40) ==
		      PHASE2_INFO1_OK, "receive validation fixture failed");
	invalid = exchange;
	invalid.phase = PHASE2_INFO1_TRANSMITTING;
	CHECK(!phase2_info1_is_valid(&invalid), "digital short exchange may transmit");
	invalid = exchange;
	invalid.carrier_tail_symbols = 1;
	CHECK(!phase2_info1_is_valid(&invalid), "digital short exchange has a tail");
	CHECK(phase2_info1_init_short_signal_a(&exchange, &tone, payload, 1) ==
		      PHASE2_INFO1_OK, "transmit validation fixture failed");
	invalid = exchange;
	invalid.phase = PHASE2_INFO1_WAITING_FOR_PEER;
	CHECK(!phase2_info1_is_valid(&invalid), "analogue short exchange may wait");
	invalid = exchange;
	invalid.first_payload_bit_count = 1;
	CHECK(!phase2_info1_is_valid(&invalid), "short exchange has a first record");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "one-way short INFO1a", test_one_way_exchange },
		{ "short receive budget", test_receive_budget },
		{ "corrupt INFO1a receive budget", test_corrupt_frame_budget },
		{ "short exchange validation", test_validation },
	};
	unsigned int failures = 0;

	for (size_t i = 0; i < ARRAY_LENGTH(tests); ++i) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[i].name);
		if (tests[i].run())
			fprintf(stderr, "[       OK ] %s\n", tests[i].name);
		else {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[i].name);
			failures++;
		}
	}
	return failures != 0;
}
