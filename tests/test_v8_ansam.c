// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <v8_ansam.h>

#include "test.h"
#include "v8_line_state_fixture.h"

#include <math.h>
#include <string.h>

enum {
	TEST_AMPLITUDE = 12000,
	TEST_FRAGMENT_SAMPLES = 37,
};

static const double TEST_TWO_PI =
	6.283185307179586476925286766559;

static bool generate_discard(
	struct v8_ansam_generator *generator, size_t count)
{
	int16_t samples[64];

	while (count != 0) {
		size_t fragment = count < ARRAY_LENGTH(samples) ?
					  count : ARRAY_LENGTH(samples);

		if (v8_ansam_generate(
			    generator, samples, fragment) !=
		    V8_ANSAM_OK)
			return false;
		count -= fragment;
	}
	return true;
}

static bool test_generated_waveform(void)
{
	struct v8_ansam_generator fragmented;
	struct v8_ansam_generator single;
	struct v8_ansam_generator controlled;
	int16_t fragment[TEST_FRAGMENT_SAMPLES];
	int16_t sample = 0;
	int16_t previous = 0;
	unsigned int crossings = 0;
	unsigned int peak = 0;
	size_t total = 3u * V8_ANSAM_REVERSAL_SAMPLES;

	CHECK(v8_ansam_generator_init(
		      &fragmented, TEST_AMPLITUDE, true) ==
		      V8_ANSAM_OK &&
	      v8_ansam_generator_init(
		      &single, TEST_AMPLITUDE, true) ==
		      V8_ANSAM_OK,
	      "ANSam generators did not initialize");
	for (size_t offset = 0; offset < total;) {
		size_t count = (offset % TEST_FRAGMENT_SAMPLES) + 1u;

		if (count > total - offset)
			count = total - offset;
		CHECK(v8_ansam_generate(
			      &fragmented, fragment, count) ==
			      V8_ANSAM_OK,
		      "fragmented ANSam generation failed");
		for (size_t index = 0; index < count; ++index) {
			int magnitude;

			CHECK(v8_ansam_generate(
				      &single, &sample, 1) ==
				      V8_ANSAM_OK &&
				      sample == fragment[index],
			      "ANSam changed across fragment %zu", offset + index);
			magnitude = sample < 0 ? -sample : sample;
			if ((unsigned int)magnitude > peak)
				peak = (unsigned int)magnitude;
		}
		offset += count;
	}
	CHECK(fragmented.carrier_phase == single.carrier_phase &&
		      fragmented.modulation_phase == single.modulation_phase &&
		      fragmented.samples_until_reversal ==
			      single.samples_until_reversal &&
		      fragmented.signed_amplitude == single.signed_amplitude &&
		      peak <= (unsigned int)(TEST_AMPLITUDE * 6 / 5 + 1),
	      "ANSam generation was not bounded or chunk invariant");

	CHECK(v8_ansam_generator_init(
		      &controlled, TEST_AMPLITUDE, false) ==
		      V8_ANSAM_OK,
	      "continuous ANSam generator did not initialize");
	for (size_t index = 0; index < V8_ANSAM_SAMPLE_RATE;
	     ++index) {
		CHECK(v8_ansam_generate(
			      &controlled, &sample, 1) ==
			      V8_ANSAM_OK,
		      "continuous ANSam generation failed");
		if (previous <= 0 && sample > 0)
			crossings++;
		previous = sample;
	}
	CHECK(crossings == V8_ANSAM_CARRIER_HZ,
	      "ANSam carrier produced %u positive crossings", crossings);

	CHECK(v8_ansam_generator_init(
		      &controlled, TEST_AMPLITUDE, true) ==
		      V8_ANSAM_OK &&
	      generate_discard(
		      &controlled,
		      V8_ANSAM_REVERSAL_SAMPLES - 1u) &&
	      controlled.samples_until_reversal == 1,
	      "ANSam phase-reversal boundary was not reached");
	{
		uint32_t phase = controlled.carrier_phase;
		int16_t amplitude = controlled.signed_amplitude;

		CHECK(v8_ansam_generate(
			      &controlled, &sample, 1) ==
			      V8_ANSAM_OK &&
			      controlled.carrier_phase != phase &&
			      controlled.signed_amplitude == -amplitude &&
			      controlled.samples_until_reversal ==
				      V8_ANSAM_REVERSAL_SAMPLES,
		      "ANSam did not reverse phase at 450 ms");
	}
	return true;
}

enum test_codec {
	TEST_LINEAR,
	TEST_PCMU,
	TEST_PCMA,
};

static int16_t codec_round_trip(int16_t sample, enum test_codec codec)
{
	if (codec == TEST_PCMU)
		return g711_decode_pcmu(
			g711_encode_pcmu(sample));
	if (codec == TEST_PCMA)
		return g711_decode_pcma(
			g711_encode_pcma(sample));
	return sample;
}

static bool detect_generated(enum test_codec codec)
{
	struct v8_ansam_generator generator;
	struct v8_ansam_detector blocked;
	struct v8_ansam_detector samplewise;
	struct v8_ansam_detector observed;
	struct v8_line_conditioner blocked_line;
	struct v8_line_conditioner samplewise_line;
	struct v8_line_conditioner observed_line;
	/* A-law uses the G.191 one's-complement quantization intervals. */
	static const uint16_t expected_energy[] = { 4680, 4637, 4686 };
	int16_t samples[73];
	bool blocked_detected = false;
	bool sample_detected = false;
	bool observed_detected = false;
	size_t detection_sample = 0;
	size_t total = 0;

	CHECK(v8_ansam_generator_init(
		      &generator, TEST_AMPLITUDE, true) ==
		      V8_ANSAM_OK &&
	      v8_ansam_detector_init(&blocked) ==
		      V8_ANSAM_OK &&
	      v8_ansam_detector_init(&samplewise) ==
		      V8_ANSAM_OK &&
	      v8_line_conditioner_init(
		      &blocked_line, V8_CALLING_STATION) ==
		      V8_LINE_CONDITIONER_OK &&
	      v8_line_conditioner_init(
		      &samplewise_line, V8_CALLING_STATION) ==
		      V8_LINE_CONDITIONER_OK,
	      "ANSam loopback did not initialize");
	observed = samplewise;
	observed_line = samplewise_line;
	while (!sample_detected && total < 2u * V8_ANSAM_SAMPLE_RATE) {
		CHECK(v8_ansam_generate(
			      &generator, samples, ARRAY_LENGTH(samples)) ==
			      V8_ANSAM_OK,
		      "ANSam loopback generation failed");
		for (size_t index = 0; index < ARRAY_LENGTH(samples); ++index)
			samples[index] = codec_round_trip(samples[index], codec);
		CHECK(v8_ansam_detect(
			      &blocked, &blocked_line, samples,
			      ARRAY_LENGTH(samples),
			      &blocked_detected) == V8_ANSAM_OK,
		      "ANSam loopback detection failed");
		for (size_t index = 0; index < ARRAY_LENGTH(samples); ++index) {
			struct v8_line_conditioned_block block, saved_block;
			bool ready;

			CHECK(v8_ansam_detect(
				      &samplewise, &samplewise_line,
				      &samples[index], 1,
				      &sample_detected) ==
				      V8_ANSAM_OK,
			      "samplewise ANSam detection failed");
			CHECK(v8_line_conditioner_process_sample(&observed_line,
				      samples[index], &block, &ready) == V8_LINE_CONDITIONER_OK,
			      "shared ANSam conditioning failed");
			if (ready) {
				memcpy(&saved_block, &block, sizeof(block));
				CHECK(v8_ansam_observe_block(&observed, &observed_line,
					      &block, &observed_detected) == V8_ANSAM_OK &&
				      v8_line_blocks_equal(&block, &saved_block),
				      "ANSam block observation modified its input");
			}
			CHECK(observed_detected == sample_detected &&
			      memcmp(&observed, &samplewise, sizeof(observed)) == 0 &&
			      v8_line_states_equal(&observed_line, &samplewise_line),
			      "ANSam block observation changed the samplewise oracle");
			if (sample_detected && detection_sample == 0)
				detection_sample = total + index + 1u;
		}
		total += ARRAY_LENGTH(samples);
	}
	CHECK(blocked_detected && sample_detected && detection_sample == 8782 &&
	      memcmp(&blocked, &samplewise, sizeof(blocked)) == 0 &&
	      v8_line_states_equal(&blocked_line, &samplewise_line) &&
	      blocked.stage == V8_ANSAM_DETECTED &&
	      blocked.dwell_blocks ==
		      V8_ANSAM_POST_DETECT_DWELL_BLOCKS + 1u &&
	      blocked_line.gain == 406 && blocked.tone.counter == 85 &&
	      blocked.modulation_energy == expected_energy[codec],
	      "ANSam codec %d changed the fixed detector oracle at %zu/%u",
	      codec, detection_sample, blocked.modulation_energy);
	return true;
}

static bool run_synthetic_signal(
	unsigned int carrier_hz, unsigned int modulation_hz,
	double modulation_depth, int amplitude,
	struct v8_ansam_detector *detector, bool *detected)
{
	struct v8_line_conditioner line;
	int16_t samples[80];
	size_t total = 3u * V8_ANSAM_SAMPLE_RATE;

	CHECK(v8_ansam_detector_init(detector) == V8_ANSAM_OK &&
	      v8_line_conditioner_init(
		      &line, V8_CALLING_STATION) ==
		      V8_LINE_CONDITIONER_OK,
	      "synthetic ANSam detector did not initialize");
	*detected = false;
	for (size_t offset = 0; offset < total;
	     offset += ARRAY_LENGTH(samples)) {
		for (size_t index = 0; index < ARRAY_LENGTH(samples); ++index) {
			double sample_index = (double)(offset + index);
			double carrier = sin(
				TEST_TWO_PI * carrier_hz * sample_index /
				V8_ANSAM_SAMPLE_RATE);
			double envelope =
				1.0 + modulation_depth *
					      cos(TEST_TWO_PI * modulation_hz *
						  sample_index /
						  V8_ANSAM_SAMPLE_RATE);

			samples[index] =
				(int16_t)lround(amplitude * envelope * carrier);
		}
		CHECK(v8_ansam_detect(
			      detector, &line, samples, ARRAY_LENGTH(samples),
			      detected) == V8_ANSAM_OK,
		      "synthetic ANSam detection failed");
	}
	return true;
}

static bool test_detection_and_rejection(void)
{
	static const unsigned int tolerated_carriers[] = { 2080, 2120 };
	struct v8_ansam_detector detector;
	bool detected;

	CHECK(detect_generated(TEST_LINEAR) && detect_generated(TEST_PCMU) &&
		      detect_generated(TEST_PCMA),
	      "ANSam was not codec-stable");
	for (size_t index = 0;
	     index < ARRAY_LENGTH(tolerated_carriers); ++index) {
		CHECK(run_synthetic_signal(
			      tolerated_carriers[index], 15, 0.2,
			      TEST_AMPLITUDE, &detector, &detected) && detected,
		      "ANSam carrier tolerance rejected %u Hz",
		      tolerated_carriers[index]);
	}
	CHECK(run_synthetic_signal(2100, 15, 0.0, TEST_AMPLITUDE,
				   &detector, &detected) &&
		      !detected &&
		      detector.stage == V8_ANSAM_WAIT_MODULATION &&
		      detector.modulation_energy <= 0x018f,
	      "unmodulated answer tone was accepted");
	CHECK(run_synthetic_signal(2100, 20, 0.2, TEST_AMPLITUDE,
				   &detector, &detected) &&
		      !detected,
	      "wrong envelope frequency was accepted");
	CHECK(run_synthetic_signal(2000, 15, 0.2, TEST_AMPLITUDE,
				   &detector, &detected) &&
		      !detected,
	      "wrong carrier frequency was accepted");
	CHECK(run_synthetic_signal(2100, 15, 0.2, 128,
				   &detector, &detected) &&
		      !detected,
	      "sub-threshold answer signal was accepted");
	return true;
}

static bool test_reversal_alignment(void)
{
	int16_t samples[80];

	for (size_t start = 0;
	     start < V8_ANSAM_REVERSAL_SAMPLES; start += 300) {
		struct v8_ansam_generator generator;
		struct v8_ansam_detector detector;
		struct v8_line_conditioner line;
		bool detected = false;

		CHECK(v8_ansam_generator_init(
			      &generator, TEST_AMPLITUDE, true) ==
			      V8_ANSAM_OK &&
		      generate_discard(&generator, start) &&
		      v8_ansam_detector_init(&detector) ==
			      V8_ANSAM_OK &&
		      v8_line_conditioner_init(
			      &line, V8_CALLING_STATION) ==
			      V8_LINE_CONDITIONER_OK,
		      "offset ANSam detector did not initialize");
		for (size_t offset = 0;
		     offset < 2u * V8_ANSAM_SAMPLE_RATE &&
		     !detected;
		     offset += ARRAY_LENGTH(samples)) {
			CHECK(v8_ansam_generate(
				      &generator, samples,
				      ARRAY_LENGTH(samples)) ==
				      V8_ANSAM_OK &&
			      v8_ansam_detect(
				      &detector, &line, samples,
				      ARRAY_LENGTH(samples), &detected) ==
				      V8_ANSAM_OK,
			      "offset ANSam processing failed");
		}
		CHECK(detected,
		      "ANSam reversal alignment %zu prevented detection", start);
	}
	return true;
}

static bool test_gain_stability(void)
{
	static const struct {
		uint16_t reference, gain;
		bool stable;
	} cases[] = {
		{ 512, 511, true }, { 512, 513, true },
		{ 512, 487, true }, { 512, 537, true },
		{ 512, 486, false }, { 512, 538, false },
		{ 512, 1536, false }, { 512, 2560, false },
		{ 1, INT16_MAX, false }, { INT16_MAX, 1, false },
	};
	const uint16_t stable_samples = V8_ANSAM_SAMPLE_RATE / 10;
	const int16_t samples[V8_LINE_INPUT_QUEUE_SAMPLES] = { 0 };

	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index) {
		for (unsigned int was_stable = 0; was_stable < 2; ++was_stable) {
			struct v8_ansam_detector detector;
			struct v8_line_conditioner line;
			uint16_t elapsed = stable_samples -
				(was_stable ? 0 : V8_ANSAM_BLOCK_SAMPLES);
			bool detected = false;

			CHECK(v8_ansam_detector_init(&detector) == V8_ANSAM_OK &&
			      v8_line_conditioner_init(&line, V8_CALLING_STATION) ==
				      V8_LINE_CONDITIONER_OK,
			      "gain stability setup failed");
			/* Observe one block with frozen gain, either just before the
			 * stability threshold or after stability was already latched. */
			detector.stage = V8_ANSAM_WAIT_MODULATION;
			detector.signal_ref_gain = cases[index].reference;
			detector.signal_stable_time = elapsed;
			detector.signal_stable = was_stable != 0;
			line.gain = cases[index].gain;
			CHECK(v8_ansam_detect(&detector, &line, samples,
				      ARRAY_LENGTH(samples), &detected) == V8_ANSAM_OK &&
			      !detected && line.gain == cases[index].gain &&
			      detector.signal_stable == cases[index].stable &&
			      detector.signal_stable_time == (cases[index].stable ?
				      elapsed + V8_ANSAM_BLOCK_SAMPLES : 0),
			      "gain %u -> %u had wrong stability (previous=%u)",
			      cases[index].reference, cases[index].gain, was_stable);
		}
	}
	return true;
}

static bool test_interrupted_carrier(void)
{
	static const struct {
		const char *name;
		size_t resume_sample;
		unsigned interruption_hz;
		size_t earliest, latest;
	} cases[] = {
		{ "silence", 24000, 0, 0, 0 },
		{ "V.21 mark", 24000, 1850, 0, 0 },
		{ "returning carrier", 8000, 0, 16000, 20000 },
		{ "10 ms gap", 4080, 0, 8000, 10000 },
	};

	for (enum test_codec codec = TEST_LINEAR; codec <= TEST_PCMA; ++codec) {
		for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index) {
			struct v8_ansam_generator generator;
			struct v8_ansam_detector detector;
			struct v8_ansam_detector observed;
			struct v8_line_conditioner line;
			struct v8_line_conditioner observed_line;
			size_t detection = 0;
			bool observed_detected = false;

			CHECK(v8_ansam_generator_init(&generator, TEST_AMPLITUDE, true) ==
				      V8_ANSAM_OK && v8_ansam_detector_init(&detector) ==
				      V8_ANSAM_OK && v8_line_conditioner_init(&line,
				      V8_CALLING_STATION) == V8_LINE_CONDITIONER_OK,
			      "interrupted ANSam fixture");
			observed = detector;
			observed_line = line;
			for (size_t clock = 0; clock < 24000 && detection == 0; ++clock) {
				int16_t sample;
				bool detected;
				struct v8_line_conditioned_block block;
				bool ready;

				CHECK(v8_ansam_generate(&generator, &sample, 1) == V8_ANSAM_OK,
				      "interrupted ANSam generation");
				if (clock >= 4000 && clock < cases[index].resume_sample)
					sample = (int16_t)lround(TEST_AMPLITUDE * sin(TEST_TWO_PI *
						cases[index].interruption_hz * clock / V8_ANSAM_SAMPLE_RATE));
				sample = codec_round_trip(sample, codec);
				CHECK(v8_ansam_detect(&detector, &line, &sample, 1, &detected) ==
					      V8_ANSAM_OK, "interrupted ANSam detection");
				CHECK(v8_line_conditioner_process_sample(&observed_line, sample,
					      &block, &ready) == V8_LINE_CONDITIONER_OK &&
				      (!ready || v8_ansam_observe_block(&observed, &observed_line,
					      &block, &observed_detected) == V8_ANSAM_OK) &&
				      observed_detected == detected &&
				      memcmp(&observed, &detector, sizeof(detector)) == 0 &&
				      memcmp(&observed_line, &line, sizeof(line)) == 0,
				      "interrupted block observation differs from samplewise detection");
				if (detected)
					detection = clock + 1;
			}
			CHECK(detection >= cases[index].earliest &&
			      detection <= cases[index].latest,
			      "codec %u %s: stale or missing ANSam at %zu",
			      codec, cases[index].name, detection);
		}
	}
	return true;
}

static bool test_block_validation(void)
{
	struct v8_ansam_detector detector, saved;
	struct v8_line_conditioner line, saved_line;
	struct v8_line_conditioned_block block = { 0 }, saved_block = { 0 };
	bool detected = true;

	CHECK(v8_ansam_detector_init(&detector) == V8_ANSAM_OK &&
	      v8_line_conditioner_init(&line, V8_CALLING_STATION) == V8_LINE_CONDITIONER_OK,
	      "ANSam block validation fixture");
	saved = detector;
	saved_line = line;
	CHECK(v8_ansam_observe_block(NULL, &line, &block, &detected) == V8_ANSAM_INVALID_ARGUMENT &&
	      v8_ansam_observe_block(&detector, NULL, &block, &detected) == V8_ANSAM_INVALID_ARGUMENT &&
	      v8_ansam_observe_block(&detector, &line, NULL, &detected) == V8_ANSAM_INVALID_ARGUMENT &&
	      v8_ansam_observe_block(&detector, &line, &block, NULL) == V8_ANSAM_INVALID_ARGUMENT &&
	      v8_ansam_observe_block(&detector, &line, (void *)&detector, &detected) == V8_ANSAM_INVALID_ARGUMENT &&
	      v8_ansam_observe_block(&detector, &line, (void *)&line, &detected) == V8_ANSAM_INVALID_ARGUMENT &&
	      v8_ansam_observe_block(&detector, &line, &block, (void *)&block) == V8_ANSAM_INVALID_ARGUMENT &&
	      v8_ansam_observe_block(&detector, &line, &block, (void *)&detector) == V8_ANSAM_INVALID_ARGUMENT &&
	      v8_ansam_observe_block(&detector, &line, &block, (void *)&line) == V8_ANSAM_INVALID_ARGUMENT &&
	      memcmp(&detector, &saved, sizeof(saved)) == 0 &&
	      v8_line_states_equal(&line, &saved_line) &&
	      v8_line_blocks_equal(&block, &saved_block) && detected,
	      "ANSam block rejection changed state, input, or output");
	line.role = V8_ANSWERING_STATION;
	saved_line = line;
	CHECK(v8_ansam_observe_block(&detector, &line, &block, &detected) == V8_ANSAM_INVALID_ARGUMENT &&
	      memcmp(&detector, &saved, sizeof(saved)) == 0 &&
	      v8_line_states_equal(&line, &saved_line) && detected,
	      "ANSam block accepted the wrong line role");
	return true;
}

/* Independent widened model of the original direct-form recurrences.
 * Division floors each product before its signed-word wrap. Keep the zero
 * feed-forward terms and old history layout here to check their removal. */
struct tone_model {
	int16_t input[3], output[3], section_input[4], section_output[4];
	int16_t energy;
};

static int32_t model_floor(int64_t value, unsigned shift)
{
	int64_t divisor = INT64_C(1) << shift;

	return (int32_t)(value >= 0 ? value / divisor :
		-((-value + divisor - 1) / divisor));
}

static int16_t model_word(int64_t value)
{
	uint16_t word = (uint16_t)value;

	return word <= INT16_MAX ? (int16_t)word :
		(int16_t)((int32_t)word - 65536);
}

static int16_t model_product(int16_t left, int16_t right)
{
	return model_word(model_floor((int64_t)left * right, 14));
}

static void model_tone_sample(struct tone_model *model, int16_t sample)
{
	const int16_t feed[] = { 15408, 0, 15408 }, feedback[] = { 0, 14487 };
	const int16_t section_feed[] = { 0, 0, 0, 0 };
	const int16_t section_feedback[] = { 1984, 15229, 2974, 15229 };
	int32_t output = 0;

	model->input[0] = sample;
	for (unsigned i = 0; i < 3; ++i)
		output += model_product(model->input[i], feed[i]);
	for (unsigned i = 0; i < 2; ++i)
		output -= model_product(model->output[i], feedback[i]);
	for (unsigned i = 2; i != 0; --i) {
		model->input[i] = model->input[i - 1];
		model->output[i] = model->output[i - 1];
	}
	model->output[0] = model_word(output);
	for (unsigned section = 0; section < 4; section += 2) {
		int32_t input = model_floor(output, 4);

		output = input;
		for (unsigned i = 0; i < 2; ++i) {
			output += model_product(model->section_input[section + i],
				section_feed[section + i]);
			output -= model_product(model->section_output[section + i],
				section_feedback[section + i]);
		}
		model->section_input[section + 1] = model->section_input[section];
		model->section_input[section] = model_word(input);
		model->section_output[section + 1] = model->section_output[section];
		model->section_output[section] = model_word(output);
	}
	int16_t measured = model_word(model_floor(output, 4));
	int16_t magnitude = model_word(measured < 0 ? -(int32_t)measured : measured);
	model->energy = model_word(model_product(magnitude, 0x706d) +
		model_product(model->energy, 0x3c2e));
}

static bool test_filter_recurrence(void)
{
	struct v8_ansam_detector detector;
	struct v8_line_conditioner line;
	struct v8_line_conditioned_block block = { 0 };
	struct tone_model model;
	uint32_t random = 0x1794ad03;
	bool detected;

	CHECK(v8_ansam_detector_init(&detector) == V8_ANSAM_OK &&
	      v8_line_conditioner_init(&line, V8_CALLING_STATION) == V8_LINE_CONDITIONER_OK,
	      "filter recurrence fixture failed");
	/* Nonzero histories also exercise zero-weight inputs in the old layout. */
	memset(&detector.tone, 0xa5, sizeof(detector.tone));
	memset(&model, 0xa5, sizeof(model));
	detector.tone.counter = 0;
	detector.tone.warmup_count = 0;
	detector.tone.locked = false;
	for (unsigned n = 0; n < 4096; ++n) {
		for (unsigned i = 0; i < ARRAY_LENGTH(block.samples); ++i) {
			random = random * UINT32_C(1664525) + UINT32_C(1013904223);
			block.samples[i] = n < 32 ? (i & 1 ? INT16_MIN : INT16_MAX) :
				model_word(random >> 16);
			model_tone_sample(&model, block.samples[i]);
		}
		/* Keep observing the carrier filter after acquisition rather than
		 * letting later modulation/loss policy reset its accumulated state. */
		detector.stage = V8_ANSAM_WAIT_CARRIER;
		detector.modulation_energy = 0;
		CHECK(v8_ansam_observe_block(&detector, &line, &block, &detected) == V8_ANSAM_OK,
		      "full-scale filter block %u failed", n);
		for (unsigned i = 0; i < 2; ++i) {
			unsigned input_index = ARRAY_LENGTH(detector.tone.tone_input) - 2 + i;

			CHECK((int16_t)detector.tone.tone_input[input_index] == model.input[i + 1] &&
			      (int16_t)detector.tone.tone_output[i] == model.output[i],
			      "rejection history differs in block %u", n);
		}
		for (unsigned i = 0; i < 4; ++i)
			CHECK((int16_t)detector.tone.iir_output[i] == model.section_output[i],
			      "recursive history differs in block %u", n);
		CHECK((int16_t)detector.tone.filtered_energy == model.energy,
		      "filter energy differs in block %u", n);
	}
	return true;
}

static bool test_validation(void)
{
	struct v8_ansam_generator generator;
	struct v8_ansam_detector detector;
	struct v8_ansam_detector saved;
	struct v8_line_conditioner line;
	struct v8_line_conditioner saved_line;
	int16_t sample = 123;
	int16_t output_sample = 123;
	bool detected = true;

	CHECK(v8_ansam_generator_init(
		      &generator, TEST_AMPLITUDE, false) ==
		      V8_ANSAM_OK &&
	      v8_ansam_generate(&generator, NULL, 0) ==
		      V8_ANSAM_OK &&
	      v8_ansam_detector_init(&detector) ==
		      V8_ANSAM_OK &&
	      v8_line_conditioner_init(
		      &line, V8_CALLING_STATION) ==
		      V8_LINE_CONDITIONER_OK &&
	      v8_ansam_detect(
		      &detector, &line, NULL, 0, &detected) ==
		      V8_ANSAM_OK &&
	      !detected && v8_ansam_detector_is_valid(&detector),
	      "ANSam zero-length operation failed");
	generator.signed_amplitude = 0;
	saved = detector;
	saved_line = line;
	CHECK(v8_ansam_generator_init(NULL, TEST_AMPLITUDE, true) ==
		      V8_ANSAM_INVALID_ARGUMENT &&
	      v8_ansam_generator_init(&generator, 0, true) ==
		      V8_ANSAM_INVALID_ARGUMENT &&
	      v8_ansam_generate(&generator, &sample, 1) ==
		      V8_ANSAM_INVALID_ARGUMENT &&
	      v8_ansam_generator_init(
		      &generator, TEST_AMPLITUDE, true) == V8_ANSAM_OK &&
	      v8_ansam_generate(
		      &generator, (int16_t *)&generator, 1) ==
		      V8_ANSAM_INVALID_ARGUMENT &&
	      sample == 123 &&
	      v8_ansam_detector_init(NULL) ==
		      V8_ANSAM_INVALID_ARGUMENT &&
	      v8_ansam_detect(NULL, &line, &sample, 1, &detected) ==
		      V8_ANSAM_INVALID_ARGUMENT &&
	      v8_ansam_detect(&detector, NULL, &sample, 1, &detected) ==
		      V8_ANSAM_INVALID_ARGUMENT &&
	      v8_ansam_detect(
		      &detector, &line, (const int16_t *)&detector, 1,
		      &detected) == V8_ANSAM_INVALID_ARGUMENT &&
	      v8_ansam_detect(
		      &detector, &line, (const int16_t *)&line, 1,
		      &detected) == V8_ANSAM_INVALID_ARGUMENT &&
	      v8_ansam_detect(
		      &detector, &line, &output_sample, 1,
		      (bool *)&detector) == V8_ANSAM_INVALID_ARGUMENT &&
	      v8_ansam_detect(
		      &detector, &line, &output_sample, 1,
		      (bool *)&line) == V8_ANSAM_INVALID_ARGUMENT &&
	      v8_ansam_detect(
		      &detector, &line, &output_sample, 1,
		      (bool *)&output_sample) == V8_ANSAM_INVALID_ARGUMENT &&
	      memcmp(&detector, &saved, sizeof(detector)) == 0 &&
	      v8_line_states_equal(&line, &saved_line) &&
	      output_sample == 123 && detected == false,
	      "ANSam overlap handling changed state or output");
	detector.signal_ref_gain = 0;
	CHECK(!v8_ansam_detector_is_valid(&detector) &&
	      v8_ansam_detect(&detector, &line, &sample, 1, &detected) ==
		      V8_ANSAM_INVALID_ARGUMENT,
	      "ANSam API accepted invalid state");
	detector = saved;
	detector.carrier_loss_blocks = UINT8_MAX;
	CHECK(!v8_ansam_detector_is_valid(&detector) &&
	      v8_ansam_detect(&detector, &line, &sample, 1, &detected) ==
		      V8_ANSAM_INVALID_ARGUMENT && !detected &&
	      v8_line_states_equal(&line, &saved_line),
	      "ANSam API accepted an invalid carrier-loss count");
	detector = saved;
	line.gain = 0;
	CHECK(!v8_line_conditioner_is_valid(&line) &&
	      v8_ansam_detect(&detector, &line, &sample, 1, &detected) ==
		      V8_ANSAM_INVALID_ARGUMENT,
	      "ANSam API accepted invalid line state");
	CHECK(v8_ansam_detector_init(&detector) == V8_ANSAM_OK &&
	      v8_ansam_detector_is_valid(&detector) &&
	      detector.stage == V8_ANSAM_WAIT_CARRIER,
	      "ANSam detector reset retained receive evidence");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated waveform", test_generated_waveform },
		{ "detection and rejection", test_detection_and_rejection },
		{ "phase-reversal alignment", test_reversal_alignment },
		{ "gain stability", test_gain_stability },
		{ "interrupted carrier", test_interrupted_carrier },
		{ "block validation", test_block_validation },
		{ "filter recurrence", test_filter_recurrence },
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
