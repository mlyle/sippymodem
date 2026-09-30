// SPDX-License-Identifier: BSD-3-Clause

#include <lapm_v44_session.h>

#include "test.h"
#include "lapm_xid_fixture.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static bool offers_equal(
	const struct v44_offer *left,
	const struct v44_offer *right)
{
	return left->transmit.enabled == right->transmit.enabled &&
	       left->transmit.parameters.codeword_limit ==
		       right->transmit.parameters.codeword_limit &&
	       left->transmit.parameters.max_string_length ==
		       right->transmit.parameters.max_string_length &&
	       left->transmit.parameters.history_size ==
		       right->transmit.parameters.history_size &&
	       left->receive.enabled == right->receive.enabled &&
	       left->receive.parameters.codeword_limit ==
		       right->receive.parameters.codeword_limit &&
	       left->receive.parameters.max_string_length ==
		       right->receive.parameters.max_string_length &&
	       left->receive.parameters.history_size ==
		       right->receive.parameters.history_size;
}

static struct lapm_v44_config config_for(
	struct v44_offer offer)
{
	const struct lapm_v44_config config = {
		.enabled = true,
		.native_v44_ready = true,
		.local_offer = offer,
	};

	return config;
}

static bool decode_v44(
	const uint8_t *wire,
	size_t wire_length,
	struct v44_offer *offer)
{
	struct v42_xid_fields fields;

	return v42_xid_decode(wire, wire_length, &fields) ==
		       V42_XID_OK &&
	       fields.user_data != NULL &&
	       v44_xid_decode(
		       fields.user_data, fields.user_data_length, offer) ==
		       V44_XID_OK;
}

static bool build_outer(
	const uint8_t *user_data,
	size_t user_data_length,
	uint8_t *wire,
	size_t *wire_length)
{
	const struct v42_xid_fields fields = {
		.user_data = user_data,
		.user_data_length = user_data_length,
	};

	return v42_xid_encode(
		       &fields, wire, LAPM_V44_MAX_XID_SIZE,
		       wire_length) == V42_XID_OK;
}

static bool test_asymmetric_exchange(void)
{
	static const uint8_t parameters[] = { 0x03, 0x01, 0x00 };
	static const uint8_t private_parameters[] = { 0x00, 0x01, 0x01 };
	const struct v42_xid_fields base = {
		.parameter_negotiation = parameters,
		.parameter_negotiation_length = sizeof(parameters),
		.private_negotiation = private_parameters,
		.private_negotiation_length = sizeof(private_parameters),
	};
	const struct v44_offer calling_offer = {
		.transmit = { true, { 4096, 200, 4096 } },
		.receive = { true, { 2048, 100, 2000 } },
	};
	const struct v44_offer answering_offer = {
		.transmit = { true, { 1024, 150, 3000 } },
		.receive = { true, { 8192, 80, 1000 } },
	};
	const struct v44_offer expected_answer = {
		.transmit = { true, { 1024, 100, 2000 } },
		.receive = { true, { 4096, 80, 1000 } },
	};
	const struct v44_offer expected_caller = {
		.transmit = { true, { 4096, 80, 1000 } },
		.receive = { true, { 1024, 100, 2000 } },
	};
	const struct lapm_v44_config calling_config =
		config_for(calling_offer);
	const struct lapm_v44_config answering_config =
		config_for(answering_offer);
	struct lapm_v44_session caller;
	struct lapm_v44_session answerer;
	struct lapm_v44_prepared command;
	struct lapm_v44_prepared response;
	struct v44_offer decoded;
	struct v44_offer selected;
	struct v42_xid_fields decoded_fields;
	uint8_t ambiguous[LAPM_V44_MAX_XID_SIZE];
	size_t ambiguous_length = 0;

	CHECK(lapm_v44_session_init(&caller, &calling_config) ==
		      LAPM_V44_OK &&
	      lapm_v44_session_init(&answerer, &answering_config) ==
		      LAPM_V44_OK,
	      "could not initialize asymmetric V.44 sessions");
	CHECK(lapm_v44_prepare_command(&caller, &base, &command) ==
		      LAPM_V44_OK &&
	      caller.phase == LAPM_V44_READY &&
	      v42_xid_decode(
		      command.data, command.length, &decoded_fields) ==
		      V42_XID_OK &&
	      decoded_fields.private_negotiation_length ==
		      sizeof(private_parameters) &&
	      decode_v44(command.data, command.length, &decoded) &&
	      offers_equal(&decoded, &calling_offer),
	      "preparing a command changed state or encoded the wrong offer");
	CHECK(lapm_v44_commit(&caller, &command) ==
		      LAPM_V44_OK &&
	      caller.phase == LAPM_V44_COMMAND_PENDING,
	      "queued command did not become pending");
	CHECK(lapm_v44_prepare_response(
		      &answerer, command.data, command.length, &base, &response) ==
		      LAPM_V44_OK &&
	      answerer.phase == LAPM_V44_READY &&
	      v42_xid_decode(
		      response.data, response.length, &decoded_fields) ==
		      V42_XID_OK &&
	      decoded_fields.private_negotiation == NULL &&
	      decode_v44(response.data, response.length, &decoded) &&
	      offers_equal(&decoded, &expected_answer),
	      "answer preparation did not select only V.44 asymmetric minima");
	CHECK(lapm_v44_commit(&answerer, &response) ==
		      LAPM_V44_OK &&
	      answerer.phase == LAPM_V44_NEGOTIATED &&
	      lapm_v44_get_negotiated_offer(
		      &answerer, &selected) == LAPM_V44_OK &&
	      offers_equal(&selected, &expected_answer),
	      "queued response did not commit responder selection");
	decoded_fields.private_negotiation = private_parameters;
	decoded_fields.private_negotiation_length = sizeof(private_parameters);
	CHECK(v42_xid_encode(
		      &decoded_fields, ambiguous, sizeof(ambiguous),
		      &ambiguous_length) == V42_XID_OK &&
	      lapm_v44_accept_response(
		      &caller, ambiguous, ambiguous_length) ==
		      LAPM_V44_MALFORMED &&
	      caller.phase == LAPM_V44_COMMAND_PENDING,
	      "an ambiguous dual-compression response completed negotiation");
	CHECK(lapm_v44_accept_response(
		      &caller, response.data, response.length) ==
		      LAPM_V44_OK &&
	      caller.phase == LAPM_V44_NEGOTIATED &&
	      lapm_v44_get_negotiated_offer(
		      &caller, &selected) == LAPM_V44_OK &&
	      offers_equal(&selected, &expected_caller),
	      "caller did not invert directions and select asymmetric minima");
	return true;
}

static bool test_queue_transactions_and_retransmission(void)
{
	uint8_t oversized_parameter[100] = { 0x7e, 98 };
	const struct v42_xid_fields oversized = {
		.parameter_negotiation = oversized_parameter,
		.parameter_negotiation_length = sizeof(oversized_parameter),
	};
	const struct lapm_v44_config config =
		config_for(v44_default_offer());
	struct lapm_v44_session session;
	struct lapm_v44_prepared first;
	struct lapm_v44_prepared second;
	struct lapm_v44_prepared unchanged;
	struct lapm_v44_prepared before;

	memset(&unchanged, 0xa5, sizeof(unchanged));
	before = unchanged;
	CHECK(lapm_v44_session_init(&session, &config) ==
		      LAPM_V44_OK &&
	      lapm_v44_prepare_command(
		      &session, &oversized, &unchanged) ==
		      LAPM_V44_XID_TOO_LARGE &&
	      memcmp(&unchanged, &before, sizeof(unchanged)) == 0 &&
	      session.phase == LAPM_V44_READY,
	      "output pressure changed a transaction or session");
	CHECK(lapm_v44_prepare_command(&session, NULL, &first) ==
		      LAPM_V44_OK &&
	      lapm_v44_prepare_command(&session, NULL, &second) ==
		      LAPM_V44_OK &&
	      first.length == second.length &&
	      memcmp(first.data, second.data, first.length) == 0 &&
	      session.phase == LAPM_V44_READY,
	      "retry after queue failure was not deterministic");
	CHECK(lapm_v44_commit(&session, &first) ==
		      LAPM_V44_OK &&
	      lapm_v44_commit(&session, &second) ==
		      LAPM_V44_STALE_TRANSACTION,
	      "a second prepared command committed twice");
	CHECK(lapm_v44_prepare_retransmission(&session, &second) ==
		      LAPM_V44_OK &&
	      second.length == first.length &&
	      memcmp(second.data, first.data, first.length) == 0 &&
	      session.phase == LAPM_V44_COMMAND_PENDING,
	      "pending command retransmission changed its wire image");
	CHECK(lapm_v44_commit(&session, &second) ==
		      LAPM_V44_OK &&
	      session.phase == LAPM_V44_COMMAND_PENDING,
	      "committed retransmission changed negotiation phase");
	return true;
}

static bool test_duplicate_command(void)
{
	static const uint8_t alternate_parameters[] = { 0x03, 0x01, 0x01 };
	const struct v42_xid_fields alternate_base = {
		.parameter_negotiation = alternate_parameters,
		.parameter_negotiation_length = sizeof(alternate_parameters),
	};
	const struct lapm_v44_config config =
		config_for(v44_default_offer());
	struct lapm_v44_session caller;
	struct lapm_v44_session answerer;
	struct lapm_v44_prepared command;
	struct lapm_v44_prepared alternate_command;
	struct lapm_v44_prepared first;
	struct lapm_v44_prepared duplicate;

	CHECK(lapm_v44_session_init(&caller, &config) ==
		      LAPM_V44_OK &&
	      lapm_v44_session_init(&answerer, &config) ==
		      LAPM_V44_OK &&
	      lapm_v44_prepare_command(&caller, NULL, &command) ==
		      LAPM_V44_OK &&
	      lapm_v44_prepare_command(
		      &caller, &alternate_base, &alternate_command) ==
		      LAPM_V44_OK &&
	      lapm_v44_prepare_response(
		      &answerer, command.data, command.length, NULL, &first) ==
		      LAPM_V44_OK &&
	      answerer.phase == LAPM_V44_READY,
	      "could not prepare duplicate-command fixture");
	CHECK(lapm_v44_commit(&answerer, &first) ==
		      LAPM_V44_OK &&
	      lapm_v44_prepare_response(
		      &answerer, command.data, command.length, &alternate_base,
		      &duplicate) ==
		      LAPM_V44_OK &&
	      duplicate.length == first.length &&
	      memcmp(duplicate.data, first.data, first.length) == 0 &&
	      lapm_v44_commit(&answerer, &duplicate) ==
		      LAPM_V44_OK &&
	      answerer.phase == LAPM_V44_NEGOTIATED,
	      "duplicate command did not replay the committed response");
	CHECK(lapm_v44_prepare_response(
		      &answerer, alternate_command.data, alternate_command.length,
		      NULL, &duplicate) == LAPM_V44_INVALID_STATE,
	      "a different valid command was mistaken for a duplicate");
	return true;
}

static bool test_decline_and_malformed_tails(void)
{
	static const uint8_t unrelated[] = { 0xff, 0xfe, 0x01, 0xa5 };
	static const uint8_t private_parameters[] = { 0x00, 0x01, 0x01 };
	static const uint8_t malformed_v44[] = {
		0xff, 0x40, 0x03, 'V', '4', '4', 0x41, 0x01, 0x3e,
	};
	const struct lapm_v44_config config =
		config_for(v44_default_offer());
	const struct v42_xid_fields legacy = {
		.private_negotiation = private_parameters,
		.private_negotiation_length = sizeof(private_parameters),
	};
	struct lapm_v44_session session;
	struct lapm_v44_prepared response;
	struct v42_xid_fields decoded;
	uint8_t command[LAPM_V44_MAX_XID_SIZE];
	size_t command_length = 0;

	CHECK(lapm_v44_session_init(&session, &config) ==
		      LAPM_V44_OK &&
	      build_outer(NULL, 0, command, &command_length) &&
	      lapm_v44_prepare_response(
		      &session, command, command_length, &legacy, &response) ==
		      LAPM_V44_OK &&
	      v42_xid_decode(
		      response.data, response.length, &decoded) ==
		      V42_XID_OK &&
	      decoded.user_data == NULL &&
	      decoded.private_negotiation_length == sizeof(private_parameters) &&
	      memcmp(decoded.private_negotiation, private_parameters,
		     sizeof(private_parameters)) == 0 &&
	      lapm_v44_commit(&session, &response) ==
		      LAPM_V44_OK &&
	      session.phase == LAPM_V44_DECLINED,
	      "missing V.44 user data did not preserve legacy fallback");
	CHECK(lapm_v44_session_call_reset(&session) ==
		      LAPM_V44_OK &&
	      build_outer(unrelated, sizeof(unrelated), command, &command_length) &&
	      lapm_v44_prepare_response(
		      &session, command, command_length, NULL, &response) ==
		      LAPM_V44_OK &&
	      lapm_v44_commit(&session, &response) ==
		      LAPM_V44_OK &&
	      session.phase == LAPM_V44_DECLINED,
	      "unrelated user data was not a clean decline");
	CHECK(lapm_v44_session_call_reset(&session) ==
		      LAPM_V44_OK &&
	      build_outer(
		      malformed_v44, sizeof(malformed_v44), command,
		      &command_length) &&
	      lapm_v44_prepare_response(
		      &session, command, command_length, NULL, &response) ==
		      LAPM_V44_MALFORMED &&
	      session.phase == LAPM_V44_READY,
	      "an explicitly malformed V.44 claim changed responder state");
	return true;
}

static bool test_response_errors_and_reset(void)
{
	static const uint8_t malformed_v44[] = {
		0xff, 0x40, 0x03, 'V', '4', '4', 0x42, 0x01, 0xfc,
	};
	const struct lapm_v44_config config =
		config_for(v44_default_offer());
	struct lapm_v44_session session;
	struct lapm_v44_prepared command;
	uint8_t response[LAPM_V44_MAX_XID_SIZE];
	size_t response_length = 0;

	CHECK(lapm_v44_session_init(&session, &config) ==
		      LAPM_V44_OK &&
	      build_outer(NULL, 0, response, &response_length) &&
	      lapm_v44_accept_response(
		      &session, response, response_length) ==
		      LAPM_V44_INVALID_STATE &&
	      lapm_v44_prepare_command(&session, NULL, &command) ==
		      LAPM_V44_OK &&
	      lapm_v44_commit(&session, &command) ==
		      LAPM_V44_OK,
	      "could not establish pending-response fixture");
	CHECK(build_outer(
		      malformed_v44, sizeof(malformed_v44), response,
		      &response_length) &&
	      lapm_v44_accept_response(
		      &session, response, response_length) ==
		      LAPM_V44_MALFORMED &&
	      session.phase == LAPM_V44_COMMAND_PENDING,
	      "malformed claimed response did not remain pending");
	CHECK(build_outer(NULL, 0, response, &response_length) &&
	      lapm_v44_accept_response(
		      &session, response, response_length) ==
		      LAPM_V44_OK &&
	      session.phase == LAPM_V44_DECLINED,
	      "missing V.44 response did not decline pending negotiation");
	CHECK(lapm_v44_session_call_reset(&session) ==
		      LAPM_V44_OK &&
	      session.phase == LAPM_V44_READY &&
	      session.config.enabled == config.enabled &&
	      session.config.native_v44_ready == config.native_v44_ready &&
	      offers_equal(&session.config.local_offer, &config.local_offer),
	      "call reset did not retain local V.44 configuration");
	return true;
}

static bool test_dual_compression_retry(void)
{
	const struct lapm_v44_config config = config_for(v44_default_offer());
	struct lapm_v44_session session, before;
	struct lapm_v44_prepared command, retry, stale, retransmission;
	struct v42_xid_fields base, decoded;

	CHECK(v42_xid_decode(lapm_dual_compression_response,
		 sizeof(lapm_dual_compression_response), &base) == V42_XID_OK,
	      "dual compression fixture did not decode");
	base.user_data = NULL;
	base.user_data_length = 0;
	CHECK(lapm_v44_session_init(&session, &config) == LAPM_V44_OK &&
	      lapm_v44_prepare_command(&session, &base, &command) == LAPM_V44_OK &&
	      lapm_v44_commit(&session, &command) == LAPM_V44_OK,
	      "dual compression command setup failed");
	before = session;
	CHECK(lapm_v44_accept_response(&session, lapm_dual_compression_response,
		 sizeof(lapm_dual_compression_response)) == LAPM_V44_MALFORMED &&
	      memcmp(&session, &before, sizeof(session)) == 0 &&
	      lapm_v44_prepare_retry_without_v44(&session, lapm_dual_compression_response,
		 sizeof(lapm_dual_compression_response), &retry) == LAPM_V44_OK &&
	      memcmp(&session, &before, sizeof(session)) == 0,
	      "rejected response or unqueued retry changed session state");
	stale = retry;
	CHECK(v42_xid_decode(retry.data, retry.length, &decoded) == V42_XID_OK &&
	      decoded.user_data == NULL &&
	      decoded.private_negotiation_length == base.private_negotiation_length &&
	      memcmp(decoded.private_negotiation, base.private_negotiation,
		 base.private_negotiation_length) == 0 &&
	      decoded.parameter_negotiation_length == base.parameter_negotiation_length &&
	      memcmp(decoded.parameter_negotiation, base.parameter_negotiation,
		 base.parameter_negotiation_length) == 0,
	      "retry did not retain the frozen LAPM and V.42bis parameters");
	CHECK(lapm_v44_commit(&session, &retry) == LAPM_V44_OK &&
	      lapm_v44_commit(&session, &stale) == LAPM_V44_STALE_TRANSACTION &&
	      lapm_v44_prepare_retry_without_v44(&session, lapm_dual_compression_response,
		 sizeof(lapm_dual_compression_response), &retry) == LAPM_V44_INVALID_STATE &&
	      lapm_v44_prepare_retransmission(&session, &retransmission) == LAPM_V44_OK &&
	      retransmission.length == stale.length &&
	      memcmp(retransmission.data, stale.data, stale.length) == 0,
	      "retry was repeated or its retransmission changed");
	CHECK(lapm_v44_accept_response(&session, lapm_dual_compression_response,
		 sizeof(lapm_dual_compression_response)) == LAPM_V44_OK &&
	      session.phase == LAPM_V44_DECLINED &&
	      lapm_v44_session_call_reset(&session) == LAPM_V44_OK &&
	      lapm_v44_prepare_command(&session, &base, &command) == LAPM_V44_OK &&
	      decode_v44(command.data, command.length, &session.command_offer) &&
	      session.command_offer.transmit.enabled && session.command_offer.receive.enabled,
	      "V.42bis reply failed or a new call lost the configured V.44 offer");
	CHECK(lapm_v44_commit(&session, &command) == LAPM_V44_OK &&
	      lapm_v44_prepare_retry_without_v44(&session, lapm_dual_compression_response,
		 sizeof(lapm_dual_compression_response) - 1, &retry) == LAPM_V44_MALFORMED,
	      "malformed V.44 response triggered recovery");
	base.private_negotiation = NULL;
	base.private_negotiation_length = 0;
	CHECK(lapm_v44_session_call_reset(&session) == LAPM_V44_OK &&
	      lapm_v44_prepare_command(&session, &base, &command) == LAPM_V44_OK &&
	      lapm_v44_commit(&session, &command) == LAPM_V44_OK &&
	      lapm_v44_prepare_retry_without_v44(&session, lapm_dual_compression_response,
		 sizeof(lapm_dual_compression_response), &retry) == LAPM_V44_INVALID_STATE,
	      "V.44-only command invented a V.42bis offer");
	return true;
}

static bool test_readiness_interlock(void)
{
	struct lapm_v44_config config =
		config_for(v44_default_offer());
	struct lapm_v44_session session;
	struct lapm_v44_prepared command;
	struct v44_offer decoded_offer;
	struct v42_xid_fields decoded;

	config.native_v44_ready = false;
	CHECK(lapm_v44_session_init(&session, &config) ==
		      LAPM_V44_OK &&
	      lapm_v44_prepare_command(&session, NULL, &command) ==
		      LAPM_V44_OK &&
	      decode_v44(command.data, command.length, &decoded_offer) &&
	      !decoded_offer.transmit.enabled && !decoded_offer.receive.enabled,
	      "an unready native codec emitted a nonzero V.44 request");
	config.enabled = false;
	config.native_v44_ready = true;
	CHECK(lapm_v44_session_init(&session, &config) ==
		      LAPM_V44_OK &&
	      lapm_v44_prepare_command(&session, NULL, &command) ==
		      LAPM_V44_OK &&
	      v42_xid_decode(
		      command.data, command.length, &decoded) ==
		      V42_XID_OK &&
	      decoded.user_data == NULL,
	      "disabled V.44 configuration advertised a request");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "asymmetric exchange", test_asymmetric_exchange },
		{ "queue transactions and retransmission",
		  test_queue_transactions_and_retransmission },
		{ "duplicate command", test_duplicate_command },
		{ "decline and malformed tails",
		  test_decline_and_malformed_tails },
		{ "response errors and reset", test_response_errors_and_reset },
		{ "readiness interlock", test_readiness_interlock },
		{ "bounded dual compression recovery", test_dual_compression_retry },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		else {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
