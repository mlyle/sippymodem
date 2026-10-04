// SPDX-License-Identifier: BSD-3-Clause
#include "v91_call.h"
#include "packed_bits.h"
#include "pcm_power.h"
#include <string.h>

static bool fail(struct v91_call *call, enum v91_failure reason)
{
	if (call->failure == V91_FAILURE_NONE)
		call->failure = reason;
	return false;
}

static void receive_stage(struct v91_call *call, enum v91_rx_stage stage)
{
	call->rx_stage = stage;
	call->rx_count = call->rx_position = call->rx_length = 0;
	call->zeroes = call->ones = 0;
	memset(call->rx_record, 0, sizeof(call->rx_record));
}

static bool transmit_stage(struct v91_call *call, enum v91_tx_stage stage)
{
	call->tx_stage = stage;
	call->tx_count = call->tx_position = call->tx_length = 0;
	switch (stage) {
	case V91_TX_INFO:
		call->tx_length = V91_INFO_BITS;
		return v91_info_encode(&call->local_info, call->tx_record);
	case V91_TX_J:
		v34_scrambler_init(&call->tx_sign.scrambler, V34_POLYNOMIAL_GPC);
		return v90_dil_encode(&call->requested_dil, call->tx_record,
			sizeof(call->tx_record), &call->tx_length) == V90_DIL_OK;
	case V91_TX_DIL:
		if (!call->peer_dil.segment_count)
			return transmit_stage(call, V91_TX_SCR);
		return v90_dil_sequence_init(&call->tx_dil, &call->peer_dil,
			call->local_info.law) == V90_DIL_SEQUENCE_OK &&
			v90_dil_training_symbol_count(&call->peer_dil, &call->tx_dil_length);
	case V91_TX_SCR:
		pcm_binary_sign_init(&call->tx_sign, V34_POLYNOMIAL_GPC);
		break;
	case V91_TX_CP:
		call->cp_deadline_active = false;
		call->local_cp.acknowledges_peer = call->have_cp;
		call->tx_length = v91_cp_encode(&call->local_cp, call->tx_record);
		return call->tx_length != 0;
	case V91_TX_B1: {
		struct pcm_constellation sets[6];
		struct v90_pcm_mapping mapping;
		for (unsigned i = 0; i < 6; ++i)
			sets[i] = call->peer_cp.constellations[call->peer_cp.frame_constellation[i]];
		if (v90_pcm_mapping_init_v91(&mapping, call->local_info.law,
			call->peer_cp.rate + 20, sets) != V90_PCM_MAPPING_OK ||
		    v90_pcm_frame_encoder_init(&call->encoder, &mapping) != V90_PCM_FRAME_ENCODER_OK)
			return false;
		v34_scrambler_init(&call->tx_sign.scrambler, V34_POLYNOMIAL_GPC);
		break;
	}
	default: break;
	}
	return true;
}

bool v91_call_init(struct v91_call *call, enum g711_law law)
{
	if (call == NULL || !g711_law_is_valid(law))
		return false;
	memset(call, 0, sizeof(*call));
	call->local_info.law = law;
	call->maximum_frame_bits = 48;
	call->local_info.custom_dil = true;
	call->local_info.control_channel = true;
	v91_control_encode(call->tx_control, false, false);
	pcm_binary_sign_init(&call->tx_sign, V34_POLYNOMIAL_GPC);
	pcm_binary_sign_init(&call->rx_sign, V34_POLYNOMIAL_GPC);
	return v91_profile_dil(&call->requested_dil);
}

static bool recoverable(enum v91_failure failure)
{
	return failure == V91_FAILURE_DESCRIPTOR || failure == V91_FAILURE_CONSTELLATION ||
	       failure == V91_FAILURE_CHANNEL || failure == V91_FAILURE_B1 ||
	       failure == V91_FAILURE_CONTROL || failure == V91_FAILURE_RETRAIN;
}

static bool start_retrain(struct v91_call *call, bool automatic)
{
	if (call == NULL || !g711_law_is_valid(call->local_info.law) ||
	    (call->failure != V91_FAILURE_NONE && !recoverable(call->failure)))
		return false;
	if (automatic && call->recovery_attempts == 2) {
		call->failure = V91_FAILURE_RECOVERY_EXHAUSTED;
		return false;
	}
	uint8_t attempts = call->recovery_attempts + automatic;
	struct v91_info local = call->local_info;
	local.acknowledges_peer = false;
	uint32_t samples = call->samples;
	uint32_t deadline = call->recovering ? call->recovery_deadline : samples + 80000;
	unsigned retrains = call->retrains + 1;
	uint8_t maximum_frame_bits = call->maximum_frame_bits;
	bool closing = call->closing;
	enum g711_law law = call->local_info.law;
	v91_call_init(call, law);
	call->local_info = local;
	call->recovery_attempts = attempts;
	call->samples = samples;
	call->info_started = samples + 1;
	call->recovering = true;
	call->recovery_deadline = deadline;
	call->retrains = retrains;
	call->maximum_frame_bits = maximum_frame_bits;
	call->closing = closing;
	return transmit_stage(call, V91_TX_INFO);
}

bool v91_call_retrain(struct v91_call *call)
{
	return start_retrain(call, false);
}

static bool received_info(struct v91_call *call, struct v91_info peer)
{
	if (call->have_info &&
	    (peer.law != call->peer_info.law || peer.custom_dil != call->peer_info.custom_dil ||
	     peer.control_channel != call->peer_info.control_channel ||
	     peer.maximum_power != call->peer_info.maximum_power ||
	     peer.receiver_power != call->peer_info.receiver_power ||
	     peer.transparent != call->peer_info.transparent ||
	     peer.require_transparent != call->peer_info.require_transparent))
		return fail(call, V91_FAILURE_DESCRIPTOR);
	if (peer.require_transparent)
		return fail(call, V91_FAILURE_TRANSPARENT);
	call->peer_info = peer;
	call->have_info = true;
	if (!peer.custom_dil) {
		call->peer_dil = (struct v90_dil_parameters){
			.segment_count = 125, .sign_pattern_bits = 12,
			.training_pattern_bits = 12,
			.sign_pattern = { 0xc0, 0x0f },
			.training_pattern = { 0xff, 0x0f },
		};
		memset(call->peer_dil.segment_length_codes, 1, 8);
		for (unsigned i = 0; i < 125; ++i)
			call->peer_dil.training_ucodes[i] = i & 1u ? i / 2u : 124u - i / 2u;
		call->have_peer_dil = true;
	}
	if (peer.acknowledges_peer) {
		call->have_info_ack = true;
		v34_scrambler_init(&call->rx_sign.scrambler, V34_POLYNOMIAL_GPC);
		if (!peer.custom_dil)
			call->rx_sign.previous_sign = 0;
		receive_stage(call, peer.custom_dil ? V91_RX_J : V91_RX_EM);
	}
	return true;
}

static bool received_dil(struct v91_call *call)
{
	struct v91_profile_limits limits = {
		.maximum_power = call->peer_info.maximum_power ?
			pcm_power_limit(call->peer_info.maximum_power) : UINT32_MAX,
		.maximum_frame_bits = call->maximum_frame_bits,
		.receive_law = call->local_info.law,
		.receiver_power = call->peer_info.receiver_power,
	};
	if (!v91_profile_learn(&call->probe, call->peer_info.law, &limits,
		&call->rx_mapping, &call->channel) ||
	    !pcm_constellations_pack(call->rx_mapping.transmit,
		call->local_cp.constellations, call->local_cp.frame_constellation,
		&call->local_cp.count))
		return fail(call, V91_FAILURE_CHANNEL);
	call->local_cp.rate = call->closing ? 0 : call->rx_mapping.frame_bits - 20;
	pcm_binary_sign_init(&call->rx_sign, V34_POLYNOMIAL_GPC);
	receive_stage(call, V91_RX_CP);
	return true;
}

static bool receive_control(struct v91_call *call, uint8_t octet)
{
	uint8_t sign = (octet >> 7) & 1u;
	uint8_t bit = sign ^ call->rx_sign.previous_sign;
	call->rx_sign.previous_sign = sign;
	if (call->rx_stage != V91_RX_INFO)
		v34_descramble_group(&call->rx_sign.scrambler, bit, 1, &bit);
	switch (call->rx_stage) {
	case V91_RX_J:
		packed_msb_set_bit(call->rx_record, call->rx_position++, bit);
		if (call->rx_position == V90_DIL_HEADER_BITS) {
			struct pcm_dil_descriptor_layout layout;
			if (v90_dil_descriptor_layout(call->rx_record, sizeof(call->rx_record),
				call->rx_position, &layout) != V90_DIL_OK)
				return fail(call, V91_FAILURE_DESCRIPTOR);
			call->rx_length = layout.bit_count;
		}
		if (call->rx_length && call->rx_position == call->rx_length) {
			if (v90_dil_decode(call->rx_record, sizeof(call->rx_record), call->rx_length,
				&call->peer_dil) != V90_DIL_OK)
				return fail(call, V91_FAILURE_DESCRIPTOR);
			call->have_peer_dil = true;
			receive_stage(call, V91_RX_EM);
		}
		break;
	case V91_RX_EM:
		call->zeroes = bit ? 0 : call->zeroes + 1;
		if (call->zeroes == 12) {
			if (v90_dil_sequence_init(&call->rx_dil, &call->requested_dil,
				call->peer_info.law) != V90_DIL_SEQUENCE_OK ||
			    !v90_dil_training_symbol_count(&call->requested_dil, &call->rx_dil_length))
				return fail(call, V91_FAILURE_DESCRIPTOR);
			receive_stage(call, V91_RX_DIL);
			call->cp_deadline = call->samples + 12000;
			call->cp_deadline_active = true;
		}
		break;
	case V91_RX_CP:
		if (call->rx_position) {
			packed_msb_set_bit(call->rx_record, call->rx_position++, bit);
			if (call->rx_position == 136) {
				call->rx_length = v91_cp_length(call->rx_record, call->rx_position);
				if (!call->rx_length)
					return fail(call, V91_FAILURE_CONSTELLATION);
			}
			if (call->rx_length && call->rx_position == call->rx_length) {
				struct v91_cp peer;
				if (!v91_cp_decode(call->rx_record, call->rx_length, &peer))
					return fail(call, V91_FAILURE_CONSTELLATION);
				if (!peer.rate)
					return fail(call, V91_FAILURE_PEER_CLEARDOWN);
				if (call->have_cp) {
					if (peer.rate != call->peer_cp.rate || peer.transparent != call->peer_cp.transparent)
						return fail(call, V91_FAILURE_CONSTELLATION);
					for (unsigned i = 0; i < 6; ++i)
						if (memcmp(&peer.constellations[peer.frame_constellation[i]],
							&call->peer_cp.constellations[call->peer_cp.frame_constellation[i]],
							sizeof(struct pcm_constellation)) != 0)
							return fail(call, V91_FAILURE_CONSTELLATION);
				}
				call->peer_cp = peer;
				call->have_cp = true;
				call->have_cp_ack |= peer.acknowledges_peer;
				call->rx_position = call->rx_length = call->ones = call->zeroes = 0;
			}
		} else if (bit) {
			if (call->ones < 17)
				++call->ones;
			call->zeroes = 0;
		} else {
			if (call->ones == 17) {
				memset(call->rx_record, 0, sizeof(call->rx_record));
				packed_msb_write_msb_field(call->rx_record, 0, 17, 0x1ffff);
				call->rx_position = 18;
			} else if (++call->zeroes == 12 && call->have_cp) {
				call->have_es = true;
				if (v90_pcm_frame_decoder_init(&call->decoder, &call->rx_mapping,
					V90_PCM_MAPPING_TRANSMIT) != V90_PCM_FRAME_DECODER_OK)
					return fail(call, V91_FAILURE_CONSTELLATION);
				v34_scrambler_init(&call->rx_sign.scrambler, V34_POLYNOMIAL_GPC);
				receive_stage(call, V91_RX_B1);
			}
			call->ones = 0;
		}
		break;
	default: return false;
	}
	return true;
}

static bool receive_sample(struct v91_call *call, uint8_t octet, struct v91_event *event)
{
	if (call->rx_stage == V91_RX_DIL) {
		struct v90_pcm_symbol expected;
		if (v90_dil_sequence_next(&call->rx_dil, &expected) != V90_DIL_SEQUENCE_OK ||
		    !pcm_octet_probe_observe(&call->probe, call->rx_count % 6, expected.octet, octet))
			return fail(call, V91_FAILURE_CHANNEL);
		if (++call->rx_count == call->rx_dil_length)
			return received_dil(call);
		return true;
	}
	if (call->rx_stage == V91_RX_INFO)
		return true;
	if (call->rx_stage < V91_RX_B1)
		return receive_control(call, octet);
	unsigned interval = call->rx_count % 6;
	uint8_t normalized;
	struct v90_pcm_symbol symbol;
	if (!pcm_octet_channel_normalize(&call->channel, interval, octet, &normalized) ||
	    !v90_pcm_symbol_parse(&symbol, call->peer_info.law, normalized))
		return fail(call, V91_FAILURE_CHANNEL);
	call->rx_ucodes[interval] = symbol.ucode;
	call->rx_signs[interval] = symbol.positive;
	if (++call->rx_count % 6)
		return true;
	struct v90_pcm_frame_decode_output decoded;
	bool control = call->rx_stage == V91_RX_DATA && call->local_info.control_channel &&
		((call->rx_count / 6 - 1) % 64 == 0);
	unsigned offset = control ? 6 : 0;
	if (v90_pcm_frame_decoder_decode(&call->decoder, call->rx_ucodes, call->rx_signs,
		6, &decoded) != V90_PCM_FRAME_DECODER_OK ||
	    v34_descramble_unpacked_bits(&call->rx_sign.scrambler, decoded.bits + offset,
		decoded.bit_count - offset, decoded.bits + offset) != V34_FRAMING_OK)
		return fail(call, V91_FAILURE_CHANNEL);
	if (control) {
		for (unsigned i = 0; i < 6; ++i)
			packed_msb_set_bit(call->rx_control, call->rx_control_position++, decoded.bits[i]);
		if (call->rx_control_position == 96) {
			bool retrain, cleardown;
			call->rx_control_position = 0;
			if (!v91_control_decode(call->rx_control, &retrain, &cleardown))
				return fail(call, V91_FAILURE_CONTROL);
			if (cleardown)
				return fail(call, V91_FAILURE_PEER_CLEARDOWN);
			if (retrain)
				return fail(call, V91_FAILURE_RETRAIN);
		}
	}
	if (call->rx_stage == V91_RX_B1) {
		for (unsigned i = 0; i < decoded.bit_count; ++i)
			if (decoded.bits[i] != 1)
				return fail(call, V91_FAILURE_B1);
		if (call->rx_count == 12)
			receive_stage(call, V91_RX_DATA);
	} else {
		memcpy(event->bits, decoded.bits + offset, decoded.bit_count - offset);
		event->count = decoded.bit_count - offset;
		call->rx_count %= 384;
	}
	return true;
}

static bool transmit_sample(struct v91_call *call, uint8_t *octet,
	v91_bit_source source, void *context)
{
	uint8_t bit = 0, sign = 0;
	bool scrambled = false, transition = false;
	enum v91_tx_stage next = call->tx_stage;
	switch (call->tx_stage) {
	case V91_TX_SILENCE:
		g711_ucode_octet(call->local_info.law, 0, true, octet);
		if (++call->tx_count == 600)
			return transmit_stage(call, V91_TX_EZ);
		return true;
	case V91_TX_EZ:
		g711_ucode_octet(call->local_info.law, 66, false, octet);
		if (++call->tx_count == 24) {
			call->info_started = call->samples;
			return transmit_stage(call, V91_TX_INFO);
		}
		return true;
	case V91_TX_INFO:
		bit = packed_msb_bit(call->tx_record, call->tx_position++);
		if (call->tx_position == call->tx_length) {
			if (call->local_info.acknowledges_peer)
				next = V91_TX_J;
			else
				call->local_info.acknowledges_peer = call->have_info;
			transition = true;
		}
		break;
	case V91_TX_J:
		bit = packed_msb_bit(call->tx_record, call->tx_position++);
		scrambled = true;
		if (call->tx_position == call->tx_length) {
			next = call->have_peer_dil ? V91_TX_EM : V91_TX_PHIL;
			transition = true;
		}
		break;
	case V91_TX_PHIL:
		bit = 1;
		scrambled = true;
		if (call->have_peer_dil) {
			next = V91_TX_EM;
			transition = true;
		}
		break;
	case V91_TX_EM:
	case V91_TX_ES:
		scrambled = true;
		if (++call->tx_count == 12) {
			next = call->tx_stage == V91_TX_EM ? V91_TX_DIL : V91_TX_B1;
			transition = true;
		}
		break;
	case V91_TX_DIL: {
		struct v90_pcm_symbol symbol;
		if (v90_dil_sequence_next(&call->tx_dil, &symbol) != V90_DIL_SEQUENCE_OK)
			return false;
		*octet = symbol.octet;
		if (++call->tx_count == call->tx_dil_length)
			return transmit_stage(call, V91_TX_SCR);
		return true;
	}
	case V91_TX_SCR:
		bit = 1;
		scrambled = true;
		if (++call->tx_count % 6 == 0 && call->local_cp.count) {
			next = V91_TX_CP;
			transition = true;
		}
		break;
	case V91_TX_CP:
		bit = packed_msb_bit(call->tx_record, call->tx_position++);
		scrambled = true;
		if (call->tx_position == call->tx_length) {
			if (!packed_msb_read_lsb_field(call->tx_record, 20, 5))
				call->close_after_frame = true;
			if (!call->closing && call->local_cp.acknowledges_peer &&
			    (call->have_cp_ack || call->have_es))
				next = V91_TX_ES;
			transition = true;
		}
		break;
	case V91_TX_B1:
	case V91_TX_DATA: {
		unsigned interval = call->tx_count % 6;
		if (!interval) {
			uint8_t bits[PCM_FRAME_MAX_BITS];
			unsigned count = call->encoder.mapping.frame_bits;
			struct v90_pcm_frame_output encoded;
			bool control = call->tx_stage == V91_TX_DATA && call->peer_info.control_channel &&
				(call->tx_count / 6 % 64 == 0);
			unsigned offset = control ? 6 : 0;
			memset(bits, 1, count);
			if (call->tx_stage == V91_TX_DATA && source)
				source(context, bits + offset, count - offset);
			if (control) {
				if (!call->tx_control_position)
					v91_control_encode(call->tx_control, false, call->closing);
				for (unsigned i = 0; i < 6; ++i) {
					bits[i] = packed_msb_bit(call->tx_control, call->tx_control_position++);
					call->tx_control_position %= 96;
				}
				if (!call->tx_control_position && packed_msb_bit(call->tx_control, 16))
					call->close_after_frame = true;
			}
			if (v34_scramble_unpacked_bits(&call->tx_sign.scrambler, bits + offset,
				count - offset, bits + offset) != V34_FRAMING_OK ||
			    v90_pcm_frame_encoder_process(&call->encoder, bits, count, &encoded) !=
				V90_PCM_FRAME_ENCODER_SYMBOLS_READY)
				return false;
			for (unsigned i = 0; i < 6; ++i)
				call->tx_octets[i] = encoded.symbols[i].octet;
		}
		*octet = call->tx_octets[interval];
		if (interval == 5 && call->close_after_frame)
			return fail(call, V91_FAILURE_LOCAL_CLEARDOWN);
		++call->tx_count;
		if (call->tx_stage == V91_TX_DATA)
			call->tx_count %= 384;
		else if (call->tx_count == 12)
			return transmit_stage(call, V91_TX_DATA);
		return true;
	}
	}
	if (scrambled) {
		if (pcm_binary_sign_encode(&call->tx_sign, bit, &sign) != PCM_BINARY_SIGN_OK)
			return false;
	} else {
		sign = call->tx_sign.previous_sign ^ bit;
		call->tx_sign.previous_sign = sign;
	}
	if (!g711_ucode_octet(call->local_info.law, 66, sign, octet))
		return false;
	if (call->close_after_frame)
		return fail(call, V91_FAILURE_LOCAL_CLEARDOWN);
	return !transition || transmit_stage(call, next);
}

bool v91_call_process(struct v91_call *call, uint8_t receive, bool available,
	uint8_t *transmit, v91_bit_source source, void *context, struct v91_event *event)
{
	if (!call || !transmit || !event)
		return false;
	*event = (struct v91_event){ 0 };
	bool retraining = false;
	if (!available) {
		v91_info_receiver_init(&call->info_receiver);
		if (call->rx_stage != V91_RX_INFO) {
			retraining = start_retrain(call, true);
		}
	} else if (call->failure == V91_FAILURE_NONE) {
		struct v91_info info;
		bool found = v91_info_receiver_sample(&call->info_receiver, receive & 0x80, &info);
		if (found && call->rx_stage != V91_RX_INFO && call->info_receiver.consecutive == 2) {
			retraining = start_retrain(call, true);
		}
		if (call->rx_stage == V91_RX_INFO) {
			if (found) {
				call->rx_sign.previous_sign = !!(receive & 0x80);
				received_info(call, info);
			}
		} else {
			receive_sample(call, receive, event);
		}
		if (recoverable(call->failure)) {
			retraining = start_retrain(call, true);
		}
	}

	if (call->failure == V91_FAILURE_NONE && !transmit_sample(call, transmit, source, context))
		fail(call, V91_FAILURE_CONSTELLATION);
	++call->samples;
	if (call->tx_stage >= V91_TX_INFO) {
		uint32_t age = call->samples - call->info_started;
		if (age > 12000 && !call->have_info_ack)
			fail(call, V91_FAILURE_INFO_TIMEOUT);
		if (age > 80000 && (call->rx_stage != V91_RX_DATA || call->tx_stage != V91_TX_DATA))
			fail(call, V91_FAILURE_TRAINING_TIMEOUT);
	}
	if (call->cp_deadline_active && (int32_t)(call->samples - call->cp_deadline) >= 0)
		fail(call, V91_FAILURE_TRAINING_TIMEOUT);
	if (call->recovering && (int32_t)(call->samples - call->recovery_deadline) >= 0)
		fail(call, V91_FAILURE_TRAINING_TIMEOUT);
	event->failure = call->failure;
	event->retraining = retraining;
	event->online = call->tx_stage == V91_TX_DATA && call->rx_stage == V91_RX_DATA;
	if (event->online && !event->failure) {
		call->recovering = false;
		/* Match PCM-upstream recovery's 12.288-second healthy interval.
		 * A brief return to data must not restart an endless retrain loop. */
		if (call->healthy_samples < 98304 && ++call->healthy_samples == 98304)
			call->recovery_attempts = 0;
	}
	if (retraining || event->failure)
		event->count = 0;
	return call->failure == V91_FAILURE_NONE;
}

bool v91_call_hangup(struct v91_call *call)
{
	if (call == NULL || call->failure != V91_FAILURE_NONE)
		return false;
	call->closing = true;
	if (call->tx_stage >= V91_TX_B1 && !call->peer_info.control_channel &&
	    !v91_call_retrain(call))
		return false;
	call->local_cp.rate = 0;
	return true;
}

const char *v91_failure_name(enum v91_failure failure)
{
	switch (failure) {
	case V91_FAILURE_NONE: return "none";
	case V91_FAILURE_RECOVERY_EXHAUSTED: return "V.91 automatic recovery exhausted";
	case V91_FAILURE_INFO_TIMEOUT: return "V.91 INFO acknowledgement timeout";
	case V91_FAILURE_TRAINING_TIMEOUT: return "V.91 training deadline";
	case V91_FAILURE_DESCRIPTOR: return "V.91 invalid DIL descriptor";
	case V91_FAILURE_CONSTELLATION: return "V.91 invalid constellation parameters";
	case V91_FAILURE_CHANNEL: return "V.91 ambiguous or unlearned codeword";
	case V91_FAILURE_MEDIA: return "V.91 missing media";
	case V91_FAILURE_B1: return "V.91 B1 mismatch";
	case V91_FAILURE_PEER_CLEARDOWN: return "V.91 peer requested cleardown";
	case V91_FAILURE_LOCAL_CLEARDOWN: return "V.91 local cleardown complete";
	case V91_FAILURE_TRANSPARENT: return "V.91 peer requires transparent mode";
	case V91_FAILURE_CONTROL: return "V.91 control alignment or CRC failure";
	case V91_FAILURE_RETRAIN: return "V.91 peer requested retrain";
	}
	return "V.91 invalid state";
}
