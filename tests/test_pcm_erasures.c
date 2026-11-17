// SPDX-License-Identifier: BSD-3-Clause
/* Reuse the native modem/media fixture; this executable owns only the bounded
 * erasure scenarios. Ordinary integration cases retain their retrain/slip tests. */
#define main complete_media_suite_main
#include "test_fsk_integration.c"
#undef main

#include <v92_erasure.h>
#include <v92_phase4_digital_station.h>
#include <v92_profile.h>

static unsigned bindings, erased_words;

enum v92_profile_result __real_v92_profile_init(struct pcm_call_configuration *configuration,
	enum v90_modem_role role, enum g711_law law, unsigned minimum, unsigned maximum,
	uint8_t upstream_maximum, const struct v90_upstream_policy *conventional);

enum v92_profile_result __wrap_v92_profile_init(struct pcm_call_configuration *configuration,
	enum v90_modem_role role, enum g711_law law, unsigned minimum, unsigned maximum,
	uint8_t upstream_maximum, const struct v90_upstream_policy *conventional)
{
	enum v92_profile_result result = __real_v92_profile_init(configuration, role, law,
		minimum, maximum, upstream_maximum, conventional);
	if (result == V92_PROFILE_OK && role == V90_ANALOG_MODEM) {
		configuration->phase3.v92_analog.upstream_lu_ucode = 60;
		configuration->phase3.v92_analog.upstream_rate_mask = UINT32_C(1) << 9;
	}
	return result;
}

enum v92_phase4_digital_station_result __real_v92_phase4_digital_station_select_upstream(
	struct v92_phase4_digital_station *station,
	const struct v92_cpd_parameters *cpd, const struct v92_upstream_plan *plan);

enum v92_phase4_digital_station_result __wrap_v92_phase4_digital_station_select_upstream(
	struct v92_phase4_digital_station *station,
	const struct v92_cpd_parameters *cpd, const struct v92_upstream_plan *plan)
{
	/* A legal second feed-forward tap selects the general inverse instead
	 * of the optional codec-only model. Actual native training learns it. */
	struct v92_cpd_parameters declared = *cpd;
	struct v92_cpd_filter *filter = &declared.filter[V92_CPD_PREFILTER_FEED_FORWARD];
	if ((size_t)plan->delay + 1 >= ARRAY_LENGTH(filter->coefficient) || filter->count > plan->delay + 1)
		return V92_PHASE4_DIGITAL_STATION_INVALID_PARAMETERS;
	filter->count = plan->delay + 2;
	filter->coefficient[plan->delay + 1] = 1;
	return __real_v92_phase4_digital_station_select_upstream(station, &declared, plan);
}

bool __real_v92_erasure_bind(struct v92_upstream_data_rx *receiver,
	struct v92_erasure *owner, uint64_t call, uint64_t epoch);
bool __wrap_v92_erasure_bind(struct v92_upstream_data_rx *receiver,
	struct v92_erasure *owner, uint64_t call, uint64_t epoch)
{
	bool result = __real_v92_erasure_bind(receiver, owner, call, epoch);
	bindings += result;
	return result;
}

bool __real_modem_put_erasure_bits(struct modem *modem, int bits);
bool __wrap_modem_put_erasure_bits(struct modem *modem, int bits)
{
	bool result = __real_modem_put_erasure_bits(modem, bits);
	erased_words += result;
	return result;
}

/* The fixture adds a one-sample postcursor after codec delivery. */
static bool erased_media_call(enum media_codec codec, unsigned digital_caller, unsigned missing)
{
	struct modem_test_case test = {
		.id = DP_V92, .name = "PCM bounded erasure with native LAPM/V.44",
		.selection_command = "AT+MS=92,0,52000,52000+V92Q=0+V92U=1,36000+DS=3,2\r",
		.caller_tx_rate = digital_caller ? 52000 : 36000,
		.caller_rx_rate = digital_caller ? 36000 : 52000,
		.answerer_tx_rate = digital_caller ? 36000 : 52000,
		.answerer_rx_rate = digital_caller ? 52000 : 36000,
		.training_frame_limit = 2500, .requires_v8 = true, .requires_exact_g711 = true,
		.require_lapm = true, .require_v44 = true, .digital_caller = digital_caller,
		.pcm_erasure_samples = missing, .cleardown_initiators = 1,
	};
	unsigned prior_bindings = bindings, prior_erased = erased_words;
	CHECK(run_media_call(&test, codec) && bindings > prior_bindings && erased_words > prior_erased,
	      "native erasure path was not exercised");
	return true;
}

int main(void)
{
	const unsigned missing[] = { 80, 160, 320 };
	for (enum media_codec codec = MEDIA_CODEC_PCMU; codec <= MEDIA_CODEC_PCMA; ++codec)
	for (unsigned digital_caller = 0; digital_caller < 2; ++digital_caller)
	for (size_t loss = 0; loss < ARRAY_LENGTH(missing); ++loss) {
		fprintf(stderr, "[ RUN      ] native PCM erasure law=%u digital-caller=%u missing=%u\n",
			codec, digital_caller, missing[loss]);
		if (!erased_media_call(codec, digital_caller, missing[loss]))
			return 1;
		fprintf(stderr, "[       OK ] native PCM erasure law=%u digital-caller=%u missing=%u\n",
			codec, digital_caller, missing[loss]);
	}
	return 0;
}
