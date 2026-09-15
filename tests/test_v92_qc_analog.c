// SPDX-License-Identifier: BSD-3-Clause

#include "v92_qc_analog.h"
#include "v92_qc_digital.h"
#include "v8_calling.h"
#include "v8_answering.h"
#include "test.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int16_t codec(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMU ? g711_decode_pcmu(g711_encode_pcmu(sample)) :
		g711_decode_pcma(g711_encode_pcma(sample));
}

static bool init(struct v92_qc_analog *analog, struct v92_qc_digital *digital,
	enum g711_law law, bool answering, uint8_t index, uint8_t level)
{
	const struct v92_qc1_message a = {
		.sender = V90_ANALOG_MODEM, .acknowledgment = answering,
		.signal.qts_index = index,
	};
	const struct v92_qc1_message d = {
		.sender = V90_DIGITAL_MODEM, .acknowledgment = !answering,
		.signal.anspcm_level = level,
	};

	return v92_qc_analog_init(analog, law, &a, &d, 4096) &&
		v92_qc_digital_init(digital, law, &d, &a);
}

static bool terminal_unchanged(struct v92_qc_analog *exchange)
{
	struct v92_qc_analog saved = *exchange;
	int16_t output = 1234;

	CHECK(v92_qc_analog_sample(exchange, 4567, &output) == saved.stage &&
	      output == 1234 && memcmp(exchange, &saved, sizeof(saved)) == 0,
	      "terminal exchange consumed a Phase 2 sample");
	return true;
}

static bool test_pairs(void)
{
	static const unsigned delays[] = { 1, 37, 320 };
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned answering = 0; answering < 2; ++answering)
			for (unsigned choice = 0; choice < 2; ++choice)
				for (unsigned latency = 0; latency < ARRAY_LENGTH(delays); ++latency) {
					struct v92_qc_analog a;
					struct v92_qc_digital d;
					int16_t to_analog[320] = { 0 }, to_digital[320] = { 0 };
					unsigned tone_samples = 0, silence_samples = 0;
					unsigned delay = delays[latency];
					int16_t prior_downstream = 0;

					CHECK(init(&a, &d, law, answering, choice ? 14 : 0, choice ? 3 : 0), "init failed");
					for (unsigned i = 0; i < 8000; ++i) {
						struct v90_pcm_symbol down = { 0 };
						int16_t up = 0;
						enum v92_qc_analog_stage previous = a.stage;
						CHECK(v92_qc_analog_sample(&a, to_analog[i % delay], &up) != V92_QC_ANALOG_INVALID &&
						      v92_qc_digital_sample(&d, to_digital[i % delay], &down) != V92_QC_DIGITAL_INVALID &&
						      a.stage != V92_QC_ANALOG_V8 && d.stage != V92_QC_DIGITAL_V8,
						      "pair failed: law %u answer %u choice %u delay %u at %u stages %u/%u",
						      law, answering, choice, delay, i, a.stage, d.stage);
						if (previous == V92_QC_ANALOG_TONEQ && a.stage == V92_QC_ANALOG_TONEQ)
							tone_samples++;
						else
							CHECK(up == 0, "payload before handshake completion");
						if (a.stage == V92_QC_ANALOG_TRAIL_SILENCE ||
						    (a.stage == V92_QC_ANALOG_PHASE2 && previous == V92_QC_ANALOG_TRAIL_SILENCE))
							silence_samples++;
						/* Quantized loss and modest one-sample channel memory;
						 * neither receiver sees the generator's internal state. */
						double sample = choice ? 0.65 * down.sample + 0.05 * prior_downstream : down.sample;
						prior_downstream = down.sample;
						to_analog[i % delay] = codec(law, (int16_t)lrint(sample));
						to_digital[i % delay] = codec(law, up);
						if (a.stage == V92_QC_ANALOG_PHASE2 && d.stage == V92_QC_DIGITAL_PHASE2)
							break;
					}
					CHECK(a.stage == V92_QC_ANALOG_PHASE2 && d.stage == V92_QC_DIGITAL_PHASE2 &&
					      tone_samples >= 400 && silence_samples == 600 && terminal_unchanged(&a),
					      "pair did not preserve TONEq/silence: %u/%u", tone_samples, silence_samples);
				}
	return true;
}

static bool test_phase1(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned digital_caller = 0; digital_caller < 2; ++digital_caller) {
			struct v8_calling caller;
			struct v8_answering answer;
			struct v8_menu cm, jm;
			struct v92_qc_analog a;
			struct v92_qc_digital d;
			struct v92_qc1_message request = {
				.sender = digital_caller ? V90_DIGITAL_MODEM : V90_ANALOG_MODEM, .lapm = true,
			};
			struct v92_qc1_message response = {
				.sender = digital_caller ? V90_ANALOG_MODEM : V90_DIGITAL_MODEM, .acknowledgment = true,
			};
			int16_t wire[2][37] = { { 0 } };
			bool signal[2] = { false }, done[2] = { false };

			request.signal.qts_index = digital_caller ? 2 : 7;
			response.signal.qts_index = digital_caller ? 7 : 2;
			CHECK(v8_pcm_call_menu_init(&cm, digital_caller ? V8_PCM_DIGITAL : V8_PCM_ANALOG, 0, true) == V8_MENU_OK &&
			      v8_pcm_call_menu_init(&jm, digital_caller ? V8_PCM_ANALOG : V8_PCM_DIGITAL, 0, false) == V8_MENU_OK &&
			      v8_calling_init(&caller, &cm) == V8_CALLING_OK &&
			      v8_answering_init(&answer, &jm) == V8_ANSWERING_OK &&
			      v8_calling_enable_quick_connect(&caller, &request) == V8_CALLING_OK &&
			      v8_answering_enable_quick_connect(&answer, &response) == V8_ANSWERING_OK,
			      "V.8 QC fixture failed");
			for (unsigned i = 0; i < 40000 && !(done[0] && done[1]); ++i) {
				int16_t tx[2] = { 0 };
				for (unsigned side = 0; side < 2; ++side) {
					bool digital = side == 0 ? digital_caller : !digital_caller;
					int16_t rx = wire[side][i % 37];
					if (!signal[side]) {
						if (side == 0) {
							CHECK(v8_calling_process(&caller, &rx, &tx[side], 1) == V8_CALLING_OK,
							      "QC caller failed at %u", i);
							signal[side] = caller.state == V8_CALLING_QC_COMPLETE;
						} else {
							CHECK(v8_answering_process(&answer, &rx, &tx[side], 1) == V8_ANSWERING_OK,
							      "QC answerer failed at %u", i);
							signal[side] = answer.state == V8_ANSWERING_QC_COMPLETE;
						}
						if (signal[side]) {
							const struct v92_qc1_message *local = side == 0 ? &caller.qc_request : &answer.qc_response;
							const struct v92_qc1_message *peer = side == 0 ? &caller.qc_response : &answer.qc_request;
							CHECK(digital ? v92_qc_digital_init(&d, law, local, peer) :
								v92_qc_analog_init(&a, law, local, peer, 4096), "signal handoff failed");
						}
					} else if (digital) {
						struct v90_pcm_symbol out = { 0 };
						enum v92_qc_digital_stage result = v92_qc_digital_sample(&d, rx, &out);
						CHECK(result != V92_QC_DIGITAL_INVALID && result != V92_QC_DIGITAL_V8, "digital signal failed");
						tx[side] = out.sample;
						done[side] = result == V92_QC_DIGITAL_PHASE2;
					} else {
						enum v92_qc_analog_stage result = v92_qc_analog_sample(&a, rx, &tx[side]);
						CHECK(result != V92_QC_ANALOG_INVALID && result != V92_QC_ANALOG_V8, "analogue signal failed");
						done[side] = result == V92_QC_ANALOG_PHASE2;
					}
				}
				for (unsigned side = 0; side < 2; ++side)
					wire[side][i % 37] = codec(law, tx[1 - side]);
			}
			CHECK(done[0] && done[1] && !caller.negotiated && !answer.negotiated &&
			      caller.qc_request.lapm && !caller.qc_response.lapm && answer.qc_request.lapm,
			      "complete short Phase 1 did not preserve capabilities and both handoffs");
		}
	return true;
}

static bool feed_qts(struct v92_qc_analog *a)
{
	for (unsigned i = 0; i < V92_QTS_TOTAL_SYMBOLS; ++i) {
		struct v90_pcm_symbol signal;
		int16_t output;
		CHECK(v92_qts_symbol(a->qts.law, 7, i, &signal) &&
		      v92_qc_analog_sample(a, signal.sample, &output) ==
			      (i + 1 == V92_QTS_TOTAL_SYMBOLS ? V92_QC_ANALOG_ANSPCM : V92_QC_ANALOG_QTS) &&
		      output == 0, "QTS boundary moved at %u", i);
	}
	return true;
}

static bool test_tone_duration(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned duration = 0; duration < 2; ++duration) {
			struct v92_qc_analog a;
			struct v92_qc_digital d;
			struct v92_anspcm anspcm;
			unsigned tone_samples = 0, silence_samples = 0;
			unsigned end = duration ? 2 * V92_ANSPCM_REVERSAL_PERIOD + 500 : 260;

			CHECK(init(&a, &d, law, true, 7, 1) && feed_qts(&a) &&
			      v92_anspcm_init(&anspcm, law, 1), "init failed");
			for (unsigned i = 0; i < end + 1200; ++i) {
				struct v90_pcm_symbol signal = { 0 };
				int16_t output;
				enum v92_qc_analog_stage previous = a.stage;

				if (i < end)
					CHECK(v92_anspcm_sample(&anspcm, &signal), "ANSpcm failed");
				CHECK(v92_qc_analog_sample(&a, signal.sample, &output) != V92_QC_ANALOG_INVALID,
				      "receive failed");
				CHECK(i >= end || a.stage == V92_QC_ANALOG_ANSPCM || a.stage == V92_QC_ANALOG_TONEQ,
				      "phase reversal mistaken for ANSpcm drop at %u", i);
				if (previous == V92_QC_ANALOG_TONEQ && a.stage == V92_QC_ANALOG_TONEQ)
					tone_samples++;
				if (a.stage == V92_QC_ANALOG_TRAIL_SILENCE || a.stage == V92_QC_ANALOG_PHASE2) {
					CHECK(output == 0, "nonzero post-TONEq sample");
					silence_samples++;
				}
				if (a.stage == V92_QC_ANALOG_PHASE2)
					break;
			}
			CHECK(tone_samples >= 400 && (duration || tone_samples == 400) &&
			      silence_samples == 600 && a.stage == V92_QC_ANALOG_PHASE2,
			      "early/late drop moved TONEq or silence: %u/%u", tone_samples, silence_samples);
		}
	return true;
}

static bool test_fallback(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned variant = 0; variant < 5; ++variant) {
			struct v92_qc_analog answer, caller;
			struct v92_qc_digital d;
			CHECK(init(&answer, &d, law, true, 7, 1) && init(&caller, &d, law, false, 7, 1), "init failed");
			for (unsigned i = 0; i < 16000; ++i) {
				int16_t input = 0, output, other;
				struct v90_pcm_symbol qts;
				if (variant >= 2 && i < V92_QTS_TOTAL_SYMBOLS) {
					CHECK(v92_qts_symbol(law, 7, i, &qts), "QTS failed");
					input = qts.sample;
				} else if (variant == 1 || variant == 2 || (variant == 4 && i % 800 < 160))
					input = (int16_t)lrint(4096 * sin(2 * acos(-1.0) * (variant == 2 ? 1650 : 2100) * i / 8000));
				else if (variant == 3)
					input = 4096;
				CHECK(v92_qc_analog_sample(&answer, codec(law, input), &output) ==
					      (i == 15999 ? V92_QC_ANALOG_V8 :
						 variant >= 2 && i >= V92_QTS_TOTAL_SYMBOLS - 1 ? V92_QC_ANALOG_ANSPCM : V92_QC_ANALOG_QTS) &&
				      v92_qc_analog_sample(&caller, codec(law, input), &other) != V92_QC_ANALOG_INVALID &&
				      output == 0 && other == 0, "false qualification at %u variant %u", i, variant);
			}
			CHECK(caller.stage != V92_QC_ANALOG_V8 && caller.stage != V92_QC_ANALOG_PHASE2 &&
			      terminal_unchanged(&answer), "caller inherited answerer's fallback timer");
		}
	return true;
}

static bool test_late_answer(void)
{
	struct v92_qc_analog a, absent;
	struct v92_qc_digital d;
	struct v92_anspcm anspcm;
	int16_t output;
	CHECK(init(&a, &d, G711_LAW_PCMU, true, 7, 1) && feed_qts(&a) &&
	      v92_anspcm_init(&anspcm, G711_LAW_PCMU, 1), "init failed");
	while (a.answer_deadline > 250)
		CHECK(v92_qc_analog_sample(&a, 0, &output) == V92_QC_ANALOG_ANSPCM, "early timeout");
	absent = a;
	for (unsigned i = 0; i < 1800; ++i) {
		struct v90_pcm_symbol signal = { 0 };
		if (i < 600)
			CHECK(v92_anspcm_sample(&anspcm, &signal), "ANSpcm failed");
		CHECK(v92_qc_analog_sample(&a, signal.sample, &output) != V92_QC_ANALOG_INVALID &&
		      a.stage != V92_QC_ANALOG_V8 &&
		      v92_qc_analog_sample(&absent, 0, &output) ==
			      (i < 249 ? V92_QC_ANALOG_ANSPCM : V92_QC_ANALOG_V8), "late answer lost handoff");
	}
	CHECK(a.stage == V92_QC_ANALOG_PHASE2 && terminal_unchanged(&a), "late answer did not finish");
	CHECK(init(&a, &d, G711_LAW_PCMU, true, 7, 1), "reinit failed");
	for (unsigned i = 0; i < 786; ++i) {
		struct v90_pcm_symbol signal;
		CHECK(v92_qts_symbol(G711_LAW_PCMU, 7, i, &signal), "QTS failed");
		CHECK(v92_qc_analog_sample(&a, i >= 782 ? 0 : signal.sample, &output) != V92_QC_ANALOG_INVALID,
		      "damaged reversal treated as invalid API input");
	}
	CHECK(a.stage == V92_QC_ANALOG_V8 && output == 0 && terminal_unchanged(&a),
	      "damaged QTS reversal entered Phase 2");
	return true;
}

static bool test_validation(void)
{
	struct v92_qc_analog a = { 0 }, saved;
	struct v92_qc_digital d;
	struct v92_qc1_message local = { .sender = V90_ANALOG_MODEM, .signal.qts_index = 7 };
	struct v92_qc1_message peer = { .sender = V90_DIGITAL_MODEM, .acknowledgment = true };
	int16_t output = 1234;
	CHECK(v92_qc_analog_sample(&a, 0, &output) == V92_QC_ANALOG_INVALID && output == 1234 &&
	      init(&a, &d, G711_LAW_PCMU, false, 7, 1), "init failed");
	saved = a;
	CHECK(!init(&a, &d, G711_LAW_PCMU, false, 15, 0) &&
	      !init(&a, &d, G711_LAW_PCMU, false, 0, 4) &&
	      !init(&a, &d, (enum g711_law)2, false, 0, 0) &&
	      !v92_qc_analog_init(NULL, G711_LAW_PCMU, &local, &peer, 4096) &&
	      !v92_qc_analog_init(&a, G711_LAW_PCMU, NULL, &peer, 4096) &&
	      !v92_qc_analog_init(&a, G711_LAW_PCMU, &local, NULL, 4096) &&
	      !v92_qc_analog_init(&a, G711_LAW_PCMU, &peer, &local, 4096) &&
	      !v92_qc_analog_init(&a, G711_LAW_PCMU, &local, &peer, 0), "bad parameters accepted");
	peer.acknowledgment = false;
	CHECK(!v92_qc_analog_init(&a, G711_LAW_PCMU, &local, &peer, 4096), "two requests accepted");
	local.acknowledgment = peer.acknowledgment = true;
	CHECK(!v92_qc_analog_init(&a, G711_LAW_PCMU, &local, &peer, 4096) &&
	      v92_qc_analog_sample(NULL, 0, &output) == V92_QC_ANALOG_INVALID &&
	      v92_qc_analog_sample(&a, 0, NULL) == V92_QC_ANALOG_INVALID &&
	      v92_qc_analog_sample(&a, 0, (int16_t *)&a) == V92_QC_ANALOG_INVALID &&
	      output == 1234 && memcmp(&a, &saved, sizeof(a)) == 0, "invalid call changed state");
	return true;
}

int main(void)
{
	const struct { const char *name; bool (*run)(void); } tests[] = {
		{ "paired QC signal exchange", test_pairs },
		{ "complete V.8 Quick Connect Phase 1", test_phase1 },
		{ "TONEq minimum and ANSpcm phase reversals", test_tone_duration },
		{ "QC answer timeout and signal qualification", test_fallback },
		{ "late ANSpcm and damaged QTS reversal", test_late_answer },
		{ "analogue QC validation", test_validation },
	};
	for (size_t i = 0; i < ARRAY_LENGTH(tests); ++i) {
		printf("[ RUN      ] %s\n", tests[i].name);
		if (!tests[i].run())
			return EXIT_FAILURE;
		printf("[       OK ] %s\n", tests[i].name);
	}
	return EXIT_SUCCESS;
}
