// SPDX-License-Identifier: BSD-3-Clause

#include <sip_media_bridge.h>

#include "test.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_SLOTS = 16,
	TEST_WORKSPACE = 8,
};

struct bridge_fixture {
	struct rtp_jitter_slot slots[TEST_SLOTS];
	struct rtp_stream stream;
	struct rtp_endpoint endpoint;
	struct sip_media_bridge bridge;
	uint8_t workspace[TEST_WORKSPACE];
};

struct callback_capture {
	struct sip_media_bridge *bridge;
	unsigned int ready;
	unsigned int error;
	unsigned int trace;
	char trace_text[1024];
	unsigned int registration;
	unsigned int call;
	unsigned int media;
	bool observed_callback_depth;
	enum sip_rtp_result observed_binding;
};

static void capture_ready(void *context)
{
	struct callback_capture *capture = context;

	capture->ready++;
	capture->observed_callback_depth |=
		sip_media_bridge_is_in_callback(capture->bridge);
}

#define DEFINE_CAPTURE(function_name, member, record_type)                   \
	static void function_name(void *context, const record_type *record)    \
	{                                                                      \
		struct callback_capture *capture = context;                       \
	                                                                       \
		(void)record;                                                   \
		capture->member++;                                              \
		capture->observed_callback_depth |=                              \
			sip_media_bridge_is_in_callback(               \
				capture->bridge);                                  \
	}

DEFINE_CAPTURE(capture_error, error,
	       struct sip_signaling_error)
DEFINE_CAPTURE(capture_registration, registration,
	       struct sip_registration_event)
DEFINE_CAPTURE(capture_call, call, struct sip_call_event)

#undef DEFINE_CAPTURE

static void capture_trace(void *context, const struct sip_trace_record *record)
{
	struct callback_capture *capture = context;

	capture->trace++;
	capture->observed_callback_depth |=
		sip_media_bridge_is_in_callback(capture->bridge);
	if (record->data != NULL && record->size < sizeof(capture->trace_text)) {
		memcpy(capture->trace_text, record->data, record->size);
		capture->trace_text[record->size] = '\0';
	}
}

static void capture_media(
	void *context, const struct sip_media_event *event)
{
	struct callback_capture *capture = context;

	(void)event;
	capture->media++;
	capture->observed_binding =
		sip_media_bridge_last_binding(capture->bridge);
	capture->observed_callback_depth |=
		sip_media_bridge_is_in_callback(capture->bridge);
}

static bool open_fixture(struct bridge_fixture *fixture,
			 struct callback_capture *capture)
{
	const struct rtp_stream_config stream_config = {
		.codec = MEDIA_CODEC_PCMU,
		.payload_type = 0,
	};
	const struct rtp_endpoint_config endpoint_config = {
		.learn_remote = true,
		.symmetric_rtp = true,
	};
	const struct sockaddr_in local = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	const struct sip_signaling_callbacks destination = {
		.context = capture,
		.ready = capture_ready,
		.error = capture_error,
		.trace = capture_trace,
		.registration_changed = capture_registration,
		.call_changed = capture_call,
		.media_changed = capture_media,
	};

	memset(fixture, 0, sizeof(*fixture));
	return rtp_stream_init(
		       &fixture->stream, &stream_config, fixture->slots,
		       ARRAY_LENGTH(fixture->slots), fixture->workspace,
		       sizeof(fixture->workspace)) &&
	       rtp_endpoint_init(
		       &fixture->endpoint, &fixture->stream, &endpoint_config) &&
	       rtp_endpoint_open(&fixture->endpoint, &local) ==
		       RTP_ENDPOINT_OK &&
	       sip_media_bridge_init(
		       &fixture->bridge, &fixture->endpoint, &destination);
}

static bool remote_is(const struct bridge_fixture *fixture,
		      unsigned int port)
{
	struct sockaddr_in remote;
	bool learned = true;

	return rtp_endpoint_transmit_remote(
		       &fixture->endpoint, &remote, &learned) &&
	       !learned && remote.sin_addr.s_addr == htonl(INADDR_LOOPBACK) &&
	       remote.sin_port == htons((uint16_t)port);
}

static bool test_forwards_and_binds_before_media_callback(void)
{
	struct bridge_fixture fixture;
	struct callback_capture capture = { 0 };
	const struct sip_signaling_callbacks *source;
	const struct sip_signaling_error error = { 0 };
	const struct sip_trace_record trace = { 0 };
	const struct sip_registration_event registration = { 0 };
	const struct sip_call_event call = { 0 };
	struct sip_media_event media = {
		.state = SIP_MEDIA_CONFIGURED,
		.remote_address_family = SIP_MEDIA_ADDRESS_IPV4,
		.codec = MEDIA_CODEC_PCMU,
		.payload_type = 0,
		.sample_rate = MEDIA_SAMPLE_RATE,
		.channels = 1,
		.remote_address = "127.0.0.1",
		.remote_port = 42000,
	};

	CHECK(open_fixture(&fixture, &capture), "could not open bridge fixture");
	capture.bridge = &fixture.bridge;
	media.local_port =
		rtp_endpoint_local_port(&fixture.endpoint);
	source = sip_media_bridge_callbacks(&fixture.bridge);
	source->ready(source->context);
	source->error(source->context, &error);
	source->trace(source->context, &trace);
	source->registration_changed(source->context, &registration);
	source->call_changed(source->context, &call);
	source->media_changed(source->context, &media);
	CHECK(capture.ready == 1 && capture.error == 1 && capture.trace == 1 &&
		      capture.registration == 1 && capture.call == 1 &&
		      capture.media == 1 && capture.observed_callback_depth,
	      "bridge did not forward every signaling callback");
	CHECK(capture.observed_binding == SIP_RTP_OK &&
		      remote_is(&fixture, 42000) &&
		      sip_media_bridge_is_configured(
			      &fixture.bridge) &&
		      sip_media_bridge_binding_failures(
			      &fixture.bridge) == 0,
	      "bridge did not bind RTP before forwarding media");

	media.payload_type = 8;
	media.remote_port = 43000;
	source->media_changed(source->context, &media);
	CHECK(capture.observed_binding ==
		      SIP_RTP_INCOMPATIBLE_MEDIA &&
		      remote_is(&fixture, 42000) &&
		      sip_media_bridge_is_configured(
			      &fixture.bridge) &&
		      sip_media_bridge_binding_failures(
			      &fixture.bridge) == 1,
	      "failed media update changed the established peer");
	sip_media_bridge_clear_media(&fixture.bridge);
	CHECK(!remote_is(&fixture, 42000) &&
		      !sip_media_bridge_is_configured(
			      &fixture.bridge),
	      "explicit media reset retained negotiated state");
	media.payload_type = 0;
	media.remote_port = 42000;
	source->media_changed(source->context, &media);
	CHECK(remote_is(&fixture, 42000) &&
		      sip_media_bridge_is_configured(&fixture.bridge),
	      "bridge did not rebind after an explicit reset");
	media.state = SIP_MEDIA_INACTIVE;
	source->media_changed(source->context, &media);
	CHECK(sip_media_bridge_last_binding(&fixture.bridge) ==
		      SIP_RTP_OK &&
		      !remote_is(&fixture, 42000) &&
		      !sip_media_bridge_is_configured(
			      &fixture.bridge),
	      "inactive media did not clear the established peer");
	rtp_endpoint_close(&fixture.endpoint);
	return true;
}

static bool test_admission_waits_for_media_cleanup(void)
{
	struct bridge_fixture fixture;
	struct callback_capture capture = {0};
	CHECK(open_fixture(&fixture, &capture), "fixture failed");
	const struct sip_signaling_callbacks *source = sip_media_bridge_callbacks(&fixture.bridge);
	CHECK(source->admit(source->context), "clean media rejected admission");
	fixture.endpoint.discard_pending = true;
	CHECK(!source->admit(source->context), "old media allowed replacement admission");
	fixture.endpoint.discard_pending = false;
	CHECK(source->admit(source->context), "cleanup did not restore admission");
	rtp_endpoint_close(&fixture.endpoint);
	return true;
}

static bool test_validates_initialization(void)
{
	struct sip_media_bridge bridge;
	struct rtp_endpoint endpoint = { 0 };

	CHECK(!sip_media_bridge_init(NULL, &endpoint, NULL) &&
		      !sip_media_bridge_init(&bridge, NULL, NULL) &&
		      !sip_media_bridge_init(&bridge, &endpoint, NULL),
	      "bridge accepted incomplete ownership");
	CHECK(sip_media_bridge_callbacks(NULL) == NULL &&
		      sip_media_bridge_last_binding(NULL) ==
			      SIP_RTP_INVALID_ARGUMENT &&
		      !sip_media_bridge_is_in_callback(NULL),
	      "bridge null accessors are inconsistent");
	return true;
}

static bool test_media_trace_intervals_and_lifecycle(void)
{
	struct bridge_fixture fixture;
	struct callback_capture capture = { 0 };
	struct sip_media_event media = {
		.state = SIP_MEDIA_CONFIGURED,
		.remote_address_family = SIP_MEDIA_ADDRESS_IPV4,
		.codec = MEDIA_CODEC_PCMU,
		.payload_type = 0,
		.sample_rate = MEDIA_SAMPLE_RATE,
		.channels = 1,
		.remote_address = "127.0.0.1",
		.remote_port = 42000,
	};
	const struct sip_signaling_callbacks *source;

	CHECK(open_fixture(&fixture, &capture), "could not open trace fixture");
	capture.bridge = &fixture.bridge;
	source = sip_media_bridge_callbacks(&fixture.bridge);
	media.local_port = rtp_endpoint_local_port(&fixture.endpoint);
	sip_media_bridge_trace_media(&fixture.bridge, 0);
	CHECK(capture.trace == 0, "unconfigured media produced diagnostics");
	source->media_changed(source->context, &media);
	sip_media_bridge_trace_media(&fixture.bridge, 0);
	CHECK(capture.trace == 1 && capture.observed_callback_depth &&
	      strstr(capture.trace_text, "RTP PCMU/8000/1 pt=0") &&
	      strstr(capture.trace_text, "remote=127.0.0.1:42000 (SDP)") &&
	      strstr(capture.trace_text, "TX=0 RX=0 accepted=0"),
	      "initial trace did not expose an empty media path");
	fixture.endpoint.stats.receive_datagrams = 4;
	fixture.endpoint.stats.receive_filtered = 1;
	fixture.endpoint.stats.transmit_sent = 2;
	fixture.stream.receiver.stats.packets_accepted = 1;
	fixture.stream.receiver.stats.malformed_packets = 1;
	fixture.stream.receiver.stats.unexpected_payloads = 1;
	fixture.stream.receiver.stats.samples_buffered = 8;
	fixture.stream.receiver.stats.samples_played = 8;
	fixture.stream.receiver.stats.samples_concealed = 16;
	fixture.endpoint.learned_address = fixture.endpoint.remote_address;
	fixture.endpoint.learned_address.sin_port = htons(43000);
	fixture.endpoint.remote_learned = true;
	sip_media_bridge_trace_media(&fixture.bridge, 999999999);
	CHECK(capture.trace == 1, "trace flooded within its interval");
	sip_media_bridge_trace_media(&fixture.bridge, 1000000000);
	CHECK(capture.trace == 2 &&
	      strstr(capture.trace_text, "remote=127.0.0.1:43000 (learned)") &&
	      strstr(capture.trace_text, "TX=2 RX=4 accepted=1") &&
	      strstr(capture.trace_text, "filtered=1 malformed=1 bad-pt=1") &&
	      strstr(capture.trace_text, "buffered=8 played=8 concealed=16"),
	      "trace hid route learning, rejected packets or concealed media");
	sip_media_bridge_trace_media(&fixture.bridge, 2000000000);
	CHECK(capture.trace == 3 &&
	      strstr(capture.trace_text, "TX=0 RX=0 accepted=0") &&
	      strstr(capture.trace_text, "buffered=0 played=0 concealed=0"),
	      "previous traffic masked an interval with no received media");
	sip_media_bridge_clear_media(&fixture.bridge);
	sip_media_bridge_trace_media(&fixture.bridge, 3000000000);
	CHECK(capture.trace == 3, "cleared media continued tracing");
	source->media_changed(source->context, &media);
	sip_media_bridge_trace_media(&fixture.bridge, 3000000000);
	CHECK(capture.trace == 4 &&
	      strstr(capture.trace_text, "TX=0 RX=0 accepted=0"),
	      "a new call inherited the previous call's counts");
	fixture.bridge.destination.trace = NULL;
	sip_media_bridge_trace_media(&fixture.bridge, 4000000000);
	CHECK(capture.trace == 4, "disabled tracing still invoked its sink");
	rtp_endpoint_close(&fixture.endpoint);
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "forward and bind", test_forwards_and_binds_before_media_callback },
		{ "media cleanup admission", test_admission_waits_for_media_cleanup },
		{ "initialization validation", test_validates_initialization },
		{ "media trace intervals", test_media_trace_intervals_and_lifecycle },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		printf("[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			printf("[       OK ] %s\n", tests[index].name);
		else {
			printf("[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
