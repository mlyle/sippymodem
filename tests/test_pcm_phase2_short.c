// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <pcm_phase2.h>
#include <pcm_phase2_info1_handoff.h>

#include "test.h"

#include <limits.h>
#include <string.h>

enum { TEST_DELAY_MAX = 257 };

enum test_upstream {
	TEST_CONVENTIONAL_LOW,
	TEST_CONVENTIONAL_HIGH,
	TEST_PCM,
};

static int16_t media_sample(unsigned law, int16_t sample)
{
	return law ? g711_decode_pcma(g711_encode_pcma(sample)) :
		g711_decode_pcmu(g711_encode_pcmu(sample));
}

static bool check_handoff(const struct pcm_phase2 *owner,
	const struct pcm_info1_selection *expected)
{
	struct pcm_info1_selection actual;
	uint8_t wanted[V90_INFO1A_PAYLOAD_BYTES], received[V90_INFO1A_PAYLOAD_BYTES];
	struct v34_phase2_probe_receiver_measurement measure, before;
	int16_t gain = 123;

	CHECK(pcm_phase2_info1_handoff(owner, &actual) == PCM_INFO1_OK &&
	      pcm_info1_encode_short(expected, wanted, sizeof(wanted)) == PCM_INFO1_OK &&
	      pcm_info1_encode_short(&actual, received, sizeof(received)) == PCM_INFO1_OK &&
	      memcmp(wanted, received, sizeof(wanted)) == 0,
	      "short INFO1 handoff changed the actual wire record");
	memset(&measure, 0xa5, sizeof(measure));
	before = measure;
	CHECK(pcm_phase2_measure_remote_probe(owner, &measure) == PCM_PHASE2_INVALID_STATE &&
	      memcmp(&measure, &before, sizeof(measure)) == 0 &&
	      pcm_phase2_receive_gain(owner, &gain) == PCM_PHASE2_INVALID_STATE && gain == 123,
	      "short owner invented probe/gain");
	return true;
}

static bool run_owner(unsigned law, unsigned delay, unsigned skew,
	bool mute_first, enum test_upstream mode, bool drop_info1)
{
	struct pcm_phase2 endpoints[2];
	struct pcm_phase2 *a = &endpoints[0], *d = &endpoints[1];
	struct v92_info0a a_info = {.v92_capable = true, .short_phase2_requested = true};
	struct v92_info0d d_info = {
		.v90 = {.pcm_law = law ? V90_PCM_A_LAW : V90_PCM_MU_LAW,
			.nominal_transmit_power = 9, .maximum_transmit_power = 18},
		.v92_capable = true, .short_phase2_requested = true,
	};
	struct pcm_info1_selection selection = {
		.startup = {PCM_PROTOCOL_V92, PCM_PHASE2_SHORT},
		.upstream = mode == TEST_PCM ? PCM_UPSTREAM_PCM : PCM_UPSTREAM_V34,
	};
	int16_t a_line[TEST_DELAY_MAX] = {0}, d_line[TEST_DELAY_MAX] = {0};
	bool initial_complete = false, a_ready = false, d_ready = false;
	bool received = false, sent = false;
	bool recovery_checked = false;
	unsigned info1_deadline = UINT_MAX, recovery = UINT_MAX, resume = UINT_MAX;
	bool a_probe = false, d_probe = false;

	if (mode == TEST_PCM)
		selection.analog.pcm = (struct v92_pcm_info1a){
			.maximum_total_coefficients = 192, .maximum_section_coefficients = 128,
			.local_md_length_276_symbol_units = 1, .training_ucode = 104,
		};
	else {
		selection.analog.conventional = (struct v90_info1a){
			.training_ucode = 104, .upstream_symbol_rate = V34_SYMBOL_RATE_3200,
			.frequency_offset_1_50_hz_units = -512,
		};
		selection.short_high_carrier = mode == TEST_CONVENTIONAL_HIGH;
	}
	CHECK(pcm_phase2_init_v92_analog(a, 12000, &a_info) == PCM_PHASE2_OK &&
	      pcm_phase2_init_v92_digital(d, 12000, &d_info) == PCM_PHASE2_OK,
	      "owner short initialization");
	for (unsigned i = 0; i < (drop_info1 ? 80000u : 32000u); ++i) {
		struct pcm_phase2_event ae = {0}, de = {0};
		int16_t a_out = 0, d_out = 0;
		unsigned cursor = i % delay;
		if (i == resume) {
			struct v92_info0d actual_peer;

			CHECK(pcm_phase2_peer_v92_info0d(a, &actual_peer) == PCM_PHASE2_OK &&
			      pcm_phase2_init_retrain_v92_analog(a, 12000,
				      &a_info, &actual_peer, 0) == PCM_PHASE2_OK,
			      "owner full-retrain response initialization");
			a_ready = false;
		}
		if (!a_ready)
			CHECK(pcm_phase2_sample(a, d_line[cursor], &a_out, &ae) == PCM_PHASE2_OK &&
			      !pcm_phase2_has_failed(a), "analog sample %u stage=%u failure=%u",
			      i, a->stage, a->failure);
		if (!d_ready && i >= skew)
			CHECK(pcm_phase2_sample(d, a_line[cursor], &d_out, &de) == PCM_PHASE2_OK &&
			      !pcm_phase2_has_failed(d), "digital sample %u stage=%u failure=%u",
			      i, d->stage, d->failure);
		CHECK(recovery != UINT_MAX ||
		      a->stage == PCM_PHASE2_RANGING || a->stage == PCM_PHASE2_INFO1,
		      "short analogue entered full probe");
		CHECK(recovery != UINT_MAX || de.kind == PCM_PHASE2_EVENT_SHORT_RECOVERY_STARTED ||
		      d->stage == PCM_PHASE2_RANGING || d->stage == PCM_PHASE2_INFO1,
		      "short digital entered full probe");
		CHECK(ae.kind != PCM_PHASE2_EVENT_LOCAL_INFO1D_SENT &&
		      de.kind != PCM_PHASE2_EVENT_LOCAL_INFO1D_SENT,
		      "short owner generated INFO1d");
		if (pcm_phase2_is_ready_for_info1(a)) {
			struct pcm_phase2 saved = *a;

			CHECK(pcm_phase2_start_short_analog_info1(a, NULL) ==
				      PCM_PHASE2_INVALID_ARGUMENT &&
			      memcmp(a, &saved, sizeof(saved)) == 0,
			      "invalid short INFO1 selection mutated owner");
			CHECK(pcm_phase2_start_analog_info1(a) == PCM_PHASE2_INVALID_STATE &&
			      memcmp(a, &saved, sizeof(saved)) == 0,
			      "full analog INFO1 API accepted short-ranging storage");
			CHECK(pcm_phase2_start_short_analog_info1(a, &selection) == PCM_PHASE2_OK,
			      "short analog INFO1a boundary");
		}
		if (!recovery_checked && d->stage == PCM_PHASE2_INFO1) {
			struct pcm_phase2 invalid = *d, fresh;

			CHECK(pcm_phase2_start_short_analog_info1(d, &selection) ==
				      PCM_PHASE2_INVALID_STATE &&
			      memcmp(d, &invalid, sizeof(invalid)) == 0,
			      "short analog INFO1 API changed digital owner");
			--invalid.state.info1.control.short_receive_budget_samples;
			CHECK(!pcm_phase2_is_valid(&invalid),
			      "short INFO1 accepted a budget unrelated to the original ranging clock");
			invalid = *d;
			CHECK(pcm_phase2_init_v92_digital(&fresh, 12000, &d_info) == PCM_PHASE2_OK,
			      "fresh owner for recovery validation");
			invalid.state.short_recovery = fresh.state.ranging;
			CHECK(!pcm_phase2_is_valid(&invalid),
			      "short INFO1 accepted unrelated unfinished recovery context");
			recovery_checked = true;
		}
		if (ae.kind == PCM_PHASE2_EVENT_LOCAL_INFO1A_SENT)
			sent = true;
		if (de.kind == PCM_PHASE2_EVENT_PEER_INFO1A_RECEIVED)
			received = true;
		if (drop_info1 && info1_deadline == UINT_MAX && d->stage == PCM_PHASE2_INFO1)
			info1_deadline = i + d->state.info1.control.short_receive_budget_samples;
		if (drop_info1 && de.kind == PCM_PHASE2_EVENT_SHORT_RECOVERY_STARTED) {
			CHECK(recovery == UINT_MAX && i == info1_deadline && d_out == 0,
			      "owner restarted original short INFO1 deadline: %u/%u", i, info1_deadline);
			CHECK(d->short_recovered && d->startup.phase2 == PCM_PHASE2_FULL &&
			      d->peer_info0.v92_analog.short_phase2_requested &&
			      d->state.ranging.path == PCM_RANGING_SHORT_FULL_RECOVERY,
			      "owner lost agreement during full recovery");
			recovery = i;
			/* The outer caller supplies the analogue retrain silence;
			 * this Phase 2 test does not manufacture another INFO0 offer. */
			resume = i + 16000;
		}
		if (!initial_complete && a->stage == PCM_PHASE2_RANGING) {
			if (mute_first)
				a_out = 0;
			initial_complete =
				a->state.ranging.info0_exchange.transport.transmit_phase ==
					PHASE2_INFO0_TONE;
		}
		if (drop_info1 && recovery == UINT_MAX && a->stage == PCM_PHASE2_INFO1)
			a_out = 0;
		a_line[cursor] = media_sample(law, a_out);
		d_line[cursor] = media_sample(law, d_out);
		if (!a_ready && pcm_phase2_is_ready_for_phase3(a))
			a_ready = true;
		if (!d_ready && pcm_phase2_is_ready_for_phase3(d))
			d_ready = true;
		if (a_ready && d_ready)
			break;
		if (recovery != UINT_MAX) {
			a_probe |= ae.kind == PCM_PHASE2_EVENT_INITIAL_PROBE_STARTED;
			d_probe |= de.kind == PCM_PHASE2_EVENT_INITIAL_PROBE_STARTED;
			if (a_probe && d_probe)
				break;
		}
	}
	if (drop_info1) {
		CHECK(recovery != UINT_MAX && a_probe && d_probe && sent && !received,
		      "owner INFO1 timeout did not recover full waveform path");
		CHECK(pcm_phase2_is_valid(a) && pcm_phase2_is_valid(d) &&
		      a->short_recovered && d->short_recovered &&
		      a->peer_info0.v92_digital.short_phase2_requested &&
		      d->peer_info0.v92_analog.short_phase2_requested,
		      "owner recovery changed actual INFO0 records");
		return true;
	}
	CHECK(a_ready && d_ready && sent && received, "short owner handoff incomplete");
	CHECK(!a->remote_probe_stored && !d->remote_probe_stored &&
	      !a->state.info1.digital_record_stored && !d->state.info1.digital_record_stored,
	      "owner fabricated full-startup records");
	CHECK(d->state.info1.analog.kind ==
		      (mode == TEST_PCM ? PCM_INFO1A_PCM : PCM_INFO1A_CONVENTIONAL) &&
	      d->state.info1.analog.short_high_carrier == (mode == TEST_CONVENTIONAL_HIGH),
	      "wrong actual selection");
	CHECK(pcm_phase2_is_valid(a) && pcm_phase2_is_valid(d), "invalid completed owner");
	return check_handoff(a, &selection) && check_handoff(d, &selection);
}

static bool test_short_exchange(void)
{
	const unsigned delays[] = {1, 13, 257}, skews[] = {0, 137, 619};

	for (unsigned law = 0; law < 2; ++law)
		for (unsigned delay = 0; delay < ARRAY_LENGTH(delays); ++delay)
			for (unsigned skew = 0; skew < ARRAY_LENGTH(skews); ++skew)
				for (unsigned mute = 0; mute < 2; ++mute)
					for (enum test_upstream mode = TEST_CONVENTIONAL_LOW;
					     mode <= TEST_PCM; ++mode)
						CHECK(run_owner(law, delays[delay], skews[skew],
							mute, mode, false),
						      "short exchange law=%u delay=%u skew=%u mute=%u mode=%u",
						      law, delays[delay], skews[skew], mute, mode);
	return true;
}

static bool test_info1_timeout_recovery(void)
{
	const unsigned delays[] = {1, 13, 257};

	for (unsigned law = 0; law < 2; ++law)
		for (unsigned delay = 0; delay < ARRAY_LENGTH(delays); ++delay)
			for (unsigned mute = 0; mute < 2; ++mute)
				CHECK(run_owner(law, delays[delay], 0, mute, TEST_PCM, true),
				      "INFO1 recovery law=%u delay=%u mute=%u",
				      law, delays[delay], mute);
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} cases[] = {
		{"short Phase 2 and actual INFO1 handoff", test_short_exchange},
		{"short INFO1 timeout resumes full probing", test_info1_timeout_recovery},
	};

	for (unsigned i = 0; i < ARRAY_LENGTH(cases); ++i) {
		fprintf(stderr, "[ RUN      ] %s\n", cases[i].name);
		if (!cases[i].run()) {
			fprintf(stderr, "[  FAILED  ] %s\n", cases[i].name);
			return 1;
		}
		fprintf(stderr, "[       OK ] %s\n", cases[i].name);
	}
	return 0;
}
