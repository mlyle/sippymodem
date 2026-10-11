// SPDX-License-Identifier: BSD-3-Clause

#include "datapump.h"
#include "datapump_transmit.h"

#include "b1_path.h"
#include "conditioning_path.h"
#include "datapump_link.h"
#include "datapump_media.h"
#include "datapump_timing.h"
#include "data_path.h"
#include "echo_training.h"
#include "end_path.h"
#include "modem.h"
#include "qam.h"
#include "rate_path.h"
#include "carrier.h"
#include "conditioning.h"
#include "constellation.h"
#include "framing.h"
#include "line_startup.h"
#include "startup.h"
#include "training_session.h"
#include "trellis.h"
#include "v8_datapump.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Each endpoint owns one continuous transmit-reference and fixed receive lane
 * across line turnaround, receiver training, and online data. */
enum {
	V32_TRANSMIT_AMPLITUDE = 12000,
	V32_CARRIER_LOSS_SAMPLES = MODEM_RATE / 5,
};

enum v32_fixed_stage {
	V32_FIXED_OFF,
	V32_FIXED_CALLER_CONDITIONING,
	V32_FIXED_CALLER_RATE_OFFER,
	V32_FIXED_CALLER_RATE_DRAIN,
	V32_FIXED_CALLER_ECHO,
	V32_FIXED_CALLER_SECOND_CONDITIONING,
	V32_FIXED_CALLER_SELECTION,
	V32_FIXED_CALLER_END,
	V32_FIXED_ANSWERER_INITIAL_ECHO,
	V32_FIXED_ANSWERER_CONDITIONING,
	V32_FIXED_ANSWERER_RATE,
	V32_FIXED_ANSWERER_SECOND_CONDITIONING,
	V32_FIXED_ANSWERER_END,
	V32_FIXED_ANSWERER_CLEARDOWN_CONDITIONING,
	V32_FIXED_ANSWERER_CLEARDOWN,
	V32_FIXED_B1,
	V32_FIXED_DATA,
};

/* Keep clocking the physical lane between the completed R1 observation and
 * the next local transmit-symbol epoch.  The rate detector is already done;
 * only differential framing and equalizer decisions remain live. */
struct v32_fixed_rate_drain {
	struct v32_receive_path receive;
	struct v32_uncoded_receiver four_point;
	uint64_t observation_epoch;
};

union v32_fixed_owner {
	struct v32_receive_path receive;
	struct v32_conditioning_path conditioning;
	struct v32_rate_path rate;
	struct v32_fixed_rate_drain drain;
	struct v32_end_path end;
	struct v32_b1_path b1;
	struct v32_data_path data;
};

struct v32_fixed_path {
	union v32_fixed_owner owner;
	union {
		struct v32_echo_training echo;
		struct v32_conditioning_transmitter conditioning;
	} training;
	enum v32_fixed_stage stage;
};

struct v32_datapump {
	struct dp dp;
	struct qam_modulator modulator;
	struct v32_symbol_encoder encoder;
	struct v32_trellis_encoder trellis_encoder;
	struct v32_fixed_path fixed_path;
	struct v32_line_startup line_startup;
	struct v32_answerer_line_startup answerer_line_startup;
	struct v32_startup startup;
	struct qam_point transmit_point;
	struct training_session training;
	uint32_t missing_carrier_samples;
	uint16_t caller_s_extension_symbols;
	uint8_t cleardown_symbols;
	bool trellis_encoder_initialized;
	bool answerer_s_detected;
	bool caller;
};

static struct dp *create_v32(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, const struct dp_operations *operations);
static int delete_v32(struct dp *dp);
static int retrain_v32(struct dp *dp);
static int process_v32(
	struct dp *dp, void *receive, void *transmit, int count);
static int enter_link(struct v32_datapump *datapump);
static const char *progress_v32(const struct dp *dp);

static int tx_tail_samples(const struct dp *dp);
static const struct dp_operations v32_operations = {
	.name = "V.32",
	.create = create_v32,
	.delete = delete_v32,
	.process = process_v32,
	.retrain = retrain_v32,
	.progress = progress_v32,
	.tx_tail_samples = tx_tail_samples,
};

static const char *progress_v32(const struct dp *dp)
{
	const struct v32_datapump *datapump = dp->dp_data;
	if (datapump == NULL)
		return "V.32/initializing";
	switch (datapump->fixed_path.stage) {
	case V32_FIXED_OFF: return "V.32/answer-tone-carrier-turnaround";
	case V32_FIXED_CALLER_CONDITIONING:
	case V32_FIXED_ANSWERER_CONDITIONING: return "V.32/S-TRN";
	case V32_FIXED_CALLER_RATE_OFFER:
	case V32_FIXED_CALLER_RATE_DRAIN:
	case V32_FIXED_ANSWERER_RATE: return "V.32/R1-rate-offer";
	case V32_FIXED_CALLER_ECHO:
	case V32_FIXED_ANSWERER_INITIAL_ECHO: return "V.32/echo-training";
	case V32_FIXED_CALLER_SECOND_CONDITIONING:
	case V32_FIXED_ANSWERER_SECOND_CONDITIONING: return "V.32/second-S-TRN";
	case V32_FIXED_CALLER_SELECTION: return "V.32/R3-rate-selection";
	case V32_FIXED_CALLER_END:
	case V32_FIXED_ANSWERER_END: return "V.32/E";
	case V32_FIXED_ANSWERER_CLEARDOWN_CONDITIONING:
	case V32_FIXED_ANSWERER_CLEARDOWN: return "V.32/cleardown";
	case V32_FIXED_B1: return "V.32/B1";
	case V32_FIXED_DATA: return "data/V.32";
	}
	return "V.32/unknown";
}

static enum v32_transmitter_role local_role(bool caller)
{
	return caller ? V32_CALLING_TRANSMITTER :
			V32_ANSWERING_TRANSMITTER;
}

static enum v32_endpoint_role endpoint_role(bool caller)
{
	return caller ? V32_CALLER : V32_ANSWERER;
}

static bool fixed_path_is_active(
	const struct v32_datapump *datapump)
{
	return datapump->fixed_path.stage != V32_FIXED_OFF;
}

static bool fixed_path_is_clearing(const struct v32_datapump *datapump)
{
	return datapump->fixed_path.stage ==
		       V32_FIXED_ANSWERER_CLEARDOWN_CONDITIONING ||
	       datapump->fixed_path.stage == V32_FIXED_ANSWERER_CLEARDOWN;
}

static const struct v32_receive_path *fixed_path_receive_const(
	const struct v32_fixed_path *path)
{
	switch (path->stage) {
	case V32_FIXED_CALLER_CONDITIONING:
	case V32_FIXED_CALLER_SECOND_CONDITIONING:
		return &path->owner.conditioning.receive;
	case V32_FIXED_CALLER_RATE_OFFER:
	case V32_FIXED_CALLER_SELECTION:
		return &path->owner.rate.receive;
	case V32_FIXED_CALLER_RATE_DRAIN:
	case V32_FIXED_CALLER_ECHO:
		return &path->owner.drain.receive;
	case V32_FIXED_CALLER_END:
		return &path->owner.end.receive;
	case V32_FIXED_B1:
		return &path->owner.b1.receive;
	case V32_FIXED_DATA:
		return &path->owner.data.b1.receive;
	case V32_FIXED_ANSWERER_INITIAL_ECHO:
		return &path->owner.receive;
	case V32_FIXED_ANSWERER_CONDITIONING:
		return &path->owner.conditioning.receive;
	case V32_FIXED_ANSWERER_RATE:
		return &path->owner.rate.receive;
	case V32_FIXED_ANSWERER_SECOND_CONDITIONING:
	case V32_FIXED_ANSWERER_END:
		return &path->owner.end.receive;
	case V32_FIXED_OFF:
	case V32_FIXED_ANSWERER_CLEARDOWN_CONDITIONING:
	case V32_FIXED_ANSWERER_CLEARDOWN:
		break;
	}
	return NULL;
}

static bool fixed_rate_drain_is_valid(
	const struct v32_fixed_rate_drain *drain)
{
	return drain != NULL &&
	       v32_receive_path_is_valid(&drain->receive) &&
	       v32_uncoded_receiver_is_valid(&drain->four_point) &&
	       !drain->receive.equalizer.decision_pending &&
	       drain->observation_epoch == drain->receive.equalizer.epoch;
}

static bool fixed_path_is_valid(
	const struct v32_datapump *datapump)
{
	const struct v32_fixed_path *path = &datapump->fixed_path;
	const struct v32_receive_path *receive;
	bool valid;

	if (path->stage <= V32_FIXED_OFF ||
	    path->stage > V32_FIXED_DATA ||
	    (!datapump->caller &&
	     path->stage >= V32_FIXED_CALLER_CONDITIONING &&
	     path->stage <= V32_FIXED_CALLER_END) ||
	    (datapump->caller &&
	     path->stage >= V32_FIXED_ANSWERER_INITIAL_ECHO &&
	     path->stage <= V32_FIXED_ANSWERER_CLEARDOWN))
		return false;
	switch (path->stage) {
	case V32_FIXED_CALLER_CONDITIONING:
	case V32_FIXED_CALLER_SECOND_CONDITIONING:
		valid = v32_conditioning_path_is_valid(
				&path->owner.conditioning) &&
			!v32_conditioning_path_is_complete(
				&path->owner.conditioning);
		break;
	case V32_FIXED_CALLER_RATE_OFFER:
	case V32_FIXED_CALLER_SELECTION:
		valid = v32_rate_path_is_valid(&path->owner.rate) &&
			!v32_rate_path_is_complete(&path->owner.rate);
		break;
	case V32_FIXED_CALLER_RATE_DRAIN:
	case V32_FIXED_CALLER_ECHO:
		valid = fixed_rate_drain_is_valid(&path->owner.drain);
		break;
	case V32_FIXED_CALLER_END:
		valid = v32_end_path_is_valid(&path->owner.end) &&
			!v32_end_path_is_complete(&path->owner.end);
		break;
	case V32_FIXED_B1:
		valid = v32_b1_path_is_valid(&path->owner.b1) &&
			!v32_b1_path_is_complete(&path->owner.b1);
		break;
	case V32_FIXED_DATA:
		valid = v32_data_path_is_valid(&path->owner.data);
		break;
	case V32_FIXED_ANSWERER_INITIAL_ECHO:
		valid = !datapump->caller &&
			datapump->startup.phase == V32_ANSWERER_TRAIN &&
			v32_receive_path_is_valid(&path->owner.receive) &&
			v32_echo_training_path_is_valid(
				&path->training.echo,
				&path->owner.receive) &&
			!v32_echo_training_is_complete(
				&path->training.echo);
		break;
	case V32_FIXED_ANSWERER_CONDITIONING:
		valid = !datapump->caller &&
			datapump->startup.phase == V32_ANSWERER_SEND_OFFER &&
			v32_conditioning_path_is_valid(
				&path->owner.conditioning) &&
			!v32_conditioning_path_is_complete(
				&path->owner.conditioning);
		break;
	case V32_FIXED_ANSWERER_RATE:
		valid = !datapump->caller &&
			datapump->startup.phase == V32_ANSWERER_SEND_OFFER &&
			v32_rate_path_is_valid(&path->owner.rate) &&
			!v32_rate_path_is_complete(&path->owner.rate);
		break;
	case V32_FIXED_ANSWERER_SECOND_CONDITIONING:
		valid = !datapump->caller &&
			datapump->startup.phase == V32_ANSWERER_RETRAIN &&
			v32_end_path_is_valid(&path->owner.end) &&
			!v32_end_path_is_complete(&path->owner.end) &&
			v32_conditioning_transmitter_is_valid(
				&path->training.conditioning);
		break;
	case V32_FIXED_ANSWERER_END:
		valid = !datapump->caller &&
			(datapump->startup.phase ==
				 V32_ANSWERER_SEND_SELECTION ||
			 datapump->startup.phase == V32_ANSWERER_SEND_END ||
			 datapump->startup.phase == V32_CONFIRM) &&
			v32_end_path_is_valid(&path->owner.end) &&
			!v32_end_path_is_complete(&path->owner.end);
		break;
	case V32_FIXED_ANSWERER_CLEARDOWN_CONDITIONING:
		return datapump->cleardown_symbols == 0 &&
		       v32_conditioning_transmitter_is_valid(
			       &path->training.conditioning);
	case V32_FIXED_ANSWERER_CLEARDOWN:
		return datapump->cleardown_symbols <= V32_CLEARDOWN_SYMBOLS +
			       datapump->modulator.channel.pulse_span_symbols + 1u &&
		       v32_conditioning_transmitter_is_complete(
			       &path->training.conditioning);
	case V32_FIXED_OFF:
		return false;
	}
	if (!valid)
		return false;
	if (path->stage == V32_FIXED_CALLER_CONDITIONING ||
	    path->stage == V32_FIXED_CALLER_RATE_OFFER)
		return datapump->caller_s_extension_symbols != 0;
	if (datapump->caller_s_extension_symbols != 0)
		return false;
	receive = fixed_path_receive_const(path);
	if (datapump->caller &&
	    (path->stage == V32_FIXED_CALLER_RATE_DRAIN ||
	     path->stage == V32_FIXED_CALLER_ECHO))
		return v32_echo_training_path_is_valid(
			&path->training.echo, receive) &&
			(path->stage == V32_FIXED_CALLER_ECHO) ==
				receive->echo_training;
	if (!datapump->caller &&
	    path->stage == V32_FIXED_ANSWERER_INITIAL_ECHO)
		return v32_echo_training_path_is_valid(
			&path->training.echo, receive);
	if (datapump->caller)
		return v32_echo_training_is_complete(
			       &path->training.echo) &&
		       v32_echo_training_path_is_valid(
			       &path->training.echo, receive);
	if (path->stage == V32_FIXED_ANSWERER_CONDITIONING ||
	    path->stage == V32_FIXED_ANSWERER_RATE)
		return v32_echo_training_is_complete(
			       &path->training.echo) &&
		       v32_echo_training_path_is_valid(
			       &path->training.echo, receive);
	if (!v32_conditioning_transmitter_is_valid(
		    &path->training.conditioning))
		return false;
	return (path->stage == V32_FIXED_ANSWERER_SECOND_CONDITIONING ||
		v32_conditioning_transmitter_is_complete(
			&path->training.conditioning)) &&
	       !receive->echo_training && !receive->echo.echo.adapting;
}

static bool line_state_is_valid(
	const struct v32_datapump *datapump)
{
	if (!v32_line_startup_is_valid(&datapump->line_startup) ||
	    !v32_answerer_line_startup_is_valid(
		    &datapump->answerer_line_startup) ||
	    (datapump->answerer_s_detected &&
	     (datapump->caller ||
	      datapump->startup.phase != V32_ANSWERER_SEND_OFFER)))
		return false;
	if (fixed_path_is_active(datapump)) {
		if (!datapump->caller)
			return v32_answerer_line_startup_is_complete(
				       &datapump->answerer_line_startup) &&
			       datapump->line_startup.state ==
				       V32_LINE_WAIT_ANSWER_TONE &&
			       datapump->caller_s_extension_symbols == 0 &&
			       fixed_path_is_valid(datapump);
		return v32_line_startup_is_complete(
			       &datapump->line_startup) &&
		       fixed_path_is_valid(datapump);
	}
	if (datapump->caller_s_extension_symbols != 0 ||
	    datapump->answerer_s_detected)
		return false;
	if (datapump->caller)
		return !v32_line_startup_is_complete(
			       &datapump->line_startup) &&
		       datapump->line_startup.state !=
			       V32_LINE_STARTUP_FAILED;
	return !v32_answerer_line_startup_is_complete(
		       &datapump->answerer_line_startup) &&
	       datapump->answerer_line_startup.state !=
		       V32_ANSWERER_LINE_FAILED;
}

static uint8_t rate_capabilities(const struct modem *modem, enum DP_ID id)
{
	uint8_t rates = 0;

	if (modem == NULL)
		return 0;
	if (modem->min_rate <= V32_4800_BIT_RATE &&
	    modem->max_rate >= V32_4800_BIT_RATE)
		rates |= V32_RATE_CAPABILITY_4800;
	if (modem->min_rate <= V32_7200_BIT_RATE &&
	    modem->max_rate >= V32_7200_BIT_RATE)
		rates |= V32_RATE_CAPABILITY_7200;
	if (modem->min_rate <= V32_9600_BIT_RATE &&
	    modem->max_rate >= V32_9600_BIT_RATE)
		rates |= V32_RATE_CAPABILITY_9600;
	if (modem->min_rate <= V32_12000_BIT_RATE &&
	    modem->max_rate >= V32_12000_BIT_RATE)
		rates |= V32_RATE_CAPABILITY_12000;
	if (modem->min_rate <= V32_14400_BIT_RATE &&
	    modem->max_rate >= V32_14400_BIT_RATE)
		rates |= V32_RATE_CAPABILITY_14400;
	if (id == DP_V32)
		rates &= V32_RATE_CAPABILITY_4800 | V32_RATE_CAPABILITY_9600;
	return rates;
}

static uint8_t selected_trellis_data_bits(
	const struct v32_datapump *datapump)
{
	struct v32_rate_offer selected;

	switch (datapump->startup.selected_rate) {
	case V32_RATE_CAPABILITY_7200:
		return V32_7200_DATA_BITS;
	case V32_RATE_CAPABILITY_9600:
		return v32_startup_selected_offer(&datapump->startup, &selected) &&
		       selected.trellis_at_highest_rate ?
			V32_9600_TRELLIS_DATA_BITS : 0;
	case V32_RATE_CAPABILITY_12000:
		return V32_12000_DATA_BITS;
	case V32_RATE_CAPABILITY_14400:
		return V32_14400_DATA_BITS;
	default:
		return 0;
	}
}

static bool uses_trellis(const struct v32_datapump *datapump)
{
	return selected_trellis_data_bits(datapump) != 0;
}

static struct v32_datapump *v32_from_dp(struct dp *dp)
{
	struct v32_datapump *datapump;
	uint8_t trellis_data_bits;

	if (dp == NULL ||
	    (dp->id != DP_V32 && dp->id != DP_V32BIS) ||
	    dp->op != &v32_operations ||
	    dp->modem == NULL || dp->dp_data == NULL)
		return NULL;
	datapump = dp->dp_data;
	trellis_data_bits = selected_trellis_data_bits(datapump);
	if (&datapump->dp != dp ||
	    !training_session_is_valid(&datapump->training) ||
	    datapump->missing_carrier_samples > V32_CARRIER_LOSS_SAMPLES ||
	    !line_state_is_valid(datapump) ||
	    !qam_modulator_is_valid(&datapump->modulator) ||
	    !v32_startup_is_valid(&datapump->startup) ||
	    datapump->startup.role != endpoint_role(datapump->caller) ||
	    datapump->startup.supported_rates != rate_capabilities(dp->modem, dp->id) ||
	    (!training_session_has_failed(&datapump->training) &&
	     training_session_is_online(&datapump->training) !=
		     v32_startup_is_complete(&datapump->startup)) ||
	    (datapump->trellis_encoder_initialized &&
	     (trellis_data_bits == 0 ||
	      !v32_trellis_encoder_is_valid(
		      &datapump->trellis_encoder) ||
	      datapump->trellis_encoder.data_bits_per_symbol !=
		      trellis_data_bits)) ||
	    datapump->encoder.phase_quadrant >=
		    QPSK_QUADRANT_COUNT ||
	    !v32_scrambler_is_valid(&datapump->encoder.scrambler) ||
	    datapump->encoder.scrambler.transmitter_role !=
		    local_role(datapump->caller) ||
	    !point_is_finite(&datapump->transmit_point) ||
	    (!training_session_has_failed(&datapump->training) &&
	     training_session_is_online(&datapump->training) !=
		     (dp->status == DPSTAT_CONNECT)) ||
	    (training_session_is_online(&datapump->training) &&
	     trellis_data_bits != 0 &&
	     !datapump->trellis_encoder_initialized))
		return NULL;
	return datapump;
}

static int initialize_protocol(
	struct v32_datapump *datapump, uint8_t rates,
	bool retraining)
{
	struct qam_channel channel;

	if (v32_carrier_channel(&channel) != V32_CARRIER_OK ||
	    channel.sample_rate != MODEM_RATE ||
	    qam_modulator_init(
		    &datapump->modulator, &channel,
		    V32_TRANSMIT_AMPLITUDE) != QAM_OK ||
	    v32_symbol_encoder_init(
		    &datapump->encoder, local_role(datapump->caller), 0) !=
		    V32_FRAMING_OK ||
	    v32_startup_init(
		    &datapump->startup, endpoint_role(datapump->caller),
		    rates) != V32_STARTUP_OK ||
	    v32_4800_constellation_point(
		    0, &datapump->transmit_point) != V32_CONSTELLATION_OK)
		return -1;
	if (datapump->caller) {
		if ((retraining ?
		     v32_line_retrain_init(
			     &datapump->line_startup,
			     V32_TRANSMIT_AMPLITUDE) :
		     v32_line_startup_init(
			     &datapump->line_startup,
			     V32_TRANSMIT_AMPLITUDE)) != V32_LINE_STARTUP_OK ||
		    v32_answerer_line_startup_init(
			    &datapump->answerer_line_startup,
			    V32_TRANSMIT_AMPLITUDE) != V32_LINE_STARTUP_OK)
			return -1;
	} else {
		if (v32_line_startup_init(
			    &datapump->line_startup,
			    V32_TRANSMIT_AMPLITUDE) != V32_LINE_STARTUP_OK ||
		    (retraining ?
		     v32_answerer_line_retrain_init(
			     &datapump->answerer_line_startup,
			     V32_TRANSMIT_AMPLITUDE) :
		     v32_answerer_line_startup_init(
			     &datapump->answerer_line_startup,
			     V32_TRANSMIT_AMPLITUDE)) != V32_LINE_STARTUP_OK)
			return -1;
	}
	memset(&datapump->trellis_encoder, 0,
	       sizeof(datapump->trellis_encoder));
	memset(&datapump->fixed_path, 0,
	       sizeof(datapump->fixed_path));
	datapump->missing_carrier_samples = 0;
	datapump->caller_s_extension_symbols = 0;
	datapump->trellis_encoder_initialized = false;
	datapump->answerer_s_detected = false;
	return 0;
}

static struct dp *create_v32(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, const struct dp_operations *operations)
{
	struct v32_datapump *datapump;
	struct single_tone_detector calling_carrier;
	bool is_caller = caller != 0;
	uint8_t rates = rate_capabilities(modem, id);

	if (modem == NULL ||
	    (id != DP_V32 && id != DP_V32BIS) ||
	    (caller != 0 && caller != 1) ||
	    sample_rate != MODEM_RATE || max_fragment != MODEM_FRAG ||
	    operations != &v32_operations ||
	    rates == 0)
		return NULL;
	datapump = calloc(1, sizeof(*datapump));
	if (datapump == NULL)
		return NULL;
	datapump->caller = is_caller;
	if (initialize_protocol(datapump, rates, false) != 0 ||
	    training_session_init(
		    &datapump->training,
		    datapump_carrier_wait_ticks(modem)) !=
		    TRAINING_SESSION_OK) {
		free(datapump);
		return NULL;
	}
	if (!is_caller && v8_datapump_v32_handoff(modem->dp, &calling_carrier) &&
	    v32_answerer_line_resume(&datapump->answerer_line_startup,
		    V32_TRANSMIT_AMPLITUDE, &calling_carrier) != V32_LINE_STARTUP_OK) {
		free(datapump);
		return NULL;
	}
	if (is_caller && v8_datapump_v32_handoff(modem->dp, &calling_carrier) &&
	    v32_line_startup_resume(&datapump->line_startup,
		    V32_TRANSMIT_AMPLITUDE) != V32_LINE_STARTUP_OK) {
		free(datapump);
		return NULL;
	}
	if (!is_caller && v8_datapump_answer_tone_complete(modem->dp) &&
	    v32_answerer_line_after_answer_init(&datapump->answerer_line_startup,
		    V32_TRANSMIT_AMPLITUDE) != V32_LINE_STARTUP_OK) {
		free(datapump);
		return NULL;
	}
	datapump->dp = (struct dp){
		.id = id,
		.modem = modem,
		.status = DPSTAT_OK,
		.op = operations,
		.dp_data = datapump,
	};
	return &datapump->dp;
}

static int prepare_retrain(
	struct v32_datapump *datapump, enum training_origin origin)
{
	if (initialize_protocol(
		    datapump, rate_capabilities(datapump->dp.modem, datapump->dp.id),
		    true) != 0 ||
	    training_session_start_retrain(
		    &datapump->training, origin) != TRAINING_SESSION_OK)
		return -1;
	datapump->dp.status = DPSTAT_OK;
	return 0;
}

static int retrain_v32(struct dp *dp)
{
	struct v32_datapump *datapump = v32_from_dp(dp);
	struct v32_datapump next;

	if (datapump == NULL)
		return -1;
	if (training_session_is_retraining(&datapump->training))
		return 0;
	if (!training_session_is_online(&datapump->training))
		return -1;
	next = *datapump;
	if (prepare_retrain(&next, TRAINING_LOCAL_RETRAIN) != 0)
		return -1;
	*datapump = next;
	if (!datapump->dp.modem->cfg.ec)
		modem_async_retrain(datapump->dp.modem);
	return 0;
}

static int delete_v32(struct dp *dp)
{
	struct v32_datapump *datapump = v32_from_dp(dp);

	if (datapump == NULL)
		return -1;
	datapump->dp.dp_data = NULL;
	free(datapump);
	return 0;
}

static enum v32_framing_result encode_4800_transmit_quadrant(
	struct v32_symbol_encoder *encoder,
	enum v32_startup_output output, uint8_t input,
	uint8_t *quadrant)
{
	if (output == V32_STARTUP_TRN_DIBIT)
		return v32_trn_symbol_encode(
			encoder, input, quadrant);
	if (output == V32_STARTUP_DIFFERENTIAL_DIBIT ||
	    output == V32_STARTUP_SELECTED_RATE_MARK ||
	    output == V32_STARTUP_DATA)
		return v32_4800_symbol_encode(
			encoder, input, quadrant);
	return V32_FRAMING_INVALID_ARGUMENT;
}

static uint8_t pack_bits(const uint8_t *bits, size_t bit_count)
{
	uint8_t packed = 0;

	for (size_t index = 0; index < bit_count; ++index)
		packed = (uint8_t)(((unsigned int)packed << 1u) |
				   (unsigned int)(bits[index] & 1u));
	return packed;
}

static int encode_trellis_transmit_point(
	struct v32_datapump *datapump, uint8_t input)
{
	struct v32_trellis_encoder next_encoder;
	struct v32_scrambler next_scrambler =
		datapump->encoder.scrambler;
	struct qam_point next_point;
	uint8_t data_bits = selected_trellis_data_bits(datapump);
	uint8_t scrambled;
	uint8_t signal_symbol;

	if (data_bits == 0)
		return -1;
	if (datapump->trellis_encoder_initialized) {
		next_encoder = datapump->trellis_encoder;
	} else if (v32_trellis_encoder_init(
			   &next_encoder, data_bits,
			   V32_TRELLIS_INITIAL_DIFFERENTIAL_OUTPUT) !=
		   V32_TRELLIS_OK) {
		return -1;
	}
	if (v32_scramble_group(
		    &next_scrambler, input, data_bits,
		    &scrambled) != V32_FRAMING_OK ||
	    v32_trellis_encode(
		    &next_encoder, scrambled, &signal_symbol) !=
		    V32_TRELLIS_OK ||
	    v32_trellis_constellation_point(
		    data_bits, signal_symbol, &next_point) !=
		    V32_CONSTELLATION_OK)
		return -1;
	datapump->encoder.scrambler = next_scrambler;
	datapump->trellis_encoder = next_encoder;
	datapump->transmit_point = next_point;
	datapump->trellis_encoder_initialized = true;
	return 0;
}

static int next_transmit_point(
	struct v32_datapump *datapump, bool *linked)
{
	uint8_t input = 3;
	uint8_t symbol;
	uint8_t trellis_data_bits = selected_trellis_data_bits(datapump);
	bool uncoded_9600 =
		datapump->startup.selected_rate == V32_RATE_CAPABILITY_9600 &&
		trellis_data_bits == 0;
	size_t bit_count = V32_4800_BITS_PER_SYMBOL;
	enum v32_startup_output output;
	enum v32_framing_result framing_result;

	if (v32_startup_next_transmit(
		    &datapump->startup, &output, &input) !=
	    V32_STARTUP_OK)
		return -1;
	if (output == V32_STARTUP_DATA) {
		uint8_t bits[V32_TRELLIS_MAX_DATA_BITS];

		if (!training_session_is_online(&datapump->training)) {
			if (enter_link(datapump) != 0)
				return -1;
			*linked = true;
		}
		if (uncoded_9600)
			bit_count = V32_9600_BITS_PER_SYMBOL;
		else if (trellis_data_bits != 0)
			bit_count = trellis_data_bits;
		if (!datapump_source_bits(
			    &datapump->dp, true, bits, bit_count))
			return -1;
		input = pack_bits(bits, bit_count);
	}
	if (output == V32_STARTUP_SELECTED_RATE_MARK &&
	    uncoded_9600)
		input = 15;
	if (output == V32_STARTUP_SELECTED_RATE_MARK &&
	    trellis_data_bits != 0)
		input = (uint8_t)((UINT8_C(1) << trellis_data_bits) - 1u);
	if (trellis_data_bits != 0 &&
	    (output == V32_STARTUP_SELECTED_RATE_MARK ||
	     output == V32_STARTUP_DATA)) {
		if (encode_trellis_transmit_point(datapump, input) != 0)
			return -1;
	} else if (uncoded_9600 &&
	    (output == V32_STARTUP_SELECTED_RATE_MARK ||
	     output == V32_STARTUP_DATA)) {
		framing_result = v32_9600_symbol_encode(
			&datapump->encoder, input, &symbol);
		if (framing_result != V32_FRAMING_OK ||
		    v32_constellation_point(
			    symbol, &datapump->transmit_point) !=
			    V32_CONSTELLATION_OK)
			return -1;
	} else {
		framing_result = encode_4800_transmit_quadrant(
			&datapump->encoder, output, input, &symbol);
		if (framing_result != V32_FRAMING_OK ||
		    v32_4800_constellation_point(
			    symbol, &datapump->transmit_point) !=
			    V32_CONSTELLATION_OK)
			return -1;
	}
	if (!training_session_is_online(&datapump->training) &&
	    v32_startup_is_complete(&datapump->startup)) {
		if (enter_link(datapump) != 0)
			return -1;
		*linked = true;
	}
	return 0;
}

static int commit_fixed_rate_drain(
	struct v32_fixed_rate_drain *drain,
	enum v32_receive_path_result receive_result,
	struct v32_equalizer_observation *observation)
{
	while (receive_result == V32_RECEIVE_PATH_OBSERVATION_READY) {
		struct v32_uncoded_receiver next = drain->four_point;
		struct v32_uncoded_symbol symbol;

		if (v32_uncoded_receiver_observe(
			    &next, V32_4800_BITS_PER_SYMBOL,
			    observation, true, &symbol) !=
			    V32_UNCODED_RECEIVER_OK)
			return -1;
		receive_result = v32_receive_path_commit(
			&drain->receive, &symbol.equalizer, observation);
		if (receive_result != V32_RECEIVE_PATH_OK &&
		    receive_result != V32_RECEIVE_PATH_OBSERVATION_READY)
			return -1;
		drain->four_point = next;
		drain->observation_epoch =
			symbol.equalizer.observation.epoch;
	}
	return receive_result == V32_RECEIVE_PATH_OK &&
	       fixed_rate_drain_is_valid(drain) ? 0 : -1;
}

static int initialize_fixed_rate_drain(
	struct v32_fixed_rate_drain *drain,
	const struct v32_rate_path_handoff *handoff)
{
	struct v32_fixed_rate_drain initialized;

	if (drain == NULL || !v32_rate_path_handoff_is_valid(handoff))
		return -1;
	initialized = (struct v32_fixed_rate_drain){
		.receive = handoff->receive,
		.four_point = handoff->four_point,
		.observation_epoch = handoff->observation_epoch,
	};
	if (handoff->observation_ready) {
		struct v32_equalizer_observation observation =
			handoff->observation;

		if (commit_fixed_rate_drain(
			    &initialized,
			    V32_RECEIVE_PATH_OBSERVATION_READY,
			    &observation) != 0)
			return -1;
	}
	if (!fixed_rate_drain_is_valid(&initialized) ||
	    v32_receive_path_freeze_gain(&initialized.receive) !=
		    V32_RECEIVE_PATH_OK ||
	    !fixed_rate_drain_is_valid(&initialized))
		return -1;
	*drain = initialized;
	return 0;
}

static int sample_fixed_rate_drain(
	struct v32_fixed_rate_drain *drain, int16_t received,
	const struct v32_echo_reference *reference)
{
	struct v32_equalizer_observation observation;
	enum v32_receive_path_result result;

	if (!fixed_rate_drain_is_valid(drain))
		return -1;
	result = v32_receive_path_sample(
		&drain->receive, received, reference, &observation);
	return commit_fixed_rate_drain(drain, result, &observation);
}

static int finish_fixed_echo(
	struct v32_datapump *datapump, bool *linked)
{
	struct v32_symbol_encoder encoder;
	struct v32_conditioning_path conditioning;
	struct v32_receive_path *receive =
		&datapump->fixed_path.owner.drain.receive;

	if (v32_echo_training_framing_state(
		    &datapump->fixed_path.training.echo, &encoder) !=
		    V32_ECHO_TRAINING_OK ||
	    v32_conditioning_path_init_from_receive(
		    &conditioning, V32_ANSWERING_TRANSMITTER, receive) !=
		    V32_CONDITIONING_PATH_OK)
		return -1;
	/* V.32bis 6.1 advances on the peer's conditioning signal. Residual
	 * echo or noise can prevent absolute silence even on a usable line;
	 * the conditioning owner must acquire S/S-bar and qualify peer TRN. */
	datapump->fixed_path.owner.conditioning = conditioning;
	datapump->encoder = encoder;
	datapump->fixed_path.stage =
		V32_FIXED_CALLER_SECOND_CONDITIONING;
	return next_transmit_point(datapump, linked);
}

static int finish_fixed_answerer_echo(
	struct v32_datapump *datapump, bool *linked)
{
	struct v32_symbol_encoder encoder;
	struct v32_startup startup = datapump->startup;
	struct v32_conditioning_path conditioning;

	if (datapump->fixed_path.stage !=
		    V32_FIXED_ANSWERER_INITIAL_ECHO ||
	    v32_echo_training_framing_state(
		    &datapump->fixed_path.training.echo, &encoder) !=
		    V32_ECHO_TRAINING_OK ||
	    v32_startup_finish_conditioning(&startup) != V32_STARTUP_OK ||
	    v32_conditioning_path_init_from_receive(
		    &conditioning, V32_CALLING_TRANSMITTER,
		    &datapump->fixed_path.owner.receive) !=
		    V32_CONDITIONING_PATH_OK)
		return -1;
	datapump->fixed_path.owner.conditioning = conditioning;
	datapump->fixed_path.stage = V32_FIXED_ANSWERER_CONDITIONING;
	datapump->startup = startup;
	datapump->encoder = encoder;
	datapump->answerer_s_detected = false;
	return next_transmit_point(datapump, linked);
}

static int finish_fixed_answerer_conditioning(
	struct v32_datapump *datapump, bool *linked)
{
	struct v32_conditioning_transmitter *conditioning =
		&datapump->fixed_path.training.conditioning;
	struct v32_end_path end = datapump->fixed_path.owner.end;
	struct v32_startup startup = datapump->startup;
	struct v32_symbol_encoder encoder;

	if (datapump->fixed_path.stage !=
		    V32_FIXED_ANSWERER_SECOND_CONDITIONING ||
	    v32_conditioning_transmitter_framing_state(
		    conditioning, &encoder) != V32_CONDITIONING_OK ||
	    v32_end_path_set_adaptation(
		    &end, V32_EQUALIZER_ADAPTATION_DISABLED) !=
		    V32_END_PATH_OK ||
	    v32_startup_finish_conditioning(&startup) != V32_STARTUP_OK)
		return -1;
	datapump->fixed_path.owner.end = end;
	datapump->fixed_path.stage = V32_FIXED_ANSWERER_END;
	datapump->startup = startup;
	datapump->encoder = encoder;
	datapump->answerer_s_detected = false;
	return next_transmit_point(datapump, linked);
}

static int next_cleardown_point(struct v32_datapump *datapump)
{
	if (datapump->cleardown_symbols < V32_CLEARDOWN_SYMBOLS) {
		const struct v32_rate_offer empty = { 0 };
		uint16_t word;
		uint8_t dibit;
		uint8_t quadrant;

		if (v32_rate_signal_encode(V32_CLEARDOWN_SIGNAL, &empty, &word) !=
			    V32_RATE_SIGNAL_OK ||
		    v32_rate_signal_dibit(
			    word, datapump->cleardown_symbols % V32_RATE_SIGNAL_DIBITS,
			    &dibit) != V32_RATE_SIGNAL_OK ||
		    v32_4800_symbol_encode(
			    &datapump->encoder, dibit, &quadrant) != V32_FRAMING_OK ||
		    v32_4800_constellation_point(
			    quadrant, &datapump->transmit_point) != V32_CONSTELLATION_OK)
			return -1;
	} else {
		/* Retain RTP long enough to flush the final rate word's pulse. */
		datapump->transmit_point = (struct qam_point){ 0 };
	}
	if (datapump->cleardown_symbols < V32_CLEARDOWN_SYMBOLS +
		    datapump->modulator.channel.pulse_span_symbols + 1u)
		datapump->cleardown_symbols++;
	return 0;
}

static int generate_fixed_path_sample(
	struct v32_datapump *datapump, int16_t *sample,
	struct v32_echo_reference *reference, bool *reference_ready,
	bool *linked, uint64_t sample_number)
{
	static const struct qam_point silence;
	bool can_accept = qam_modulator_can_accept_symbol(
		&datapump->modulator);
	bool accepted;
	bool line_is_silent =
		datapump->fixed_path.stage ==
			V32_FIXED_CALLER_CONDITIONING ||
		datapump->fixed_path.stage ==
			V32_FIXED_CALLER_RATE_OFFER ||
		datapump->fixed_path.stage ==
			V32_FIXED_CALLER_RATE_DRAIN;
	uint32_t carrier_phase = datapump->modulator.carrier_phase;

	if (!fixed_path_is_valid(datapump) || sample == NULL ||
	    reference == NULL || reference_ready == NULL || linked == NULL)
		return -1;
	*reference_ready = false;
	if (can_accept) {
		switch (datapump->fixed_path.stage) {
		case V32_FIXED_CALLER_CONDITIONING:
		case V32_FIXED_CALLER_RATE_OFFER:
			datapump->transmit_point = silence;
			break;
		case V32_FIXED_CALLER_RATE_DRAIN:
			datapump->fixed_path.stage = V32_FIXED_CALLER_ECHO;
			line_is_silent = false;
			/* fall through */
		case V32_FIXED_CALLER_ECHO: {
			uint8_t quadrant;
			enum v32_echo_training_result result =
				v32_echo_training_next(
					&datapump->fixed_path.training.echo,
					&datapump->fixed_path.owner.drain.receive,
					&quadrant);

			if (result == V32_ECHO_TRAINING_COMPLETE) {
				if (finish_fixed_echo(datapump, linked) != 0)
					return -1;
			} else if (result != V32_ECHO_TRAINING_OK ||
				   v32_4800_constellation_point(
					   quadrant,
					   &datapump->transmit_point) !=
					   V32_CONSTELLATION_OK) {
				return -1;
			}
			break;
		}
		case V32_FIXED_CALLER_SECOND_CONDITIONING:
		case V32_FIXED_CALLER_SELECTION:
		case V32_FIXED_CALLER_END:
		case V32_FIXED_B1:
		case V32_FIXED_DATA:
			if (next_transmit_point(datapump, linked) != 0)
				return -1;
			break;
		case V32_FIXED_ANSWERER_INITIAL_ECHO: {
			uint8_t quadrant;
			enum v32_echo_training_result result =
				v32_echo_training_next(
					&datapump->fixed_path.training.echo,
					&datapump->fixed_path.owner.receive,
					&quadrant);

			if (result == V32_ECHO_TRAINING_COMPLETE) {
				if (finish_fixed_answerer_echo(
					    datapump, linked) != 0)
					return -1;
			} else if (result != V32_ECHO_TRAINING_OK ||
				   v32_4800_constellation_point(
					   quadrant,
					   &datapump->transmit_point) !=
					   V32_CONSTELLATION_OK) {
				return -1;
			}
			break;
		}
		case V32_FIXED_ANSWERER_SECOND_CONDITIONING:
		case V32_FIXED_ANSWERER_CLEARDOWN_CONDITIONING: {
			uint8_t quadrant;
			enum v32_conditioning_result result =
				v32_conditioning_transmitter_next(
					&datapump->fixed_path.training.conditioning,
					&quadrant);

			if (result == V32_CONDITIONING_COMPLETE) {
				if (fixed_path_is_clearing(datapump)) {
					if (v32_conditioning_transmitter_framing_state(
						    &datapump->fixed_path.training.conditioning,
						    &datapump->encoder) != V32_CONDITIONING_OK)
						return -1;
					datapump->fixed_path.stage =
						V32_FIXED_ANSWERER_CLEARDOWN;
					if (next_cleardown_point(datapump) != 0)
						return -1;
				} else if (finish_fixed_answerer_conditioning(
						   datapump, linked) != 0) {
					return -1;
				}
			} else if (result != V32_CONDITIONING_OK ||
				   v32_4800_constellation_point(
					   quadrant,
					   &datapump->transmit_point) !=
					   V32_CONSTELLATION_OK) {
				return -1;
			}
			break;
		}
		case V32_FIXED_ANSWERER_CONDITIONING:
			if (datapump->answerer_s_detected)
				datapump->transmit_point = silence;
			else if (next_transmit_point(datapump, linked) != 0)
				return -1;
			break;
		case V32_FIXED_ANSWERER_RATE:
			datapump->transmit_point = silence;
			break;
		case V32_FIXED_ANSWERER_CLEARDOWN:
			if (next_cleardown_point(datapump) != 0)
				return -1;
			break;
		case V32_FIXED_ANSWERER_END:
			if (next_transmit_point(datapump, linked) != 0)
				return -1;
			break;
		case V32_FIXED_OFF:
			return -1;
		}
	}
	if (qam_modulator_sample(
		    &datapump->modulator, &datapump->transmit_point,
		    sample, &accepted) != QAM_OK ||
	    accepted != can_accept)
		return -1;
	if (line_is_silent)
		*sample = (sample_number & 1u) != 0 ? -1 : 0;
	if (accepted) {
		if (v32_echo_reference_init(
			    reference, &datapump->transmit_point,
			    carrier_phase) != V32_ECHO_OK)
			return -1;
		*reference_ready = true;
	}
	return 0;
}

static int process_caller_line_startup(
	struct v32_datapump *datapump, int16_t receive_sample,
	int16_t *transmit_sample)
{
	enum v32_line_startup_result result;
	bool complete;

	result = v32_line_startup_process(
		&datapump->line_startup, receive_sample, transmit_sample,
		&complete);
	if (result != V32_LINE_STARTUP_OK)
		return -1;
	if (complete) {
		struct v32_conditioning_path conditioning;
		struct v32_line_timing timing;

		if (!v32_line_timing_from_samples(
			    v32_line_startup_round_trip_samples(
				    &datapump->line_startup),
			    true, &timing) ||
		    v32_conditioning_path_init(
			    &conditioning, V32_ANSWERING_TRANSMITTER, 0,
			    timing.echo_round_trip_symbols) !=
			    V32_CONDITIONING_PATH_OK)
			return -1;
		datapump->fixed_path.owner.conditioning = conditioning;
		datapump->fixed_path.stage =
			V32_FIXED_CALLER_CONDITIONING;
		datapump->caller_s_extension_symbols =
			timing.caller_s_extension_symbols;
	}
	return 0;
}

static int process_answerer_line_startup(
	struct v32_datapump *datapump, int16_t receive_sample,
	int16_t *transmit_sample)
{
	struct v32_receive_path receive;
	struct v32_echo_training echo;
	struct v32_line_timing timing;
	bool complete;

	if (v32_answerer_line_startup_process(
		    &datapump->answerer_line_startup, receive_sample,
		    transmit_sample, &complete) != V32_LINE_STARTUP_OK)
		return -1;
	if (!complete)
		return 0;
	if (!v32_line_timing_from_samples(
		    v32_answerer_line_startup_round_trip_samples(
			    &datapump->answerer_line_startup),
		    false, &timing) ||
	    v32_receive_path_init(
		    &receive, 0, timing.echo_round_trip_symbols) !=
		    V32_RECEIVE_PATH_OK ||
	    v32_echo_training_init(
		    &echo, V32_ANSWERING_TRANSMITTER, 0) !=
		    V32_ECHO_TRAINING_OK ||
	    !v32_echo_training_path_is_valid(&echo, &receive))
		return -1;
	datapump->fixed_path.owner.receive = receive;
	datapump->fixed_path.training.echo = echo;
	datapump->fixed_path.stage =
		V32_FIXED_ANSWERER_INITIAL_ECHO;
	return 0;
}

static unsigned int selected_bit_rate(
	const struct v32_datapump *datapump)
{
	switch (datapump->startup.selected_rate) {
	case V32_RATE_CAPABILITY_4800:
		return V32_4800_BIT_RATE;
	case V32_RATE_CAPABILITY_7200:
		return V32_7200_BIT_RATE;
	case V32_RATE_CAPABILITY_9600:
		return V32_9600_BIT_RATE;
	case V32_RATE_CAPABILITY_12000:
		return V32_12000_BIT_RATE;
	case V32_RATE_CAPABILITY_14400:
		return V32_14400_BIT_RATE;
	default:
		return 0;
	}
}

static int enter_link(struct v32_datapump *datapump)
{
	unsigned int bit_rate = selected_bit_rate(datapump);

	if (bit_rate == 0 || !fixed_path_is_active(datapump) ||
	    datapump->fixed_path.stage != V32_FIXED_DATA ||
	    !v32_data_path_is_valid(&datapump->fixed_path.owner.data) ||
	    (uses_trellis(datapump) &&
	     !datapump->trellis_encoder_initialized) ||
	    (!uses_trellis(datapump) &&
	     datapump->trellis_encoder_initialized))
		return -1;
	if (!datapump_set_link_rates(&datapump->dp, bit_rate, bit_rate))
		return -1;
	datapump->missing_carrier_samples = 0;
	return datapump_complete_training(&datapump->dp, &datapump->training) ? 0 : -1;
}

static int deliver_fixed_batch(
	struct v32_datapump *datapump,
	const struct v32_data_batch *batch)
{
	for (uint8_t index = 0; index < batch->count; ++index) {
		const struct v32_data_group *group = &batch->groups[index];
		uint8_t bits[V32_TRELLIS_MAX_DATA_BITS];

		for (size_t bit = 0; bit < group->bit_count; ++bit)
			bits[bit] = (uint8_t)((group->value >>
				(group->bit_count - bit - 1u)) & 1u);
		if (!datapump_deliver_bits(
			    &datapump->dp, true, bits, group->bit_count))
			return -1;
	}
	return 0;
}

static int advance_fixed_b1(
	struct v32_datapump *datapump, uint8_t previous_observations,
	enum v32_b1_path_result result, bool *linked)
{
	struct v32_b1_path *b1 = &datapump->fixed_path.owner.b1;
	struct v32_startup next_startup = datapump->startup;
	uint8_t observations;
	bool connected = false;

	if ((result != V32_B1_PATH_OK &&
	     result != V32_B1_PATH_COMPLETE) ||
	    !v32_b1_path_is_valid(b1) ||
	    previous_observations > b1->observations)
		return -1;
	observations = b1->observations;
	for (uint8_t index = previous_observations;
	     index < observations; ++index) {
		/*
		 * B1 is a fixed 128-symbol transition interval.  Its decoded
		 * contents remain useful receive-quality evidence, but an early
		 * decision error must not turn the whole interval into 128
		 * synthetic non-mark symbols and permanently withhold CONNECT.
		 */
		if (v32_startup_note_confirmation(
			    &next_startup, 3,
			    &connected) != V32_STARTUP_OK)
			return -1;
	}
	if (result != V32_B1_PATH_COMPLETE) {
		if (connected)
			return -1;
		datapump->startup = next_startup;
		return 0;
	}
	{
		struct v32_data_path data;
		struct v32_data_batch batch;

		if (v32_data_path_init(&data, b1, &batch) !=
			    V32_DATA_PATH_OK)
			return -1;
		datapump->startup = next_startup;
		datapump->fixed_path.owner.data = data;
		datapump->fixed_path.stage = V32_FIXED_DATA;
		if (deliver_fixed_batch(datapump, &batch) != 0)
			return -1;
		if (connected) {
			if (enter_link(datapump) != 0)
				return -1;
			*linked = true;
		}
	}
	return 0;
}

static int finish_fixed_end(
	struct v32_datapump *datapump, bool *linked)
{
	struct v32_end_path_handoff handoff;
	struct v32_b1_path b1;
	struct v32_startup next_startup = datapump->startup;
	enum v32_b1_path_result result;

	if (v32_end_path_handoff(
		    &datapump->fixed_path.owner.end, &handoff) !=
		    V32_END_PATH_OK ||
	    v32_startup_note_end_signal(
		    &next_startup, &handoff.selected_offer) !=
		    V32_STARTUP_OK ||
	    !next_startup.end_received)
		return -1;
	result = v32_b1_path_init(&b1, &handoff);
	if (result != V32_B1_PATH_OK &&
	    result != V32_B1_PATH_COMPLETE)
		return -1;
	datapump->startup = next_startup;
	datapump->fixed_path.owner.b1 = b1;
	datapump->fixed_path.stage = V32_FIXED_B1;
	return advance_fixed_b1(datapump, 0, result, linked);
}

static int finish_fixed_rate(
	struct v32_datapump *datapump, bool *linked)
{
	struct v32_rate_path_handoff handoff;
	struct v32_startup next_startup = datapump->startup;
	enum v32_fixed_stage stage =
		datapump->fixed_path.stage;

	if ((stage != V32_FIXED_CALLER_RATE_OFFER &&
	     stage != V32_FIXED_CALLER_SELECTION &&
	     stage != V32_FIXED_ANSWERER_RATE) ||
	    v32_rate_path_handoff(
		    &datapump->fixed_path.owner.rate, &handoff) !=
		    V32_RATE_PATH_OK ||
	    v32_startup_note_rate_signal(
		    &next_startup, &handoff.offer) != V32_STARTUP_OK)
		return -1;
	if (stage == V32_FIXED_ANSWERER_RATE) {
		struct v32_rate_offer selected_offer;
		struct v32_conditioning_transmitter conditioning;
		struct v32_end_path end;
		enum v32_end_path_result end_result;

		if (!v32_startup_selected_offer(
			    &next_startup, &selected_offer))
			return -1;
		end_result = v32_end_path_init(
			&end, &handoff, &selected_offer,
			V32_EQUALIZER_ADAPTATION_FINE);
		if (end_result != V32_END_PATH_OK ||
		    v32_conditioning_transmitter_init(
			    &conditioning, V32_ANSWERING_TRANSMITTER,
			    V32_ANSWERING_SECOND_TRN_SYMBOLS) !=
			    V32_CONDITIONING_OK)
			return -1;
		datapump->startup = next_startup;
		datapump->fixed_path.owner.end = end;
		datapump->fixed_path.training.conditioning = conditioning;
		datapump->fixed_path.stage =
			V32_FIXED_ANSWERER_SECOND_CONDITIONING;
		datapump->answerer_s_detected = false;
		return 0;
	}
	if (stage == V32_FIXED_CALLER_RATE_OFFER) {
		struct v32_fixed_rate_drain drain;
		struct v32_echo_training echo;

		if (datapump->caller_s_extension_symbols == 0 ||
		    initialize_fixed_rate_drain(&drain, &handoff) != 0 ||
		    v32_echo_training_init(
			    &echo, V32_CALLING_TRANSMITTER,
			    datapump->caller_s_extension_symbols) !=
			    V32_ECHO_TRAINING_OK ||
		    !v32_echo_training_path_is_valid(
			    &echo, &drain.receive))
			return -1;
		datapump->startup = next_startup;
		datapump->fixed_path.owner.drain = drain;
		datapump->fixed_path.training.echo = echo;
		datapump->fixed_path.stage = V32_FIXED_CALLER_RATE_DRAIN;
		datapump->caller_s_extension_symbols = 0;
		return 0;
	}
	{
		struct v32_end_path end;
		struct v32_rate_offer selected_offer;
		enum v32_end_path_result result;

		if ((next_startup.selected_rate & handoff.offer.rates) !=
			    next_startup.selected_rate ||
		    next_startup.selection_expected ||
		    !v32_startup_selected_offer(&next_startup, &selected_offer))
			return -1;
		result = v32_end_path_init(
			&end, &handoff, &selected_offer,
			V32_EQUALIZER_ADAPTATION_FINE);
		if (result != V32_END_PATH_OK &&
		    result != V32_END_PATH_COMPLETE)
			return -1;
		datapump->startup = next_startup;
		datapump->fixed_path.owner.end = end;
		datapump->fixed_path.stage = V32_FIXED_CALLER_END;
		return result == V32_END_PATH_COMPLETE ?
			finish_fixed_end(datapump, linked) : 0;
	}
}

static int begin_fixed_rate(
	struct v32_datapump *datapump, bool selection, bool *linked)
{
	struct v32_conditioning_path_handoff handoff;
	struct v32_rate_path rate;
	struct v32_startup next_startup = datapump->startup;
	enum v32_rate_path_result result;

	if ((!datapump->caller && selection) ||
	    v32_conditioning_path_handoff(
		    &datapump->fixed_path.owner.conditioning,
		    &handoff) != V32_CONDITIONING_PATH_OK ||
	    (selection &&
	     v32_startup_expect_selection(&next_startup) !=
		     V32_STARTUP_OK))
		return -1;
	result = v32_rate_path_init(&rate, &handoff);
	if (result != V32_RATE_PATH_OK &&
	    result != V32_RATE_PATH_COMPLETE)
		return -1;
	datapump->startup = next_startup;
	datapump->fixed_path.owner.rate = rate;
	if (datapump->caller) {
		datapump->fixed_path.stage = selection ?
			V32_FIXED_CALLER_SELECTION :
			V32_FIXED_CALLER_RATE_OFFER;
	} else {
		datapump->fixed_path.stage = V32_FIXED_ANSWERER_RATE;
	}
	return result == V32_RATE_PATH_COMPLETE ?
		finish_fixed_rate(datapump, linked) : 0;
}

static int receive_fixed_path_sample(
	struct v32_datapump *datapump, int16_t sample,
	const struct v32_echo_reference *reference, bool *linked)
{
	switch (datapump->fixed_path.stage) {
	case V32_FIXED_CALLER_CONDITIONING:
	case V32_FIXED_CALLER_SECOND_CONDITIONING: {
		bool selection = datapump->fixed_path.stage ==
			V32_FIXED_CALLER_SECOND_CONDITIONING;
		enum v32_conditioning_path_result result =
			v32_conditioning_path_sample(
				&datapump->fixed_path.owner.conditioning,
				sample, reference);

		if (result != V32_CONDITIONING_PATH_OK &&
		    result != V32_CONDITIONING_PATH_COMPLETE)
			return -1;
		return result == V32_CONDITIONING_PATH_COMPLETE ?
			begin_fixed_rate(datapump, selection, linked) : 0;
	}
	case V32_FIXED_CALLER_RATE_OFFER:
	case V32_FIXED_CALLER_SELECTION:
	case V32_FIXED_ANSWERER_RATE: {
		enum v32_rate_path_result result = v32_rate_path_sample(
			&datapump->fixed_path.owner.rate, sample, reference);

		if (result == V32_RATE_PATH_CLEARDOWN && !datapump->caller) {
			/* V.32bis 6.2: send second conditioning, then R3 cleardown
			 * for at least 64T. No E or data handoff follows refusal. */
			if (v32_conditioning_transmitter_init(
				    &datapump->fixed_path.training.conditioning,
				    V32_ANSWERING_TRANSMITTER,
				    V32_CONDITIONING_TRN_MIN_SYMBOLS) != V32_CONDITIONING_OK)
				return -1;
			datapump->cleardown_symbols = 0;
			datapump->fixed_path.stage =
				V32_FIXED_ANSWERER_CLEARDOWN_CONDITIONING;
			return 0;
		}
		if (result != V32_RATE_PATH_OK &&
		    result != V32_RATE_PATH_COMPLETE)
			return -1;
		return result == V32_RATE_PATH_COMPLETE ?
			finish_fixed_rate(datapump, linked) : 0;
	}
	case V32_FIXED_CALLER_RATE_DRAIN:
		return sample_fixed_rate_drain(
			&datapump->fixed_path.owner.drain, sample, reference);
	case V32_FIXED_CALLER_ECHO: {
		struct v32_equalizer_observation observation;

		return v32_receive_path_sample(
			       &datapump->fixed_path.owner.drain.receive,
			       sample, reference, &observation) ==
			       V32_RECEIVE_PATH_OK ? 0 : -1;
	}
	case V32_FIXED_CALLER_END: {
		enum v32_end_path_result result = v32_end_path_sample(
			&datapump->fixed_path.owner.end, sample, reference);

		if (result != V32_END_PATH_OK &&
		    result != V32_END_PATH_COMPLETE)
			return -1;
		return result == V32_END_PATH_COMPLETE ?
			finish_fixed_end(datapump, linked) : 0;
	}
	case V32_FIXED_B1: {
		uint8_t previous =
			datapump->fixed_path.owner.b1.observations;
		enum v32_b1_path_result result = v32_b1_path_sample(
			&datapump->fixed_path.owner.b1, sample, reference);

		return advance_fixed_b1(datapump, previous, result, linked);
	}
	case V32_FIXED_DATA: {
		struct v32_data_batch batch;

		if (v32_data_path_sample(
			    &datapump->fixed_path.owner.data, sample,
			    reference, &batch) != V32_DATA_PATH_OK)
			return -1;
		if (deliver_fixed_batch(datapump, &batch) != 0)
			return -1;
		if (batch.retrain_requested) {
			struct v32_datapump next = *datapump;

			if (prepare_retrain(
				    &next, TRAINING_REMOTE_RETRAIN) != 0)
				return -1;
			*datapump = next;
			if (!datapump->dp.modem->cfg.ec)
				modem_async_retrain(datapump->dp.modem);
		}
		return 0;
	}
	case V32_FIXED_ANSWERER_INITIAL_ECHO: {
		struct v32_equalizer_observation observation;
		struct v32_receive_path *receive =
			&datapump->fixed_path.owner.receive;

		return v32_receive_path_sample(
			       receive, sample, reference, &observation) ==
			       V32_RECEIVE_PATH_OK ? 0 : -1;
	}
	case V32_FIXED_ANSWERER_SECOND_CONDITIONING:
		/* V.32bis 6.1 keeps R2 on the line until R3. Preserve the
		 * trained timing, carrier and decoder while sending our second
		 * conditioning sequence; only initial echo training pauses RX. */
		return v32_end_path_sample(
			       &datapump->fixed_path.owner.end, sample, reference) ==
			       V32_END_PATH_OK ? 0 : -1;
	case V32_FIXED_ANSWERER_CONDITIONING: {
		enum v32_conditioning_path_result result =
			v32_conditioning_path_sample(
				&datapump->fixed_path.owner.conditioning,
				sample, reference);

		if (result != V32_CONDITIONING_PATH_OK &&
		    result != V32_CONDITIONING_PATH_COMPLETE)
			return -1;
		if (datapump->fixed_path.owner.conditioning.conditioning.phase !=
		    V32_CONDITIONING_SEARCH_S_BAR)
			datapump->answerer_s_detected = true;
		return result == V32_CONDITIONING_PATH_COMPLETE ?
			begin_fixed_rate(datapump, false, linked) : 0;
	}
	case V32_FIXED_ANSWERER_END: {
		enum v32_end_path_result result = v32_end_path_sample(
			&datapump->fixed_path.owner.end, sample, reference);

		if (result != V32_END_PATH_OK &&
		    result != V32_END_PATH_COMPLETE)
			return -1;
		return result == V32_END_PATH_COMPLETE ?
			finish_fixed_end(datapump, linked) : 0;
	}
	case V32_FIXED_ANSWERER_CLEARDOWN_CONDITIONING:
	case V32_FIXED_ANSWERER_CLEARDOWN:
		return 0;
	case V32_FIXED_OFF:
		break;
	}
	return -1;
}

static int process_v32(
	struct dp *dp, void *receive_buffer, void *transmit_buffer, int count)
{
	struct v32_datapump *datapump = v32_from_dp(dp);
	const int16_t *receive = receive_buffer;
	int16_t *transmit = transmit_buffer;
	uint64_t frame_start = 0;
	bool linked = false;

	if (!datapump_linear_fragment_is_ready(datapump, receive, transmit, count))
		return DPSTAT_ERROR;
	if (training_session_has_failed(&datapump->training)) {
		memset(transmit, 0, (size_t)count * sizeof(*transmit));
		return DPSTAT_ERROR;
	}
	/* The final nonempty fragment must reach the host before reporting the
	 * disconnect; the ordinary failure path deliberately blanks its output. */
	if (datapump->fixed_path.stage == V32_FIXED_ANSWERER_CLEARDOWN &&
	    datapump->cleardown_symbols == V32_CLEARDOWN_SYMBOLS +
		    datapump->modulator.channel.pulse_span_symbols + 1u)
		goto failure;
	if (!training_session_is_online(&datapump->training) &&
	    !fixed_path_is_clearing(datapump) &&
	    training_session_advance(
		    &datapump->training,
		    (uint64_t)(unsigned int)count) !=
		    TRAINING_SESSION_OK)
		goto failure;
	if (!training_session_is_online(&datapump->training))
		frame_start = datapump->training.elapsed_ticks -
			      (uint64_t)(unsigned int)count;
	for (int index = 0; index < count; ++index) {
		if (!fixed_path_is_active(datapump)) {
			int startup_result = datapump->caller ?
				process_caller_line_startup(
					datapump, receive[index], &transmit[index]) :
				process_answerer_line_startup(
					datapump, receive[index], &transmit[index]);

			if (startup_result != 0)
				goto failure;
			continue;
		}
		{
			struct v32_echo_reference reference;
			bool reference_ready;

			if (generate_fixed_path_sample(
				    datapump, &transmit[index], &reference,
				    &reference_ready, &linked,
				    frame_start +
					    (uint64_t)(unsigned int)index) != 0 ||
			    receive_fixed_path_sample(
				    datapump, receive[index],
				    reference_ready ? &reference : NULL,
				    &linked) != 0)
				goto failure;
		}
	}
	if (linked)
		return DPSTAT_CONNECT;
	if (!training_session_is_online(&datapump->training))
		return DPSTAT_OK;
	if (fixed_path_receive_const(
		    &datapump->fixed_path)->line.timing.signal_detected &&
	    datapump_qam_line_power_present(receive, count)) {
		datapump->missing_carrier_samples = 0;
	} else {
		datapump_add_samples_bounded(
			&datapump->missing_carrier_samples,
			(uint32_t)(unsigned int)count,
			V32_CARRIER_LOSS_SAMPLES);
	}
	if (datapump->missing_carrier_samples < V32_CARRIER_LOSS_SAMPLES)
		return DPSTAT_OK;

failure:
	(void)training_session_fail(&datapump->training);
	memset(transmit, 0, (size_t)count * sizeof(*transmit));
	return DPSTAT_ERROR;
}

const struct dp_operations *v32_datapump_operations(enum DP_ID id)
{
	return id == DP_V32 || id == DP_V32BIS ? &v32_operations : NULL;
}

/* Automatic answering may stop an unanswered AC probe, but it must retain
 * the full family training interval once the calling carrier has arrived. */
bool v32_datapump_has_peer(struct dp *dp)
{
	struct v32_datapump *datapump = v32_from_dp(dp);

	return datapump != NULL && !datapump->caller &&
		(datapump->answerer_line_startup.calling_carrier.tone_present ||
		 v32_answerer_line_startup_is_complete(&datapump->answerer_line_startup));
}

static int tx_tail_samples(const struct dp *dp)
{
	const struct v32_datapump *p = v32_from_dp((struct dp *)dp);
	return p != NULL && training_session_is_online(&p->training) ?
		datapump_qam_tail(&p->modulator) : -1;
}
