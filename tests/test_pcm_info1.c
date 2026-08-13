// SPDX-License-Identifier: BSD-3-Clause

#include <pcm_info1.h>

#include "test.h"

#include <string.h>

static struct v34_info1c v90_digital(void)
{
	return (struct v34_info1c){
		.common = {
			.minimum_peer_power_reduction_db = 2,
			.additional_peer_power_reduction_db = 1,
			.local_md_length_35_ms_units = 3,
			.frequency_offset_1_50_hz_units = -4,
		},
		.answer_transmit = {
			[V34_SYMBOL_RATE_2400] = { false, 1, 4 },
			[V34_SYMBOL_RATE_2743] = { false, 2, 5 },
			[V34_SYMBOL_RATE_2800] = { true, 3, 6 },
			[V34_SYMBOL_RATE_3000] = { false, 4, 7 },
			[V34_SYMBOL_RATE_3200] = { true, 5, 8 },
			[V34_SYMBOL_RATE_3429] = { false, 6, 9 },
		},
	};
}

static struct v90_info1a conventional(void)
{
	return (struct v90_info1a){
		.local_md_length_35_ms_units = 5,
		.training_ucode = 78,
		.upstream_symbol_rate = V34_SYMBOL_RATE_3200,
		.frequency_offset_1_50_hz_units = 9,
	};
}

static struct v92_pcm_info1a pcm(void)
{
	return (struct v92_pcm_info1a){
		.filter_sections = V92_PCM_FILTER_P1_Z2,
		.maximum_total_coefficients = 256,
		.maximum_section_coefficients = 128,
		.local_md_length_276_symbol_units = 1,
		.training_ucode = 78,
	};
}

static bool test_constructors(void)
{
	struct v34_info1c v90 = v90_digital();
	struct v92_info1d v92 = {
		.v34 = v90,
		.pcm_upstream_supported = true,
	};
	struct pcm_info1d digital_v90;
	struct pcm_info1d digital_v92;
	struct pcm_info1d unchanged;
	uint8_t saved[sizeof(unchanged)];

	CHECK(pcm_info1d_init_v90(&digital_v90, &v90) == PCM_INFO1_OK &&
	      digital_v90.protocol == PCM_PROTOCOL_V90 &&
	      pcm_info1d_is_valid(&digital_v90) &&
	      pcm_info1d_init_v92(&digital_v92, &v92) == PCM_INFO1_OK &&
	      digital_v92.protocol == PCM_PROTOCOL_V92 &&
	      digital_v92.record.v92.pcm_upstream_supported &&
	      pcm_info1d_is_valid(&digital_v92),
	      "protocol-tagged INFO1d construction failed");
	memset(&unchanged, 0xa5, sizeof(unchanged));
	memcpy(saved, &unchanged, sizeof(saved));
	v90.answer_transmit[V34_SYMBOL_RATE_3000]
		.preemphasis_filter_index = 11;
	CHECK(pcm_info1d_init_v90(&unchanged, &v90) ==
		      PCM_INFO1_INVALID_ARGUMENT &&
	      pcm_info1d_init_v90(NULL, &digital_v90.record.v90) ==
		      PCM_INFO1_INVALID_ARGUMENT &&
	      pcm_info1d_init_v92(&unchanged, NULL) ==
		      PCM_INFO1_INVALID_ARGUMENT &&
	      memcmp(&unchanged, saved, sizeof(saved)) == 0 &&
	      !pcm_info1d_is_valid(NULL),
	      "invalid INFO1d construction changed output");
	return true;
}

static bool run_selection(
	const struct pcm_startup_selection *startup,
	const struct pcm_info1d *digital, const uint8_t *payload,
	enum pcm_info1_result expected,
	enum pcm_upstream_mode expected_upstream)
{
	struct pcm_info1_selection selected;
	uint8_t saved[sizeof(selected)];

	memset(&selected, 0xa5, sizeof(selected));
	memcpy(saved, &selected, sizeof(saved));
	CHECK(pcm_info1_select(
		      startup, digital, payload, V90_INFO1A_PAYLOAD_BYTES,
		      V90_INFO1A_PAYLOAD_BITS, &selected) == expected,
	      "INFO1 selection returned the wrong result");
	if (expected != PCM_INFO1_OK)
		return memcmp(&selected, saved, sizeof(saved)) == 0;
	return pcm_info1_selection_is_valid(&selected) &&
	       selected.startup.protocol == startup->protocol &&
	       selected.startup.phase2 == startup->phase2 &&
	       selected.digital.protocol == digital->protocol &&
	       selected.upstream == expected_upstream &&
	       (selected.upstream == PCM_UPSTREAM_V34 ?
			selected.analog.conventional.training_ucode == 78 &&
			selected.analog.conventional.upstream_symbol_rate ==
				V34_SYMBOL_RATE_3200 &&
			selected.analog.conventional
				.frequency_offset_1_50_hz_units == 9 :
			selected.analog.pcm.maximum_total_coefficients == 256 &&
			selected.analog.pcm.maximum_section_coefficients == 128 &&
			selected.analog.pcm.training_ucode == 78);
}

static bool test_selection_matrix(void)
{
	struct v34_info1c base = v90_digital();
	struct v92_info1d unsupported = { .v34 = base };
	struct v92_info1d supported = {
		.v34 = base,
		.pcm_upstream_supported = true,
	};
	struct pcm_info1d v90;
	struct pcm_info1d v92_without_pcm;
	struct pcm_info1d v92_with_pcm;
	struct v90_info1a conventional_record = conventional();
	struct v92_pcm_info1a pcm_record = pcm();
	uint8_t conventional_payload[V90_INFO1A_PAYLOAD_BYTES];
	uint8_t pcm_payload[V92_PCM_INFO1A_PAYLOAD_BYTES];
	struct pcm_startup_selection v90_startup = {
		.protocol = PCM_PROTOCOL_V90,
		.phase2 = PCM_PHASE2_FULL,
	};

	CHECK(pcm_info1d_init_v90(&v90, &base) == PCM_INFO1_OK &&
	      pcm_info1d_init_v92(&v92_without_pcm, &unsupported) ==
		      PCM_INFO1_OK &&
	      pcm_info1d_init_v92(&v92_with_pcm, &supported) ==
		      PCM_INFO1_OK &&
	      v90_info1a_encode(
		      &conventional_record, conventional_payload,
		      sizeof(conventional_payload)) == V90_INFO1_OK &&
	      v92_pcm_info1a_encode(
		      &pcm_record, pcm_payload, sizeof(pcm_payload)) ==
		      V92_PCM_INFO1A_OK,
	      "INFO1 selection fixtures did not initialize");
	CHECK(run_selection(
		      &v90_startup, &v90, conventional_payload, PCM_INFO1_OK,
		      PCM_UPSTREAM_V34) &&
	      run_selection(
		      &v90_startup, &v90, pcm_payload,
		      PCM_INFO1_PCM_UNAVAILABLE, PCM_UPSTREAM_PCM),
	      "V.90 upstream selection changed");
	for (unsigned int short_phase2 = 0; short_phase2 <= 1;
	     ++short_phase2) {
		struct pcm_startup_selection startup = {
			.protocol = PCM_PROTOCOL_V92,
			.phase2 = short_phase2 ? PCM_PHASE2_SHORT :
						 PCM_PHASE2_FULL,
		};

		CHECK(run_selection(
			      &startup, &v92_without_pcm,
			      conventional_payload, PCM_INFO1_OK,
			      PCM_UPSTREAM_V34) &&
		      run_selection(
			      &startup, &v92_without_pcm, pcm_payload,
			      PCM_INFO1_PCM_UNAVAILABLE, PCM_UPSTREAM_PCM) &&
		      run_selection(
			      &startup, &v92_with_pcm,
			      conventional_payload, PCM_INFO1_OK,
			      PCM_UPSTREAM_V34) &&
		      run_selection(
			      &startup, &v92_with_pcm, pcm_payload,
			      PCM_INFO1_OK, PCM_UPSTREAM_PCM),
		      "V.92 full/short selection %u changed", short_phase2);
	}
	return true;
}

static bool test_validation(void)
{
	struct v34_info1c base = v90_digital();
	struct v92_info1d v92_record = {
		.v34 = base,
		.pcm_upstream_supported = true,
	};
	struct pcm_info1d v90;
	struct pcm_info1d v92;
	struct pcm_startup_selection startup = {
		.protocol = PCM_PROTOCOL_V90,
		.phase2 = PCM_PHASE2_FULL,
	};
	struct pcm_startup_selection invalid_startup = startup;
	struct pcm_info1_selection selected;
	uint8_t malformed[V90_INFO1A_PAYLOAD_BYTES] = { 0 };
	uint8_t saved[sizeof(selected)];

	CHECK(pcm_info1d_init_v90(&v90, &base) == PCM_INFO1_OK &&
	      pcm_info1d_init_v92(&v92, &v92_record) == PCM_INFO1_OK,
	      "validation fixtures did not initialize");
	memset(&selected, 0xa5, sizeof(selected));
	memcpy(saved, &selected, sizeof(saved));
	invalid_startup.phase2 = PCM_PHASE2_SHORT;
	CHECK(pcm_info1_select(
		      &startup, &v92, malformed, sizeof(malformed),
		      V90_INFO1A_PAYLOAD_BITS, &selected) ==
		      PCM_INFO1_PROTOCOL_MISMATCH &&
	      pcm_info1_select(
		      &invalid_startup, &v90, malformed, sizeof(malformed),
		      V90_INFO1A_PAYLOAD_BITS, &selected) ==
		      PCM_INFO1_INVALID_RECORD &&
	      pcm_info1_select(
		      &startup, &v90, malformed, sizeof(malformed),
		      V90_INFO1A_PAYLOAD_BITS, &selected) ==
		      PCM_INFO1_INVALID_PAYLOAD &&
	      pcm_info1_select(
		      &startup, &v90, malformed, sizeof(malformed),
		      V90_INFO1A_PAYLOAD_BITS - 1u, &selected) ==
		      PCM_INFO1_INVALID_PAYLOAD &&
	      pcm_info1_select(
		      NULL, &v90, malformed, sizeof(malformed),
		      V90_INFO1A_PAYLOAD_BITS, &selected) ==
		      PCM_INFO1_INVALID_ARGUMENT &&
	      pcm_info1_select(
		      &startup, NULL, malformed, sizeof(malformed),
		      V90_INFO1A_PAYLOAD_BITS, &selected) ==
		      PCM_INFO1_INVALID_ARGUMENT &&
	      pcm_info1_select(
		      &startup, &v90, NULL, sizeof(malformed),
		      V90_INFO1A_PAYLOAD_BITS, &selected) ==
		      PCM_INFO1_INVALID_ARGUMENT &&
	      pcm_info1_select(
		      &startup, &v90, malformed, sizeof(malformed),
		      V90_INFO1A_PAYLOAD_BITS, NULL) ==
		      PCM_INFO1_INVALID_ARGUMENT &&
	      pcm_info1_select(
		      &startup, &v90, malformed, sizeof(malformed) - 1u,
		      V90_INFO1A_PAYLOAD_BITS, &selected) ==
		      PCM_INFO1_INVALID_ARGUMENT &&
	      memcmp(&selected, saved, sizeof(saved)) == 0,
	      "invalid INFO1 selection changed output or result");
	CHECK(!pcm_info1_selection_is_valid(NULL) &&
	      strcmp(pcm_info1_result_name(PCM_INFO1_PCM_UNAVAILABLE),
		     "PCM upstream unavailable") == 0 &&
	      strcmp(pcm_info1_result_name((enum pcm_info1_result)6),
		     "unknown") == 0,
	      "INFO1 validation or result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "constructors", test_constructors },
		{ "selection matrix", test_selection_matrix },
		{ "validation", test_validation },
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
