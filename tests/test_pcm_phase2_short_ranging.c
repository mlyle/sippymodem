// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <pcm_phase2_ranging.h>
#include <phase2_info1_exchange.h>
#include <v90_info1.h>

#include "test.h"

#include <limits.h>
#include <string.h>

enum test_codec { TEST_CODEC_PCMU, TEST_CODEC_PCMA };

static int16_t codec_round_trip(int16_t sample, enum test_codec codec)
{
	return codec == TEST_CODEC_PCMU ?
		       g711_decode_pcmu(g711_encode_pcmu(sample)) :
		       g711_decode_pcma(g711_encode_pcma(sample));
}

static struct v34_info0 analog_capabilities(void)
{
	return (struct v34_info0){
		.symbol_rate_2743_supported = true,
		.symbol_rate_3429_supported = true,
		.low_carrier_3000_supported = true,
		.symbol_rate_3429_transmit_permitted = true,
		.transmit_power_reduction_supported = true,
		.maximum_symbol_rate_difference_steps = 2,
		.constellation_1664_supported = true,
	};
}

static struct v90_info0d digital_capabilities(enum test_codec codec)
{
	return (struct v90_info0d){
		.common = {
			.symbol_rate_2800_supported = true,
			.symbol_rate_3429_supported = true,
			.high_carrier_3000_supported = true,
			.symbol_rate_3429_transmit_permitted = true,
			.maximum_symbol_rate_difference_steps = 4,
			.cme_modem = true,
			.constellation_1664_supported = true,
		},
		.nominal_transmit_power = 9,
		.maximum_transmit_power = 18,
		.measure_power_at_codec_output = true,
		.pcm_law = codec == TEST_CODEC_PCMU ?
				   V90_PCM_MU_LAW : V90_PCM_A_LAW,
		.upstream_symbol_rate_3429_supported = true,
	};
}

static size_t absolute_difference(size_t left, size_t right)
{
	return left > right ? left - right : right - left;
}

enum { TEST_MAX_DELAY = 257 };

struct test_endpoint {
	struct pcm_phase2_ranging ranging;
	struct pcm_phase2_ranging_event last;
	unsigned reversal;
	unsigned complete;
};

static bool init_endpoint(struct test_endpoint *end, bool digital,
	unsigned law, bool short_offer)
{
	struct v92_info0a a = {
		.v90 = analog_capabilities(), .v92_capable = true,
		.short_phase2_requested = short_offer,
	};
	struct v92_info0d d = {
		.v90 = digital_capabilities(law ? TEST_CODEC_PCMA : TEST_CODEC_PCMU),
		.v92_capable = true, .short_phase2_requested = short_offer,
	};
	*end = (struct test_endpoint){ .reversal = UINT_MAX, .complete = UINT_MAX };
	CHECK((digital ? pcm_phase2_info0_exchange_init_v92_digital(
			&end->ranging.info0_exchange, 12000, &d) :
		pcm_phase2_info0_exchange_init_v92_analog(
			&end->ranging.info0_exchange, 12000, &a)) == PCM_PHASE2_INFO0_EXCHANGE_OK,
	      "typed INFO0 init failed");
	CHECK(phase2_ranging_control_init(&end->ranging.control,
		      &end->ranging.info0_exchange.transport) == PHASE2_RANGING_OK &&
	      pcm_phase2_ranging_is_valid(&end->ranging), "ranging init failed");
	return true;
}

static bool sample_endpoint(struct test_endpoint *end, int16_t in,
	unsigned index, int16_t *out)
{
	struct pcm_phase2_ranging_event event;
	*out = 0;
	if (end->complete != UINT_MAX)
		return true;
	CHECK(pcm_phase2_ranging_sample(&end->ranging, in, out, &event) == PCM_PHASE2_RANGING_OK,
	      "sample %u failed path=%u info=%u", index, end->ranging.path,
	      end->ranging.info0_exchange.state);
	if (event.short_step.reverse_carrier) {
		CHECK(end->reversal == UINT_MAX, "more than one short reversal");
		end->reversal = index;
	}
	if (event.kind == PHASE2_PROBE_TRANSMIT_READY || event.kind == PHASE2_PROBE_RECEIVE_READY ||
	    event.short_step.event != V92_SHORT_RANGING_NO_EVENT) {
		end->last = event;
		end->complete = index;
	}
	return true;
}

static bool check_pair(unsigned law, unsigned delay, unsigned skew,
	bool mute_first, unsigned offers)
{
	struct test_endpoint a, d;
	int16_t a_line[TEST_MAX_DELAY] = {0}, d_line[TEST_MAX_DELAY] = {0};
	bool initial_complete = false;
	CHECK(init_endpoint(&a, false, law, offers & 1) &&
	      init_endpoint(&d, true, law, offers & 2), "pair init failed");
	for (unsigned i = 0; i < 32000; ++i) {
		unsigned cursor = i % delay;
		int16_t a_out, d_out = 0;
		CHECK(sample_endpoint(&a, d_line[cursor], i, &a_out), "analog tick failed");
		if (i >= skew)
			CHECK(sample_endpoint(&d, a_line[cursor], i, &d_out), "digital tick failed");
		if (!initial_complete) {
			if (mute_first)
				a_out = 0;
			initial_complete = a.ranging.info0_exchange.transport.transmit_phase == PHASE2_INFO0_TONE;
		}
		a_line[cursor] = codec_round_trip(a_out, law ? TEST_CODEC_PCMA : TEST_CODEC_PCMU);
		d_line[cursor] = codec_round_trip(d_out, law ? TEST_CODEC_PCMA : TEST_CODEC_PCMU);
		if (a.complete != UINT_MAX && d.complete != UINT_MAX)
			break;
	}
	CHECK(a.complete != UINT_MAX && d.complete != UINT_MAX, "pair unfinished");
	if (offers == 3) {
		CHECK(a.last.short_step.event == V92_SHORT_RANGING_TRANSMIT_INFO1A &&
		      d.last.short_step.event == V92_SHORT_RANGING_RECEIVE_INFO1A &&
		      a.last.kind == PHASE2_RANGING_NO_EVENT && d.last.kind == PHASE2_RANGING_NO_EVENT,
		      "mutual short agreement ran wrong procedure");
		CHECK(absolute_difference(d.last.short_step.round_trip_delay_samples, 2 * delay) <= 8 &&
		      absolute_difference(a.reversal - d.reversal - delay, 320) <= 8 &&
		      d.last.short_step.info1a_remaining_samples == 20000 - (d.complete - d.reversal),
		      "short ranging changed physical timing/budget");
	} else {
		CHECK(a.last.kind == PHASE2_PROBE_TRANSMIT_READY && d.last.kind == PHASE2_PROBE_RECEIVE_READY &&
		      a.ranging.path == PCM_RANGING_FULL && d.ranging.path == PCM_RANGING_FULL,
		      "unilateral/absent short offer did not run full ranging");
	}
	CHECK(a.ranging.info0_exchange.peer_info0.v92_digital.short_phase2_requested == !!(offers & 2) &&
	      d.ranging.info0_exchange.peer_info0.v92_analog.short_phase2_requested == !!(offers & 1),
	      "changed actual received INFO0 offer");
	return true;
}

static bool missing_short_signal(unsigned law, unsigned delay, unsigned missing)
{
	/* missing: 0 = A remains steady, 1 = A disappears, 2 = no INFO1a. */
	struct test_endpoint a, d;
	struct phase2_info1_exchange info1;
	struct pcm_phase2_ranging_event event;
	int16_t a_line[TEST_MAX_DELAY] = {0}, d_line[TEST_MAX_DELAY] = {0};
	bool receiving_info1 = false, recovering = false, full_analog_started = false;
	unsigned deadline = UINT_MAX, recovery = UINT_MAX, resume = UINT_MAX;
	unsigned digital_nonzero_before_resume = 0;
	union pcm_info0_record original_peer = {0};

	CHECK(init_endpoint(&a, false, law, true) && init_endpoint(&d, true, law, true),
	      "recovery pair init failed");
	for (unsigned i = 0; i < 70000; ++i) {
		unsigned cursor = i % delay;
		int16_t a_out = 0, d_out = 0;
		if (recovering && i == resume) {
			/* The peer call owner now begins full ranging after silence.
			 * Keep the actual INFO0 records; only the execution path changes. */
			a.ranging.control = (struct phase2_ranging_control){
				.phase = PHASE2_SIGNAL_A_WAIT_INITIAL_TONE,
			};
			a.ranging.path = PCM_RANGING_SHORT_FULL_RECOVERY;
			a.complete = UINT_MAX;
			CHECK(v34_phase2_tone_detector_reset(&a.ranging.info0_exchange.transport.tone_detector) ==
			      V34_PHASE2_TONE_OK, "analog detector reset");
			CHECK(pcm_phase2_ranging_is_valid(&a.ranging), "full analog reentry invalid");
			full_analog_started = true;
		}
		if (full_analog_started || (!recovering && (missing == 2 ||
		    a.ranging.path != PCM_RANGING_SHORT_ACTIVE))) {
			CHECK(sample_endpoint(&a, d_line[cursor], i, &a_out), "analog recovery tick");
		} else if (!recovering && (missing == 0 || d.reversal == UINT_MAX)) {
			CHECK(v34_phase2_tone_transmitter_sample(&a.ranging.info0_exchange.transport.tone_transmitter,
			      &a_out) == V34_PHASE2_TONE_OK, "steady A failed");
		}
		if (receiving_info1) {
			struct phase2_info1_event frame;
			CHECK(phase2_info1_sample(&info1, 0, &d_out, &frame) == PHASE2_INFO1_OK && d_out == 0,
			      "silent INFO1 wait failed");
			if (frame.kind == PHASE2_INFO1_EVENT_FAILED) {
				CHECK(i == deadline, "INFO1 timeout restarted: %u/%u", i, deadline);
				CHECK(pcm_phase2_ranging_recover_short_info1(&d.ranging), "INFO1 recovery failed");
				receiving_info1 = false;
				recovering = true;
				recovery = i;
				resume = i + 16000;
			}
		} else if (d.complete == UINT_MAX) {
			CHECK(pcm_phase2_ranging_sample(&d.ranging, a_line[cursor], &d_out, &event) ==
			      PCM_PHASE2_RANGING_OK, "digital recovery sample %u", i);
			if (event.short_step.reverse_carrier)
				d.reversal = i;
			if (event.short_step.event == V92_SHORT_RANGING_RECEIVE_INFO1A) {
				CHECK(missing == 2, "unexpected A reversal");
				CHECK(phase2_info1_init_short_signal_b(&info1,
				      event.short_step.info1a_remaining_samples, V90_INFO1A_PAYLOAD_BITS) == PHASE2_INFO1_OK,
				      "INFO1 wait init");
				deadline = i + event.short_step.info1a_remaining_samples;
				CHECK(deadline == d.reversal + 20000, "wrong original epoch");
				receiving_info1 = true;
				original_peer = d.ranging.info0_exchange.peer_info0;
			} else if (event.short_step.event == V92_SHORT_RANGING_RECOVER_FULL_PHASE2) {
				CHECK(missing != 2 && i == d.reversal + 20000 && d_out == 0,
				      "wrong reversal deadline");
				recovering = true;
				recovery = i;
				resume = i + 16000;
				original_peer = d.ranging.info0_exchange.peer_info0;
			}
			if (event.kind == PHASE2_PROBE_RECEIVE_READY) {
				d.complete = i;
				d.last = event;
			}
		}
		if (recovering && i > recovery && i < resume && d_out != 0)
			++digital_nonzero_before_resume;
		a_line[cursor] = codec_round_trip(a_out, law ? TEST_CODEC_PCMA : TEST_CODEC_PCMU);
		d_line[cursor] = codec_round_trip(d_out, law ? TEST_CODEC_PCMA : TEST_CODEC_PCMU);
		if (d.complete != UINT_MAX && a.complete != UINT_MAX)
			break;
	}
	CHECK(recovering && a.complete != UINT_MAX && d.complete != UINT_MAX &&
	      a.last.kind == PHASE2_PROBE_TRANSMIT_READY && d.last.kind == PHASE2_PROBE_RECEIVE_READY,
	      "full recovery did not reach real probe boundary");
	CHECK((missing == 1) == (digital_nonzero_before_resume == 0), "wrong recovery Tone-B behavior");
	CHECK(memcmp(&original_peer, &d.ranging.info0_exchange.peer_info0, sizeof(original_peer)) == 0,
	      "full recovery altered received INFO0");
	return true;
}

enum repeat_kind { REPEAT_BAD_SEMANTICS, REPEAT_BAD_CRC, REPEAT_LATE_ACK, REPEAT_VALID };

static bool check_short_repeat(unsigned law, bool digital, enum repeat_kind kind, unsigned timing)
{
	bool near_timeout = timing == 1, during_guard = timing == 2;
	struct test_endpoint local;
	struct phase2_info0_exchange peer;
	struct v92_info0a a = {
		.v90 = analog_capabilities(), .v92_capable = true,
		.short_phase2_requested = true,
	};
	struct v92_info0d d = {
		.v90 = digital_capabilities(law ? TEST_CODEC_PCMA : TEST_CODEC_PCMU),
		.v92_capable = true, .short_phase2_requested = true,
	};
	uint8_t payload[V90_INFO0D_PAYLOAD_BYTES] = {0};
	size_t bits = digital ? V90_INFO0A_PAYLOAD_BITS : V90_INFO0D_PAYLOAD_BITS;
	unsigned started = UINT_MAX, decided = UINT_MAX, terminal = UINT_MAX;
	unsigned first_reversal = UINT_MAX, last_reversal = UINT_MAX, deadline = UINT_MAX;
	bool synchronized = false, crc_changed = false;
	enum pcm_info0_recovery_event decision = PCM_INFO0_RECOVERY_NO_EVENT;
	union pcm_info0_record original = {0};

	a.v90.acknowledges_peer_info0 = kind != REPEAT_LATE_ACK;
	d.v90.common.acknowledges_peer_info0 = kind != REPEAT_LATE_ACK;
	CHECK(init_endpoint(&local, digital, law, true), "repeat local init");
	CHECK((digital ? v92_info0a_encode(&a, payload, sizeof(payload)) :
		v92_info0d_encode(&d, payload, sizeof(payload))) == V92_INFO0_OK &&
	      phase2_info0_exchange_init(&peer, digital ? PHASE2_SIGNAL_A : PHASE2_SIGNAL_B,
		12000, payload, bits, digital ? V90_INFO0D_PAYLOAD_BITS : V90_INFO0A_PAYLOAD_BITS) ==
		PHASE2_INFO0_EXCHANGE_OK, "repeat peer init");
	for (unsigned i = 0; i < 50000; ++i) {
		struct phase2_info0_exchange_event peer_event;
		struct pcm_phase2_ranging_event event;
		struct v92_short_ranging before = local.ranging.short_control;
		bool active = local.ranging.path == PCM_RANGING_SHORT_ACTIVE;
		int16_t peer_out, out;

		CHECK(phase2_info0_exchange_sample(&peer, 0, &peer_out, &peer_event) ==
		      PHASE2_INFO0_EXCHANGE_OK &&
		      pcm_phase2_ranging_sample(&local.ranging,
			codec_round_trip(peer_out, law ? TEST_CODEC_PCMA : TEST_CODEC_PCMU),
			&out, &event) == PCM_PHASE2_RANGING_OK,
		      "repeat sample %u failed law=%u digital=%u kind=%u", i, law, digital, kind);
		if (event.short_step.reverse_carrier) {
			if (first_reversal == UINT_MAX)
				first_reversal = i;
			last_reversal = i;
			if (digital && deadline == UINT_MAX)
				deadline = i + V92_SHORT_RANGING_TIMEOUT_SAMPLES;
		}
		if (!digital && deadline == UINT_MAX && local.ranging.path == PCM_RANGING_SHORT_ACTIVE)
			deadline = i + V92_SHORT_RANGING_TIMEOUT_SAMPLES - local.ranging.short_control.elapsed_samples;
		if (started != UINT_MAX && event.exchange_event.recovery_event == PCM_INFO0_RECOVERY_STARTED)
			synchronized = true;
		if (started != UINT_MAX && event.exchange_event.recovery_event >= PCM_INFO0_RECOVERY_ACCEPTED) {
			decided = i;
			decision = event.exchange_event.recovery_event;
		}
		if (active && started != UINT_MAX && kind != REPEAT_VALID &&
		    before.phase != V92_SHORT_RANGING_DIGITAL_GUARD &&
		    event.short_step.event == V92_SHORT_RANGING_NO_EVENT) {
			CHECK(local.ranging.path == PCM_RANGING_SHORT_ACTIVE &&
			      local.ranging.short_control.elapsed_samples == before.elapsed_samples + 1,
			      "unvalidated/late INFO0 reset clock at %u: path %u elapsed %u -> %u",
			      i, local.ranging.path, before.elapsed_samples, local.ranging.short_control.elapsed_samples);
			CHECK(!event.short_step.reverse_carrier, "rejected/late INFO0 restarted ranging");
			if (digital && before.phase == V92_SHORT_RANGING_DIGITAL_WAIT)
				CHECK(out == 0, "rejected/late INFO0 broke digital silence at %u", i);
		}
		if (event.short_step.event != V92_SHORT_RANGING_NO_EVENT) {
			terminal = i;
			CHECK(event.short_step.event == (digital ? V92_SHORT_RANGING_RECOVER_FULL_PHASE2 :
			      V92_SHORT_RANGING_REQUEST_FULL_RETRAIN), "false ranging completion from INFO0");
			break;
		}
		if (started == UINT_MAX && local.ranging.path == PCM_RANGING_SHORT_ACTIVE &&
		    (during_guard ? local.ranging.short_control.tone_samples >= 100 :
		     local.ranging.short_control.elapsed_samples >= (near_timeout ? 19600 : 1000))) {
			original = local.ranging.info0_exchange.peer_info0;
			a.v90.acknowledges_peer_info0 = true;
			d.v90.common.acknowledges_peer_info0 = true;
			if (kind == REPEAT_BAD_SEMANTICS) {
				a.v90.maximum_symbol_rate_difference_steps++;
				d.v90.nominal_transmit_power++;
			}
			CHECK((digital ? v92_info0a_encode(&a, payload, sizeof(payload)) :
				v92_info0d_encode(&d, payload, sizeof(payload))) == V92_INFO0_OK &&
			      phase2_info0_exchange_restart_info(&peer, payload, bits) ==
				PHASE2_INFO0_EXCHANGE_OK, "start repeat");
			started = i;
		}
		if (started != UINT_MAX && kind == REPEAT_BAD_CRC && !crc_changed &&
		    peer.info_transmitter.frame.bit_index == V34_PHASE2_INFO_PREFIX_BITS + bits) {
			/* The original payload has already gone onto the wire. Send the
			 * CRC of a different payload without touching the receiver. */
			struct v34_phase2_info_generator replacement;
			/* Two changed bits retain the following carrier polarity, so
			 * this tests a bad frame rather than also injecting a real edge. */
			payload[0] ^= 0xc0;
			CHECK(v34_phase2_info_generator_init(&replacement, payload, bits) == V34_PHASE2_INFO_OK,
			      "corrupt wire CRC fixture");
			replacement.bit_index = peer.info_transmitter.frame.bit_index;
			peer.info_transmitter.frame = replacement;
			crc_changed = true;
		}
	}
	if (near_timeout) {
		CHECK(synchronized && decided == UINT_MAX && terminal == deadline &&
		      local.ranging.info0_exchange.state == PCM_PHASE2_INFO0_VERIFY_REPEAT,
		      "in-flight record postponed the original timeout");
		return true;
	}
	CHECK(synchronized && decided != UINT_MAX && terminal != UINT_MAX, "repeat not exercised");
	CHECK(decision == (kind == REPEAT_VALID ? PCM_INFO0_RECOVERY_ACCEPTED :
	      kind == REPEAT_LATE_ACK ? PCM_INFO0_LATE_ACK_RECEIVED : PCM_INFO0_RECOVERY_REJECTED),
	      "wrong repeat decision %u", decision);
	if (kind != REPEAT_VALID)
		CHECK(terminal == deadline && first_reversal == last_reversal,
		      "repeat moved original timeout/reversal");
	else if (digital)
		CHECK(last_reversal >= decided + (during_guard ? 1 : PHASE2_RANGING_INITIAL_TONE_SAMPLES) &&
		      terminal == last_reversal + V92_SHORT_RANGING_TIMEOUT_SAMPLES,
		      "validated repeat did not start a new guarded ranging turn");
	else
		CHECK(terminal == deadline, "unchanged local INFO0 should retain its deadline");
	if (kind < REPEAT_LATE_ACK)
		CHECK(memcmp(&original, &local.ranging.info0_exchange.peer_info0, sizeof(original)) == 0,
		      "rejected repeat changed peer record");
	if (kind == REPEAT_BAD_CRC)
		CHECK(crc_changed, "bad CRC was not transmitted");
	if (during_guard)
		CHECK(first_reversal > decided, "reversed while peer INFO0 still owned the carrier");
	return true;
}

static bool response_repeat(
	unsigned law, unsigned offset, enum repeat_kind kind, bool new_turn)
{
	struct test_endpoint local;
	struct phase2_info0_exchange peer;
	struct v92_info0d d = {
		.v90 = digital_capabilities(law ? TEST_CODEC_PCMA : TEST_CODEC_PCMU),
		.v92_capable = true, .short_phase2_requested = true,
	};
	uint8_t payload[V90_INFO0D_PAYLOAD_BYTES] = {0};
	unsigned reversed = UINT_MAX, noticed = UINT_MAX, started = UINT_MAX;
	unsigned synchronized = UINT_MAX, decided = UINT_MAX, responded = UINT_MAX;
	unsigned terminal = UINT_MAX;
	unsigned deadline = UINT_MAX, fresh_edge = UINT_MAX;
	bool crc_changed = false;
	enum v92_short_ranging_event result = V92_SHORT_RANGING_NO_EVENT;

	d.v90.common.acknowledges_peer_info0 = kind != REPEAT_LATE_ACK;
	CHECK(init_endpoint(&local, false, law, true), "analog init");
	CHECK(v92_info0d_encode(&d, payload, sizeof(payload)) == V92_INFO0_OK &&
	      phase2_info0_exchange_init(&peer, PHASE2_SIGNAL_B, 12000, payload,
		V90_INFO0D_PAYLOAD_BITS, V90_INFO0A_PAYLOAD_BITS) == PHASE2_INFO0_EXCHANGE_OK,
	      "peer init");
	for (unsigned i = 0; i < 26000; ++i) {
		struct phase2_info0_exchange_event peer_event;
		struct pcm_phase2_ranging_event event;
		enum phase2_info0_transmit_phase before = local.ranging.info0_exchange.transport.transmit_phase;
		int16_t peer_out, out;

		if (new_turn && decided != UINT_MAX && fresh_edge == UINT_MAX && i == decided + 500) {
			CHECK(peer.transmit_phase == PHASE2_INFO0_TONE &&
			      v34_phase2_tone_transmitter_reverse(&peer.tone_transmitter) ==
			      V34_PHASE2_TONE_OK, "fresh ranging edge after repeated INFO0");
			fresh_edge = i;
		}
		if (reversed == UINT_MAX && local.ranging.path == PCM_RANGING_SHORT_ACTIVE &&
		    local.ranging.short_control.elapsed_samples >= 1000) {
			CHECK(v34_phase2_tone_transmitter_reverse(&peer.tone_transmitter) ==
			      V34_PHASE2_TONE_OK, "reverse peer B");
			reversed = i;
		}
		if (noticed != UINT_MAX && started == UINT_MAX && i == noticed + offset + 1) {
			d.v90.common.acknowledges_peer_info0 = true;
			if (kind == REPEAT_BAD_SEMANTICS)
				++d.v90.nominal_transmit_power;
			CHECK(v92_info0d_encode(&d, payload, sizeof(payload)) == V92_INFO0_OK &&
			      phase2_info0_exchange_restart_info(&peer, payload, V90_INFO0D_PAYLOAD_BITS) ==
				PHASE2_INFO0_EXCHANGE_OK, "repeat during pending A response");
			started = i;
		}
		CHECK(phase2_info0_exchange_sample(&peer, 0, &peer_out, &peer_event) ==
		      PHASE2_INFO0_EXCHANGE_OK &&
		      pcm_phase2_ranging_sample(&local.ranging,
			codec_round_trip(peer_out, law ? TEST_CODEC_PCMA : TEST_CODEC_PCMU),
			&out, &event) == PCM_PHASE2_RANGING_OK,
		      "response repeat sample %u failed law=%u offset=%u kind=%u", i, law, offset, kind);
		if (started != UINT_MAX && kind == REPEAT_BAD_CRC && !crc_changed &&
		    peer.info_transmitter.frame.bit_index == V34_PHASE2_INFO_PREFIX_BITS + V90_INFO0D_PAYLOAD_BITS) {
			struct v34_phase2_info_generator replacement;
			/* Keep the transmitted payload and carrier parity, but emit the
			 * CRC of a two-bit-different record. Do not edit the receiver. */
			payload[0] ^= 0xc0;
			CHECK(v34_phase2_info_generator_init(&replacement, payload,
			      V90_INFO0D_PAYLOAD_BITS) == V34_PHASE2_INFO_OK, "corrupt wire CRC");
			replacement.bit_index = peer.info_transmitter.frame.bit_index;
			peer.info_transmitter.frame = replacement;
			crc_changed = true;
		}
		/* Date the deadline from the actual local waveform handoff, not
		 * from the counter whose origin this test is supposed to check. */
		if (before != PHASE2_INFO0_TONE &&
		    local.ranging.info0_exchange.transport.transmit_phase == PHASE2_INFO0_TONE)
			deadline = i + V92_SHORT_RANGING_TIMEOUT_SAMPLES;
		if (noticed == UINT_MAX && local.ranging.path == PCM_RANGING_SHORT_ACTIVE &&
		    local.ranging.short_control.phase == V92_SHORT_RANGING_ANALOG_DELAY)
			noticed = i;
		if (event.exchange_event.recovery_event == PCM_INFO0_RECOVERY_STARTED)
			synchronized = i;
		if (event.exchange_event.recovery_event >= PCM_INFO0_RECOVERY_ACCEPTED) {
			decided = i;
			CHECK(event.exchange_event.recovery_event == (kind == REPEAT_VALID ? PCM_INFO0_RECOVERY_ACCEPTED :
			      kind == REPEAT_LATE_ACK ? PCM_INFO0_LATE_ACK_RECEIVED : PCM_INFO0_RECOVERY_REJECTED),
			      "wrong repeat decision");
		}
		if (event.short_step.reverse_carrier)
			responded = i;
		if (event.short_step.event != V92_SHORT_RANGING_NO_EVENT) {
			terminal = i;
			result = event.short_step.event;
			break;
		}
	}
	CHECK(reversed != UINT_MAX && noticed != UINT_MAX && started != UINT_MAX,
	      "response overlap not exercised");
	CHECK(result != V92_SHORT_RANGING_TRANSMIT_INFO1A ||
	      local.ranging.info0_exchange.state != PCM_PHASE2_INFO0_VERIFY_REPEAT,
	      "advanced to INFO1a while a repeated INFO0 still owns the receiver");
	if (offset == 160) {
		CHECK(synchronized == UINT_MAX && result == V92_SHORT_RANGING_TRANSMIT_INFO1A &&
		      absolute_difference(responded - reversed, 320) <= 8 && terminal == responded + 79,
		      "late, not-yet-detected framing changed the original physical response");
	} else {
		CHECK(synchronized != UINT_MAX && decided != UINT_MAX,
		      "interrupted turn discarded the INFO0 decision");
		CHECK(kind != REPEAT_BAD_CRC || crc_changed, "CRC rejection did not exercise corrupt wire CRC");
		if (new_turn)
			CHECK(result == V92_SHORT_RANGING_TRANSMIT_INFO1A && fresh_edge != UINT_MAX &&
			      responded > fresh_edge && absolute_difference(responded - fresh_edge, 320) <= 8 &&
			      terminal == responded + 79, "fresh ranging turn has wrong response timing");
		else
			CHECK(result == V92_SHORT_RANGING_REQUEST_FULL_RETRAIN && terminal == deadline,
			      "interrupted turn changed the original timeout");
	}
	return true;
}

static bool ack_repeat(unsigned law, bool digital)
{
	struct test_endpoint local;
	struct phase2_info0_exchange peer;
	struct v92_info0a a = {
		.v90 = analog_capabilities(), .v92_capable = true, .short_phase2_requested = true,
	};
	struct v92_info0d d = {
		.v90 = digital_capabilities(law ? TEST_CODEC_PCMA : TEST_CODEC_PCMU),
		.v92_capable = true, .short_phase2_requested = true,
	};
	uint8_t payload[V90_INFO0D_PAYLOAD_BYTES] = {0};
	size_t bits = digital ? V90_INFO0A_PAYLOAD_BITS : V90_INFO0D_PAYLOAD_BITS;
	unsigned repeats = 0, start = UINT_MAX, local_end = UINT_MAX;
	unsigned ack_started = UINT_MAX, last_reversal = UINT_MAX, decoded_ack = 0;
	unsigned terminal = UINT_MAX;
	int16_t last_out = 0;
	bool pending = false;

	CHECK(init_endpoint(&local, digital, law, true), "local init");
	CHECK((digital ? v92_info0a_encode(&a, payload, sizeof(payload)) :
		v92_info0d_encode(&d, payload, sizeof(payload))) == V92_INFO0_OK &&
	      phase2_info0_exchange_init(&peer, digital ? PHASE2_SIGNAL_A : PHASE2_SIGNAL_B,
		12000, payload, bits, digital ? V90_INFO0D_PAYLOAD_BITS : V90_INFO0A_PAYLOAD_BITS) ==
		PHASE2_INFO0_EXCHANGE_OK, "peer init");
	for (unsigned i = 0; i < 32000; ++i) {
		struct phase2_info0_exchange_event peer_event;
		struct pcm_phase2_ranging_event event;
		enum phase2_info0_transmit_phase before = local.ranging.info0_exchange.transport.transmit_phase;
		int16_t peer_out, out;
		bool should_start = start == UINT_MAX ? local.ranging.path == PCM_RANGING_SHORT_ACTIVE &&
			local.ranging.short_control.elapsed_samples >= 1000 :
			repeats < 3 && peer.transmit_phase == PHASE2_INFO0_TONE;

		if (should_start) {
			++repeats;
			if (start == UINT_MAX)
				start = i;
			a.v90.acknowledges_peer_info0 = repeats == 3;
			d.v90.common.acknowledges_peer_info0 = repeats == 3;
			CHECK((digital ? v92_info0a_encode(&a, payload, sizeof(payload)) :
				v92_info0d_encode(&d, payload, sizeof(payload))) == V92_INFO0_OK &&
			      phase2_info0_exchange_restart_info(&peer, payload, bits) ==
				PHASE2_INFO0_EXCHANGE_OK, "repeat INFO0");
		}
		CHECK(phase2_info0_exchange_sample(&peer, last_out, &peer_out, &peer_event) ==
		      PHASE2_INFO0_EXCHANGE_OK &&
		      pcm_phase2_ranging_sample(&local.ranging,
			codec_round_trip(peer_out, law ? TEST_CODEC_PCMA : TEST_CODEC_PCMU),
			&out, &event) == PCM_PHASE2_RANGING_OK,
		      "ACK repeat sample %u failed law=%u digital=%u info=%u path=%u", i, law, digital,
		      local.ranging.info0_exchange.state, local.ranging.path);
		last_out = codec_round_trip(out, law ? TEST_CODEC_PCMA : TEST_CODEC_PCMU);
		if (start != UINT_MAX && peer_event.info_event.type == V34_PHASE2_INFO_FRAME_RECEIVED) {
			struct v92_info0a actual_a;
			struct v92_info0d actual_d;
			CHECK((digital ? v92_info0d_decode(peer_event.info_event.payload,
				sizeof(peer_event.info_event.payload), peer_event.info_event.payload_bit_count, &actual_d) :
				v92_info0a_decode(peer_event.info_event.payload, sizeof(peer_event.info_event.payload),
				peer_event.info_event.payload_bit_count, &actual_a)) == V92_INFO0_OK,
			      "peer failed to decode local ACK");
			CHECK(digital ? actual_d.v90.common.acknowledges_peer_info0 :
			      actual_a.v90.acknowledges_peer_info0, "repeated local record lacks ACK");
			++decoded_ack;
		}
		if (start != UINT_MAX) {
			enum phase2_info0_transmit_phase after = local.ranging.info0_exchange.transport.transmit_phase;
			pending |= local.ranging.path == PCM_RANGING_SHORT_PENDING;
			if (before == PHASE2_INFO0_TONE && after == PHASE2_INFO0_SEQUENCE)
				ack_started = i;
			if (before != PHASE2_INFO0_TONE && after == PHASE2_INFO0_TONE)
				local_end = i;
		}
		if (event.short_step.reverse_carrier)
			last_reversal = i;
		if (event.short_step.event != V92_SHORT_RANGING_NO_EVENT) {
			terminal = i;
			break;
		}
	}
	CHECK(repeats == 3 && pending && ack_started != UINT_MAX && local_end != UINT_MAX && decoded_ack,
	      "new local ACK exchange was not completed on the wire");
	CHECK(!digital || (last_reversal > local_end && last_reversal - local_end >= 400),
	      "digital reversal skipped the new INFO0 tone guard");
	CHECK(digital || terminal == local_end + V92_SHORT_RANGING_TIMEOUT_SAMPLES,
	      "analogue deadline is not dated from actual INFO0 line handoff: end=%u timeout=%u", local_end, terminal);
	return true;
}

static bool test_agreements(void)
{
	const unsigned delays[] = { 1, 13, 257 }, skews[] = { 0, 137, 619 };
	for (unsigned law = 0; law < 2; ++law)
		for (unsigned delay = 0; delay < ARRAY_LENGTH(delays); ++delay)
			for (unsigned skew = 0; skew < ARRAY_LENGTH(skews); ++skew)
				for (unsigned mute = 0; mute < 2; ++mute)
					for (unsigned offers = 0; offers < 4; ++offers)
						CHECK(check_pair(law, delays[delay], skews[skew], mute, offers),
						      "short agreement matrix");
	return true;
}

static bool test_recovery(void)
{
	const unsigned delays[] = { 1, 13, 257 };
	for (unsigned law = 0; law < 2; ++law)
		for (unsigned delay = 0; delay < ARRAY_LENGTH(delays); ++delay)
			for (unsigned missing = 0; missing < 3; ++missing)
				CHECK(missing_short_signal(law, delays[delay], missing), "missing signal recovery");
	return true;
}

static bool test_repeats(void)
{
	for (unsigned law = 0; law < 2; ++law)
		for (unsigned digital = 0; digital < 2; ++digital)
			for (unsigned kind = 0; kind < 4; ++kind)
				for (unsigned timing = 0; timing < (digital ? 3u : 2u); ++timing)
					CHECK(check_short_repeat(law, digital, kind, timing), "repeat/deadline matrix");
	return true;
}

static bool test_response_overlap(void)
{
	const unsigned offsets[] = { 0, 40, 80, 160 };
	for (unsigned law = 0; law < 2; ++law)
		for (unsigned kind = 0; kind < 4; ++kind)
			for (unsigned i = 0; i < ARRAY_LENGTH(offsets); ++i)
				for (unsigned fresh = 0; fresh < 2; ++fresh)
					CHECK(response_repeat(law, offsets[i], kind, fresh), "response overlap matrix");
	return true;
}

static bool test_repeated_ack(void)
{
	for (unsigned law = 0; law < 2; ++law)
		for (unsigned digital = 0; digital < 2; ++digital)
			CHECK(ack_repeat(law, digital), "new local ACK matrix");
	return true;
}

static bool test_validation(void)
{
	struct test_endpoint endpoint;
	struct pcm_phase2_ranging before;
	struct pcm_phase2_ranging_event event = { .kind = PHASE2_ROUND_TRIP_DELAY_MEASURED };
	int16_t sample = 1234;

	CHECK(init_endpoint(&endpoint, true, 0, true), "validation init");
	before = endpoint.ranging;
	CHECK(!pcm_phase2_ranging_recover_short_info1(NULL) &&
	      !pcm_phase2_ranging_recover_short_info1(&endpoint.ranging) &&
	      memcmp(&before, &endpoint.ranging, sizeof(before)) == 0,
	      "INFO1 recovery accepted an unfinished agreement");
	for (unsigned invalid = 0; invalid < 3; ++invalid) {
		endpoint.ranging = before;
		if (invalid == 0)
			endpoint.ranging.path = (enum pcm_ranging_path)99;
		else if (invalid == 1)
			endpoint.ranging.actual_tone_samples = PHASE2_RANGING_INITIAL_TONE_SAMPLES + 1;
		else
			endpoint.ranging.since_info0_samples = V92_SHORT_RANGING_TIMEOUT_SAMPLES + 1;
		struct pcm_phase2_ranging invalid_before = endpoint.ranging;
		CHECK(!pcm_phase2_ranging_is_valid(&endpoint.ranging) &&
		      pcm_phase2_ranging_sample(&endpoint.ranging, 0, &sample, &event) ==
			PCM_PHASE2_RANGING_INVALID_ARGUMENT &&
		      memcmp(&invalid_before, &endpoint.ranging, sizeof(invalid_before)) == 0 &&
		      sample == 1234 && event.kind == PHASE2_ROUND_TRIP_DELAY_MEASURED,
		      "invalid short-ranging state mutated state or output");
	}
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "short/full INFO0 agreements", test_agreements },
		{ "short missing-signal recovery", test_recovery },
		{ "repeated INFO0 and original deadlines", test_repeats },
		{ "INFO0 during analogue response", test_response_overlap },
		{ "new local INFO0 ACK", test_repeated_ack },
		{ "short-ranging validation", test_validation },
	};
	unsigned failures = 0;
	for (unsigned i = 0; i < ARRAY_LENGTH(tests); ++i) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[i].name);
		if (!tests[i].run()) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[i].name);
			++failures;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[i].name);
		}
	}
	return failures != 0;
}
