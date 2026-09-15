// SPDX-License-Identifier: BSD-3-Clause

#include "v92_qc_digital.h"
#include "test.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool init(struct v92_qc_digital *exchange, enum g711_law law,
	bool answerer, unsigned index, unsigned level)
{
	const struct v92_qc1_message local = {
		.sender = V90_DIGITAL_MODEM, .acknowledgment = answerer,
		.signal.anspcm_level = level,
	};
	const struct v92_qc1_message peer = {
		.sender = V90_ANALOG_MODEM, .acknowledgment = !answerer,
		.signal.qts_index = index,
	};

	return v92_qc_digital_init(exchange, law, &local, &peer);
}

static int16_t tone(enum g711_law law, unsigned i, double frequency,
	double amplitude, double phase)
{
	int16_t sample = (int16_t)lrint(amplitude *
		sin(2.0 * acos(-1.0) * frequency * i / 8000.0 + phase));

	return law == G711_LAW_PCMU ? g711_decode_pcmu(g711_encode_pcmu(sample)) :
		g711_decode_pcma(g711_encode_pcma(sample));
}

static bool terminal_unchanged(struct v92_qc_digital *exchange)
{
	struct v92_qc_digital saved = *exchange;
	struct v90_pcm_symbol output = { .octet = 0xa5 }, before = output;

	CHECK(v92_qc_digital_sample(exchange, 1234, &output) == saved.stage &&
	      memcmp(exchange, &saved, sizeof(saved)) == 0 &&
	      memcmp(&output, &before, sizeof(output)) == 0,
	      "terminal exchange consumed the next phase's sample");
	return true;
}

static bool test_wire_sequence(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned answerer = 0; answerer < 2; ++answerer)
			for (unsigned choice = 0; choice < 2; ++choice) {
				struct v92_qc_digital exchange;
				struct v92_anspcm anspcm;
				struct v90_pcm_symbol output, expected;
				unsigned index = choice ? 14 : 0, level = choice ? 3 : 0;
				unsigned silence = 0;

				CHECK(init(&exchange, law, answerer, index, level) &&
				      v92_anspcm_init(&anspcm, law, level), "init failed");
				for (unsigned i = 0; i < 12000; ++i) {
					enum v92_qc_digital_stage before = exchange.stage;
					/* Early TONEq cannot skip silence/QTS. The real reply
					 * arrives after two ANSpcm phase reversals. */
					int16_t input = i < 1416 || i >= 10000 ?
						tone(law, i, 980, 4096, 0.7) : 0;
					enum v92_qc_digital_stage result =
						v92_qc_digital_sample(&exchange, input, &output);

					CHECK(result != V92_QC_DIGITAL_INVALID &&
					      result != V92_QC_DIGITAL_V8, "unexpected stage %u", result);
					if (i < 600 || before == V92_QC_DIGITAL_TRAIL_SILENCE) {
						CHECK(v90_pcm_symbol_make(&expected, law, 0, true),
						      "silence symbol failed");
						if (i >= 600)
							++silence;
					} else if (i < 1416) {
						CHECK(v92_qts_symbol(law, index, i - 600, &expected),
						      "QTS symbol failed");
					} else {
						CHECK(v92_anspcm_sample(&anspcm, &expected),
						      "ANSpcm symbol failed");
					}
					CHECK(v90_pcm_symbol_is_valid(&output, law) &&
					      output.octet == expected.octet,
					      "law %u role %u changed wire symbol %u", law, answerer, i);
					if (result == V92_QC_DIGITAL_PHASE2)
						break;
				}
				CHECK(exchange.stage == V92_QC_DIGITAL_PHASE2 && silence == 600 &&
				      terminal_unchanged(&exchange), "handoff did not preserve 75 ms");
			}
	return true;
}

static bool test_toneq(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned phase = 0; phase < 4; ++phase)
			for (unsigned variant = 0; variant < 3; ++variant) {
				struct v92_qc_digital exchange;
				struct v90_pcm_symbol output;
				unsigned detected = 0;

				CHECK(init(&exchange, law, true, 7, 1), "init failed");
				for (unsigned i = 0; i < 2700; ++i) {
					int16_t input = i >= 1600 && i < 2000 ?
						tone(law, i - 1600, 975 + 5 * variant,
							1024 << variant, phase * 0.8) : 0;
					enum v92_qc_digital_stage result =
						v92_qc_digital_sample(&exchange, input, &output);

					if (result == V92_QC_DIGITAL_TRAIL_SILENCE && detected == 0)
						detected = i + 1;
					if (result == V92_QC_DIGITAL_PHASE2) {
						CHECK(i + 1 == detected + 600, "handoff silence shifted");
						break;
					}
				}
				CHECK(detected > 1600 && detected <= 2000 &&
				      exchange.stage == V92_QC_DIGITAL_PHASE2,
				      "50 ms TONEq not accepted: law %u phase %u variant %u at %u",
				      law, phase, variant, detected);
			}
	return true;
}

static bool test_fallback(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned variant = 0; variant < 5; ++variant) {
			struct v92_qc_digital answer, caller;
			struct v90_pcm_symbol output, calling;

			CHECK(init(&answer, law, true, 7, 1) &&
			      init(&caller, law, false, 7, 1), "init failed");
			for (unsigned i = 0; i < 16000; ++i) {
				/* Silence, wrong frequency, separate 30 ms bursts,
				 * too-weak TONEq and DC must not complete the handshake. */
				int16_t input = variant == 1 ? tone(law, i, 1650, 8192, 0) :
					variant == 2 && i % 800 < 240 ? tone(law, i, 980, 4096, 0) :
					variant == 3 ? tone(law, i, 980, 128, 0) :
					variant == 4 ? 4096 : 0;
				enum v92_qc_digital_stage result =
					v92_qc_digital_sample(&answer, input, &output);

				CHECK(result == (i == 15999 ? V92_QC_DIGITAL_V8 :
					i < 599 ? V92_QC_DIGITAL_LEAD_SILENCE :
					i < 1415 ? V92_QC_DIGITAL_QTS : V92_QC_DIGITAL_ANSPCM),
				      "answer deadline or qualification changed at %u, variant %u", i, variant);
				CHECK(v92_qc_digital_sample(&caller, input, &calling) !=
					      V92_QC_DIGITAL_INVALID && calling.octet == output.octet,
				      "station role changed waveform");
			}
			CHECK(terminal_unchanged(&answer), "fallback consumed input");
			/* The caller remains bounded in storage, not subject to the
			 * answerer's two-second fallback. Its owner controls timeout. */
			for (unsigned i = 0; i < 16000; ++i)
				CHECK(v92_qc_digital_sample(&caller, 0, &output) ==
					      V92_QC_DIGITAL_ANSPCM,
				      "caller inherited the answerer's deadline");
		}
	return true;
}

static bool test_late_toneq(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		struct v92_qc_digital absent, reply;
		struct v90_pcm_symbol output;
		unsigned detected = 0, completed = 0;

		CHECK(init(&absent, law, true, 7, 1), "init failed");
		for (unsigned i = 0; i < 15500; ++i)
			CHECK(v92_qc_digital_sample(&absent, 0, &output) !=
				      V92_QC_DIGITAL_INVALID, "prefix failed");
		reply = absent;
		/* Independent copies see different replies. Once qualified, the
		 * answer deadline must not cut short the final 75 ms silence. */
		for (unsigned i = 15500; i < 17000; ++i) {
			enum v92_qc_digital_stage result = v92_qc_digital_sample(&reply,
				tone(law, i - 15500, 980, 4096, 0), &output);

			if (result == V92_QC_DIGITAL_TRAIL_SILENCE && detected == 0)
				detected = i + 1;
			if (result == V92_QC_DIGITAL_PHASE2) {
				completed = i + 1;
				break;
			}
			CHECK(v92_qc_digital_sample(&absent, 0, &output) ==
				      (i < 15999 ? V92_QC_DIGITAL_ANSPCM : V92_QC_DIGITAL_V8),
			      "copied receiver shared reply state");
		}
		CHECK(detected > 15500 && detected < 16000 &&
		      completed > 16000 && completed == detected + 600,
		      "late TONEq confused fallback and Phase 2 handoff");
		CHECK(terminal_unchanged(&reply) && terminal_unchanged(&absent),
		      "terminal state changed");
	}
	return true;
}

static bool test_validation(void)
{
	struct v92_qc_digital exchange = { 0 }, saved;
	struct v90_pcm_symbol output = { .octet = 0xa5 }, before = output;
	struct v92_qc1_message local = { .sender = V90_DIGITAL_MODEM };
	struct v92_qc1_message peer = { .sender = V90_ANALOG_MODEM };

	CHECK(v92_qc_digital_sample(&exchange, 0, &output) == V92_QC_DIGITAL_INVALID,
	      "zeroed exchange accepted");
	CHECK(init(&exchange, G711_LAW_PCMU, true, 7, 1), "init failed");
	saved = exchange;
	CHECK(!v92_qc_digital_init(&exchange, G711_LAW_PCMU, &local, &peer),
	      "unacknowledged exchange accepted");
	local.acknowledgment = true;
	peer.acknowledgment = true;
	CHECK(!v92_qc_digital_init(&exchange, G711_LAW_PCMU, &local, &peer),
	      "two acknowledgements accepted");
	CHECK(!init(&exchange, G711_LAW_PCMU, true, 15, 0) &&
	      !init(&exchange, G711_LAW_PCMU, false, 0, 4) &&
	      !init(&exchange, (enum g711_law)2, true, 0, 0) &&
	      !init(NULL, G711_LAW_PCMU, true, 0, 0) &&
	      !v92_qc_digital_init(&exchange, G711_LAW_PCMU, NULL, &peer) &&
	      !v92_qc_digital_init(&exchange, G711_LAW_PCMU, &local, NULL) &&
	      !v92_qc_digital_init(&exchange, G711_LAW_PCMU, &peer, &local),
	      "invalid parameters accepted");
	CHECK(v92_qc_digital_sample(NULL, 0, &output) == V92_QC_DIGITAL_INVALID &&
	      v92_qc_digital_sample(&exchange, 0, NULL) == V92_QC_DIGITAL_INVALID &&
	      v92_qc_digital_sample(&exchange, 0, (struct v90_pcm_symbol *)&exchange) ==
		      V92_QC_DIGITAL_INVALID &&
	      memcmp(&exchange, &saved, sizeof(saved)) == 0 &&
	      memcmp(&output, &before, sizeof(output)) == 0,
	      "invalid input changed state or output");
	CHECK(v92_qc_digital_is_valid(&exchange) && !v92_qc_digital_is_valid(NULL), "invalid QC state query");
	for (unsigned field = 0; field < 4; ++field) {
		exchange = saved;
		switch (field) {
		case 0: exchange.position = 600; break;
		case 1: exchange.answer_deadline = 16001; break;
		case 2: exchange.qts_index = 15; break;
		case 3: exchange.anspcm.position = 2u * V92_ANSPCM_REVERSAL_PERIOD; break;
		}
		const struct v92_qc_digital invalid = exchange;
		CHECK(!v92_qc_digital_is_valid(&exchange) &&
		      v92_qc_digital_sample(&exchange, 0, &output) == V92_QC_DIGITAL_INVALID &&
		      memcmp(&output, &before, sizeof(output)) == 0 &&
		      memcmp(&exchange, &invalid, sizeof(exchange)) == 0,
		      "invalid digital QC field %u consumed a sample", field);
	}
	return true;
}

int main(void)
{
	const struct { const char *name; bool (*run)(void); } tests[] = {
		{ "digital QC wire timing and handoff", test_wire_sequence },
		{ "TONEq phase, level and frequency tolerance", test_toneq },
		{ "answer fallback and caller deadline ownership", test_fallback },
		{ "late TONEq and independent copied state", test_late_toneq },
		{ "digital QC validation", test_validation },
	};

	for (size_t i = 0; i < ARRAY_LENGTH(tests); ++i) {
		printf("[ RUN      ] %s\n", tests[i].name);
		if (!tests[i].run())
			return EXIT_FAILURE;
		printf("[       OK ] %s\n", tests[i].name);
	}
	return EXIT_SUCCESS;
}
