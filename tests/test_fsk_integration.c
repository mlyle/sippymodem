// SPDX-License-Identifier: BSD-3-Clause

#include <modem.h>
#include <baudot.h>
#include <datapumps.h>
#include <fsk300_datapump.h>
#include <g711.h>
#include <media.h>
#include <qam.h>
#include <v22_datapump.h>
#include <v22_framing.h>
#include <v23_datapump.h>
#include <datapump.h>
#include <v8_datapump.h>
#include <v34_datapump.h>
#include <v34_frame_schedule.h>
#include <v90_profile.h>

#include "channel_bench.h"
#include "test.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
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
	RETRAIN_RESPONSE_LOSS_FRAMES = 70,
	V22BIS_INITIAL_DELAY_FRAMES = 70,
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
	size_t delayed_codeword_cursor;
	bool delay_codewords;
};

struct recording_capture {
	size_t blocks[RECORD_TX_RTP_PAYLOAD_SENT + 1];
	size_t units[RECORD_TX_RTP_PAYLOAD_SENT + 1];
	bool invalid;
};

struct media_frame_delay {
	uint8_t codewords[V22BIS_INITIAL_DELAY_FRAMES][SIP_FRAME_SAMPLES];
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
};

struct modem_test_case {
	enum DP_ID id;
	const char *name;
	const char *selection_command;
	unsigned int caller_tx_rate;
	unsigned int caller_rx_rate;
	unsigned int answerer_tx_rate;
	unsigned int answerer_rx_rate;
	unsigned int carrier_loss_samples;
	unsigned int training_frame_limit;
	unsigned int initial_caller_delay_frames;
	unsigned int minimum_caller_rate_signals;
	unsigned int minimum_caller_rate_signal_sample;
	bool automode;
	bool requires_v8;
	bool requires_exact_g711;
	bool test_prelink;
	bool test_retrain;
	bool raw_transport;
	enum integration_impairment impairment;
};

extern int process_at_command(struct modem *modem, char *command);

static int start_modem(struct modem *modem)
{
	(void)modem;
	return 0;
}

static int stop_modem(struct modem *modem)
{
	(void)modem;
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
	endpoint->modem = modem_create(&integration_driver, name);
	if (endpoint->modem == NULL) {
		close(sockets[1]);
		close(sockets[0]);
		return false;
	}
	endpoint->modem_fd = sockets[0];
	endpoint->client_fd = sockets[1];
	endpoint->modem->pty = endpoint->modem_fd;
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

static bool process_transmit_frame(
	struct integration_endpoint *endpoint, enum media_codec codec,
	const struct media_recorder *recorder)
{
	enum g711_law law;
	bool exact_output = false;
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
	modem_process_g711(
		endpoint->modem, law, endpoint->receive_codewords,
		endpoint->receive, endpoint->transmit_codewords,
		endpoint->transmit, SIP_FRAME_SAMPLES, &exact_output);
	if (!exact_output)
		return media_encode(
			       codec, endpoint->transmit, SIP_FRAME_SAMPLES,
			       endpoint->transmit_codewords,
			       ARRAY_LENGTH(endpoint->transmit_codewords),
			       recorder) == MEDIA_OK;
	for (size_t index = 0; index < SIP_FRAME_SAMPLES; ++index)
		if (endpoint->transmit[index] != decode_codeword(
				law, endpoint->transmit_codewords[index]))
			return false;
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
			.kind = CHANNEL_BENCH_ECHO,
			.parameters.echo = {
				.delay_samples = 8,
				.gain_q15 = 256,
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

static bool reject_unavailable_retrain(
	struct integration_endpoint *endpoint, enum DP_ID id)
{
	enum {
		TEST_STATE_MODEM_ONLINE = 5,
		TEST_STATE_COMMAND_ONLINE = 6,
	};
	char retrain_command[] = "ATO1";
	char resume_command[] = "ATO0";
	struct dp *datapump;
	uint32_t pending_data;
	unsigned int status;
	int pending_bits;
	int pending_chars;

	if (endpoint == NULL || !modem_is_online(endpoint->modem, id))
		return false;
	datapump = endpoint->modem->dp;
	status = datapump->status;
	pending_bits = endpoint->modem->pack.bit;
	pending_data = endpoint->modem->pack.data;
	pending_chars = endpoint->modem->xmit.count;
	endpoint->output_count = 0;
	endpoint->modem->state = TEST_STATE_COMMAND_ONLINE;
	endpoint->modem->command = 1;
	if (process_at_command(endpoint->modem, retrain_command) != -1 ||
	    endpoint->modem->state != TEST_STATE_COMMAND_ONLINE ||
	    endpoint->modem->command != 1 ||
	    endpoint->modem->retrain_pending != 0 ||
	    endpoint->modem->dp != datapump || datapump->status != status ||
	    endpoint->modem->pack.bit != pending_bits ||
	    endpoint->modem->pack.data != pending_data ||
	    endpoint->modem->xmit.count != pending_chars)
		return false;
	if (process_at_command(endpoint->modem, resume_command) != 1 ||
	    endpoint->modem->state != TEST_STATE_MODEM_ONLINE ||
	    endpoint->modem->command != 0 ||
	    endpoint->modem->retrain_pending != 0 ||
	    endpoint->modem->dp != datapump || datapump->status != status ||
	    !endpoint_drain(endpoint) || !output_contains(endpoint, "CONNECT"))
		return false;
	endpoint->output_count = 0;
	return true;
}

static bool exercise_retrain(
	struct integration_endpoint *caller,
	struct integration_endpoint *answerer,
	struct integration_endpoint *initiator,
	enum DP_ID id, unsigned int bit_rate,
	enum media_codec codec,
	const struct media_recorder *recorder,
	unsigned int sequence, bool raw_transport, bool lose_first_response,
	bool reduce_peer_response)
{
	enum {
		TEST_STATE_COMMAND_ONLINE = 6,
		PRESERVED_CAPACITY = 32,
	};
	static const uint8_t caller_payloads[][8] = {
		{ 'c', 'a', 'l', 'l', '-', 'r', '1', 0x81 },
		{ 'c', 'a', 'l', 'l', '-', 'r', '2', 0x82 },
	};
	static const uint8_t answerer_payloads[][8] = {
		{ 'a', 'n', 's', '-', 'r', '1', 0x00, 0xfe },
		{ 'a', 'n', 's', '-', 'r', '2', 0x01, 0xff },
	};
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
	char command[] = "ATO1";
	size_t caller_preserved_offset;
	size_t answerer_preserved_offset;
	size_t pre_retrain_length;
	size_t preserved_length;
	uint32_t pending_transmit_data = 0;
	int pending_transmit_bits = 0;
	int pending_transmit_chars = 0;

	if (caller == NULL || answerer == NULL || initiator == NULL ||
	    sequence >= ARRAY_LENGTH(caller_payloads))
		return false;
	caller->output_count = 0;
	answerer->output_count = 0;
	pre_retrain_length = raw_transport ? bit_rate / 50u / 10u : 0;
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
	    !process_sip_frame(caller, answerer, codec, recorder) ||
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
	initiator->modem->state = TEST_STATE_COMMAND_ONLINE;
	initiator->modem->command = 1;
	if (process_at_command(initiator->modem, command) != 1 ||
	    initiator->modem->retrain_pending == 0 ||
	    !endpoint_drain(caller) || !endpoint_drain(answerer) ||
	    caller->output_count != 0 || answerer->output_count != 0) {
		fprintf(stderr,
			"retrain request failed (pending=%u outputs=%zu/%zu)\n",
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
	     frame < (id == DP_V32 ?
			      V32_RETRAIN_FRAME_LIMIT : RETRAIN_FRAME_LIMIT);
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

		if (!process_sip_frame_with_loss(
			    caller, answerer, codec, recorder,
			    block_caller_to_answerer,
			    block_answerer_to_caller,
			    reduce_caller_to_answerer,
			    reduce_answerer_to_caller) ||
		    !endpoint_drain(caller) || !endpoint_drain(answerer))
			return false;
		if (initiator->modem->retrain_pending == 0 &&
		    modem_is_online(caller->modem, id) &&
		    modem_is_online(answerer->modem, id)) {
			completed = true;
			break;
		}
	}
	if (!completed ||
	    !output_contains_connect(initiator, bit_rate)) {
		fprintf(stderr,
			"retrain carrier failed (completed=%d pending=%u "
			"status=%u/%u state=%u/%u outputs=%zu/%zu)\n",
			completed, initiator->modem->retrain_pending,
			caller->modem->dp != NULL ? caller->modem->dp->status : 0,
			answerer->modem->dp != NULL ? answerer->modem->dp->status : 0,
			caller->modem->state, answerer->modem->state,
			caller->output_count, answerer->output_count);
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
		if (!process_sip_frame(caller, answerer, codec, recorder) ||
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

static bool exercise_v22bis_carrier_recovery(
	struct integration_endpoint *caller,
	struct integration_endpoint *answerer,
	enum media_codec codec, const struct media_recorder *recorder,
	bool raw_transport)
{
	enum {
		SHORT_LOSS_FRAMES = 2,
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
	for (unsigned int recovery = raw_transport ? 0u : 1u;
	     recovery < 2u; ++recovery) {
		bool transferred = false;

		caller->output_count = 0;
		answerer->output_count = 0;
		if (recovery == 0) {
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

static bool run_media_call(
	const struct modem_test_case *test_case,
	enum media_codec codec)
{
	static const char prelink_payload[] = "early-v23";
	static const uint8_t caller_payload[] = {
		0x00, 0x01, 0x55, 0xaa, 0x7f, 0x80, 0xfe, 0xff,
		'V', '2', '1', 'A',
	};
	static const uint8_t answer_payload[] = {
		0xff, 0x81, 0x42, 0x24, 0x18, 0x00, 0x7e, 0xe7,
		'S', 'I', 'P', 'B',
	};
	static const char dial_command[] = "ATDpeer\r";
	struct datapumps datapumps = { 0 };
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
	bool online = false;
	bool answerer_used_v8 = !uses_v8;
	bool prelink_queued = false;
	bool prelink_delivered = false;
	bool transferred = false;
	bool answerer_signal_seen = false;
	unsigned int answerer_silence_frames = 0;
	unsigned int answerer_silence_max = 0;
	enum DP_ID initial_datapump = uses_v8 ? DP_V8 : test_case->id;
	unsigned int training_frame_limit =
		test_case->training_frame_limit != 0 ?
			test_case->training_frame_limit :
			(test_case->id == DP_V32 ? V32_TRAINING_FRAME_LIMIT :
			 uses_v8 ? NEGOTIATED_TRAINING_FRAME_LIMIT :
				   TRAINING_FRAME_LIMIT);
	int selection_length = (int)strlen(test_case->selection_command);
	static const char raw_command[] = "AT\\N0\r";
	static const char lapm_command[] = "AT\\N4\r";
	static const char no_compression_command[] = "AT+DS=0,0\r";
	bool monitor_caller_rate =
		test_case->minimum_caller_rate_signals != 0;

	CHECK(datapumps_start(&datapumps) ==
		      DATAPUMPS_OK,
	      "%s integration registrations did not start", test_case->name);
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
	if (v8_datapump_uses_pcm(test_case->id)) {
		CHECK(modem_set_sreg(
			      caller.modem, SREG_V90_DIGITAL_INDICATION, 0) == 0 &&
		      modem_set_sreg(
			      answerer.modem,
			      SREG_V90_DIGITAL_INDICATION, 1) == 0,
		      "%s integration endpoints did not accept PCM roles",
		      test_case->name);
	}
	CHECK(MODEM_DP(caller.modem) == DP_V21 &&
		      MODEM_DP(answerer.modem) == DP_V21 &&
		      MODEM_AUTOMODE(caller.modem) == 0 &&
		      MODEM_AUTOMODE(answerer.modem) == 0,
	      "source-built modem did not default to its available capability");
	if (test_case->raw_transport) {
		CHECK(modem_write(
			      caller.modem, raw_command,
			      (int)(sizeof(raw_command) - 1u)) ==
			      (int)(sizeof(raw_command) - 1u) &&
		      modem_write(
			      answerer.modem, raw_command,
			      (int)(sizeof(raw_command) - 1u)) ==
			      (int)(sizeof(raw_command) - 1u) &&
		      endpoint_drain(&caller) && endpoint_drain(&answerer) &&
		      output_contains(&caller, "OK") &&
		      output_contains(&answerer, "OK"),
		      "%s did not accept raw transport", test_case->name);
		caller.output_count = 0;
		answerer.output_count = 0;
	}
	if (test_case->test_retrain && !test_case->raw_transport) {
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
		      "%s did not require LAPM for the retrain test",
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
			      answerer.modem, test_case->selection_command,
			      selection_length) == selection_length &&
		      MODEM_DP(caller.modem) == test_case->id &&
		      MODEM_DP(answerer.modem) == test_case->id &&
		      MODEM_AUTOMODE(caller.modem) == test_case->automode &&
		      MODEM_AUTOMODE(answerer.modem) == test_case->automode &&
		      endpoint_drain(&caller) && endpoint_drain(&answerer) &&
		      output_contains(&caller, "OK") &&
		      output_contains(&answerer, "OK"),
	      "%s was not selectable through AT+MS", test_case->name);
	caller.output_count = 0;
	answerer.output_count = 0;
	CHECK(modem_set_sreg(answerer.modem, SREG_ANS_DELAY, 0) == 0 &&
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
		      modem_answer(answerer.modem) == 0,
	      "connected SIP call did not select %s roles", test_case->name);

	for (unsigned int frame = 0; frame < training_frame_limit; ++frame) {
		CHECK(process_test_frame(
			      &caller, &answerer, codec, &recorder,
			      &caller_delay,
			      test_case->initial_caller_delay_frames) &&
		      (!monitor_caller_rate ||
		       v22_rate_monitor_observe(
			       &caller_rate_monitor, caller.transmit)),
		      "%s training failed at the SIP media boundary",
		      test_case->name);
		if (answerer.modem->dp != NULL &&
		    answerer.modem->dp->id == DP_V32) {
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
		if (modem_is_online(caller.modem, test_case->id) &&
		    modem_is_online(answerer.modem, test_case->id)) {
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
	      modem_is_online(caller.modem, test_case->id),
	      caller.modem->tx_rate, caller.modem->rx_rate,
	      modem_is_online(answerer.modem, test_case->id),
	      answerer.modem->tx_rate, answerer.modem->rx_rate);
	CHECK(!monitor_caller_rate ||
		      caller_rate_monitor.starts >=
			      test_case->minimum_caller_rate_signals,
	      "%s caller emitted only %u late V.22bis rate signal(s)",
	      test_case->name, caller_rate_monitor.starts);
	CHECK(test_case->id != DP_V32 ||
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
			      &caller_delay,
			      test_case->initial_caller_delay_frames),
		      "%s pre-link delivery failed at the SIP media boundary",
		      test_case->name);
	}
	CHECK(prelink_delivered &&
		      output_contains_connect(
			      &caller, test_case->caller_rx_rate) &&
		      output_contains_connect(
			      &answerer, test_case->answerer_rx_rate),
	      "%s connection did not reach both terminal interfaces "
	      "(caller state=%u ec=%u lapm=%d/%d ctrl=%d timer=%d; "
	      "answerer state=%u ec=%u lapm=%d/%d ctrl=%d timer=%d)",
	      test_case->name, caller.modem->state, caller.modem->cfg.ec,
	      caller.modem->ec.lapm.state, caller.modem->ec.lapm.config,
	      caller.modem->ec.lapm.ctrl_count, caller.modem->bit_timer,
	      answerer.modem->state, answerer.modem->cfg.ec,
	      answerer.modem->ec.lapm.state,
	      answerer.modem->ec.lapm.config,
	      answerer.modem->ec.lapm.ctrl_count,
	      answerer.modem->bit_timer);
	caller.output_count = 0;
	answerer.output_count = 0;
	CHECK(process_test_frame(
		      &caller, &answerer, codec, &recorder, &caller_delay,
		      test_case->initial_caller_delay_frames) &&
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
			      test_case->initial_caller_delay_frames) &&
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
	if (test_case->test_retrain) {
		CHECK(caller.modem->cfg.ec == !test_case->raw_transport &&
		      answerer.modem->cfg.ec == !test_case->raw_transport,
		      "%s did not retain its requested transport", test_case->name);
		if (test_case->id == DP_V22) {
			CHECK(reject_unavailable_retrain(&caller, test_case->id) &&
			      reject_unavailable_retrain(&answerer, test_case->id),
			      "%s accepted a non-standard retrain",
			      test_case->name);
		} else {
			CHECK(exercise_retrain(
				      &caller, &answerer, &answerer, test_case->id,
				      test_case->answerer_rx_rate, codec, &recorder, 0,
				      test_case->raw_transport,
				      test_case->id == DP_V22BIS &&
					      test_case->raw_transport &&
					      codec == MEDIA_CODEC_PCMU,
				      false),
			      "%s answerer retrain did not reconnect and pass data",
			      test_case->name);
			CHECK(exercise_retrain(
				      &caller, &answerer, &caller, test_case->id,
				      test_case->caller_rx_rate, codec, &recorder, 1,
				      test_case->raw_transport, false,
				      test_case->id == DP_V22BIS),
			      "%s caller retrain did not reconnect and pass data",
			      test_case->name);
			if (test_case->id == DP_V22BIS)
				CHECK(exercise_v22bis_carrier_recovery(
					      &caller, &answerer, codec, &recorder,
					      test_case->raw_transport),
					      "%s carrier recovery did not retrain and pass data",
					      test_case->name);
		}
	}
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
	CHECK(datapumps_stop(&datapumps) ==
		      DATAPUMPS_BUSY,
	      "live %s calls did not protect their process-global registration",
	      test_case->name);
	if (test_case->carrier_loss_samples != 0) {
		memset(caller.receive, 0, sizeof(caller.receive));
		for (unsigned int frame = 0;
		     frame * SIP_FRAME_SAMPLES <=
			     test_case->carrier_loss_samples;
		     ++frame)
			modem_process(caller.modem, caller.receive, caller.transmit,
				      SIP_FRAME_SAMPLES);
		CHECK(caller.modem->process == NULL,
		      "%s carrier loss did not stop the modem data path",
		      test_case->name);
	}

	endpoint_destroy(&answerer);
	endpoint_destroy(&caller);
	CHECK(datapumps_stop(&datapumps) ==
		      DATAPUMPS_OK,
	      "%s integration registrations remained busy", test_case->name);
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
	struct datapumps datapumps = { 0 };
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

	CHECK(datapumps_start(&datapumps) ==
		      DATAPUMPS_OK &&
		      endpoint_init(&caller, "Baudot caller") &&
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
	CHECK(datapumps_stop(&datapumps) ==
		      DATAPUMPS_BUSY,
	      "live Baudot calls did not protect their registration");
	endpoint_destroy(&answerer);
	endpoint_destroy(&caller);
	CHECK(datapumps_stop(&datapumps) ==
		      DATAPUMPS_OK,
	      "Baudot SIP registration did not stop cleanly");
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
		{ V34_SYMBOL_RATE_2743, 2743, 24000 },
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
		      profiles == 64 && cases == 128 :
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

static bool run_v32_recording_replay(const char *path)
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
	struct datapumps datapumps = { 0 };
	struct integration_endpoint endpoint;
	size_t connected_at = SIZE_MAX;

	CHECK(channel_bench_recording_load(
		      path, REPLAY_MAX_SAMPLES, &recording) ==
		      CHANNEL_BENCH_OK,
	      "optional V.32 recording could not be loaded");
	CHECK(datapumps_start(&datapumps) == DATAPUMPS_OK &&
		      endpoint_init(&endpoint, "V.32 recording replay"),
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
		CHECK(endpoint_drain(&endpoint),
		      "V.32 recording replay terminal output overflowed");
		if (connected_at == SIZE_MAX &&
		    modem_is_online(endpoint.modem, DP_V32))
			connected_at = offset;
		if (connected_at != SIZE_MAX)
			endpoint.output_count = 0;
	}
	CHECK(connected_at != SIZE_MAX,
	      "V.32 recording did not acquire carrier (final DP %d/status %d)",
	      endpoint.modem->dp != NULL ? (int)endpoint.modem->dp->id : -1,
	      endpoint.modem->dp != NULL ? (int)endpoint.modem->dp->status : -1);
	fprintf(stderr,
		"[       OK ] V.32 recording replay (%zu samples, carrier at %zu)\n",
		recording.count, connected_at);

	endpoint_destroy(&endpoint);
	CHECK(datapumps_stop(&datapumps) == DATAPUMPS_OK,
	      "V.32 recording replay registrations remained busy");
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
	const char *path, const char *symbol_rate_value,
	const char *retrain_sample_value, bool answerer)
{
	enum { TEST_STATE_COMMAND_ONLINE = 6 };
	static const char dial_command[] = "ATDrecording\r";
	static const char *const commands[] = {
		"ATZ\r",
		"ATE0\r",
		"ATX3\r",
		"AT+DS=0,1\r",
		"AT\\N0\r",
		"AT+MS=34,0,9600,9600\r",
	};
	struct channel_bench_recording recording = { 0 };
	struct datapumps datapumps = { 0 };
	struct integration_endpoint endpoint;
	const char *role = answerer ? "answer" : "originator";
	char symbol_rate_command[32];
	char symbol_rate_description[32];
	const char *symbol_rate_commands[] = { symbol_rate_command };
	unsigned int symbol_rate;
	size_t connected_at = SIZE_MAX;
	size_t handoff_at = SIZE_MAX;
	size_t retrain_sample;
	size_t retrained_at = SIZE_MAX;
	bool v8_seen = false;
	bool retrain_requested = false;
	char retrain_command[] = "ATO1";

	CHECK(v34_replay_symbol_rate_parse(symbol_rate_value, &symbol_rate) &&
		      v34_replay_sample_parse(
			      retrain_sample_value, &retrain_sample),
	      "optional V.34 recording replay parameter is invalid");
	CHECK(!answerer || retrain_sample == SIZE_MAX,
	      "V.34 answer recording does not accept an originator retrain sample");
	CHECK(channel_bench_recording_load(
		      path, REPLAY_MAX_SAMPLES, &recording) ==
		      CHANNEL_BENCH_OK,
	      "optional V.34 recording could not be loaded");
	CHECK(datapumps_start(&datapumps) == DATAPUMPS_OK &&
		      endpoint_init(&endpoint, "V.34 recording replay"),
	      "V.34 recording replay did not initialize");
	CHECK(configure_recording_replay(
		      &endpoint, commands, ARRAY_LENGTH(commands)),
	      "V.34 recording replay setup was rejected");
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
		/* A recording can contain arbitrarily much online data, while this
		 * optional replay has no DTE consuming it. Retain terminal output only
		 * while it can still establish the CONNECT boundary. */
		if (modem_is_online(endpoint.modem, DP_V34) &&
		    (!retrain_requested || retrained_at != SIZE_MAX))
			endpoint.output_count = 0;
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
	fprintf(stderr,
		"[       OK ] V.34 %s recording replay (%s, %zu samples, carrier at %zu, retrain at %zu, terminal bytes %zu)\n",
		role, symbol_rate_description, recording.count, connected_at,
		retrained_at, endpoint.output_count);

	endpoint_destroy(&endpoint);
	CHECK(datapumps_stop(&datapumps) == DATAPUMPS_OK,
	      "V.34 recording replay registrations remained busy");
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
	struct datapumps datapumps = { 0 };
	struct integration_endpoint endpoint;
	size_t connected_at = SIZE_MAX;

	CHECK(channel_bench_recording_load(
		      path, REPLAY_MAX_SAMPLES, &recording) ==
		      CHANNEL_BENCH_OK,
	      "optional V.22bis recording could not be loaded");
	CHECK(datapumps_start(&datapumps) == DATAPUMPS_OK &&
		      endpoint_init(&endpoint, "V.22bis recording replay"),
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
	CHECK(datapumps_stop(&datapumps) == DATAPUMPS_OK,
	      "V.22bis recording replay registrations remained busy");
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

static bool run_v90_recording_replay(
	const char *path, enum media_codec codec, bool originator)
{
	enum { MINIMUM_ONLINE_SAMPLES = 3200 };
	static const char *const answerer_commands[] = {
		"ATZ\r",
		"ATE0\r",
		"ATX3\r",
		"AT\\N0\r",
		"AT+DS=0,0\r",
		"AT+V90D=1\r",
		"AT+MS=90,0,28000,28000\r",
		"AT+V90U=19200,19200,3000\r",
	};
	static const char *const originator_commands[] = {
		"ATZ\r",
		"ATE0\r",
		"ATX3\r",
		"AT\\N0\r",
		"AT+DS=0,0\r",
		"AT+V90D=1\r",
		"AT+MS=90,0,28000,28000\r",
		"AT+V90U=14400,14400,3000\r",
	};
	static const char dial_command[] = "ATDpeer\r";
	const char *const *commands = originator ?
		originator_commands : answerer_commands;
	size_t command_count = originator ?
		ARRAY_LENGTH(originator_commands) :
		ARRAY_LENGTH(answerer_commands);
	struct datapumps datapumps = { 0 };
	struct integration_endpoint endpoint;
	enum g711_law law;
	uint8_t *recording = NULL;
	size_t recording_count = 0;
	size_t handoff_at = SIZE_MAX;
	size_t connected_at = SIZE_MAX;
	bool v8_seen = false;

	CHECK(codec_law(codec, &law) &&
		      load_octet_recording(
			      path, &recording, &recording_count),
	      "optional V.90 G.711 recording could not be loaded");
	CHECK(datapumps_start(&datapumps) == DATAPUMPS_OK &&
		      endpoint_init(&endpoint, "V.90 recording replay") &&
		      configure_recording_replay(
			      &endpoint, commands, command_count),
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

		if (frame_count > SIP_FRAME_SAMPLES)
			frame_count = SIP_FRAME_SAMPLES;
		memcpy(endpoint.receive_codewords, recording + offset,
		       frame_count);
		for (size_t index = 0; index < frame_count; ++index)
			endpoint.receive[index] = decode_codeword(
				law, endpoint.receive_codewords[index]);
		v8_before = endpoint.modem->dp != NULL &&
			endpoint.modem->dp->id == DP_V8;
		modem_process_g711(
			endpoint.modem, law, endpoint.receive_codewords,
			endpoint.receive, endpoint.transmit_codewords,
			endpoint.transmit, (int)frame_count, &exact_output);
		offset += frame_count;
		CHECK(endpoint_drain(&endpoint),
		      "V.90 recording replay terminal output overflowed");
		v8_seen = v8_seen || v8_before;
		if (handoff_at == SIZE_MAX && v8_before &&
		    endpoint.modem->dp != NULL &&
		    endpoint.modem->dp->id == DP_V90)
			handoff_at = offset;
		if (modem_is_online(endpoint.modem, DP_V90)) {
			if (connected_at == SIZE_MAX)
				connected_at = offset;
			/* The external recording may continue with arbitrary data,
			 * while this replay has no terminal consumer. */
			endpoint.output_count = 0;
		}
	}
	if (originator) {
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
	CHECK(datapumps_stop(&datapumps) == DATAPUMPS_OK,
	      "V.90 recording replay registrations remained busy");
	free(recording);
	return true;
}

int main(void)
{
	const char *v32_recording_path =
		getenv("V32_REPLAY_RX_PCM");
	const char *v34_recording_path =
		getenv("V34_REPLAY_RX_PCM");
	const char *v34_answer_recording_path =
		getenv("V34_ANSWER_REPLAY_RX_PCM");
	const char *v34_recording_symbol_rate =
		getenv("V34_REPLAY_SYMBOL_RATE");
	const char *v34_recording_retrain_sample =
		getenv("V34_REPLAY_RETRAIN_SAMPLE");
	const char *v34_answer_recording_symbol_rate =
		getenv("V34_ANSWER_REPLAY_SYMBOL_RATE");
	const char *v34_matrix_filter = getenv("V34_MATRIX_FILTER");
	const char *integration_filter = getenv("MODEM_INTEGRATION_FILTER");
	const char *v22_recording_path =
		getenv("V22_REPLAY_RX_PCM");
	const char *v22_answer_recording_path =
		getenv("V22_ANSWER_REPLAY_RX_PCM");
	const char *v90_recording_path =
		getenv("V90_REPLAY_RX_G711");
	const char *v90_recording_law =
		getenv("V90_REPLAY_LAW");
	const char *v90_recording_role =
		getenv("V90_REPLAY_ROLE");
	enum media_codec v90_replay_codec;
	bool v90_replay_originator;
	bool integration_case_selected = false;
	static const struct modem_test_case test_cases[] = {
		{
			.id = DP_V21,
			.name = "forced V.21",
			.selection_command = "AT+MS=21,0,300,300\r",
			.caller_tx_rate = FSK300_BIT_RATE,
			.caller_rx_rate = FSK300_BIT_RATE,
			.answerer_tx_rate = FSK300_BIT_RATE,
			.answerer_rx_rate = FSK300_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE / 5,
		},
		{
			.id = DP_V21,
			.name = "V.21 automode",
			.selection_command = "AT+MS=21,1,300,300\r",
			.caller_tx_rate = FSK300_BIT_RATE,
			.caller_rx_rate = FSK300_BIT_RATE,
			.answerer_tx_rate = FSK300_BIT_RATE,
			.answerer_rx_rate = FSK300_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE / 5,
			.automode = true,
		},
		{
			.id = DP_B103,
			.name = "Bell 103",
			.selection_command = "AT+MS=103,1,300,300\r",
			.caller_tx_rate = FSK300_BIT_RATE,
			.caller_rx_rate = FSK300_BIT_RATE,
			.answerer_tx_rate = FSK300_BIT_RATE,
			.answerer_rx_rate = FSK300_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE / 5,
			.automode = true,
		},
		{
			.id = DP_V22,
			.name = "V.22 600 raw rejects retrain",
			.selection_command = "AT+MS=22,0,600,600\r",
			.caller_tx_rate = V22_600_BIT_RATE,
			.caller_rx_rate = V22_600_BIT_RATE,
			.answerer_tx_rate = V22_600_BIT_RATE,
			.answerer_rx_rate = V22_600_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE / 5,
			.test_retrain = true,
			.raw_transport = true,
		},
		{
			.id = DP_V22,
			.name = "V.22 raw rejects retrain",
			.selection_command = "AT+MS=22,1,1200,1200\r",
			.caller_tx_rate = V22_1200_BIT_RATE,
			.caller_rx_rate = V22_1200_BIT_RATE,
			.answerer_tx_rate = V22_1200_BIT_RATE,
			.answerer_rx_rate = V22_1200_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE / 5,
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
			.initial_caller_delay_frames =
				V22BIS_INITIAL_DELAY_FRAMES,
			.minimum_caller_rate_signals = 1,
			.minimum_caller_rate_signal_sample =
				V22BIS_INITIAL_DELAY_FRAMES * SIP_FRAME_SAMPLES,
			.raw_transport = true,
		},
		{
			.id = DP_V22,
			.name = "V.22 600 LAPM rejects retrain",
			.selection_command = "AT+MS=22,0,600,600\r",
			.caller_tx_rate = V22_600_BIT_RATE,
			.caller_rx_rate = V22_600_BIT_RATE,
			.answerer_tx_rate = V22_600_BIT_RATE,
			.answerer_rx_rate = V22_600_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE / 5,
			.test_retrain = true,
		},
		{
			.id = DP_V22,
			.name = "V.22 LAPM rejects retrain",
			.selection_command = "AT+MS=22,0,1200,1200\r",
			.caller_tx_rate = V22_1200_BIT_RATE,
			.caller_rx_rate = V22_1200_BIT_RATE,
			.answerer_tx_rate = V22_1200_BIT_RATE,
			.answerer_rx_rate = V22_1200_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE / 5,
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
			.name = "V.32bis 7200 LAPM retrain",
			.selection_command = "AT+MS=32,1,7200,7200\r",
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
			.id = DP_V32,
			.name = "V.32bis 12000 LAPM retrain",
			.selection_command = "AT+MS=32,1,12000,12000\r",
			.caller_tx_rate = V32_12000_BIT_RATE,
			.caller_rx_rate = V32_12000_BIT_RATE,
			.answerer_tx_rate = V32_12000_BIT_RATE,
			.answerer_rx_rate = V32_12000_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE / 5,
			.automode = true,
			.test_retrain = true,
		},
		{
			.id = DP_V32,
			.name = "V.32bis 14400 LAPM retrain",
			.selection_command = "AT+MS=32,1,14400,14400\r",
			.caller_tx_rate = V32_14400_BIT_RATE,
			.caller_rx_rate = V32_14400_BIT_RATE,
			.answerer_tx_rate = V32_14400_BIT_RATE,
			.answerer_rx_rate = V32_14400_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE / 5,
			.automode = true,
			.test_retrain = true,
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
			.name = "V.34 9600 LAPM retrain",
			.selection_command = "AT+MS=34,1,9600,9600\r",
			.caller_tx_rate = 9600,
			.caller_rx_rate = 9600,
			.answerer_tx_rate = 9600,
			.answerer_rx_rate = 9600,
			.carrier_loss_samples = MODEM_RATE / 2,
			.training_frame_limit = 1500,
			.automode = true,
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
			.impairment = INTEGRATION_IMPAIRMENT_COMPOSITE,
		},
		{
			.id = DP_V90,
			.name = "V.90 28000 downstream",
			.selection_command = "AT+MS=90,0,28000,28000\r",
			.caller_tx_rate = 19200,
			.caller_rx_rate = V90_PROFILE_MIN_DOWNSTREAM_BIT_RATE,
			.answerer_tx_rate = V90_PROFILE_MIN_DOWNSTREAM_BIT_RATE,
			.answerer_rx_rate = 19200,
			.training_frame_limit = 1500,
			.requires_v8 = true,
			.requires_exact_g711 = true,
			.impairment = INTEGRATION_IMPAIRMENT_DIGITAL_DELAY,
		},
		{
			.id = DP_B212,
			.name = "Bell 212A",
			.selection_command = "AT+MS=212,1,1200,1200\r",
			.caller_tx_rate = V22_1200_BIT_RATE,
			.caller_rx_rate = V22_1200_BIT_RATE,
			.answerer_tx_rate = V22_1200_BIT_RATE,
			.answerer_rx_rate = V22_1200_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE / 5,
			.automode = true,
		},
		{
			.id = DP_V23,
			.name = "V.23",
			.selection_command = "AT+MS=23,1,300,1200\r",
			.caller_tx_rate = V23_BACKWARD_BIT_RATE,
			.caller_rx_rate = V23_FORWARD_BIT_RATE,
			.answerer_tx_rate = V23_FORWARD_BIT_RATE,
			.answerer_rx_rate = V23_BACKWARD_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE * 7 / 10,
			.automode = true,
			.test_prelink = true,
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
	if (v32_recording_path == NULL || v32_recording_path[0] == '\0') {
		fprintf(stderr,
			"[  SKIPPED ] V.32 recording replay (V32_REPLAY_RX_PCM unset)\n");
	} else if (!run_v32_recording_replay(v32_recording_path)) {
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
	if (v34_recording_path == NULL || v34_recording_path[0] == '\0') {
		fprintf(stderr,
			"[  SKIPPED ] V.34 recording replay (V34_REPLAY_RX_PCM unset)\n");
	} else if (!run_v34_recording_replay(
			   v34_recording_path, v34_recording_symbol_rate,
			   v34_recording_retrain_sample, false)) {
		return 1;
	}
	if (v34_answer_recording_path == NULL ||
	    v34_answer_recording_path[0] == '\0') {
		fprintf(stderr,
			"[  SKIPPED ] V.34 answer recording replay (V34_ANSWER_REPLAY_RX_PCM unset)\n");
	} else if (!run_v34_recording_replay(
			   v34_answer_recording_path,
			   v34_answer_recording_symbol_rate, NULL, true)) {
		return 1;
	}
	if (v90_recording_path == NULL || v90_recording_path[0] == '\0') {
		fprintf(stderr,
			"[  SKIPPED ] V.90 recording replay (V90_REPLAY_RX_G711 unset)\n");
	} else if (!v90_replay_codec_parse(
			   v90_recording_law, &v90_replay_codec) ||
		   !v90_replay_role_parse(
			   v90_recording_role, &v90_replay_originator)) {
		fprintf(stderr, "invalid V.90 recording replay configuration\n");
		return 1;
	} else if (!run_v90_recording_replay(
			   v90_recording_path, v90_replay_codec,
			   v90_replay_originator)) {
		return 1;
	}
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
	if (!run_v34_rate_matrix(NULL))
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
