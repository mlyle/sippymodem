// SPDX-License-Identifier: BSD-3-Clause

#include <modem.h>
#include <baudot.h>
#include <datapumps.h>
#include <fsk300_datapump.h>
#include <media.h>
#include <v22_datapump.h>
#include <v23_datapump.h>
#include <datapump.h>
#include <v8_datapump.h>
#include <v34_datapump.h>
#include <v34_frame_schedule.h>

#include "channel_bench.h"
#include "test.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum {
	SIP_FRAME_SAMPLES = 160,
	ENDPOINT_OUTPUT_CAPACITY = 1024,
	TRAINING_FRAME_LIMIT = 250,
	NEGOTIATED_TRAINING_FRAME_LIMIT = 500,
	LINK_DRAIN_FRAME_LIMIT = 50,
	DATA_FRAME_LIMIT = 400,
	REPLAY_MAX_SAMPLES = MODEM_RATE * 180,
};

struct integration_endpoint {
	struct modem *modem;
	int modem_fd;
	int client_fd;
	int16_t receive[SIP_FRAME_SAMPLES];
	int16_t transmit[SIP_FRAME_SAMPLES];
	uint8_t output[ENDPOINT_OUTPUT_CAPACITY];
	size_t output_count;
};

struct recording_capture {
	size_t blocks[RECORD_TX_RTP_PAYLOAD_SENT + 1];
	size_t units[RECORD_TX_RTP_PAYLOAD_SENT + 1];
	bool invalid;
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
	bool automode;
	bool requires_v8;
	bool test_prelink;
};

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

static bool transcode(
	enum media_codec codec,
	const int16_t transmit[SIP_FRAME_SAMPLES],
	int16_t receive[SIP_FRAME_SAMPLES],
	const struct media_recorder *recorder)
{
	uint8_t payload[SIP_FRAME_SAMPLES];

	return media_encode(
		       codec, transmit, SIP_FRAME_SAMPLES,
		       payload, ARRAY_LENGTH(payload), recorder) ==
		       MEDIA_OK &&
	       media_decode(
		       codec, payload, ARRAY_LENGTH(payload), receive,
		       SIP_FRAME_SAMPLES, recorder) == MEDIA_OK;
}

static bool process_sip_frame(
	struct integration_endpoint *caller,
	struct integration_endpoint *answerer,
	enum media_codec codec,
	const struct media_recorder *recorder)
{
	modem_process(caller->modem, caller->receive, caller->transmit,
		      SIP_FRAME_SAMPLES);
	modem_process(answerer->modem, answerer->receive, answerer->transmit,
		      SIP_FRAME_SAMPLES);
	return transcode(
		       codec, caller->transmit, answerer->receive, recorder) &&
	       transcode(
		       codec, answerer->transmit, caller->receive, recorder);
}

static bool modem_is_online(const struct modem *modem, enum DP_ID id)
{
	return modem != NULL && modem->started && modem->dp != NULL &&
	       modem->dp->id == id && modem->dp->status == DPSTAT_CONNECT &&
	       modem->get_bits != NULL && modem->put_bits != NULL;
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
	struct recording_capture capture = { 0 };
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
	enum DP_ID initial_datapump = uses_v8 ? DP_V8 : test_case->id;
	unsigned int training_frame_limit =
		test_case->training_frame_limit != 0 ?
			test_case->training_frame_limit :
			(uses_v8 ? NEGOTIATED_TRAINING_FRAME_LIMIT :
				   TRAINING_FRAME_LIMIT);
	int selection_length = (int)strlen(test_case->selection_command);

	CHECK(datapumps_start(&datapumps) ==
		      DATAPUMPS_OK,
	      "%s integration registrations did not start", test_case->name);
	CHECK(endpoint_init(&caller, "FSK caller") &&
		      endpoint_init(&answerer, "FSK answerer"),
	      "%s integration endpoints did not initialize", test_case->name);
	CHECK(MODEM_DP(caller.modem) == DP_V21 &&
		      MODEM_DP(answerer.modem) == DP_V21 &&
		      MODEM_AUTOMODE(caller.modem) == 0 &&
		      MODEM_AUTOMODE(answerer.modem) == 0,
	      "source-built modem did not default to its available capability");
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
		CHECK(process_sip_frame(
		      &caller, &answerer, codec, &recorder),
		      "%s training failed at the SIP media boundary",
		      test_case->name);
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
		CHECK(process_sip_frame(
			      &caller, &answerer, codec, &recorder),
		      "%s pre-link delivery failed at the SIP media boundary",
		      test_case->name);
	}
	CHECK(prelink_delivered &&
		      output_contains_connect(
			      &caller, test_case->caller_rx_rate) &&
		      output_contains_connect(
			      &answerer, test_case->answerer_rx_rate),
	      "%s connection did not reach both terminal interfaces",
	      test_case->name);
	caller.output_count = 0;
	answerer.output_count = 0;
	CHECK(process_sip_frame(&caller, &answerer, codec, &recorder) &&
		      endpoint_drain(&caller) && endpoint_drain(&answerer) &&
		      caller.output_count == 0 && answerer.output_count == 0,
	      "%s idle link emitted terminal data (caller=%zu answerer=%zu)",
	      test_case->name, caller.output_count, answerer.output_count);

	CHECK(modem_write(caller.modem, (const char *)caller_payload,
			  (int)sizeof(caller_payload)) ==
		      (int)sizeof(caller_payload) &&
		      modem_write(answerer.modem, (const char *)answer_payload,
			  (int)sizeof(answer_payload)) ==
		      (int)sizeof(answer_payload),
	      "%s terminals did not accept full-duplex payloads",
	      test_case->name);
	for (unsigned int frame = 0; frame < DATA_FRAME_LIMIT; ++frame) {
		CHECK(process_sip_frame(
		      &caller, &answerer, codec, &recorder) &&
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
	CHECK(!capture.invalid &&
		      capture.blocks[RECORD_TX_LINE_PCM] != 0 &&
		      capture.blocks[
			      RECORD_TX_RTP_PAYLOAD_PREPARED] != 0 &&
		      capture.blocks[
			      RECORD_RX_RTP_PAYLOAD_PLAYOUT] != 0 &&
		      capture.blocks[RECORD_RX_LINE_PCM] != 0,
	      "%s SIP call did not retain recording boundaries",
	      test_case->name);
	CHECK(datapumps_stop(&datapumps) ==
		      DATAPUMPS_BUSY,
	      "live %s calls did not protect their process-global registration",
	      test_case->name);
	memset(caller.receive, 0, sizeof(caller.receive));
	for (unsigned int frame = 0;
	     frame * SIP_FRAME_SAMPLES <= test_case->carrier_loss_samples;
	     ++frame)
		modem_process(caller.modem, caller.receive, caller.transmit,
			      SIP_FRAME_SAMPLES);
	CHECK(caller.modem->process == NULL,
	      "%s carrier loss did not stop the modem data path",
	      test_case->name);

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
		      endpoint_init(&answerer, "Baudot answerer"),
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

static bool run_v34_rate_matrix(void)
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
		{ V34_SYMBOL_RATE_2800, 2800, 24000 },
		{ V34_SYMBOL_RATE_3000, 3000, 24000 },
		{ V34_SYMBOL_RATE_3200, 3200, 24000 },
		{ V34_SYMBOL_RATE_3429, 3429, 24000 },
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
				.carrier_loss_samples = MODEM_RATE / 5,
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
	CHECK(profiles == 45 && cases == 90,
	      "V.34 matrix covered %u profiles/%u cases instead of 45/90",
	      profiles, cases);
	return true;
}

static bool configure_recording_replay(
	struct integration_endpoint *endpoint)
{
	static const char *const commands[] = {
		"ATZ\r",
		"ATE0\r",
		"ATX3\r",
		"AT+DS=0,1\r",
		"AT\\N0\r",
		"AT+MS=34,0,9600,9600\r",
	};

	for (size_t index = 0; index < ARRAY_LENGTH(commands); ++index) {
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

static bool run_v34_recording_replay(const char *path)
{
	static const char dial_command[] = "ATDrecording\r";
	struct channel_bench_recording recording = { 0 };
	struct datapumps datapumps = { 0 };
	struct integration_endpoint endpoint;
	size_t connected_at = SIZE_MAX;
	bool passed = false;

	CHECK(channel_bench_recording_load(
		      path, REPLAY_MAX_SAMPLES, &recording) ==
		      CHANNEL_BENCH_OK,
	      "optional V.34 recording could not be loaded");
	CHECK(datapumps_start(&datapumps) == DATAPUMPS_OK &&
		      endpoint_init(&endpoint, "V.34 recording replay"),
	      "V.34 recording replay did not initialize");
	CHECK(configure_recording_replay(&endpoint),
	      "V.34 recording replay setup was rejected");
	CHECK(modem_write(
		      endpoint.modem, dial_command,
		      (int)(sizeof(dial_command) - 1u)) ==
		      (int)(sizeof(dial_command) - 1u) &&
		      modem_report_outgoing_call_connected(endpoint.modem) == 0,
	      "V.34 recording replay did not start the call");
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
		      "V.34 recording replay terminal output overflowed");
		if (connected_at == SIZE_MAX &&
		    modem_is_online(endpoint.modem, DP_V34))
			connected_at = offset;
	}
	passed = connected_at != SIZE_MAX;
	CHECK(passed,
	      "V.34 recording did not reproduce carrier acquisition");
	fprintf(stderr,
		"[       OK ] V.34 recording replay (%zu samples, carrier at %zu, terminal bytes %zu)\n",
		recording.count, connected_at, endpoint.output_count);

	endpoint_destroy(&endpoint);
	CHECK(datapumps_stop(&datapumps) == DATAPUMPS_OK,
	      "V.34 recording replay registrations remained busy");
	channel_bench_recording_release(&recording);
	return true;
}

int main(void)
{
	const char *recording_path =
		getenv("V34_REPLAY_RX_PCM");
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
			.name = "V.22 600",
			.selection_command = "AT+MS=22,0,600,600\r",
			.caller_tx_rate = V22_600_BIT_RATE,
			.caller_rx_rate = V22_600_BIT_RATE,
			.answerer_tx_rate = V22_600_BIT_RATE,
			.answerer_rx_rate = V22_600_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE / 5,
		},
		{
			.id = DP_V22,
			.name = "V.22",
			.selection_command = "AT+MS=22,1,1200,1200\r",
			.caller_tx_rate = V22_1200_BIT_RATE,
			.caller_rx_rate = V22_1200_BIT_RATE,
			.answerer_tx_rate = V22_1200_BIT_RATE,
			.answerer_rx_rate = V22_1200_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE / 5,
			.automode = true,
		},
		{
			.id = DP_V22BIS,
			.name = "V.22bis",
			.selection_command = "AT+MS=122,1,2400,2400\r",
			.caller_tx_rate = V22BIS_BIT_RATE,
			.caller_rx_rate = V22BIS_BIT_RATE,
			.answerer_tx_rate = V22BIS_BIT_RATE,
			.answerer_rx_rate = V22BIS_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE / 5,
			.automode = true,
		},
		{
			.id = DP_V32,
			.name = "V.32 4800",
			.selection_command = "AT+MS=32,1,4800,4800\r",
			.caller_tx_rate = V32_4800_BIT_RATE,
			.caller_rx_rate = V32_4800_BIT_RATE,
			.answerer_tx_rate = V32_4800_BIT_RATE,
			.answerer_rx_rate = V32_4800_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE / 5,
			.automode = true,
		},
		{
			.id = DP_V32,
			.name = "V.32bis 7200",
			.selection_command = "AT+MS=32,1,7200,7200\r",
			.caller_tx_rate = V32_7200_BIT_RATE,
			.caller_rx_rate = V32_7200_BIT_RATE,
			.answerer_tx_rate = V32_7200_BIT_RATE,
			.answerer_rx_rate = V32_7200_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE / 5,
			.automode = true,
		},
		{
			.id = DP_V32,
			.name = "V.32 9600",
			.selection_command = "AT+MS=32,1,9600,9600\r",
			.caller_tx_rate = V32_9600_BIT_RATE,
			.caller_rx_rate = V32_9600_BIT_RATE,
			.answerer_tx_rate = V32_9600_BIT_RATE,
			.answerer_rx_rate = V32_9600_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE / 5,
			.automode = true,
		},
		{
			.id = DP_V32,
			.name = "V.32bis 12000",
			.selection_command = "AT+MS=32,1,12000,12000\r",
			.caller_tx_rate = V32_12000_BIT_RATE,
			.caller_rx_rate = V32_12000_BIT_RATE,
			.answerer_tx_rate = V32_12000_BIT_RATE,
			.answerer_rx_rate = V32_12000_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE / 5,
			.automode = true,
		},
		{
			.id = DP_V32,
			.name = "V.32bis 14400",
			.selection_command = "AT+MS=32,1,14400,14400\r",
			.caller_tx_rate = V32_14400_BIT_RATE,
			.caller_rx_rate = V32_14400_BIT_RATE,
			.answerer_tx_rate = V32_14400_BIT_RATE,
			.answerer_rx_rate = V32_14400_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE / 5,
			.automode = true,
		},
		{
			.id = DP_V34,
			.name = "V.34 9600 automode",
			.selection_command = "AT+MS=34,1,9600,9600\r",
			.caller_tx_rate = 9600,
			.caller_rx_rate = 9600,
			.answerer_tx_rate = 9600,
			.answerer_rx_rate = 9600,
			.carrier_loss_samples = MODEM_RATE / 5,
			.training_frame_limit = 1500,
			.automode = true,
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

	for (size_t standard = 0; standard < ARRAY_LENGTH(test_cases);
	     ++standard) {
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
	if (!run_v34_rate_matrix())
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
	if (recording_path == NULL || recording_path[0] == '\0') {
		fprintf(stderr,
			"[  SKIPPED ] V.34 recording replay (V34_REPLAY_RX_PCM unset)\n");
	} else if (!run_v34_recording_replay(recording_path)) {
		return 1;
	}
	return 0;
}
