// SPDX-License-Identifier: BSD-3-Clause

#include <v34_phase3_exchange.h>

#include <g711.h>

#include "test.h"

#include <limits.h>
#include <string.h>

enum {
	SAMPLE_RATE = 8000,
	SAMPLE_LIMIT = 50000,
	MAXIMUM_DELAY_SAMPLES = 48,
};

enum test_codec {
	TEST_LINEAR,
	TEST_PCMU,
	TEST_PCMA,
};

static int16_t transcode(int16_t sample, enum test_codec codec)
{
	switch (codec) {
	case TEST_LINEAR:
		return sample;
	case TEST_PCMU:
		return g711_decode_pcmu(g711_encode_pcmu(sample));
	case TEST_PCMA:
		return g711_decode_pcma(g711_encode_pcma(sample));
	}
	return 0;
}

struct test_channel {
	int16_t call_to_answer[MAXIMUM_DELAY_SAMPLES];
	int16_t answer_to_call[MAXIMUM_DELAY_SAMPLES];
	size_t delay_samples;
	size_t cursor;
	enum test_codec codec;
};

static void channel_receive(
	const struct test_channel *channel, int16_t *caller,
	int16_t *answerer)
{
	*caller = channel->answer_to_call[channel->cursor];
	*answerer = channel->call_to_answer[channel->cursor];
}

static void channel_transmit(
	struct test_channel *channel, int16_t caller, int16_t answerer)
{
	channel->call_to_answer[channel->cursor] =
		transcode(caller, channel->codec);
	channel->answer_to_call[channel->cursor] =
		transcode(answerer, channel->codec);
	channel->cursor = (channel->cursor + 1u) % channel->delay_samples;
}

static struct v34_phase3_profile profile(enum v34_station_role role)
{
	return (struct v34_phase3_profile){
		.symbol_rate_numerator = 3000,
		.symbol_rate_denominator = 1,
		.carrier_frequency_numerator =
			role == V34_CALLING_STATION ? 1800u : 2000u,
		.carrier_frequency_denominator = 1,
		.symbol_rate = V34_SYMBOL_RATE_3000,
		.local_role = role,
		.preemphasis_filter_index =
			role == V34_CALLING_STATION ? 2u : 3u,
		.projected_data_rate_2400_bps_units = 8,
		.high_carrier = role == V34_ANSWERING_STATION,
	};
}

static bool profile_at_rate(
	enum v34_station_role role, enum v34_symbol_rate rate,
	struct v34_phase3_profile *selected)
{
	struct v34_info1_probe_result probe = {
		.preemphasis_filter_index = 3,
		.projected_data_rate_2400_bps_units = 8,
		.high_carrier = rate != V34_SYMBOL_RATE_3429 &&
			role == V34_ANSWERING_STATION,
	};

	return v34_phase3_profile_init(
		       selected, role, rate, &probe, 0, 0) ==
	       V34_PHASE3_PROFILE_OK;
}

static struct v34_phase3_exchange_configuration configuration(
	enum v34_constellation_size requested)
{
	return (struct v34_phase3_exchange_configuration){
		.receive = {
			.s_gain_adaptation = 0.1,
			.pp_equalizer_adaptation = 0.2,
			.tracking_equalizer_adaptation = 0.05,
		},
		.requested_remote_constellation = requested,
		.sample_rate = SAMPLE_RATE,
		.nominal_peak_amplitude = 7000,
	};
}

static bool record_events(uint32_t events, uint32_t *seen)
{
	if ((events & *seen) != 0)
		return false;
	*seen |= events;
	return true;
}

static bool run_generated_duplex_exchange(
	enum test_codec codec, size_t delay_samples)
{
	const uint32_t common_events =
		V34_PHASE3_EXCHANGE_EVENT_TRANSMIT_STARTED |
		V34_PHASE3_EXCHANGE_EVENT_TRANSMIT_J_STARTED |
		V34_PHASE3_EXCHANGE_EVENT_PEER_TIMING_ACQUIRED |
		V34_PHASE3_EXCHANGE_EVENT_PEER_S_DETECTED |
		V34_PHASE3_EXCHANGE_EVENT_PEER_PP_COMPLETE |
		V34_PHASE3_EXCHANGE_EVENT_PEER_J_DETECTED |
		V34_PHASE3_EXCHANGE_EVENT_HANDOFF_READY;
	struct v34_phase3_profile caller_profile =
		profile(V34_CALLING_STATION);
	struct v34_phase3_profile answerer_profile =
		profile(V34_ANSWERING_STATION);
	struct v34_phase3_exchange_configuration caller_configuration =
		configuration(V34_CONSTELLATION_16_POINT);
	struct v34_phase3_exchange_configuration answerer_configuration =
		configuration(V34_CONSTELLATION_4_POINT);
	struct v34_phase3_exchange caller;
	struct v34_phase3_exchange answerer;
	struct test_channel channel = {
		.delay_samples = delay_samples,
		.codec = codec,
	};
	struct v34_phase3_exchange_handoff caller_handoff;
	struct v34_phase3_exchange_handoff answerer_handoff;
	uint32_t caller_seen = 0;
	uint32_t answerer_seen = 0;
	uint32_t answer_silence = 0;
	uint64_t answerer_samples_at_stop = 0;
	size_t exchanged_samples = 0;
	size_t answerer_receive_restart = SAMPLE_LIMIT;
	size_t answerer_signal_start = SAMPLE_LIMIT;
	int16_t last_answer_transmit = 1;
	bool complete = false;

	CHECK(v34_phase3_exchange_init(
		      &caller, &caller_profile, &answerer_profile,
		      &caller_configuration) == V34_PHASE3_EXCHANGE_OK &&
	      v34_phase3_exchange_init(
		      &answerer, &answerer_profile, &caller_profile,
		      &answerer_configuration) == V34_PHASE3_EXCHANGE_OK,
	      "duplex Phase 3 exchange did not initialize");
	for (size_t index = 0; index < SAMPLE_LIMIT; ++index) {
		int16_t caller_receive;
		int16_t answerer_receive;
		int16_t next_call_to_answer;
		int16_t next_answer_to_call;
		uint32_t caller_events;
		uint32_t answerer_events;
		enum v34_phase3_exchange_result caller_result;
		enum v34_phase3_exchange_result answerer_result;
		bool answerer_was_transmitting = answerer.transmit_started;
		bool answerer_signal_was_started =
			answerer.receive.line.fixed_signal_started;

		channel_receive(&channel, &caller_receive, &answerer_receive);
		caller_result = v34_phase3_exchange_sample(
			      &caller, caller_receive,
			      &next_call_to_answer,
			      &caller_events);
		answerer_result = v34_phase3_exchange_sample(
			      &answerer, answerer_receive,
			      &next_answer_to_call,
			      &answerer_events);
		CHECK(caller_result == V34_PHASE3_EXCHANGE_OK &&
		      answerer_result == V34_PHASE3_EXCHANGE_OK,
		      "codec %d Phase 3 exchange failed at sample %zu: %s/%s",
		      codec, index,
		      v34_phase3_exchange_result_name(caller_result),
		      v34_phase3_exchange_result_name(answerer_result));
		if (!answerer_was_transmitting) {
			CHECK(next_answer_to_call == 0,
			      "answerer emitted sample %d during initial silence",
			      next_answer_to_call);
			answer_silence++;
		}
		CHECK(record_events(caller_events, &caller_seen) &&
		      record_events(answerer_events, &answerer_seen),
		      "Phase 3 event repeated at sample %zu", index);
		if (!answerer_signal_was_started &&
		    answerer.receive.line.fixed_signal_started)
			answerer_signal_start = index;
		if ((answerer_events &
		     V34_PHASE3_EXCHANGE_EVENT_TRANSMIT_STOPPED) != 0)
			answerer_samples_at_stop =
				answerer.transmit.line.generated_samples;
		if ((answerer_events &
		     V34_PHASE3_EXCHANGE_EVENT_TRANSMIT_J_STARTED) != 0) {
			answerer_receive_restart = index;
			CHECK(answerer.receive.points.stage ==
				      V34_PHASE3_RECEIVER_STAGE_S &&
			      answerer.receive.line.mode ==
				      V34_LINE_FRONTEND_FIXED &&
			      !answerer.receive.line.fixed_signal_started &&
			      answerer.receive.line.state.fixed.carrier.phase == 0,
			      "answerer did not restart S reception at local J");
		}
		if ((caller_events &
		     V34_PHASE3_EXCHANGE_EVENT_TRANSMIT_STARTED) != 0)
			CHECK((caller_events &
			       V34_PHASE3_EXCHANGE_EVENT_PEER_J_DETECTED) != 0,
			      "caller started before peer J detection");
		channel_transmit(
			&channel, next_call_to_answer, next_answer_to_call);
		exchanged_samples = index + 1u;
		last_answer_transmit = next_answer_to_call;
		if (v34_phase3_exchange_is_handoff_ready(&caller) &&
		    v34_phase3_exchange_is_handoff_ready(&answerer)) {
			complete = true;
			break;
		}
	}
	CHECK(complete && !v34_phase3_exchange_has_failed(&caller) &&
	      !v34_phase3_exchange_has_failed(&answerer),
	      "codec %d delay %zu stopped at caller tx/rx=%d/%d "
	      "answerer tx/rx=%d/%d events=0x%x/0x%x",
	      codec, delay_samples, caller.transmit.symbols.stage,
	      caller.receive.points.stage, answerer.transmit.symbols.stage,
	      answerer.receive.points.stage, caller_seen, answerer_seen);
	CHECK(caller.receive.line.mode == V34_LINE_FRONTEND_QAM &&
	      caller.receive.line.state.qam.carrier_phase ==
		      (uint32_t)((uint64_t)caller.receive.line.state.qam
					 .carrier_phase_step *
			 exchanged_samples) &&
	      answerer_receive_restart < exchanged_samples &&
	      answerer_signal_start < exchanged_samples &&
	      answerer.receive.line.mode == V34_LINE_FRONTEND_FIXED &&
	      answerer.receive.line.state.fixed.carrier.phase ==
		      (uint32_t)((uint64_t)answerer.receive.line.state.fixed
					 .carrier.phase_step *
			 (exchanged_samples - answerer_signal_start)),
	      "Phase 3 receivers did not consume the complete sample timeline");
	CHECK(answer_silence ==
		      SAMPLE_RATE * V34_PHASE3_ANSWER_SILENCE_MS / 1000u,
	      "answerer silence was %u samples", answer_silence);
	CHECK(caller_seen == common_events &&
	      answerer_seen ==
		      (common_events |
		       V34_PHASE3_EXCHANGE_EVENT_TRANSMIT_STOPPED),
	      "Phase 3 event coverage differed: caller=0x%x answerer=0x%x",
	      caller_seen, answerer_seen);
	CHECK(caller.transmit.symbols.stage == V34_PHASE3_TRANSMITTER_J &&
	      caller.trn_target_symbols == V34_PHASE3_TRN_MIN_SYMBOLS &&
	      caller.transmit.symbols.trn_symbols == caller.trn_target_symbols &&
	      caller.transmit.symbols.j_symbols >=
		      V34_PHASE3_J_PATTERN_SYMBOLS &&
	      answerer.stop_transmit_requested && answerer.transmit_stopped &&
	      answerer.transmit.symbols.stage ==
		      V34_PHASE3_TRANSMITTER_COMPLETE &&
	      answerer.trn_target_symbols == 5568u &&
	      answerer.transmit.symbols.trn_symbols ==
		      answerer.trn_target_symbols &&
	      answerer_samples_at_stop != 0 &&
	      answerer.transmit.line.generated_samples >
		      answerer_samples_at_stop && last_answer_transmit == 0,
	      "Phase 3 transmitters reached the wrong asymmetric boundary");
	CHECK(v34_phase3_exchange_handoff(
		      &caller, &caller_handoff) == V34_PHASE3_EXCHANGE_OK &&
	      v34_phase3_exchange_handoff(
		      &answerer, &answerer_handoff) == V34_PHASE3_EXCHANGE_OK &&
	      v34_phase3_exchange_handoff_is_valid(&caller_handoff) &&
	      v34_phase3_exchange_handoff_is_valid(&answerer_handoff) &&
	      caller_handoff.receive.phase3.j.detection.sequence
			      .remote_constellation == V34_CONSTELLATION_4_POINT &&
	      answerer_handoff.receive.phase3.j.detection.sequence
			      .remote_constellation == V34_CONSTELLATION_16_POINT &&
	      caller_handoff.phase3_transmit.remote_constellation ==
		      V34_CONSTELLATION_16_POINT &&
	      answerer_handoff.phase3_transmit.remote_constellation ==
		      V34_CONSTELLATION_4_POINT &&
	      v34_line_frontend_timing_acquired(
		      &caller_handoff.receive.line) &&
	      v34_line_frontend_timing_acquired(
		      &answerer_handoff.receive.line) &&
	      caller_handoff.receive.line.mode == V34_LINE_FRONTEND_QAM &&
	      answerer_handoff.receive.line.mode == V34_LINE_FRONTEND_FIXED,
	      "Phase 3 handoff lost either J constellation selection");
	return true;
}

static bool test_trn_duration_policy(void)
{
	static const struct {
		enum v34_symbol_rate rate;
		uint32_t symbols;
	} cases[] = {
		{ V34_SYMBOL_RATE_2400, 4368 },
		{ V34_SYMBOL_RATE_2743, 5053 },
		{ V34_SYMBOL_RATE_2800, 5168 },
		{ V34_SYMBOL_RATE_3000, 5568 },
		{ V34_SYMBOL_RATE_3200, 5968 },
		{ V34_SYMBOL_RATE_3429, 6425 },
	};
	struct v34_phase3_exchange_configuration config =
		configuration(V34_CONSTELLATION_4_POINT);

	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index) {
		struct v34_phase3_profile caller;
		struct v34_phase3_profile answerer;
		struct v34_phase3_exchange exchange;

		CHECK(profile_at_rate(
			      V34_CALLING_STATION, cases[index].rate, &caller) &&
		      profile_at_rate(
			      V34_ANSWERING_STATION, cases[index].rate,
			      &answerer) &&
		      v34_phase3_exchange_init(
			      &exchange, &caller, &answerer, &config) ==
			      V34_PHASE3_EXCHANGE_OK &&
		      exchange.trn_target_symbols ==
			      V34_PHASE3_TRN_MIN_SYMBOLS,
		      "rate %d caller selected %u TRN symbols instead of %u",
		      cases[index].rate, exchange.trn_target_symbols,
		      V34_PHASE3_TRN_MIN_SYMBOLS);
		CHECK(v34_phase3_exchange_init(
			      &exchange, &answerer, &caller, &config) ==
			      V34_PHASE3_EXCHANGE_OK &&
		      exchange.trn_target_symbols == cases[index].symbols,
		      "rate %d answerer selected %u TRN symbols instead of %u",
		      cases[index].rate, exchange.trn_target_symbols,
		      cases[index].symbols);
	}
	return true;
}

static bool test_generated_duplex_exchange(void)
{
	static const struct {
		enum test_codec codec;
		size_t delay_samples;
	} cases[] = {
		{ TEST_LINEAR, 1 },
		{ TEST_PCMU, 13 },
		{ TEST_PCMA, 47 },
	};

	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index)
		CHECK(run_generated_duplex_exchange(
			      cases[index].codec, cases[index].delay_samples),
		      "generated case %zu failed", index);
	return true;
}

static bool test_receiver_selection(void)
{
	struct v34_phase3_profile caller = profile(V34_CALLING_STATION);
	struct v34_phase3_profile answerer = profile(V34_ANSWERING_STATION);
	struct v34_phase3_exchange_configuration config =
		configuration(V34_CONSTELLATION_4_POINT);
	struct v34_phase3_exchange exchange;

	CHECK(v34_phase3_exchange_init(
		      &exchange, &caller, &answerer, &config) ==
		      V34_PHASE3_EXCHANGE_OK &&
	      exchange.receive.line.mode == V34_LINE_FRONTEND_QAM,
	      "caller did not retain its compatibility receiver");
	CHECK(v34_phase3_exchange_init(
		      &exchange, &answerer, &caller, &config) ==
		      V34_PHASE3_EXCHANGE_OK &&
	      exchange.receive.line.mode == V34_LINE_FRONTEND_FIXED,
	      "qualified answerer did not select the fixed receiver");
	caller.projected_data_rate_2400_bps_units = 9;
	CHECK(v34_phase3_profile_is_valid(&caller) &&
	      v34_phase3_exchange_init(
		      &exchange, &answerer, &caller, &config) ==
		      V34_PHASE3_EXCHANGE_OK &&
	      exchange.receive.line.mode == V34_LINE_FRONTEND_QAM,
	      "unqualified answerer did not retain its compatibility receiver");
	return true;
}

static bool test_validation_and_transactionality(void)
{
	struct v34_phase3_profile caller_profile =
		profile(V34_CALLING_STATION);
	struct v34_phase3_profile answerer_profile =
		profile(V34_ANSWERING_STATION);
	struct v34_phase3_profile with_md = caller_profile;
	struct v34_phase3_exchange_configuration config =
		configuration(V34_CONSTELLATION_4_POINT);
	struct v34_phase3_exchange exchange;
	struct v34_phase3_exchange saved;
	struct v34_phase3_exchange_handoff handoff;
	struct v34_phase3_exchange_handoff saved_handoff;
	union {
		uint32_t events;
		int16_t sample[2];
	} overlap = { .events = UINT32_C(0x5a5aa5a5) };
	int16_t sample = 1234;
	uint32_t events = UINT32_C(0xa5a5a5a5);

	with_md.md_length_35_ms_units = 1;
	with_md.md_symbol_count = 105;
	memset(&exchange, 0xa5, sizeof(exchange));
	saved = exchange;
	CHECK(v34_phase3_exchange_init(
		      NULL, &caller_profile, &answerer_profile, &config) ==
		      V34_PHASE3_EXCHANGE_INVALID_ARGUMENT &&
	      v34_phase3_exchange_init(
		      &exchange, &caller_profile, &caller_profile, &config) ==
		      V34_PHASE3_EXCHANGE_INVALID_ARGUMENT &&
	      v34_phase3_exchange_init(
		      &exchange, &with_md, &answerer_profile, &config) ==
		      V34_PHASE3_EXCHANGE_INVALID_ARGUMENT &&
	      memcmp(&exchange, &saved, sizeof(exchange)) == 0,
	      "invalid Phase 3 ownership changed the destination");
	config.sample_rate = 1000;
	CHECK(v34_phase3_exchange_init(
		      &exchange, &caller_profile, &answerer_profile, &config) ==
		      V34_PHASE3_EXCHANGE_OUT_OF_RANGE &&
	      memcmp(&exchange, &saved, sizeof(exchange)) == 0,
	      "out-of-band Phase 3 line changed the destination");
	config = configuration(V34_CONSTELLATION_4_POINT);
	config.nominal_peak_amplitude = INT16_MAX;
	CHECK(v34_phase3_exchange_init(
		      &exchange, &caller_profile, &answerer_profile, &config) ==
		      V34_PHASE3_EXCHANGE_CLIPPING_RISK &&
	      memcmp(&exchange, &saved, sizeof(exchange)) == 0,
	      "clipping-risk Phase 3 line changed the destination");
	config = configuration(V34_CONSTELLATION_4_POINT);
	CHECK(v34_phase3_exchange_init(
		      &exchange, &caller_profile, &answerer_profile, &config) ==
		      V34_PHASE3_EXCHANGE_OK,
	      "validation fixture did not initialize");
	saved = exchange;
	exchange.trn_target_symbols--;
	CHECK(!v34_phase3_exchange_is_valid(&exchange),
	      "changed Phase 3 TRN target remained valid");
	exchange = saved;
	memset(&handoff, 0x5a, sizeof(handoff));
	saved_handoff = handoff;
	CHECK(v34_phase3_exchange_sample(
		      &exchange, 0, NULL, &events) ==
		      V34_PHASE3_EXCHANGE_INVALID_ARGUMENT &&
	      v34_phase3_exchange_sample(
		      &exchange, 0, &sample, NULL) ==
		      V34_PHASE3_EXCHANGE_INVALID_ARGUMENT &&
	      v34_phase3_exchange_sample(
		      &exchange, 0, &overlap.sample[0], &overlap.events) ==
		      V34_PHASE3_EXCHANGE_INVALID_ARGUMENT &&
	      v34_phase3_exchange_sample(
		      &exchange, 0, &overlap.sample[1], &overlap.events) ==
		      V34_PHASE3_EXCHANGE_INVALID_ARGUMENT &&
	      v34_phase3_exchange_handoff(&exchange, &handoff) ==
		      V34_PHASE3_EXCHANGE_INVALID_STATE &&
	      memcmp(&exchange, &saved, sizeof(exchange)) == 0 &&
	      memcmp(&handoff, &saved_handoff, sizeof(handoff)) == 0 &&
	      sample == 1234 && events == UINT32_C(0xa5a5a5a5) &&
	      overlap.events == UINT32_C(0x5a5aa5a5),
	      "rejected Phase 3 calls were not transactional");
	CHECK(!v34_phase3_exchange_is_valid(NULL) &&
	      !v34_phase3_exchange_has_failed(NULL) &&
	      !v34_phase3_exchange_is_handoff_ready(NULL) &&
	      !v34_phase3_exchange_handoff_is_valid(NULL) &&
	      strcmp(v34_phase3_exchange_result_name(
		     V34_PHASE3_EXCHANGE_SEQUENCE_MISMATCH),
		     "sequence mismatch") == 0 &&
	      strcmp(v34_phase3_exchange_result_name(
		     (enum v34_phase3_exchange_result)99),
		     "unknown") == 0,
	      "Phase 3 validation helpers returned the wrong result");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "TRN duration policy", test_trn_duration_policy },
		{ "generated duplex exchange", test_generated_duplex_exchange },
		{ "receiver selection", test_receiver_selection },
		{ "validation and transactionality",
		  test_validation_and_transactionality },
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
