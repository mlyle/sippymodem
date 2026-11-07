// SPDX-License-Identifier: BSD-3-Clause

#include "v92_echo_timing.h"
#include "test.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { SAMPLES = 7000, PREFIX = 1024, CHANGE = 1800, GAP = 2400 };

struct channel_case {
	enum g711_law law;
	unsigned seed, robbing, peak, noise, transient;
	double host_gain;
	int shift, gap;
	bool inverse, nonuniform, unrelated, drift;
};

struct fixture {
	struct pcm_host_echo echo;
	struct v92_upstream_data_rx receiver, saved;
	struct v92_echo_timing timing;
};

static uint32_t random_value(uint32_t *seed)
{
	*seed = *seed * UINT32_C(1664525) + UINT32_C(1013904223);
	return *seed;
}

static bool test_residual_projection(void)
{
	struct v92_residual filter = {
		.active = true, .count = 2, .delay = { 2, 6 },
		.coefficient = { .03125, -.015625 }, .offset = 123, .cursor = 9,
	};
	CHECK(v92_residual_is_valid(&filter), "invalid projection fixture");
	CHECK(v92_residual_project(&filter, 0) == 0 &&
	      v92_residual_output(&filter, 0) == -123,
	      "known zero echo subtracted the channel DC offset");
	filter.history[7] = 64;
	filter.history[3] = -64;
	struct v92_residual saved = filter;
	CHECK(v92_residual_project(&filter, 20) == 17 &&
	      v92_residual_output(&filter, 20) == -106 &&
	      memcmp(&filter, &saved, sizeof(filter)) == 0,
	      "projection changed feedback order, channel DC, or retained history");
	return true;
}

/* An actual wire byte retains its cell through host gain, polarity and
 * constellation calibration, even when two octets decode to the same level. */
static bool test_wire_cells(void)
{
	const double gains[] = { .5, 1., 1.25 };
	struct fixture *fixture = calloc(1, sizeof(*fixture));
	CHECK(fixture != NULL, "wire-cell fixture allocation failed");
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		int lower[256], upper[256];

		/* Derive each cell directly from the encoder, independently of
		 * the inverse interval API, including both mu-law zero octets. */
		for (unsigned byte = 0; byte < 256; ++byte) {
			lower[byte] = INT32_MAX;
			upper[byte] = INT32_MIN;
		}
		for (int sample = INT16_MIN; sample <= INT16_MAX; ++sample) {
			uint8_t byte = g711_encode(law, (int16_t)sample);

			if (sample < lower[byte]) lower[byte] = sample;
			if (sample > upper[byte]) upper[byte] = sample;
		}
		for (unsigned gain = 0; gain < ARRAY_LENGTH(gains); ++gain)
			for (int polarity = -1; polarity <= 1; polarity += 2) {
				memset(fixture, 0, sizeof(*fixture));
				struct pcm_host_echo *echo = &fixture->echo;
				struct v92_upstream_data_rx *receiver = &fixture->receiver;
				struct v92_upstream_data_line *line = &receiver->line;
				struct v92_echo_timing *timing = &fixture->timing;
				CHECK(pcm_host_echo_init(echo) == PCM_HOST_ECHO_OK,
				      "wire-cell echo initialization failed");
				echo->reference.fill = PCM_ECHO_REFERENCE_HISTORY_SAMPLES;
				echo->filter_ready = echo->delay_acquired = true;
				echo->train_samples = PCM_HOST_ECHO_TRAIN_MIN_SAMPLES;
				echo->train_peak = 256;
				line->plan.observation = V92_PCM_FRAME_CONSTELLATION;
				line->polarity = polarity;
				line->received_frames = V92_B1U_FRAMES + V92_PCM_RX_DELAY_FRAMES;
				line->calibration.gain = 1.25;
				line->host.gain = line->host.bounds.lower = line->host.bounds.upper = gains[gain];
				receiver->decoder.mapping.law = law;
				receiver->decoder.mapping.cpd.constellation[0].count = 2;
				receiver->decoder.mapping.cpd.constellation[0].positive_linear[0] = 3;
				receiver->decoder.mapping.cpd.constellation[0].positive_linear[1] = 7;
				for (unsigned byte = 0; byte < 256; ++byte) {
					long host = lrint(gains[gain] * g711_decode(law, byte));

					if (host <= INT16_MIN || host >= INT16_MAX) continue;
					CHECK(lower[byte] <= upper[byte], "encoder cell absent");
					v92_echo_timing_reset(timing);
					CHECK(v92_echo_timing_observe(timing, echo, receiver, PCM_HOST_ECHO_FILTER,
					      byte, (int16_t)host, (int16_t)host, true) == 0 &&
					      timing->initialized && timing->samples == 1,
					      "wire-cell observation failed: law %u byte %02x gain %g", law, byte, gains[gain]);
					double scale = gains[gain] * line->calibration.gain;
					float midpoint = (float)(polarity * (lower[byte] + upper[byte]) * .5 * scale);
					float half_width = (float)((upper[byte] - lower[byte]) * .5 * scale);

					CHECK(timing->observation[0] == midpoint && timing->half_width[0] == half_width,
					      "wrong wire cell: law %u byte %02x gain %g polarity %d", law, byte, gains[gain], polarity);
				}
			}
	}
	free(fixture);
	return true;
}

/* Compare the complete shift score plane with an exhaustive signed-alphabet
 * oracle. Cover table endpoints, interior points, and cells overlapping more
 * than one point, independently of the monitor's search and uniform shortcut. */
static bool test_constellation_scores(void)
{
	const unsigned counts[] = { 1, 2, 32, V92_CPD_MAX_CONSTELLATION_POINTS };
	const float widths[] = { 0, .25f, 1000 };
	struct fixture *fixture = calloc(1, sizeof(*fixture));
	CHECK(fixture != NULL, "score fixture allocation failed");
	for (unsigned c = 0; c < ARRAY_LENGTH(counts); ++c)
		for (unsigned irregular = 0; irregular < 2; ++irregular)
			for (unsigned w = 0; w < ARRAY_LENGTH(widths); ++w) {
				memset(fixture, 0, sizeof(*fixture));
				struct pcm_host_echo *echo = &fixture->echo;
				struct v92_upstream_data_rx *receiver = &fixture->receiver;
				struct v92_upstream_data_line *line = &receiver->line;
				struct v92_echo_timing *timing = &fixture->timing;
				CHECK(pcm_host_echo_init(echo) == PCM_HOST_ECHO_OK,
				      "score echo initialization failed");
				echo->reference.fill = PCM_ECHO_REFERENCE_HISTORY_SAMPLES;
				echo->filter_ready = echo->delay_acquired = true;
				echo->train_samples = PCM_HOST_ECHO_TRAIN_MIN_SAMPLES;
				echo->train_peak = 256;
				line->plan.observation = V92_PCM_FRAME_CONSTELLATION;
				line->calibration.gain = 1;
				line->polarity = 1;
				line->received_frames = V92_B1U_FRAMES + V92_PCM_RX_DELAY_FRAMES;
				struct v92_cpd_constellation *points =
					&receiver->decoder.mapping.cpd.constellation[0];
				points->count = (uint8_t)counts[c];
				for (unsigned j = 0; j < points->count; ++j)
					points->positive_linear[j] = points->count == 1 ? INT16_MAX :
						(int16_t)(irregular ? 2 + j * (j + 1) : 3 + 7 * j);
				CHECK(v92_echo_timing_observe(timing, echo, receiver,
				      PCM_HOST_ECHO_FILTER, 0, 0, 0, true) == 0 &&
				      timing->initialized, "score monitor initialization failed");
				/* Seed a full, disjoint history; this call scores exactly one
				 * observation without completing the selection window. */
				timing->samples = ECHO_TIMING_BINS + 2;
				timing->expected_sample = line->input_samples;
				const unsigned mask = V92_ECHO_TIMING_HISTORY - 1;
				unsigned at = (unsigned)timing->samples & mask;
				unsigned center = (at + V92_ECHO_TIMING_HISTORY -
					ECHO_TIMING_RADIUS) & mask;
				for (unsigned j = 0; j < V92_ECHO_TIMING_HISTORY; ++j) {
					unsigned point = (j / 4) % points->count;
					float value = points->positive_linear[point];
					if (j % 4 == 1 && point + 1 < points->count)
						value = (value + points->positive_linear[point + 1]) * .5f;
					else if (j % 4 >= 2)
						value += j % 4 == 2 ? -.25f : .25f;
					timing->prediction[j] = j & 1 ? -value : value;
				}
				timing->prediction[center] = 0;
				float observed = w == 0 ? 0 : 123.25f;
				timing->observation[center] = observed;
				timing->half_width[center] = widths[w];
				memcpy(&fixture->saved, receiver, sizeof(*receiver));
				CHECK(v92_echo_timing_observe(timing, echo, receiver,
				      PCM_HOST_ECHO_FILTER, 0, 0, 0, true) == 0 &&
				      timing->window.position == 1 &&
				      memcmp(&fixture->saved, receiver, sizeof(*receiver)) == 0,
				      "single score observation changed receive state or cadence");
				for (unsigned j = 0; j < ECHO_TIMING_BINS; ++j) {
					unsigned shifted = (center + V92_ECHO_TIMING_HISTORY +
						ECHO_TIMING_RADIUS - j) & mask;
					double input = (double)observed - timing->prediction[shifted];
					double best = INFINITY;
					for (int point = -(int)points->count; point < points->count; ++point) {
						double distance = fabs(input - v92_cpd_signed_point(points, point));
						distance = fmax(distance - widths[w], 0);
						best = fmin(best, distance * distance);
					}
					CHECK(timing->window.score[0][j] == best,
					      "wrong score for count %u irregular %u width %g bin %u",
					      points->count, irregular, (double)widths[w], j);
				}
			}
	free(fixture);
	return true;
}

static bool run_case(const struct channel_case *test)
{
	struct fixture *fixture = calloc(1, sizeof(*fixture));
	CHECK(fixture != NULL, "echo fixture allocation failed");
	struct pcm_host_echo *echo = &fixture->echo;
	struct v92_upstream_data_rx *receiver = &fixture->receiver;
	struct v92_upstream_data_line *line = &receiver->line;
	struct v92_echo_timing *timing = &fixture->timing;
	CHECK(pcm_host_echo_init(echo) == PCM_HOST_ECHO_OK, "echo initialization failed");
	/* Provide a qualified frozen owner with enough TX history to accept any
	 * supported shift. The test supplies its prediction numerically below. */
	echo->reference.fill = PCM_ECHO_REFERENCE_HISTORY_SAMPLES;
	echo->filter_ready = echo->delay_acquired = true;
	echo->train_samples = PCM_HOST_ECHO_TRAIN_MIN_SAMPLES;
	echo->train_peak = 256;
	echo->coefficients[0] = .1f;
	CHECK(pcm_host_echo_is_valid(echo), "invalid frozen echo fixture");
	unsigned original_delay = pcm_host_echo_window_delay_samples(echo);
	/* Populate the numerical receive view consumed by the monitor. Coding
	 * itself is exercised by the full datapump tests, not this component. */
	receiver->decoder.mapping.law = test->law;
	line->plan.observation = V92_PCM_FRAME_CONSTELLATION;
	line->calibration.gain = 3.2 / test->host_gain;
	line->polarity = test->seed & 1 ? 1 : -1;
	line->received_frames = V92_B1U_FRAMES + V92_PCM_RX_DELAY_FRAMES;
	line->host.gain = line->host.bounds.lower = line->host.bounds.upper = test->host_gain;
	if (test->inverse)
		line->residual = (struct v92_residual){
			.active = true, .count = 2, .delay = { 4, 6 },
			.coefficient = { -.025, .01 }, .offset = 123,
		};
	for (unsigned phase = 0; phase < V92_CPD_MAX_CONSTELLATIONS; phase++) {
		struct v92_cpd_constellation *points = &receiver->decoder.mapping.cpd.constellation[phase];
		receiver->decoder.mapping.cpd.frame_constellation[phase] = (uint8_t)phase;
		points->count = 32;
		for (unsigned i = 0; i < points->count; i++)
			points->positive_linear[i] = (int16_t)(test->nonuniform ?
				128 + 32 * i + 16 * i * i + phase * 8 : 256 + 512 * i);
	}
	uint32_t seed = test->seed;
	double prediction[SAMPLES + 2 * PREFIX], target_history[12] = { 0 };
	for (unsigned i = 0; i < ARRAY_LENGTH(prediction); i++)
		prediction[i] = (int)(random_value(&seed) % (2 * test->peak + 1)) - (int)test->peak;
	unsigned actions = 0, first = 0;
	for (unsigned i = 0; i < SAMPLES; i++) {
		const struct v92_cpd_constellation *points = &receiver->decoder.mapping.cpd.constellation[i % 6];
		double source = points->positive_linear[(random_value(&seed) >> 16) % points->count];
		if (random_value(&seed) & 0x10000)
			source = -source;
		line->tracking.bias = test->drift ? 2. * (i / 1536u) * test->host_gain : 0.;
		double target = source / line->calibration.gain + line->tracking.bias;
		double peer = target;
		if (test->inverse) {
			peer += line->residual.coefficient[0] * target_history[(i + 12 - 4) % 12] +
				line->residual.coefficient[1] * target_history[(i + 12 - 6) % 12] + line->residual.offset;
			target_history[i % 12] = target;
		}
		int actual_shift = i >= CHANGE && (!test->transient || i < CHANGE + test->transient) ? test->shift : 0;
		int model_shift = (int)pcm_host_echo_window_delay_samples(echo) - (int)original_delay;
		double own = test->unrelated ? 0 : prediction[PREFIX + i - actual_shift];
		int noise = test->noise ? (int)((random_value(&seed) >> 8) % (2 * test->noise + 1)) - (int)test->noise : 0;
		double physical = (peer + own + noise) * line->polarity;
		uint8_t wire = g711_encode(test->law, (int16_t)lrint(physical / test->host_gain));
		if (test->robbing && i % 6 == 2)
			wire = test->robbing == 1 ? (wire & 0xfeu) : (wire | 1u);
		int16_t received = (int16_t)lrint(test->host_gain * g711_decode(test->law, wire));
		int16_t cleaned = (int16_t)(received - lrint(prediction[PREFIX + i - model_shift] * line->polarity));
		bool finishing = timing->initialized &&
			(timing->samples + 1) % V92_ECHO_TIMING_STRIDE == 0 &&
			timing->window.position + 1 == timing->window.points;
		if (finishing)
			memcpy(&fixture->saved, receiver, sizeof(*receiver));
		uint64_t prior_samples = timing->samples;
		int moved = v92_echo_timing_observe(timing, echo, receiver, PCM_HOST_ECHO_FILTER,
			wire, received, cleaned, (int)i != test->gap);
		CHECK(!test->drift || !i || i % 1536u || timing->samples == prior_samples + 1,
		      "a data bias update restarted the echo timing window");
		CHECK(!timing->initialized || timing->inverse.offset == line->residual.offset,
		      "echo projection lost its frozen channel parameters");
		if (moved != 0) {
			CHECK(finishing && memcmp(&fixture->saved, receiver, sizeof(*receiver)) == 0,
			      "timing changed receive/coding state");
			actions++;
			first = i;
			CHECK(!test->unrelated && test->shift && !test->transient && i >= CHANGE &&
			      moved == test->shift && actions == 1,
			      "wrong shift %d for law %u seed %u gain %.2f shift %d", moved,
			      test->law, test->seed, test->host_gain, test->shift);
			CHECK(test->gap < 0 || i >= (unsigned)test->gap + 1151,
			      "retime reused evidence from before missing input");
			CHECK(echo->coefficients[0] == .1f && pcm_host_echo_is_valid(echo),
			      "retime changed the frozen response");
			/* The adapter predicts this same unconsumed input again. */
			model_shift = (int)pcm_host_echo_window_delay_samples(echo) - (int)original_delay;
			cleaned = (int16_t)(received - lrint(prediction[PREFIX + i - model_shift] * line->polarity));
		}
		int32_t correction;
		CHECK(v92_residual_sample(&line->residual, (int32_t)cleaned * line->polarity,
			line->polarity, true, &correction), "test residual inverse failed");
		line->input_samples++;
		line->symbol_count = (line->symbol_count + 1) % 12;
	}
	bool expected = test->shift && !test->unrelated && !test->transient;
	CHECK(actions == (unsigned)expected, "missed shift %d for law %u seed %u gain %.2f",
	      test->shift, test->law, test->seed, test->host_gain);
	CHECK(!expected || test->gap >= 0 || first - CHANGE <= 1000,
	      "shift was too late for the quality recovery window");
	if (test->inverse) {
		line->residual.offset += 1;
		memcpy(&fixture->saved, receiver, sizeof(*receiver));
		uint8_t wire = g711_encode(test->law, 0);
		int16_t received = (int16_t)lrint(test->host_gain * g711_decode(test->law, wire));
		CHECK(v92_echo_timing_observe(timing, echo, receiver, PCM_HOST_ECHO_FILTER,
		      wire, received, received, true) == 0 && timing->initialized &&
		      timing->samples == 1 && timing->inverse.offset == line->residual.offset &&
		      memcmp(&fixture->saved, receiver, sizeof(*receiver)) == 0,
		      "changed channel DC reused old timing evidence or changed receive state");
	}
	/* A physical inverse overflow cannot seed timing just because calibration
	 * would make it fit. Match the real inverse's strict signed-16 bounds. */
	line->polarity = 1;
	line->calibration.gain = .5;
	line->host.gain = 0;
	line->residual = (struct v92_residual){ .active = true, .offset = -1.75 };
	memcpy(&fixture->saved, receiver, sizeof(*receiver));
	CHECK(v92_echo_timing_observe(timing, echo, receiver, PCM_HOST_ECHO_FILTER,
	      0, 32000, 32766, true) == 0 && !timing->initialized &&
	      memcmp(&fixture->saved, receiver, sizeof(*receiver)) == 0,
	      "physical overflow retained evidence or changed the receiver");
	free(fixture);
	return true;
}

int main(void)
{
	if (!test_residual_projection()) return 1;
	if (!test_wire_cells()) return 1;
	if (!test_constellation_scores()) return 1;
	unsigned cases = 0;
	for (unsigned law = 0; law < 2; law++)
		for (int shift = -97; shift <= 97; shift += 97)
			for (unsigned g = 0; g < 3; g++) {
				const double gains[] = { .75, 1., 2. };
				struct channel_case test = { .law = law, .seed = g + 1, .host_gain = gains[g],
					.peak = 300, .noise = 4, .inverse = true, .shift = shift,
					.drift = true, .gap = -1 };
				if (!run_case(&test)) return 1;
				cases++;
			}
	for (unsigned law = 0; law < 2; law++)
		for (unsigned robbing = 0; robbing < 3; robbing++)
			for (unsigned seed = 1; seed <= 4; seed++) {
				const int shifts[] = { -320, -97, -1, 0, 1, 97, 320 };
				struct channel_case test = { .law = law, .robbing = robbing, .seed = seed,
					.host_gain = 1, .peak = seed & 1 ? 100 : 300, .noise = 4, .gap = -1 };
				for (unsigned s = 0; s < ARRAY_LENGTH(shifts); s++)
					for (unsigned inverse = 0; inverse < 2; inverse++)
						for (unsigned nonuniform = 0; nonuniform < 2; nonuniform++) {
							test.shift = shifts[s]; test.inverse = inverse; test.nonuniform = nonuniform;
							if (!run_case(&test)) return 1;
							cases++;
						}
				test.shift = 0; test.inverse = true; test.nonuniform = false;
				test.peak = 100; test.noise = 100;
				if (!run_case(&test)) return 1;
				test.unrelated = test.nonuniform = true; test.peak = 300;
				if (!run_case(&test)) return 1;
				cases += 2;
			}
	const double gains[] = { .5, .8, 1.7, 2. };
	for (unsigned g = 0; g < ARRAY_LENGTH(gains); g++)
		for (unsigned law = 0; law < 2; law++)
			for (unsigned robbing = 0; robbing < 3; robbing++)
				for (unsigned seed = 1; seed <= 2; seed++) {
					const int shifts[] = { -97, 0, 97 };
					struct channel_case test = { .law = law, .robbing = robbing, .seed = seed,
						.host_gain = gains[g], .peak = seed & 1 ? 100 : 300, .noise = 4, .gap = -1 };
					for (unsigned s = 0; s < ARRAY_LENGTH(shifts); s++)
						for (unsigned inverse = 0; inverse < 2; inverse++)
							for (unsigned nonuniform = 0; nonuniform < 2; nonuniform++) {
								test.shift = shifts[s]; test.inverse = inverse; test.nonuniform = nonuniform;
								if (!run_case(&test)) return 1;
								cases++;
							}
					test.shift = 0; test.inverse = true; test.nonuniform = false;
					test.peak = 100; test.noise = 100;
					if (!run_case(&test)) return 1;
					test.unrelated = test.nonuniform = true; test.peak = 300;
					if (!run_case(&test)) return 1;
					cases += 2;
				}
	for (unsigned law = 0; law < 2; law++)
		for (unsigned seed = 1; seed <= 4; seed++) {
			struct channel_case test = { .law = law, .seed = seed, .host_gain = 1,
				.peak = 300, .noise = 4, .inverse = true, .shift = 97, .transient = 128, .gap = -1 };
			if (!run_case(&test)) return 1;
			test.shift = -97; test.transient = 256; test.nonuniform = true;
			test.robbing = 1; test.peak = 100;
			if (!run_case(&test)) return 1;
			test.shift = 97; test.transient = 0; test.nonuniform = false;
			test.robbing = 0; test.peak = 300; test.gap = GAP;
			if (!run_case(&test)) return 1;
			test.shift = -97; test.nonuniform = true; test.robbing = 2; test.peak = 100;
			if (!run_case(&test)) return 1;
			cases += 4;
		}
	printf("PCM echo timing: %u channel and evidence-boundary cases passed\n", cases);
	return 0;
}
