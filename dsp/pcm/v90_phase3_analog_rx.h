// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE3_ANALOG_RX_H
#define V90_PHASE3_ANALOG_RX_H

#include "qam_gain.h"
#include "v34_precoder_trainer.h"
#include "v34_phase3_pp_receiver.h"
#include "v34_phase3_s_receiver.h"
#include "v90_ja_search.h"

#include <stdbool.h>
#include <stdint.h>

enum v90_phase3_analog_rx_stage {
	V90_PHASE3_ANALOG_RX_STAGE_S,
	V90_PHASE3_ANALOG_RX_STAGE_PP,
	V90_PHASE3_ANALOG_RX_STAGE_JA,
	V90_PHASE3_ANALOG_RX_STAGE_COMPLETE,
};

enum v90_phase3_analog_rx_result {
	V90_PHASE3_ANALOG_RX_OK,
	V90_PHASE3_ANALOG_RX_S_DETECTED,
	V90_PHASE3_ANALOG_RX_PP_COMPLETE,
	V90_PHASE3_ANALOG_RX_JA_REJECTED,
	V90_PHASE3_ANALOG_RX_JA_DETECTED,
	V90_PHASE3_ANALOG_RX_COMPLETE,
	V90_PHASE3_ANALOG_RX_INVALID_ARGUMENT,
	V90_PHASE3_ANALOG_RX_INVALID_STATE,
	V90_PHASE3_ANALOG_RX_OUT_OF_RANGE,
	V90_PHASE3_ANALOG_RX_NUMERIC_FAILURE,
};

struct v90_phase3_analog_rx_configuration {
	double s_gain_adaptation;
	double pp_equalizer_adaptation;
	double tracking_equalizer_adaptation;
};

struct v90_phase3_ja_point_receiver {
	struct qam_equalizer equalizer;
	struct v34_precoder_trainer precoder;
	struct v90_ja_search search;
	double adaptation;
	uint32_t decisions;
};

struct v90_phase3_s_point_receiver {
	struct qam_gain gain;
	struct v34_phase3_s_receiver detector;
	bool gain_seeded;
};

union v90_phase3_analog_rx_state {
	struct v90_phase3_s_point_receiver s;
	struct v34_phase3_pp_receiver pp;
	struct v90_phase3_ja_point_receiver ja;
};

/* Receive the analogue modem's zero-MD S/S-bar, PP, TRN, and repeated Ja
 * point stream. The V.90 prefix owns gain and equalizer acquisition; Ja
 * search begins only after the required 512 equalized TRN decisions. */
struct v90_phase3_analog_rx {
	union v90_phase3_analog_rx_state state;
	struct v90_phase3_analog_rx_configuration configuration;
	struct v34_phase3_s_detection s_detection;
	enum v90_phase3_analog_rx_stage stage;
};

struct v90_phase3_analog_handoff {
	struct v34_phase3_s_detection s;
	struct v34_equalizer_handoff equalizer;
	struct v34_precoder_recommendation precoder;
	struct v90_dil_parameters dil;
};

bool v90_phase3_analog_rx_configuration_is_valid(
	const struct v90_phase3_analog_rx_configuration *configuration);
enum v90_phase3_analog_rx_result v90_phase3_analog_rx_init(
	struct v90_phase3_analog_rx *rx,
	const struct v90_phase3_analog_rx_configuration *configuration);
/* Begin from a qualified suffix when an upstream line receiver spends the
 * start of finite S acquiring symbol timing. */
enum v90_phase3_analog_rx_result v90_phase3_analog_rx_init_suffix(
	struct v90_phase3_analog_rx *rx,
	const struct v90_phase3_analog_rx_configuration *configuration,
	uint8_t s_symbols);
bool v90_phase3_analog_rx_is_valid(
	const struct v90_phase3_analog_rx *rx);
bool v90_phase3_analog_rx_is_complete(
	const struct v90_phase3_analog_rx *rx);

/* Accept one finite, carrier- and timing-recovered point. All errors and
 * complete calls leave the receiver unchanged. */
enum v90_phase3_analog_rx_result v90_phase3_analog_rx_point(
	struct v90_phase3_analog_rx *rx,
	const struct qam_point *received);

bool v90_phase3_analog_handoff_is_valid(
	const struct v90_phase3_analog_handoff *handoff);
enum v90_phase3_analog_rx_result v90_phase3_analog_rx_handoff(
	const struct v90_phase3_analog_rx *rx,
	struct v90_phase3_analog_handoff *handoff);

const char *v90_phase3_analog_rx_result_name(
	enum v90_phase3_analog_rx_result result);

#endif /* V90_PHASE3_ANALOG_RX_H */
