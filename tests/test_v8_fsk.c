// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <v21.h>
#include <v8_fsk.h>
#include <v8_menu.h>

#include "test.h"
#include "v8_line_state_fixture.h"

#include <math.h>
#include <string.h>

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

static bool channel_matches(
	const struct fsk_channel *actual,
	const struct fsk_channel *expected)
{
	return actual->sample_rate == expected->sample_rate &&
	       actual->symbol_rate_numerator ==
		       expected->symbol_rate_numerator &&
	       actual->symbol_rate_denominator ==
		       expected->symbol_rate_denominator &&
	       actual->mark_frequency_hz == expected->mark_frequency_hz &&
	       actual->space_frequency_hz == expected->space_frequency_hz;
}

static uint64_t hash_word(uint64_t hash, uint16_t word)
{
	hash ^= word & UINT16_C(0xff);
	hash *= UINT64_C(1099511628211);
	hash ^= word >> 8;
	return hash * UINT64_C(1099511628211);
}

static bool test_station_channels(void)
{
	struct v8_fsk caller;
	struct v8_fsk answerer;

	CHECK(v8_fsk_init(
		      &caller, V8_CALLING_STATION) ==
		      V8_FSK_OK &&
	      v8_fsk_init(
		      &answerer, V8_ANSWERING_STATION) ==
		      V8_FSK_OK,
	      "V.8 FSK stations did not initialize");
	CHECK(channel_matches(
		      &caller.receiver.channel, &v21_channel_2) &&
	      channel_matches(
		      &answerer.receiver.channel, &v21_channel_1) &&
	      channel_matches(&caller.transmitter.source.channel, &v21_channel_1) &&
	      channel_matches(&answerer.transmitter.source.channel, &v21_channel_2) &&
	      caller.transmitter.source.tones.amplitude == 0x3ead &&
	      answerer.transmitter.source.tones.amplitude == 0x3ead,
	      "V.8 FSK stations selected the wrong V.21 channels");
	return true;
}

static bool test_configured_receiver_channel(void)
{
	for (enum v8_station_role role = V8_CALLING_STATION;
	     role <= V8_ANSWERING_STATION; ++role) {
		struct v8_fsk fsk, saved;
		uint8_t bit = 99;
		bool ready = true;

		CHECK(v8_fsk_init(&fsk, role) == V8_FSK_OK,
		      "V.8 channel contract fixture did not initialize");
		/* The generic detector accepts this smoothing value, but it is
		 * not the detector configured for this station's V.21 channel. */
		fsk.receiver.channel.metric_smoothing_shift ^= 1u;
		memcpy(&saved, &fsk, sizeof(saved));
		CHECK(v8_fsk_receive_sample(&fsk, 0, &bit, &ready) ==
			      V8_FSK_INVALID_ARGUMENT && bit == 99 && ready &&
		      memcmp(&saved, &fsk, sizeof(saved)) == 0,
		      "V.8 accepted or mutated a mismatched receiver channel");
	}
	return true;
}

static bool test_transmit_oracle(void)
{
	/* Reviewed 32-bit continuous-phase source plus the retained V.8 FIR.
	 * The separate nominal-tone and clock tests constrain this wire change. */
	static const uint64_t expected_hash[] = {
		UINT64_C(0xe0ae01d6334beaa4),
		UINT64_C(0x448f815c4704b6e0),
	};
	static const unsigned int expected_periods[] = { 27, 27, 26 };

	for (enum v8_station_role role = V8_CALLING_STATION;
	     role <= V8_ANSWERING_STATION; ++role) {
		struct v8_fsk fsk;
		uint64_t hash = UINT64_C(14695981039346656037);
		unsigned int period = 0;
		unsigned int period_index = 0;

		CHECK(v8_fsk_init(&fsk, role) == V8_FSK_OK,
		      "V.8 FSK oracle role %u did not initialize", role);
		for (size_t index = 0; index < 512; ++index) {
			int16_t sample;
			bool consumed;

			CHECK(v8_fsk_transmit_sample(
				      &fsk, 1, &sample, &consumed) == V8_FSK_OK,
			      "V.8 FSK oracle role %u stopped at %zu",
			      role, index);
			hash = hash_word(hash, (uint16_t)sample);
			period++;
			if (consumed && period_index < ARRAY_LENGTH(expected_periods)) {
				CHECK(period == expected_periods[period_index] &&
					      v8_fsk_can_accept_bit(&fsk),
				      "V.8 FSK bit period %u was %u",
				      period_index, period);
				period = 0;
				period_index++;
			}
		}
		CHECK(hash == expected_hash[role] &&
			      period_index == ARRAY_LENGTH(expected_periods),
		      "V.8 FSK role %u wire hash changed: %#llx",
		      role, (unsigned long long)hash);
	}
	return true;
}

/* At 300 baud, 10,000 samples end on exactly 375 symbol boundaries.
 * Changing the input between those boundaries must not replace a held bit. */
static bool test_transmit_clock_latching(void)
{
	for (enum v8_station_role role = V8_CALLING_STATION;
	     role <= V8_ANSWERING_STATION; ++role) {
		struct v8_fsk actual, held;
		uint8_t bit = 0;
		unsigned symbols = 0;
		CHECK(v8_fsk_init(&actual, role) == V8_FSK_OK &&
		      v8_fsk_init(&held, role) == V8_FSK_OK, "FSK latch initialization");
		for (unsigned n = 0; n < 10000; ++n) {
			bool boundary = n == 0 || n * 3 / 80 != (n - 1) * 3 / 80;
			uint8_t offered = (uint8_t)((n * 17 / 13) & 1);
			bool consumed, held_consumed;
			int16_t sample, held_sample;
			CHECK(v8_fsk_can_accept_bit(&actual) == boundary,
			      "FSK role %d readiness at sample %u", role, n);
			if (boundary) bit = offered;
			CHECK(v8_fsk_transmit_sample(&actual, offered, &sample, &consumed) == V8_FSK_OK &&
			      v8_fsk_transmit_sample(&held, bit, &held_sample, &held_consumed) == V8_FSK_OK &&
			      sample == held_sample && consumed == held_consumed &&
			      consumed == ((n + 1) * 3 / 80 != n * 3 / 80),
			      "FSK role %d clock or held bit changed at sample %u", role, n);
			symbols += consumed;
		}
		CHECK(symbols == 375 && v8_fsk_can_accept_bit(&actual),
		      "FSK role %d fractional clock drifted", role);
	}
	return true;
}

/* Project a full second onto the nominal tone, independently of oscillator
 * phase or its implementation. After the unchanged FIR has settled, its
 * calibrated waveform is about 4,000 peak PCM units. Integer output rounding
 * permits one unit RMS residual, without accepting the old frequency offset. */
static bool test_transmit_spectrum(void)
{
	const double tau = 6.28318530717958647693;
	for (enum v8_station_role role = V8_CALLING_STATION;
	     role <= V8_ANSWERING_STATION; ++role) {
		const struct fsk_channel *channel = role == V8_CALLING_STATION ?
			&v21_channel_1 : &v21_channel_2;
		for (unsigned bit = 0; bit < 2; ++bit) {
			struct v8_fsk fsk;
			double energy = 0, in_phase = 0, quadrature = 0, sum = 0;
			unsigned hz = bit ? channel->mark_frequency_hz : channel->space_frequency_hz;
			CHECK(v8_fsk_init(&fsk, role) == V8_FSK_OK, "FSK spectrum initialization");
			for (unsigned n = 0; n < 8064; ++n) {
				int16_t sample;
				bool consumed;
				CHECK(v8_fsk_transmit_sample(&fsk, bit, &sample, &consumed) == V8_FSK_OK,
				      "FSK spectrum stopped");
				if (n < 64) continue;
				double phase = tau * hz * (n - 64) / 8000;
				in_phase += sample * cos(phase);
				quadrature += sample * sin(phase);
				energy += (double)sample * sample;
				sum += sample;
			}
			in_phase /= 4000;
			quadrature /= 4000;
			double amplitude = hypot(in_phase, quadrature);
			double residual = energy / 8000 - amplitude * amplitude / 2 -
				(sum / 8000) * (sum / 8000);
			CHECK(amplitude > 3900 && amplitude < 4100 && residual > -1e-6 && residual < 1,
			      "FSK role %d bit %u nominal tone/power changed: amplitude %.6f residual %.6f",
			      role, bit, amplitude, residual);
		}
	}
	return true;
}

static bool test_configured_transmitter_channel(void)
{
	for (enum v8_station_role role = V8_CALLING_STATION;
	     role <= V8_ANSWERING_STATION; ++role) {
		for (unsigned fault = 0; fault < 6; ++fault) {
			struct v8_fsk fsk, saved;
			int16_t sample = 123;
			bool consumed = true;
			CHECK(v8_fsk_init(&fsk, role) == V8_FSK_OK, "FSK invalid transmitter setup");
			switch (fault) {
			case 0: fsk.transmitter.source.channel.metric_smoothing_shift ^= 1; break;
			case 1: fsk.transmitter.source.tones.mark_phase_step ^= 1; break;
			case 2: fsk.transmitter.source.tones.amplitude--; break;
			case 3: fsk.transmitter.source.held_bit = 2; break;
			case 4: fsk.transmitter.source.symbol_accumulator = 8000; break;
			case 5: fsk.transmitter.source.tones.sample_rate = 4000; break;
			}
			saved = fsk;
			CHECK(!v8_fsk_can_accept_bit(&fsk) &&
			      v8_fsk_transmit_sample(&fsk, 1, &sample, &consumed) == V8_FSK_INVALID_ARGUMENT &&
			      sample == 123 && consumed && !memcmp(&saved, &fsk, sizeof(fsk)),
			      "FSK role %d malformed transmitter %u mutated", role, fault);
		}
	}
	return true;
}

/* Preserve the historical advance-before-output phase convention while
 * sharing the normal FSK oscillator and clock. The phase oracle integrates
 * nominal frequencies independently; the separate FIR instance isolates
 * this oscillator boundary from the already checked V.8 filter response. */
static bool test_transmit_transition_phase(void)
{
	for (enum v8_station_role role = V8_CALLING_STATION;
	     role <= V8_ANSWERING_STATION; ++role) {
		struct v8_fsk actual, filter;
		const struct fsk_channel *channel = role == V8_CALLING_STATION ?
			&v21_channel_1 : &v21_channel_2;
		uint32_t phase = UINT32_C(0x40000000);
		uint8_t held = 0;

		CHECK(v8_fsk_init(&actual, role) == V8_FSK_OK &&
		      v8_fsk_init(&filter, role) == V8_FSK_OK,
		      "phase oracle initialization");
		for (unsigned n = 0; n < 10000; ++n) {
			uint8_t offered = (uint8_t)((n * 17 / 13) & 1);
			bool boundary = n == 0 ||
				n * 3 / 80 != (n - 1) * 3 / 80;
			uint64_t frequency;
			int16_t raw, expected, sample;
			bool consumed;

			if (boundary)
				held = offered;
			frequency = held ? channel->mark_frequency_hz :
				channel->space_frequency_hz;
			phase += (uint32_t)(((frequency << 32) + 4000) / 8000);
			raw = (int16_t)lround(0x3ead * sin((double)phase *
				(6.283185307179586476925286766559 / 4294967296.0)));
			CHECK(v8_fsk_filter_transmit_sample(
				      &filter, raw, &expected) == V8_FSK_OK &&
			      v8_fsk_transmit_sample(
				      &actual, offered, &sample, &consumed) == V8_FSK_OK &&
			      sample == expected,
			      "FSK role %d transition phase changed at %u", role, n);
		}
	}
	return true;
}

static bool menu_round_trip(
	enum v8_station_role source_role,
	enum test_codec codec)
{
	struct v8_control_generator generator;
	struct v8_menu_detector detector;
	struct v8_menu offered;
	struct v8_menu detected = { 0 };
	struct v8_fsk source;
	struct v8_fsk destination;
	struct v8_fsk blocked;
	uint8_t information[V8_MAX_INFORMATION_OCTETS];
	uint8_t transmit_bit = 1;
	size_t information_count;
	unsigned int completed_sequences = 0;
	bool confirmed = false;

	CHECK(v8_call_menu_init(
		      &offered, V8_MODULATION_V21, true) ==
		      V8_MENU_OK &&
	      v8_menu_encode(
		      &offered, information, sizeof(information),
		      &information_count) == V8_MENU_OK &&
	      v8_control_generator_init(
		      &generator, information, information_count) ==
		      V8_FRAMING_OK &&
	      v8_menu_detector_init(&detector) ==
		      V8_MENU_OK &&
	      v8_fsk_init(&source, source_role) ==
		      V8_FSK_OK &&
	      v8_fsk_init(
		      &destination,
		      source_role == V8_CALLING_STATION ?
			      V8_ANSWERING_STATION :
			      V8_CALLING_STATION) ==
		      V8_FSK_OK,
	      "V.8 menu audio endpoints did not initialize");
	blocked = destination;

	for (size_t sample_index = 0;
	     sample_index < 3u * V21_SAMPLE_RATE && !confirmed;
	     ++sample_index) {
		int16_t sample;
		uint8_t receive_bit;
		bool bit_consumed;
		bool bit_ready;
		struct v8_line_conditioned_block block, saved_block;
		struct v8_line_conditioner saved_line;
		uint8_t blocked_bit = 0;
		bool ready, blocked_ready = false;

		if (v8_fsk_can_accept_bit(&source)) {
			bool sequence_complete;

			CHECK(v8_control_next_bit(
				      &generator, &transmit_bit,
				      &sequence_complete) ==
				      V8_FRAMING_OK,
			      "V.8 menu bit generation failed");
			if (sequence_complete)
				completed_sequences++;
		}
		CHECK(v8_fsk_transmit_sample(
			      &source, transmit_bit, &sample, &bit_consumed) ==
			      V8_FSK_OK &&
		      v8_fsk_receive_sample(
			      &destination, codec_round_trip(sample, codec),
			      &receive_bit, &bit_ready) ==
			      V8_FSK_OK,
		      "V.8 menu audio processing failed");
		CHECK(v8_line_conditioner_process_sample(&blocked.line,
			      codec_round_trip(sample, codec), &block, &ready) == V8_LINE_CONDITIONER_OK,
		      "shared FSK conditioning failed");
		if (ready) {
			memcpy(&saved_block, &block, sizeof(block));
			saved_line = blocked.line;
			CHECK(v8_fsk_receive_block(&blocked, &block, &blocked_bit, &blocked_ready) == V8_FSK_OK &&
			      v8_line_states_equal(&saved_line, &blocked.line) &&
			      v8_line_blocks_equal(&saved_block, &block),
			      "FSK block processing advanced the line or modified input");
		}
		CHECK(blocked_bit == receive_bit && blocked_ready == bit_ready &&
		      memcmp(&blocked, &destination, sizeof(blocked)) == 0,
		      "FSK block processing changed samplewise state or decisions");
		if (bit_ready)
			CHECK(v8_menu_detect_bit(
				      &detector, receive_bit, &detected,
				      &confirmed) == V8_MENU_OK,
			      "V.8 menu bit recovery failed");
	}
	CHECK(confirmed && completed_sequences >= 2 &&
		      detected.call_function == offered.call_function &&
		      detected.modulations == offered.modulations &&
		      detected.modulation_octets == offered.modulation_octets &&
		      detected.lapm == offered.lapm,
	      "V.8 role %d codec %d did not recover the repeated menu",
	      source_role, codec);
	return true;
}

static bool test_menu_audio_round_trip(void)
{
	for (enum test_codec codec = TEST_LINEAR; codec <= TEST_PCMA; ++codec)
		if (!menu_round_trip(V8_CALLING_STATION, codec) ||
		    !menu_round_trip(V8_ANSWERING_STATION, codec))
			return false;
	return true;
}

static bool test_transmit_ownership(void)
{
	for (enum v8_station_role role = V8_CALLING_STATION;
	     role <= V8_ANSWERING_STATION; ++role) {
		struct {
			int16_t before;
			struct v8_fsk fsk;
			int16_t after;
		} adjacent = { .before = 123, .after = 456 };
		struct v8_fsk *fsk = &adjacent.fsk;
		struct v8_fsk saved, twin;
		int16_t sample = 123, expected;
		bool consumed = true, expected_consumed;
		int16_t *sample_aliases[] = {
			&fsk->transmitter.filter_history[0],
			&fsk->transmitter.filter_history[V8_FSK_TRANSMIT_FILTER_TAPS - 1],
			&fsk->line.entrance_history[0],
			&fsk->receiver.history[0],
			(void *)&fsk->role,
		};
		bool *consumed_aliases[] = {
			&fsk->transmitter.source.holding_bit,
			(void *)&fsk->line.input_count,
			&fsk->receiver.timing_locked,
			(void *)&fsk->role,
		};

		CHECK(v8_fsk_init(fsk, role) == V8_FSK_OK,
		      "V.8 transmit ownership fixture role %d", role);
		for (size_t index = 0; index < 128; ++index) {
			CHECK(v8_fsk_transmit_sample(fsk, (index / 7u) & 1u,
				      &sample, &consumed) == V8_FSK_OK &&
			      v8_fsk_filter_transmit_sample(fsk,
				      (int16_t)((int)index * 137 - 16384),
				      &sample) == V8_FSK_OK,
			      "V.8 transmit warmup role %d sample %zu", role, index);
		}
		memcpy(&saved, fsk, sizeof(saved));
		sample = 123;
		consumed = true;
		for (size_t index = 0; index < ARRAY_LENGTH(sample_aliases); ++index)
			CHECK(v8_fsk_transmit_sample(fsk, 0, sample_aliases[index],
				      &consumed) == V8_FSK_INVALID_ARGUMENT &&
			      v8_fsk_filter_transmit_sample(fsk, INT16_MIN,
				      sample_aliases[index]) == V8_FSK_INVALID_ARGUMENT &&
			      memcmp(fsk, &saved, sizeof(saved)) == 0 && consumed,
			      "V.8 role %d accepted sample owner alias %zu", role, index);
		for (size_t index = 0; index < ARRAY_LENGTH(consumed_aliases); ++index)
			CHECK(v8_fsk_transmit_sample(fsk, 0, &sample,
				      consumed_aliases[index]) == V8_FSK_INVALID_ARGUMENT &&
			      memcmp(fsk, &saved, sizeof(saved)) == 0 && sample == 123,
			      "V.8 role %d accepted consumed owner alias %zu", role, index);
		for (size_t index = 0; index < sizeof(sample); ++index) {
			union {
				int16_t sample;
				unsigned char bytes[sizeof(int16_t)];
			} outputs = { .sample = 123 };

			CHECK(v8_fsk_transmit_sample(fsk, 0, &outputs.sample,
				      (void *)&outputs.bytes[index]) == V8_FSK_INVALID_ARGUMENT &&
			      memcmp(fsk, &saved, sizeof(saved)) == 0 && outputs.sample == 123,
			      "V.8 role %d accepted overlapping outputs at %zu", role, index);
		}
		CHECK(v8_fsk_transmit_sample(fsk, 2, &sample, &consumed) == V8_FSK_INVALID_ARGUMENT &&
		      v8_fsk_transmit_sample(fsk, 0, NULL, &consumed) == V8_FSK_INVALID_ARGUMENT &&
		      v8_fsk_transmit_sample(fsk, 0, &sample, NULL) == V8_FSK_INVALID_ARGUMENT &&
		      v8_fsk_filter_transmit_sample(fsk, 0, NULL) == V8_FSK_INVALID_ARGUMENT &&
		      memcmp(fsk, &saved, sizeof(saved)) == 0 && sample == 123 && consumed,
		      "V.8 role %d changed warmed state on rejection", role);

		twin = *fsk;
		CHECK(v8_fsk_transmit_sample(&twin, 0, &expected, &expected_consumed) == V8_FSK_OK &&
		      v8_fsk_transmit_sample(fsk, 0, &adjacent.before, &consumed) == V8_FSK_OK &&
		      adjacent.before == expected && adjacent.after == 456 &&
		      consumed == expected_consumed && memcmp(fsk, &twin, sizeof(twin)) == 0,
		      "V.8 role %d rejected disjoint neighboring sample", role);
		CHECK(v8_fsk_filter_transmit_sample(&twin, INT16_MAX, &expected) == V8_FSK_OK &&
		      v8_fsk_filter_transmit_sample(fsk, INT16_MAX, &adjacent.after) == V8_FSK_OK &&
		      adjacent.after == expected && memcmp(fsk, &twin, sizeof(twin)) == 0,
		      "V.8 role %d rejected disjoint neighboring filtered sample", role);
	}
	return true;
}

static bool receive_rejects_unchanged(struct v8_fsk *fsk, const char *reason)
{
	struct v8_fsk saved;
	uint8_t bit = 7;
	bool ready = true;

	memcpy(&saved, fsk, sizeof(saved));
	CHECK(v8_fsk_receive_sample(fsk, 0, &bit, &ready) == V8_FSK_INVALID_ARGUMENT &&
	      memcmp(fsk, &saved, sizeof(saved)) == 0 && bit == 7 && ready,
	      "V.8 receive role %d changed owner or outputs: %s", fsk->role, reason);
	return true;
}

static bool test_receive_rollback(void)
{
	for (enum v8_station_role role = V8_CALLING_STATION;
	     role <= V8_ANSWERING_STATION; ++role) {
		struct v8_fsk fsk, saved;
		struct fsk_receiver saved_receiver;
		uint8_t bit = 7;
		bool ready = true;

		CHECK(v8_fsk_init(&fsk, role) == V8_FSK_OK,
		      "V.8 deferred detector fixture role %d", role);
		fsk.receiver.samples_per_symbol_q16 = 0;
		memcpy(&saved_receiver, &fsk.receiver, sizeof(saved_receiver));
		/* Until a block is ready, only the line front end advances. */
		for (size_t index = 0; index < V8_LINE_INPUT_QUEUE_SAMPLES - 1; ++index)
			CHECK(v8_fsk_receive_sample(&fsk, 0, &bit, &ready) == V8_FSK_OK &&
			      memcmp(&fsk.receiver, &saved_receiver, sizeof(saved_receiver)) == 0 &&
			      bit == 0 && !ready,
			      "V.8 role %d advanced detector without a block", role);
		CHECK(receive_rejects_unchanged(&fsk, "deferred detector rejection"),
		      "V.8 deferred detector rollback failed");
		memcpy(&saved, &fsk, sizeof(saved));
		bit = 7;
		ready = true;
		CHECK(v8_fsk_receive_sample(&fsk, 0, (void *)&fsk.receiver, &ready) == V8_FSK_INVALID_ARGUMENT &&
		      v8_fsk_receive_sample(&fsk, 0, &bit, (void *)&fsk.line) == V8_FSK_INVALID_ARGUMENT &&
		      v8_fsk_receive_sample(&fsk, 0, (void *)&ready, &ready) == V8_FSK_INVALID_ARGUMENT &&
		      memcmp(&fsk, &saved, sizeof(saved)) == 0 && bit == 7 && ready,
		      "V.8 receive role %d accepted overlapping outputs", role);

		CHECK(v8_fsk_init(&fsk, role) == V8_FSK_OK,
		      "V.8 partial detector fixture role %d", role);
		fsk.line.input_count = V8_LINE_INPUT_QUEUE_SAMPLES - 1;
		fsk.receiver.history_count = fsk.receiver.channel.detector_window;
		fsk.receiver.samples_per_symbol_q16 = UINT32_C(1) << 16;
		fsk.receiver.sample_countdown_q16 = 1;
		fsk.receiver.timing_locked = true;
		fsk.receiver.last_bit = 1;
		/* A second bit in the same block rejects after detector mutation. */
		CHECK(receive_rejects_unchanged(&fsk, "multiple bits from one block"),
		      "V.8 partial detector rollback failed");

		CHECK(v8_fsk_init(&fsk, role) == V8_FSK_OK,
		      "V.8 gain postvalidation fixture role %d", role);
		fsk.line.input_count = V8_LINE_INPUT_QUEUE_SAMPLES - 1;
		fsk.line.gain_mode = V8_LINE_GAIN_COARSE;
		fsk.line.gain = 1;
		fsk.line.filtered_energy = INT16_MAX;
		fsk.line.entrance_history[16] = INT16_MAX;
		for (size_t index = 0; index < fsk.line.input_count; ++index)
			fsk.line.input_queue[index] = INT16_MAX;
		/* Gain contraction rounds to zero and fails line postvalidation. */
		CHECK(receive_rejects_unchanged(&fsk, "line gain postvalidation"),
		      "V.8 line postvalidation rollback failed");
	}
	return true;
}

static bool test_block_validation(void)
{
	struct v8_fsk fsk, saved;
	struct v8_line_conditioned_block block = { 0 }, saved_block = { 0 };
	uint8_t bit = 7;
	bool ready = true;

	CHECK(v8_fsk_init(&fsk, V8_CALLING_STATION) == V8_FSK_OK,
	      "FSK block validation fixture");
	saved = fsk;
	CHECK(v8_fsk_receive_block(NULL, &block, &bit, &ready) == V8_FSK_INVALID_ARGUMENT &&
	      v8_fsk_receive_block(&fsk, NULL, &bit, &ready) == V8_FSK_INVALID_ARGUMENT &&
	      v8_fsk_receive_block(&fsk, &block, NULL, &ready) == V8_FSK_INVALID_ARGUMENT &&
	      v8_fsk_receive_block(&fsk, &block, &bit, NULL) == V8_FSK_INVALID_ARGUMENT &&
	      v8_fsk_receive_block(&fsk, (void *)&fsk, &bit, &ready) == V8_FSK_INVALID_ARGUMENT &&
	      v8_fsk_receive_block(&fsk, &block, (void *)&fsk, &ready) == V8_FSK_INVALID_ARGUMENT &&
	      v8_fsk_receive_block(&fsk, &block, &bit, (void *)&fsk) == V8_FSK_INVALID_ARGUMENT &&
	      v8_fsk_receive_block(&fsk, &block, (void *)&block, &ready) == V8_FSK_INVALID_ARGUMENT &&
	      v8_fsk_receive_block(&fsk, &block, &bit, (void *)&block) == V8_FSK_INVALID_ARGUMENT &&
	      v8_fsk_receive_block(&fsk, &block, (void *)&ready, &ready) == V8_FSK_INVALID_ARGUMENT &&
	      memcmp(&fsk, &saved, sizeof(fsk)) == 0 &&
	      v8_line_blocks_equal(&block, &saved_block) && bit == 7 && ready,
	      "FSK block rejection changed state, input, or output");
	fsk.line.role = V8_ANSWERING_STATION;
	saved = fsk;
	CHECK(v8_fsk_receive_block(&fsk, &block, &bit, &ready) == V8_FSK_INVALID_ARGUMENT &&
	      memcmp(&fsk, &saved, sizeof(fsk)) == 0 && bit == 7 && ready,
	      "FSK block accepted inconsistent line ownership");
	return true;
}

static bool test_validation_and_reset(void)
{
	struct v8_fsk fsk;
	struct v8_fsk saved;
	int16_t sample = 123;
	uint8_t bit = 7;
	bool ready = true;

	CHECK(v8_fsk_init(
		      &fsk, V8_CALLING_STATION) ==
		      V8_FSK_OK,
	      "V.8 FSK validation fixture did not initialize");
	fsk.role = (enum v8_station_role)2;
	saved = fsk;
	CHECK(v8_fsk_init(
		      NULL, V8_CALLING_STATION) ==
		      V8_FSK_INVALID_ARGUMENT &&
	      v8_fsk_init(
		      &fsk, (enum v8_station_role)2) ==
		      V8_FSK_INVALID_ARGUMENT &&
	      memcmp(&fsk, &saved, sizeof(fsk)) == 0 &&
	      !v8_fsk_can_accept_bit(&fsk) &&
	      v8_fsk_transmit_sample(
		      &fsk, 1, &sample, &ready) ==
		      V8_FSK_INVALID_ARGUMENT &&
	      sample == 123 && ready &&
	      v8_fsk_receive_sample(
		      &fsk, 0, &bit, &ready) ==
		      V8_FSK_INVALID_ARGUMENT &&
	      bit == 7 && ready &&
	      v8_fsk_reset(&fsk) ==
		      V8_FSK_INVALID_ARGUMENT,
	      "V.8 FSK API accepted invalid state");

	CHECK(v8_fsk_init(
		      &fsk, V8_ANSWERING_STATION) ==
		      V8_FSK_OK,
	      "V.8 FSK valid-argument fixture did not initialize");
	saved = fsk;
	sample = 123;
	ready = true;
	CHECK(v8_fsk_transmit_sample(
		      &fsk, 2, &sample, &ready) ==
		      V8_FSK_INVALID_ARGUMENT &&
	      sample == 123 && ready && memcmp(&fsk, &saved, sizeof(fsk)) == 0 &&
	      v8_fsk_transmit_sample(
		      &fsk, 1, NULL, &ready) ==
		      V8_FSK_INVALID_ARGUMENT &&
	      v8_fsk_receive_sample(
		      &fsk, 0, NULL, &ready) ==
		      V8_FSK_INVALID_ARGUMENT &&
	      memcmp(&fsk, &saved, sizeof(fsk)) == 0,
	      "V.8 FSK API changed state after an invalid argument");

	CHECK(v8_fsk_transmit_sample(
		      &fsk, 1, &sample, &ready) == V8_FSK_OK &&
	      v8_fsk_reset(&fsk) == V8_FSK_OK &&
	      fsk.role == V8_ANSWERING_STATION &&
	      fsk.transmitter.source.tones.carrier_phase ==
		      UINT32_C(0x40000000) + fsk.transmitter.source.tones.mark_phase_step &&
	      fsk.transmitter.source.symbol_accumulator == 0 &&
	      !fsk.transmitter.source.holding_bit &&
	      fsk.transmitter.filter_history[33] == 0x1000 &&
	      fsk.receiver.history_count == 0,
	      "V.8 FSK reset did not preserve role and clear stream state");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "station channels", test_station_channels },
		{ "configured receiver channel", test_configured_receiver_channel },
		{ "transmit oracle", test_transmit_oracle },
		{ "transmit clock and latching", test_transmit_clock_latching },
		{ "transmit spectrum", test_transmit_spectrum },
		{ "transmit transition phase", test_transmit_transition_phase },
		{ "configured transmitter channel", test_configured_transmitter_channel },
		{ "menu audio round trip", test_menu_audio_round_trip },
		{ "transmit ownership", test_transmit_ownership },
		{ "receive rollback", test_receive_rollback },
		{ "block validation", test_block_validation },
		{ "validation and reset", test_validation_and_reset },
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
