// SPDX-License-Identifier: BSD-3-Clause

#include "datapump_media.h"
#include "test.h"

static bool test_linear_fragment(void)
{
	int owner = 0;
	int16_t receive[MODEM_FRAG] = { 0 };
	int16_t transmit[MODEM_FRAG + 1];

	for (size_t i = 0; i < MODEM_FRAG + 1u; ++i)
		transmit[i] = 7;
	CHECK(datapump_linear_fragment_is_ready(
		      &owner, receive, transmit, MODEM_FRAG) &&
	      transmit[0] == 7 && transmit[MODEM_FRAG] == 7,
	      "valid fragment changed output");
	CHECK(datapump_linear_fragment_is_ready(
		      &owner, transmit, transmit, 1),
	      "aliased input and output rejected");

	CHECK(!datapump_linear_fragment_is_ready(NULL, receive, transmit, 2) &&
	      transmit[0] == 0 && transmit[1] == 0 && transmit[2] == 7,
	      "missing owner did not mute only the bounded output");
	transmit[0] = transmit[1] = 7;
	CHECK(!datapump_linear_fragment_is_ready(&owner, NULL, transmit, 2) &&
	      transmit[0] == 0 && transmit[1] == 0 && transmit[2] == 7,
	      "missing input did not mute only the bounded output");
	CHECK(!datapump_linear_fragment_is_ready(&owner, receive, NULL, 2),
	      "missing output was accepted");

	transmit[0] = 7;
	CHECK(!datapump_linear_fragment_is_ready(&owner, receive, transmit, 0) &&
	      !datapump_linear_fragment_is_ready(&owner, receive, transmit, -1) &&
	      !datapump_linear_fragment_is_ready(
		      &owner, receive, transmit, MODEM_FRAG + 1) &&
	      transmit[0] == 7 && transmit[MODEM_FRAG] == 7,
	      "unbounded count changed output");
	return true;
}

int main(void)
{
	return test_linear_fragment() ? 0 : 1;
}
