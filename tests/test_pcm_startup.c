// SPDX-License-Identifier: BSD-3-Clause

#include <pcm_startup.h>

#include "test.h"

#include <string.h>

static struct v90_info0 common(void)
{
	return (struct v90_info0){
		.symbol_rate_2743_supported = true,
		.symbol_rate_2800_supported = true,
		.symbol_rate_3429_supported = true,
		.symbol_rate_3429_transmit_permitted = true,
		.maximum_symbol_rate_difference_steps = 2,
	};
}

static struct v92_info0a analog(bool capable, bool short_requested)
{
	return (struct v92_info0a){
		.v90 = common(),
		.v92_capable = capable,
		.short_phase2_requested = short_requested,
	};
}

static struct v92_info0d digital(bool capable, bool short_requested)
{
	return (struct v92_info0d){
		.v90 = {
			.common = common(),
			.nominal_transmit_power = 6,
			.maximum_transmit_power = 16,
			.pcm_law = V90_PCM_MU_LAW,
		},
		.v92_capable = capable,
		.short_phase2_requested = short_requested,
	};
}

static bool test_selection_matrix(void)
{
	for (unsigned int analog_capable = 0; analog_capable <= 1;
	     ++analog_capable) {
		for (unsigned int digital_capable = 0; digital_capable <= 1;
		     ++digital_capable) {
			for (unsigned int analog_short = 0;
			     analog_short <= analog_capable; ++analog_short) {
				for (unsigned int digital_short = 0;
				     digital_short <= digital_capable;
				     ++digital_short) {
					struct v92_info0a a = analog(
						analog_capable != 0,
						analog_short != 0);
					struct v92_info0d d = digital(
						digital_capable != 0,
						digital_short != 0);
					struct pcm_startup_selection selection;
					bool v92 = analog_capable && digital_capable;
					bool short_phase2 = v92 && analog_short &&
						digital_short;

					CHECK(pcm_startup_select(&a, &d, &selection) ==
						      PCM_STARTUP_OK &&
					      pcm_startup_selection_is_valid(
						      &selection) &&
					      selection.protocol ==
						      (v92 ? PCM_PROTOCOL_V92 :
							     PCM_PROTOCOL_V90) &&
					      selection.phase2 ==
						      (short_phase2 ? PCM_PHASE2_SHORT :
							       PCM_PHASE2_FULL),
					      "startup selection %u/%u/%u/%u changed",
					      analog_capable, digital_capable,
					      analog_short, digital_short);
				}
			}
		}
	}
	return true;
}

static bool test_validation(void)
{
	struct v92_info0a a = analog(true, true);
	struct v92_info0d d = digital(true, true);
	struct pcm_startup_selection selection = {
		.protocol = PCM_PROTOCOL_V90,
		.phase2 = PCM_PHASE2_FULL,
	};
	struct pcm_startup_selection saved = selection;

	CHECK(pcm_startup_select(NULL, &d, &selection) ==
		      PCM_STARTUP_INVALID_ARGUMENT &&
	      pcm_startup_select(&a, NULL, &selection) ==
		      PCM_STARTUP_INVALID_ARGUMENT &&
	      pcm_startup_select(&a, &d, NULL) ==
		      PCM_STARTUP_INVALID_ARGUMENT &&
	      memcmp(&selection, &saved, sizeof(selection)) == 0,
	      "invalid startup arguments changed the selection");
	a.v92_capable = false;
	CHECK(pcm_startup_select(&a, &d, &selection) ==
		      PCM_STARTUP_INVALID_CAPABILITIES &&
	      memcmp(&selection, &saved, sizeof(selection)) == 0,
	      "invalid startup capabilities changed the selection");
	selection.phase2 = PCM_PHASE2_SHORT;
	CHECK(!pcm_startup_selection_is_valid(&selection),
	      "V.90 accepted short Phase 2");
	selection.protocol = PCM_PROTOCOL_V92;
	CHECK(pcm_startup_selection_is_valid(&selection) &&
	      !pcm_startup_selection_is_valid(NULL) &&
	      strcmp(pcm_startup_result_name(
		     PCM_STARTUP_INVALID_CAPABILITIES),
		     "invalid capabilities") == 0 &&
	      strcmp(pcm_startup_result_name((enum pcm_startup_result)3),
		     "unknown") == 0,
	      "startup selection validation or result names changed");
	return true;
}

static bool test_identity_validation(void)
{
	for (int value = -2; value <= 3; ++value) {
		bool expected = value == 0 || value == 1;

		CHECK(pcm_protocol_is_valid((enum pcm_protocol)value) == expected,
		      "protocol validity changed for %d", value);
		CHECK(pcm_modem_role_is_valid((enum v90_modem_role)value) ==
			      expected,
		      "modem-role validity changed for %d", value);
	}
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "selection matrix", test_selection_matrix },
		{ "validation", test_validation },
		{ "identity validation", test_identity_validation },
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
