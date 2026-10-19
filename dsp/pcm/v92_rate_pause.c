// SPDX-License-Identifier: BSD-3-Clause

#include "v92_rate_pause.h"
#include "memory_region.h"

#include <stddef.h>

bool v92_rate_pause_analog_is_valid(const struct v92_rate_pause_analog *pause)
{
	return pause && pcm_downstream_rx_is_valid(&pause->downstream) &&
		pause->downstream.protocol == PCM_PROTOCOL_V92 &&
		pause->downstream.stream.decoder.mapping.kind == V90_CP_TRAINING &&
		pcm_r_rx_is_valid(&pause->rt) && pause->rt.signal == PCM_R_RT &&
		pause->rt.mapping.law == pause->downstream.stream.decoder.mapping.law &&
		pause->rt.decision_plane == pause->downstream.stream.decoder.decision_plane &&
		v92_cpu_parameters_are_valid(&pause->cpu) &&
		pause->cpu.law == pause->downstream.stream.decoder.mapping.law &&
		(pause->sending_status ? v92_phase4_analog_tx_is_valid(&pause->upstream.status) :
		 (v92_pcm_phase4_trn2u_is_valid(&pause->upstream.training) &&
		  pause->upstream.training.emitted_symbols <= V92_RATE_PAUSE_TRN2U_SYMBOLS));
}

bool v92_rate_pause_digital_is_valid(const struct v92_rate_pause_digital *pause)
{
	return pause && v92_upstream_control_line_rx_is_valid(&pause->upstream) &&
		pause->upstream.signal.line.renegotiation &&
		pcm_downstream_tx_is_valid(&pause->downstream) &&
		pause->downstream.protocol == PCM_PROTOCOL_V92 &&
		pause->downstream.encoder.mapping.kind == V90_CP_TRAINING &&
		pause->upstream.signal.line.law == pause->downstream.encoder.mapping.law &&
		pcm_r_tx_is_valid(&pause->rt) && pause->rt.signal == PCM_R_RT &&
		pause->rt.mapping.law == pause->downstream.encoder.mapping.law;
}

bool v92_rate_pause_analog_init(struct v92_rate_pause_analog *pause,
	const struct v92_phase4_e2u_handoff *handoff, bool peer_requested)
{
	struct v92_rate_pause_analog next = { .peer_requested = peer_requested };
	if (!pause || !v92_phase4_e2u_handoff_is_valid(handoff) ||
	    handoff->local_role != V90_ANALOG_MODEM ||
	    handoff->state.analog.downstream_stage != V92_PHASE4_E2U_DOWNSTREAM_PAUSE)
		return false;
	next.cpu = handoff->state.analog.cpu;
	next.upstream.training.modulator = handoff->state.analog.transmit;
	next.downstream = handoff->state.analog.downstream.training;
	if (!pcm_downstream_rx_is_complete(&next.downstream) &&
	    pcm_downstream_rx_expect_ed(&next.downstream) != PCM_DOWNSTREAM_RX_OK &&
	    next.downstream.stage != PCM_DOWNSTREAM_RX_V92_CONTROL)
		return false;
	if (pcm_r_rx_init(&next.rt, &next.downstream.stream.decoder.mapping,
		next.downstream.stream.decoder.decision_plane, PCM_R_RT) != PCM_R_RX_OK ||
	    !v92_rate_pause_analog_is_valid(&next))
		return false;
	*pause = next;
	return true;
}

bool v92_rate_pause_digital_init(struct v92_rate_pause_digital *pause,
	const struct v92_phase4_e2u_handoff *handoff, bool peer_requested)
{
	struct v92_rate_pause_digital next = { .peer_requested = peer_requested };
	if (!pause || !v92_phase4_e2u_handoff_is_valid(handoff) ||
	    handoff->local_role != V90_DIGITAL_MODEM ||
	    handoff->state.digital.downstream_stage != V92_PHASE4_E2U_DOWNSTREAM_PAUSE)
		return false;
	next.upstream.signal = handoff->state.digital.receive;
	next.downstream = handoff->state.digital.downstream.training.transmit;
	unsigned bits = v92_phase4_modulator_bits_per_symbol(next.upstream.signal.line.demodulator.constellation);
	if (v92_control_rx_init_upstream(&next.upstream.control, 12u * bits) != V92_CONTROL_RX_OK ||
	    pcm_r_tx_init(&next.rt, &next.downstream.encoder.mapping, PCM_R_RT) != PCM_R_TX_OK ||
	    !v92_rate_pause_digital_is_valid(&next))
		return false;
	*pause = next;
	return true;
}

bool v92_rate_pause_analog_is_ready(const struct v92_rate_pause_analog *pause)
{
	return v92_rate_pause_analog_is_valid(pause) && !pause->failed &&
		pause->sending_status && pcm_r_rx_is_complete(&pause->rt);
}

bool v92_rate_pause_digital_is_ready(const struct v92_rate_pause_digital *pause)
{
	return v92_rate_pause_digital_is_valid(pause) && !pause->failed &&
		pcm_r_tx_is_complete(&pause->rt);
}

bool v92_rate_pause_analog_transmit(struct v92_rate_pause_analog *pause, int16_t *sample)
{
	if (!v92_rate_pause_analog_is_valid(pause) || pause->failed || !sample ||
	    memory_regions_overlap(pause, sizeof(*pause), sample, sizeof(*sample)) ||
	    v92_rate_pause_analog_is_ready(pause))
		return false;
	if (!pause->sending_status) {
		struct v92_pcm_phase4_trn2u *training = &pause->upstream.training;
		if (training->emitted_symbols % V92_PCM_PHASE4_TRN2U_ALIGNMENT == 0u &&
		    (training->emitted_symbols == V92_RATE_PAUSE_TRN2U_SYMBOLS ||
		     (pause->peer_requested && pcm_r_rx_signal_detected(&pause->rt)))) {
			struct v92_phase4_analog_tx status;
			if (v92_phase4_analog_tx_resume(&status, &training->modulator, &pause->cpu) !=
			    V92_PHASE4_ANALOG_TX_OK)
				goto fail;
			pause->upstream.status = status;
			pause->sending_status = true;
		} else if (v92_pcm_phase4_trn2u_next(training, sample) == V92_PCM_PHASE4_TRN2U_OK)
			return true;
		else
			goto fail;
	}
	if (v92_phase4_analog_tx_next(&pause->upstream.status, sample) == V92_PHASE4_ANALOG_TX_OK)
		return true;
fail:
	pause->failed = true;
	return false;
}

bool v92_rate_pause_analog_receive(struct v92_rate_pause_analog *pause, uint8_t octet)
{
	if (!v92_rate_pause_analog_is_valid(pause) || pause->failed ||
	    pcm_r_rx_is_complete(&pause->rt))
		return false;
	if (!pcm_downstream_rx_is_complete(&pause->downstream)) {
		struct pcm_downstream_event event;
		enum pcm_downstream_rx_result result = pcm_downstream_rx_receive(&pause->downstream, octet, &event);
		if (result == PCM_DOWNSTREAM_RX_SUVD &&
		    event.control.record.suvd.parameters.acknowledges_cpu &&
		    pcm_downstream_rx_expect_ed(&pause->downstream) == PCM_DOWNSTREAM_RX_OK)
			return true;
		if (result == PCM_DOWNSTREAM_RX_BUFFERING || result == PCM_DOWNSTREAM_RX_ED ||
		    result == PCM_DOWNSTREAM_RX_SUVD_REJECTED)
			return true;
	} else {
		enum pcm_r_rx_result result = pcm_r_rx_receive(&pause->rt, octet);
		if (result == PCM_R_RX_BAR_COMPLETE) {
			if (pcm_downstream_rx_resume_after_rt(&pause->downstream, true) == PCM_DOWNSTREAM_RX_OK)
				return true;
		} else if (result == PCM_R_RX_BUFFERING || result == PCM_R_RX_SIGNAL_DETECTED ||
			   result == PCM_R_RX_BAR_STARTED || result == PCM_R_RX_SEQUENCE_MISMATCH)
			return true;
	}
	pause->failed = true;
	return false;
}

static bool peer_ready(const struct v92_rate_pause_digital *pause)
{
	return (pause->event.type == V92_CONTROL_EVENT_SUVU &&
		!pause->event.record.suvu.parameters.request_silence &&
		!pause->event.record.suvu.parameters.acknowledges_cpd) ||
		pause->event.type == V92_CONTROL_EVENT_CPU || pause->event.type == V92_CONTROL_EVENT_CPUS;
}

bool v92_rate_pause_digital_transmit(struct v92_rate_pause_digital *pause, uint8_t *octet)
{
	struct pcm_downstream_symbol emitted;
	if (!v92_rate_pause_digital_is_valid(pause) || pause->failed || !octet ||
	    memory_regions_overlap(pause, sizeof(*pause), octet, sizeof(*octet)) ||
	    v92_rate_pause_digital_is_ready(pause))
		return false;
	if (!pcm_downstream_tx_is_complete(&pause->downstream)) {
		enum pcm_downstream_tx_result result = pcm_downstream_tx_next(&pause->downstream, &emitted);
		if (result == PCM_DOWNSTREAM_TX_OK) {
			*octet = emitted.pcm.octet;
			return true;
		}
		/* COMPLETE drains no character; this same interval begins silence. */
		if (result != PCM_DOWNSTREAM_TX_COMPLETE)
			goto fail;
	}
	/* Local policy: allow 300 ms of silence for training when the peer did
	 * not request it. A peer-requested pause waits for its fresh clear SUVu.
	 * Both paths retain the six-symbol frame phase across silence and Rt. */
	if (!pause->sending_rt && pause->silence_symbols % V90_CP_FRAME_INTERVALS == 0u &&
	    (peer_ready(pause) || (!pause->peer_requested && pause->silence_symbols >= 2400u)))
		pause->sending_rt = true;
	if (pause->sending_rt) {
		if (pcm_r_tx_next(&pause->rt, &emitted.pcm) != PCM_R_TX_OK)
			goto fail;
		if (pcm_r_tx_is_complete(&pause->rt) &&
		    pcm_downstream_tx_resume_after_rt(&pause->downstream) != PCM_DOWNSTREAM_TX_OK)
			goto fail;
	} else {
		if (pause->silence_symbols == UINT32_MAX ||
		    !v90_pcm_symbol_make(&emitted.pcm, pause->downstream.encoder.mapping.law, 0u, true))
			goto fail;
		pause->silence_symbols++;
	}
	*octet = emitted.pcm.octet;
	return true;
fail:
	pause->failed = true;
	return false;
}

bool v92_rate_pause_digital_receive(struct v92_rate_pause_digital *pause, int16_t sample)
{
	struct v92_control_event event;
	if (!v92_rate_pause_digital_is_valid(pause) || pause->failed)
		return false;
	enum v92_upstream_control_line_rx_result result =
		v92_upstream_control_line_rx_sample(&pause->upstream, sample, &event);
	if (result == V92_UPSTREAM_CONTROL_LINE_RX_OK)
		return true;
	if (result == V92_UPSTREAM_CONTROL_LINE_RX_EVENT) {
		/* Zero-rate requests remain available for the resumed cleardown
		 * owner and take precedence over status, including later repeats. */
		if ((event.type == V92_CONTROL_EVENT_CPU && event.record.cpu.parameters.data_rate_number != 0u) ||
		    (event.type == V92_CONTROL_EVENT_CPUS && event.record.cpus.parameters.data_rate_number != 0u))
			goto fail;
		if (event.type == V92_CONTROL_EVENT_CPU || event.type == V92_CONTROL_EVENT_CPUS ||
		    (event.type == V92_CONTROL_EVENT_SUVU &&
		     pause->event.type != V92_CONTROL_EVENT_CPU && pause->event.type != V92_CONTROL_EVENT_CPUS))
			pause->event = event;
		return true;
	}
fail:
	pause->failed = true;
	return false;
}

bool v92_rate_pause_analog_resume(struct v92_phase4_analog_station *station,
	const struct v92_rate_pause_analog *pause, const struct v92_pcm_mapping *upstream,
	uint16_t round_trip_delay_samples)
{
	if (!station || !v92_rate_pause_analog_is_ready(pause) ||
	    memory_regions_overlap(station, sizeof(*station), pause, sizeof(*pause)) ||
	    !v92_pcm_mapping_is_valid(upstream) || upstream->law != pause->cpu.law)
		return false;
	struct v92_phase4_analog_station next = {
		.transmit = pause->upstream.status,
		.receive = pause->downstream,
		.mapping = *upstream,
		.round_trip_delay_samples = round_trip_delay_samples,
	};
	next.mapping.cpd.acknowledges_cpu = false;
	if (!v92_phase4_analog_station_is_valid(&next) ||
	    next.receive.stage != PCM_DOWNSTREAM_RX_V92_CONTROL ||
	    !next.transmit.control.renegotiation || next.transmit.control.suvu.request_silence ||
	    next.transmit.control.suvu.acknowledges_cpd || next.transmit.control.cpu.acknowledges_cpd)
		return false;
	*station = next;
	return true;
}

bool v92_rate_pause_digital_resume(struct v92_phase4_digital_station *station,
	const struct v92_rate_pause_digital *pause, const struct v92_cpd_parameters *cpd,
	const struct v92_upstream_plan *plan, const struct v92_cpu_parameters *cpu,
	uint16_t round_trip_delay_samples)
{
	if (!station || !v92_rate_pause_digital_is_ready(pause) ||
	    memory_regions_overlap(station, sizeof(*station), pause, sizeof(*pause)) ||
	    !v92_cpd_parameters_are_valid(cpd) || !cpd->data_rate_number ||
	    !v92_upstream_plan_is_valid(plan) || !v92_cpu_parameters_are_valid(cpu) ||
	    !cpu->data_rate_number)
		return false;
	struct v92_phase4_digital_station next = { .upstream_plan = *plan };
	struct v92_cpd_parameters selected = *cpd;
	const struct v92_suvd_parameters suvd = { 0 };
	struct v92_control_event event = pause->event;
	selected.acknowledges_cpu = false;
	selected.extend_e2u = false;
	if (event.type == V92_CONTROL_EVENT_SUVU &&
	    (event.record.suvu.frame_result != V92_SUVU_OK ||
	     !v92_suvu_parameters_are_valid(&event.record.suvu.parameters)))
		return false;
	/* Only fresh cleared SUVu may qualify the resumed parameter exchange.
	 * A preceding pause acknowledgement is not confirmation of this CPd. */
	if (event.type == V92_CONTROL_EVENT_SUVU &&
	    (event.record.suvu.parameters.request_silence || event.record.suvu.parameters.acknowledges_cpd))
		event = (struct v92_control_event){ 0 };
	if ((event.type != V92_CONTROL_EVENT_NONE && event.type != V92_CONTROL_EVENT_SUVU &&
	     event.type != V92_CONTROL_EVENT_CPU && event.type != V92_CONTROL_EVENT_CPUS) ||
	    (event.type == V92_CONTROL_EVENT_CPU && event.record.cpu.parameters.data_rate_number) ||
	    (event.type == V92_CONTROL_EVENT_CPUS && event.record.cpus.parameters.data_rate_number) ||
	    v92_phase4_digital_rx_resume(&next.receive, &pause->upstream) != V92_PHASE4_DIGITAL_RX_OK ||
	    !v92_downstream_control_init(&next.control, &pause->downstream, &suvd, &selected, cpu,
		    round_trip_delay_samples) ||
	    v92_downstream_control_receive(&next.control, &event) != V92_DOWNSTREAM_CONTROL_OK ||
	    !v92_phase4_digital_station_is_valid(&next))
		return false;
	*station = next;
	return true;
}
