// SPDX-License-Identifier: BSD-3-Clause

#include <modem.h>
#include <sippymodem_datapumps.h>
#include <sippymodem_fsk300_datapump.h>
#include <sippymodem_media.h>
#include <sippymodem_v23_datapump.h>

#include "test.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum {
	SIP_FRAME_SAMPLES = 160,
	ENDPOINT_OUTPUT_CAPACITY = 1024,
	TRAINING_FRAME_LIMIT = 250,
	LINK_DRAIN_FRAME_LIMIT = 50,
	DATA_FRAME_LIMIT = 400,
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
	size_t blocks[SIPPYMODEM_RECORD_TX_RTP_PAYLOAD_SENT + 1];
	size_t units[SIPPYMODEM_RECORD_TX_RTP_PAYLOAD_SENT + 1];
	bool invalid;
};

struct fsk_test_case {
	enum DP_ID id;
	const char *name;
	const char *selection_command;
	unsigned int caller_tx_rate;
	unsigned int caller_rx_rate;
	unsigned int answerer_tx_rate;
	unsigned int answerer_rx_rate;
	unsigned int carrier_loss_samples;
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
	.name = "FSK SIP media test",
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
	void *context, const struct sippymodem_recording_block *block)
{
	struct recording_capture *capture = context;

	if (capture == NULL || block == NULL || block->data == NULL ||
	    block->count == 0 ||
	    block->stream < SIPPYMODEM_RECORD_RX_RTP_PAYLOAD_ARRIVAL ||
	    block->stream > SIPPYMODEM_RECORD_TX_RTP_PAYLOAD_SENT) {
		if (capture != NULL)
			capture->invalid = true;
		return;
	}
	capture->blocks[block->stream]++;
	capture->units[block->stream] += block->count;
}

static bool transcode(
	enum sippymodem_media_codec codec,
	const int16_t transmit[SIP_FRAME_SAMPLES],
	int16_t receive[SIP_FRAME_SAMPLES],
	const struct sippymodem_recorder *recorder)
{
	uint8_t payload[SIP_FRAME_SAMPLES];

	return sippymodem_media_encode(
		       codec, transmit, SIP_FRAME_SAMPLES,
		       payload, ARRAY_LENGTH(payload), recorder) ==
		       SIPPYMODEM_MEDIA_OK &&
	       sippymodem_media_decode(
		       codec, payload, ARRAY_LENGTH(payload), receive,
		       SIP_FRAME_SAMPLES, recorder) == SIPPYMODEM_MEDIA_OK;
}

static bool process_sip_frame(
	struct integration_endpoint *caller,
	struct integration_endpoint *answerer,
	enum sippymodem_media_codec codec,
	const struct sippymodem_recorder *recorder)
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
	const struct fsk_test_case *test_case,
	enum sippymodem_media_codec codec)
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
	struct sippymodem_datapumps datapumps = { 0 };
	struct integration_endpoint caller;
	struct integration_endpoint answerer;
	struct recording_capture capture = { 0 };
	const struct sippymodem_recorder recorder = {
		.context = &capture,
		.write = capture_recording,
	};
	bool online = false;
	bool prelink_queued = false;
	bool prelink_delivered = false;
	bool transferred = false;
	int selection_length = (int)strlen(test_case->selection_command);

	CHECK(sippymodem_datapumps_start(&datapumps) ==
		      SIPPYMODEM_DATAPUMPS_OK,
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
		      caller.modem->dp->id == test_case->id &&
		      modem_answer(answerer.modem) == 0,
	      "connected SIP call did not select %s roles", test_case->name);

	for (unsigned int frame = 0; frame < TRAINING_FRAME_LIMIT; ++frame) {
		CHECK(process_sip_frame(
		      &caller, &answerer, codec, &recorder),
		      "%s training failed at the SIP media boundary",
		      test_case->name);
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
	CHECK(online && caller.modem->tx_rate == test_case->caller_tx_rate &&
		      caller.modem->rx_rate == test_case->caller_rx_rate &&
		      answerer.modem->tx_rate == test_case->answerer_tx_rate &&
		      answerer.modem->rx_rate == test_case->answerer_rx_rate,
	      "%s peers did not acquire carrier and report their rates",
	      test_case->name);
	CHECK(!test_case->test_prelink || prelink_queued,
	      "%s did not expose its asymmetric pre-link interval",
	      test_case->name);
	for (unsigned int frame = 0; frame < LINK_DRAIN_FRAME_LIMIT; ++frame) {
		CHECK(endpoint_drain(&caller) && endpoint_drain(&answerer),
		      "%s connection output could not be read", test_case->name);
		if (!test_case->test_prelink ||
		    output_contains(&caller, prelink_payload)) {
			prelink_delivered = true;
			break;
		}
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
	      "%s full-duplex terminal payload changed in transit",
	      test_case->name);
	CHECK(!capture.invalid &&
		      capture.blocks[SIPPYMODEM_RECORD_TX_LINE_PCM] != 0 &&
		      capture.blocks[
			      SIPPYMODEM_RECORD_TX_RTP_PAYLOAD_PREPARED] != 0 &&
		      capture.blocks[
			      SIPPYMODEM_RECORD_RX_RTP_PAYLOAD_PLAYOUT] != 0 &&
		      capture.blocks[SIPPYMODEM_RECORD_RX_LINE_PCM] != 0,
	      "%s SIP call did not retain recording boundaries",
	      test_case->name);
	CHECK(sippymodem_datapumps_stop(&datapumps) ==
		      SIPPYMODEM_DATAPUMPS_BUSY,
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
	CHECK(sippymodem_datapumps_stop(&datapumps) ==
		      SIPPYMODEM_DATAPUMPS_OK,
	      "%s integration registrations remained busy", test_case->name);
	return true;
}

static const char *codec_name(enum sippymodem_media_codec codec)
{
	return codec == SIPPYMODEM_CODEC_PCMU ? "PCMU" : "PCMA";
}

int main(void)
{
	static const struct fsk_test_case test_cases[] = {
		{
			.id = DP_V21,
			.name = "V.21",
			.selection_command = "AT+MS=21,0,300,300\r",
			.caller_tx_rate = SIPPYMODEM_FSK300_BIT_RATE,
			.caller_rx_rate = SIPPYMODEM_FSK300_BIT_RATE,
			.answerer_tx_rate = SIPPYMODEM_FSK300_BIT_RATE,
			.answerer_rx_rate = SIPPYMODEM_FSK300_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE / 5,
		},
		{
			.id = DP_B103,
			.name = "Bell 103",
			.selection_command = "AT+MS=103,0,300,300\r",
			.caller_tx_rate = SIPPYMODEM_FSK300_BIT_RATE,
			.caller_rx_rate = SIPPYMODEM_FSK300_BIT_RATE,
			.answerer_tx_rate = SIPPYMODEM_FSK300_BIT_RATE,
			.answerer_rx_rate = SIPPYMODEM_FSK300_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE / 5,
		},
		{
			.id = DP_V23,
			.name = "V.23",
			.selection_command = "AT+MS=23,0,300,1200\r",
			.caller_tx_rate = SIPPYMODEM_V23_BACKWARD_BIT_RATE,
			.caller_rx_rate = SIPPYMODEM_V23_FORWARD_BIT_RATE,
			.answerer_tx_rate = SIPPYMODEM_V23_FORWARD_BIT_RATE,
			.answerer_rx_rate = SIPPYMODEM_V23_BACKWARD_BIT_RATE,
			.carrier_loss_samples = MODEM_RATE * 7 / 10,
			.test_prelink = true,
		},
	};
	static const enum sippymodem_media_codec codecs[] = {
		SIPPYMODEM_CODEC_PCMU,
		SIPPYMODEM_CODEC_PCMA,
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
	return 0;
}
