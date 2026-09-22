// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_downstream_tx.h"

#include "memory_region.h"
#include "packed_bits.h"

#include <string.h>

enum { CLEARDOWN_RECORDS = 2 };

static bool stage_is_valid(enum pcm_downstream_tx_stage stage)
{
	return stage == PCM_DOWNSTREAM_TX_TRN2D ||
	       stage == PCM_DOWNSTREAM_TX_MP ||
	       stage == PCM_DOWNSTREAM_TX_MP_PRIME ||
	       stage == PCM_DOWNSTREAM_TX_ED ||
	       stage == PCM_DOWNSTREAM_TX_V92_CONTROL;
}

static bool stage_matches_protocol(
	const struct pcm_downstream_tx *transmitter)
{
	if (transmitter->stage == PCM_DOWNSTREAM_TX_TRN2D)
		return true;
	if (transmitter->stage == PCM_DOWNSTREAM_TX_ED)
		return true;
	if (transmitter->protocol == PCM_PROTOCOL_V92)
		return transmitter->stage == PCM_DOWNSTREAM_TX_V92_CONTROL;
	return transmitter->stage == PCM_DOWNSTREAM_TX_MP ||
	       transmitter->stage == PCM_DOWNSTREAM_TX_MP_PRIME;
}

static bool record_requests_cleardown(const struct v90_mp_record *mp)
{
	const struct v90_mp_parameters *common = v90_mp_record_common(mp);

	return common->maximum_upstream_rate_2400_bps_units == 0u;
}

static bool requests_cleardown(const struct pcm_downstream_tx *transmitter)
{
	return transmitter->control_configured &&
	       transmitter->protocol == PCM_PROTOCOL_V90 &&
	       record_requests_cleardown(&transmitter->control.mp);
}

static bool v92_cleardown_can_finish(const struct pcm_downstream_tx *transmitter)
{
	return transmitter->protocol == PCM_PROTOCOL_V92 &&
	       transmitter->stage == PCM_DOWNSTREAM_TX_V92_CONTROL &&
	       transmitter->control_configured &&
	       transmitter->v92_control_kind == PCM_DOWNSTREAM_V92_CONTROL_CPD &&
	       transmitter->control.cpd.data_rate_number == 0u &&
	       transmitter->v92_control_record_accepted &&
	       transmitter->control_bit_index == 0u;
}

static uint64_t initial_training_frames(const struct pcm_downstream_tx *transmitter)
{
	if (transmitter->protocol == PCM_PROTOCOL_V92)
		return V90_PHASE4_TRN2D_MINIMUM_FRAMES;
	/* A validated CP requests MP-prime. Until then, allow the analogue
	 * receiver longer to condition, without waiting indefinitely for CP.
	 * Cleardown does not need that extra data-path conditioning. */
	return transmitter->mp_prime_requested || requests_cleardown(transmitter) ?
		V90_PHASE4_TRN2D_INITIAL_FRAMES : V90_PHASE4_TRN2D_WAIT_CP_FRAMES;
}

static bool encode_mp_sequence(
	const struct v90_mp_record *base, uint8_t frame_bits,
	bool acknowledges_cp, uint8_t *sequence, size_t *sequence_bits)
{
	struct v90_mp_record record = *base;
	uint8_t encoded[V90_MP_MAXIMUM_SEQUENCE_BYTES] = { 0 };
	size_t bit_count = 0;

	v90_mp_record_mutable_common(&record)->acknowledges_cp = acknowledges_cp;
	if (v90_mp_encode(
		    &record, frame_bits, encoded, sizeof(encoded),
		    &bit_count) != V90_MP_OK ||
	    bit_count == 0 || bit_count % frame_bits != 0)
		return false;
	memset(sequence, 0, V90_MP_MAXIMUM_SEQUENCE_BYTES);
	memcpy(sequence, encoded, sizeof(encoded));
	*sequence_bits = bit_count;
	return true;
}

static bool encode_suvd_sequence(
	const struct v92_suvd_parameters *parameters, uint8_t frame_bits,
	uint8_t *sequence, size_t *sequence_bits)
{
	uint8_t encoded[V92_SUVD_MAX_FRAME_BYTES] = { 0 };
	size_t bit_count = 0u;

	if (v92_suvd_encode(
		    parameters, frame_bits, encoded, sizeof(encoded),
		    &bit_count) != V92_SUVD_OK ||
	    bit_count == 0u || bit_count % frame_bits != 0u)
		return false;
	memset(sequence, 0, V92_SUVD_MAX_FRAME_BYTES);
	memcpy(sequence, encoded, sizeof(encoded));
	*sequence_bits = bit_count;
	return true;
}

static bool encode_cpd_sequence(
	const struct v92_cpd_base_parameters *parameters, uint8_t frame_bits,
	uint8_t *sequence, size_t *sequence_bits)
{
	size_t bit_count = 0u;

	memset(sequence, 0, V92_CPD_BASE_MAX_FRAME_BYTES);
	if (v92_cpd_encode_base(
		    parameters, frame_bits, sequence,
		    V92_CPD_BASE_MAX_FRAME_BYTES, &bit_count) != V92_CPD_OK ||
	    bit_count == 0u || bit_count % frame_bits != 0u)
		return false;
	*sequence_bits = bit_count;
	return true;
}

static bool stored_sequence_matches(
	const struct pcm_downstream_tx *transmitter,
	const uint8_t *expected, size_t expected_capacity,
	size_t expected_bits)
{
	size_t expected_bytes = (expected_bits + 7u) / 8u;

	return expected_bytes <= expected_capacity &&
	       expected_bytes <= sizeof(transmitter->control_sequence) &&
	       transmitter->control_sequence_bits == expected_bits &&
	       memcmp(
		       transmitter->control_sequence, expected,
		       expected_bytes) == 0 &&
	       memory_region_is_zero(
		       transmitter->control_sequence + expected_bytes,
		       PCM_DOWNSTREAM_TX_CONTROL_BYTES - expected_bytes);
}

static bool store_sequence(
	struct pcm_downstream_tx *transmitter,
	const uint8_t *sequence, size_t sequence_capacity,
	size_t sequence_bits)
{
	size_t sequence_bytes = (sequence_bits + 7u) / 8u;

	if (sequence_bits == 0u || sequence_bytes > sequence_capacity ||
	    sequence_bytes > sizeof(transmitter->control_sequence))
		return false;
	memset(
		transmitter->control_sequence, 0,
		sizeof(transmitter->control_sequence));
	memcpy(transmitter->control_sequence, sequence, sequence_bytes);
	transmitter->control_sequence_bits = sequence_bits;
	return true;
}

static bool unconfigured_control_is_valid(
	const struct pcm_downstream_tx *transmitter)
{
	static const union pcm_downstream_control_tx empty_control;
	static const uint8_t empty_sequence[PCM_DOWNSTREAM_TX_CONTROL_BYTES];

	return transmitter->stage == PCM_DOWNSTREAM_TX_TRN2D &&
	       transmitter->v92_control_kind ==
		       PCM_DOWNSTREAM_V92_CONTROL_NONE &&
	       !transmitter->mp_prime_requested &&
	       !transmitter->mp_prime_record_accepted &&
	       !transmitter->v92_control_record_accepted &&
	       !transmitter->ed_requested && transmitter->ed_frames == 0 &&
	       transmitter->control_sequence_bits == 0 &&
	       transmitter->control_bit_index == 0 &&
	       memcmp(&transmitter->control, &empty_control,
		      sizeof(empty_control)) == 0 &&
	       memcmp(transmitter->control_sequence, empty_sequence,
		      sizeof(empty_sequence)) == 0;
}

static bool v90_control_is_valid(
	const struct pcm_downstream_tx *transmitter)
{
	uint8_t expected[V90_MP_MAXIMUM_SEQUENCE_BYTES] = { 0 };
	size_t expected_bits = 0;
	bool prime;

	if (transmitter->v92_control_kind !=
		    PCM_DOWNSTREAM_V92_CONTROL_NONE ||
	    transmitter->v92_control_record_accepted ||
	    !v90_mp_record_is_initial(&transmitter->control.mp) ||
	    transmitter->control_sequence_bits == 0 ||
	    transmitter->control_bit_index >=
		    transmitter->control_sequence_bits ||
	    transmitter->control_bit_index %
		    transmitter->encoder.mapping.frame_bits != 0 ||
	    (transmitter->stage == PCM_DOWNSTREAM_TX_TRN2D &&
	     transmitter->control_bit_index != 0) ||
	    (transmitter->stage != PCM_DOWNSTREAM_TX_TRN2D &&
	     transmitter->accepted_frames <=
		     V90_PHASE4_TRN2D_INITIAL_FRAMES) ||
	    (transmitter->stage == PCM_DOWNSTREAM_TX_MP_PRIME &&
	     !transmitter->mp_prime_requested) ||
	    (transmitter->stage == PCM_DOWNSTREAM_TX_MP &&
	     transmitter->mp_prime_requested &&
	     transmitter->control_bit_index == 0) ||
	    (transmitter->mp_prime_record_accepted &&
	     (!transmitter->mp_prime_requested ||
	      (transmitter->stage != PCM_DOWNSTREAM_TX_MP_PRIME &&
	       transmitter->stage != PCM_DOWNSTREAM_TX_ED))) ||
	    (transmitter->ed_requested &&
	     !transmitter->mp_prime_requested))
		return false;
	if (transmitter->stage == PCM_DOWNSTREAM_TX_ED) {
		if (!transmitter->ed_requested ||
		    !transmitter->mp_prime_record_accepted ||
		    transmitter->control_bit_index != 0 ||
		    transmitter->ed_frames > V90_PHASE4_ED_FRAMES ||
		    transmitter->finish_requested !=
			    (transmitter->ed_frames == V90_PHASE4_ED_FRAMES) ||
		    (transmitter->complete &&
		     !transmitter->finish_requested))
			return false;
	} else if (requests_cleardown(transmitter)) {
		if (transmitter->ed_requested || transmitter->ed_frames != 0u ||
		    transmitter->mp_prime_requested ||
		    transmitter->cleardown_records > CLEARDOWN_RECORDS ||
		    transmitter->finish_requested !=
			    (transmitter->cleardown_records == CLEARDOWN_RECORDS) ||
		    (transmitter->finish_requested &&
		     transmitter->control_bit_index != 0u) ||
		    (transmitter->complete && !transmitter->finish_requested))
			return false;
	} else if (transmitter->ed_frames != 0 ||
		   transmitter->finish_requested || transmitter->complete ||
		   (transmitter->stage == PCM_DOWNSTREAM_TX_MP_PRIME &&
		    transmitter->ed_requested &&
		    transmitter->mp_prime_record_accepted &&
		    transmitter->control_bit_index == 0)) {
		return false;
	}
	prime = transmitter->stage == PCM_DOWNSTREAM_TX_MP_PRIME ||
		transmitter->stage == PCM_DOWNSTREAM_TX_ED;
	return encode_mp_sequence(
		       &transmitter->control.mp,
		       transmitter->encoder.mapping.frame_bits, prime,
		       expected, &expected_bits) &&
	       stored_sequence_matches(
		       transmitter, expected, sizeof(expected), expected_bits);
}

static bool v92_control_is_valid(
	const struct pcm_downstream_tx *transmitter)
{
	uint8_t expected[V92_CPD_BASE_MAX_FRAME_BYTES] = { 0 };
	size_t expected_bits = 0u;

	if (transmitter->stage != PCM_DOWNSTREAM_TX_TRN2D &&
	    transmitter->stage != PCM_DOWNSTREAM_TX_V92_CONTROL &&
	    transmitter->stage != PCM_DOWNSTREAM_TX_ED)
		return false;
	if (transmitter->v92_control_kind !=
		    PCM_DOWNSTREAM_V92_CONTROL_SUVD &&
	    transmitter->v92_control_kind !=
		    PCM_DOWNSTREAM_V92_CONTROL_CPD)
		return false;
	if (transmitter->control_sequence_bits == 0u ||
	    transmitter->control_bit_index >=
		    transmitter->control_sequence_bits ||
	    transmitter->control_bit_index %
		    transmitter->encoder.mapping.frame_bits != 0u ||
	    (transmitter->stage == PCM_DOWNSTREAM_TX_TRN2D &&
	     transmitter->control_bit_index != 0u) ||
	    ((transmitter->stage == PCM_DOWNSTREAM_TX_V92_CONTROL ||
	      transmitter->stage == PCM_DOWNSTREAM_TX_ED) &&
	     transmitter->encoder.mapping.kind == V90_CP_TRAINING &&
	     transmitter->accepted_frames <=
		     V90_PHASE4_TRN2D_MINIMUM_FRAMES) ||
	    transmitter->mp_prime_requested ||
	    transmitter->mp_prime_record_accepted ||
	    (transmitter->stage == PCM_DOWNSTREAM_TX_TRN2D &&
	     transmitter->v92_control_record_accepted))
		return false;
	if (transmitter->ed_requested &&
	    (transmitter->v92_control_kind !=
		     PCM_DOWNSTREAM_V92_CONTROL_SUVD ||
	     !transmitter->control.suvd.acknowledges_cpu))
		return false;
	if (transmitter->stage == PCM_DOWNSTREAM_TX_ED) {
		if (!transmitter->ed_requested ||
		    !transmitter->v92_control_record_accepted ||
		    transmitter->control_bit_index != 0u ||
		    transmitter->ed_frames > V90_PHASE4_ED_FRAMES ||
		    transmitter->finish_requested !=
			    (transmitter->ed_frames == V90_PHASE4_ED_FRAMES) ||
		    (transmitter->complete &&
		     !transmitter->finish_requested))
			return false;
	} else if (transmitter->ed_frames != 0u ||
		   (transmitter->finish_requested && !v92_cleardown_can_finish(transmitter)) ||
		   (transmitter->complete && !transmitter->finish_requested) ||
		   (transmitter->ed_requested &&
		    transmitter->control_bit_index == 0u)) {
		return false;
	}
	if (transmitter->v92_control_kind ==
	    PCM_DOWNSTREAM_V92_CONTROL_SUVD) {
		return v92_suvd_parameters_are_valid(
			       &transmitter->control.suvd) &&
		       encode_suvd_sequence(
			       &transmitter->control.suvd,
			       transmitter->encoder.mapping.frame_bits,
			       expected, &expected_bits) &&
		       stored_sequence_matches(
			       transmitter, expected, sizeof(expected),
			       expected_bits);
	}
	return v92_cpd_base_parameters_are_valid(
		       &transmitter->control.cpd) &&
	       encode_cpd_sequence(
		       &transmitter->control.cpd,
		       transmitter->encoder.mapping.frame_bits,
		       expected, &expected_bits) &&
	       stored_sequence_matches(
		       transmitter, expected, sizeof(expected), expected_bits);
}

static bool control_state_is_valid(
	const struct pcm_downstream_tx *transmitter)
{
	if (!stage_is_valid(transmitter->stage) ||
	    !stage_matches_protocol(transmitter))
		return false;
	if (!transmitter->control_configured)
		return unconfigured_control_is_valid(transmitter);
	return transmitter->protocol == PCM_PROTOCOL_V90 ?
		       v90_control_is_valid(transmitter) :
		       v92_control_is_valid(transmitter);
}

static bool queued_symbol_is_zero(
	const struct pcm_downstream_symbol *symbol)
{
	return symbol->pcm.ucode == 0 && !symbol->pcm.positive &&
	       symbol->pcm.octet == 0 && symbol->pcm.sample == 0 &&
	       symbol->shaping_rule == V90_SPECTRAL_RULE_A &&
	       !symbol->shaping_rule_valid &&
	       !symbol->shaping_frame_start;
}

static bool queue_is_valid(const struct pcm_downstream_tx *transmitter)
{
	const struct v90_pcm_mapping *mapping =
		&transmitter->encoder.mapping;
	size_t shaping_length = mapping->sign_redundancy == 0 ?
				0 : V90_CP_FRAME_INTERVALS /
					    mapping->sign_redundancy;

	if (transmitter->queue_count > V90_CP_FRAME_INTERVALS)
		return false;
	for (size_t index = 0;
	     index < V90_CP_FRAME_INTERVALS; ++index) {
		const struct pcm_downstream_symbol *symbol =
			&transmitter->queue[index];

		if (index >= transmitter->queue_count) {
			if (!queued_symbol_is_zero(symbol))
				return false;
			continue;
		}
		if (!v90_pcm_symbol_is_valid(&symbol->pcm, mapping->law))
			return false;
		if (mapping->sign_redundancy == 0) {
			if (symbol->shaping_rule_valid ||
			    symbol->shaping_frame_start ||
			    symbol->shaping_rule != V90_SPECTRAL_RULE_A)
				return false;
		} else {
			uint64_t stream_index =
				transmitter->transmitted_symbols + index;

			if (!symbol->shaping_rule_valid ||
			    symbol->shaping_rule > V90_SPECTRAL_RULE_D ||
			    symbol->shaping_frame_start !=
				    (stream_index % shaping_length == 0) ||
			    (index != 0 && !symbol->shaping_frame_start &&
			     symbol->shaping_rule !=
				     transmitter->queue[index - 1u]
					     .shaping_rule))
				return false;
		}
	}
	return true;
}

static bool symbol_accounting_is_valid(
	const struct pcm_downstream_tx *transmitter)
{
	const struct v90_pcm_mapping *mapping =
		&transmitter->encoder.mapping;
	uint64_t accepted_symbols;
	uint64_t delayed_symbols = 0;

	if (transmitter->accepted_frames >
	    UINT64_MAX / V90_CP_FRAME_INTERVALS)
		return false;
	accepted_symbols = transmitter->accepted_frames *
			   V90_CP_FRAME_INTERVALS;
	if (mapping->sign_redundancy != 0) {
		delayed_symbols =
			(uint64_t)transmitter->encoder.shaper.pending_count *
			(V90_CP_FRAME_INTERVALS /
			 mapping->sign_redundancy);
	}
	return transmitter->transmitted_symbols <= accepted_symbols &&
	       delayed_symbols <= accepted_symbols -
				  transmitter->transmitted_symbols &&
	       transmitter->queue_count <=
		       accepted_symbols - transmitter->transmitted_symbols -
		       delayed_symbols &&
	       transmitter->transmitted_symbols +
		       transmitter->queue_count + delayed_symbols ==
		       accepted_symbols;
}

static bool source_record_is_valid(
	const struct pcm_downstream_source_record *record,
	unsigned int frame_bits)
{
	if (record == NULL || record->sequence == NULL || frame_bits == 0u ||
	    record->sequence_bits == 0u ||
	    record->sequence_bits % frame_bits != 0u)
		return false;
	if (record->sequence_bits / 8u +
		    (record->sequence_bits % 8u != 0u) >
	    record->sequence_capacity)
		return false;
	return record->complete ?
		       record->bit_index == record->sequence_bits :
		       record->bit_index < record->sequence_bits &&
			       record->bit_index % frame_bits == 0u;
}

bool pcm_downstream_tx_is_valid(const struct pcm_downstream_tx *transmitter)
{
	if (transmitter == NULL ||
	    !pcm_protocol_is_valid(transmitter->protocol) ||
	    !v90_pcm_frame_encoder_is_valid(&transmitter->encoder) ||
	    (transmitter->encoder.mapping.kind != V90_CP_TRAINING &&
	     (transmitter->protocol != PCM_PROTOCOL_V92 ||
	      transmitter->stage == PCM_DOWNSTREAM_TX_TRN2D ||
	      !transmitter->control_configured)) ||
	    !v34_scrambler_is_valid(&transmitter->scrambler) ||
	    transmitter->scrambler.polynomial != V34_POLYNOMIAL_GPC ||
	    (transmitter->encoder.finishing &&
	     !transmitter->finish_requested) ||
	    !control_state_is_valid(transmitter) ||
	    (transmitter->cleardown_records != 0u &&
	     !requests_cleardown(transmitter)) ||
	    !queue_is_valid(transmitter) ||
	    !symbol_accounting_is_valid(transmitter))
		return false;
	if (!transmitter->complete)
		return true;
	return transmitter->finish_requested &&
	       transmitter->encoder.finishing &&
	       transmitter->queue_count == 0 &&
	       (transmitter->encoder.mapping.sign_redundancy == 0 ||
		transmitter->encoder.shaper.pending_count == 0);
}

enum pcm_downstream_tx_result pcm_downstream_tx_init(
	struct pcm_downstream_tx *transmitter,
	enum pcm_protocol protocol,
	const struct v90_pcm_mapping *mapping)
{
	struct pcm_downstream_tx initialized = { 0 };

	if (transmitter == NULL || mapping == NULL ||
	    !pcm_protocol_is_valid(protocol))
		return PCM_DOWNSTREAM_TX_INVALID_ARGUMENT;
	if (!v90_pcm_mapping_is_valid(mapping) ||
	    (mapping->kind != V90_CP_TRAINING &&
	     protocol != PCM_PROTOCOL_V92))
		return PCM_DOWNSTREAM_TX_INVALID_PARAMETERS;
	if (v90_pcm_frame_encoder_init(
		    &initialized.encoder, mapping) !=
		    V90_PCM_FRAME_ENCODER_OK ||
	    v34_scrambler_init(
		    &initialized.scrambler, V34_POLYNOMIAL_GPC) !=
		    V34_FRAMING_OK)
		return PCM_DOWNSTREAM_TX_INVALID_PARAMETERS;
	initialized.protocol = protocol;
	if (mapping->kind == V90_CP_DATA) {
		/* V.92 9.9 starts a fresh coding epoch after Rf-bar, using the
		 * preceding data constellation without another TRN2d interval. */
		initialized.stage = PCM_DOWNSTREAM_TX_V92_CONTROL;
		initialized.control_configured = true;
		initialized.v92_control_kind = PCM_DOWNSTREAM_V92_CONTROL_SUVD;
		if (!encode_suvd_sequence(&initialized.control.suvd,
				mapping->frame_bits,
				initialized.control_sequence,
				&initialized.control_sequence_bits))
			return PCM_DOWNSTREAM_TX_INTERNAL_ERROR;
	}
	if (!pcm_downstream_tx_is_valid(&initialized))
		return PCM_DOWNSTREAM_TX_INTERNAL_ERROR;
	*transmitter = initialized;
	return PCM_DOWNSTREAM_TX_OK;
}

bool pcm_downstream_tx_is_complete(const struct pcm_downstream_tx *transmitter)
{
	return pcm_downstream_tx_is_valid(transmitter) && transmitter->complete;
}

bool pcm_downstream_tx_mp_starts_next_frame(const struct pcm_downstream_tx *transmitter)
{
	return transmitter != NULL &&
	       transmitter->protocol == PCM_PROTOCOL_V90 &&
	       transmitter->stage == PCM_DOWNSTREAM_TX_TRN2D &&
	       transmitter->control_configured && transmitter->queue_count == 0u &&
	       pcm_downstream_tx_is_valid(transmitter) &&
	       transmitter->accepted_frames >= initial_training_frames(transmitter);
}

static bool same_mp_record(
	const struct pcm_downstream_tx *transmitter,
	const struct v90_mp_record *record)
{
	return v90_mp_record_parameters_equal(
		&transmitter->control.mp, record);
}

enum pcm_downstream_tx_result pcm_downstream_tx_start_mp(
	struct pcm_downstream_tx *transmitter,
	const struct v90_mp_record *record)
{
	struct pcm_downstream_tx next;
	uint8_t sequence[V90_MP_MAXIMUM_SEQUENCE_BYTES] = { 0 };
	size_t sequence_bits = 0;
	struct v90_mp_record canonical = { 0 };

	if (transmitter == NULL || record == NULL)
		return PCM_DOWNSTREAM_TX_INVALID_ARGUMENT;
	if (!pcm_downstream_tx_is_valid(transmitter) ||
	    transmitter->protocol != PCM_PROTOCOL_V90 ||
	    transmitter->finish_requested)
		return PCM_DOWNSTREAM_TX_INVALID_STATE;
	if (!v90_mp_record_is_initial(record))
		return PCM_DOWNSTREAM_TX_INVALID_PARAMETERS;
	if (transmitter->control_configured) {
		if (same_mp_record(transmitter, record))
			return PCM_DOWNSTREAM_TX_OK;
		/* A zero-rate request may replace MP at a complete source-record
		 * boundary. Keep every already encoded symbol and coding history;
		 * never splice a new record into the middle of the old one. */
		if (transmitter->stage != PCM_DOWNSTREAM_TX_TRN2D &&
		    !(record_requests_cleardown(record) &&
		      transmitter->stage != PCM_DOWNSTREAM_TX_ED &&
		      transmitter->control_bit_index == 0u))
			return PCM_DOWNSTREAM_TX_INVALID_STATE;
	}
	if (!encode_mp_sequence(
		    record, transmitter->encoder.mapping.frame_bits, false,
		    sequence, &sequence_bits) ||
	    v90_mp_decode(
		    sequence, sequence_bits,
		    transmitter->encoder.mapping.frame_bits,
		    &canonical) != V90_MP_OK)
		return PCM_DOWNSTREAM_TX_INTERNAL_ERROR;
	next = *transmitter;
	next.control.mp = canonical;
	if (!store_sequence(
		    &next, sequence, sizeof(sequence), sequence_bits))
		return PCM_DOWNSTREAM_TX_INTERNAL_ERROR;
	next.control_configured = true;
	if (record_requests_cleardown(record)) {
		if (next.stage != PCM_DOWNSTREAM_TX_TRN2D)
			next.stage = PCM_DOWNSTREAM_TX_MP;
		next.mp_prime_requested = false;
		next.mp_prime_record_accepted = false;
		next.ed_requested = false;
	}
	if (!pcm_downstream_tx_is_valid(&next))
		return PCM_DOWNSTREAM_TX_INTERNAL_ERROR;
	*transmitter = next;
	return PCM_DOWNSTREAM_TX_OK;
}

enum pcm_downstream_tx_result pcm_downstream_tx_resume_after_rt(
	struct pcm_downstream_tx *transmitter)
{
	struct pcm_downstream_tx next = { 0 };
	enum pcm_downstream_tx_result result;

	if (!pcm_downstream_tx_is_complete(transmitter) ||
	    transmitter->encoder.mapping.kind != V90_CP_TRAINING ||
	    transmitter->stage != PCM_DOWNSTREAM_TX_ED)
		return PCM_DOWNSTREAM_TX_INVALID_STATE;

	next.protocol = transmitter->protocol;
	next.encoder = transmitter->encoder;
	next.encoder.finishing = false;
	next.encoder.shaper.finishing = false;
	next.scrambler = transmitter->scrambler;
	next.accepted_frames = transmitter->accepted_frames;
	next.transmitted_symbols = transmitter->transmitted_symbols;
	if (next.protocol == PCM_PROTOCOL_V92) {
		const struct v92_suvd_parameters suvd = { 0 };

		/* Rt is outside the continuous scrambled/shaped stream. For
		 * the single-sign coder, 8.8.5/V.92 seeds SUVd from Rt-bar's
		 * final positive symbol, not the old Ed sign. */
		if (next.encoder.mapping.sign_redundancy == 0u)
			next.encoder.signs.previous_pcm_sign = 1u;
		result = pcm_downstream_tx_start_suvd(&next, &suvd);
		if (result != PCM_DOWNSTREAM_TX_OK)
			return result;
		next.stage = PCM_DOWNSTREAM_TX_V92_CONTROL;
		if (!pcm_downstream_tx_is_valid(&next))
			return PCM_DOWNSTREAM_TX_INTERNAL_ERROR;
		*transmitter = next;
		return PCM_DOWNSTREAM_TX_OK;
	}
	result = pcm_downstream_tx_start_mp(&next, &transmitter->control.mp);
	if (result != PCM_DOWNSTREAM_TX_OK)
		return result;
	/* Rt follows a validated CP. Latch it before generating the next frame:
	 * the elapsed short interval skips TRN2d, but its boundary still owns
	 * one complete MP before the queued acknowledgement selects MP-prime. */
	result = pcm_downstream_tx_request_mp_prime(&next);
	if (result != PCM_DOWNSTREAM_TX_OK)
		return result;
	if (!pcm_downstream_tx_is_valid(&next))
		return PCM_DOWNSTREAM_TX_INTERNAL_ERROR;
	*transmitter = next;
	return PCM_DOWNSTREAM_TX_OK;
}

static bool same_v92_control(
	const struct pcm_downstream_tx *transmitter,
	enum pcm_downstream_v92_control_kind kind,
	const uint8_t *sequence, size_t sequence_capacity,
	size_t sequence_bits)
{
	return transmitter->v92_control_kind == kind &&
	       stored_sequence_matches(
		       transmitter, sequence, sequence_capacity, sequence_bits);
}

bool pcm_downstream_tx_is_v92_control_boundary(
	const struct pcm_downstream_tx *transmitter)
{
	return pcm_downstream_tx_is_valid(transmitter) &&
	       transmitter->protocol == PCM_PROTOCOL_V92 &&
	       transmitter->control_configured &&
	       transmitter->stage == PCM_DOWNSTREAM_TX_V92_CONTROL &&
	       !transmitter->finish_requested &&
	       transmitter->control_bit_index == 0u;
}

enum pcm_downstream_tx_result pcm_downstream_tx_start_suvd(
	struct pcm_downstream_tx *transmitter,
	const struct v92_suvd_parameters *parameters)
{
	struct pcm_downstream_tx next;
	struct v92_suvd_parameters canonical = { 0 };
	uint8_t sequence[V92_SUVD_MAX_FRAME_BYTES] = { 0 };
	size_t sequence_bits = 0u;

	if (transmitter == NULL || parameters == NULL)
		return PCM_DOWNSTREAM_TX_INVALID_ARGUMENT;
	if (!pcm_downstream_tx_is_valid(transmitter) ||
	    transmitter->protocol != PCM_PROTOCOL_V92 ||
	    transmitter->finish_requested)
		return PCM_DOWNSTREAM_TX_INVALID_STATE;
	if (!v92_suvd_parameters_are_valid(parameters))
		return PCM_DOWNSTREAM_TX_INVALID_PARAMETERS;
	if (!encode_suvd_sequence(
		    parameters, transmitter->encoder.mapping.frame_bits,
		    sequence, &sequence_bits) ||
	    v92_suvd_decode(
		    sequence, sizeof(sequence), sequence_bits,
		    transmitter->encoder.mapping.frame_bits,
		    &canonical) != V92_SUVD_OK)
		return PCM_DOWNSTREAM_TX_INTERNAL_ERROR;
	if (transmitter->control_configured) {
		if (same_v92_control(
			    transmitter, PCM_DOWNSTREAM_V92_CONTROL_SUVD,
			    sequence, sizeof(sequence), sequence_bits))
			return PCM_DOWNSTREAM_TX_OK;
		if (!pcm_downstream_tx_is_v92_control_boundary(transmitter))
			return PCM_DOWNSTREAM_TX_INVALID_STATE;
	}
	next = *transmitter;
	next.control = (union pcm_downstream_control_tx){ 0 };
	next.control.suvd = canonical;
	if (!store_sequence(
		    &next, sequence, sizeof(sequence), sequence_bits))
		return PCM_DOWNSTREAM_TX_INTERNAL_ERROR;
	next.control_bit_index = 0u;
	next.control_configured = true;
	next.v92_control_kind = PCM_DOWNSTREAM_V92_CONTROL_SUVD;
	next.v92_control_record_accepted = false;
	if (!pcm_downstream_tx_is_valid(&next))
		return PCM_DOWNSTREAM_TX_INTERNAL_ERROR;
	*transmitter = next;
	return PCM_DOWNSTREAM_TX_OK;
}

enum pcm_downstream_tx_result pcm_downstream_tx_start_cpd(
	struct pcm_downstream_tx *transmitter,
	const struct v92_cpd_base_parameters *parameters)
{
	struct pcm_downstream_tx next;
	uint8_t sequence[PCM_DOWNSTREAM_TX_CONTROL_BYTES] = { 0 };
	size_t sequence_bits = 0u;

	if (transmitter == NULL || parameters == NULL)
		return PCM_DOWNSTREAM_TX_INVALID_ARGUMENT;
	if (!pcm_downstream_tx_is_valid(transmitter) ||
	    transmitter->protocol != PCM_PROTOCOL_V92 ||
	    transmitter->finish_requested ||
	    !transmitter->control_configured)
		return PCM_DOWNSTREAM_TX_INVALID_STATE;
	if (!v92_cpd_base_parameters_are_valid(parameters))
		return PCM_DOWNSTREAM_TX_INVALID_PARAMETERS;
	if (!encode_cpd_sequence(
		    parameters, transmitter->encoder.mapping.frame_bits,
		    sequence, &sequence_bits))
		return PCM_DOWNSTREAM_TX_INTERNAL_ERROR;
	if (same_v92_control(
		    transmitter, PCM_DOWNSTREAM_V92_CONTROL_CPD,
		    sequence, sizeof(sequence), sequence_bits))
		return PCM_DOWNSTREAM_TX_OK;
	if (!pcm_downstream_tx_is_v92_control_boundary(transmitter))
		return PCM_DOWNSTREAM_TX_INVALID_STATE;
	next = *transmitter;
	next.control = (union pcm_downstream_control_tx){ 0 };
	if (!store_sequence(
		    &next, sequence, sizeof(sequence), sequence_bits))
		return PCM_DOWNSTREAM_TX_INTERNAL_ERROR;
	next.control.cpd = *parameters;
	next.control_bit_index = 0u;
	next.v92_control_kind = PCM_DOWNSTREAM_V92_CONTROL_CPD;
	next.v92_control_record_accepted = false;
	if (!pcm_downstream_tx_is_valid(&next))
		return PCM_DOWNSTREAM_TX_INTERNAL_ERROR;
	*transmitter = next;
	return PCM_DOWNSTREAM_TX_OK;
}

enum pcm_downstream_tx_result pcm_downstream_tx_request_mp_prime(
	struct pcm_downstream_tx *transmitter)
{
	struct pcm_downstream_tx next;

	if (transmitter == NULL)
		return PCM_DOWNSTREAM_TX_INVALID_ARGUMENT;
	if (!pcm_downstream_tx_is_valid(transmitter) ||
	    transmitter->protocol != PCM_PROTOCOL_V90 ||
	    !transmitter->control_configured || requests_cleardown(transmitter))
		return PCM_DOWNSTREAM_TX_INVALID_STATE;
	if (transmitter->mp_prime_requested)
		return PCM_DOWNSTREAM_TX_OK;
	next = *transmitter;
	next.mp_prime_requested = true;
	if (next.stage == PCM_DOWNSTREAM_TX_MP &&
	    next.control_bit_index == 0) {
		next.stage = PCM_DOWNSTREAM_TX_MP_PRIME;
		if (!encode_mp_sequence(
			    &next.control.mp, next.encoder.mapping.frame_bits,
			    true, next.control_sequence,
			    &next.control_sequence_bits))
			return PCM_DOWNSTREAM_TX_INTERNAL_ERROR;
	}
	if (!pcm_downstream_tx_is_valid(&next))
		return PCM_DOWNSTREAM_TX_INTERNAL_ERROR;
	*transmitter = next;
	return PCM_DOWNSTREAM_TX_OK;
}

enum pcm_downstream_tx_result pcm_downstream_tx_request_ed(
	struct pcm_downstream_tx *transmitter)
{
	struct pcm_downstream_tx next;
	bool v90_ready;
	bool v92_ready;

	if (transmitter == NULL)
		return PCM_DOWNSTREAM_TX_INVALID_ARGUMENT;
	if (!pcm_downstream_tx_is_valid(transmitter))
		return PCM_DOWNSTREAM_TX_INVALID_STATE;
	v90_ready = transmitter->protocol == PCM_PROTOCOL_V90 &&
		transmitter->control_configured &&
		transmitter->mp_prime_requested;
	v92_ready = transmitter->protocol == PCM_PROTOCOL_V92 &&
		transmitter->control_configured &&
		(transmitter->stage == PCM_DOWNSTREAM_TX_V92_CONTROL ||
		 transmitter->stage == PCM_DOWNSTREAM_TX_ED) &&
		transmitter->v92_control_kind ==
			PCM_DOWNSTREAM_V92_CONTROL_SUVD &&
		transmitter->control.suvd.acknowledges_cpu &&
		(transmitter->v92_control_record_accepted ||
		 transmitter->control_bit_index != 0u);
	if (!v90_ready && !v92_ready)
		return PCM_DOWNSTREAM_TX_INVALID_STATE;
	if (transmitter->complete)
		return PCM_DOWNSTREAM_TX_COMPLETE;
	if (transmitter->ed_requested)
		return PCM_DOWNSTREAM_TX_OK;
	next = *transmitter;
	next.ed_requested = true;
	if (next.stage == PCM_DOWNSTREAM_TX_MP_PRIME &&
	    next.mp_prime_record_accepted && next.control_bit_index == 0)
		next.stage = PCM_DOWNSTREAM_TX_ED;
	if (next.stage == PCM_DOWNSTREAM_TX_V92_CONTROL &&
	    next.control_bit_index == 0u)
		next.stage = PCM_DOWNSTREAM_TX_ED;
	if (!pcm_downstream_tx_is_valid(&next))
		return PCM_DOWNSTREAM_TX_INTERNAL_ERROR;
	*transmitter = next;
	return PCM_DOWNSTREAM_TX_OK;
}

enum pcm_downstream_tx_result pcm_downstream_tx_request_finish(
	struct pcm_downstream_tx *transmitter)
{
	struct pcm_downstream_tx next;

	if (transmitter == NULL)
		return PCM_DOWNSTREAM_TX_INVALID_ARGUMENT;
	if (!pcm_downstream_tx_is_valid(transmitter))
		return PCM_DOWNSTREAM_TX_INVALID_STATE;
	if (transmitter->complete)
		return PCM_DOWNSTREAM_TX_COMPLETE;
	if (transmitter->control_configured && !v92_cleardown_can_finish(transmitter))
		return PCM_DOWNSTREAM_TX_INVALID_STATE;
	next = *transmitter;
	next.finish_requested = true;
	if (!pcm_downstream_tx_is_valid(&next))
		return PCM_DOWNSTREAM_TX_INTERNAL_ERROR;
	*transmitter = next;
	return PCM_DOWNSTREAM_TX_OK;
}

static bool fill_queue(
	struct pcm_downstream_tx *transmitter,
	const struct v90_pcm_frame_output *output)
{
	const struct v90_pcm_mapping *mapping =
		&transmitter->encoder.mapping;
	size_t shaping_length = mapping->sign_redundancy == 0 ?
				0 : V90_CP_FRAME_INTERVALS /
					    mapping->sign_redundancy;
	size_t rule_index = 0;
	enum v90_spectral_rule rule = V90_SPECTRAL_RULE_A;

	if (transmitter->queue_count != 0 || output->symbol_count == 0 ||
	    output->symbol_count > V90_CP_FRAME_INTERVALS)
		return false;
	memset(transmitter->queue, 0, sizeof(transmitter->queue));
	for (size_t index = 0; index < output->symbol_count; ++index) {
		struct pcm_downstream_symbol queued = {
			.pcm = output->symbols[index],
		};

		if (!v90_pcm_symbol_is_valid(&queued.pcm, mapping->law))
			return false;

		if (mapping->sign_redundancy != 0) {
			bool starts =
				(transmitter->transmitted_symbols + index) %
					shaping_length == 0;

			if (starts) {
				if (rule_index >= output->rule_count)
					return false;
				rule = output->rules[rule_index++];
				if (rule > V90_SPECTRAL_RULE_D)
					return false;
			}
			queued.shaping_rule = rule;
			queued.shaping_rule_valid = true;
			queued.shaping_frame_start = starts;
		}
		transmitter->queue[index] = queued;
	}
	if (rule_index != output->rule_count)
		return false;
	transmitter->queue_count = output->symbol_count;
	return true;
}

static enum pcm_downstream_tx_result generate_frame(
	struct pcm_downstream_tx *transmitter,
	struct pcm_downstream_source_record *source_record)
{
	struct v90_pcm_frame_output output;
	uint8_t bits[V90_PCM_FRAME_MAX_BITS];
	size_t bit_count = transmitter->encoder.mapping.frame_bits;
	enum v90_pcm_frame_encoder_result result;
	bool external_record = source_record != NULL &&
		!source_record->complete;

	if (transmitter->accepted_frames >=
	    UINT64_MAX / V90_CP_FRAME_INTERVALS)
		return PCM_DOWNSTREAM_TX_OUT_OF_RANGE;
	if (transmitter->stage == PCM_DOWNSTREAM_TX_TRN2D &&
	    transmitter->control_configured &&
	    transmitter->accepted_frames >=
		    initial_training_frames(transmitter))
		transmitter->stage =
			transmitter->protocol == PCM_PROTOCOL_V90 ?
				PCM_DOWNSTREAM_TX_MP :
				PCM_DOWNSTREAM_TX_V92_CONTROL;
	for (size_t bit = 0; bit < bit_count; ++bit) {
		uint8_t source;

		if (transmitter->stage == PCM_DOWNSTREAM_TX_TRN2D)
			source = 1;
		else if (transmitter->stage == PCM_DOWNSTREAM_TX_ED)
			source = 0;
		else if (external_record)
			source = packed_msb_bit(
				source_record->sequence,
				source_record->bit_index + bit);
		else
			source = packed_msb_bit(
				transmitter->control_sequence,
				transmitter->control_bit_index + bit);

		if (v34_scramble_group(
			    &transmitter->scrambler, source, 1,
			    &bits[bit]) != V34_FRAMING_OK)
			return PCM_DOWNSTREAM_TX_INTERNAL_ERROR;
	}
	result = v90_pcm_frame_encoder_process(
		&transmitter->encoder, bits, bit_count, &output);
	if (result == V90_PCM_FRAME_ENCODER_NUMERIC_FAILURE)
		return PCM_DOWNSTREAM_TX_NUMERIC_FAILURE;
	if (result != V90_PCM_FRAME_ENCODER_BUFFERING &&
	    result != V90_PCM_FRAME_ENCODER_SYMBOLS_READY)
		return PCM_DOWNSTREAM_TX_INTERNAL_ERROR;
	transmitter->accepted_frames++;
	if (external_record) {
		if (source_record->bit_index == 0u)
			transmitter->v92_control_record_accepted = false;
		source_record->bit_index += bit_count;
		if (source_record->bit_index == source_record->sequence_bits) {
			source_record->complete = true;
			transmitter->v92_control_record_accepted = true;
		}
	} else if (transmitter->stage == PCM_DOWNSTREAM_TX_MP ||
	    transmitter->stage == PCM_DOWNSTREAM_TX_MP_PRIME ||
	    transmitter->stage == PCM_DOWNSTREAM_TX_V92_CONTROL) {
		transmitter->control_bit_index += bit_count;
		if (transmitter->control_bit_index ==
		    transmitter->control_sequence_bits) {
			transmitter->control_bit_index = 0;
			if (requests_cleardown(transmitter)) {
				transmitter->cleardown_records++;
				transmitter->finish_requested =
					transmitter->cleardown_records == CLEARDOWN_RECORDS;
			} else if (transmitter->stage == PCM_DOWNSTREAM_TX_MP &&
			    transmitter->mp_prime_requested) {
				transmitter->stage =
					PCM_DOWNSTREAM_TX_MP_PRIME;
				if (!encode_mp_sequence(
					    &transmitter->control.mp,
					    transmitter->encoder.mapping.frame_bits,
					    true,
					    transmitter->control_sequence,
					    &transmitter->control_sequence_bits))
					return PCM_DOWNSTREAM_TX_INTERNAL_ERROR;
			} else if (transmitter->stage ==
				   PCM_DOWNSTREAM_TX_MP_PRIME) {
				transmitter->mp_prime_record_accepted = true;
				if (transmitter->ed_requested)
					transmitter->stage =
						PCM_DOWNSTREAM_TX_ED;
			} else if (transmitter->stage ==
				   PCM_DOWNSTREAM_TX_V92_CONTROL &&
				   transmitter->ed_requested) {
				transmitter->v92_control_record_accepted = true;
				transmitter->stage = PCM_DOWNSTREAM_TX_ED;
			} else if (transmitter->stage ==
				   PCM_DOWNSTREAM_TX_V92_CONTROL) {
				transmitter->v92_control_record_accepted = true;
			}
		}
	} else if (transmitter->stage == PCM_DOWNSTREAM_TX_ED) {
		transmitter->ed_frames++;
		if (transmitter->ed_frames == V90_PHASE4_ED_FRAMES)
			transmitter->finish_requested = true;
	}
	if (result == V90_PCM_FRAME_ENCODER_SYMBOLS_READY &&
	    !fill_queue(transmitter, &output))
		return PCM_DOWNSTREAM_TX_INTERNAL_ERROR;
	return PCM_DOWNSTREAM_TX_OK;
}

static enum pcm_downstream_tx_result drain_encoder(
	struct pcm_downstream_tx *transmitter)
{
	struct v90_pcm_frame_output output;
	enum v90_pcm_frame_encoder_result result =
		v90_pcm_frame_encoder_finish(
			&transmitter->encoder, &output);

	if (result == V90_PCM_FRAME_ENCODER_COMPLETE) {
		transmitter->complete = true;
		return PCM_DOWNSTREAM_TX_COMPLETE;
	}
	if (result == V90_PCM_FRAME_ENCODER_NUMERIC_FAILURE)
		return PCM_DOWNSTREAM_TX_NUMERIC_FAILURE;
	if (result != V90_PCM_FRAME_ENCODER_SYMBOLS_READY ||
	    !fill_queue(transmitter, &output))
		return PCM_DOWNSTREAM_TX_INTERNAL_ERROR;
	return PCM_DOWNSTREAM_TX_OK;
}

static struct pcm_downstream_symbol pop_symbol(
	struct pcm_downstream_tx *transmitter)
{
	struct pcm_downstream_symbol symbol = transmitter->queue[0];

	transmitter->queue_count--;
	memmove(&transmitter->queue[0], &transmitter->queue[1],
		transmitter->queue_count * sizeof(transmitter->queue[0]));
	memset(&transmitter->queue[transmitter->queue_count], 0,
	       sizeof(transmitter->queue[0]));
	transmitter->transmitted_symbols++;
	return symbol;
}

static enum pcm_downstream_tx_result next_symbol(
	struct pcm_downstream_tx *transmitter,
	struct pcm_downstream_source_record *source_record,
	struct pcm_downstream_symbol *symbol)
{
	struct pcm_downstream_tx next;
	struct pcm_downstream_symbol generated;
	enum pcm_downstream_tx_result result = PCM_DOWNSTREAM_TX_OK;

	if (transmitter == NULL || symbol == NULL)
		return PCM_DOWNSTREAM_TX_INVALID_ARGUMENT;
	if (!pcm_downstream_tx_is_valid(transmitter))
		return PCM_DOWNSTREAM_TX_INVALID_STATE;
	if (transmitter->complete)
		return PCM_DOWNSTREAM_TX_COMPLETE;
	next = *transmitter;
	if (next.queue_count == 0 && next.finish_requested) {
		result = drain_encoder(&next);
		if (result == PCM_DOWNSTREAM_TX_COMPLETE) {
			if (!pcm_downstream_tx_is_valid(&next))
				return PCM_DOWNSTREAM_TX_INTERNAL_ERROR;
			*transmitter = next;
			return PCM_DOWNSTREAM_TX_COMPLETE;
		}
	} else if (next.queue_count == 0) {
		for (size_t attempt = 0;
		     attempt <= next.encoder.mapping.spectral_lookahead &&
		     next.queue_count == 0 && !next.finish_requested;
		     ++attempt) {
			result = generate_frame(&next, source_record);
			if (result != PCM_DOWNSTREAM_TX_OK)
				break;
		}
		if (result == PCM_DOWNSTREAM_TX_OK &&
		    next.queue_count == 0 && next.finish_requested)
			result = drain_encoder(&next);
		else if (result == PCM_DOWNSTREAM_TX_OK &&
			 next.queue_count == 0)
			result = PCM_DOWNSTREAM_TX_INTERNAL_ERROR;
	}
	if (result == PCM_DOWNSTREAM_TX_COMPLETE) {
		if (!pcm_downstream_tx_is_valid(&next))
			return PCM_DOWNSTREAM_TX_INTERNAL_ERROR;
		*transmitter = next;
		return PCM_DOWNSTREAM_TX_COMPLETE;
	}
	if (result != PCM_DOWNSTREAM_TX_OK)
		return result;
	generated = pop_symbol(&next);
	if (!pcm_downstream_tx_is_valid(&next))
		return PCM_DOWNSTREAM_TX_INTERNAL_ERROR;
	*transmitter = next;
	*symbol = generated;
	return PCM_DOWNSTREAM_TX_OK;
}

enum pcm_downstream_tx_result pcm_downstream_tx_next(
	struct pcm_downstream_tx *transmitter,
	struct pcm_downstream_symbol *symbol)
{
	return next_symbol(transmitter, NULL, symbol);
}

enum pcm_downstream_tx_result pcm_downstream_tx_next_source_record(
	struct pcm_downstream_tx *transmitter,
	struct pcm_downstream_source_record *record,
	struct pcm_downstream_symbol *symbol)
{
	struct pcm_downstream_tx next_transmitter;
	struct pcm_downstream_source_record next_record;
	struct pcm_downstream_symbol generated;
	enum pcm_downstream_tx_result result;
	size_t sequence_bytes;

	if (transmitter == NULL || record == NULL || symbol == NULL ||
	    memory_regions_overlap(
		    transmitter, sizeof(*transmitter), record,
		    sizeof(*record)) ||
	    memory_regions_overlap(
		    transmitter, sizeof(*transmitter), symbol,
		    sizeof(*symbol)) ||
	    memory_regions_overlap(
		    record, sizeof(*record), symbol, sizeof(*symbol)))
		return PCM_DOWNSTREAM_TX_INVALID_ARGUMENT;
	if (!pcm_downstream_tx_is_v92_control_boundary(transmitter) ||
	    transmitter->v92_control_kind !=
		    PCM_DOWNSTREAM_V92_CONTROL_SUVD ||
	    !source_record_is_valid(
		    record, transmitter->encoder.mapping.frame_bits) ||
	    record->complete)
		return PCM_DOWNSTREAM_TX_INVALID_STATE;
	sequence_bytes = record->sequence_bits / 8u +
			 (record->sequence_bits % 8u != 0u);
	if (memory_regions_overlap(
		    record->sequence, sequence_bytes,
		    transmitter, sizeof(*transmitter)) ||
	    memory_regions_overlap(
		    record->sequence, sequence_bytes,
		    record, sizeof(*record)) ||
	    memory_regions_overlap(
		    record->sequence, sequence_bytes,
		    symbol, sizeof(*symbol)))
		return PCM_DOWNSTREAM_TX_INVALID_ARGUMENT;
	next_transmitter = *transmitter;
	next_record = *record;
	result = next_symbol(
		&next_transmitter, &next_record, &generated);
	if (result != PCM_DOWNSTREAM_TX_OK)
		return result;
	if (!source_record_is_valid(
		    &next_record,
		    next_transmitter.encoder.mapping.frame_bits))
		return PCM_DOWNSTREAM_TX_INTERNAL_ERROR;
	*transmitter = next_transmitter;
	*record = next_record;
	*symbol = generated;
	return PCM_DOWNSTREAM_TX_OK;
}

const char *pcm_downstream_tx_result_name(
	enum pcm_downstream_tx_result result)
{
	switch (result) {
	case PCM_DOWNSTREAM_TX_OK:
		return "ok";
	case PCM_DOWNSTREAM_TX_COMPLETE:
		return "complete";
	case PCM_DOWNSTREAM_TX_INVALID_ARGUMENT:
		return "invalid argument";
	case PCM_DOWNSTREAM_TX_INVALID_PARAMETERS:
		return "invalid parameters";
	case PCM_DOWNSTREAM_TX_INVALID_STATE:
		return "invalid state";
	case PCM_DOWNSTREAM_TX_OUT_OF_RANGE:
		return "out of range";
	case PCM_DOWNSTREAM_TX_NUMERIC_FAILURE:
		return "numeric failure";
	case PCM_DOWNSTREAM_TX_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
