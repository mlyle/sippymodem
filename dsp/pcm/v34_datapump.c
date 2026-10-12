// SPDX-License-Identifier: BSD-3-Clause

#include "v34_datapump.h"

#include "datapump_link.h"
#include "datapump_media.h"
#include "datapump_timing.h"
#include "memory_region.h"
#include "modem.h"
#include "training_session.h"
#include "v8_datapump.h"
#include "v34_call.h"
#include "v34_data_quality.h"
#include "v34_profile.h"
#include "v90_datapump.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

_Static_assert((int)V34_MAX_MAPPING_FRAME_BITS + 1 <= DATAPUMP_LINK_MAX_BITS,
	       "V.34 frame exceeds host bit delivery capacity");

enum {
	/* A local full retrain does not receive the peer's response until both
	 * stations have observed their 70 ms silence and 50 ms tone guards. */
	V34_CARRIER_LOSS_SAMPLES = MODEM_RATE / 2,
	/* A 2.5-ms window spans at least six V.34 symbols.  Requiring every
	 * window avoids treating a half-carrier, half-silence RTP frame as data. */
	V34_CARRIER_WINDOW_SAMPLES = MODEM_RATE / 400,
	/* One 20-ms media interval exceeds the trained equalizer's 20-symbol
	 * centre delay at every V.34 symbol rate. */
	V34_CARRIER_RECOVERY_SAMPLES = MODEM_RATE / 50,
};

struct v34_datapump {
	struct dp dp;
	struct v34_call call;
	struct v34_data_quality data_quality;
	struct training_session training;
	struct pcm_fallback_restart *pcm_restart;
	uint32_t missing_carrier_samples;
	uint32_t carrier_power_floor;
	uint16_t carrier_recovery_samples;
	uint16_t rate_mask;
	bool caller;
	bool retrain_carrier_acquired;
	bool pcm_retrain_pending;
};

static struct dp *create_v34(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, const struct dp_operations *operations);
static int delete_v34(struct dp *dp);
static int process_v34(
	struct dp *dp, void *receive, void *transmit, int count);
static int retrain_v34(struct dp *dp);
static int hangup_v34(struct dp *dp);
static const char *progress_v34(const struct dp *dp);
static struct dp_link_rates link_rates_v34(const struct dp *dp);

static int tx_tail_samples(const struct dp *dp);
static const struct dp_operations v34_operations = {
	.name = "V.34",
	.create = create_v34,
	.delete = delete_v34,
	.process = process_v34,
	.retrain = retrain_v34,
	.hangup = hangup_v34,
	.progress = progress_v34,
	.link_rates = link_rates_v34,
	.tx_tail_samples = tx_tail_samples,
};

static struct dp_link_rates link_rates_v34(const struct dp *dp)
{
	const struct v34_datapump *datapump = dp->dp_data;
	struct dp_link_rates rates = { 0 };
	const struct v34_phase3_profile *transmit, *receive;

	if (datapump == NULL || datapump->call.failed)
		return rates;
	const struct v34_call *call = &datapump->call;
	if (call->stage == V34_CALL_PHASE3) {
		transmit = &call->state.phase3.transmit.line.profile;
		receive = &call->state.phase3.receive.transmitting_profile;
	} else if (call->stage == V34_CALL_PHASE4) {
		const struct v34_phase4_exchange *phase4 = &call->state.phase4;

		transmit = phase4->transmit_data_active ?
			&phase4->transmit.data.line.profile :
			&phase4->transmit.training.line.profile;
		receive = &phase4->receive.transmitting_profile;
		if (phase4->data_startup_prepared) {
			rates.transmit_bps = phase4->data_startup.mode.transmit
				.primary_data_rate_2400_bps_units * V34_RATE_STEP;
			rates.receive_bps = phase4->data_startup.mode.receive
				.primary_data_rate_2400_bps_units * V34_RATE_STEP;
		}
	} else {
		return rates;
	}
	if (transmit->symbol_rate_denominator != 0)
		rates.transmit_baud = (double)transmit->symbol_rate_numerator /
			transmit->symbol_rate_denominator;
	if (receive->symbol_rate_denominator != 0)
		rates.receive_baud = (double)receive->symbol_rate_numerator /
			receive->symbol_rate_denominator;
	return rates;
}

static const char *progress_v34(const struct dp *dp)
{
	const struct v34_datapump *datapump = dp->dp_data;
	if (datapump == NULL)
		return "V.34/initializing";
	const struct v34_call *call = &datapump->call;
	switch (call->stage) {
	case V34_CALL_RETRAIN_SILENCE: return "retrain/silence-tone";
	case V34_CALL_CLEARDOWN: return "cleardown/zero-rate-MP";
	case V34_CALL_PHASE2:
		switch (call->state.phase2.stage) {
		case V34_PHASE2_RANGING:
			return call->state.phase2.state.ranging.control.phase ==
				PHASE2_RANGING_INFO0_EXCHANGE ? "phase2/INFO0" : "phase2/ranging";
		case V34_PHASE2_INITIAL_PROBE: return "phase2/L1-L2-probe";
		case V34_PHASE2_PROBE_TURNAROUND: return "phase2/probe-turnaround";
		case V34_PHASE2_RECIPROCAL_PROBE: return "phase2/reciprocal-probe";
		case V34_PHASE2_INFO1: return "phase2/INFO1";
		}
		break;
	case V34_CALL_PHASE3:
		switch (call->state.phase3.receive.semantic.phase3.stage) {
		case V34_PHASE3_DECISION_RECEIVER_S: return "phase3/wait-S-Sbar";
		case V34_PHASE3_DECISION_RECEIVER_MD: return "phase3/MD";
		case V34_PHASE3_DECISION_RECEIVER_REPEATED_S: return "phase3/repeated-S";
		case V34_PHASE3_DECISION_RECEIVER_PP: return "phase3/PP";
		case V34_PHASE3_DECISION_RECEIVER_J: return "phase3/TRN-wait-J";
		case V34_PHASE3_DECISION_RECEIVER_COMPLETE: return "phase3/finish-J";
		}
		break;
	case V34_CALL_PHASE4:
		if (call->state.phase4.online_announced)
			return "data/V.34";
		if (call->state.phase4.data_preparation_active) {
			switch (call->data_preparation.stage) {
			case V34_DATA_PREPARATION_TRANSMIT_MAPPING:
				return "phase4/calibrate-TX-mapping";
			case V34_DATA_PREPARATION_TRANSMIT_LINE:
				return "phase4/calibrate-TX-line";
			case V34_DATA_PREPARATION_RECEIVE_MAPPING:
				return "phase4/calibrate-RX-mapping";
			case V34_DATA_PREPARATION_COMPLETE:
				return "phase4/finish-calibration";
			}
		}
		if (call->state.phase4.receive_data_active)
			return "phase4/B1-data-validation";
		if (call->state.phase4.peer_mp_prime_received)
			return "phase4/E";
		if (call->state.phase4.peer_mp_received)
			return "phase4/MP-prime";
		return call->state.phase4.transmit_mp_started ? "phase4/MP" : "phase4/S-TRN";
	}
	return "V.34/unknown";
}

static enum v34_station_role station_role(bool caller)
{
	return caller ? V34_CALLING_STATION : V34_ANSWERING_STATION;
}

static enum v34_profile_result fallback_profile(
	struct v34_call_configuration *configuration, uint16_t *rate_mask,
	const struct v34_phase2_handoff *handoff)
{
	struct v34_phase3_profile caller;
	struct v34_phase3_profile answerer;
	uint8_t symmetric_rate_units;

	if (v34_phase2_handoff_profiles(handoff, &caller, &answerer) !=
	    V34_PHASE2_SELECTION_OK)
		return V34_PROFILE_INVALID_ARGUMENT;
	/* The current MP policy has one cap for both directions. Do not let it
	 * exceed either projection retained from the completed exchange. */
	symmetric_rate_units =
		caller.projected_data_rate_2400_bps_units <
				answerer.projected_data_rate_2400_bps_units ?
			caller.projected_data_rate_2400_bps_units :
			answerer.projected_data_rate_2400_bps_units;
	return v34_profile_init(
		configuration, rate_mask, handoff->local_role,
		V34_MIN_BIT_RATE,
		(unsigned int)symmetric_rate_units * V34_RATE_STEP, 0);
}

static enum v34_profile_result v8_fallback_profile(
	struct v34_call_configuration *configuration, uint16_t *rate_mask,
	const struct modem *modem, enum v34_station_role role)
{
	unsigned int minimum = modem->min_rate;
	unsigned int maximum = modem->max_rate;

	/* +MS V.90 downstream rates do not share V.34's 2400-bit/s grid.
	 * Translate both bounds downward when V.8 selects conventional V.34. */
	if (maximum > V34_MAX_BIT_RATE)
		maximum = V34_MAX_BIT_RATE;
	maximum -= maximum % V34_RATE_STEP;
	if (maximum < V34_MIN_BIT_RATE)
		return V34_PROFILE_UNSUPPORTED_RATE;
	if (minimum > V34_MAX_BIT_RATE)
		minimum = V34_MAX_BIT_RATE;
	minimum -= minimum % V34_RATE_STEP;
	if (minimum < V34_MIN_BIT_RATE)
		minimum = V34_MIN_BIT_RATE;
	if (minimum > maximum)
		minimum = maximum;
	return v34_profile_init(
		configuration, rate_mask, role, minimum, maximum,
		modem->v34_symbol_rate);
}

static struct v34_datapump *v34_from_dp(struct dp *dp)
{
	struct v34_datapump *datapump;
	bool linked;

	if (dp == NULL || dp->id != DP_V34 || dp->op != &v34_operations ||
	    dp->modem == NULL || dp->dp_data == NULL)
		return NULL;
	datapump = dp->dp_data;
	linked = v34_call_is_online(&datapump->call) ||
		 datapump->call.stage == V34_CALL_CLEARDOWN;
	if (&datapump->dp != dp || datapump->rate_mask == 0 ||
	    !v34_data_quality_is_valid(&datapump->data_quality) ||
	    !training_session_is_valid(&datapump->training) ||
	    datapump->missing_carrier_samples > V34_CARRIER_LOSS_SAMPLES ||
	    datapump->carrier_power_floor == 0u ||
	    datapump->carrier_power_floor > DATAPUMP_MIN_QAM_LINE_POWER ||
	    datapump->carrier_recovery_samples >
		    V34_CARRIER_RECOVERY_SAMPLES ||
	    !v34_call_is_valid(&datapump->call) ||
	    datapump->call.local_role != station_role(datapump->caller) ||
	    (datapump->pcm_retrain_pending &&
	     (datapump->pcm_restart == NULL ||
	      datapump->call.stage != V34_CALL_RETRAIN_SILENCE)) ||
	    (v34_call_has_failed(&datapump->call) &&
	     !training_session_has_failed(&datapump->training)) ||
	    (!training_session_has_failed(&datapump->training) &&
	     training_session_is_online(&datapump->training) != linked &&
	     !(training_session_is_retraining(&datapump->training) &&
	       !linked)) ||
	    (!training_session_has_failed(&datapump->training) &&
	     linked &&
	     dp->status != DPSTAT_CONNECT) ||
	    (!training_session_has_failed(&datapump->training) &&
	     !linked &&
	     dp->status == DPSTAT_CONNECT &&
	     !training_session_is_retraining(&datapump->training)))
		return NULL;
	return datapump;
}

bool v34_datapump_pcm_retrain_handoff(
	struct dp *dp, struct pcm_call *call, struct training_session *training)
{
	struct v34_datapump *datapump = v34_from_dp(dp);

	if (datapump == NULL || call == NULL || training == NULL ||
	    !datapump->pcm_retrain_pending ||
	    memory_regions_overlap(datapump, sizeof(*datapump), call, sizeof(*call)) ||
	    memory_regions_overlap(
		    datapump, sizeof(*datapump), training, sizeof(*training)) ||
	    memory_regions_overlap(call, sizeof(*call), training, sizeof(*training)) ||
	    pcm_call_init_from_v34(
		    call, datapump->pcm_restart, &datapump->call.retrain) !=
		    PCM_CALL_OK)
		return false;
	*training = datapump->training;
	return true;
}

static struct dp *create_v34(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, const struct dp_operations *operations)
{
	struct v34_call_configuration configuration;
	struct v34_datapump *datapump;
	struct pcm_v34_fallback_handoff fallback;
	enum v34_profile_result profile_result;
	enum v34_station_role role;
	bool from_pcm;
	bool from_v8_fallback;
	uint16_t rate_mask;

	from_pcm = modem != NULL && modem->dp != NULL &&
		v90_datapump_v34_fallback_handoff(modem->dp, &fallback);
	from_v8_fallback = modem != NULL && modem->dp != NULL &&
		v8_datapump_selected_v34_fallback(modem->dp);
	role = from_pcm ? fallback.phase2.local_role :
		station_role(caller != 0);
	if (modem == NULL || id != DP_V34 || (caller != 0 && caller != 1) ||
	    sample_rate != MODEM_RATE || max_fragment != MODEM_FRAG ||
	    operations != &v34_operations)
		return NULL;
	if (from_pcm)
		profile_result = fallback_profile(
			&configuration, &rate_mask, &fallback.phase2);
	else if (from_v8_fallback)
		profile_result = v8_fallback_profile(
			&configuration, &rate_mask, modem, role);
	else
		profile_result = v34_profile_init(
			&configuration, &rate_mask, role, modem->min_rate,
			modem->max_rate, modem->v34_symbol_rate);
	if (profile_result != V34_PROFILE_OK)
		return NULL;
	datapump = calloc(1, sizeof(*datapump));
	if (datapump == NULL)
		return NULL;
	if ((from_pcm ?
	     v34_call_init_from_phase2(
		     &datapump->call, &configuration, &fallback.phase2) :
	     v34_call_init(&datapump->call, &configuration)) != V34_CALL_OK) {
		free(datapump);
		return NULL;
	}
	if (v34_data_quality_init(&datapump->data_quality) !=
	    V34_DATA_QUALITY_OK) {
		free(datapump);
		return NULL;
	}
	datapump->carrier_power_floor = DATAPUMP_MIN_QAM_LINE_POWER;
	datapump->caller = role == V34_CALLING_STATION;
	datapump->rate_mask = rate_mask;
	if (from_pcm)
		datapump->training = fallback.training;
	else if (training_session_init(
			 &datapump->training,
			 datapump_carrier_wait_ticks(modem)) !=
		 TRAINING_SESSION_OK) {
		free(datapump);
		return NULL;
	}
	if (from_pcm) {
		datapump->pcm_restart = malloc(sizeof(*datapump->pcm_restart));
		if (datapump->pcm_restart == NULL) {
			free(datapump);
			return NULL;
		}
		*datapump->pcm_restart = fallback.restart;
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

static int delete_v34(struct dp *dp)
{
	struct v34_datapump *datapump = v34_from_dp(dp);

	if (datapump == NULL)
		return -1;
	datapump->dp.dp_data = NULL;
	free(datapump->pcm_restart);
	free(datapump);
	return 0;
}

static int start_local_retrain(struct v34_datapump *datapump)
{
	struct training_session training;
	struct v34_call call;

	if (!training_session_is_online(&datapump->training))
		return -1;
	training = datapump->training;
	call = datapump->call;
	if (v34_call_start_retrain(&call) != V34_CALL_OK ||
	    training_session_start_retrain(
		    &training, TRAINING_LOCAL_RETRAIN) !=
		    TRAINING_SESSION_OK)
		return -1;
	datapump->call = call;
	datapump->training = training;
	datapump->dp.status = DPSTAT_OK;
	datapump->missing_carrier_samples = 0;
	datapump->carrier_recovery_samples = 0;
	datapump->retrain_carrier_acquired = false;
	return 0;
}

static int retrain_v34(struct dp *dp)
{
	struct v34_datapump *datapump = v34_from_dp(dp);

	if (datapump == NULL)
		return -1;
	/* A remote or quality-triggered full retrain may begin during the guard
	 * interval before ATO1 reaches the datapump. Join that same transaction;
	 * the modem wrapper will report CONNECT when this generation completes. */
	if (training_session_is_retraining(&datapump->training))
		return 0;
	return start_local_retrain(datapump);
}

static int hangup_v34(struct dp *dp)
{
	struct v34_datapump *datapump = v34_from_dp(dp);

	return datapump != NULL &&
	       v34_call_start_cleardown(&datapump->call) == V34_CALL_OK ? 0 : -1;
}

static bool source_mapping_frame(
	void *context, const struct v34_mapping_frame *request,
	uint8_t *primary_bits, uint8_t *auxiliary_bit)
{
	struct v34_datapump *datapump = context;

	if (datapump == NULL || request == NULL ||
	    request->primary_bits > V34_MAX_MAPPING_FRAME_BITS)
		return false;
	return datapump_source_split_bits(
		&datapump->dp,
		training_session_is_online(&datapump->training),
		primary_bits, request->primary_bits, auxiliary_bit);
}

static bool deliver_mapping_frame(
	struct v34_datapump *datapump,
	const struct v34_received_mapping_frame *received)
{
	if (received == NULL ||
	    received->primary_bit_count > V34_MAX_MAPPING_FRAME_BITS)
		return false;
	return datapump_deliver_bits(
		&datapump->dp, training_session_is_online(&datapump->training),
		received->primary_bits, received->primary_bit_count);
}

static bool enter_link(struct v34_datapump *datapump)
{
	struct v34_data_startup startup;
	struct v34_data_quality data_quality;
	const struct v34_adaptive_equalizer_quality *measurement;
	unsigned int transmit_rate;
	unsigned int receive_rate;

	if (!v34_call_get_data_startup(&datapump->call, &startup))
		return false;
	measurement = &datapump->call.state.phase4.receive.line.equalizer.quality;
	data_quality = datapump->data_quality;
	if (v34_data_quality_start(
		    &data_quality, startup.receive.geometry.symbol_rate,
		    startup.receive.geometry.primary_data_rate_2400_bps_units,
		    measurement, 2) != V34_DATA_QUALITY_OK)
		return false;
	transmit_rate =
		(unsigned int)startup.mode.transmit
			.primary_data_rate_2400_bps_units * V34_RATE_STEP;
	receive_rate =
		(unsigned int)startup.mode.receive
			.primary_data_rate_2400_bps_units * V34_RATE_STEP;
	if (!datapump_set_link_rates(
		    &datapump->dp, transmit_rate, receive_rate))
		return false;
	/* Apply the trained Q10 gain to the carrier test as well as decoding.
	 * Retain this floor through retraining, where the new gain is not ready.
	 * Never raise the existing floor for stronger signals. */
	uint32_t gain = (uint32_t)
		datapump->call.state.phase4.receive.line.timing.gain.gain;
	uint32_t unity = 1u << V34_GAIN_FRACTION_BITS;

	datapump->carrier_power_floor = DATAPUMP_MIN_QAM_LINE_POWER;
	if (gain > unity)
		datapump->carrier_power_floor = (uint32_t)(
			(uint64_t)DATAPUMP_MIN_QAM_LINE_POWER * unity * unity /
			(gain * gain));
	datapump->data_quality = data_quality;
	datapump->missing_carrier_samples = 0;
	datapump->carrier_recovery_samples = 0;
	return datapump_complete_training(&datapump->dp, &datapump->training);
}

static bool observation_has_progress(
	const struct v34_call_observation *observation)
{
	if (observation->processed != observation->next)
		return true;
	switch (observation->processed) {
	case V34_CALL_PHASE2:
		return observation->events.phase2.kind !=
		       V34_PHASE2_EVENT_NONE;
	case V34_CALL_PHASE3:
		return observation->events.phase3 != 0;
	case V34_CALL_PHASE4:
		return observation->events.phase4 != 0;
	case V34_CALL_RETRAIN_SILENCE:
	case V34_CALL_CLEARDOWN:
		return false;
	}
	return false;
}

static void fail_training(struct v34_datapump *datapump)
{
	(void)training_session_fail(&datapump->training);
}

static bool observe_online_quality(struct v34_datapump *datapump)
{
	struct v34_data_quality next = datapump->data_quality;
	const struct v34_adaptive_equalizer_quality *measurement =
		&datapump->call.state.phase4.receive.line.equalizer.quality;
	enum v34_data_quality_result result =
		v34_data_quality_observe(&next, measurement);

	if (result == V34_DATA_QUALITY_FREEZE_TIMING ||
	    result == V34_DATA_QUALITY_RESUME_TIMING) {
		if (v34_phase34_receiver_set_data_timing_frozen(
			    &datapump->call.state.phase4.receive,
			    result == V34_DATA_QUALITY_FREEZE_TIMING) !=
		    V34_PHASE34_RECEIVER_OK)
			return false;
	} else if (result == V34_DATA_QUALITY_RETRAIN) {
		if (start_local_retrain(datapump) != 0)
			return false;
	} else if (result != V34_DATA_QUALITY_OK) {
		return false;
	}
	datapump->data_quality = next;
	return true;
}

static bool data_link_is_online(const struct v34_datapump *datapump)
{
	const struct modem *modem = datapump->dp.modem;

	return modem != NULL && modem->get_chars != NULL &&
	       modem->put_chars != NULL;
}

static bool fragment_has_carrier(
	const int16_t *samples, int count, uint32_t power_floor)
{
	int offset;

	if (count <= V34_CARRIER_WINDOW_SAMPLES)
		return datapump_qam_line_power_above(samples, count, power_floor);
	for (offset = 0;
	     offset + V34_CARRIER_WINDOW_SAMPLES <= count;
	     offset += V34_CARRIER_WINDOW_SAMPLES)
		if (!datapump_qam_line_power_above(
			    samples + offset, V34_CARRIER_WINDOW_SAMPLES, power_floor))
			return false;
	/* Also cover a short tail without making a sub-window zero crossing its
	 * own carrier decision. */
	if (offset != count &&
	    !datapump_qam_line_power_above(
		    samples + count - V34_CARRIER_WINDOW_SAMPLES,
		    V34_CARRIER_WINDOW_SAMPLES, power_floor))
		return false;
	return true;
}

static bool set_data_erasure(
	struct v34_datapump *datapump, bool erasure)
{
	return v34_phase34_receiver_set_data_erasure(
		       &datapump->call.state.phase4.receive, erasure) ==
	       V34_PHASE34_RECEIVER_OK;
}

static int process_v34(
	struct dp *dp, void *receive_buffer, void *transmit_buffer, int count)
{
	struct v34_datapump *datapump = v34_from_dp(dp);
	const int16_t *receive = receive_buffer;
	int16_t *transmit = transmit_buffer;
	bool connected = false;
	bool carrier_present;
	bool carrier_erased = false;

	if (!datapump_linear_fragment_is_ready(datapump, receive, transmit, count))
		return DPSTAT_ERROR;
	if (training_session_has_failed(&datapump->training)) {
		memset(transmit, 0, (size_t)count * sizeof(*transmit));
		return DPSTAT_ERROR;
	}
	if (datapump->call.stage == V34_CALL_CLEARDOWN) {
		for (int index = 0; index < count; ++index) {
			struct v34_call_observation observation;
			struct v34_received_mapping_frame ignored;
			enum v34_call_result result = v34_call_sample(
				&datapump->call, receive[index], NULL, NULL,
				&transmit[index], &observation, &ignored);

			if (result == V34_CALL_CLEARDOWN_COMPLETE) {
				memset(transmit + index + 1, 0,
				       (size_t)(count - index - 1) * sizeof(*transmit));
				return DPSTAT_DISCONNECT;
			}
			if (result != V34_CALL_OK) {
				fail_training(datapump);
				memset(transmit, 0, (size_t)count * sizeof(*transmit));
				return DPSTAT_ERROR;
			}
		}
		return DPSTAT_OK;
	}
	carrier_present = fragment_has_carrier(
		receive, count, datapump->carrier_power_floor);
	if (training_session_is_online(&datapump->training)) {
		carrier_erased =
			datapump->call.state.phase4.receive.data_erasure;
		if (!carrier_present) {
			datapump->carrier_recovery_samples = 0;
			datapump_add_samples_bounded(
				&datapump->missing_carrier_samples,
				(uint32_t)(unsigned int)count,
				V34_CARRIER_LOSS_SAMPLES);
			carrier_erased = true;
		} else if (datapump->missing_carrier_samples != 0u) {
			datapump->missing_carrier_samples = 0;
			datapump->carrier_recovery_samples =
				V34_CARRIER_RECOVERY_SAMPLES;
			carrier_erased = true;
		} else if (datapump->carrier_recovery_samples != 0u) {
			carrier_erased = true;
		}
		if (carrier_erased &&
		    !datapump->call.state.phase4.receive.data_erasure &&
		    !set_data_erasure(datapump, true)) {
			fail_training(datapump);
			memset(transmit, 0, (size_t)count * sizeof(*transmit));
			return DPSTAT_ERROR;
		}
		if (carrier_present &&
		    datapump->carrier_recovery_samples != 0u) {
			if ((unsigned int)count >=
			    datapump->carrier_recovery_samples) {
				datapump->carrier_recovery_samples = 0;
			} else {
				datapump->carrier_recovery_samples = (uint16_t)(
					datapump->carrier_recovery_samples -
					(unsigned int)count);
			}
		}
	}
	if (!training_session_is_online(&datapump->training)) {
		if (training_session_advance(
			    &datapump->training,
			    (uint64_t)(unsigned int)count) !=
		    TRAINING_SESSION_OK) {
			modem_note_disconnect(dp->modem, true, "V.34 training deadline expired");
			memset(transmit, 0, (size_t)count * sizeof(*transmit));
			return DPSTAT_ERROR;
		}
	}
	for (int index = 0; index < count; ++index) {
		struct v34_call_observation observation;
		struct v34_received_mapping_frame received;
		enum v34_call_result result = v34_call_sample(
			&datapump->call, receive[index], source_mapping_frame,
			datapump, &transmit[index], &observation, &received);

		if (result != V34_CALL_OK) {
			modem_note_disconnect(dp->modem, true,
				result == V34_CALL_NO_COMMON_RATE ? "no common V.34 rate" :
				result == V34_CALL_SEQUENCE_MISMATCH ? "V.34 sequence mismatch" :
				result == V34_CALL_NUMERIC_FAILURE ? "V.34 numeric failure" : "V.34 protocol failed");
			fail_training(datapump);
			memset(transmit, 0, (size_t)count * sizeof(*transmit));
			return DPSTAT_ERROR;
		}
		if (training_session_is_online(&datapump->training) &&
		    ((observation.processed != V34_CALL_RETRAIN_SILENCE &&
		      observation.next == V34_CALL_RETRAIN_SILENCE) ||
		     (observation.processed == V34_CALL_PHASE4 &&
		      (observation.events.phase4 &
		       V34_PHASE4_EXCHANGE_EVENT_RATE_RENEGOTIATION) != 0))) {
			if (training_session_start_retrain(
				    &datapump->training,
				    TRAINING_REMOTE_RETRAIN) !=
			    TRAINING_SESSION_OK) {
				fail_training(datapump);
				memset(transmit, 0,
				       (size_t)count * sizeof(*transmit));
				return DPSTAT_ERROR;
			}
			datapump->retrain_carrier_acquired = false;
			datapump->dp.status = DPSTAT_OK;
		}
		if (!training_session_is_online(&datapump->training) &&
		    observation_has_progress(&observation) &&
		    training_session_note_progress(&datapump->training) !=
			    TRAINING_SESSION_OK) {
			fail_training(datapump);
			memset(transmit, 0, (size_t)count * sizeof(*transmit));
			return DPSTAT_ERROR;
		}
		if (!training_session_is_online(&datapump->training) &&
		    v34_call_is_online(&datapump->call)) {
			if (!enter_link(datapump)) {
				fail_training(datapump);
				memset(transmit, 0,
				       (size_t)count * sizeof(*transmit));
				return DPSTAT_ERROR;
			}
			connected = true;
		}
		if (observation.processed == V34_CALL_PHASE4 &&
		    (observation.events.phase4 &
		     V34_PHASE4_EXCHANGE_EVENT_MAPPING_FRAME_RECEIVED) != 0 &&
		    !carrier_erased &&
		    !deliver_mapping_frame(datapump, &received)) {
			fail_training(datapump);
			memset(transmit, 0, (size_t)count * sizeof(*transmit));
			return DPSTAT_ERROR;
		}
		if (training_session_is_online(&datapump->training) &&
		    data_link_is_online(datapump) &&
		    !carrier_erased &&
		    !observe_online_quality(datapump)) {
			fail_training(datapump);
			memset(transmit, 0, (size_t)count * sizeof(*transmit));
			return DPSTAT_ERROR;
		}
	}
	/* A qualified silence prefix remains an active remote-retrain candidate
	 * throughout the following tone guard.  Do not leak that tone as data or
	 * resume semantic state merely because line power has returned. */
	if (training_session_is_online(&datapump->training) &&
	    datapump->call.state.phase4.receive.data_erasure &&
	    carrier_present && datapump->carrier_recovery_samples == 0u &&
	    datapump->call.retrain.detector.silence_samples == 0u &&
	    !set_data_erasure(datapump, false)) {
		fail_training(datapump);
		return DPSTAT_ERROR;
	}
	if (training_session_is_retraining(&datapump->training) &&
	    !datapump->retrain_carrier_acquired && carrier_present) {
		/* Once the peer has answered a full retrain, later half-duplex
		 * training gaps are not carrier loss.  The training-session deadline
		 * still bounds a peer that responds but never completes. */
		datapump->retrain_carrier_acquired = true;
		datapump->missing_carrier_samples = 0;
	} else if (training_session_is_retraining(&datapump->training) &&
		   !datapump->retrain_carrier_acquired) {
		datapump_add_samples_bounded(
			&datapump->missing_carrier_samples,
			(uint32_t)(unsigned int)count,
			V34_CARRIER_LOSS_SAMPLES);
	}
	if (datapump->pcm_restart != NULL &&
	    datapump->call.stage == V34_CALL_RETRAIN_SILENCE) {
		/* The first media block already consumed part of the shared 70 ms
		 * response. Transfer that exact epoch into the agreed PCM Phase 2. */
		datapump->dp.modem->dp_requested =
			datapump->pcm_restart->protocol == PCM_PROTOCOL_V92 ? DP_V92 : DP_V90;
		datapump->pcm_retrain_pending = true;
		return DPSTAT_CHANGEDP;
	}
	if (connected)
		return DPSTAT_CONNECT;
	if (datapump->missing_carrier_samples >= V34_CARRIER_LOSS_SAMPLES) {
		modem_note_disconnect(dp->modem, true, "V.34 carrier lost");
		fail_training(datapump);
		return DPSTAT_ERROR;
	}
	return DPSTAT_OK;
}

const struct dp_operations *v34_datapump_operations(enum DP_ID id)
{
	return id == DP_V34 ? &v34_operations : NULL;
}

static int tx_tail_samples(const struct dp *dp)
{
	const struct v34_datapump *p = dp != NULL ? dp->dp_data : NULL;
	return p != NULL && training_session_is_online(&p->training) &&
		p->call.stage == V34_CALL_PHASE4 && p->call.state.phase4.transmit_data_active ?
		v34_data_stream_tx_tail_samples(&p->call.state.phase4.transmit.data) : -1;
}
