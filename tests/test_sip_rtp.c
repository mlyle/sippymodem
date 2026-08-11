// SPDX-License-Identifier: BSD-3-Clause

#include <sip_rtp.h>

#include "test.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_SLOTS = 16,
	TEST_WORKSPACE = 8,
};

struct binding_fixture {
	struct sippymodem_rtp_jitter_slot slots[TEST_SLOTS];
	struct sippymodem_rtp_stream stream;
	struct sippymodem_rtp_endpoint endpoint;
	uint8_t workspace[TEST_WORKSPACE];
};

static bool open_fixture(struct binding_fixture *fixture)
{
	const struct sippymodem_rtp_stream_config stream_config = {
		.codec = SIPPYMODEM_CODEC_PCMU,
		.payload_type = 0,
		.transmit_sequence = 1,
		.transmit_timestamp = 2,
		.transmit_ssrc = 3,
	};
	const struct sippymodem_rtp_endpoint_config endpoint_config = {
		.learn_remote = true,
		.symmetric_rtp = true,
	};
	const struct sockaddr_in local = {
		.sin_family = AF_INET,
		.sin_port = 0,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};

	memset(fixture, 0, sizeof(*fixture));
	return sippymodem_rtp_stream_init(
		       &fixture->stream, &stream_config, fixture->slots,
		       ARRAY_LENGTH(fixture->slots), fixture->workspace,
		       sizeof(fixture->workspace)) &&
	       sippymodem_rtp_endpoint_init(
		       &fixture->endpoint, &fixture->stream, &endpoint_config) &&
	       sippymodem_rtp_endpoint_open(&fixture->endpoint, &local) ==
		       SIPPYMODEM_RTP_ENDPOINT_OK;
}

static struct sippymodem_sip_media_event configured_event(
	const struct binding_fixture *fixture)
{
	const struct sippymodem_sip_media_event event = {
		.state = SIPPYMODEM_SIP_MEDIA_CONFIGURED,
		.remote_address_family = SIPPYMODEM_SIP_MEDIA_ADDRESS_IPV4,
		.codec = SIPPYMODEM_CODEC_PCMU,
		.payload_type = 0,
		.sample_rate = SIPPYMODEM_MEDIA_SAMPLE_RATE,
		.channels = 1,
		.local_port =
			sippymodem_rtp_endpoint_local_port(&fixture->endpoint),
		.remote_address = "127.0.0.1",
		.remote_port = 42000,
	};

	return event;
}

static bool remote_is(const struct binding_fixture *fixture,
		      const char *address, unsigned int port)
{
	struct sockaddr_in remote;
	struct in_addr expected;
	bool learned = true;

	return inet_pton(AF_INET, address, &expected) == 1 &&
	       sippymodem_rtp_endpoint_transmit_remote(
		       &fixture->endpoint, &remote, &learned) &&
	       !learned && remote.sin_family == AF_INET &&
	       remote.sin_port == htons((uint16_t)port) &&
	       remote.sin_addr.s_addr == expected.s_addr;
}

static bool test_configure_and_clear(void)
{
	struct binding_fixture fixture;
	struct sippymodem_sip_media_event event;
	const struct sippymodem_sip_media_event inactive = {
		.state = SIPPYMODEM_SIP_MEDIA_INACTIVE,
	};
	struct sockaddr_in unused;

	CHECK(open_fixture(&fixture), "could not open RTP binding fixture");
	event = configured_event(&fixture);
	CHECK(sippymodem_sip_rtp_apply(&fixture.endpoint, &event) ==
		      SIPPYMODEM_SIP_RTP_OK &&
		      remote_is(&fixture, "127.0.0.1", 42000),
	      "configured media did not install the RTP peer");
	CHECK(sippymodem_sip_rtp_apply(&fixture.endpoint, &event) ==
		      SIPPYMODEM_SIP_RTP_OK,
	      "repeated media configuration was not idempotent");
	fixture.stream.receiver.stream_active = true;
	CHECK(sippymodem_sip_rtp_apply(&fixture.endpoint, &inactive) ==
		      SIPPYMODEM_SIP_RTP_OK &&
		      !sippymodem_rtp_endpoint_transmit_remote(
			      &fixture.endpoint, &unused, NULL) &&
		      !fixture.stream.receiver.stream_active,
	      "inactive media did not clear and restart the RTP peer");
	sippymodem_rtp_endpoint_close(&fixture.endpoint);
	return true;
}

static bool test_rejects_invalid_updates_transactionally(void)
{
	struct binding_fixture fixture;
	struct sippymodem_sip_media_event event;

	CHECK(open_fixture(&fixture), "could not open RTP validation fixture");
	event = configured_event(&fixture);
	CHECK(sippymodem_sip_rtp_apply(&fixture.endpoint, &event) ==
		      SIPPYMODEM_SIP_RTP_OK,
	      "could not install initial RTP peer");

	event.payload_type = 8;
	CHECK(sippymodem_sip_rtp_apply(&fixture.endpoint, &event) ==
		      SIPPYMODEM_SIP_RTP_INCOMPATIBLE_MEDIA &&
		      remote_is(&fixture, "127.0.0.1", 42000),
	      "payload mismatch changed the existing RTP peer");
	event = configured_event(&fixture);
	event.remote_address_family = SIPPYMODEM_SIP_MEDIA_ADDRESS_IPV6;
	event.remote_address = "::1";
	CHECK(sippymodem_sip_rtp_apply(&fixture.endpoint, &event) ==
		      SIPPYMODEM_SIP_RTP_UNSUPPORTED_ADDRESS &&
		      remote_is(&fixture, "127.0.0.1", 42000),
	      "unsupported address changed the existing RTP peer");
	event = configured_event(&fixture);
	event.remote_address_family =
		SIPPYMODEM_SIP_MEDIA_ADDRESS_UNSPECIFIED;
	CHECK(sippymodem_sip_rtp_apply(&fixture.endpoint, &event) ==
		      SIPPYMODEM_SIP_RTP_INVALID_ARGUMENT &&
		      remote_is(&fixture, "127.0.0.1", 42000),
	      "unspecified address changed the existing RTP peer");
	event = configured_event(&fixture);
	event.remote_address = "not-an-address";
	CHECK(sippymodem_sip_rtp_apply(&fixture.endpoint, &event) ==
		      SIPPYMODEM_SIP_RTP_INVALID_ADDRESS &&
		      remote_is(&fixture, "127.0.0.1", 42000),
	      "invalid address changed the existing RTP peer");
	sippymodem_rtp_endpoint_close(&fixture.endpoint);
	event = configured_event(&fixture);
	CHECK(sippymodem_sip_rtp_apply(&fixture.endpoint, &event) ==
		      SIPPYMODEM_SIP_RTP_ENDPOINT_ERROR,
	      "closed RTP endpoint accepted configured media");
	CHECK(strcmp(sippymodem_sip_rtp_result_name(
			     SIPPYMODEM_SIP_RTP_INCOMPATIBLE_MEDIA),
		     "negotiated media does not match the RTP endpoint") == 0,
	      "SIP RTP result name was wrong");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "configure and clear", test_configure_and_clear },
		{ "transactional rejection",
		  test_rejects_invalid_updates_transactionally },
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
