// SPDX-License-Identifier: BSD-3-Clause

#include <modem.h>
#include <modem_frontend.h>
#include <baudot.h>
#include <fsk300_datapump.h>
#include <fsk.h>
#include <fractional_delay.h>
#include <g711.h>
#include <media.h>
#include <qam.h>
#include <single_tone.h>
#include <v21.h>
#include <v22_datapump.h>
#include <v22_framing.h>
#include <v23_datapump.h>
#include <datapump.h>
#include <datapump_link.h>
#include <v8_ansam.h>
#include <v8_datapump.h>
#include <v34_datapump.h>
#include <v34_frame_schedule.h>
#include <v90_profile.h>
#include <v90_datapump.h>

#include "channel_bench.h"
#include "test.h"
#include "lapm_state_fixture.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum {
	SIP_FRAME_SAMPLES = 160,
	ENDPOINT_OUTPUT_CAPACITY = 1024,
	TRAINING_FRAME_LIMIT = 250,
	NEGOTIATED_TRAINING_FRAME_LIMIT = 500,
	/* Both V.32 endpoints now retain their complete role-specific echo
	 * training intervals.  Leave more than one second of protocol margin
	 * around both intervals without relaxing the modem's own deadline. */
	V32_TRAINING_FRAME_LIMIT = 900,
	LINK_DRAIN_FRAME_LIMIT = 400,
	DATA_FRAME_LIMIT = 400,
	RETRAIN_FRAME_LIMIT = 500,
	V32_RETRAIN_FRAME_LIMIT = 750,
	/* Nonzero V.92 DIL adds about four seconds to full retraining. */
	V92_RETRAIN_FRAME_LIMIT = 750,
	RETRAIN_RESPONSE_LOSS_FRAMES = 70,
	MAX_CALLER_DELAY_FRAMES = 70,
	/* 2,400 line samples are 720 V.32 symbols. */
	V32_LONG_DELAY_FRAMES = 15,
	V32_LONG_TRAINING_FRAME_LIMIT = 1000,
	DIGITAL_MEDIA_DELAY_SAMPLES = 1200,
	V32_MIN_HALF_DUPLEX_GAP_FRAMES = 16,
	REPLAY_MAX_SAMPLES = MODEM_RATE * 180,
	V22_REPLAY_CONNECT_SAMPLES = MODEM_RATE * 15,
};

struct integration_endpoint {
	struct modem *modem;
	struct channel_bench receive_bench;
	int modem_fd;
	int client_fd;
	int16_t receive[SIP_FRAME_SAMPLES];
	int16_t transmit[SIP_FRAME_SAMPLES];
	uint8_t receive_codewords[SIP_FRAME_SAMPLES];
	uint8_t transmit_codewords[SIP_FRAME_SAMPLES];
	uint8_t delayed_codewords[DIGITAL_MEDIA_DELAY_SAMPLES];
	uint8_t output[ENDPOINT_OUTPUT_CAPACITY];
	size_t output_count;
	size_t exact_frames;
	uint64_t process_ns;
	size_t process_samples;
	size_t delayed_codeword_cursor;
	size_t stops;
	size_t erased_samples;
	bool delay_codewords;
	bool robbed_bits;
	unsigned robbed_position;
	bool profile_cpu;
	bool delay_one_sample;
	uint8_t prior_codeword;
};

struct recording_capture {
	size_t blocks[RECORD_TX_RTP_PAYLOAD_SENT + 1];
	size_t units[RECORD_TX_RTP_PAYLOAD_SENT + 1];
	bool invalid;
};

struct media_frame_delay {
	uint8_t codewords[MAX_CALLER_DELAY_FRAMES][SIP_FRAME_SAMPLES];
	size_t head;
	size_t count;
};

struct v22_rate_monitor {
	struct qam_demodulator demodulator;
	struct v22bis_rate_signal_detector detector;
	struct qam_point previous;
	size_t samples;
	size_t minimum_sample;
	unsigned int starts;
	bool has_previous;
};

enum integration_impairment {
	INTEGRATION_IMPAIRMENT_NONE,
	INTEGRATION_IMPAIRMENT_COMPOSITE,
	INTEGRATION_IMPAIRMENT_DIGITAL_DELAY,
	INTEGRATION_IMPAIRMENT_TRAINING_NOISE,
};

struct modem_test_case {
	enum DP_ID id;
	enum DP_ID answerer_id;
	enum DP_ID connected_id;
	enum DP_ID answerer_connected_id;
	const char *name;
	const char *selection_command;
	const char *answerer_selection_command;
	unsigned int caller_tx_rate;
	unsigned int caller_rx_rate;
	unsigned int answerer_tx_rate;
	unsigned int answerer_rx_rate;
	unsigned int carrier_loss_samples;
	unsigned int training_frame_limit;
	unsigned int answer_tone_prefix_ms;
	unsigned int caller_delay_frames;
	unsigned int minimum_caller_rate_signals;
	unsigned int minimum_caller_rate_signal_sample;
	bool answer_tone_prefix_modulated;
	bool automode;
	bool answerer_automode;
	bool answerer_fixed;
	bool requires_v8;
	bool requires_exact_g711;
	bool test_prelink;
	bool test_retrain;
	unsigned int cleardown_initiators;
	bool raw_transport;
	bool require_lapm;
	bool require_v44;
	bool digital_caller;
	bool automatic_pcm_roles;
	bool startup_robbed_bits;
	bool conventional_upstream;
	bool factory_transport;
	enum integration_impairment impairment;
};

extern int process_at_command(struct modem *modem, char *command);

static struct modem *create_compat_modem(struct modem_driver *driver, const char *name)
{
	struct modem *modem = modem_create(driver, name);
	modem_frontend_init(modem);
	return modem;
}

static int start_modem(struct modem *modem)
{
	(void)modem;
	return 0;
}

static int stop_modem(struct modem *modem)
{
	struct integration_endpoint *endpoint = modem->dev_data;

	if (endpoint != NULL)
		endpoint->stops++;
	return 0;
}

static int control_modem(
	struct modem *modem, unsigned int command, unsigned long argument)
{
	(void)modem;
	(void)command;
	(void)argument;
	return 0;
}

static struct modem_driver integration_driver = {
	.name = "SIP modem media test",
	.start = start_modem,
	.stop = stop_modem,
	.ioctl = control_modem,
};

static bool endpoint_init(
	struct integration_endpoint *endpoint, const char *name)
{
	int sockets[2];
	int flags;

	memset(endpoint, 0, sizeof(*endpoint));
	endpoint->profile_cpu = getenv("MODEM_CPU_BENCH") != NULL;
	endpoint->modem_fd = -1;
	endpoint->client_fd = -1;
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
		return false;
	flags = fcntl(sockets[1], F_GETFL);
	if (flags < 0 || fcntl(sockets[1], F_SETFL, flags | O_NONBLOCK) != 0) {
		close(sockets[1]);
		close(sockets[0]);
		return false;
	}
	endpoint->modem = create_compat_modem(&integration_driver, name);
	if (endpoint->modem == NULL) {
		close(sockets[1]);
		close(sockets[0]);
		return false;
	}
	endpoint->modem_fd = sockets[0];
	endpoint->client_fd = sockets[1];
	endpoint->modem->pty = endpoint->modem_fd;
	endpoint->modem->dev_data = endpoint;
	/* This harness joins exact G.711 codecs without an analogue conversion. */
	endpoint->modem->pcm_codec_grid = true;
	return true;
}

static void endpoint_destroy(struct integration_endpoint *endpoint)
{
	if (endpoint == NULL)
		return;
	channel_bench_destroy(&endpoint->receive_bench);
	modem_delete(endpoint->modem);
	if (endpoint->client_fd >= 0)
		close(endpoint->client_fd);
	if (endpoint->modem_fd >= 0)
		close(endpoint->modem_fd);
	memset(endpoint, 0, sizeof(*endpoint));
	endpoint->modem_fd = -1;
	endpoint->client_fd = -1;
}

static bool endpoint_drain(struct integration_endpoint *endpoint)
{
	for (;;) {
		ssize_t count;
		size_t available = sizeof(endpoint->output) - endpoint->output_count;

		if (available == 0)
			return false;
		count = read(endpoint->client_fd,
			     endpoint->output + endpoint->output_count, available);
		if (count > 0) {
			endpoint->output_count += (size_t)count;
			continue;
		}
		if (count == 0)
			return true;
		if (errno == EINTR)
			continue;
		return errno == EAGAIN || errno == EWOULDBLOCK;
	}
}

static bool output_contains(
	const struct integration_endpoint *endpoint, const char *text)
{
	size_t text_length = strlen(text);

	if (text_length > endpoint->output_count)
		return false;
	for (size_t offset = 0;
	     offset <= endpoint->output_count - text_length; ++offset)
		if (memcmp(endpoint->output + offset, text, text_length) == 0)
			return true;
	return false;
}

/* Online recordings have no terminal consumer. Search each bounded read and
 * retain only a possible partial match, including when the next read fills
 * the buffer. Raw data may contain NUL bytes. */
static bool recording_replay_drain_online(
	struct integration_endpoint *endpoint, const char *expected_text,
	bool *expected_text_seen)
{
	size_t length = expected_text != NULL ? strlen(expected_text) : 0;

	if (length >= sizeof(endpoint->output))
		return false;
	for (;;) {
		size_t retained = 0;
		ssize_t count;

		if (length != 0 && !*expected_text_seen) {
			*expected_text_seen = output_contains(endpoint, expected_text);
			if (!*expected_text_seen) {
				retained = length - 1u;
				if (retained > endpoint->output_count)
					retained = endpoint->output_count;
			}
		}
		memmove(endpoint->output,
			endpoint->output + endpoint->output_count - retained,
			retained);
		endpoint->output_count = retained;
		count = read(endpoint->client_fd,
			     endpoint->output + retained,
			     sizeof(endpoint->output) - retained);
		if (count > 0) {
			endpoint->output_count += (size_t)count;
			continue;
		}
		if (count == 0)
			return true;
		if (errno == EINTR)
			continue;
		return errno == EAGAIN || errno == EWOULDBLOCK;
	}
}

static bool run_recording_replay_output(void)
{
	struct integration_endpoint endpoint = { 0 };
	char long_text[ENDPOINT_OUTPUT_CAPACITY];
	const char *texts[] = { NULL, "", "nonce", long_text };
	uint8_t noise[ENDPOINT_OUTPUT_CAPACITY * 3u] = { 0 };
	int sockets[2];
	int flags;
	bool seen = false;

	memset(long_text, 'x', sizeof(long_text) - 2u);
	long_text[sizeof(long_text) - 2u] = 'y';
	long_text[sizeof(long_text) - 1u] = '\0';
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
	      "recording replay output sockets failed");
	endpoint.client_fd = sockets[1];
	flags = fcntl(endpoint.client_fd, F_GETFL);
	CHECK(flags >= 0 &&
		      fcntl(endpoint.client_fd, F_SETFL, flags | O_NONBLOCK) == 0,
	      "recording replay output socket is not nonblocking");
	for (size_t index = 0; index < ARRAY_LENGTH(texts); ++index) {
		const char *text = texts[index];
		size_t length = text != NULL ? strlen(text) : 0;

		endpoint.output_count = 0;
		seen = false;
		CHECK(write(sockets[0], noise, sizeof(noise)) == sizeof(noise) &&
			      recording_replay_drain_online(&endpoint, text, &seen) &&
			      !seen && endpoint.output_count ==
				      (length != 0 ? length - 1u : 0),
		      "recording replay did not bound unmatched binary output");
		if (length != 0) {
			size_t prefix = length / 2u;

			CHECK(write(sockets[0], text, prefix) == (ssize_t)prefix &&
				      recording_replay_drain_online(&endpoint, text, &seen) &&
				      !seen &&
				      write(sockets[0], text + prefix, length - prefix) ==
					      (ssize_t)(length - prefix) &&
				      write(sockets[0], noise, sizeof(noise)) == sizeof(noise) &&
				      recording_replay_drain_online(&endpoint, text, &seen) &&
				      seen && endpoint.output_count == 0,
			      "recording replay lost a split substring or retained consumed data");
		}
	}
	close(sockets[0]);
	CHECK(recording_replay_drain_online(&endpoint, "nonce", &seen),
	      "recording replay rejected terminal EOF");
	close(sockets[1]);
	CHECK(!recording_replay_drain_online(&endpoint, "nonce", &seen),
	      "recording replay hid a terminal read error");
	return true;
}

static bool output_contains_bytes(
	const struct integration_endpoint *endpoint,
	const uint8_t *bytes, size_t byte_count)
{
	if (endpoint == NULL || bytes == NULL || byte_count == 0 ||
	    byte_count > endpoint->output_count)
		return false;
	for (size_t offset = 0;
	     offset <= endpoint->output_count - byte_count; ++offset)
		if (memcmp(endpoint->output + offset, bytes, byte_count) == 0)
			return true;
	return false;
}

static bool output_contains_connect(
	const struct integration_endpoint *endpoint, unsigned int bit_rate)
{
	char text[32];
	int count = snprintf(text, sizeof(text), "CONNECT %u", bit_rate);

	return count > 0 && (size_t)count < sizeof(text) &&
	       output_contains(endpoint, text);
}

static void capture_recording(
	void *context, const struct recording_block *block)
{
	struct recording_capture *capture = context;

	if (capture == NULL || block == NULL || block->data == NULL ||
	    block->count == 0 ||
	    block->stream < RECORD_RX_RTP_PAYLOAD_ARRIVAL ||
	    block->stream > RECORD_TX_RTP_PAYLOAD_SENT) {
		if (capture != NULL)
			capture->invalid = true;
		return;
	}
	capture->blocks[block->stream]++;
	capture->units[block->stream] += block->count;
}

static bool codec_law(enum media_codec codec, enum g711_law *law)
{
	if (law == NULL)
		return false;
	if (codec == MEDIA_CODEC_PCMU)
		*law = G711_LAW_PCMU;
	else if (codec == MEDIA_CODEC_PCMA)
		*law = G711_LAW_PCMA;
	else
		return false;
	return true;
}

static int16_t decode_codeword(enum g711_law law, uint8_t codeword)
{
	return law == G711_LAW_PCMA ?
		       g711_decode_pcma(codeword) :
		       g711_decode_pcmu(codeword);
}

static uint8_t encode_codeword(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMA ?
		       g711_encode_pcma(sample) :
		       g711_encode_pcmu(sample);
}

static bool endpoint_prepare_media(
	struct integration_endpoint *endpoint, enum media_codec codec)
{
	enum g711_law law;
	int silence;

	if (endpoint == NULL || !codec_law(codec, &law))
		return false;
	silence = media_codec_silence_octet(codec);
	if (silence < 0 || silence > UINT8_MAX)
		return false;
	for (size_t index = 0; index < SIP_FRAME_SAMPLES; ++index) {
		endpoint->receive_codewords[index] = (uint8_t)silence;
		endpoint->receive[index] =
			decode_codeword(law, (uint8_t)silence);
	}
	return true;
}

static uint32_t noise_state_next(uint32_t *state)
{
	*state ^= *state << 13;
	*state ^= *state >> 17;
	*state ^= *state << 5;
	return *state;
}

static bool endpoint_replace_precarrier_silence(
	struct integration_endpoint *endpoint, enum media_codec codec,
	uint32_t *state, bool *carrier_seen)
{
	enum g711_law law;
	int silence;

	if (endpoint == NULL || state == NULL || *state == 0 ||
	    carrier_seen == NULL || !codec_law(codec, &law) ||
	    (silence = media_codec_silence_octet(codec)) < 0 ||
	    silence > UINT8_MAX)
		return false;
	if (*carrier_seen)
		return true;
	for (size_t index = 0; index < SIP_FRAME_SAMPLES; ++index) {
		if (endpoint->receive_codewords[index] == (uint8_t)silence)
			continue;
		*carrier_seen = true;
		return true;
	}
	for (size_t index = 0; index < SIP_FRAME_SAMPLES; ++index) {
		uint8_t codeword;

		uint32_t random = noise_state_next(state);

		/* Exercise the generic QAM floor while remaining far below the
		 * V.22 family's carrier threshold after either codec. */
		codeword = encode_codeword(
			law, (int16_t)(((int32_t)(random & 31u) - 16) * 8));
		endpoint->receive_codewords[index] = codeword;
		endpoint->receive[index] = decode_codeword(law, codeword);
	}
	return true;
}

static bool process_transmit_frame(
	struct integration_endpoint *endpoint, enum media_codec codec,
	const struct media_recorder *recorder)
{
	enum g711_law law;
	bool exact_output = false;
	bool received_mask[SIP_FRAME_SAMPLES];
	struct timespec started, finished;
	int silence;

	if (endpoint == NULL || endpoint->modem == NULL ||
	    !codec_law(codec, &law))
		return false;
	silence = media_codec_silence_octet(codec);
	if (silence < 0 || silence > UINT8_MAX)
		return false;
	memset(endpoint->transmit, 0, sizeof(endpoint->transmit));
	memset(endpoint->transmit_codewords, silence,
	       sizeof(endpoint->transmit_codewords));
	if (endpoint->delay_one_sample) {
		for (unsigned sample = 0; sample < SIP_FRAME_SAMPLES; ++sample) {
			uint8_t current = endpoint->receive_codewords[sample];
			endpoint->receive_codewords[sample] = endpoint->prior_codeword;
			endpoint->receive[sample] = decode_codeword(law, endpoint->prior_codeword);
			endpoint->prior_codeword = current;
		}
	}
	CHECK(endpoint->erased_samples <= SIP_FRAME_SAMPLES, "invalid media erasure fixture");
	for (size_t index = 0; index < SIP_FRAME_SAMPLES; ++index) {
		received_mask[index] = index >= endpoint->erased_samples;
		if (!received_mask[index]) {
			endpoint->receive_codewords[index] = (uint8_t)silence;
			endpoint->receive[index] = decode_codeword(law, (uint8_t)silence);
		}
	}
	if (endpoint->profile_cpu && clock_gettime(CLOCK_THREAD_CPUTIME_ID, &started) != 0)
		return false;
	modem_process_g711(
		endpoint->modem, law, endpoint->receive_codewords,
		endpoint->receive, received_mask, endpoint->transmit_codewords,
		endpoint->transmit, SIP_FRAME_SAMPLES, &exact_output);
	if (endpoint->profile_cpu) {
		if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &finished) != 0)
			return false;
		endpoint->process_ns += (uint64_t)(
			(int64_t)(finished.tv_sec - started.tv_sec) * INT64_C(1000000000) +
			finished.tv_nsec - started.tv_nsec);
		endpoint->process_samples += SIP_FRAME_SAMPLES;
	}
	endpoint->erased_samples = 0;
	if (!exact_output)
		return media_encode(
			       codec, endpoint->transmit, SIP_FRAME_SAMPLES,
			       endpoint->transmit_codewords,
			       ARRAY_LENGTH(endpoint->transmit_codewords),
			       recorder) == MEDIA_OK;
	for (size_t index = 0; index < SIP_FRAME_SAMPLES; ++index)
		CHECK(endpoint->transmit[index] == decode_codeword(law, endpoint->transmit_codewords[index]),
		      "exact media mismatch law=%u sample=%zu linear=%d octet=%u decoded=%d",
		      law, index, endpoint->transmit[index], endpoint->transmit_codewords[index],
		      decode_codeword(law, endpoint->transmit_codewords[index]));
	endpoint->exact_frames++;
	return recording_emit(
		       recorder, RECORD_TX_LINE_PCM, RECORD_S16_NATIVE,
		       MEDIA_SAMPLE_RATE, endpoint->transmit,
		       SIP_FRAME_SAMPLES) &&
	       recording_emit(
		       recorder, RECORD_TX_RTP_PAYLOAD_PREPARED,
		       RECORD_OCTETS, MEDIA_SAMPLE_RATE,
		       endpoint->transmit_codewords, SIP_FRAME_SAMPLES);
}

static bool deliver_media_frame(
	const struct integration_endpoint *source,
	struct integration_endpoint *destination,
	enum media_codec codec,
	const struct media_recorder *recorder)
{
	enum g711_law law;

	memcpy(destination->receive_codewords, source->transmit_codewords,
	       sizeof(destination->receive_codewords));
	if (destination->delay_codewords) {
		for (size_t index = 0; index < SIP_FRAME_SAMPLES; ++index) {
			uint8_t delayed = destination->delayed_codewords[
				destination->delayed_codeword_cursor];

			destination->delayed_codewords[
				destination->delayed_codeword_cursor] =
				destination->receive_codewords[index];
			destination->receive_codewords[index] = delayed;
			destination->delayed_codeword_cursor =
				(destination->delayed_codeword_cursor + 1u) %
				DIGITAL_MEDIA_DELAY_SAMPLES;
		}
	}
	if (destination->receive_bench.effect_count != 0) {
		if (!codec_law(codec, &law))
			return false;
		for (size_t index = 0; index < SIP_FRAME_SAMPLES; ++index)
			destination->receive[index] = decode_codeword(
				law, destination->receive_codewords[index]);
		if (channel_bench_process(
			    &destination->receive_bench, destination->receive,
			    SIP_FRAME_SAMPLES) != CHANNEL_BENCH_OK)
			return false;
		for (size_t index = 0; index < SIP_FRAME_SAMPLES; ++index)
			destination->receive_codewords[index] =
				encode_codeword(law, destination->receive[index]);
	}
	if (destination->robbed_bits)
		for (unsigned i = 0; i < SIP_FRAME_SAMPLES; ++i) {
			destination->receive_codewords[i] = (destination->receive_codewords[i] & ~1u) |
				((destination->robbed_position / 6) & 1u);
			destination->robbed_position = (destination->robbed_position + 1u) % 24;
		}
	return media_decode(
		       codec, destination->receive_codewords,
		       SIP_FRAME_SAMPLES, destination->receive,
		       SIP_FRAME_SAMPLES, recorder) == MEDIA_OK;
}

static bool endpoint_add_impairment(
	struct integration_endpoint *endpoint,
	enum integration_impairment impairment, enum media_codec codec,
	uint32_t seed)
{
	struct channel_bench_effect_spec composite[] = {
		{
			.kind = CHANNEL_BENCH_GAIN,
			.parameters.gain.scale_q15 = 32000,
		},
		{
			.kind = CHANNEL_BENCH_DELAY,
			.parameters.delay.samples = 2,
		},
		{
			.kind = CHANNEL_BENCH_FRACTIONAL_DELAY,
			.parameters.fractional_delay.phase_q16 =
				FRACTIONAL_DELAY_HALF_SAMPLE_Q16,
		},
		{
			.kind = CHANNEL_BENCH_ECHO,
			.parameters.echo = {
				.delay_samples = 8,
				/* Representative delayed echo at roughly -34 dB. */
				.gain_q15 = 640,
			},
		},
		{
			.kind = CHANNEL_BENCH_NOISE,
			.parameters.noise = {
				.peak = 4,
				.seed = seed,
			},
		},
	};

	if (endpoint == NULL)
		return false;
	switch (impairment) {
	case INTEGRATION_IMPAIRMENT_NONE:
		return true;
	case INTEGRATION_IMPAIRMENT_COMPOSITE:
		return channel_bench_init(
			       &endpoint->receive_bench, composite,
			       ARRAY_LENGTH(composite)) == CHANNEL_BENCH_OK;
	case INTEGRATION_IMPAIRMENT_TRAINING_NOISE: {
		const struct channel_bench_effect_spec noise = {
			.kind = CHANNEL_BENCH_NOISE,
			.parameters.noise = { .peak = 800, .seed = seed },
		};

		return channel_bench_init(
			       &endpoint->receive_bench, &noise, 1) ==
		       CHANNEL_BENCH_OK;
	}
	case INTEGRATION_IMPAIRMENT_DIGITAL_DELAY: {
		int silence = media_codec_silence_octet(codec);

		if (silence < 0 || silence > UINT8_MAX)
			return false;
		memset(endpoint->delayed_codewords, (uint8_t)silence,
		       sizeof(endpoint->delayed_codewords));
		endpoint->delay_codewords = true;
		return true;
	}
	default:
		return false;
	}
}

static bool process_sip_frame(
	struct integration_endpoint *caller,
	struct integration_endpoint *answerer,
	enum media_codec codec,
	const struct media_recorder *recorder)
{
	return process_transmit_frame(caller, codec, recorder) &&
	       process_transmit_frame(answerer, codec, recorder) &&
	       deliver_media_frame(caller, answerer, codec, recorder) &&
	       deliver_media_frame(answerer, caller, codec, recorder);
}

static bool process_sip_frame_with_caller_delay(
	struct integration_endpoint *caller,
	struct integration_endpoint *answerer,
	enum media_codec codec, const struct media_recorder *recorder,
	struct media_frame_delay *delay, size_t delay_frames)
{
	size_t slot;

	if (delay == NULL || delay_frames == 0 ||
	    delay_frames > ARRAY_LENGTH(delay->codewords) ||
	    !process_transmit_frame(caller, codec, recorder) ||
	    !process_transmit_frame(answerer, codec, recorder))
		return false;
	if (delay->count < delay_frames) {
		slot = (delay->head + delay->count) % delay_frames;
		delay->count++;
		if (!endpoint_prepare_media(answerer, codec))
			return false;
	} else {
		slot = delay->head;
		memcpy(answerer->receive_codewords, delay->codewords[slot],
		       sizeof(answerer->receive_codewords));
		if (media_decode(
			    codec, answerer->receive_codewords,
			    SIP_FRAME_SAMPLES, answerer->receive,
			    SIP_FRAME_SAMPLES, recorder) != MEDIA_OK)
			return false;
		delay->head = (delay->head + 1u) % delay_frames;
	}
	memcpy(delay->codewords[slot], caller->transmit_codewords,
	       sizeof(delay->codewords[slot]));
	return deliver_media_frame(answerer, caller, codec, recorder);
}

static bool process_test_frame(
	struct integration_endpoint *caller,
	struct integration_endpoint *answerer,
	enum media_codec codec, const struct media_recorder *recorder,
	struct media_frame_delay *delay, size_t delay_frames)
{
	return delay_frames != 0 ?
		process_sip_frame_with_caller_delay(
			caller, answerer, codec, recorder, delay, delay_frames) :
		process_sip_frame(caller, answerer, codec, recorder);
}

static bool exercise_transmit_completion(struct integration_endpoint *caller,
	struct integration_endpoint *answerer, enum media_codec codec,
	const struct media_recorder *recorder, struct media_frame_delay *delay,
	size_t delay_frames)
{
	struct integration_endpoint *ends[] = { caller, answerer };
	uint8_t payload[2][128];
	int result[2] = { 1, 1 };

	for (unsigned side = 0; side < 2; ++side) {
		CHECK(endpoint_drain(ends[side]), "final message setup");
		ends[side]->output_count = 0;
		for (size_t i = 0; i < sizeof(payload[side]); ++i)
			payload[side][i] = (uint8_t)(i * 31u + 17u * side);
		CHECK(modem_data_write(ends[side]->modem, payload[side], sizeof(payload[side])) ==
		      (int)sizeof(payload[side]) && modem_finish_transmit(ends[side]->modem) == 1 &&
		      modem_data_write(ends[side]->modem, "extra", 5) == 0,
		      "final message was not queued and sealed");
	}
	for (unsigned frame = 0; frame < 30u * MODEM_RATE / SIP_FRAME_SAMPLES; ++frame) {
		CHECK(process_test_frame(caller, answerer, codec, recorder, delay, delay_frames) &&
		      endpoint_drain(caller) && endpoint_drain(answerer), "final message media");
		for (unsigned side = 0; side < 2; ++side) {
			result[side] = modem_finish_transmit(ends[side]->modem);
			CHECK(result[side] >= 0, "transmit completion failed on side %u", side);
		}
		if (result[0] == 0 && result[1] == 0 &&
		    caller->output_count == sizeof(payload[0]) &&
		    answerer->output_count == sizeof(payload[0]))
			break;
	}
	for (unsigned side = 0; side < 2; ++side)
		CHECK(result[side] == 0 && ends[side]->output_count == sizeof(payload[side]) &&
		      !memcmp(ends[side]->output, payload[1u - side], sizeof(payload[side])) &&
		      modem_finish_transmit(ends[side]->modem) == 0,
		      "final 128-byte message did not complete exactly, side=%u bytes=%zu",
		      side, ends[side]->output_count);
	return true;
}

static bool v22_rate_monitor_init(
	struct v22_rate_monitor *monitor, size_t minimum_sample)
{
	static const struct qam_channel channel = {
		.sample_rate = MODEM_RATE,
		.symbol_rate_numerator = 600,
		.symbol_rate_denominator = 1,
		.carrier_frequency_numerator = 1200,
		.carrier_frequency_denominator = 1,
		.rolloff = 0.75,
		.pulse_span_symbols = 6,
	};

	if (monitor == NULL)
		return false;
	memset(monitor, 0, sizeof(*monitor));
	monitor->minimum_sample = minimum_sample;
	v22bis_rate_signal_detector_reset(&monitor->detector);
	return qam_demodulator_init(&monitor->demodulator, &channel) == QAM_OK;
}

static bool v22_rate_monitor_observe(
	struct v22_rate_monitor *monitor,
	const int16_t samples[SIP_FRAME_SAMPLES])
{
	for (size_t index = 0; index < SIP_FRAME_SAMPLES; ++index) {
		struct qam_point point;
		size_t sample_offset = monitor->samples++;
		bool ready;

		if (qam_demodulator_sample(
			    &monitor->demodulator, samples[index], &point,
			    &ready) != QAM_OK)
			return false;
		if (!ready)
			continue;
		if (monitor->has_previous) {
			enum v22bis_rate_signal_event event;
			uint8_t turn;

			if (qam_nearest_quarter_turn(
				    &monitor->previous, &point, &turn) != QAM_OK ||
			    v22bis_rate_signal_observe(
				    &monitor->detector, turn, &event) !=
				    V22_FRAMING_OK)
				return false;
			if (event == V22BIS_RATE_SIGNAL_FULLY_ENDED &&
			    sample_offset >= monitor->minimum_sample)
				monitor->starts++;
		}
		monitor->previous = point;
		monitor->has_previous = true;
	}
	return true;
}

static bool process_sip_frame_with_loss(
	struct integration_endpoint *caller,
	struct integration_endpoint *answerer,
	enum media_codec codec, const struct media_recorder *recorder,
	bool block_caller_to_answerer, bool block_answerer_to_caller,
	bool reduce_caller_to_answerer, bool reduce_answerer_to_caller)
{
	if (!process_transmit_frame(caller, codec, recorder) ||
	    !process_transmit_frame(answerer, codec, recorder))
		return false;
	if (reduce_caller_to_answerer)
		for (size_t sample = 0; sample < ARRAY_LENGTH(caller->transmit);
		     ++sample)
			caller->transmit[sample] =
				(int16_t)(caller->transmit[sample] * 4 / 5);
	if (reduce_answerer_to_caller)
		for (size_t sample = 0; sample < ARRAY_LENGTH(answerer->transmit);
		     ++sample)
			answerer->transmit[sample] =
				(int16_t)(answerer->transmit[sample] * 4 / 5);
	return (block_caller_to_answerer ?
			endpoint_prepare_media(answerer, codec) :
			deliver_media_frame(
				caller, answerer, codec, recorder)) &&
	       (block_answerer_to_caller ?
			endpoint_prepare_media(caller, codec) :
			deliver_media_frame(
				answerer, caller, codec, recorder));
}

static bool modem_is_online(const struct modem *modem, enum DP_ID id)
{
	return modem != NULL && modem->started && modem->dp != NULL &&
	       modem->dp->id == id && modem->dp->status == DPSTAT_CONNECT &&
	       modem->get_bits != NULL && modem->put_bits != NULL;
}

static bool exercise_carrier_loss_limit(
	struct integration_endpoint *endpoint,
	enum DP_ID id,
	unsigned int carrier_loss_samples)
{
	static const unsigned int fragments[] = { 37, 29, 1, 23 };
	int16_t silence[MODEM_FRAG] = { 0 };
	int16_t transmit[MODEM_FRAG];
	unsigned int presented = 0;
	unsigned int fragment = 0;

	if (endpoint == NULL || endpoint->modem == NULL ||
	    carrier_loss_samples < 2)
		return false;
	if (id != DP_B103 && id != DP_V21 && id != DP_B212 &&
	    id != DP_V22 && id != DP_V23) {
		for (unsigned int frame = 0;
		     frame * SIP_FRAME_SAMPLES <= carrier_loss_samples;
		     ++frame) {
			/* Carrier loss removes AC energy, but not the learned channel
			 * offset presented to the modem receive front end. */
			for (size_t index = 0;
			     index < ARRAY_LENGTH(endpoint->receive); ++index)
				endpoint->receive[index] =
					endpoint->modem->receive_dc.level;
			modem_process(
				endpoint->modem, endpoint->receive,
				endpoint->transmit, SIP_FRAME_SAMPLES);
		}
		return endpoint->modem->process == NULL;
	}
	while (presented < carrier_loss_samples - 1u) {
		unsigned int remaining = carrier_loss_samples - 1u - presented;
		unsigned int count = fragments[fragment++ % ARRAY_LENGTH(fragments)];

		if (count > remaining)
			count = remaining;
		for (unsigned int index = 0; index < count; ++index)
			silence[index] = endpoint->modem->receive_dc.level;
		modem_process(endpoint->modem, silence, transmit, (int)count);
		if (endpoint->modem->process == NULL)
			return false;
		presented += count;
	}
	/* The QAM modes see silence immediately, so their S10 boundary is exact.
	 * FSK first drains its bounded tone-detector window; nevertheless it must
	 * disconnect within one maximum detector window after S10. */
	for (unsigned int sample = 0;
	     sample < (id == DP_V22 || id == DP_B212 ? 1u :
						      FSK_MAX_DETECTOR_WINDOW);
	     ++sample) {
		silence[0] = endpoint->modem->receive_dc.level;
		modem_process(endpoint->modem, silence, transmit, 1);
		if (endpoint->modem->process == NULL)
			return true;
	}
	return false;
}

static bool exercise_retrain(
	struct integration_endpoint *caller,
	struct integration_endpoint *answerer,
	struct integration_endpoint *initiator,
	enum DP_ID id, unsigned int bit_rate,
	enum media_codec codec,
	const struct media_recorder *recorder,
	struct media_frame_delay *delay, size_t delay_frames,
	unsigned int sequence, bool raw_transport, bool lose_first_response,
	bool reduce_peer_response, int peer_request_frame)
{
	enum {
		TEST_STATE_COMMAND_ONLINE = 6,
		PRESERVED_CAPACITY = 128,
		ATO1_100MS_FRAMES =
			(MODEM_RATE / 10) / SIP_FRAME_SAMPLES,
	};
	static const uint8_t caller_payloads[][8] = {
		{ 'c', '+', '+', '+', '-', 'r', '1', 0x81 },
		{ 'c', '+', '+', '+', '-', 'r', '2', 0x82 },
	};
	static const uint8_t answerer_payloads[][8] = {
		{ 'a', '+', '+', '+', '-', 'r', '1', 0xfe },
		{ 'a', '+', '+', '+', '-', 'r', '2', 0xff },
	};
	/* Retained slow-link calls carried a literal +++ when the peer failed to
	 * leave data mode.  It remains ordinary online payload here; only the
	 * endpoint issuing ATO1 changes its own host-side state. */
	struct integration_endpoint *peer;
	uint8_t caller_preserved[PRESERVED_CAPACITY] = {
		0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
	};
	uint8_t answerer_preserved[PRESERVED_CAPACITY] = {
		0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8,
	};
	bool completed = false;
	bool transferred = false;
	bool caller_preserved_delivered;
	bool answerer_preserved_delivered;
	bool peer_requested = peer_request_frame >= 0;
	bool initiator_retrains;
	bool peer_retrains;
	char command[] = "ATO1";
	char peer_command[] = "ATO1";
	size_t caller_preserved_offset;
	size_t answerer_preserved_offset;
	size_t pre_retrain_length;
	size_t preserved_length;
	unsigned int transmit_rate;
	uint32_t pending_transmit_data = 0;
	unsigned int completion_frames = 0;
	int pending_transmit_bits = 0;
	int pending_transmit_chars = 0;
	bool interrupt_exchange = id == DP_V92 && !raw_transport;
	bool check_v32_resume = (id == DP_V32 || id == DP_V32BIS) && !raw_transport;
	bool resume_poll_seen = false;
	unsigned retrain_quiet = 0, longest_retrain_quiet = 0;

	if (caller == NULL || answerer == NULL || initiator == NULL ||
	    sequence >= ARRAY_LENGTH(caller_payloads))
		return false;
	peer = initiator == caller ? answerer : caller;
	if (initiator->modem->dp == NULL ||
	    initiator->modem->dp->op == NULL || peer->modem->dp == NULL ||
	    peer->modem->dp->op == NULL)
		return false;
	initiator_retrains = initiator->modem->dp->op->retrain != NULL;
	peer_retrains = peer->modem->dp->op->retrain != NULL;
	caller->output_count = 0;
	answerer->output_count = 0;
	transmit_rate = caller->modem->tx_rate > answerer->modem->tx_rate ?
		caller->modem->tx_rate : answerer->modem->tx_rate;
	pre_retrain_length = raw_transport ? transmit_rate / 50u / 10u : 0;
	/* Leave a clean character queued behind any asynchronous character or
	 * mapping-frame input already owned by the datapump. */
	preserved_length = raw_transport ? pre_retrain_length + 8u : 5u;
	if (preserved_length > PRESERVED_CAPACITY ||
	    pre_retrain_length + 1u >= preserved_length) {
		fprintf(stderr, "invalid retrain preservation fixture\n");
		return false;
	}
	if (raw_transport) {
		/* The interrupted character is deliberately unrecoverable at the
		 * receiver.  Its all-mark tail lets the next queued character regain
		 * a clean asynchronous framing boundary. */
		caller_preserved[preserved_length - 2u] = 0xff;
		answerer_preserved[preserved_length - 2u] = 0xff;
		caller_preserved[preserved_length - 1u] = 0x9f;
		answerer_preserved[preserved_length - 1u] = 0xaf;
	}
	if (modem_write(
		    caller->modem, (const char *)caller_preserved,
		    (int)preserved_length) != (int)preserved_length ||
	    modem_write(
		    answerer->modem, (const char *)answerer_preserved,
		    (int)preserved_length) != (int)preserved_length ||
	    !process_test_frame(
		    caller, answerer, codec, recorder, delay, delay_frames) ||
	    !endpoint_drain(caller) || !endpoint_drain(answerer)) {
		fprintf(stderr,
			"buffered retrain setup failed (outputs=%zu/%zu)\n",
			caller->output_count, answerer->output_count);
		return false;
	}
	caller->output_count = 0;
	answerer->output_count = 0;
	if (raw_transport) {
		pending_transmit_bits = initiator->modem->pack.bit;
		pending_transmit_data = initiator->modem->pack.data;
		pending_transmit_chars = initiator->modem->xmit.count;
		/* Some mapped rates consume an integral number of async frames in
		 * one media interval.  A partial packer word is useful coverage when
		 * present, but the queued clean suffix is the required ownership
		 * boundary for every rate. */
		if (pending_transmit_chars <= 0) {
			fprintf(stderr,
				"raw retrain fixture did not retain transmit data "
				"(bits=%d chars=%d)\n",
				pending_transmit_bits, pending_transmit_chars);
			return false;
		}
	}
	/* LAPM recovery must interrupt a real outstanding information frame,
	 * not merely queue terminal bytes behind an idle link or resume without
	 * retraining.  Exact delivery of this payload is checked after recovery. */
	if ((interrupt_exchange || check_v32_resume) &&
	    (!initiator_retrains || !peer_retrains ||
	     initiator->modem->ec.lapm.va == initiator->modem->ec.lapm.vs)) {
		fprintf(stderr,
			"retrain fixture did not interrupt pending LAPM data\n");
		return false;
	}
	/* ATO1 must request a full retrain even while a shorter exchange is
	 * already active. The two directions exercise RR and FPE respectively.
	 * PCMA lets the peer start the exchange and clocks its request across
	 * the media boundary; PCMU interrupts a local request immediately. */
	if (interrupt_exchange) {
		struct dp *exchange = codec == MEDIA_CODEC_PCMA ?
			peer->modem->dp : initiator->modem->dp;
		if (!(sequence == 0 ? v90_datapump_start_rate_renegotiation(exchange, false) :
		      v90_datapump_start_fast_exchange(exchange))) {
			fprintf(stderr, "pre-ATO1 parameter exchange did not start\n");
			return false;
		}
		if (codec == MEDIA_CODEC_PCMA)
			for (unsigned frame = 0; frame < 4; ++frame)
				if (!process_sip_frame(caller, answerer, codec, recorder) ||
				    !endpoint_drain(caller) || !endpoint_drain(answerer))
					return false;
	}
	initiator->modem->state = TEST_STATE_COMMAND_ONLINE;
	initiator->modem->command = 1;
	size_t initiator_output_before_request = initiator->output_count;
	size_t peer_output_before_request = peer->output_count;
	if (peer_request_frame == 0) {
		peer->modem->state = TEST_STATE_COMMAND_ONLINE;
		peer->modem->command = 1;
	}
	if (process_at_command(initiator->modem, command) != 1 ||
	    (initiator->modem->retrain_pending != 0) != initiator_retrains ||
	    (peer_request_frame == 0 &&
	     (process_at_command(peer->modem, peer_command) != 1 ||
	      (peer->modem->retrain_pending != 0) != peer_retrains)) ||
	    !endpoint_drain(caller) || !endpoint_drain(answerer) ||
	    (initiator_retrains && initiator->output_count != initiator_output_before_request) ||
	    (!initiator_retrains &&
	     (!output_contains(initiator, "WARNING: Retrain unavailable") ||
	      !output_contains_connect(initiator, bit_rate))) ||
	    (peer_request_frame == 0 && peer_retrains &&
	     peer->output_count != peer_output_before_request) ||
	    (peer_request_frame == 0 && !peer_retrains &&
	     (!output_contains(peer, "WARNING: Retrain unavailable") ||
	      !output_contains_connect(peer, bit_rate)))) {
		fprintf(stderr,
			"return-online request failed "
			"(retrain=%d/%d pending=%u outputs=%zu/%zu)\n",
			initiator_retrains, peer_retrains,
			initiator->modem->retrain_pending,
			caller->output_count, answerer->output_count);
		return false;
	}
	if (raw_transport &&
	    (initiator->modem->pack.bit != pending_transmit_bits ||
	     initiator->modem->pack.data != pending_transmit_data ||
	     initiator->modem->xmit.count != pending_transmit_chars)) {
		fprintf(stderr,
			"ATO1 changed buffered raw transmit state "
			"(bits=%d/%d chars=%d/%d)\n",
			initiator->modem->pack.bit, pending_transmit_bits,
			initiator->modem->xmit.count, pending_transmit_chars);
		return false;
	}
	for (unsigned int frame = 0;
	     frame < (v8_datapump_modulation(id) == V8_MODULATION_V32 ?
			      V32_RETRAIN_FRAME_LIMIT : v8_datapump_uses_pcm(id) ?
			      V92_RETRAIN_FRAME_LIMIT : RETRAIN_FRAME_LIMIT);
	     ++frame) {
		bool losing_response = lose_first_response &&
			frame < RETRAIN_RESPONSE_LOSS_FRAMES;
		bool block_caller_to_answerer = losing_response &&
			initiator == answerer;
		bool block_answerer_to_caller = losing_response &&
			initiator == caller;
		bool reduce_caller_to_answerer = reduce_peer_response &&
			initiator == answerer;
		bool reduce_answerer_to_caller = reduce_peer_response &&
			initiator == caller;

		if (peer_request_frame > 0 &&
		    frame == (unsigned int)peer_request_frame) {
			peer->modem->state = TEST_STATE_COMMAND_ONLINE;
			peer->modem->command = 1;
			if (process_at_command(peer->modem, peer_command) != 1 ||
			    (peer->modem->retrain_pending != 0) != peer_retrains)
				return false;
		}

		if ((delay_frames != 0 &&
		     (block_caller_to_answerer || block_answerer_to_caller ||
		      reduce_caller_to_answerer || reduce_answerer_to_caller)) ||
		    !(delay_frames != 0 ?
			      process_test_frame(
				      caller, answerer, codec, recorder,
				      delay, delay_frames) :
			      process_sip_frame_with_loss(
				      caller, answerer, codec, recorder,
				      block_caller_to_answerer,
				      block_answerer_to_caller,
				      reduce_caller_to_answerer,
				      reduce_answerer_to_caller)) ||
		    !endpoint_drain(caller) || !endpoint_drain(answerer))
			return false;
		if (interrupt_exchange) {
			enum g711_law law;
			if (!codec_law(codec, &law))
				return false;
			for (unsigned sample = 0; sample < SIP_FRAME_SAMPLES; ++sample) {
				bool quiet = abs(decode_codeword(law,
					initiator->transmit_codewords[sample])) <= 8;
				retrain_quiet = quiet ? retrain_quiet + 1u : 0u;
				if (retrain_quiet > longest_retrain_quiet)
					longest_retrain_quiet = retrain_quiet;
			}
		}
		if (id == DP_V22BIS && !peer_requested &&
		    frame < ATO1_100MS_FRAMES) {
			/* V.22bis 6.4 defines a complete 100 ms S1 before the
			 * reciprocal response.  A stable prefix is detection evidence,
			 * not the physical transition boundary. */
			if (!modem_is_online(peer->modem, id)) {
				fprintf(stderr,
					"V.22bis accepted an incomplete retrain signal "
					"at frame %u\n",
					frame);
				return false;
			}
		}
		if (!initiator_retrains) {
			/* Carrier-preserving ATO1 must not disturb the peer. */
			if (!modem_is_online(peer->modem, id)) {
				fprintf(stderr,
					"ATO1 disturbed the continuing peer link\n");
				return false;
			}
		}
		resume_poll_seen |= initiator->modem->ec.lapm.retrain_poll != 0;
		if (initiator->modem->retrain_pending == 0 &&
		    (!peer_requested || peer->modem->retrain_pending == 0) &&
		    modem_is_online(caller->modem, id) &&
		    modem_is_online(answerer->modem, id)) {
			completed = true;
			completion_frames = frame + 1u;
			break;
		}
	}
	if (check_v32_resume && !resume_poll_seen) {
		fprintf(stderr, "V.32 retrain did not resume outstanding LAPM data\n");
		return false;
	}
	if (!completed ||
	    !output_contains_connect(initiator, bit_rate) ||
	    (peer_requested && !output_contains_connect(peer, bit_rate))) {
		fprintf(stderr,
			"retrain carrier failed (completed=%d frames=%u pending=%u "
			"status=%u/%u state=%u/%u outputs=%zu/%zu)\n",
			completed, completion_frames,
			initiator->modem->retrain_pending,
			caller->modem->dp != NULL ? caller->modem->dp->status : 0,
			answerer->modem->dp != NULL ? answerer->modem->dp->status : 0,
			caller->modem->state, answerer->modem->state,
			caller->output_count, answerer->output_count);
		return false;
	}
	if (id == DP_V92 && !output_contains(initiator, "Upstream: PCM\r\n")) {
		fprintf(stderr, "V.92 retrain omitted negotiated upstream evidence\n");
		return false;
	}
	if (interrupt_exchange && longest_retrain_quiet < MODEM_RATE * 70 / 1000) {
		fprintf(stderr, "ATO1 completed a parameter exchange without full retrain\n");
		return false;
	}
	if (raw_transport) {
		caller_preserved_delivered = output_contains_bytes(
			caller, answerer_preserved + preserved_length - 1u, 1);
		answerer_preserved_delivered = output_contains_bytes(
			answerer, caller_preserved + preserved_length - 1u, 1);
		/* The peer can consume raw data before it observes S1, so only the
		 * initiator's paused transmit queue is losslessly owned here.  The
		 * second exercise reverses roles and proves the other direction. */
		caller_preserved_offset = initiator == answerer &&
			!caller_preserved_delivered ?
			preserved_length - 1u : preserved_length;
		answerer_preserved_offset = initiator == caller &&
			!answerer_preserved_delivered ?
			preserved_length - 1u : preserved_length;
	} else {
		caller_preserved_delivered = output_contains_bytes(
			caller, answerer_preserved, preserved_length);
		answerer_preserved_delivered = output_contains_bytes(
			answerer, caller_preserved, preserved_length);
		caller_preserved_offset = caller_preserved_delivered ?
			preserved_length : 0;
		answerer_preserved_offset = answerer_preserved_delivered ?
			preserved_length : 0;
	}
	caller->output_count = 0;
	answerer->output_count = 0;
	if (modem_write(
		    caller->modem,
		    (const char *)caller_payloads[sequence],
		    (int)sizeof(caller_payloads[sequence])) !=
		    (int)sizeof(caller_payloads[sequence]) ||
	    modem_write(
		    answerer->modem,
		    (const char *)answerer_payloads[sequence],
		    (int)sizeof(answerer_payloads[sequence])) !=
		    (int)sizeof(answerer_payloads[sequence]))
		return false;
	for (unsigned int frame = 0; frame < DATA_FRAME_LIMIT; ++frame) {
		if (!process_test_frame(
			    caller, answerer, codec, recorder,
			    delay, delay_frames) ||
		    !endpoint_drain(caller) || !endpoint_drain(answerer))
			return false;
		if ((raw_transport &&
		     output_contains_bytes(
			     caller, answerer_payloads[sequence],
			     sizeof(answerer_payloads[sequence])) &&
		     output_contains_bytes(
			     answerer, caller_payloads[sequence],
			     sizeof(caller_payloads[sequence]))) ||
		    (!raw_transport &&
		     caller->output_count >=
			     preserved_length - caller_preserved_offset +
				     sizeof(answerer_payloads[sequence]) &&
		     answerer->output_count >=
			     preserved_length - answerer_preserved_offset +
				     sizeof(caller_payloads[sequence]))) {
			transferred = true;
			break;
		}
	}
	if (raw_transport) {
		size_t caller_tail = preserved_length - caller_preserved_offset +
				     sizeof(answerer_payloads[sequence]);
		size_t answerer_tail = preserved_length - answerer_preserved_offset +
				       sizeof(caller_payloads[sequence]);

		/* The receiver abandons a partial asynchronous character at retrain.
		 * Any hunt bytes therefore precede the clean queued suffix. */
		if (!transferred || caller->output_count < caller_tail ||
		    answerer->output_count < answerer_tail ||
		    caller->output_count > preserved_length +
					   sizeof(answerer_payloads[sequence]) ||
		    answerer->output_count > preserved_length +
					     sizeof(caller_payloads[sequence]) ||
		    memcmp(caller->output + caller->output_count - caller_tail,
			   answerer_preserved + caller_preserved_offset,
			   preserved_length - caller_preserved_offset) != 0 ||
		    memcmp(caller->output + caller->output_count -
					   sizeof(answerer_payloads[sequence]),
			   answerer_payloads[sequence],
			   sizeof(answerer_payloads[sequence])) != 0 ||
		    memcmp(answerer->output + answerer->output_count - answerer_tail,
			   caller_preserved + answerer_preserved_offset,
			   preserved_length - answerer_preserved_offset) != 0 ||
		    memcmp(answerer->output + answerer->output_count -
					     sizeof(caller_payloads[sequence]),
			   caller_payloads[sequence],
			   sizeof(caller_payloads[sequence])) != 0) {
			fprintf(stderr,
				"raw preserved/post-retrain data failed "
				"(transferred=%d outputs=%zu/%zu tails=%zu/%zu "
				"status=%u/%u state=%u/%u)\n",
				transferred, caller->output_count,
				answerer->output_count, caller_tail, answerer_tail,
				caller->modem->dp != NULL ?
					caller->modem->dp->status : 0,
				answerer->modem->dp != NULL ?
					answerer->modem->dp->status : 0,
				caller->modem->state, answerer->modem->state);
			return false;
		}
		return true;
	}
	if (!transferred ||
	    caller->output_count != preserved_length - caller_preserved_offset +
				    sizeof(answerer_payloads[sequence]) ||
	    answerer->output_count !=
		    preserved_length - answerer_preserved_offset +
			    sizeof(caller_payloads[sequence]) ||
	    memcmp(caller->output,
		   answerer_preserved + caller_preserved_offset,
		   preserved_length - caller_preserved_offset) != 0 ||
	    memcmp(caller->output + preserved_length - caller_preserved_offset,
		   answerer_payloads[sequence],
		   sizeof(answerer_payloads[sequence])) != 0 ||
	    memcmp(answerer->output,
		   caller_preserved + answerer_preserved_offset,
		   preserved_length - answerer_preserved_offset) != 0 ||
	    memcmp(answerer->output +
			   preserved_length - answerer_preserved_offset,
		   caller_payloads[sequence],
		   sizeof(caller_payloads[sequence])) != 0) {
		fprintf(stderr,
			"preserved/post-retrain data failed "
			"(transferred=%d outputs=%zu/%zu offsets=%zu/%zu "
			"status=%u/%u state=%u/%u)\n",
			transferred, caller->output_count, answerer->output_count,
			caller_preserved_offset, answerer_preserved_offset,
			caller->modem->dp != NULL ? caller->modem->dp->status : 0,
			answerer->modem->dp != NULL ? answerer->modem->dp->status : 0,
			caller->modem->state, answerer->modem->state);
		return false;
	}
	return true;
}

static bool preserves_link_across_carrier_return(enum DP_ID id)
{
	return id == DP_B103 || id == DP_V21 || id == DP_B212 ||
	       id == DP_V22 || id == DP_V23;
}

static bool contains_pcm_rate_signal(const uint8_t *octets, size_t count, enum g711_law law, bool fast)
{
	/* Observe the exact 384/24 waveform without accessing datapump state or
	 * using its sequence generator. These direct-law profiles use one fixed
	 * magnitude per constellation, selected during negotiation rather than
	 * fixed by the profile seed. Rf is ++-- instead of Ru/Rd/Rt's +++---.
	 * The accepted data-frame tail comes first. */
	for (size_t start = 0; start + 408u <= count; ++start) {
		if (decode_codeword(law, octets[start]) <= 8)
			continue;
		size_t sample = 0;
		for (; sample < 408u; ++sample) {
			bool positive = (fast ? sample % 4u < 2u : sample % 6u < 3u) != (sample >= 384u);
			uint8_t expected = (octets[start + sample % 6u] & 0x7fu) | (positive ? 0x80u : 0u);
			if (octets[start + sample] != expected)
				break;
		}
		if (sample == 408u)
			return true;
	}
	return false;
}

static bool exercise_v92_rate_exchange(
	struct integration_endpoint *caller, struct integration_endpoint *answerer,
	enum media_codec codec, const struct media_recorder *recorder)
{
	/* A digital request sends Rd before the analogue emits Ru. Allow both
	 * signals and the media-frame buffering in the bounded Ru observation. */
	enum { PAYLOAD = 128, PREFIX = 2 * (PCM_R_SYMBOLS + PCM_R_BAR_SYMBOLS + SIP_FRAME_SAMPLES),
	       FRAME_LIMIT = 250, QUIET_LIMIT = MODEM_RATE * 70 / 1000,
	       PAUSE_LIMIT = 2 * MODEM_RATE };
	struct integration_endpoint *endpoints[] = { caller, answerer };
	unsigned analog = caller->modem->dsp_info.pcm_role == MODEM_PCM_ROLE_ANALOG ? 0u : 1u;
	struct dp *owned[] = { caller->modem->dp, answerer->modem->dp };
	unsigned host_state[] = { caller->modem->state, answerer->modem->state };
	enum g711_law law;

	CHECK(codec_law(codec, &law), "rate-exchange signal monitor setup failed");
	for (unsigned epoch = 0; epoch < 12; ++epoch) {
		bool fast = epoch >= 4u && epoch < 8u;
		bool pause = epoch >= 8u && epoch < 11u;
		uint8_t payload[2][2 * PAYLOAD];
		uint8_t prefix[2][PREFIX];
		uint8_t resume[PREFIX];
		struct lapm_state retained[2];
		unsigned quiet[2] = { 0 };
		unsigned longest_silence = 0u;
		bool resume_started = false;
		size_t resume_count = 0u;
		bool poll_seen[2] = { false };
		size_t prefix_count[2] = { 0 };
		unsigned settled = 0;
		for (unsigned side = 0; side < 2; ++side) {
			struct modem *m = endpoints[side]->modem;
			CHECK(modem_is_online(m, DP_V92) && m->cfg.ec &&
			      m->dsp_info.upstream_mode == MODEM_UPSTREAM_PCM,
			      "rate exchange did not start with a PCM LAPM link");
			endpoints[side]->output_count = 0;
			uint32_t random = UINT32_C(0x4e415449) + side + 17u * epoch;
			for (unsigned i = 0; i < PAYLOAD; ++i)
				payload[side][i] = (uint8_t)noise_state_next(&random);
			/* The first burst fills a real information frame. Repeating it
			 * after the exchange exercises retained compression history. */
			memcpy(payload[side] + PAYLOAD, payload[side], PAYLOAD);
			CHECK(modem_write(m, (const char *)payload[side], PAYLOAD) == PAYLOAD,
			      "rate exchange did not accept initial queued data");
		}
		CHECK(process_sip_frame(caller, answerer, codec, recorder) &&
		      endpoint_drain(caller) && endpoint_drain(answerer), "rate exchange setup failed");
		for (unsigned side = 0; side < 2; ++side) {
			struct modem *m = endpoints[side]->modem;
			frame_t *tx = m->packer.hdlc.tx_frame;
			CHECK(m->ec.lapm.va != m->ec.lapm.vs && tx != NULL &&
			      (tx->buf[1] & 1u) == 0 && tx->count > 0 && tx->count < tx->size &&
			      endpoints[side]->output_count < PAYLOAD,
			      "side %u has no outstanding partial LAPM frame", side);
			retained[side] = m->ec.lapm;
		}
		unsigned initiator = (epoch < 8u ? epoch % 4u < 2u : epoch != 9u) ? analog : 1u - analog;
		CHECK(fast ? v90_datapump_start_fast_exchange(owned[initiator]) &&
		      !v90_datapump_start_fast_exchange(owned[initiator]) :
		      v90_datapump_start_rate_renegotiation(owned[initiator], pause) &&
		      !v90_datapump_start_rate_renegotiation(owned[initiator], !pause),
		      "native rate request or duplicate rejection failed");
		CHECK(owned[initiator]->status == DPSTAT_OK &&
		      strcmp(modem_call_phase(endpoints[initiator]->modem), "retraining") == 0,
		      "rate exchange reported online before completing training");
		/* Exercise each role requesting silence, simultaneous requests, and
		 * then a plain exchange that must not inherit the preceding pause. */
		if (epoch == 10u)
			CHECK(v90_datapump_start_rate_renegotiation(owned[1u - initiator], true) &&
			      !v90_datapump_start_rate_renegotiation(owned[1u - initiator], false),
			      "simultaneous pause request or duplicate rejection failed");
		for (unsigned side = 0; side < 2; ++side) {
			struct modem *m = endpoints[side]->modem;
			CHECK(lapm_states_equal(&m->ec.lapm, &retained[side]) && !m->command,
			      "rate request changed retained LAPM or terminal mode");
			CHECK(modem_write(m, (const char *)payload[side] + PAYLOAD, PAYLOAD) == PAYLOAD,
			      "rate exchange did not retain following host data");
		}
		for (unsigned frame = 0; frame < FRAME_LIMIT; ++frame) {
			CHECK(process_sip_frame(caller, answerer, codec, recorder) &&
			      endpoint_drain(caller) && endpoint_drain(answerer),
			      "rate exchange media failed at frame %u", frame);
			for (unsigned side = 0; side < 2; ++side) {
				struct modem *m = endpoints[side]->modem;
				CHECK(m->started && m->get_bits != NULL && m->put_bits != NULL &&
				      m->state == host_state[side] && m->dp == owned[side] &&
				      (m->dp->status == DPSTAT_OK || m->dp->status == DPSTAT_CONNECT) &&
				      strcmp(modem_call_phase(m), m->dp->status == DPSTAT_OK ?
					     "retraining" : "online") == 0 && m->cfg.ec && !m->command &&
				      m->ec.lapm.state == retained[side].state &&
				      m->ec.lapm.v44_path == retained[side].v44_path &&
				      lapm_v44_sessions_equal(&m->ec.lapm.v44_session,
						     &retained[side].v44_session),
				      "rate exchange replaced side %u's online link", side);
				poll_seen[side] |= m->ec.lapm.retrain_poll != 0;
				for (unsigned sample = 0; sample < SIP_FRAME_SAMPLES; ++sample) {
					uint8_t code = endpoints[side]->transmit_codewords[sample];
					bool silent = abs(decode_codeword(law, code)) <= 8;
					if (pause && side != analog && !silent && quiet[side] >= QUIET_LIMIT)
						resume_started = true;
					quiet[side] = silent ? quiet[side] + 1u : 0u;
					CHECK(quiet[side] < (pause && side != analog ? PAUSE_LIMIT : QUIET_LIMIT),
					      "rate exchange fell back to full retrain or exceeded its pause bound");
					if (side != analog) {
						if (quiet[side] > longest_silence)
							longest_silence = quiet[side];
						if (resume_started && resume_count < sizeof(resume))
							resume[resume_count++] = code;
					}
					if (prefix_count[side] < sizeof(prefix[side]))
						prefix[side][prefix_count[side]++] = code;
				}
			}
			bool delivered = caller->output_count == sizeof(payload[1]) &&
				answerer->output_count == sizeof(payload[0]) &&
				caller->modem->ec.lapm.va == caller->modem->ec.lapm.vs &&
				answerer->modem->ec.lapm.va == answerer->modem->ec.lapm.vs;
			/* Continue after delivery to detect delayed duplicate payload. */
			settled = delivered ? settled + 1u : 0u;
			if (settled == 25u)
				break;
		}
		CHECK(fast ? contains_pcm_rate_signal(prefix[1u - analog], prefix_count[1u - analog], law, true) :
		      (contains_pcm_rate_signal(prefix[0], prefix_count[0], law, false) &&
		       contains_pcm_rate_signal(prefix[1], prefix_count[1], law, false)),
		      "rate exchange %u did not emit %s", epoch, fast ? "Rf" : "Ru/Rd");
		CHECK(!pause || (longest_silence >= MODEM_RATE * 300 / 1000 &&
		      contains_pcm_rate_signal(resume, resume_count, law, false)),
		      "rate exchange %u did not transmit bounded Ucode-zero silence then Rt (silence=%u resume=%zu)",
		      epoch, longest_silence, resume_count);
		CHECK(poll_seen[0] && poll_seen[1],
		      "rate exchange %u did not poll retained LAPM state (polls=%d/%d)",
		      epoch, poll_seen[0], poll_seen[1]);
		CHECK(settled == 25u && modem_is_online(caller->modem, DP_V92) &&
		      modem_is_online(answerer->modem, DP_V92) &&
		      memcmp(caller->output, payload[1], sizeof(payload[1])) == 0 &&
		      memcmp(answerer->output, payload[0], sizeof(payload[0])) == 0,
		      "rate exchange %u lost ordered LAPM data (outputs=%zu/%zu, settled=%u)",
		      epoch, caller->output_count, answerer->output_count, settled);
	}
	return true;
}

static bool exercise_v34_media_gap(
	struct integration_endpoint *caller, struct integration_endpoint *answerer,
	enum media_codec codec, const struct media_recorder *recorder)
{
	struct integration_endpoint *ends[] = { caller, answerer };
	uint8_t payload[2][64];

	for (unsigned side = 0; side < 2u; ++side) {
		uint32_t random = UINT32_C(0x47415000) + side;
		for (unsigned i = 0; i < sizeof(payload[side]); ++i)
			payload[side][i] = (uint8_t)noise_state_next(&random);
	}
	for (unsigned lost_side = 0; lost_side < 2u; ++lost_side) {
		ends[lost_side]->erased_samples = 80u;
		/* Clock the missing 10 ms and recovery tail. Raw bytes spanning the
		 * gap may be corrupt; later bytes must not retain a framing offset. */
		for (unsigned frame = 0; frame < 50u; ++frame)
			CHECK(process_sip_frame(caller, answerer, codec, recorder) &&
			      endpoint_drain(caller) && endpoint_drain(answerer),
			      "V.34 gap recovery media failed");
		for (unsigned side = 0; side < 2u; ++side) {
			CHECK(modem_is_online(ends[side]->modem, DP_V34) &&
			      !ends[side]->modem->cfg.ec, "V.34 gap lost its raw link");
			ends[side]->output_count = 0;
			CHECK(modem_write(ends[side]->modem, (const char *)payload[side],
					  sizeof(payload[side])) == sizeof(payload[side]),
			      "V.34 gap payload could not be queued");
		}
		for (unsigned frame = 0; frame < 50u; ++frame)
			CHECK(process_sip_frame(caller, answerer, codec, recorder) &&
			      endpoint_drain(caller) && endpoint_drain(answerer),
			      "V.34 gap payload media failed");
		for (unsigned side = 0; side < 2u; ++side)
			CHECK(ends[side]->output_count == sizeof(payload[side]) &&
			      memcmp(ends[side]->output, payload[1u - side],
				     sizeof(payload[side])) == 0,
			      "V.34 gap at side %u corrupted side %u's later data (%zu bytes)",
			      lost_side, side, ends[side]->output_count);
	}
	return true;
}

static bool exercise_v92_media_loss(
	struct integration_endpoint *caller, struct integration_endpoint *answerer,
	enum media_codec codec, const struct media_recorder *recorder)
{
	enum { PAYLOAD = 128 };
	struct integration_endpoint *ends[] = { caller, answerer };
	struct dp *owned[] = { caller->modem->dp, answerer->modem->dp };
	uint32_t rates[2][2] = {
		{ caller->modem->tx_rate, caller->modem->rx_rate },
		{ answerer->modem->tx_rate, answerer->modem->rx_rate },
	};
	struct lapm_state retained[2];
	uint8_t payload[2][2 * PAYLOAD];
	bool poll_seen[2] = { false, false };
	unsigned settled = 0;

	for (unsigned side = 0; side < 2u; ++side) {
		struct modem *m = ends[side]->modem;
		CHECK(modem_is_online(m, DP_V92) && m->cfg.ec && !m->ec.lapm.retrain_poll &&
		      m->dsp_info.upstream_mode == MODEM_UPSTREAM_PCM,
		      "media loss fixture requires a settled PCM LAPM link");
		ends[side]->output_count = 0;
		uint32_t random = UINT32_C(0x45524153) + side;
		for (unsigned i = 0; i < PAYLOAD; ++i)
			payload[side][i] = (uint8_t)noise_state_next(&random);
		memcpy(payload[side] + PAYLOAD, payload[side], PAYLOAD);
		CHECK(modem_write(m, (const char *)payload[side], PAYLOAD) == PAYLOAD,
		      "media loss fixture did not queue LAPM data");
	}
	CHECK(process_sip_frame(caller, answerer, codec, recorder) &&
	      endpoint_drain(caller) && endpoint_drain(answerer), "media loss setup failed");
	for (unsigned side = 0; side < 2u; ++side) {
		struct modem *m = ends[side]->modem;
		frame_t *tx = m->packer.hdlc.tx_frame;
		CHECK(m->ec.lapm.va != m->ec.lapm.vs && tx != NULL &&
		      (tx->buf[1] & 1u) == 0 && tx->count > 0 && tx->count < tx->size &&
		      ends[side]->output_count < PAYLOAD,
		      "media loss did not interrupt side %u's partial LAPM frame", side);
		retained[side] = m->ec.lapm;
		/* A 10 ms hole in PCM upstream must cause autonomous recovery,
		 * not require ATO1 or discard the acknowledged link's queues. */
		if (m->dsp_info.pcm_role == MODEM_PCM_ROLE_DIGITAL)
			ends[side]->erased_samples = 80u;
		CHECK(modem_write(m, (const char *)payload[side] + PAYLOAD, PAYLOAD) == PAYLOAD,
		      "media loss fixture did not retain following terminal data");
	}
	for (unsigned frame = 0; frame < RETRAIN_FRAME_LIMIT + DATA_FRAME_LIMIT; ++frame) {
		CHECK(process_sip_frame(caller, answerer, codec, recorder) &&
		      endpoint_drain(caller) && endpoint_drain(answerer), "media loss recovery failed");
		for (unsigned side = 0; side < 2u; ++side) {
			struct modem *m = ends[side]->modem;
			CHECK(m->dp == owned[side] && m->cfg.ec && !m->command &&
			      m->ec.lapm.state == retained[side].state &&
			      m->ec.lapm.v44_path == retained[side].v44_path &&
			      lapm_v44_sessions_equal(&m->ec.lapm.v44_session,
					     &retained[side].v44_session),
			      "media loss replaced side %u's data link", side);
			poll_seen[side] |= m->ec.lapm.retrain_poll != 0;
		}
		bool delivered = caller->output_count == sizeof(payload[1]) &&
			answerer->output_count == sizeof(payload[0]) &&
			caller->modem->ec.lapm.va == caller->modem->ec.lapm.vs &&
			answerer->modem->ec.lapm.va == answerer->modem->ec.lapm.vs;
		settled = delivered ? settled + 1u : 0u;
		if (settled == 25u)
			break;
	}
	CHECK(poll_seen[0] && poll_seen[1] && settled == 25u &&
	      caller->modem->dsp_info.upstream_mode == MODEM_UPSTREAM_PCM &&
	      answerer->modem->dsp_info.upstream_mode == MODEM_UPSTREAM_PCM &&
	      memcmp(caller->output, payload[1], sizeof(payload[1])) == 0 &&
	      memcmp(answerer->output, payload[0], sizeof(payload[0])) == 0,
	      "media loss recovery lost ordered PCM LAPM data (polls=%d/%d outputs=%zu/%zu settled=%u)",
	      poll_seen[0], poll_seen[1], caller->output_count, answerer->output_count, settled);
	for (unsigned side = 0; side < 2; ++side)
		CHECK(ends[side]->modem->tx_rate == rates[side][0] &&
		      ends[side]->modem->rx_rate == rates[side][1],
		      "known media loss unnecessarily lowered the negotiated PCM rates");
	return true;
}

static bool exercise_pcm_sample_slip(
	struct integration_endpoint *caller, struct integration_endpoint *answerer,
	enum media_codec codec, const struct media_recorder *recorder, bool downstream)
{
	enum { PAYLOAD = 384, FRAME_LIMIT = 1500 };
	struct integration_endpoint *ends[] = { caller, answerer };
	unsigned analog = caller->modem->dsp_info.pcm_role == MODEM_PCM_ROLE_ANALOG ? 0u : 1u;
	unsigned receiver = downstream ? analog : 1u - analog;
	struct dp *owned[] = { caller->modem->dp, answerer->modem->dp };
	struct v44_character_path *compression[] = {
		caller->modem->ec.lapm.v44_path, answerer->modem->ec.lapm.v44_path,
	};
	uint8_t payload[2][PAYLOAD];
	unsigned upstream = caller->modem->dsp_info.upstream_mode;
	bool recovered = false;
	unsigned settled = 0;

	CHECK(downstream || upstream == MODEM_UPSTREAM_PCM,
	      "upstream slip requires negotiated PCM upstream");
	for (unsigned side = 0; side < 2; ++side) {
		CHECK(ends[side]->modem->cfg.ec && !ends[side]->modem->ec.lapm.retrain_poll &&
		      !ends[side]->modem->ec.lapm.stalled_retrain,
		      "PCM slip requires a settled LAPM link");
		ends[side]->output_count = 0;
		uint32_t random = UINT32_C(0x534c4950) + side;
		for (unsigned i = 0; i < PAYLOAD; ++i)
			payload[side][i] = (uint8_t)noise_state_next(&random);
		CHECK(modem_write(ends[side]->modem, (const char *)payload[side], PAYLOAD) == PAYLOAD,
		      "PCM slip did not queue terminal data");
	}
	CHECK(process_sip_frame(caller, answerer, codec, recorder) &&
	      endpoint_drain(caller) && endpoint_drain(answerer), "PCM slip setup failed");
	CHECK(ends[analog]->modem->ec.lapm.va != ends[analog]->modem->ec.lapm.vs &&
	      ends[1u - analog]->modem->ec.lapm.va != ends[1u - analog]->modem->ec.lapm.vs,
	      "PCM slip has no unacknowledged frames");
	/* Repeat one received sample, retaining the added delay through the
	 * recovery. These are available G.711 symbols, not flagged erasures. A
	 * downstream mapper-phase slip can leave every hard decision legal but HDLC
	 * broken; upstream recovery can instead start at the physical quality gate.
	 * Keep the delay through the following cleardown, too. */
	CHECK(!ends[receiver]->delay_one_sample, "PCM receiver already has a sample delay");
	ends[receiver]->prior_codeword = ends[receiver]->receive_codewords[0];
	ends[receiver]->delay_one_sample = true;
	for (unsigned frame = 0; frame < FRAME_LIMIT; ++frame) {
		CHECK(process_sip_frame(caller, answerer, codec, recorder) &&
		      endpoint_drain(caller) && endpoint_drain(answerer), "PCM slip media failed");
		for (unsigned side = 0; side < 2; ++side) {
			struct modem *m = ends[side]->modem;
			recovered |= downstream ? m->ec.lapm.stalled_retrain : m->ec.lapm.retrain_poll;
			CHECK(m->dp == owned[side] && m->cfg.ec && !m->command && !m->retrain_pending &&
			      m->ec.lapm.v44_path == compression[side] &&
			      ends[side]->output_count <= PAYLOAD &&
			      memcmp(ends[side]->output, payload[1u - side], ends[side]->output_count) == 0,
			      "PCM slip changed the link or delivered incorrect bytes");
		}
		bool delivered = caller->output_count == PAYLOAD && answerer->output_count == PAYLOAD &&
			caller->modem->ec.lapm.va == caller->modem->ec.lapm.vs &&
			answerer->modem->ec.lapm.va == answerer->modem->ec.lapm.vs;
		settled = delivered ? settled + 1u : 0u;
		if (settled == 50)
			break;
	}
	CHECK(recovered && settled == 50 &&
	      caller->modem->dsp_info.upstream_mode == upstream &&
	      answerer->modem->dsp_info.upstream_mode == upstream,
	      "PCM slip did not retrain and preserve all queued data (%zu/%zu, recovered=%d)",
	      caller->output_count, answerer->output_count, recovered);
	return true;
}

static bool exercise_low_speed_carrier_recovery(
	struct integration_endpoint *caller,
	struct integration_endpoint *answerer,
	enum DP_ID id,
	enum media_codec codec, const struct media_recorder *recorder,
	bool raw_transport)
{
	enum {
		SHORT_LOSS_FRAMES = 2,
		CARRIER_LOSS_SAMPLES = MODEM_RATE / 2,
		CARRIER_RECOVERY_SAMPLES = MODEM_RATE * 3 / 10,
	};
	static const uint8_t caller_clamped_payload[] = {
		'c', 'l', 'a', 'm', 'p', '-', 'c', 0x00, 0xa5, 0xff,
	};
	static const uint8_t answerer_clamped_payload[] = {
		'c', 'l', 'a', 'm', 'p', '-', 'a', 0xff, 0x5a, 0x00,
	};
	static const uint8_t caller_payload[] = {
		'c', 'a', 'r', 'r', 'i', 'e', 'r', '-', 'c', 0xa5,
	};
	static const uint8_t answerer_payload[] = {
		'c', 'a', 'r', 'r', 'i', 'e', 'r', '-', 'a', 0x5a,
	};
	enum DP_ID caller_id = caller->modem->dp->id;
	enum DP_ID answerer_id = answerer->modem->dp->id;

	if (!preserves_link_across_carrier_return(id) && id != DP_V22BIS)
		return false;
	for (unsigned int recovery =
		     id != DP_V22BIS ? 0u : raw_transport ? 0u : 1u;
	     recovery < (id != DP_V22BIS ? 1u : 2u); ++recovery) {
		bool link_uses_lapm = caller->modem->cfg.ec &&
				      answerer->modem->cfg.ec;
		bool transferred = false;

		caller->output_count = 0;
		answerer->output_count = 0;
		if (id != DP_V22BIS) {
			if (link_uses_lapm &&
			    (modem_write(
				     caller->modem,
				     (const char *)caller_clamped_payload,
				     (int)sizeof(caller_clamped_payload)) !=
					     (int)sizeof(caller_clamped_payload) ||
			     modem_write(
				     answerer->modem,
				     (const char *)answerer_clamped_payload,
				     (int)sizeof(answerer_clamped_payload)) !=
					     (int)sizeof(answerer_clamped_payload)))
				return false;
			/* Cut each 300-bit/s raw receiver through a partial character;
			 * the carrier transition must abandon only that receive prefix. */
			if (!link_uses_lapm && (id == DP_B103 || id == DP_V21)) {
				static const uint8_t partial = 0x00;

				if (modem_write(
					    caller->modem, (const char *)&partial, 1) != 1 ||
				    modem_write(
					    answerer->modem, (const char *)&partial, 1) != 1 ||
				    !process_sip_frame(
					    caller, answerer, codec, recorder) ||
				    !endpoint_drain(caller) ||
				    !endpoint_drain(answerer))
					return false;
				caller->output_count = 0;
				answerer->output_count = 0;
			}
			/* These constant-carrier modes preserve the established link
			 * across an S10-bounded interruption.  Idle carrier after the
			 * interruption lets the existing receiver reacquire without
			 * replaying the initial handshake or releasing LAPM state. */
			for (unsigned int samples = 0;
			     samples < CARRIER_LOSS_SAMPLES;
			     samples += SIP_FRAME_SAMPLES) {
				if (!process_sip_frame_with_loss(
					    caller, answerer, codec, recorder,
					    true, true, false, false) ||
				    !endpoint_drain(caller) ||
				    !endpoint_drain(answerer) ||
				    caller->output_count != 0 ||
				    answerer->output_count != 0 ||
				    !modem_is_online(caller->modem, caller_id) ||
				    !modem_is_online(answerer->modem, answerer_id))
					return false;
			}
			for (unsigned int samples = 0;
			     samples < CARRIER_RECOVERY_SAMPLES;
			     samples += SIP_FRAME_SAMPLES) {
				if (!process_sip_frame(
					    caller, answerer, codec, recorder) ||
				    !endpoint_drain(caller) ||
				    !endpoint_drain(answerer) ||
				    (!link_uses_lapm &&
				     (caller->output_count != 0 ||
				      answerer->output_count != 0)) ||
				    !modem_is_online(caller->modem, caller_id) ||
				    !modem_is_online(answerer->modem, answerer_id))
					return false;
			}
			if (link_uses_lapm) {
				for (unsigned int frame = 0;
				     frame < DATA_FRAME_LIMIT &&
				     (caller->output_count <
					      sizeof(answerer_clamped_payload) ||
				      answerer->output_count <
					      sizeof(caller_clamped_payload));
				     ++frame) {
					if (!process_sip_frame(
						    caller, answerer, codec, recorder) ||
					    !endpoint_drain(caller) ||
					    !endpoint_drain(answerer) ||
					    !modem_is_online(caller->modem, caller_id) ||
					    !modem_is_online(answerer->modem, answerer_id))
						return false;
				}
				if (caller->output_count !=
					    sizeof(answerer_clamped_payload) ||
				    answerer->output_count !=
					    sizeof(caller_clamped_payload) ||
				    memcmp(
					    caller->output, answerer_clamped_payload,
					    sizeof(answerer_clamped_payload)) != 0 ||
				    memcmp(
					    answerer->output, caller_clamped_payload,
					    sizeof(caller_clamped_payload)) != 0) {
					fprintf(stderr,
						"carrier recovery did not preserve LAPM "
						"payloads (id=%u outputs=%zu/%zu "
						"states=%u/%u lapm=%d/%d control pending=%d/%d "
						"timer=%d/%d)\n",
						(unsigned int)id, caller->output_count,
						answerer->output_count,
						caller->modem->state,
						answerer->modem->state,
						caller->modem->ec.lapm.state,
						answerer->modem->ec.lapm.state,
						caller->modem->ec.lapm.tx_ctrl != caller->modem->ec.lapm.ctrl_list,
						answerer->modem->ec.lapm.tx_ctrl != answerer->modem->ec.lapm.ctrl_list,
						caller->modem->bit_timer,
						answerer->modem->bit_timer);
					return false;
				}
			}
		} else if (recovery == 0) {
			for (unsigned int frame = 0;
			     frame < SHORT_LOSS_FRAMES; ++frame) {
				if (!process_sip_frame_with_loss(
					    caller, answerer, codec, recorder,
					    true, true, false, false) ||
				    !endpoint_drain(caller) ||
				    !endpoint_drain(answerer) ||
				    caller->output_count != 0 ||
				    answerer->output_count != 0 ||
				    !modem_is_online(caller->modem, DP_V22BIS) ||
				    !modem_is_online(answerer->modem, DP_V22BIS))
					return false;
			}
			if (modem_write(
				    caller->modem,
				    (const char *)caller_clamped_payload,
				    (int)sizeof(caller_clamped_payload)) !=
					    (int)sizeof(caller_clamped_payload) ||
			    modem_write(
				    answerer->modem,
				    (const char *)answerer_clamped_payload,
				    (int)sizeof(answerer_clamped_payload)) !=
					    (int)sizeof(answerer_clamped_payload))
				return false;
			for (unsigned int samples = 0;
			     samples < MODEM_RATE / 10;
			     samples += SIP_FRAME_SAMPLES) {
				if (!process_sip_frame(
					    caller, answerer, codec, recorder) ||
				    !endpoint_drain(caller) ||
				    !endpoint_drain(answerer) ||
				    caller->output_count != 0 ||
				    answerer->output_count != 0 ||
				    !modem_is_online(caller->modem, DP_V22BIS) ||
				    !modem_is_online(answerer->modem, DP_V22BIS))
					return false;
			}
		} else {
			bool retrain_started = false;
			bool retrain_completed = false;

			for (unsigned int samples = 0; samples < MODEM_RATE / 2;
			     samples += SIP_FRAME_SAMPLES) {
				if (!process_sip_frame_with_loss(
					    caller, answerer, codec, recorder,
					    true, true, false, false) ||
				    !endpoint_drain(caller) ||
				    !endpoint_drain(answerer) ||
				    caller->output_count != 0 ||
				    answerer->output_count != 0 ||
				    caller->modem->process == NULL ||
				    answerer->modem->process == NULL)
					return false;
			}
			for (unsigned int frame = 0;
			     frame < RETRAIN_FRAME_LIMIT; ++frame) {
				if (!process_sip_frame(
					    caller, answerer, codec, recorder) ||
				    !endpoint_drain(caller) ||
				    !endpoint_drain(answerer))
					return false;
				if (!modem_is_online(caller->modem, DP_V22BIS) ||
				    !modem_is_online(answerer->modem, DP_V22BIS))
					retrain_started = true;
				if (retrain_started &&
				    modem_is_online(caller->modem, DP_V22BIS) &&
				    modem_is_online(answerer->modem, DP_V22BIS)) {
					retrain_completed = true;
					break;
				}
			}
			if (!retrain_completed)
				return false;
		}
		caller->output_count = 0;
		answerer->output_count = 0;
		if (modem_write(
			    caller->modem, (const char *)caller_payload,
			    (int)sizeof(caller_payload)) !=
				    (int)sizeof(caller_payload) ||
		    modem_write(
			    answerer->modem, (const char *)answerer_payload,
			    (int)sizeof(answerer_payload)) !=
				    (int)sizeof(answerer_payload))
			return false;
		for (unsigned int frame = 0; frame < DATA_FRAME_LIMIT; ++frame) {
			if (!process_sip_frame(caller, answerer, codec, recorder) ||
			    !endpoint_drain(caller) || !endpoint_drain(answerer))
				return false;
			if (caller->output_count >= sizeof(answerer_payload) &&
			    answerer->output_count >= sizeof(caller_payload)) {
				transferred = true;
				break;
			}
		}
		if (!transferred ||
		    caller->output_count != sizeof(answerer_payload) ||
		    answerer->output_count != sizeof(caller_payload) ||
		    memcmp(caller->output, answerer_payload,
			   sizeof(answerer_payload)) != 0 ||
		    memcmp(answerer->output, caller_payload,
			   sizeof(caller_payload)) != 0)
			return false;
	}
	return true;
}

static bool exercise_fsk_data_carrier_recovery(
	struct integration_endpoint *caller,
	struct integration_endpoint *answerer, enum DP_ID id,
	enum media_codec codec, const struct media_recorder *recorder)
{
	struct integration_endpoint *endpoints[] = { caller, answerer };
	uint8_t payload[2][72];

	for (size_t side = 0; side < ARRAY_LENGTH(endpoints); ++side) {
		endpoints[side]->output_count = 0;
		for (size_t byte = 0; byte < sizeof(payload[side]); ++byte)
			payload[side][byte] = byte % 3 == 0 ? 0xff :
				(uint8_t)(byte + 53 * side);
		CHECK(modem_write(endpoints[side]->modem,
				  (const char *)payload[side], sizeof(payload[side])) ==
		      (int)sizeof(payload[side]), "FSK data recovery payload was not queued");
	}
	for (unsigned int frame = 0; frame < 200; ++frame) {
		/* Lose 10 ms during continuous characters, without an idle MARK
		 * interval afterward. Both receivers must recover on data. */
		if (frame == 9) {
			caller->erased_samples = MODEM_RATE / 100;
			answerer->erased_samples = MODEM_RATE / 100;
		}
		CHECK(process_sip_frame(caller, answerer, codec, recorder),
		      "FSK data recovery failed at the media boundary");
		for (size_t side = 0; side < ARRAY_LENGTH(endpoints); ++side)
			CHECK(endpoint_drain(endpoints[side]) &&
			      modem_is_online(endpoints[side]->modem, id),
			      "FSK id=%d side=%zu lost data-bearing carrier at frame %u",
			      id, side, frame);
	}
	for (size_t side = 0; side < ARRAY_LENGTH(endpoints); ++side) {
		struct integration_endpoint *endpoint = endpoints[side];

		/* Raw bytes overlapping the loss may be damaged or missing, but
		 * the undisturbed suffix must arrive before the sender goes idle. */
		CHECK(endpoint->output_count >= 32 &&
		      endpoint->output_count <= sizeof(payload[0]) &&
		      memcmp(endpoint->output + endpoint->output_count - 32,
			     payload[1 - side] + sizeof(payload[0]) - 32, 32) == 0,
		      "FSK id=%d side=%zu did not recover the data suffix (%zu bytes)",
		      id, side, endpoint->output_count);
	}
	return true;
}

static bool exercise_cleardown(
	struct integration_endpoint *caller,
	struct integration_endpoint *answerer,
	enum DP_ID id, unsigned int initiators,
	enum media_codec codec, const struct media_recorder *recorder)
{
	enum { TEST_STATE_COMMAND_ONLINE = 6 };
	struct integration_endpoint *endpoints[] = { caller, answerer };
	bool completed = false;

	for (size_t index = 0; index < ARRAY_LENGTH(endpoints); ++index) {
		struct integration_endpoint *endpoint = endpoints[index];
		char command[] = "ATH0";

		CHECK(modem_is_online(endpoint->modem, id) &&
		      endpoint->stops == 0u,
		      "cleardown did not start on a live line");
		endpoint->output_count = 0u;
		if (!(initiators & (1u << index)))
			continue;
		endpoint->modem->state = TEST_STATE_COMMAND_ONLINE;
		endpoint->modem->command = 1;
		CHECK(process_at_command(endpoint->modem, command) == 1 &&
		      endpoint_drain(endpoint) && endpoint->output_count == 0u &&
		      endpoint->modem->dp != NULL && endpoint->stops == 0u,
		      "ATH0 completed before its line sequence");
	}
	for (unsigned int frame = 0u; frame < 100u; ++frame) {
		CHECK(process_sip_frame(caller, answerer, codec, recorder),
		      "cleardown failed at the SIP media boundary");
		for (size_t index = 0; index < ARRAY_LENGTH(endpoints); ++index) {
			struct integration_endpoint *endpoint = endpoints[index];
			bool local = (initiators & (1u << index)) != 0u;

			CHECK(endpoint_drain(endpoint) && endpoint->stops <= 1u &&
			      (endpoint->stops == 0u ?
				(endpoint->modem->dp != NULL &&
				 (!local || endpoint->output_count == 0u)) :
				(endpoint->modem->dp == NULL &&
				 !endpoint->modem->started &&
				 output_contains(endpoint, local ? "OK" : "NO CARRIER") &&
				 (!local || !output_contains(endpoint, "NO CARRIER")))),
			      "ATH0 final boundary frame=%u side=%zu stops=%zu "
			      "dp=%p started=%d output=%.*s",
			      frame, index, endpoint->stops,
			      (void *)endpoint->modem->dp,
			      endpoint->modem->started, (int)endpoint->output_count,
			      endpoint->output);
		}
		if (caller->stops != 0u && answerer->stops != 0u) {
			CHECK(frame > 2u,
			      "ATH0 retained the old immediate teardown");
			completed = true;
			break;
		}
	}
	CHECK(completed,
	      "ATH0 did not finish before the missing-peer safety deadline");
	return true;
}

/* A peer can send ANS or unanswered ANSam before its legacy mark. Keep the
 * fixed datapump unclocked until that preamble has finished. */
static bool present_legacy_answer_tone(struct integration_endpoint *caller,
	enum media_codec codec, unsigned int milliseconds, bool modulated)
{
	struct single_tone_transmitter tone;
	struct v8_ansam_generator ansam;
	enum g711_law law;
	unsigned int samples = milliseconds * (MODEM_RATE / 1000);

	if (!codec_law(codec, &law))
		return false;
	if (modulated) {
		if (v8_ansam_generator_init(&ansam, 12000, true) != V8_ANSAM_OK)
			return false;
	} else if (single_tone_transmitter_init(&tone, 2100, 12000) != SINGLE_TONE_OK)
		return false;
	for (unsigned int offset = 0; offset < samples; offset += MODEM_FRAG) {
		int16_t receive[MODEM_FRAG], transmit[MODEM_FRAG];
		unsigned int count = samples - offset;

		if (count > MODEM_FRAG)
			count = MODEM_FRAG;
		for (unsigned int i = 0; i < count; ++i) {
			int16_t sample;

			if (modulated) {
				if (v8_ansam_generate(&ansam, &sample, 1) != V8_ANSAM_OK)
					return false;
			} else if (single_tone_transmitter_sample(&tone, &sample) != SINGLE_TONE_OK)
				return false;
			receive[i] = decode_codeword(law, encode_codeword(law, sample));
		}
		modem_process(caller->modem, receive, transmit, (int)count);
		if (!endpoint_drain(caller) || caller->modem->dp == NULL ||
		    caller->modem->dp->id != DP_V8)
			return false;
	}
	return !modulated ||
		strcmp(modem_protocol_step(caller->modem), "V.8/CM-wait-JM") == 0;
}

static bool exercise_v91_payload(
	struct integration_endpoint *caller, struct integration_endpoint *answerer,
	enum media_codec codec, const struct media_recorder *recorder)
{
	struct integration_endpoint *ends[] = { caller, answerer };
	uint32_t random = UINT32_C(0x91345678);
	for (unsigned epoch = 0; epoch < 32; ++epoch) {
		uint8_t payload[2][512];
		for (unsigned side = 0; side < 2; ++side) {
			ends[side]->output_count = 0;
			for (unsigned i = 0; i < sizeof(payload[side]); ++i)
				payload[side][i] = noise_state_next(&random);
			CHECK(modem_write(ends[side]->modem, (char *)payload[side], sizeof(payload[side])) ==
			      sizeof(payload[side]), "V.91 payload queue rejected data");
		}
		for (unsigned frame = 0; frame < 100; ++frame) {
			CHECK(process_sip_frame(caller, answerer, codec, recorder) &&
			      endpoint_drain(caller) && endpoint_drain(answerer) &&
			      modem_is_online(caller->modem, DP_V91) && modem_is_online(answerer->modem, DP_V91),
			      "V.91 stable channel retrained or lost carrier");
			if (caller->output_count >= sizeof(payload[1]) && answerer->output_count >= sizeof(payload[0]))
				break;
		}
		for (unsigned side = 0; side < 2; ++side)
			CHECK(ends[side]->output_count == sizeof(payload[side]) &&
			      memcmp(ends[side]->output, payload[1 - side], sizeof(payload[side])) == 0,
			      "V.91 payload differed at epoch=%u side=%u", epoch, side);
	}
	return true;
}

static bool exercise_v91_recovery(
	struct integration_endpoint *caller, struct integration_endpoint *answerer,
	enum media_codec codec, const struct media_recorder *recorder, bool change_channel)
{
	struct integration_endpoint *ends[] = { caller, answerer };
	struct dp *owned[] = { caller->modem->dp, answerer->modem->dp };
	uint8_t payload[2][512];
	bool retrained = false;
	/* Rearm the bounded automatic recovery budget after prior ATO1 tests. */
	for (unsigned frame = 0; frame < 650; ++frame)
		CHECK(process_sip_frame(caller, answerer, codec, recorder) &&
		      endpoint_drain(caller) && endpoint_drain(answerer), "V.91 healthy interval failed");
	for (unsigned side = 0; side < 2; ++side) {
		ends[side]->output_count = 0;
		uint32_t random = UINT32_C(0x91928374) + side;
		for (unsigned i = 0; i < sizeof(payload[side]); ++i)
			payload[side][i] = noise_state_next(&random);
		CHECK(modem_write(ends[side]->modem, (char *)payload[side], sizeof(payload[side])) ==
		      sizeof(payload[side]), "V.91 recovery queue rejected data");
	}
	if (change_channel)
		caller->robbed_bits = true;
	else
		caller->erased_samples = 80;
	for (unsigned frame = 0; frame < 2000; ++frame) {
		CHECK(process_sip_frame(caller, answerer, codec, recorder) &&
		      endpoint_drain(caller) && endpoint_drain(answerer), "V.91 recovery media failed");
		for (unsigned side = 0; side < 2; ++side) {
			CHECK(ends[side]->modem->dp == owned[side] && ends[side]->modem->cfg.ec &&
			      !ends[side]->modem->command, "V.91 recovery replaced the connected data link");
			retrained |= owned[side]->status == DPSTAT_OK;
		}
		if (retrained && caller->output_count == sizeof(payload[1]) &&
		    answerer->output_count == sizeof(payload[0]) &&
		    modem_is_online(caller->modem, DP_V91) && modem_is_online(answerer->modem, DP_V91))
			break;
	}
	for (unsigned side = 0; side < 2; ++side)
		CHECK(retrained && ends[side]->output_count == sizeof(payload[side]) &&
		      memcmp(ends[side]->output, payload[1 - side], sizeof(payload[side])) == 0,
		      "V.91 recovery lost queued data, channel change=%u side=%u count=%zu retrained=%u",
		      change_channel, side, ends[side]->output_count, retrained);
	CHECK(caller->modem->tx_rate == 64000 && answerer->modem->rx_rate == 64000 &&
	      caller->modem->rx_rate == (change_channel ? 56000u : 64000u) &&
	      answerer->modem->tx_rate == caller->modem->rx_rate,
	      "V.91 recovery did not relearn reciprocal rates");
	return exercise_v91_payload(caller, answerer, codec, recorder);
}

static bool run_media_call(
	const struct modem_test_case *test_case,
	enum media_codec codec)
{
	static const char prelink_payload[] = "early-banner\r\n";
	static const uint8_t caller_payload[] = {
		0x00, 0x01, 0x55, 0xaa, 0x7f, 0x80, 0xfe, 0xff,
		'V', '2', '1', 'A',
	};
	static const uint8_t answer_payload[] = {
		0xff, 0x81, 0x42, 0x24, 0x18, 0x00, 0x7e, 0xe7,
		'S', 'I', 'P', 'B',
	};
	static const char dial_command[] = "ATDpeer\r";
	static const char auto_answer_command[] = "ATS0=1\r";
	struct integration_endpoint caller;
	struct integration_endpoint answerer;
	struct media_frame_delay caller_delay = { 0 };
	struct recording_capture capture = { 0 };
	struct v22_rate_monitor caller_rate_monitor;
	const struct media_recorder recorder = {
		.context = &capture,
		.write = capture_recording,
	};
	bool uses_v8 = (test_case->automode || test_case->requires_v8) &&
		v8_datapump_modulation(test_case->id) !=
		V8_MODULATION_NONE;
	enum DP_ID answerer_id = test_case->answerer_id != 0 ?
		test_case->answerer_id : test_case->id;
	enum DP_ID connected_id = test_case->connected_id != 0 ?
		test_case->connected_id : test_case->id;
	enum DP_ID answerer_connected_id = test_case->answerer_connected_id != 0 ?
		test_case->answerer_connected_id : connected_id;
	const char *answerer_selection =
		test_case->answerer_selection_command != NULL ?
			test_case->answerer_selection_command :
			test_case->selection_command;
	bool answerer_requires_v8 =
		((!test_case->answerer_fixed && test_case->automode) ||
		 test_case->answerer_automode || test_case->requires_v8) &&
		v8_datapump_modulation(answerer_id) != V8_MODULATION_NONE;
	bool online = false;
	bool answerer_used_v8 = !answerer_requires_v8;
	bool prelink_queued = false;
	bool prelink_delivered = false;
	bool transferred = false;
	bool answerer_signal_seen = false;
	bool answerer_carrier_seen = false;
	uint32_t answerer_precarrier_noise = 0x42519554u;
	unsigned int answerer_silence_frames = 0;
	unsigned int answerer_silence_max = 0;
	enum DP_ID initial_datapump = uses_v8 ? DP_V8 : test_case->id;
	unsigned int training_frame_limit =
		test_case->training_frame_limit != 0 ?
			test_case->training_frame_limit :
			(v8_datapump_modulation(test_case->id) == V8_MODULATION_V32 ?
			 V32_TRAINING_FRAME_LIMIT :
			 uses_v8 ? NEGOTIATED_TRAINING_FRAME_LIMIT :
				   TRAINING_FRAME_LIMIT);
	int selection_length = (int)strlen(test_case->selection_command);
	int answerer_selection_length = (int)strlen(answerer_selection);
	static const char raw_command[] = "AT\\N0\r";
	static const char lapm_command[] = "AT\\N4\r";
	static const char no_compression_command[] = "AT+DS=0,0\r";
	bool monitor_caller_rate =
		test_case->minimum_caller_rate_signals != 0;
	bool require_lapm = test_case->require_lapm ||
		(test_case->test_retrain && !test_case->raw_transport);
	bool impair_answerer_precarrier =
		test_case->id == DP_V22 || test_case->id == DP_V22BIS ||
		test_case->id == DP_B212;

	CHECK(endpoint_init(&caller, "FSK caller") &&
		      endpoint_init(&answerer, "FSK answerer") &&
		      endpoint_prepare_media(&caller, codec) &&
		      endpoint_prepare_media(&answerer, codec) &&
		      endpoint_add_impairment(
			      &caller, test_case->impairment, codec,
			      UINT32_C(0x243f6a88)) &&
		      endpoint_add_impairment(
			      &answerer, test_case->impairment, codec,
			      UINT32_C(0x85a308d3)) &&
		      (!monitor_caller_rate ||
		       v22_rate_monitor_init(
			       &caller_rate_monitor,
			       test_case->minimum_caller_rate_signal_sample)),
	      "%s integration endpoints did not initialize", test_case->name);
	caller.robbed_bits = answerer.robbed_bits = test_case->startup_robbed_bits;
	if (v8_datapump_uses_pcm(test_case->id) ||
	    v8_datapump_uses_pcm(answerer_id)) {
		CHECK(modem_set_sreg(caller.modem, SREG_CONNNECT_MSG_FORMAT, 1) == 0 &&
		      modem_set_sreg(answerer.modem, SREG_CONNNECT_MSG_FORMAT, 1) == 0,
		      "%s did not enable negotiated PCM details", test_case->name);
		CHECK(test_case->automatic_pcm_roles || (modem_set_sreg(
			      caller.modem, SREG_V90_DIGITAL_INDICATION,
			      test_case->digital_caller) == 0 &&
		      modem_set_sreg(
			      answerer.modem,
			      SREG_V90_DIGITAL_INDICATION,
			      !test_case->digital_caller) == 0),
		      "%s integration endpoints did not accept PCM roles",
		      test_case->name);
	}
	CHECK(MODEM_DP(caller.modem) == DP_V92 &&
		      MODEM_DP(answerer.modem) == DP_V92 &&
		      MODEM_AUTOMODE(caller.modem) == 1 &&
		      MODEM_AUTOMODE(answerer.modem) == 1,
	      "source-built modem did not default to its available capability");
	if (test_case->raw_transport) {
		const char *caller_command = test_case->factory_transport && test_case->automode ?
			"AT\r" : raw_command;
		const char *answerer_command = test_case->factory_transport && test_case->answerer_automode ?
			"AT\r" : raw_command;

		CHECK(modem_write(caller.modem, caller_command, (int)strlen(caller_command)) ==
		      (int)strlen(caller_command) &&
		      modem_write(answerer.modem, answerer_command, (int)strlen(answerer_command)) ==
		      (int)strlen(answerer_command) &&
		      endpoint_drain(&caller) && endpoint_drain(&answerer) &&
		      output_contains(&caller, "OK") && output_contains(&answerer, "OK"),
		      "%s did not accept raw-peer transport", test_case->name);
		caller.output_count = 0;
		answerer.output_count = 0;
	}
	if (require_lapm) {
		CHECK(modem_write(
			      caller.modem, lapm_command,
			      (int)(sizeof(lapm_command) - 1u)) ==
			      (int)(sizeof(lapm_command) - 1u) &&
			      modem_write(
				      answerer.modem, lapm_command,
				      (int)(sizeof(lapm_command) - 1u)) ==
				      (int)(sizeof(lapm_command) - 1u) &&
			      endpoint_drain(&caller) && endpoint_drain(&answerer) &&
			      output_contains(&caller, "OK") &&
			      output_contains(&answerer, "OK"),
		      "%s did not accept its required LAPM transport",
		      test_case->name);
		caller.output_count = 0;
		answerer.output_count = 0;
	}
	if (test_case->test_retrain) {
		CHECK(modem_write(
			      caller.modem, no_compression_command,
			      (int)(sizeof(no_compression_command) - 1u)) ==
			      (int)(sizeof(no_compression_command) - 1u) &&
		      modem_write(
			      answerer.modem, no_compression_command,
			      (int)(sizeof(no_compression_command) - 1u)) ==
			      (int)(sizeof(no_compression_command) - 1u) &&
		      endpoint_drain(&caller) && endpoint_drain(&answerer) &&
		      output_contains(&caller, "OK") &&
		      output_contains(&answerer, "OK"),
		      "%s did not disable compression for the retrain test",
		      test_case->name);
		caller.output_count = 0;
		answerer.output_count = 0;
	}
	CHECK(modem_write(
		      caller.modem, test_case->selection_command,
		      selection_length) == selection_length &&
		      modem_write(
			      answerer.modem, answerer_selection,
			      answerer_selection_length) ==
			      answerer_selection_length &&
		      MODEM_DP(caller.modem) == test_case->id &&
		      MODEM_DP(answerer.modem) == answerer_id &&
		      MODEM_AUTOMODE(caller.modem) == test_case->automode &&
		      MODEM_AUTOMODE(answerer.modem) ==
			      (!test_case->answerer_fixed &&
			       (test_case->automode || test_case->answerer_automode)) &&
		      endpoint_drain(&caller) && endpoint_drain(&answerer) &&
		      output_contains(&caller, "OK") &&
		      output_contains(&answerer, "OK"),
	      "%s was not selectable through AT+MS", test_case->name);
	caller.output_count = 0;
	answerer.output_count = 0;
	CHECK(modem_write(answerer.modem, auto_answer_command,
			  (int)(sizeof(auto_answer_command) - 1)) ==
		      (int)(sizeof(auto_answer_command) - 1) &&
		      modem_set_sreg(answerer.modem, SREG_ANS_DELAY, 0) == 0 &&
		      modem_write(caller.modem, dial_command,
			  (int)(sizeof(dial_command) - 1)) ==
			      (int)(sizeof(dial_command) - 1) &&
		      caller.modem->dp != NULL &&
		      caller.modem->dp->id == DP_CALLPROG,
	      "%s outgoing SIP call did not enter call progress",
	      test_case->name);
	CHECK(modem_report_outgoing_call_connected(caller.modem) == 0 &&
		      caller.modem->dp != NULL &&
		      caller.modem->dp->id == initial_datapump &&
		      modem_report_incoming_call(answerer.modem) == 0 &&
		      answerer.modem->started,
	      "connected SIP call did not select %s roles", test_case->name);
	CHECK(test_case->answer_tone_prefix_ms == 0 ||
	      present_legacy_answer_tone(&caller, codec,
		      test_case->answer_tone_prefix_ms,
		      test_case->answer_tone_prefix_modulated),
	      "%s did not retain automatic startup through the answer preamble", test_case->name);
	CHECK(!impair_answerer_precarrier ||
		      endpoint_replace_precarrier_silence(
			      &answerer, codec,
			      &answerer_precarrier_noise,
			      &answerer_carrier_seen),
	      "%s answerer pre-carrier noise could not be represented",
	      test_case->name);

	for (unsigned int frame = 0; frame < training_frame_limit; ++frame) {
		CHECK(process_test_frame(
			      &caller, &answerer, codec, &recorder,
			      &caller_delay,
			      test_case->caller_delay_frames) &&
		      endpoint_drain(&caller) && endpoint_drain(&answerer) &&
		      (!monitor_caller_rate ||
		       v22_rate_monitor_observe(
			       &caller_rate_monitor, caller.transmit)),
		      "%s training failed at the SIP media boundary",
		      test_case->name);

		CHECK(!impair_answerer_precarrier ||
			      endpoint_replace_precarrier_silence(
				      &answerer, codec,
				      &answerer_precarrier_noise,
				      &answerer_carrier_seen),
		      "%s answerer pre-carrier noise failed",
		      test_case->name);
		if (answerer.modem->dp != NULL &&
		    v8_datapump_modulation(answerer.modem->dp->id) == V8_MODULATION_V32) {
			bool answerer_silent = true;

			for (size_t sample = 0;
			     sample < ARRAY_LENGTH(answerer.transmit); ++sample)
				if (answerer.transmit[sample] != 0) {
					answerer_silent = false;
					break;
				}
			if (answerer_silent && answerer_signal_seen) {
				answerer_silence_frames++;
			} else if (!answerer_silent) {
				answerer_signal_seen = true;
				if (answerer_silence_frames > answerer_silence_max)
					answerer_silence_max =
						answerer_silence_frames;
				answerer_silence_frames = 0;
			}
		}
		if (answerer.modem->dp != NULL &&
		    answerer.modem->dp->id == DP_V8)
			answerer_used_v8 = true;
		if (test_case->test_prelink && !prelink_queued &&
		    modem_is_online(answerer.modem, test_case->id) &&
		    !modem_is_online(caller.modem, test_case->id)) {
			CHECK(modem_write(
				      answerer.modem, prelink_payload,
				      (int)(sizeof(prelink_payload) - 1)) ==
				      (int)(sizeof(prelink_payload) - 1),
			      "%s answerer did not accept pre-link data",
			      test_case->name);
			prelink_queued = true;
		}
		if (modem_is_online(caller.modem, connected_id) &&
		    modem_is_online(answerer.modem, answerer_connected_id)) {
			online = true;
			break;
		}
	}
	if (answerer_silence_frames > answerer_silence_max)
		answerer_silence_max = answerer_silence_frames;
	CHECK(online && answerer_used_v8 &&
		      caller.modem->tx_rate == test_case->caller_tx_rate &&
		      caller.modem->rx_rate == test_case->caller_rx_rate &&
		      answerer.modem->tx_rate == test_case->answerer_tx_rate &&
		      answerer.modem->rx_rate == test_case->answerer_rx_rate,
	      "%s peers did not acquire carrier and report their rates "
	      "(caller=%d/%u/%u answerer=%d/%u/%u)",
	      test_case->name,
	      modem_is_online(caller.modem, connected_id),
	      caller.modem->tx_rate, caller.modem->rx_rate,
	      modem_is_online(answerer.modem, answerer_connected_id),
	      answerer.modem->tx_rate, answerer.modem->rx_rate);
	{
		struct dp_link_rates c = datapump_get_link_rates(caller.modem->dp);
		struct dp_link_rates a = datapump_get_link_rates(answerer.modem->dp);

		CHECK(c.transmit_bps == caller.modem->tx_rate &&
		      c.receive_bps == caller.modem->rx_rate &&
		      a.transmit_bps == answerer.modem->tx_rate &&
		      a.receive_bps == answerer.modem->rx_rate &&
		      c.transmit_baud > 0 && c.receive_baud > 0 &&
		      c.transmit_baud == a.receive_baud && c.receive_baud == a.transmit_baud,
		      "%s diagnostics lost negotiated or reciprocal rates", test_case->name);
	}
	CHECK(!monitor_caller_rate ||
		      caller_rate_monitor.starts >=
			      test_case->minimum_caller_rate_signals,
	      "%s caller emitted only %u late V.22bis rate signal(s)",
	      test_case->name, caller_rate_monitor.starts);
	CHECK(v8_datapump_modulation(test_case->id) != V8_MODULATION_V32 ||
		      answerer_silence_max >=
			      V32_MIN_HALF_DUPLEX_GAP_FRAMES,
	      "%s answerer did not cease transmission while conditioning its "
	      "receiver (longest gap=%u frames)",
	      test_case->name, answerer_silence_max);
	CHECK(!test_case->test_prelink || prelink_queued,
	      "%s did not expose its asymmetric pre-link interval",
	      test_case->name);
	for (unsigned int frame = 0; frame < LINK_DRAIN_FRAME_LIMIT; ++frame) {
		CHECK(endpoint_drain(&caller) && endpoint_drain(&answerer),
		      "%s connection output could not be read", test_case->name);
		if (!test_case->test_prelink ||
		    output_contains(&caller, prelink_payload))
			prelink_delivered = true;
		if (prelink_delivered &&
		    output_contains_connect(
			    &caller, test_case->caller_rx_rate) &&
		    output_contains_connect(
			    &answerer, test_case->answerer_rx_rate))
			break;
		CHECK(process_test_frame(
			      &caller, &answerer, codec, &recorder,
			      &caller_delay, test_case->caller_delay_frames),
		      "%s pre-link delivery failed at the SIP media boundary",
		      test_case->name);
	}
	CHECK(prelink_delivered &&
		      output_contains_connect(
			      &caller, test_case->caller_rx_rate) &&
		      output_contains_connect(
			      &answerer, test_case->answerer_rx_rate),
	      "%s connection did not reach both terminal interfaces "
	      "(caller state=%u ec=%u lapm=%d xid=%d control pending=%d timer=%d; "
	      "answerer state=%u ec=%u lapm=%d xid=%d control pending=%d timer=%d)",
	      test_case->name, caller.modem->state, caller.modem->cfg.ec,
	      caller.modem->ec.lapm.state, caller.modem->ec.lapm.v44_session.phase,
	      caller.modem->ec.lapm.tx_ctrl != caller.modem->ec.lapm.ctrl_list,
	      caller.modem->bit_timer,
	      answerer.modem->state, answerer.modem->cfg.ec,
	      answerer.modem->ec.lapm.state,
	      answerer.modem->ec.lapm.v44_session.phase,
	      answerer.modem->ec.lapm.tx_ctrl != answerer.modem->ec.lapm.ctrl_list,
	      answerer.modem->bit_timer);
	if (connected_id == DP_V91) {
		CHECK(output_contains(&caller, "Modulation: V91\r\n") &&
		      output_contains(&caller, "Startup: V8\r\n") &&
		      output_contains(&caller, "TxRate:") &&
		      output_contains(&answerer, "Modulation: V91\r\n"),
		      "%s lost negotiated AT&A7 details", test_case->name);
		fprintf(stderr, "[ CONNECT  ] %s caller over %s:\n%.*s\n",
			test_case->name, codec == MEDIA_CODEC_PCMU ? "PCMU" : "PCMA",
			(int)caller.output_count, caller.output);
	}
	if (connected_id == DP_V90 || connected_id == DP_V92) {
		const char *upstream = connected_id == DP_V92 && !test_case->conventional_upstream ?
			"Upstream: PCM\r\n" : "Upstream: V34\r\n";
		CHECK(output_contains(&caller, upstream) && output_contains(&answerer, upstream),
		      "%s did not report negotiated upstream to both terminals", test_case->name);
	} else {
		CHECK(!output_contains(&caller, "Upstream:") &&
		      !output_contains(&answerer, "Upstream:"),
		      "%s leaked a PCM upstream report to another modulation", test_case->name);
	}
	caller.output_count = 0;
	answerer.output_count = 0;
	CHECK(process_test_frame(
		      &caller, &answerer, codec, &recorder, &caller_delay,
		      test_case->caller_delay_frames) &&
		      endpoint_drain(&caller) && endpoint_drain(&answerer) &&
		      caller.output_count == 0 && answerer.output_count == 0,
	      "%s idle link emitted terminal data "
	      "(caller=%zu:%02x answerer=%zu:%02x)",
	      test_case->name, caller.output_count,
	      caller.output_count != 0 ? caller.output[0] : 0,
	      answerer.output_count,
	      answerer.output_count != 0 ? answerer.output[0] : 0);

	CHECK(modem_write(caller.modem, (const char *)caller_payload,
			  (int)sizeof(caller_payload)) ==
		      (int)sizeof(caller_payload) &&
		      modem_write(answerer.modem, (const char *)answer_payload,
			  (int)sizeof(answer_payload)) ==
		      (int)sizeof(answer_payload),
	      "%s terminals did not accept full-duplex payloads",
	      test_case->name);
	for (unsigned int frame = 0; frame < DATA_FRAME_LIMIT; ++frame) {
		CHECK(process_test_frame(
			      &caller, &answerer, codec, &recorder,
			      &caller_delay,
			      test_case->caller_delay_frames) &&
			      endpoint_drain(&caller) && endpoint_drain(&answerer),
		      "%s data failed at the SIP media boundary",
		      test_case->name);
		if (caller.output_count >= sizeof(answer_payload) &&
		    answerer.output_count >= sizeof(caller_payload)) {
			transferred = true;
			break;
		}
	}
	CHECK(transferred && caller.output_count == sizeof(answer_payload) &&
		      answerer.output_count == sizeof(caller_payload) &&
		      memcmp(caller.output, answer_payload,
			     sizeof(answer_payload)) == 0 &&
		      memcmp(answerer.output, caller_payload,
			     sizeof(caller_payload)) == 0,
	      "%s full-duplex terminal payload changed in transit "
	      "(caller=%zu:%02x answerer=%zu:%02x)",
	      test_case->name, caller.output_count,
	      caller.output_count != 0 ? caller.output[0] : 0,
	      answerer.output_count,
	      answerer.output_count != 0 ? answerer.output[0] : 0);
	CHECK(!require_lapm ||
		      (caller.modem->cfg.ec && answerer.modem->cfg.ec),
	      "%s did not retain its required LAPM transport", test_case->name);
	CHECK(!test_case->require_v44 ||
	      (modem_ec_active_compression(caller.modem) == MODEM_EC_COMPRESSION_V44 &&
	       modem_ec_active_compression(answerer.modem) == MODEM_EC_COMPRESSION_V44),
	      "%s did not negotiate V.44 in both directions", test_case->name);
	if (connected_id == DP_V92 && !test_case->raw_transport)
		CHECK(exercise_v92_rate_exchange(&caller, &answerer, codec, &recorder),
		      "%s rate exchange did not preserve outstanding LAPM data", test_case->name);
	if (test_case->test_retrain) {
		if (connected_id == DP_V34 && test_case->raw_transport)
			CHECK(exercise_v34_media_gap(&caller, &answerer, codec, &recorder),
			      "%s did not recover after a short media gap", test_case->name);
		CHECK(caller.modem->cfg.ec == !test_case->raw_transport &&
		      answerer.modem->cfg.ec == !test_case->raw_transport,
		      "%s did not retain its requested transport", test_case->name);
		CHECK(exercise_retrain(
				      &caller, &answerer, &answerer, connected_id,
				      test_case->answerer_rx_rate, codec, &recorder,
				      &caller_delay,
				      test_case->caller_delay_frames, 0,
				      test_case->raw_transport,
				      test_case->id == DP_V22BIS &&
					      test_case->raw_transport &&
					      codec == MEDIA_CODEC_PCMU,
				      false, -1),
		      "%s answerer retrain did not reconnect and pass data",
		      test_case->name);
		CHECK(exercise_retrain(
				      &caller, &answerer, &caller, connected_id,
				      test_case->caller_rx_rate, codec, &recorder,
				      &caller_delay,
				      test_case->caller_delay_frames, 1,
				      test_case->raw_transport, false,
				      test_case->id == DP_V22BIS, -1),
		      "%s caller retrain did not reconnect and pass data",
		      test_case->name);
		/* An eager peer can begin its reciprocal S1 after the stable 20 ms
		 * prefix, while our complete 100 ms S1 is still in flight. */
		if (test_case->id == DP_V22BIS)
			CHECK(exercise_retrain(
				      &caller, &answerer, &caller, test_case->id,
				      test_case->caller_rx_rate, codec, &recorder,
				      &caller_delay,
				      test_case->caller_delay_frames, 0,
				      test_case->raw_transport, false, false, 1),
			      "%s overlapping retrain did not reconnect and pass data",
			      test_case->name);
		if (test_case->id == DP_V22BIS)
			CHECK(exercise_low_speed_carrier_recovery(
					      &caller, &answerer, test_case->id,
					      codec, &recorder,
					      test_case->raw_transport),
					      "%s carrier recovery did not retrain "
					      "and pass data",
					      test_case->name);
	}
	if (preserves_link_across_carrier_return(test_case->id))
		CHECK(exercise_low_speed_carrier_recovery(
				      &caller, &answerer, test_case->id,
				      codec, &recorder,
				      test_case->raw_transport),
			      "%s did not preserve the link and pass data after "
			      "carrier reacquisition",
			      test_case->name);
	if ((test_case->id == DP_V21 || test_case->id == DP_B103) &&
	    test_case->raw_transport)
		CHECK(exercise_fsk_data_carrier_recovery(
			      &caller, &answerer, test_case->id, codec, &recorder),
		      "%s did not recover carrier during continuous data", test_case->name);
	CHECK(!capture.invalid &&
		      capture.blocks[RECORD_TX_LINE_PCM] != 0 &&
		      capture.blocks[
			      RECORD_TX_RTP_PAYLOAD_PREPARED] != 0 &&
		      capture.blocks[
			      RECORD_RX_RTP_PAYLOAD_PLAYOUT] != 0 &&
		      capture.blocks[RECORD_RX_LINE_PCM] != 0,
	      "%s SIP call did not retain recording boundaries",
	      test_case->name);
	CHECK(!test_case->requires_exact_g711 ||
		      (caller.exact_frames != 0 && answerer.exact_frames != 0),
	      "%s did not exercise exact G.711 media in both directions",
	      test_case->name);
	if (test_case->carrier_loss_samples != 0) {
		CHECK(exercise_transmit_completion(&caller, &answerer, codec, &recorder,
			      &caller_delay, test_case->caller_delay_frames),
		      "%s lost its final message", test_case->name);
		CHECK(exercise_carrier_loss_limit(
			      &caller, connected_id,
			      test_case->carrier_loss_samples),
		      "%s carrier loss did not honor the fragmented S10 boundary",
		      test_case->name);
	}
	/* Automatic quality recovery can lower the upstream rate. Exercise it
	 * after the fixed-rate ATO1 checks, before protocol cleardown. */
	if (connected_id == DP_V92 && !test_case->raw_transport) {
		CHECK(exercise_v92_media_loss(&caller, &answerer, codec, &recorder),
		      "%s media loss did not preserve outstanding LAPM data", test_case->name);
		CHECK(exercise_pcm_sample_slip(&caller, &answerer, codec, &recorder, false),
		      "%s upstream slip did not preserve outstanding LAPM data", test_case->name);
	}
	if ((connected_id == DP_V90 || connected_id == DP_V92) && !test_case->raw_transport)
		CHECK(exercise_pcm_sample_slip(&caller, &answerer, codec, &recorder, true),
		      "%s downstream slip did not preserve outstanding LAPM data", test_case->name);
	if (connected_id == DP_V91) {
		CHECK(exercise_v91_payload(&caller, &answerer, codec, &recorder),
		      "%s sustained payload failed", test_case->name);
		if (!test_case->raw_transport && test_case->caller_rx_rate == 64000) {
			CHECK(exercise_v91_recovery(&caller, &answerer, codec, &recorder, false) &&
			      exercise_v91_recovery(&caller, &answerer, codec, &recorder, true),
			      "%s automatic recovery failed", test_case->name);
		}
	}
	if (test_case->carrier_loss_samples == 0)
		CHECK(exercise_transmit_completion(&caller, &answerer, codec, &recorder,
			      &caller_delay, test_case->caller_delay_frames),
		      "%s lost its final message", test_case->name);
	if (test_case->cleardown_initiators)
		CHECK(exercise_cleardown(
			      &caller, &answerer, connected_id, test_case->cleardown_initiators, codec, &recorder),
		      "%s did not complete protocol ATH0 cleardown", test_case->name);

	/* CPU time inside modem_process_g711 excludes media plumbing and test work. */
	if (caller.profile_cpu && caller.process_ns != 0u && answerer.process_ns != 0u)
		fprintf(stderr, "[ CPU      ] %s over %s: caller %.2fx, answerer %.2fx (%zu/%zu samples)\n",
			test_case->name, codec == MEDIA_CODEC_PCMU ? "PCMU" : "PCMA",
			(double)caller.process_samples * 1000000000.0 /
				(MODEM_RATE * (double)caller.process_ns),
			(double)answerer.process_samples * 1000000000.0 /
				(MODEM_RATE * (double)answerer.process_ns),
			caller.process_samples, answerer.process_samples);

	endpoint_destroy(&answerer);
	endpoint_destroy(&caller);
	return true;
}

static bool frame_has_signal(const int16_t samples[SIP_FRAME_SAMPLES])
{
	for (size_t index = 0; index < SIP_FRAME_SAMPLES; ++index)
		if (samples[index] != 0)
			return true;
	return false;
}

static bool run_baudot_media_call(
	enum baudot_rate rate,
	enum media_codec codec)
{
	static const char request[] = "hello 123? ga";
	static const char expected_request[] = "HELLO 123? GA";
	static const char reply[] = "ok ga";
	static const char expected_reply[] = "OK GA";
	static const char dial_command[] = "ATDpeer\r";
	const char *selection_command =
		rate == BAUDOT_RATE_50 ?
			"AT+MS=45,0,50,50\r" : "AT+MS=45,0,45,45\r";
	struct integration_endpoint caller;
	struct integration_endpoint answerer;
	struct recording_capture capture = { 0 };
	const struct media_recorder recorder = {
		.context = &capture,
		.write = capture_recording,
	};
	bool online = false;
	bool request_started = false;
	bool request_received = false;
	bool reply_queued = false;
	bool reply_received = false;
	unsigned int silent_request_frames = 0;
	int reported_rate = rate == BAUDOT_RATE_50 ? 50 : 45;
	int selection_length = (int)strlen(selection_command);

	CHECK(endpoint_init(&caller, "Baudot caller") &&
		      endpoint_init(&answerer, "Baudot answerer") &&
		      endpoint_prepare_media(&caller, codec) &&
		      endpoint_prepare_media(&answerer, codec),
	      "Baudot SIP integration did not initialize");
	CHECK(modem_write(
		      caller.modem, selection_command, selection_length) ==
		      selection_length &&
		      modem_write(
			      answerer.modem, selection_command,
			      selection_length) == selection_length &&
		      MODEM_DP(caller.modem) == DP_BAUDOT &&
		      MODEM_DP(answerer.modem) == DP_BAUDOT &&
		      endpoint_drain(&caller) && endpoint_drain(&answerer) &&
		      output_contains(&caller, "OK") &&
		      output_contains(&answerer, "OK"),
	      "Baudot was not selectable through AT+MS");
	caller.output_count = 0;
	answerer.output_count = 0;
	CHECK(modem_set_sreg(answerer.modem, SREG_ANS_DELAY, 0) == 0 &&
		      modem_write(caller.modem, dial_command,
			  (int)(sizeof(dial_command) - 1u)) ==
			      (int)(sizeof(dial_command) - 1u) &&
		      modem_report_outgoing_call_connected(caller.modem) == 0 &&
		      modem_answer(answerer.modem) == 0,
	      "Baudot SIP call did not establish its media roles");
	for (unsigned int frame = 0; frame < TRAINING_FRAME_LIMIT; ++frame) {
		CHECK(process_sip_frame(
		      &caller, &answerer, codec, &recorder),
		      "Baudot link failed at the SIP media boundary");
		if (modem_is_online(caller.modem, DP_BAUDOT) &&
		    modem_is_online(answerer.modem, DP_BAUDOT)) {
			online = true;
			break;
		}
	}
	CHECK(online && caller.modem->tx_rate == (unsigned int)reported_rate &&
		      caller.modem->rx_rate == (unsigned int)reported_rate &&
		      answerer.modem->tx_rate == (unsigned int)reported_rate &&
		      answerer.modem->rx_rate == (unsigned int)reported_rate &&
		      endpoint_drain(&caller) && endpoint_drain(&answerer) &&
		      output_contains_connect(&caller, (unsigned int)reported_rate) &&
		      output_contains_connect(&answerer, (unsigned int)reported_rate),
	      "Baudot peers did not report their selected rate");
	caller.output_count = 0;
	answerer.output_count = 0;
	CHECK(modem_write(
		      caller.modem, request, (int)(sizeof(request) - 1u)) ==
		      (int)(sizeof(request) - 1u),
	      "Baudot caller did not accept terminal text");

	for (unsigned int frame = 0; frame < DATA_FRAME_LIMIT; ++frame) {
		bool caller_active;
		bool answerer_active;

		CHECK(process_sip_frame(
		      &caller, &answerer, codec, &recorder) &&
			      endpoint_drain(&caller) && endpoint_drain(&answerer),
		      "Baudot text failed at the SIP media boundary");
		caller_active = frame_has_signal(caller.transmit);
		answerer_active = frame_has_signal(answerer.transmit);
		CHECK(!(caller_active && answerer_active),
		      "Baudot peers transmitted over one another");
		if (caller_active) {
			request_started = true;
			silent_request_frames = 0;
		} else if (request_started && silent_request_frames != UINT_MAX) {
			silent_request_frames++;
		}
		request_received =
			answerer.output_count == sizeof(expected_request) - 1u &&
			memcmp(answerer.output, expected_request,
			       sizeof(expected_request) - 1u) == 0;
		if (request_received && silent_request_frames >= 2 &&
		    !reply_queued) {
			CHECK(modem_write(
			      answerer.modem, reply,
			      (int)(sizeof(reply) - 1u)) ==
				      (int)(sizeof(reply) - 1u),
			      "Baudot answerer did not accept its reply");
			reply_queued = true;
		}
		if (reply_queued &&
		    caller.output_count == sizeof(expected_reply) - 1u &&
		    memcmp(caller.output, expected_reply,
			   sizeof(expected_reply) - 1u) == 0) {
			reply_received = true;
			break;
		}
	}
	CHECK(request_received && reply_received && !capture.invalid &&
		      capture.blocks[RECORD_TX_LINE_PCM] != 0 &&
		      capture.blocks[
			      RECORD_TX_RTP_PAYLOAD_PREPARED] != 0 &&
		      capture.blocks[
			      RECORD_RX_RTP_PAYLOAD_PLAYOUT] != 0 &&
		      capture.blocks[RECORD_RX_LINE_PCM] != 0,
	      "Baudot SIP turn-taking or recording evidence was incomplete");
	endpoint_destroy(&answerer);
	endpoint_destroy(&caller);
	return true;
}

static const char *codec_name(enum media_codec codec)
{
	return codec == MEDIA_CODEC_PCMU ? "PCMU" : "PCMA";
}

static bool run_v34_rate_matrix(const char *filter)
{
	static const enum media_codec codecs[] = {
		MEDIA_CODEC_PCMU,
		MEDIA_CODEC_PCMA,
	};
	static const struct {
		enum v34_symbol_rate selection;
		unsigned int symbols_per_second;
		unsigned int maximum_bit_rate;
	} symbol_rates[] = {
		{ V34_SYMBOL_RATE_2400, 2400, 21600 },
		{ V34_SYMBOL_RATE_2743, 2743, 26400 },
		{ V34_SYMBOL_RATE_2800, 2800, 26400 },
		{ V34_SYMBOL_RATE_3000, 3000, 28800 },
		{ V34_SYMBOL_RATE_3200, 3200, 31200 },
		{ V34_SYMBOL_RATE_3429, 3429, 33600 },
	};
	unsigned int profiles = 0;
	unsigned int cases = 0;

	for (size_t symbol = 0; symbol < ARRAY_LENGTH(symbol_rates); ++symbol) {
		for (unsigned int rate = V34_MIN_BIT_RATE;
		     rate <= symbol_rates[symbol].maximum_bit_rate;
		     rate += V34_RATE_STEP) {
			struct v34_frame_geometry geometry;
			struct modem_test_case test_case = {
				.id = DP_V34,
				.caller_tx_rate = rate,
				.caller_rx_rate = rate,
				.answerer_tx_rate = rate,
				.answerer_rx_rate = rate,
				.carrier_loss_samples = MODEM_RATE / 2,
				.training_frame_limit = 1000,
				.requires_v8 = true,
			};
			char name[64];
			char command[64];
			int name_length;
			int command_length;

			if (v34_frame_geometry_init(
				    &geometry, symbol_rates[symbol].selection,
				    (uint8_t)(rate / V34_RATE_STEP), false) !=
			    V34_FRAME_SCHEDULE_OK)
				continue;
			name_length = snprintf(
				name, sizeof(name), "V.34 %u at %u symbol/s", rate,
				symbol_rates[symbol].symbols_per_second);
			if (filter != NULL && filter[0] != '\0' &&
			    strstr(name, filter) == NULL)
				continue;
			command_length = snprintf(
				command, sizeof(command),
				"AT+MS=34,0,%u,%u+V34B=%u\r", rate, rate,
				symbol_rates[symbol].symbols_per_second);
			CHECK(name_length > 0 &&
				      (size_t)name_length < sizeof(name) &&
				      command_length > 0 &&
				      (size_t)command_length < sizeof(command),
			      "V.34 matrix command formatting overflowed");
			test_case.name = name;
			test_case.selection_command = command;
			profiles++;
			for (size_t codec = 0; codec < ARRAY_LENGTH(codecs); ++codec) {
				fprintf(stderr, "[ RUN      ] %s over %s SIP media\n",
					name, codec_name(codecs[codec]));
				if (!run_media_call(&test_case, codecs[codec])) {
					fprintf(stderr,
						"[  FAILED  ] %s over %s SIP media\n",
						name, codec_name(codecs[codec]));
					return false;
				}
				fprintf(stderr, "[       OK ] %s over %s SIP media\n",
					name, codec_name(codecs[codec]));
				cases++;
			}
		}
	}
	CHECK((filter == NULL || filter[0] == '\0') ?
		      profiles == 65 && cases == 130 :
		      profiles != 0 && cases == profiles * 2u,
	      "V.34 matrix covered an unexpected %u profiles/%u cases",
	      profiles, cases);
	return true;
}

static bool configure_recording_replay(
	struct integration_endpoint *endpoint,
	const char *const *commands, size_t command_count)
{
	for (size_t index = 0; index < command_count; ++index) {
		int length = (int)strlen(commands[index]);

		endpoint->output_count = 0;
		if (modem_write(endpoint->modem, commands[index], length) !=
			    length ||
		    !endpoint_drain(endpoint) ||
		    !output_contains(endpoint, "OK"))
			return false;
	}
	endpoint->output_count = 0;
	return true;
}

struct v22_signal_generator {
	struct qam_modulator modulator;
	struct v22_symbol_encoder encoder;
	uint8_t quadrant;
};

static bool v22_signal_generator_init(
	struct v22_signal_generator *generator, uint32_t carrier_hz)
{
	struct qam_channel channel = {
		.sample_rate = MODEM_RATE,
		.symbol_rate_numerator = 600,
		.symbol_rate_denominator = 1,
		.carrier_frequency_numerator = carrier_hz,
		.carrier_frequency_denominator = 1,
		.rolloff = 0.75,
		.pulse_span_symbols = 6,
	};

	if (generator == NULL)
		return false;
	memset(generator, 0, sizeof(*generator));
	return qam_modulator_init(
		       &generator->modulator, &channel, 12000) == QAM_OK &&
	       v22_symbol_encoder_init(&generator->encoder, 0) ==
		       V22_FRAMING_OK;
}

static bool v22_signal_generate(
	struct v22_signal_generator *generator, int16_t *samples,
	size_t count, bool scrambled)
{
	static const struct qam_point quadrants[] = {
		{ 1.0, 0.0 },
		{ 0.0, 1.0 },
		{ -1.0, 0.0 },
		{ 0.0, -1.0 },
	};

	if (generator == NULL || samples == NULL)
		return false;
	for (size_t index = 0; index < count; ++index) {
		bool can_accept = qam_modulator_can_accept_symbol(
			&generator->modulator);
		bool accepted;

		if (can_accept && scrambled &&
		    v22_symbol_encode(
			    &generator->encoder, 1, 1,
			    &generator->quadrant, true) != V22_FRAMING_OK)
			return false;
		if (can_accept && !scrambled)
			generator->quadrant =
				(uint8_t)((generator->quadrant + 3u) & 3u);
		if (qam_modulator_sample(
			    &generator->modulator,
			    &quadrants[generator->quadrant], &samples[index],
			    &accepted) != QAM_OK ||
		    accepted != can_accept)
			return false;
	}
	return true;
}

static bool start_v22_answer_replay(
	struct integration_endpoint *endpoint);

static bool run_v22_answer_response_deadline(void)
{
	enum {
		V22_PROBE_START_SAMPLES = MODEM_RATE / 2,
		V22_RESPONSE_MIN_SAMPLES = MODEM_RATE * 230 / 1000,
		V22_RESPONSE_MAX_SAMPLES = MODEM_RATE * 310 / 1000,
		V22_RESPONSE_TEST_SAMPLES =
			V22_PROBE_START_SAMPLES + V22_RESPONSE_MAX_SAMPLES,
	};
	struct integration_endpoint endpoint;
	struct v22_signal_generator probe;
	struct v22_signal_generator answer_mark;
	int16_t expected[SIP_FRAME_SAMPLES];
	size_t response_at = SIZE_MAX;

	CHECK(start_v22_answer_replay(&endpoint) &&
		      v22_signal_generator_init(&probe, 1200) &&
		      v22_signal_generator_init(&answer_mark, 2400),
	      "V.22 answer-response replay did not initialize");
	memset(endpoint.receive, 0, sizeof(endpoint.receive));
	modem_process(
		endpoint.modem, endpoint.receive, endpoint.transmit,
		SIP_FRAME_SAMPLES);
	CHECK(endpoint_drain(&endpoint),
	      "V.22 answer-response delay did not complete");
	endpoint.output_count = 0;
	for (size_t offset = 0; offset < V22_RESPONSE_TEST_SAMPLES;
	     offset += SIP_FRAME_SAMPLES) {
		size_t count = V22_RESPONSE_TEST_SAMPLES - offset;

		if (count > SIP_FRAME_SAMPLES)
			count = SIP_FRAME_SAMPLES;
		if (offset < V22_PROBE_START_SAMPLES) {
			memset(endpoint.receive, 0,
			       count * sizeof(*endpoint.receive));
		} else {
			CHECK(v22_signal_generate(
				      &probe, endpoint.receive, count, true),
			      "V.22 calling-probe generation failed");
		}
		modem_process(
			endpoint.modem, endpoint.receive, endpoint.transmit,
			(int)count);
		CHECK(v22_signal_generate(
			      &answer_mark, expected, count, false) &&
			      endpoint_drain(&endpoint),
		      "V.22 answer-response replay failed");
		for (size_t index = 0; index < count; ++index) {
			if (endpoint.transmit[index] == expected[index])
				continue;
			CHECK(offset + index >= V22_PROBE_START_SAMPLES,
			      "V.22 answerer left mark before the calling probe");
			if (response_at == SIZE_MAX)
				response_at = offset + index -
					V22_PROBE_START_SAMPLES;
		}
	}
	CHECK(response_at >= V22_RESPONSE_MIN_SAMPLES &&
		      response_at <= V22_RESPONSE_MAX_SAMPLES,
	      "V.22 answer response missed its 270 +/- 40 ms window (sample %zu)",
	      response_at);

	endpoint_destroy(&endpoint);
	return true;
}

static bool start_v22_answer_replay(
	struct integration_endpoint *endpoint)
{
	static const char *const commands[] = {
		"ATZ\r",
		"ATE0\r",
		"ATX3\r",
		"AT\\N0\r",
		"AT+DS=0,0\r",
		"AT+MS=22,0,1200,1200\r",
	};

	return endpoint_init(endpoint, "V.22 answer replay") &&
	       configure_recording_replay(
		       endpoint, commands, ARRAY_LENGTH(commands)) &&
	       modem_set_sreg(endpoint->modem, SREG_ANS_DELAY, 0) == 0 &&
	       modem_report_incoming_call(endpoint->modem) == 0 &&
	       endpoint_drain(endpoint) && output_contains(endpoint, "RING") &&
	       modem_answer(endpoint->modem) == 0;
}

static bool run_v22_abandoned_probe(void)
{
	enum {
		V22_PROBE_ACTIVE_SAMPLES = MODEM_RATE * 4 / 5,
		V22_PROBE_TOTAL_SAMPLES = MODEM_RATE * 2,
	};
	struct integration_endpoint endpoint;
	struct v22_signal_generator generator;
	bool connected = false;

	CHECK(start_v22_answer_replay(&endpoint) &&
		      v22_signal_generator_init(&generator, 1200),
	      "V.22 abandoned-probe replay did not initialize");
	endpoint.output_count = 0;
	for (size_t offset = 0; offset < V22_PROBE_TOTAL_SAMPLES;
	     offset += SIP_FRAME_SAMPLES) {
		size_t count = V22_PROBE_TOTAL_SAMPLES - offset;

		if (count > SIP_FRAME_SAMPLES)
			count = SIP_FRAME_SAMPLES;
		if (offset < V22_PROBE_ACTIVE_SAMPLES) {
			size_t active = V22_PROBE_ACTIVE_SAMPLES - offset;

			if (active > count)
				active = count;
			CHECK(v22_signal_generate(
				      &generator, endpoint.receive, active, true),
			      "V.22 probe generation failed");
			memset(endpoint.receive + active, 0,
			       (count - active) * sizeof(*endpoint.receive));
		} else {
			memset(endpoint.receive, 0,
			       count * sizeof(*endpoint.receive));
		}
		modem_process(
			endpoint.modem, endpoint.receive, endpoint.transmit,
			(int)count);
		CHECK(endpoint_drain(&endpoint),
		      "V.22 abandoned-probe terminal output overflowed");
		connected = connected ||
			modem_is_online(endpoint.modem, DP_V22);
	}
	CHECK(!connected,
	      "V.22 answerer connected after the calling probe disappeared");

	endpoint_destroy(&endpoint);
	return true;
}

static bool run_v22_mark_replay(unsigned int bit_rate, bool answerer,
	enum g711_law law, bool switch_to_scrambled)
{
	char selection[64];
	const char *commands[] = {
		"ATZ\r", "ATE0\r", "ATX3\r", "AT\\N0\r", "AT+DS=0,0\r", selection,
	};
	struct integration_endpoint endpoint;
	struct v22_signal_generator generator;
	bool connected = false;

	snprintf(selection, sizeof(selection), "AT+MS=22,0,%u,%u\r", bit_rate, bit_rate);
	CHECK(endpoint_init(&endpoint, "V.22 mark replay") &&
	      configure_recording_replay(&endpoint, commands, ARRAY_LENGTH(commands)) &&
	      v22_signal_generator_init(&generator, answerer ? 1200 : 2400),
	      "V.22 mark replay did not initialize");
	if (answerer) {
		CHECK(modem_set_sreg(endpoint.modem, SREG_ANS_DELAY, 0) == 0 &&
		      modem_report_incoming_call(endpoint.modem) == 0 &&
		      endpoint_drain(&endpoint) && modem_answer(endpoint.modem) == 0,
		      "V.22 mark replay did not answer");
	} else {
		static const char dial[] = "ATDrecording\r";
		CHECK(modem_write(endpoint.modem, dial, sizeof(dial) - 1) == sizeof(dial) - 1 &&
		      modem_report_outgoing_call_connected(endpoint.modem) == 0,
		      "V.22 mark replay did not originate");
	}
	for (size_t offset = 0; offset < MODEM_RATE * 4u; offset += SIP_FRAME_SAMPLES) {
		CHECK(v22_signal_generate(&generator, endpoint.receive, SIP_FRAME_SAMPLES,
		      switch_to_scrambled && offset >= MODEM_RATE), "V.22 mark generation failed");
		for (size_t index = 0; index < SIP_FRAME_SAMPLES; ++index)
			endpoint.receive[index] = g711_quantize(law, endpoint.receive[index]);
		modem_process(endpoint.modem, endpoint.receive, endpoint.transmit, SIP_FRAME_SAMPLES);
		CHECK(endpoint_drain(&endpoint), "V.22 mark replay output overflowed");
		connected |= modem_is_online(endpoint.modem, DP_V22);
	}
	CHECK(connected == switch_to_scrambled && endpoint.modem->started &&
	      endpoint.modem->dp != NULL && endpoint.modem->dp->id == DP_V22,
	      "V.22 %u mark qualification failed (answer=%u law=%u scrambled=%u connected=%u)",
	      bit_rate, answerer, law, switch_to_scrambled, connected);
	endpoint_destroy(&endpoint);
	return true;
}

static bool run_v22_mark_qualification(void)
{
	static const enum g711_law laws[] = { G711_LAW_PCMU, G711_LAW_PCMA };
	static const unsigned int rates[] = { 600, 1200 };

	for (size_t law = 0; law < ARRAY_LENGTH(laws); ++law) {
		for (unsigned int answerer = 0; answerer < 2; ++answerer) {
			for (size_t rate = 0; rate < ARRAY_LENGTH(rates); ++rate)
				CHECK(run_v22_mark_replay(rates[rate], answerer, laws[law], false),
				      "constant-mark rejection failed");
			CHECK(run_v22_mark_replay(1200, answerer, laws[law], true),
			      "scrambled-mark transition failed");
		}
	}
	return true;
}

static bool run_v32_recording_replay(const char *path, const char *expected_text)
{
	static const char dial_command[] = "ATDrecording\r";
	static const char *const commands[] = {
		"ATZ\r",
		"ATE0\r",
		"ATX3\r",
		"AT\\N0\r",
		"AT+DS=0,0\r",
		"AT+MS=32,0,9600,9600\r",
	};
	struct channel_bench_recording recording = { 0 };
	struct integration_endpoint endpoint;
	size_t connected_at = SIZE_MAX;
	bool expected_text_seen = false;
	size_t expected_text_length = expected_text != NULL ? strlen(expected_text) : 0;

	CHECK(expected_text_length < ENDPOINT_OUTPUT_CAPACITY,
	      "V.32 replay expected text exceeds the output bound");
	CHECK(channel_bench_recording_load(
		      path, REPLAY_MAX_SAMPLES, &recording) ==
		      CHANNEL_BENCH_OK,
	      "optional V.32 recording could not be loaded");
	CHECK(endpoint_init(&endpoint, "V.32 recording replay"),
	      "V.32 recording replay did not initialize");
	CHECK(configure_recording_replay(
		      &endpoint, commands, ARRAY_LENGTH(commands)),
	      "V.32 recording replay setup was rejected");
	CHECK(modem_write(
		      endpoint.modem, dial_command,
		      (int)(sizeof(dial_command) - 1u)) ==
		      (int)(sizeof(dial_command) - 1u) &&
		      modem_report_outgoing_call_connected(endpoint.modem) == 0,
	      "V.32 recording replay did not start the call");
	for (size_t offset = 0; offset < recording.count;) {
		size_t count = recording.count - offset;

		if (count > SIP_FRAME_SAMPLES)
			count = SIP_FRAME_SAMPLES;
		memcpy(endpoint.receive, recording.samples + offset,
		       count * sizeof(*endpoint.receive));
		modem_process(
			endpoint.modem, endpoint.receive, endpoint.transmit,
			(int)count);
		offset += count;
		if (modem_is_online(endpoint.modem, DP_V32)) {
			if (connected_at == SIZE_MAX)
				connected_at = offset;
			CHECK(recording_replay_drain_online(
				      &endpoint, expected_text, &expected_text_seen),
			      "V.32 recording replay could not drain online output");
		} else {
			CHECK(endpoint_drain(&endpoint),
			      "V.32 recording replay terminal output overflowed");
		}
	}
	CHECK(connected_at != SIZE_MAX,
	      "V.32 recording did not acquire carrier (final DP %d/status %d)",
	      endpoint.modem->dp != NULL ? (int)endpoint.modem->dp->id : -1,
	      endpoint.modem->dp != NULL ? (int)endpoint.modem->dp->status : -1);
	CHECK(expected_text_length == 0 || expected_text_seen,
	      "V.32 recording did not reproduce expected text '%s'", expected_text);
	fprintf(stderr,
		"[       OK ] V.32 recording replay (%zu samples, carrier at %zu%s)\n",
		recording.count, connected_at,
		expected_text_seen ? ", expected text received" : "");

	endpoint_destroy(&endpoint);
	channel_bench_recording_release(&recording);
	return true;
}

static bool v34_replay_symbol_rate_parse(
	const char *value, unsigned int *symbol_rate)
{
	char *end;
	unsigned long parsed;

	if (symbol_rate == NULL)
		return false;
	if (value == NULL || value[0] == '\0') {
		*symbol_rate = 0;
		return true;
	}
	errno = 0;
	parsed = strtoul(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0')
		return false;
	switch (parsed) {
	case 2400:
	case 2743:
	case 2800:
	case 3000:
	case 3200:
	case 3429:
		*symbol_rate = (unsigned int)parsed;
		return true;
	default:
		return false;
	}
}

static bool v34_replay_bit_rate_parse(
	const char *value, unsigned int *bit_rate)
{
	char *end;
	unsigned long parsed;

	if (bit_rate == NULL)
		return false;
	if (value == NULL || value[0] == '\0') {
		*bit_rate = 9600;
		return true;
	}
	errno = 0;
	parsed = strtoul(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0' ||
	    parsed < V34_MIN_BIT_RATE || parsed > V34_MAX_BIT_RATE ||
	    parsed % V34_RATE_STEP != 0)
		return false;
	*bit_rate = (unsigned int)parsed;
	return true;
}

static bool replay_link_mode_parse(const char *value, bool *lapm)
{
	if (lapm == NULL)
		return false;
	if (value == NULL || value[0] == '\0' || strcmp(value, "raw") == 0) {
		*lapm = false;
		return true;
	}
	if (strcmp(value, "lapm") == 0) {
		*lapm = true;
		return true;
	}
	return false;
}

static bool v34_replay_sample_parse(const char *value, size_t *sample)
{
	char *end;
	unsigned long long parsed;

	if (sample == NULL)
		return false;
	if (value == NULL || value[0] == '\0') {
		*sample = SIZE_MAX;
		return true;
	}
	errno = 0;
	parsed = strtoull(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0' || parsed == 0 ||
	    parsed > REPLAY_MAX_SAMPLES)
		return false;
	*sample = (size_t)parsed;
	return true;
}

static bool run_v34_recording_replay(
	const char *path, const char *bit_rate_value,
	const char *symbol_rate_value, const char *link_mode_value,
	const char *retrain_sample_value, const char *expected_text,
	bool answerer)
{
	enum { TEST_STATE_COMMAND_ONLINE = 6 };
	static const char dial_command[] = "ATDrecording\r";
	static const char *const commands[] = {
		"ATZ\r",
		"ATE0\r",
		"ATX3\r",
	};
	static const char *const raw_commands[] = {
		"AT\\N0\r",
		"AT+DS=0,1\r",
	};
	static const char *const lapm_commands[] = {
		"AT+DS=3,1\r",
		"AT\\N4\r",
	};
	struct channel_bench_recording recording = { 0 };
	struct integration_endpoint endpoint;
	const char *role = answerer ? "answer" : "originator";
	char bit_rate_command[40];
	const char *bit_rate_commands[] = { bit_rate_command };
	char symbol_rate_command[32];
	char symbol_rate_description[32];
	const char *symbol_rate_commands[] = { symbol_rate_command };
	unsigned int symbol_rate;
	unsigned int bit_rate;
	bool lapm;
	size_t connected_at = SIZE_MAX;
	size_t handoff_at = SIZE_MAX;
	size_t retrain_sample;
	size_t retrained_at = SIZE_MAX;
	bool v8_seen = false;
	bool expected_text_seen = false;
	bool retrain_requested = false;
	char retrain_command[] = "ATO1";
	size_t expected_text_length =
		expected_text != NULL ? strlen(expected_text) : 0;

	CHECK(v34_replay_bit_rate_parse(bit_rate_value, &bit_rate) &&
		      v34_replay_symbol_rate_parse(
			      symbol_rate_value, &symbol_rate) &&
		      replay_link_mode_parse(link_mode_value, &lapm) &&
		      v34_replay_sample_parse(
			      retrain_sample_value, &retrain_sample),
		      "optional V.34 recording replay parameter is invalid");
	CHECK(expected_text_length < sizeof(endpoint.output),
	      "optional V.34 recording replay text is too long");
	CHECK(channel_bench_recording_load(
		      path, REPLAY_MAX_SAMPLES, &recording) ==
		      CHANNEL_BENCH_OK,
	      "optional V.34 recording could not be loaded");
	CHECK(endpoint_init(&endpoint, "V.34 recording replay"),
	      "V.34 recording replay did not initialize");
	CHECK(configure_recording_replay(
		      &endpoint, commands, ARRAY_LENGTH(commands)),
	      "V.34 recording replay setup was rejected");
	CHECK(configure_recording_replay(
		      &endpoint, lapm ? lapm_commands : raw_commands,
		      lapm ? ARRAY_LENGTH(lapm_commands) :
			     ARRAY_LENGTH(raw_commands)),
	      "V.34 recording replay link mode was rejected");
	{
		int length = snprintf(
			bit_rate_command, sizeof(bit_rate_command),
			"AT+MS=34,0,%u,%u\r", V34_MIN_BIT_RATE, bit_rate);

		CHECK(length > 0 && (size_t)length < sizeof(bit_rate_command) &&
			      configure_recording_replay(
				      &endpoint, bit_rate_commands,
				      ARRAY_LENGTH(bit_rate_commands)),
		      "V.34 recording replay bit rate was rejected");
	}
	if (symbol_rate != 0) {
		int length = snprintf(
			symbol_rate_command, sizeof(symbol_rate_command),
			"AT+V34B=%u\r", symbol_rate);

		CHECK(length > 0 && (size_t)length < sizeof(symbol_rate_command) &&
			      configure_recording_replay(
				      &endpoint, symbol_rate_commands,
				      ARRAY_LENGTH(symbol_rate_commands)),
		      "V.34 recording replay symbol rate was rejected");
	}
	if (symbol_rate == 0) {
		memcpy(symbol_rate_description, "automatic",
		       sizeof("automatic"));
	} else {
		int length = snprintf(
			symbol_rate_description,
			sizeof(symbol_rate_description), "%u symbol/s",
			symbol_rate);

		CHECK(length > 0 &&
			      (size_t)length < sizeof(symbol_rate_description),
		      "V.34 recording symbol rate description overflowed");
	}
	if (answerer) {
		CHECK(modem_report_incoming_call(endpoint.modem) == 0 &&
			      endpoint_drain(&endpoint) &&
			      output_contains(&endpoint, "RING") &&
			      modem_answer(endpoint.modem) == 0,
		      "V.34 answer recording replay did not start the call");
		endpoint.output_count = 0;
	} else {
		CHECK(modem_write(
			      endpoint.modem, dial_command,
			      (int)(sizeof(dial_command) - 1u)) ==
			      (int)(sizeof(dial_command) - 1u) &&
			      modem_report_outgoing_call_connected(endpoint.modem) == 0,
		      "V.34 originator recording replay did not start the call");
	}
	for (size_t offset = 0; offset < recording.count;) {
		size_t count = recording.count - offset;
		bool v8_before;

		if (count > SIP_FRAME_SAMPLES)
			count = SIP_FRAME_SAMPLES;
		if (!retrain_requested && retrain_sample != SIZE_MAX &&
		    offset < retrain_sample && count > retrain_sample - offset)
			count = retrain_sample - offset;
		if (!retrain_requested && offset == retrain_sample) {
			CHECK(connected_at != SIZE_MAX,
			      "V.34 recording retrain preceded initial CONNECT");
			endpoint.output_count = 0;
			endpoint.modem->state = TEST_STATE_COMMAND_ONLINE;
			CHECK(process_at_command(
				      endpoint.modem, retrain_command) == 1 &&
				      endpoint.modem->retrain_pending != 0,
			      "V.34 recording replay rejected ATO1");
			retrain_requested = true;
		}
		memcpy(endpoint.receive, recording.samples + offset,
		       count * sizeof(*endpoint.receive));
		v8_before = endpoint.modem->dp != NULL &&
			endpoint.modem->dp->id == DP_V8;
		modem_process(
			endpoint.modem, endpoint.receive, endpoint.transmit,
			(int)count);
		offset += count;
		CHECK(endpoint_drain(&endpoint),
		      "V.34 recording replay terminal output overflowed");
		v8_seen = v8_seen || v8_before;
		if (handoff_at == SIZE_MAX && v8_before &&
		    endpoint.modem->dp != NULL &&
		    endpoint.modem->dp->id == DP_V34)
			handoff_at = offset;
		if (connected_at == SIZE_MAX &&
		    modem_is_online(endpoint.modem, DP_V34))
			connected_at = offset;
		if (retrain_requested && retrained_at == SIZE_MAX &&
		    endpoint.modem->retrain_pending == 0 &&
		    output_contains(&endpoint, "CONNECT"))
			retrained_at = offset;
		if ((retrain_sample == SIZE_MAX ?
		     connected_at != SIZE_MAX : retrained_at != SIZE_MAX) &&
		    expected_text != NULL &&
		    expected_text[0] != '\0' &&
		    output_contains(&endpoint, expected_text))
			expected_text_seen = true;
		/* A recording can contain arbitrarily much online data, while this
		 * optional replay has no DTE consuming it. Retain terminal output only
		 * while it can still establish the CONNECT boundary. */
		if (modem_is_online(endpoint.modem, DP_V34) &&
		    (!retrain_requested || retrained_at != SIZE_MAX) &&
		    (expected_text_length == 0 || expected_text_seen)) {
			endpoint.output_count = 0;
		} else if (connected_at != SIZE_MAX && expected_text_length != 0 &&
			   endpoint.output_count >= expected_text_length) {
			size_t retained = expected_text_length - 1u;

			memmove(endpoint.output,
				endpoint.output + endpoint.output_count - retained,
				retained);
			endpoint.output_count = retained;
		}
	}
	CHECK(connected_at != SIZE_MAX,
	      "V.34 %s recording did not acquire carrier (V.8 seen %d, handoff %zu, final DP %d/status %d)",
	      role, v8_seen, handoff_at,
	      endpoint.modem->dp != NULL ? (int)endpoint.modem->dp->id : -1,
	      endpoint.modem->dp != NULL ? (int)endpoint.modem->dp->status : -1);
	CHECK(retrain_sample == SIZE_MAX ||
		      (retrain_requested && retrained_at != SIZE_MAX),
	      "V.34 %s recording did not complete ATO1 (requested %d, sample %zu, pending %u, final DP %d/status %d)",
	      role, retrain_requested, retrain_sample,
	      endpoint.modem->retrain_pending,
	      endpoint.modem->dp != NULL ? (int)endpoint.modem->dp->id : -1,
	      endpoint.modem->dp != NULL ? (int)endpoint.modem->dp->status : -1);
	CHECK(expected_text == NULL || expected_text[0] == '\0' ||
		      expected_text_seen,
	      "V.34 %s recording did not reproduce expected text '%s'",
	      role, expected_text != NULL ? expected_text : "");
	fprintf(stderr,
		"[       OK ] V.34 %s recording replay (%s, %zu samples, carrier at %zu, retrain at %zu, terminal bytes %zu)\n",
		role, symbol_rate_description, recording.count, connected_at,
		retrained_at, endpoint.output_count);

	endpoint_destroy(&endpoint);
	channel_bench_recording_release(&recording);
	return true;
}

static bool run_v22bis_recording_replay(
	const char *path, bool answerer)
{
	static const char dial_command[] = "ATDrecording\r";
	static const char *const commands[] = {
		"ATZ\r",
		"ATE0\r",
		"ATX3\r",
		"AT+DS=3,1\r",
		"AT+MS=122,0,2400,2400\r",
	};
	struct channel_bench_recording recording = { 0 };
	struct integration_endpoint endpoint;
	size_t connected_at = SIZE_MAX;

	CHECK(channel_bench_recording_load(
		      path, REPLAY_MAX_SAMPLES, &recording) ==
		      CHANNEL_BENCH_OK,
	      "optional V.22bis recording could not be loaded");
	CHECK(endpoint_init(&endpoint, "V.22bis recording replay"),
	      "V.22bis recording replay did not initialize");
	CHECK(configure_recording_replay(
		      &endpoint, commands, ARRAY_LENGTH(commands)),
	      "V.22bis recording replay setup was rejected");
	if (answerer) {
		CHECK(modem_report_incoming_call(endpoint.modem) == 0 &&
			      endpoint_drain(&endpoint) &&
			      output_contains(&endpoint, "RING") &&
			      modem_answer(endpoint.modem) == 0,
		      "V.22bis answer recording replay did not start the call");
		endpoint.output_count = 0;
	} else {
		CHECK(modem_write(
			      endpoint.modem, dial_command,
			      (int)(sizeof(dial_command) - 1u)) ==
			      (int)(sizeof(dial_command) - 1u) &&
			      modem_report_outgoing_call_connected(endpoint.modem) == 0,
		      "V.22bis originator recording replay did not start the call");
	}
	for (size_t offset = 0; offset < recording.count;) {
		size_t count = recording.count - offset;

		if (count > SIP_FRAME_SAMPLES)
			count = SIP_FRAME_SAMPLES;
		memcpy(endpoint.receive, recording.samples + offset,
		       count * sizeof(*endpoint.receive));
		modem_process(
			endpoint.modem, endpoint.receive, endpoint.transmit,
			(int)count);
		offset += count;
		CHECK(endpoint_drain(&endpoint),
		      "V.22bis recording replay terminal output overflowed");
		if (connected_at == SIZE_MAX &&
		    modem_is_online(endpoint.modem, DP_V22BIS))
			connected_at = offset;
	}
	CHECK(connected_at != SIZE_MAX &&
		      connected_at <= V22_REPLAY_CONNECT_SAMPLES,
	      "V.22bis %s recording did not acquire promptly (sample %zu)",
	      answerer ? "answer" : "originator", connected_at);
	fprintf(stderr,
		"[       OK ] V.22bis %s recording replay (%zu samples, carrier at %zu, terminal bytes %zu)\n",
		answerer ? "answer" : "originator", recording.count,
		connected_at, endpoint.output_count);

	endpoint_destroy(&endpoint);
	channel_bench_recording_release(&recording);
	return true;
}

static bool run_v22_raw_answer_recording_replay(const char *path)
{
	static const char *const commands[] = {
		"ATZ\r",
		"ATE0\r",
		"ATX3\r",
		"AT\\N0\r",
		"AT+DS=0,0\r",
		"AT+MS=22,0,1200,1200\r",
	};
	struct channel_bench_recording recording = { 0 };
	struct integration_endpoint endpoint;
	size_t connected_at = SIZE_MAX;

	CHECK(channel_bench_recording_load(
		      path, REPLAY_MAX_SAMPLES, &recording) ==
		      CHANNEL_BENCH_OK,
	      "optional raw V.22 answer recording could not be loaded");
	CHECK(endpoint_init(&endpoint, "raw V.22 answer replay"),
	      "raw V.22 answer recording replay did not initialize");
	CHECK(configure_recording_replay(
		      &endpoint, commands, ARRAY_LENGTH(commands)) &&
		      modem_report_incoming_call(endpoint.modem) == 0 &&
		      endpoint_drain(&endpoint) &&
		      output_contains(&endpoint, "RING") &&
		      modem_answer(endpoint.modem) == 0,
	      "raw V.22 answer recording replay did not start the call");
	endpoint.output_count = 0;
	for (size_t offset = 0; offset < recording.count;) {
		size_t count = recording.count - offset;

		if (count > SIP_FRAME_SAMPLES)
			count = SIP_FRAME_SAMPLES;
		memcpy(endpoint.receive, recording.samples + offset,
		       count * sizeof(*endpoint.receive));
		modem_process(
			endpoint.modem, endpoint.receive, endpoint.transmit,
			(int)count);
		offset += count;
		CHECK(endpoint_drain(&endpoint),
		      "raw V.22 answer replay terminal output overflowed");
		if (connected_at == SIZE_MAX &&
		    modem_is_online(endpoint.modem, DP_V22))
			connected_at = offset;
		if (connected_at != SIZE_MAX)
			endpoint.output_count = 0;
	}
	CHECK(connected_at != SIZE_MAX &&
		      connected_at <= V22_REPLAY_CONNECT_SAMPLES,
	      "raw V.22 answer recording did not acquire promptly (sample %zu)",
	      connected_at);
	fprintf(stderr,
		"[       OK ] raw V.22 answer recording replay (%zu samples, carrier at %zu)\n",
		recording.count, connected_at);

	endpoint_destroy(&endpoint);
	channel_bench_recording_release(&recording);
	return true;
}

static bool load_octet_recording(
	const char *path, uint8_t **octets, size_t *count)
{
	struct stat status;
	uint8_t *loaded;
	size_t offset = 0;
	int descriptor;

	if (path == NULL || path[0] == '\0' || octets == NULL || count == NULL)
		return false;
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0 || fstat(descriptor, &status) != 0 ||
	    !S_ISREG(status.st_mode) || status.st_size <= 0 ||
	    (uintmax_t)status.st_size > REPLAY_MAX_SAMPLES) {
		if (descriptor >= 0)
			close(descriptor);
		return false;
	}
	loaded = malloc((size_t)status.st_size);
	if (loaded == NULL) {
		close(descriptor);
		return false;
	}
	while (offset < (size_t)status.st_size) {
		ssize_t received = read(
			descriptor, loaded + offset,
			(size_t)status.st_size - offset);

		if (received < 0 && errno == EINTR)
			continue;
		if (received <= 0) {
			free(loaded);
			close(descriptor);
			return false;
		}
		offset += (size_t)received;
	}
	if (close(descriptor) != 0) {
		free(loaded);
		return false;
	}
	*octets = loaded;
	*count = offset;
	return true;
}

static bool v90_replay_codec_parse(
	const char *value, enum media_codec *codec)
{
	if (codec == NULL)
		return false;
	if (value == NULL || value[0] == '\0' || strcmp(value, "PCMU") == 0) {
		*codec = MEDIA_CODEC_PCMU;
		return true;
	}
	if (strcmp(value, "PCMA") == 0) {
		*codec = MEDIA_CODEC_PCMA;
		return true;
	}
	return false;
}

static bool v90_replay_role_parse(const char *value, bool *originator)
{
	if (originator == NULL)
		return false;
	if (value == NULL || value[0] == '\0' ||
	    strcmp(value, "answerer") == 0) {
		*originator = false;
		return true;
	}
	if (strcmp(value, "originator") == 0) {
		*originator = true;
		return true;
	}
	return false;
}

static bool v90_replay_pcm_role_parse(
	const char *value, bool originator, enum v90_modem_role *role)
{
	if (role == NULL)
		return false;
	if (value == NULL || value[0] == '\0' || strcmp(value, "by-call") == 0) {
		*role = originator ? V90_ANALOG_MODEM : V90_DIGITAL_MODEM;
		return true;
	}
	if (strcmp(value, "analog") == 0) {
		*role = V90_ANALOG_MODEM;
		return true;
	}
	if (strcmp(value, "digital") == 0) {
		*role = V90_DIGITAL_MODEM;
		return true;
	}
	return false;
}

struct v90_replay_profile {
	unsigned int downstream_minimum_rate;
	unsigned int downstream_rate;
	unsigned int upstream_minimum_rate;
	unsigned int upstream_rate;
	unsigned int symbol_rate;
};

static bool v90_replay_unsigned_parse(
	const char *value, unsigned int fallback, unsigned int *parsed_value)
{
	char *end;
	unsigned long parsed;

	if (parsed_value == NULL)
		return false;
	if (value == NULL || value[0] == '\0') {
		*parsed_value = fallback;
		return true;
	}
	errno = 0;
	parsed = strtoul(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0' || parsed > UINT_MAX)
		return false;
	*parsed_value = (unsigned int)parsed;
	return true;
}

static bool v90_replay_profile_parse(
	const char *downstream_minimum_value, const char *downstream_value,
	const char *upstream_minimum_value, const char *upstream_value,
	const char *symbol_value, enum g711_law law, bool originator,
	enum v90_modem_role pcm_role,
	struct v90_replay_profile *profile)
{
	struct v90_replay_profile selected;
	struct v90_upstream_policy upstream;
	struct pcm_call_configuration configuration;

	if (profile == NULL ||
	    !v90_replay_unsigned_parse(
		    downstream_value, 28000u, &selected.downstream_rate) ||
	    !v90_replay_unsigned_parse(
		    downstream_minimum_value, selected.downstream_rate,
		    &selected.downstream_minimum_rate) ||
	    !v90_replay_unsigned_parse(
		    upstream_value, originator ? 14400u : 19200u,
		    &selected.upstream_rate) ||
	    !v90_replay_unsigned_parse(
		    upstream_minimum_value, selected.upstream_rate,
		    &selected.upstream_minimum_rate) ||
	    !v90_replay_unsigned_parse(
		    symbol_value, 3000u, &selected.symbol_rate))
		return false;
	upstream = (struct v90_upstream_policy){
		.minimum_bit_rate = selected.upstream_minimum_rate,
		.maximum_bit_rate = selected.upstream_rate,
		.maximum_symbol_rate = selected.symbol_rate,
	};
	if (v90_profile_init(
		    &configuration, pcm_role, law,
		    selected.downstream_minimum_rate,
		    selected.downstream_rate,
		    &upstream) != V90_PROFILE_OK)
		return false;
	*profile = selected;
	return true;
}

static bool v90_replay_gain_parse(const char *value, double *gain)
{
	char *end;
	double parsed;

	if (gain == NULL)
		return false;
	if (value == NULL || value[0] == '\0') {
		*gain = 1.0;
		return true;
	}
	errno = 0;
	parsed = strtod(value, &end);
	if (errno != 0 || end == value || *end != '\0' ||
	    !(parsed >= 0.0 && parsed <= 4.0))
		return false;
	*gain = parsed;
	return true;
}

static int16_t v90_replay_apply_gain(int16_t sample, double gain)
{
	double scaled = (double)sample * gain;

	if (scaled >= INT16_MAX)
		return INT16_MAX;
	if (scaled <= INT16_MIN)
		return INT16_MIN;
	return (int16_t)(scaled + (scaled >= 0.0 ? 0.5 : -0.5));
}

static bool run_v90_recording_replay(
	const char *path, enum media_codec codec, bool originator,
	enum v90_modem_role pcm_role,
	const char *downstream_minimum_value, const char *downstream_value,
	const char *upstream_minimum_value, const char *upstream_value,
	const char *symbol_value, const char *link_mode_value,
	const char *receive_gain_value,
	const char *expected_text)
{
	enum { MINIMUM_ONLINE_SAMPLES = 3200 };
	static const char *const common_commands[] = {
		"ATZ\r",
		"ATE0\r",
		"ATX3\r",
	};
	static const char *const raw_commands[] = {
		"AT\\N0\r",
		"AT+DS=0,0\r",
	};
	static const char *const lapm_commands[] = {
		"AT+DS=3,1\r",
		"AT\\N4\r",
	};
	static const char analog_command[] = "AT+V90D=0\r";
	static const char digital_command[] = "AT+V90D=1\r";
	static const char dial_command[] = "ATDpeer\r";
	const char *commands[
		ARRAY_LENGTH(common_commands) + ARRAY_LENGTH(raw_commands) + 3u];
	const char *const *link_commands;
	char downstream_command[48];
	char upstream_command[48];
	int downstream_command_length;
	int upstream_command_length;
	struct integration_endpoint endpoint;
	struct v90_replay_profile profile;
	enum g711_law law;
	uint8_t *recording = NULL;
	size_t recording_count = 0;
	size_t handoff_at = SIZE_MAX;
	size_t fallback_at = SIZE_MAX;
	size_t connected_at = SIZE_MAX;
	bool v8_seen = false;
	bool expected_text_seen = false;
	bool lapm;
	double receive_gain;
	size_t expected_text_length =
		expected_text != NULL ? strlen(expected_text) : 0;
	bool require_carrier = !originator || pcm_role == V90_DIGITAL_MODEM ||
			       expected_text_length != 0;

	CHECK(codec_law(codec, &law) &&
		      replay_link_mode_parse(link_mode_value, &lapm) &&
		      v90_replay_gain_parse(receive_gain_value, &receive_gain) &&
		      v90_replay_profile_parse(
			      downstream_minimum_value, downstream_value,
			      upstream_minimum_value, upstream_value, symbol_value,
			      law, originator, pcm_role, &profile),
		      "optional V.90 G.711 recording profile is invalid");
	CHECK(expected_text_length < sizeof(endpoint.output),
	      "optional V.90 recording replay text is too long");
	downstream_command_length = snprintf(
		downstream_command, sizeof(downstream_command),
		"AT+MS=90,0,%u,%u\r", profile.downstream_minimum_rate,
		profile.downstream_rate);
	upstream_command_length = snprintf(
		upstream_command, sizeof(upstream_command),
		"AT+V90U=%u,%u,%u\r", profile.upstream_minimum_rate,
		profile.upstream_rate, profile.symbol_rate);
	CHECK(downstream_command_length > 0 &&
		      (size_t)downstream_command_length <
			      sizeof(downstream_command) &&
		      upstream_command_length > 0 &&
		      (size_t)upstream_command_length < sizeof(upstream_command),
	      "optional V.90 G.711 recording commands are too long");
	CHECK(load_octet_recording(path, &recording, &recording_count),
	      "optional V.90 G.711 recording could not be loaded");
	for (size_t index = 0; index < ARRAY_LENGTH(common_commands); ++index)
		commands[index] = common_commands[index];
	link_commands = lapm ? lapm_commands : raw_commands;
	for (size_t index = 0; index < ARRAY_LENGTH(raw_commands); ++index)
		commands[ARRAY_LENGTH(common_commands) + index] =
			link_commands[index];
	commands[ARRAY_LENGTH(common_commands) + ARRAY_LENGTH(raw_commands)] =
		pcm_role == V90_DIGITAL_MODEM ? digital_command : analog_command;
	commands[ARRAY_LENGTH(common_commands) + ARRAY_LENGTH(raw_commands) + 1u] =
		downstream_command;
	commands[ARRAY_LENGTH(common_commands) + ARRAY_LENGTH(raw_commands) + 2u] =
		upstream_command;
	CHECK(endpoint_init(&endpoint, "V.90 recording replay") &&
		      configure_recording_replay(
			      &endpoint, commands, ARRAY_LENGTH(commands)),
	      "V.90 recording replay did not initialize");
	if (originator) {
		CHECK(modem_write(
			      endpoint.modem, dial_command,
			      (int)(sizeof(dial_command) - 1u)) ==
			      (int)(sizeof(dial_command) - 1u) &&
			      endpoint_drain(&endpoint) &&
			      modem_report_outgoing_call_connected(
				      endpoint.modem) == 0,
		      "V.90 recording replay did not originate the call");
	} else {
		CHECK(modem_report_incoming_call(endpoint.modem) == 0 &&
			      endpoint_drain(&endpoint) &&
			      output_contains(&endpoint, "RING") &&
			      modem_answer(endpoint.modem) == 0,
		      "V.90 recording replay did not answer the call");
	}
	endpoint.output_count = 0;
	for (size_t offset = 0; offset < recording_count;) {
		size_t frame_count = recording_count - offset;
		bool exact_output = false;
		bool v8_before;
		bool v90_before;

		if (frame_count > SIP_FRAME_SAMPLES)
			frame_count = SIP_FRAME_SAMPLES;
		memcpy(endpoint.receive_codewords, recording + offset,
		       frame_count);
		for (size_t index = 0; index < frame_count; ++index)
			endpoint.receive[index] = v90_replay_apply_gain(
				decode_codeword(
					law, endpoint.receive_codewords[index]),
				receive_gain);
		v8_before = endpoint.modem->dp != NULL &&
			endpoint.modem->dp->id == DP_V8;
		v90_before = endpoint.modem->dp != NULL &&
			endpoint.modem->dp->id == DP_V90;
		modem_process_g711(
			endpoint.modem, law, endpoint.receive_codewords,
			endpoint.receive, NULL, endpoint.transmit_codewords,
			endpoint.transmit, (int)frame_count, &exact_output);
		offset += frame_count;
		if (modem_is_online(endpoint.modem, DP_V90) ||
		    modem_is_online(endpoint.modem, DP_V34)) {
			CHECK(recording_replay_drain_online(
				      &endpoint, expected_text, &expected_text_seen),
			      "V.90 recording replay could not drain online output");
		} else {
			CHECK(endpoint_drain(&endpoint),
			      "V.90 recording replay terminal output overflowed");
		}
		v8_seen = v8_seen || v8_before;
		if (handoff_at == SIZE_MAX && v8_before &&
		    endpoint.modem->dp != NULL &&
		    endpoint.modem->dp->id == DP_V90)
			handoff_at = offset;
		if (fallback_at == SIZE_MAX && (v8_before || v90_before) &&
		    endpoint.modem->dp != NULL &&
		    endpoint.modem->dp->id == DP_V34)
			fallback_at = offset;
		if (connected_at == SIZE_MAX &&
		    modem_is_online(endpoint.modem, DP_V90))
			connected_at = offset;
	}
	CHECK(expected_text_length == 0 || expected_text_seen,
	      "V.90 recording did not reproduce expected text '%s'",
	      expected_text != NULL ? expected_text : "");
	if (fallback_at != SIZE_MAX) {
		CHECK(endpoint.modem->dp != NULL &&
			      endpoint.modem->dp->id == DP_V34,
		      "V.90 recording lost its V.34 fallback owner (handoff %zu, fallback %zu, final DP %d/status %d)",
		      handoff_at, fallback_at,
		      endpoint.modem->dp != NULL ?
			      (int)endpoint.modem->dp->id : -1,
		      endpoint.modem->dp != NULL ?
			      (int)endpoint.modem->dp->status : -1);
		fprintf(stderr,
			"[       OK ] V.90 recording selected V.34 fallback over %s (%zu octets, fallback at %zu)\n",
			codec_name(codec), recording_count, fallback_at);
	} else if (!require_carrier) {
		/* A recording captured from a peer that never received CJ cannot
		 * contain the interactive Phase 2 response to a repaired caller.
		 * It can still independently prove ANSam/JM acceptance and the
		 * V.90 ownership handoff. */
		CHECK(handoff_at != SIZE_MAX,
		      "outgoing V.90 recording did not hand off from V.8 (V.8 seen %d, final DP %d/status %d)",
		      v8_seen,
		      endpoint.modem->dp != NULL ?
			      (int)endpoint.modem->dp->id : -1,
		      endpoint.modem->dp != NULL ?
			      (int)endpoint.modem->dp->status : -1);
		fprintf(stderr,
			"[       OK ] outgoing V.90 V.8 recording replay over %s (%zu octets, handoff at %zu)\n",
			codec_name(codec), recording_count, handoff_at);
	} else {
		CHECK(connected_at != SIZE_MAX &&
			      recording_count - connected_at >=
				      MINIMUM_ONLINE_SAMPLES &&
			      modem_is_online(endpoint.modem, DP_V90),
		      "V.90 recording did not sustain carrier (V.8 seen %d, handoff %zu, carrier %zu, final DP %d/status %d)",
		      v8_seen, handoff_at,
		      connected_at,
		      endpoint.modem->dp != NULL ?
			      (int)endpoint.modem->dp->id : -1,
		      endpoint.modem->dp != NULL ?
			      (int)endpoint.modem->dp->status : -1);
		fprintf(stderr,
			"[       OK ] V.90 recording replay over %s (%zu octets, carrier at %zu)\n",
			codec_name(codec), recording_count, connected_at);
	}
	endpoint_destroy(&endpoint);
	free(recording);
	return true;
}

enum fsk_mark_fixture {
	FSK_MARK_FRAGMENTED,
	FSK_MARK_INITIAL,
	FSK_MARK_BRIEF,
	FSK_MARK_WITHDRAWN,
	FSK_MARK_PROBE,
	FSK_MARK_DATA,
	FSK_MARK_ABSENT,
	FSK_MARK_OFFSET_LOW,
	FSK_MARK_OFFSET_HIGH,
	FSK_MARK_WRONG_FREQUENCY,
};

static bool run_fsk300_mark_qualification(
	enum DP_ID id, int caller, unsigned int fragment, enum fsk_mark_fixture fixture)
{
	enum { MARK_LENGTH = MODEM_RATE * 15 / 1000 };
	struct modem modem = { 0 };
	const struct dp_operations *operations = modem_dp_get_registered_ops(id);
	struct fsk_channel channel = caller ? v21_channel_2 : v21_channel_1;
	struct fsk_tone_generator tones;
	struct fsk_tone_generator probe;
	int16_t receive[MODEM_FRAG], transmit[MODEM_FRAG];
	struct dp *dp;
	unsigned int connected = 0;
	bool timed_out = false;
	/* End a qualifying short mark between native block boundaries. */
	unsigned int mark_start = fixture == FSK_MARK_INITIAL ? 0 :
		240 + (id == DP_V21 ? 9 - caller : 8 + caller);
	bool valid = true;

	if (id == DP_B103) {
		channel.mark_frequency_hz = caller ? 2225 : 1270;
		channel.space_frequency_hz = caller ? 2025 : 1070;
	}
	if (fixture == FSK_MARK_OFFSET_LOW)
		channel.mark_frequency_hz -= 15;
	if (fixture == FSK_MARK_OFFSET_HIGH)
		channel.mark_frequency_hz += 15;
	CHECK(operations != NULL &&
	      fsk_tone_generator_init(&tones, &channel, 8192) == FSK_OK,
	      "FSK qualification fixture did not initialize");
	/* Model a nearby answer probe followed by an unrelated tone. */
	channel.mark_frequency_hz += 25;
	channel.space_frequency_hz = id == DP_B103 && caller ? 1800 : 600;
	CHECK(fsk_tone_generator_init(&probe, &channel, 8192) == FSK_OK,
	      "FSK answer probe did not initialize");
	modem.sregs[SREG_WAIT_CARRIER_AFTER_DIAL] = 3;
	dp = operations->create(&modem, id, caller, MODEM_RATE, MODEM_FRAG,
				operations);
	CHECK(dp != NULL, "FSK qualification datapump did not initialize");
	for (unsigned int at = 0; at < 4 * MODEM_RATE && valid;) {
		unsigned int count = fragment;
		int status;

		if (count > 4 * MODEM_RATE - at)
			count = 4 * MODEM_RATE - at;
		for (unsigned int index = 0; index < count; ++index) {
			/* Short marks end each native block, but never last the
			 * required 15 ms. Follow them with a genuine carrier. */
			uint8_t bit = at + index >= MODEM_RATE ||
				(at + index) % MODEM_FRAG >= 13;

			if (fixture == FSK_MARK_INITIAL || fixture == FSK_MARK_BRIEF)
				bit = at + index >= mark_start &&
					at + index < mark_start + MARK_LENGTH;
			if (fixture >= FSK_MARK_WITHDRAWN)
				bit = at + index < MODEM_RATE * (caller ? 300 : 80) / 1000;
			if (fixture == FSK_MARK_PROBE)
				bit = at + index < MODEM_RATE * (caller ? 950 : 140) / 1000;
			if (fixture == FSK_MARK_DATA)
				bit = at + index < MODEM_RATE * (caller ? 300 : 80) / 1000 ||
					((at + index) / 27) % 2;
			if (at + index >= MODEM_RATE && fixture != FSK_MARK_ABSENT)
				bit = 1;
			if (fixture >= FSK_MARK_OFFSET_LOW)
				bit = 1;
			if (fsk_tone_generator_sample(
				    (fixture == FSK_MARK_PROBE && at + index < MODEM_RATE) ||
					fixture == FSK_MARK_WRONG_FREQUENCY ?
					&probe : &tones, bit, &receive[index]) != FSK_OK) {
				valid = false;
				break;
			}
			/* A brief carrier is insufficient for CONNECT. A later
			 * genuine carrier must still work within the same deadline. */
			if ((fixture == FSK_MARK_INITIAL && at + index >= MARK_LENGTH &&
			     at + index < MODEM_RATE) ||
			    ((fixture == FSK_MARK_WITHDRAWN || fixture == FSK_MARK_ABSENT) &&
			     !bit))
				receive[index] = 0;
			if (fragment == 1)
				receive[index] = g711_decode_pcmu(g711_encode_pcmu(receive[index]));
			if (fragment == 7)
				receive[index] = g711_decode_pcma(g711_encode_pcma(receive[index]));
		}
		if (!valid)
			break;
		status = operations->process(dp, receive, transmit, (int)count);
		at += count;
		if (status == DPSTAT_CONNECT) {
			connected = at;
			break;
		}
		if ((fixture == FSK_MARK_ABSENT || fixture == FSK_MARK_WRONG_FREQUENCY) &&
		    status == DPSTAT_ERROR) {
			timed_out = at > 3 * MODEM_RATE && at <= 3 * MODEM_RATE + fragment;
			break;
		}
		valid = status == DPSTAT_OK;
	}
	CHECK(operations->delete(dp) == 0,
	      "FSK qualification datapump did not release");
	bool early_data = fixture == FSK_MARK_DATA ||
		(!caller && fixture == FSK_MARK_BRIEF) ||
		fixture == FSK_MARK_OFFSET_LOW || fixture == FSK_MARK_OFFSET_HIGH;
	CHECK(valid && (fixture == FSK_MARK_ABSENT || fixture == FSK_MARK_WRONG_FREQUENCY ?
	      connected == 0 && timed_out :
	      connected > (early_data ? mark_start + MARK_LENGTH : MODEM_RATE) &&
	      connected <= 3 * MODEM_RATE),
	      "FSK id=%d caller=%d fragment=%u fixture=%d connected at %u, valid=%d",
	      id, caller, fragment, fixture, connected, valid);
	return true;
}

struct fsk_startup_output {
	uint8_t bytes[8];
	size_t count;
};

static int capture_fsk_startup(struct modem *modem, const char *input, int count)
{
	struct fsk_startup_output *output = modem->dev_data;
	if (count < 0 || (size_t)count > sizeof(output->bytes) - output->count)
		return 0;
	memcpy(output->bytes + output->count, input, (size_t)count);
	output->count += (size_t)count;
	return count;
}

static bool test_fsk_startup_carrier_withdrawal(void)
{
	const unsigned int fragments[] = { 1, 7, MODEM_FRAG };
	const uint8_t stale[] = { 'x' }, fresh[] = { 'n', 0, 255 };
	for (size_t case_index = 0; case_index < ARRAY_LENGTH(fragments); ++case_index) {
		struct modem modem = { 0 };
		struct fsk_startup_output output = { 0 };
		struct fsk_tone_generator tones;
		const struct dp_operations *ops = modem_dp_get_registered_ops(DP_V21);
		int16_t receive[MODEM_FRAG], transmit[MODEM_FRAG];
		bool connected = false;

		CHECK(ops != NULL, "V.21 startup datapump is unavailable");
		modem.dev_data = &output;
		modem.sregs[SREG_WAIT_CARRIER_AFTER_DIAL] = 3;
		modem.sregs[SREG_CARRIER_LOSS_DISCONNECT_TIME] = 7;
		modem.dp = ops->create(&modem, DP_V21, 1, MODEM_RATE, MODEM_FRAG, ops);
		CHECK(modem.dp != NULL &&
		      fsk_tone_generator_init(&tones, &v21_channel_2, 8192) == FSK_OK,
		      "withdrawn startup fixture failed");
		for (unsigned int at = 0; at < MODEM_RATE * 2; at += fragments[case_index]) {
			unsigned int count = fragments[case_index];
			if (count > MODEM_RATE * 2 - at)
				count = MODEM_RATE * 2 - at;
			for (unsigned int i = 0; i < count; ++i) {
				unsigned int sample = at + i;
				unsigned int start = MODEM_RATE * (sample < MODEM_RATE ? 320 : 1040) / 1000;
				const uint8_t *payload = sample < MODEM_RATE ? stale : fresh;
				size_t size = sample < MODEM_RATE ? sizeof(stale) : sizeof(fresh);
				unsigned int bit_index = sample >= start ? (sample - start) * 300 / MODEM_RATE : 30;
				uint8_t bit = 1;
				if (bit_index < size * 10) {
					unsigned int cell = bit_index % 10;
					bit = cell == 0 ? 0 : cell == 9 ? 1 :
						(payload[bit_index / 10] >> (cell - 1)) & 1u;
				}
				CHECK(fsk_tone_generator_sample(&tones, bit, &receive[i]) == FSK_OK,
				      "withdrawn startup tone failed");
				/* Withdraw the first carrier after its early byte, then
				 * offer a new carrier and a different pre-CONNECT payload. */
				if (sample >= MODEM_RATE * 450 / 1000 && sample < MODEM_RATE * 650 / 1000)
					receive[i] = 0;
				if (fragments[case_index] == 1)
					receive[i] = g711_decode_pcmu(g711_encode_pcmu(receive[i]));
				if (fragments[case_index] == 7)
					receive[i] = g711_decode_pcma(g711_encode_pcma(receive[i]));
			}
			int status = ops->process(modem.dp, receive, transmit, (int)count);
			CHECK(status == DPSTAT_OK || status == DPSTAT_CONNECT,
			      "withdrawn startup failed at sample %u", at);
			if (status == DPSTAT_CONNECT) {
				CHECK(!connected && at > MODEM_RATE,
				      "withdrawn startup connected prematurely");
				connected = true;
				modem_async_start(&modem);
				modem.put_bits = modem_async_put_bits;
				modem.put_chars = capture_fsk_startup;
			}
		}
		CHECK(connected && output.count == sizeof(fresh) &&
		      memcmp(output.bytes, fresh, sizeof(fresh)) == 0,
		      "withdrawn startup leaked or lost data with %u-sample fragments: %zu bytes",
		      fragments[case_index], output.count);
		CHECK(ops->delete(modem.dp) == 0, "withdrawn startup cleanup failed");
	}
	return true;
}

static bool test_fsk300_mark_qualification(void)
{
	const enum DP_ID ids[] = { DP_V21, DP_B103 };
	const unsigned int fragments[] = { 1, 7, MODEM_FRAG };
	bool valid = true;

	for (size_t mode = 0; mode < ARRAY_LENGTH(ids); ++mode)
		for (int caller = 0; caller <= 1; ++caller)
			for (size_t fragment = 0; fragment < ARRAY_LENGTH(fragments);
			     ++fragment)
				for (enum fsk_mark_fixture fixture = FSK_MARK_FRAGMENTED;
				     fixture <= FSK_MARK_WRONG_FREQUENCY; ++fixture)
					if ((fixture != FSK_MARK_WRONG_FREQUENCY ||
					     (ids[mode] == DP_B103 && caller)) &&
					    !run_fsk300_mark_qualification(
						    ids[mode], caller, fragments[fragment],
						    fixture))
						valid = false;
	return valid;
}

/* Exercise actual factory settings against fixed, non-V.8 peers. Each
 * case must negotiate and carry exact binary bytes in both directions. */
static bool run_factory_startup_cases(const char *filter, bool *selected)
{
	static const struct {
		enum DP_ID id;
		const char *name;
		unsigned code, rate;
	} peers[] = {
		{ DP_V21, "V.21", 21, 300 },
		{ DP_B103, "Bell103", 103, 300 },
		{ DP_V23, "V.23", 23, 1200 },
		{ DP_V22, "V.22-600", 22, 600 },
		{ DP_V22, "V.22-1200", 22, 1200 },
		{ DP_V22BIS, "V.22bis", 122, 2400 },
		{ DP_V32, "V.32", 32, 9600 },
		{ DP_V32BIS, "V.32bis", 132, 14400 },
		{ DP_B212, "Bell212", 212, 1200 },
	};
	static const enum media_codec codecs[] = { MEDIA_CODEC_PCMU, MEDIA_CODEC_PCMA };

	for (size_t peer = 0; peer < ARRAY_LENGTH(peers); ++peer) {
		for (unsigned answering = 0; answering < 2; ++answering) {
			char name[80], fixed[80];
			bool test_prefix = !answering && (peers[peer].id == DP_V22 ||
				peers[peer].id == DP_V22BIS || peers[peer].id == DP_V23);
			enum DP_ID modern_id = peers[peer].id == DP_V32 ? DP_V32BIS :
				answering && peers[peer].id == DP_B212 ? DP_V22 : peers[peer].id;
			unsigned caller_rate = peers[peer].id == DP_V23 ? 75 : peers[peer].rate;
			unsigned answerer_rate = peers[peer].rate;
			struct modem_test_case test = {
				.id = answering ? peers[peer].id : DP_V92,
				.answerer_id = answering ? DP_V92 : peers[peer].id,
				.connected_id = answering ? peers[peer].id : modern_id,
				.answerer_connected_id = answering ? modern_id : peers[peer].id,
				.name = name,
				.selection_command = answering ? fixed : "AT\r",
				.answerer_selection_command = answering ? "AT\r" : fixed,
				.caller_tx_rate = caller_rate, .caller_rx_rate = answerer_rate,
				.answerer_tx_rate = answerer_rate, .answerer_rx_rate = caller_rate,
				.carrier_loss_samples = MODEM_RATE * 7 / 10,
				.training_frame_limit = 2000,
				.automode = !answering, .answerer_fixed = !answering,
				.answerer_automode = answering, .raw_transport = true,
				.automatic_pcm_roles = true, .factory_transport = true,
			};

			snprintf(fixed, sizeof(fixed), "AT+MS=%u,0,%u,%u\r",
				peers[peer].code, peers[peer].rate, peers[peer].rate);
			unsigned int last_prefix = !answering && peers[peer].id == DP_V22BIS ?
				2 : (unsigned int)test_prefix;
			for (unsigned int prefix = 0; prefix <= last_prefix; ++prefix) {
				test.answer_tone_prefix_ms = prefix ? 2600 : 0;
				test.answer_tone_prefix_modulated = prefix == 2;
				snprintf(name, sizeof(name), "legacy startup %s %s%s",
					answering ? "answering" : "calling", peers[peer].name,
					prefix == 2 ? " after ANSam" : prefix ? " after ANS" : "");
				if (filter != NULL && filter[0] != '\0' && strstr(name, filter) == NULL)
					continue;
				*selected = true;
				for (size_t codec = 0; codec < ARRAY_LENGTH(codecs); ++codec) {
					fprintf(stderr, "[ RUN      ] %s over %s SIP media\n",
						name, codec_name(codecs[codec]));
					if (!run_media_call(&test, codecs[codec]))
						return false;
					fprintf(stderr, "[       OK ] %s over %s SIP media\n",
						name, codec_name(codecs[codec]));
				}
			}
		}
	}
	return true;
}

static bool run_factory_pcm_cases(const char *filter, bool *selected)
{
	static const enum media_codec codecs[] = { MEDIA_CODEC_PCMU, MEDIA_CODEC_PCMA };

	for (unsigned protocol = 90; protocol <= 92; protocol += 2) {
		for (unsigned answering = 0; answering < 2; ++answering) {
			for (unsigned peer_digital = 0; peer_digital < 2; ++peer_digital) {
				char name[100], fixed[100];
				bool digital_caller = answering ? peer_digital : !peer_digital;
				enum DP_ID peer_id = protocol == 90 ? DP_V90 : DP_V92;
				unsigned upstream_rate = digital_caller && answering && protocol == 90 ? 31200 : 33600;
				struct modem_test_case test = {
					.id = answering ? peer_id : DP_V92,
					.answerer_id = answering ? DP_V92 : peer_id,
					.connected_id = peer_id,
					.answerer_connected_id = peer_id,
					.name = name,
					.selection_command = answering ? fixed : "AT\r",
					.answerer_selection_command = answering ? "AT\r" : fixed,
					.caller_tx_rate = digital_caller ? 28000 : upstream_rate,
					.caller_rx_rate = digital_caller ? upstream_rate : 28000,
					.answerer_tx_rate = digital_caller ? upstream_rate : 28000,
					.answerer_rx_rate = digital_caller ? 28000 : upstream_rate,
					.training_frame_limit = 1500,
					.automode = !answering, .answerer_fixed = !answering,
					.answerer_automode = answering, .requires_v8 = true,
					.requires_exact_g711 = true, .raw_transport = true,
					.digital_caller = digital_caller, .automatic_pcm_roles = true,
					.conventional_upstream = true, .factory_transport = true,
					.test_retrain = protocol == 90 && !answering && peer_digital,
					.impairment = INTEGRATION_IMPAIRMENT_DIGITAL_DELAY,
				};

				snprintf(name, sizeof(name), "factory PCM %s V.%u %s peer",
					answering ? "answering" : "calling", protocol,
					peer_digital ? "digital" : "analog");
				snprintf(fixed, sizeof(fixed), "AT+MS=%u,0,28000,28000+V90D=%u\r",
					protocol, peer_digital);
				if (filter != NULL && filter[0] != '\0' && strstr(name, filter) == NULL)
					continue;
				*selected = true;
				for (size_t codec = 0; codec < ARRAY_LENGTH(codecs); ++codec) {
					fprintf(stderr, "[ RUN      ] %s over %s SIP media\n",
						name, codec_name(codecs[codec]));
					if (!run_media_call(&test, codecs[codec]))
						return false;
					fprintf(stderr, "[       OK ] %s over %s SIP media\n",
						name, codec_name(codecs[codec]));
				}
			}
		}
	}
	return true;
}

int main(void)
{
	const char *v32_recording_path =
		getenv("V32_REPLAY_RX_PCM");
	const char *v32_recording_expected_text =
		getenv("V32_REPLAY_EXPECT_TEXT");
	const char *v34_recording_path =
		getenv("V34_REPLAY_RX_PCM");
	const char *v34_answer_recording_path =
		getenv("V34_ANSWER_REPLAY_RX_PCM");
	const char *v34_recording_symbol_rate =
		getenv("V34_REPLAY_SYMBOL_RATE");
	const char *v34_recording_bit_rate =
		getenv("V34_REPLAY_BIT_RATE");
	const char *v34_recording_link_mode =
		getenv("V34_REPLAY_LINK_MODE");
	const char *v34_recording_retrain_sample =
		getenv("V34_REPLAY_RETRAIN_SAMPLE");
	const char *v34_recording_expected_text =
		getenv("V34_REPLAY_EXPECT_TEXT");
	const char *v34_answer_recording_symbol_rate =
		getenv("V34_ANSWER_REPLAY_SYMBOL_RATE");
	const char *v34_answer_recording_bit_rate =
		getenv("V34_ANSWER_REPLAY_BIT_RATE");
	const char *v34_answer_recording_link_mode =
		getenv("V34_ANSWER_REPLAY_LINK_MODE");
	const char *v34_answer_recording_retrain_sample =
		getenv("V34_ANSWER_REPLAY_RETRAIN_SAMPLE");
	const char *v34_answer_recording_expected_text =
		getenv("V34_ANSWER_REPLAY_EXPECT_TEXT");
	const char *v34_matrix_filter = getenv("V34_MATRIX_FILTER");
	const bool skip_v34_matrix =
		getenv("MODEM_INTEGRATION_SKIP_V34_MATRIX") != NULL;
	const char *integration_filter = getenv("MODEM_INTEGRATION_FILTER");
	const char *v22_recording_path =
		getenv("V22_REPLAY_RX_PCM");
	const char *v22_answer_recording_path =
		getenv("V22_ANSWER_REPLAY_RX_PCM");
	const char *v22_raw_answer_recording_path =
		getenv("V22_RAW_ANSWER_REPLAY_RX_PCM");
	const char *v90_recording_path =
		getenv("V90_REPLAY_RX_G711");
	const char *v90_recording_law =
		getenv("V90_REPLAY_LAW");
	const char *v90_recording_role =
		getenv("V90_REPLAY_ROLE");
	const char *v90_recording_pcm_role =
		getenv("V90_REPLAY_PCM_ROLE");
	const char *v90_recording_downstream_minimum_rate =
		getenv("V90_REPLAY_DOWNSTREAM_MIN_RATE");
	const char *v90_recording_downstream_rate =
		getenv("V90_REPLAY_DOWNSTREAM_RATE");
	const char *v90_recording_upstream_minimum_rate =
		getenv("V90_REPLAY_UPSTREAM_MIN_RATE");
	const char *v90_recording_upstream_rate =
		getenv("V90_REPLAY_UPSTREAM_RATE");
	const char *v90_recording_symbol_rate =
		getenv("V90_REPLAY_SYMBOL_RATE");
	const char *v90_recording_link_mode =
		getenv("V90_REPLAY_LINK_MODE");
	const char *v90_recording_receive_gain =
		getenv("V90_REPLAY_RX_GAIN");
	const char *v90_recording_expected_text =
		getenv("V90_REPLAY_EXPECT_TEXT");
	enum media_codec v90_replay_codec;
	bool v90_replay_originator;
	enum v90_modem_role v90_replay_pcm_role;
	bool integration_case_selected = false;
	static const struct modem_test_case test_cases[] = {
		{
			.id = DP_V92, .connected_id = DP_V91,
			.name = "automatic V.91 robbed bits raw",
			.selection_command = "AT&A7\r",
			.caller_tx_rate = 56000, .caller_rx_rate = 56000,
			.answerer_tx_rate = 56000, .answerer_rx_rate = 56000,
			.automode = true, .automatic_pcm_roles = true, .startup_robbed_bits = true,
			.requires_v8 = true, .requires_exact_g711 = true,
			.raw_transport = true, .training_frame_limit = 1000,
		},
		{
			.id = DP_V91, .connected_id = DP_V91,
			.name = "explicit V.91 asymmetric cap",
			.selection_command = "AT&A7+MS=91,1,28000,54667\r",
			.answerer_selection_command = "AT&A7+MS=91,1,28000,64000\r",
			.caller_tx_rate = 64000, .caller_rx_rate = 54667,
			.answerer_tx_rate = 54667, .answerer_rx_rate = 64000,
			.automode = true, .automatic_pcm_roles = true,
			.requires_v8 = true, .requires_exact_g711 = true,
			.require_lapm = true, .require_v44 = true, .training_frame_limit = 1000,
			.cleardown_initiators = 1u,
		},
		{
			.id = DP_V92, .connected_id = DP_V91,
			.name = "automatic V.91 raw",
			.selection_command = "AT&A7\r",
			.caller_tx_rate = 64000, .caller_rx_rate = 64000,
			.answerer_tx_rate = 64000, .answerer_rx_rate = 64000,
			.automode = true, .automatic_pcm_roles = true,
			.requires_v8 = true, .requires_exact_g711 = true,
			.raw_transport = true, .test_retrain = false, .require_v44 = false,
			.training_frame_limit = 1000,
			.impairment = INTEGRATION_IMPAIRMENT_NONE,
		},
		{
			.id = DP_V92, .connected_id = DP_V91,
			.name = "automatic V.91 LAPM retrain",
			.selection_command = "AT&A7\r",
			.caller_tx_rate = 64000, .caller_rx_rate = 64000,
			.answerer_tx_rate = 64000, .answerer_rx_rate = 64000,
			.automode = true, .automatic_pcm_roles = true,
			.requires_v8 = true, .requires_exact_g711 = true,
			.raw_transport = false, .test_retrain = true, .require_v44 = false,
			.training_frame_limit = 1000,
			.impairment = INTEGRATION_IMPAIRMENT_DIGITAL_DELAY,
		},
		{
			.id = DP_V92, .connected_id = DP_V91,
			.name = "automatic V.91 default V44",
			.selection_command = "AT&A7\r",
			.caller_tx_rate = 64000, .caller_rx_rate = 64000,
			.answerer_tx_rate = 64000, .answerer_rx_rate = 64000,
			.automode = true, .automatic_pcm_roles = true,
			.requires_v8 = true, .requires_exact_g711 = true,
			.raw_transport = false, .test_retrain = false, .require_v44 = true,
			.training_frame_limit = 1000,
			.impairment = INTEGRATION_IMPAIRMENT_NONE,
			.cleardown_initiators = 2u,
		},
		{
			.id = DP_V91, .connected_id = DP_V91,
			.name = "explicit V.91 capped",
			.selection_command = "AT&A7+MS=91,1,28000,56000\r",
			.caller_tx_rate = 56000, .caller_rx_rate = 56000,
			.answerer_tx_rate = 56000, .answerer_rx_rate = 56000,
			.automode = true, .automatic_pcm_roles = true,
			.requires_v8 = true, .requires_exact_g711 = true,
			.raw_transport = false, .test_retrain = true, .require_v44 = false,
			.training_frame_limit = 1000,
			.impairment = INTEGRATION_IMPAIRMENT_NONE,
		},
		{
			.id = DP_V22BIS, .answerer_id = DP_V22,
			.name = "automatic V.22bis caller to legacy V.22",
			.connected_id = DP_V22,
			.selection_command = "AT+MS=122,1,300,2400\r",
			.answerer_selection_command = "AT+MS=22,0,1200,1200\r",
			.caller_tx_rate = 1200, .caller_rx_rate = 1200,
			.answerer_tx_rate = 1200, .answerer_rx_rate = 1200,
			.carrier_loss_samples = MODEM_RATE * 7 / 10,
			.automode = true, .answerer_fixed = true, .raw_transport = true,
			.training_frame_limit = 2000,
		},
		{
			.id = DP_V22, .answerer_id = DP_V22BIS,
			.name = "legacy V.22 caller to automatic V.22bis",
			.answerer_connected_id = DP_V22,
			.selection_command = "AT+MS=22,0,1200,1200\r",
			.answerer_selection_command = "AT+MS=122,1,300,2400\r",
			.caller_tx_rate = 1200, .caller_rx_rate = 1200,
			.answerer_tx_rate = 1200, .answerer_rx_rate = 1200,
			.carrier_loss_samples = MODEM_RATE * 7 / 10,
			.answerer_automode = true, .raw_transport = true,
			.training_frame_limit = 2000,
		},

		{
			.id = DP_V32, .answerer_id = DP_V92,
			.name = "legacy auto-answer V.32 raw",
			.answerer_connected_id = DP_V32BIS,
			.selection_command = "AT+MS=32,0,9600,9600\r",
			.answerer_selection_command = "AT+MS=92,1,300,56000\r",
			.caller_tx_rate = 9600, .caller_rx_rate = 9600,
			.answerer_tx_rate = 9600, .answerer_rx_rate = 9600,
			.carrier_loss_samples = MODEM_RATE / 5,
			.training_frame_limit = V32_TRAINING_FRAME_LIMIT,
			.answerer_automode = true, .raw_transport = true,
		},
		{
			.id = DP_V32, .answerer_id = DP_V92,
			.name = "legacy auto-answer V.32 LAPM",
			.answerer_connected_id = DP_V32BIS,
			.selection_command = "AT+MS=32,0,9600,9600\r",
			.answerer_selection_command = "AT+MS=92,1,300,56000\r",
			.caller_tx_rate = 9600, .caller_rx_rate = 9600,
			.answerer_tx_rate = 9600, .answerer_rx_rate = 9600,
			.carrier_loss_samples = MODEM_RATE / 5,
			.training_frame_limit = V32_TRAINING_FRAME_LIMIT,
			.answerer_automode = true, .require_lapm = true,
		},
		{
			.id = DP_V21, .answerer_id = DP_V92,
			.name = "subset auto-answer V.21",
			.test_prelink = true,
			.selection_command = "AT+MS=21,1,300,300\r",
			.answerer_selection_command = "AT+MS=92,1,300,56000\r",
			.caller_tx_rate = 300, .caller_rx_rate = 300,
			.answerer_tx_rate = 300, .answerer_rx_rate = 300,
			.carrier_loss_samples = MODEM_RATE * 7 / 10,
			.automode = true, .raw_transport = true,
		},
		{
			.id = DP_V23, .answerer_id = DP_V92,
			.name = "subset auto-answer V.23",
			.selection_command = "AT+MS=23,1,300,1200\r",
			.answerer_selection_command = "AT+MS=92,1,300,56000\r",
			.caller_tx_rate = 75, .caller_rx_rate = 1200,
			.answerer_tx_rate = 1200, .answerer_rx_rate = 75,
			.carrier_loss_samples = MODEM_RATE * 7 / 10,
			.automode = true, .raw_transport = true,
		},
		{
			.id = DP_V22BIS, .answerer_id = DP_V92,
			.name = "subset auto-answer V.22bis",
			.selection_command = "AT+MS=122,1,2400,2400\r",
			.answerer_selection_command = "AT+MS=92,1,300,56000\r",
			.caller_tx_rate = 2400, .caller_rx_rate = 2400,
			.answerer_tx_rate = 2400, .answerer_rx_rate = 2400,
			.carrier_loss_samples = MODEM_RATE * 7 / 10,
			.automode = true, .require_lapm = true,
		},
		{
			.id = DP_V32, .answerer_id = DP_V92,
			.name = "subset auto-answer V.32",
			.answerer_connected_id = DP_V32BIS,
			.selection_command = "AT+MS=32,1,9600,9600\r",
			.answerer_selection_command = "AT+MS=92,1,300,56000\r",
			.caller_tx_rate = 9600, .caller_rx_rate = 9600,
			.answerer_tx_rate = 9600, .answerer_rx_rate = 9600,
			.carrier_loss_samples = MODEM_RATE / 5,
			.training_frame_limit = V32_TRAINING_FRAME_LIMIT,
			.automode = true, .require_lapm = true,
		},
		{
			.id = DP_V34, .answerer_id = DP_V92,
			.name = "subset auto-answer V.34",
			.selection_command = "AT+MS=34,1,9600,9600\r",
			.answerer_selection_command = "AT+MS=92,1,300,56000\r",
			.caller_tx_rate = 9600, .caller_rx_rate = 9600,
			.answerer_tx_rate = 9600, .answerer_rx_rate = 9600,
			.carrier_loss_samples = MODEM_RATE / 2,
			.training_frame_limit = 1500,
			.automode = true, .require_lapm = true,
		},
		{
			.id = DP_V21,
			.name = "forced V.21 raw ATO1 resume",
			.test_prelink = true,
			.selection_command = "AT+MS=21,0,300,300\r",
			.caller_tx_rate = FSK300_BIT_RATE,
			.caller_rx_rate = FSK300_BIT_RATE,
			.answerer_tx_rate = FSK300_BIT_RATE,
			.answerer_rx_rate = FSK300_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE * 7 / 10,
			.test_retrain = true,
			.raw_transport = true,
		},
		{
			.id = DP_V21,
			.name = "V.21 automode",
			.test_prelink = true,
			.selection_command = "AT+MS=21,1,300,300\r",
			.caller_tx_rate = FSK300_BIT_RATE,
			.caller_rx_rate = FSK300_BIT_RATE,
			.answerer_tx_rate = FSK300_BIT_RATE,
			.answerer_rx_rate = FSK300_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE * 7 / 10,
			.automode = true,
		},
		{
			.id = DP_B103,
			.name = "Bell 103 raw ATO1 resume",
			.test_prelink = true,
			.selection_command = "AT+MS=103,1,300,300\r",
			.caller_tx_rate = FSK300_BIT_RATE,
			.caller_rx_rate = FSK300_BIT_RATE,
			.answerer_tx_rate = FSK300_BIT_RATE,
			.answerer_rx_rate = FSK300_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE * 7 / 10,
			.automode = true,
			.test_retrain = true,
			.raw_transport = true,
		},
		{
			.id = DP_V22,
			.name = "V.22 600 raw ATO1 resume",
			.selection_command = "AT+MS=22,0,600,600\r",
			.caller_tx_rate = V22_600_BIT_RATE,
			.caller_rx_rate = V22_600_BIT_RATE,
			.answerer_tx_rate = V22_600_BIT_RATE,
			.answerer_rx_rate = V22_600_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE * 7 / 10,
			.test_retrain = true,
			.raw_transport = true,
		},
		{
			.id = DP_V22,
			.name = "V.22 raw ATO1 resume",
			.selection_command = "AT+MS=22,1,1200,1200\r",
			.caller_tx_rate = V22_1200_BIT_RATE,
			.caller_rx_rate = V22_1200_BIT_RATE,
			.answerer_tx_rate = V22_1200_BIT_RATE,
			.answerer_rx_rate = V22_1200_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE * 7 / 10,
			.automode = true,
			.test_retrain = true,
			.raw_transport = true,
		},
		{
			.id = DP_V22BIS,
			.name = "V.22bis raw retrain",
			.selection_command = "AT+MS=122,1,2400,2400\r",
			.caller_tx_rate = V22BIS_BIT_RATE,
			.caller_rx_rate = V22BIS_BIT_RATE,
			.answerer_tx_rate = V22BIS_BIT_RATE,
			.answerer_rx_rate = V22BIS_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE * 7 / 10,
			.automode = true,
			.test_retrain = true,
			.raw_transport = true,
		},
		{
			.id = DP_V22BIS,
			.name = "V.22bis delayed rate response",
			.selection_command = "AT+MS=122,0,2400,2400\r",
			.caller_tx_rate = V22BIS_BIT_RATE,
			.caller_rx_rate = V22BIS_BIT_RATE,
			.answerer_tx_rate = V22BIS_BIT_RATE,
			.answerer_rx_rate = V22BIS_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE * 7 / 10,
			.training_frame_limit = 400,
			/* Force the answer S1 beyond the 1.2 s retry boundary. */
			.caller_delay_frames = MAX_CALLER_DELAY_FRAMES,
			.minimum_caller_rate_signals = 1,
			.minimum_caller_rate_signal_sample =
				MAX_CALLER_DELAY_FRAMES * SIP_FRAME_SAMPLES,
			.raw_transport = true,
		},
		{
			.id = DP_V22,
			.name = "V.22 600 LAPM ATO1 resume",
			.selection_command = "AT+MS=22,0,600,600\r",
			.caller_tx_rate = V22_600_BIT_RATE,
			.caller_rx_rate = V22_600_BIT_RATE,
			.answerer_tx_rate = V22_600_BIT_RATE,
			.answerer_rx_rate = V22_600_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE * 7 / 10,
			.test_retrain = true,
		},
		{
			.id = DP_V22,
			.name = "V.22 LAPM ATO1 resume",
			.selection_command = "AT+MS=22,0,1200,1200\r",
			.caller_tx_rate = V22_1200_BIT_RATE,
			.caller_rx_rate = V22_1200_BIT_RATE,
			.answerer_tx_rate = V22_1200_BIT_RATE,
			.answerer_rx_rate = V22_1200_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE * 7 / 10,
			.test_retrain = true,
		},
		{
			.id = DP_V22BIS,
			.name = "V.22bis LAPM retrain",
			.selection_command = "AT+MS=122,0,2400,2400\r",
			.caller_tx_rate = V22BIS_BIT_RATE,
			.caller_rx_rate = V22BIS_BIT_RATE,
			.answerer_tx_rate = V22BIS_BIT_RATE,
			.answerer_rx_rate = V22BIS_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE * 7 / 10,
			.test_retrain = true,
		},
		{
			.id = DP_V32,
			.name = "V.32 4800 LAPM retrain",
			.selection_command = "AT+MS=32,1,4800,4800\r",
			.caller_tx_rate = V32_4800_BIT_RATE,
			.caller_rx_rate = V32_4800_BIT_RATE,
			.answerer_tx_rate = V32_4800_BIT_RATE,
			.answerer_rx_rate = V32_4800_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE / 5,
			.automode = true,
			.test_retrain = true,
		},
		{
			.id = DP_V32,
			.name = "V.32 4800 long-delay LAPM retrain",
			.selection_command = "AT+MS=32,1,4800,4800\r",
			.caller_tx_rate = V32_4800_BIT_RATE,
			.caller_rx_rate = V32_4800_BIT_RATE,
			.answerer_tx_rate = V32_4800_BIT_RATE,
			.answerer_rx_rate = V32_4800_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE / 5,
			.training_frame_limit = V32_LONG_TRAINING_FRAME_LIMIT,
			.caller_delay_frames = V32_LONG_DELAY_FRAMES,
			.automode = true,
			.test_retrain = true,
		},
		{
			.id = DP_V32,
			.name = "V.32 4800 noisy conditioning",
			.selection_command = "AT+MS=32,1,4800,4800\r",
			.caller_tx_rate = V32_4800_BIT_RATE,
			.caller_rx_rate = V32_4800_BIT_RATE,
			.answerer_tx_rate = V32_4800_BIT_RATE,
			.answerer_rx_rate = V32_4800_BIT_RATE,
			.automode = true,
			.require_lapm = true,
			.test_retrain = true,
			.impairment = INTEGRATION_IMPAIRMENT_TRAINING_NOISE,
		},
		{
			.id = DP_V32BIS,
			.name = "V.32bis 7200 LAPM retrain",
			.selection_command = "AT+MS=132,1,7200,7200\r",
			.caller_tx_rate = V32_7200_BIT_RATE,
			.caller_rx_rate = V32_7200_BIT_RATE,
			.answerer_tx_rate = V32_7200_BIT_RATE,
			.answerer_rx_rate = V32_7200_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE / 5,
			.automode = true,
			.test_retrain = true,
		},
		{
			.id = DP_V32,
			.name = "V.32 9600 LAPM retrain",
			.selection_command = "AT+MS=32,1,9600,9600\r",
			.caller_tx_rate = V32_9600_BIT_RATE,
			.caller_rx_rate = V32_9600_BIT_RATE,
			.answerer_tx_rate = V32_9600_BIT_RATE,
			.answerer_rx_rate = V32_9600_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE / 5,
			.automode = true,
			.test_retrain = true,
		},
		{
			.id = DP_V32,
			.name = "V.32 default range stays at 9600",
			.selection_command = "AT+MS=32,0,300,56000\r",
			.caller_tx_rate = V32_9600_BIT_RATE,
			.caller_rx_rate = V32_9600_BIT_RATE,
			.answerer_tx_rate = V32_9600_BIT_RATE,
			.answerer_rx_rate = V32_9600_BIT_RATE,
			.raw_transport = true,
		},
		{
			.id = DP_V32,
			.name = "V.32 9600 raw retrain",
			.selection_command = "AT+MS=32,1,9600,9600\r",
			.caller_tx_rate = V32_9600_BIT_RATE,
			.caller_rx_rate = V32_9600_BIT_RATE,
			.answerer_tx_rate = V32_9600_BIT_RATE,
			.answerer_rx_rate = V32_9600_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE / 5,
			.automode = true,
			.test_retrain = true,
			.raw_transport = true,
		},
		{
			.id = DP_V32BIS,
			.name = "V.32bis 12000 LAPM retrain",
			.selection_command = "AT+MS=132,1,12000,12000\r",
			.caller_tx_rate = V32_12000_BIT_RATE,
			.caller_rx_rate = V32_12000_BIT_RATE,
			.answerer_tx_rate = V32_12000_BIT_RATE,
			.answerer_rx_rate = V32_12000_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE / 5,
			.automode = true,
			.test_retrain = true,
		},
		{
			.id = DP_V32BIS,
			.name = "V.32bis 14400 LAPM retrain",
			.selection_command = "AT+MS=132,1,14400,14400\r",
			.caller_tx_rate = V32_14400_BIT_RATE,
			.caller_rx_rate = V32_14400_BIT_RATE,
			.answerer_tx_rate = V32_14400_BIT_RATE,
			.answerer_rx_rate = V32_14400_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE / 5,
			.automode = true,
			.test_retrain = true,
		},
		{
			.id = DP_V32BIS,
			.name = "V.32bis 7200 impaired LAPM retrain",
			.selection_command = "AT+MS=132,1,7200,7200\r",
			.caller_tx_rate = V32_7200_BIT_RATE,
			.caller_rx_rate = V32_7200_BIT_RATE,
			.answerer_tx_rate = V32_7200_BIT_RATE,
			.answerer_rx_rate = V32_7200_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE / 5,
			.automode = true,
			.test_retrain = true,
			.impairment = INTEGRATION_IMPAIRMENT_COMPOSITE,
		},
		{
			.id = DP_V32BIS,
			.name = "V.32bis 14400 impaired LAPM retrain",
			.selection_command = "AT+MS=132,1,14400,14400\r",
			.caller_tx_rate = V32_14400_BIT_RATE,
			.caller_rx_rate = V32_14400_BIT_RATE,
			.answerer_tx_rate = V32_14400_BIT_RATE,
			.answerer_rx_rate = V32_14400_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE / 5,
			.automode = true,
			.test_retrain = true,
			.impairment = INTEGRATION_IMPAIRMENT_COMPOSITE,
		},
		{
			.id = DP_V32BIS,
			.name = "V.32bis 14400 impaired raw retrain",
			.selection_command = "AT+MS=132,1,14400,14400\r",
			.caller_tx_rate = V32_14400_BIT_RATE,
			.caller_rx_rate = V32_14400_BIT_RATE,
			.answerer_tx_rate = V32_14400_BIT_RATE,
			.answerer_rx_rate = V32_14400_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE / 5,
			.automode = true,
			.test_retrain = true,
			.raw_transport = true,
			.impairment = INTEGRATION_IMPAIRMENT_COMPOSITE,
		},
		{
			.id = DP_V34,
			.name = "V.34 short-frame 2400 raw retrain",
			.selection_command = "AT+MS=34,0,2400,2400+V34B=2400\r",
			.caller_tx_rate = 2400,
			.caller_rx_rate = 2400,
			.answerer_tx_rate = 2400,
			.answerer_rx_rate = 2400,
			.carrier_loss_samples = MODEM_RATE / 2,
			.training_frame_limit = 1500,
			.requires_v8 = true,
			.test_retrain = true,
			.raw_transport = true,
		},
		{
			.id = DP_V34,
			.name = "V.34 short-frame 4800 raw retrain",
			.selection_command = "AT+MS=34,0,4800,4800+V34B=3429\r",
			.caller_tx_rate = 4800,
			.caller_rx_rate = 4800,
			.answerer_tx_rate = 4800,
			.answerer_rx_rate = 4800,
			.carrier_loss_samples = MODEM_RATE / 2,
			.training_frame_limit = 1500,
			.requires_v8 = true,
			.test_retrain = true,
			.raw_transport = true,
		},
		{
			.id = DP_V34,
			.name = "V.34 9600 raw retrain",
			.selection_command = "AT+MS=34,1,9600,9600\r",
			.caller_tx_rate = 9600,
			.caller_rx_rate = 9600,
			.answerer_tx_rate = 9600,
			.answerer_rx_rate = 9600,
			.carrier_loss_samples = MODEM_RATE / 2,
			.training_frame_limit = 1500,
			.automode = true,
			.test_retrain = true,
			.raw_transport = true,
		},
		{
			.id = DP_V34,
			.name = "V.34 9600 raw ATH0 cleardown",
			.selection_command = "AT+MS=34,1,9600,9600\r",
			.caller_tx_rate = 9600,
			.caller_rx_rate = 9600,
			.answerer_tx_rate = 9600,
			.answerer_rx_rate = 9600,
			.training_frame_limit = 1500,
			.automode = true,
			.cleardown_initiators = 3u,
			.raw_transport = true,
		},
		{
			.id = DP_V34,
			.name = "V.34 9600 LAPM ATH0 cleardown",
			.selection_command = "AT+MS=34,1,9600,9600\r",
			.caller_tx_rate = 9600,
			.caller_rx_rate = 9600,
			.answerer_tx_rate = 9600,
			.answerer_rx_rate = 9600,
			.training_frame_limit = 1500,
			.automode = true,
			.cleardown_initiators = 3u,
			.require_lapm = true,
		},
		{
			.id = DP_V34,
			.name = "V.34 21600 LAPM retrain",
			.selection_command =
				"AT+MS=34,0,21600,21600+V34B=2743\r",
			.caller_tx_rate = 21600,
			.caller_rx_rate = 21600,
			.answerer_tx_rate = 21600,
			.answerer_rx_rate = 21600,
			.carrier_loss_samples = MODEM_RATE / 2,
			.training_frame_limit = 1500,
			.requires_v8 = true,
			.test_retrain = true,
		},
		{
			.id = DP_V34,
			.name = "V.34 9600 generated impairment",
			.selection_command = "AT+MS=34,1,9600,9600\r",
			.caller_tx_rate = 9600,
			.caller_rx_rate = 9600,
			.answerer_tx_rate = 9600,
			.answerer_rx_rate = 9600,
			.carrier_loss_samples = MODEM_RATE / 2,
			.training_frame_limit = 1500,
			.automode = true,
			.require_lapm = true,
			.impairment = INTEGRATION_IMPAIRMENT_COMPOSITE,
		},
		{
			.id = DP_V90,
			.name = "V.90 28000 downstream",
			.selection_command = "AT+MS=90,0,28000,28000\r",
			.caller_tx_rate = 33600,
			.caller_rx_rate = V90_PROFILE_MIN_DOWNSTREAM_BIT_RATE,
			.answerer_tx_rate = V90_PROFILE_MIN_DOWNSTREAM_BIT_RATE,
			.answerer_rx_rate = 33600,
			.training_frame_limit = 1500,
			.requires_v8 = true,
			.requires_exact_g711 = true,
			.impairment = INTEGRATION_IMPAIRMENT_DIGITAL_DELAY,
		},
		{
			.id = DP_V90,
			.name = "V.90 28000 downstream with digital caller",
			.selection_command = "AT+MS=90,0,28000,28000\r",
			.caller_tx_rate = V90_PROFILE_MIN_DOWNSTREAM_BIT_RATE,
			/* The digital-caller policy omits optional 3429-baud probing. */
			.caller_rx_rate = 31200,
			.answerer_tx_rate = 31200,
			.answerer_rx_rate = V90_PROFILE_MIN_DOWNSTREAM_BIT_RATE,
			.training_frame_limit = 1500,
			.requires_v8 = true,
			.requires_exact_g711 = true,
			.digital_caller = true,
			.impairment = INTEGRATION_IMPAIRMENT_DIGITAL_DELAY,
		},
		{
			.id = DP_V92,
			.name = "V.92 28000 PCM duplex raw ATH0 cleardown",
			.selection_command =
				"AT+MS=92,0,28000,28000+V92Q=0+V92U=1,28000\r",
			.caller_tx_rate = 28000,
			.caller_rx_rate = 28000,
			.answerer_tx_rate = 28000,
			.answerer_rx_rate = 28000,
			.training_frame_limit = 2500,
			.requires_v8 = true,
			.requires_exact_g711 = true,
			.raw_transport = true,
			.cleardown_initiators = 1u,
		},
		{
			.id = DP_V92,
			.name = "V.92 28000 PCM duplex raw ATH0 cleardown with digital caller",
			.selection_command =
				"AT+MS=92,0,28000,28000+V92Q=0+V92U=1,28000\r",
			.caller_tx_rate = 28000,
			.caller_rx_rate = 28000,
			.answerer_tx_rate = 28000,
			.answerer_rx_rate = 28000,
			.training_frame_limit = 2500,
			.requires_v8 = true,
			.requires_exact_g711 = true,
			.raw_transport = true,
			.digital_caller = true,
			.cleardown_initiators = 1u,
		},
		{
			.id = DP_V92,
			.name = "V.92 28000 PCM duplex LAPM rate exchange and retrain",
			.cleardown_initiators = 1u,
			.selection_command =
				"AT+MS=92,0,28000,28000+V92Q=0+V92U=1,28000\r",
			.caller_tx_rate = 28000,
			.caller_rx_rate = 28000,
			.answerer_tx_rate = 28000,
			.answerer_rx_rate = 28000,
			.training_frame_limit = 2500,
			.requires_v8 = true,
			.requires_exact_g711 = true,
			.test_retrain = true,
		},
		{
			.id = DP_V92,
			.name = "V.92 52000/40000 PCM duplex LAPM rate exchange and retrain with digital caller",
			.cleardown_initiators = 1u,
			.selection_command =
				"AT+MS=92,0,52000,52000+V92Q=0+V92U=1,40000\r",
			.caller_tx_rate = 52000,
			.caller_rx_rate = 40000,
			.answerer_tx_rate = 40000,
			.answerer_rx_rate = 52000,
			.training_frame_limit = 2500,
			.requires_v8 = true,
			.requires_exact_g711 = true,
			.test_retrain = true,
			.digital_caller = true,
		},
		{
			.id = DP_V92,
			.name = "V.92 50667/40000 PCM duplex LAPM with U69 training and retrain",
			.cleardown_initiators = 1u,
			.selection_command =
				"AT+MS=92,0,50667,50667+V92Q=0+V92U=1,40000,69\r",
			.caller_tx_rate = 40000,
			.caller_rx_rate = 50667,
			.answerer_tx_rate = 50667,
			.answerer_rx_rate = 40000,
			.training_frame_limit = 2500,
			.requires_v8 = true,
			.requires_exact_g711 = true,
			.test_retrain = true,
		},
		{
			.id = DP_V92,
			.name = "V.92 28000 PCM duplex V.44 rate exchange",
			.cleardown_initiators = 2u,
			.selection_command =
				"AT+MS=92,0,28000,28000+V92Q=0+V92U=1,28000+DS=3,2\r",
			.caller_tx_rate = 28000,
			.caller_rx_rate = 28000,
			.answerer_tx_rate = 28000,
			.answerer_rx_rate = 28000,
			.training_frame_limit = 2500,
			.requires_v8 = true,
			.requires_exact_g711 = true,
			.require_lapm = true,
			.require_v44 = true,
		},
		{
			.id = DP_V92,
			.name = "V.92 28000 PCM duplex V.44 rate exchange with digital caller",
			.cleardown_initiators = 2u,
			.selection_command =
				"AT+MS=92,0,28000,28000+V92Q=0+V92U=1,28000+DS=3,2\r",
			.caller_tx_rate = 28000,
			.caller_rx_rate = 28000,
			.answerer_tx_rate = 28000,
			.answerer_rx_rate = 28000,
			.training_frame_limit = 2500,
			.requires_v8 = true,
			.requires_exact_g711 = true,
			.require_lapm = true,
			.require_v44 = true,
			.digital_caller = true,
		},
		{
			.id = DP_V90,
			.answerer_id = DP_V34,
			.connected_id = DP_V34,
			.name = "V.90 28000 selection fallback to V.34",
			.selection_command =
				"AT+MS=90,0,28000,28000+V90U=14400,14400,3000\r",
			.answerer_selection_command =
				"AT+MS=34,0,26400,26400+V34B=3000\r",
			.caller_tx_rate = 26400,
			.caller_rx_rate = 26400,
			.answerer_tx_rate = 26400,
			.answerer_rx_rate = 26400,
			.training_frame_limit = 1500,
			.requires_v8 = true,
		},
		{
			.id = DP_B212,
			.name = "Bell 212A LAPM ATO1 resume",
			.selection_command = "AT+MS=212,1,1200,1200\r",
			.caller_tx_rate = V22_1200_BIT_RATE,
			.caller_rx_rate = V22_1200_BIT_RATE,
			.answerer_tx_rate = V22_1200_BIT_RATE,
			.answerer_rx_rate = V22_1200_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE * 7 / 10,
			.automode = true,
			.test_retrain = true,
		},
		{
			.id = DP_V23,
			.name = "V.23 raw ATO1 resume",
			.selection_command = "AT+MS=23,1,300,1200\r",
			.caller_tx_rate = V23_BACKWARD_BIT_RATE,
			.caller_rx_rate = V23_FORWARD_BIT_RATE,
			.answerer_tx_rate = V23_FORWARD_BIT_RATE,
			.answerer_rx_rate = V23_BACKWARD_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE * 7 / 10,
			.automode = true,
			.test_prelink = true,
			.test_retrain = true,
			.raw_transport = true,
		},
	};
	static const enum media_codec codecs[] = {
		MEDIA_CODEC_PCMU,
		MEDIA_CODEC_PCMA,
	};
	static const enum baudot_rate baudot_rates[] = {
		BAUDOT_RATE_45,
		BAUDOT_RATE_50,
	};

	if (v34_matrix_filter != NULL && v34_matrix_filter[0] != '\0')
		return run_v34_rate_matrix(v34_matrix_filter) ? 0 : 1;
	fprintf(stderr, "[ RUN      ] continuous 300-bit/s FSK mark qualification\n");
	if (!test_fsk300_mark_qualification())
		return 1;
	fprintf(stderr, "[       OK ] continuous 300-bit/s FSK mark qualification\n");
	fprintf(stderr, "[ RUN      ] raw FSK startup carrier withdrawal\n");
	if (!test_fsk_startup_carrier_withdrawal())
		return 1;
	fprintf(stderr, "[       OK ] raw FSK startup carrier withdrawal\n");
	fprintf(stderr, "[ RUN      ] bounded recording replay output\n");
	if (!run_recording_replay_output()) {
		fprintf(stderr, "[  FAILED  ] bounded recording replay output\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] bounded recording replay output\n");
	fprintf(stderr, "[ RUN      ] V.22 answer response deadline\n");
	if (!run_v22_answer_response_deadline()) {
		fprintf(stderr, "[  FAILED  ] V.22 answer response deadline\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] V.22 answer response deadline\n");
	fprintf(stderr, "[ RUN      ] V.22 scrambled mark qualification\n");
	if (!run_v22_mark_qualification())
		return 1;
	fprintf(stderr, "[       OK ] V.22 scrambled mark qualification\n");
	fprintf(stderr, "[ RUN      ] abandoned V.22 calling probe\n");
	if (!run_v22_abandoned_probe()) {
		fprintf(stderr, "[  FAILED  ] abandoned V.22 calling probe\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] abandoned V.22 calling probe\n");
	if (v32_recording_path == NULL || v32_recording_path[0] == '\0') {
		fprintf(stderr,
			"[  SKIPPED ] V.32 recording replay (V32_REPLAY_RX_PCM unset)\n");
	} else if (!run_v32_recording_replay(
			   v32_recording_path, v32_recording_expected_text)) {
		return 1;
	}
	if (v22_recording_path == NULL || v22_recording_path[0] == '\0') {
		fprintf(stderr,
			"[  SKIPPED ] V.22bis recording replay (V22_REPLAY_RX_PCM unset)\n");
	} else if (!run_v22bis_recording_replay(
			   v22_recording_path, false)) {
		return 1;
	}
	if (v22_answer_recording_path == NULL ||
	    v22_answer_recording_path[0] == '\0') {
		fprintf(stderr,
			"[  SKIPPED ] V.22bis answer recording replay (V22_ANSWER_REPLAY_RX_PCM unset)\n");
	} else if (!run_v22bis_recording_replay(
			   v22_answer_recording_path, true)) {
		return 1;
	}
	if (v22_raw_answer_recording_path == NULL ||
	    v22_raw_answer_recording_path[0] == '\0') {
		fprintf(stderr,
			"[  SKIPPED ] raw V.22 answer recording replay (V22_RAW_ANSWER_REPLAY_RX_PCM unset)\n");
	} else if (!run_v22_raw_answer_recording_replay(
			   v22_raw_answer_recording_path)) {
		return 1;
	}
	if (v34_recording_path == NULL || v34_recording_path[0] == '\0') {
		fprintf(stderr,
			"[  SKIPPED ] V.34 recording replay (V34_REPLAY_RX_PCM unset)\n");
	} else if (!run_v34_recording_replay(
			   v34_recording_path, v34_recording_bit_rate,
			   v34_recording_symbol_rate,
			   v34_recording_link_mode,
			   v34_recording_retrain_sample,
			   v34_recording_expected_text, false)) {
		return 1;
	}
	if (v34_answer_recording_path == NULL ||
	    v34_answer_recording_path[0] == '\0') {
		fprintf(stderr,
			"[  SKIPPED ] V.34 answer recording replay (V34_ANSWER_REPLAY_RX_PCM unset)\n");
	} else if (!run_v34_recording_replay(
			   v34_answer_recording_path,
			   v34_answer_recording_bit_rate,
			   v34_answer_recording_symbol_rate,
			   v34_answer_recording_link_mode,
			   v34_answer_recording_retrain_sample,
			   v34_answer_recording_expected_text, true)) {
		return 1;
	}
	if (v90_recording_path == NULL || v90_recording_path[0] == '\0') {
		fprintf(stderr,
			"[  SKIPPED ] V.90 recording replay (V90_REPLAY_RX_G711 unset)\n");
	} else if (!v90_replay_codec_parse(
			   v90_recording_law, &v90_replay_codec) ||
		   !v90_replay_role_parse(
			   v90_recording_role, &v90_replay_originator) ||
		   !v90_replay_pcm_role_parse(
			   v90_recording_pcm_role, v90_replay_originator,
			   &v90_replay_pcm_role)) {
		fprintf(stderr, "invalid V.90 recording replay configuration\n");
		return 1;
	} else if (!run_v90_recording_replay(
			   v90_recording_path, v90_replay_codec,
			   v90_replay_originator, v90_replay_pcm_role,
			   v90_recording_downstream_minimum_rate,
			   v90_recording_downstream_rate,
			   v90_recording_upstream_minimum_rate,
			   v90_recording_upstream_rate,
			   v90_recording_symbol_rate,
			   v90_recording_link_mode,
			   v90_recording_receive_gain,
			   v90_recording_expected_text)) {
		return 1;
	}
	if (!run_factory_pcm_cases(integration_filter, &integration_case_selected))
		return 1;
	if (!run_factory_startup_cases(integration_filter, &integration_case_selected))
		return 1;
	for (size_t standard = 0; standard < ARRAY_LENGTH(test_cases);
	     ++standard) {
		if (integration_filter != NULL && integration_filter[0] != '\0' &&
		    strstr(test_cases[standard].name, integration_filter) == NULL)
			continue;
		integration_case_selected = true;
		for (size_t codec = 0; codec < ARRAY_LENGTH(codecs); ++codec) {
			fprintf(stderr, "[ RUN      ] %s over %s SIP media\n",
				test_cases[standard].name,
				codec_name(codecs[codec]));
			if (!run_media_call(
				    &test_cases[standard], codecs[codec])) {
				fprintf(stderr, "[  FAILED  ] %s over %s SIP media\n",
					test_cases[standard].name,
					codec_name(codecs[codec]));
				return 1;
			}
			fprintf(stderr, "[       OK ] %s over %s SIP media\n",
				test_cases[standard].name,
				codec_name(codecs[codec]));
		}
	}
	if (integration_filter != NULL && integration_filter[0] != '\0') {
		if (!integration_case_selected) {
			fprintf(stderr,
				"[  FAILED  ] no integration case matched '%s'\n",
				integration_filter);
			return 1;
		}
		return 0;
	}
	if (!skip_v34_matrix && !run_v34_rate_matrix(NULL))
		return 1;
	for (size_t codec = 0; codec < ARRAY_LENGTH(codecs); ++codec) {
		for (size_t rate = 0; rate < ARRAY_LENGTH(baudot_rates);
		     ++rate) {
			const char *rate_name =
				baudot_rates[rate] == BAUDOT_RATE_50 ?
					"50" : "45.45";

			fprintf(stderr,
				"[ RUN      ] Baudot %s over %s SIP media\n",
				rate_name, codec_name(codecs[codec]));
			if (!run_baudot_media_call(
				    baudot_rates[rate], codecs[codec])) {
				fprintf(stderr,
					"[  FAILED  ] Baudot %s over %s SIP media\n",
					rate_name, codec_name(codecs[codec]));
				return 1;
			}
			fprintf(stderr,
				"[       OK ] Baudot %s over %s SIP media\n",
				rate_name, codec_name(codecs[codec]));
		}
	}
	return 0;
}
