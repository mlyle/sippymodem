// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <modem.h>
#include <modem_frontend.h>
#include <modem_state.h>
#include <v34_profile.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

enum { FRAGMENT = 80, PAYLOAD = 128, TRAINING_LIMIT = 50 * 8000 };

static unsigned long ticks;
unsigned long __wrap_get_time(void) { return ticks; }

/* Exercise the caller's nonlinear, expanded transmit mapping with an
 * answerer configured to receive it. The ordinary caller receive profile
 * remains linear. This does not substitute for independent modem interop. */
enum v34_profile_result __real_v34_profile_init(
	struct v34_call_configuration *, uint16_t *, enum v34_station_role,
	unsigned int, unsigned int, unsigned int);
enum v34_profile_result __wrap_v34_profile_init(
	struct v34_call_configuration *configuration, uint16_t *mask,
	enum v34_station_role role, unsigned int minimum, unsigned int maximum,
	unsigned int baud)
{
	enum v34_profile_result result = __real_v34_profile_init(
		configuration, mask, role, minimum, maximum, baud);
	if (result == V34_PROFILE_OK && role == V34_ANSWERING_STATION) {
		struct v34_mp_parameters *parameters =
			v34_mp_record_common_parameters_mutable(
				&configuration->phase4.transmit_mp);
		parameters->remote_transmitter_nonlinear_encoding = true;
		parameters->remote_transmitter_expanded_shaping = true;
		parameters->asymmetric_rates_enabled = false;
	}
	return result;
}

struct endpoint {
	struct modem *modem;
	uint8_t received[PAYLOAD];
	size_t count;
	char reply[128];
	size_t reply_count;
	bool overflow;
};

static int start(struct modem *modem) { (void)modem; return 0; }
static int stop(struct modem *modem) { (void)modem; return 0; }
static int control(struct modem *modem, unsigned int command, unsigned long value)
{ (void)modem; (void)command; (void)value; return 0; }

static int receive(void *context, const char *bytes, int count)
{
	struct endpoint *endpoint = context;
	if (count < 0 || (size_t)count > sizeof(endpoint->received) - endpoint->count) {
		endpoint->overflow = true;
		return -1;
	}
	memcpy(endpoint->received + endpoint->count, bytes, (size_t)count);
	endpoint->count += (size_t)count;
	return count;
}

static int reply(void *context, const char *bytes, int count)
{
	struct endpoint *endpoint = context;
	if (count < 0)
		return -1;
	size_t copied = (size_t)count;
	if (copied > sizeof(endpoint->reply) - 1 - endpoint->reply_count)
		copied = sizeof(endpoint->reply) - 1 - endpoint->reply_count;
	memcpy(endpoint->reply + endpoint->reply_count, bytes, copied);
	endpoint->reply_count += copied;
	endpoint->reply[endpoint->reply_count] = '\0';
	return count;
}

static bool command(struct endpoint *endpoint, const char *text)
{
	endpoint->reply_count = 0;
	endpoint->reply[0] = '\0';
	CHECK(modem_write(endpoint->modem, text, (int)strlen(text)) == (int)strlen(text) &&
	      modem_write(endpoint->modem, "\r", 1) == 1 &&
	      strstr(endpoint->reply, "\r\nOK\r\n") != NULL,
	      "command failed: %s: %s", text, endpoint->reply);
	return true;
}

static bool ready(const struct endpoint *endpoint)
{
	return endpoint->modem->state == STATE_MODEM_ONLINE &&
	       endpoint->modem->dp != NULL && endpoint->modem->dp->status == DPSTAT_CONNECT;
}

static bool roundtrip(enum g711_law law, unsigned int rate, bool compression)
{
	struct modem_driver driver = { "V.34 duplex", start, stop, control };
	struct endpoint endpoints[2] = { 0 };
	uint8_t payload[2][PAYLOAD], wire[2][FRAGMENT];
	char selection[64];
	snprintf(selection, sizeof(selection), "AT+MS=34,0,%u,%u", rate, rate);
	ticks = 100000;
	for (unsigned int side = 0; side < 2; ++side) {
		struct endpoint *endpoint = &endpoints[side];
		endpoint->modem = modem_create(&driver, "V.34 duplex");
		CHECK(endpoint->modem != NULL && endpoint->modem->pty == -1,
		      "native endpoint did not initialize");
		modem_frontend_init(endpoint->modem);
		endpoint->modem->data_output = receive;
		endpoint->modem->data_output_context = endpoint;
		endpoint->modem->control_output = reply;
		endpoint->modem->control_output_context = endpoint;
		CHECK(command(endpoint, "ATZ") && command(endpoint, "ATE0V1Q0") &&
		      command(endpoint, "ATX3\\N4+V34B=3200") && command(endpoint, selection) &&
		      command(endpoint, compression ? "AT+DS=3,1+DSC=1" : "AT+DS=0"),
		      "native endpoint setup failed");
		for (size_t byte = 0; byte < PAYLOAD; ++byte)
			payload[side][byte] = (uint8_t)(byte * (side ? 29u : 73u) + side * 47u);
		/* Literal binary and escape-like bytes must survive both codecs. */
		memcpy(payload[side], "\0\xff\x7e\x7d+++\rAT\r", 11);
		memset(wire[side], g711_encode(law, 0), FRAGMENT);
	}
	CHECK(modem_answer(endpoints[1].modem) == 0 &&
	      modem_dial(endpoints[0].modem) == 0 &&
	      modem_report_outgoing_call_connected(endpoints[0].modem) == 0,
	      "native call did not start");
	unsigned int answer_ready = 0, both_ready = 0;
	bool submitted[2] = { false, false }, completed = false;
	for (unsigned int sample = 0; sample < TRAINING_LIMIT; sample += FRAGMENT) {
		uint8_t output[2][FRAGMENT];
		for (unsigned int side = 0; side < 2; ++side) {
			int16_t input[FRAGMENT], transmitted[FRAGMENT];
			for (size_t index = 0; index < FRAGMENT; ++index)
				input[index] = g711_decode(law, wire[side ^ 1u][index]);
			bool exact;
			modem_process_g711(endpoints[side].modem, law, wire[side ^ 1u],
				input, NULL, output[side], transmitted, FRAGMENT, &exact);
			if (!exact)
				for (size_t index = 0; index < FRAGMENT; ++index)
					output[side][index] = g711_encode(law, transmitted[index]);
		}
		memcpy(wire, output, sizeof(wire));
		ticks += 1;
		for (unsigned int side = 0; side < 2; ++side) {
			for (unsigned int event = 0;
			     event < 16 && modem_service_event(endpoints[side].modem) > 0; ++event) {}
			CHECK(!endpoints[side].overflow && endpoints[side].modem->started,
			      "endpoint %u stopped at %u: %s", side, sample,
			      modem_protocol_step(endpoints[side].modem));
		}
		if (!answer_ready && ready(&endpoints[1]))
			answer_ready = sample + FRAGMENT;
		if (!both_ready && ready(&endpoints[0]) && ready(&endpoints[1]))
			both_ready = sample + FRAGMENT;
		/* A greeting may precede the caller's readiness. Its response starts
		 * shortly after readiness, rather than after a long idle warmup. */
		const unsigned int send_at[2] = { both_ready + 4000u, answer_ready + 800u };
		for (unsigned int side = 0; side < 2; ++side) {
			if (!submitted[side] && (side ? answer_ready : both_ready) &&
			    sample + FRAGMENT >= send_at[side]) {
				CHECK(modem_data_write(endpoints[side].modem,
					(const char *)payload[side], PAYLOAD) == PAYLOAD,
				      "endpoint %u rejected initial data", side);
				submitted[side] = true;
			}
		}
		if (submitted[0] && submitted[1] && endpoints[0].count == PAYLOAD &&
		    endpoints[1].count == PAYLOAD && modem_ec_transmit_idle(endpoints[0].modem) &&
		    modem_ec_transmit_idle(endpoints[1].modem)) {
			completed = true;
			break;
		}
		if (both_ready && sample + FRAGMENT - both_ready >= 2u * 8000u)
			break;
	}
	CHECK(completed, "initial duplex timed out: law=%u rate=%u compression=%u received=%zu/%zu",
	      law, rate, compression, endpoints[0].count, endpoints[1].count);
	for (unsigned int side = 0; side < 2; ++side) {
		struct modem *modem = endpoints[side].modem;
		CHECK(ready(&endpoints[side]) && modem->dp->id == DP_V34 &&
		      modem->tx_rate == rate && modem->rx_rate == rate &&
		      modem->cfg.ec && modem->cfg.comp == (compression ? 3u : 0u) &&
		      modem_ec_active_compression(modem) ==
			(compression ? MODEM_EC_COMPRESSION_V42BIS : MODEM_EC_COMPRESSION_NONE) &&
		      modem->ec.lapm.v44_path == NULL &&
		      memcmp(endpoints[side].received, payload[side ^ 1u], PAYLOAD) == 0,
		      "endpoint %u lost negotiated mode or literal data", side);
		modem_delete(modem);
	}
	return true;
}

int main(void)
{
	const unsigned int rates[] = { 16800, 28800 };
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (size_t rate = 0; rate < ARRAY_LENGTH(rates); ++rate)
			for (unsigned int compression = 0; compression < 2; ++compression) {
				if (!roundtrip(law, rates[rate], compression != 0))
					return 1;
				printf("[       OK ] V.34 initial duplex law=%u rate=%u V.42bis=%u\n",
				       law, rates[rate], compression);
			}
	return 0;
}
