###########################################################################
#
#
#       Makefile  --  modem application Makefile.
#
#       Copyright(c) 2003, Smart Link Ltd. (www.smlink.com)
#       All rights reserved.
#
#       Author: Sasha K (sashak@smlink.com)
#
#
###########################################################################

include ../build/compiler.mk

PYTHON ?= python3
CPPFLAGS ?=
CFLAGS ?= -O2 -g
LDFLAGS ?=
LDLIBS ?=
PKG_CONFIG ?= pkg-config

PJSIP_PACKAGE ?= libpjproject
PJSIP_DEPENDENCY ?=
PJSIP_AVAILABLE := $(shell \
	$(PKG_CONFIG) --exists $(PJSIP_PACKAGE) 2>/dev/null && echo yes)
PJSIP_CFLAGS := $(shell \
	$(PKG_CONFIG) --cflags $(PJSIP_PACKAGE) 2>/dev/null)
PJSIP_LDLIBS := $(shell \
	$(PKG_CONFIG) --static --libs $(PJSIP_PACKAGE) 2>/dev/null)

DSP_SOURCE_DIR := ../dsp
DSP_V21_202_SOURCE_DIR := $(DSP_SOURCE_DIR)/v21_202
DSP_V22_SOURCE_DIR := $(DSP_SOURCE_DIR)/v22
DSP_V32_SOURCE_DIR := $(DSP_SOURCE_DIR)/v32
DSP_PCM_SOURCE_DIR := $(DSP_SOURCE_DIR)/pcm

PHASE_PROFILE_HEADER := ../.build/generated/phase_profile.h
PREEMPHASIS_PROFILE_HEADER := ../.build/generated/v34_preemphasis_profiles.h
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -I. -I$(DSP_SOURCE_DIR) \
	-I$(dir $(PHASE_PROFILE_HEADER)) \
	-I$(DSP_V21_202_SOURCE_DIR) -I$(DSP_V22_SOURCE_DIR) \
	-I$(DSP_V32_SOURCE_DIR) -I$(DSP_PCM_SOURCE_DIR)
CFLAGS += -std=gnu11 -Wall -Wextra
LDLIBS += $(PJSIP_LDLIBS) -lm

MODEM_CORE_SOURCES := \
	homolog_data.c \
	modem.c \
	modem_at.c \
	modem_comp.c \
	modem_debug.c \
	modem_ec.c \
	modem_pack.c \
	modem_param.c \
	modem_timer.c \
	sysdep_common.c
APP_SOURCES := \
	audio_path.c \
	audio_scheduler.c \
	config.c \
	frame_clock.c \
	host.c \
	lapm_v44_session.c \
	loop.c \
	main.c \
	media.c \
	recording_files.c \
	rtp.c \
	rtp_endpoint.c \
	rtp_receiver.c \
	rtp_runtime.c \
	rtp_stream.c \
	runtime.c \
	sdp.c \
	sdp_pjsip.c \
	session.c \
	sip_media_bridge.c \
	sip_rtp.c \
	sip_signaling_pjsip.c \
	sip_uri.c \
	slmodem_adapter.c \
	slmodem_calls.c \
	slmodem_driver.c \
	slmodem_terminal.c \
	terminal.c \
	v42_xid.c \
	v44_character_path.c \
	v44_compressed_decoder.c \
	v44_decoder.c \
	v44_duplex.c \
	v44_dictionary_encoder.c \
	v44_dictionary_decoder.c \
	v44_encoder.c \
	v44_stream.c \
	v44_transparent_decoder.c \
	v44_tx_monitor.c \
	v44_xid.c
SHARED_DSP_SOURCES := \
	$(DSP_SOURCE_DIR)/agc.c \
	$(DSP_SOURCE_DIR)/dc_remover.c \
	$(DSP_SOURCE_DIR)/binary_equalizer.c \
	$(DSP_SOURCE_DIR)/crc16_ccitt_false.c \
	$(DSP_SOURCE_DIR)/datapumps.c \
	$(DSP_SOURCE_DIR)/datapump_registry.c \
	$(DSP_SOURCE_DIR)/differential_qpsk.c \
	$(DSP_SOURCE_DIR)/fsk.c \
	$(DSP_SOURCE_DIR)/fractional_delay.c \
	$(DSP_SOURCE_DIR)/fixed_phase.c \
	$(DSP_SOURCE_DIR)/g711.c \
	$(DSP_SOURCE_DIR)/mixed_radix.c \
	$(DSP_SOURCE_DIR)/qam_equalizer.c \
	$(DSP_SOURCE_DIR)/qam.c \
	$(DSP_SOURCE_DIR)/qam_gain.c \
	$(DSP_SOURCE_DIR)/self_sync_scrambler.c \
	$(DSP_SOURCE_DIR)/single_tone.c \
	$(DSP_SOURCE_DIR)/training_session.c \
	$(DSP_SOURCE_DIR)/v8_ansam.c \
	$(DSP_SOURCE_DIR)/v8_answering.c \
	$(DSP_SOURCE_DIR)/v8_calling.c \
	$(DSP_SOURCE_DIR)/v8_datapump.c \
	$(DSP_SOURCE_DIR)/v8_fsk.c \
	$(DSP_SOURCE_DIR)/v8_framing.c \
	$(DSP_SOURCE_DIR)/v8_line_conditioner.c \
	$(DSP_SOURCE_DIR)/v8_menu.c
V21_202_SOURCES := \
	$(DSP_V21_202_SOURCE_DIR)/baudot.c \
	$(DSP_V21_202_SOURCE_DIR)/baudot_datapump.c \
	$(DSP_V21_202_SOURCE_DIR)/fsk300_datapump.c \
	$(DSP_V21_202_SOURCE_DIR)/v21.c \
	$(DSP_V21_202_SOURCE_DIR)/v23_datapump.c
V22_SOURCES := \
	$(DSP_V22_SOURCE_DIR)/v22_datapump.c \
	$(DSP_V22_SOURCE_DIR)/v22_framing.c \
	$(DSP_V22_SOURCE_DIR)/v22bis_receiver.c \
	$(DSP_V22_SOURCE_DIR)/v22bis_symbols.c
V32_SOURCES := \
	$(DSP_V32_SOURCE_DIR)/b1_path.c \
	$(DSP_V32_SOURCE_DIR)/carrier.c \
	$(DSP_V32_SOURCE_DIR)/conditioning_path.c \
	$(DSP_V32_SOURCE_DIR)/datapump.c \
	$(DSP_V32_SOURCE_DIR)/data_path.c \
	$(DSP_V32_SOURCE_DIR)/echo_canceller.c \
	$(DSP_V32_SOURCE_DIR)/echo_frontend.c \
	$(DSP_V32_SOURCE_DIR)/echo_training.c \
	$(DSP_V32_SOURCE_DIR)/end_path.c \
	$(DSP_V32_SOURCE_DIR)/equalizer.c \
	$(DSP_V32_SOURCE_DIR)/line_receiver.c \
	$(DSP_V32_SOURCE_DIR)/rate_path.c \
	$(DSP_V32_SOURCE_DIR)/receive_path.c \
	$(DSP_V32_SOURCE_DIR)/symbol_timing.c \
	$(DSP_V32_SOURCE_DIR)/trellis_decoder.c \
	$(DSP_V32_SOURCE_DIR)/trellis_profile.c \
	$(DSP_V32_SOURCE_DIR)/trellis_receiver.c \
	$(DSP_V32_SOURCE_DIR)/uncoded_receiver.c \
	$(DSP_V32_SOURCE_DIR)/conditioning.c \
	$(DSP_V32_SOURCE_DIR)/conditioning_receiver.c \
	$(DSP_V32_SOURCE_DIR)/constellation.c \
	$(DSP_V32_SOURCE_DIR)/decision.c \
	$(DSP_V32_SOURCE_DIR)/framing.c \
	$(DSP_V32_SOURCE_DIR)/line_startup.c \
	$(DSP_V32_SOURCE_DIR)/rate_signal.c \
	$(DSP_V32_SOURCE_DIR)/startup.c \
	$(DSP_V32_SOURCE_DIR)/trellis.c
PCM_SOURCES := \
	$(DSP_PCM_SOURCE_DIR)/pcm_gain.c \
	$(DSP_PCM_SOURCE_DIR)/v92_observation.c \
	$(DSP_PCM_SOURCE_DIR)/pcm_startup.c \
	$(DSP_PCM_SOURCE_DIR)/pcm_info1.c \
	$(DSP_PCM_SOURCE_DIR)/pcm_phase3_profile.c \
	$(DSP_PCM_SOURCE_DIR)/pcm_dil_descriptor.c \
	$(DSP_PCM_SOURCE_DIR)/pcm_binary_sign.c \
	$(DSP_PCM_SOURCE_DIR)/pcm_j_frame.c \
	$(DSP_PCM_SOURCE_DIR)/pcm_phase3_digital_prefix.c \
	$(DSP_PCM_SOURCE_DIR)/v34_call.c \
	$(DSP_PCM_SOURCE_DIR)/v34_cleardown.c \
	$(DSP_PCM_SOURCE_DIR)/v34_datapump.c \
	$(DSP_PCM_SOURCE_DIR)/v34_profile.c \
	$(DSP_PCM_SOURCE_DIR)/v34_rate_profile.c \
	$(DSP_PCM_SOURCE_DIR)/v34_data_quality.c \
	$(DSP_PCM_SOURCE_DIR)/v34_4d_decoder.c \
	$(DSP_PCM_SOURCE_DIR)/v34_4d_encoder.c \
	$(DSP_PCM_SOURCE_DIR)/v34_data_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v34_mapping_frame_decoder.c \
	$(DSP_PCM_SOURCE_DIR)/v34_mapping_frame_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v34_constellation.c \
	$(DSP_PCM_SOURCE_DIR)/v34_data_mapping.c \
	$(DSP_PCM_SOURCE_DIR)/v34_data_mode.c \
	$(DSP_PCM_SOURCE_DIR)/v34_data_startup.c \
	$(DSP_PCM_SOURCE_DIR)/v34_data_points.c \
	$(DSP_PCM_SOURCE_DIR)/v34_data_stream.c \
	$(DSP_PCM_SOURCE_DIR)/v34_frame_schedule.c \
	$(DSP_PCM_SOURCE_DIR)/v34_framing.c \
	$(DSP_PCM_SOURCE_DIR)/v34_info0.c \
	$(DSP_PCM_SOURCE_DIR)/pcm_call.c \
	$(DSP_PCM_SOURCE_DIR)/pcm_echo_reference.c \
	$(DSP_PCM_SOURCE_DIR)/pcm_rate.c \
	$(DSP_PCM_SOURCE_DIR)/v90_profile.c \
	$(DSP_PCM_SOURCE_DIR)/v92_profile.c \
	$(DSP_PCM_SOURCE_DIR)/v90_datapump.c \
	$(DSP_PCM_SOURCE_DIR)/pcm_host_echo.c \
	$(DSP_PCM_SOURCE_DIR)/v90_info0.c \
	$(DSP_PCM_SOURCE_DIR)/v92_info0.c \
	$(DSP_PCM_SOURCE_DIR)/v92_qc1.c \
	$(DSP_PCM_SOURCE_DIR)/v92_qc_signal.c \
	$(DSP_PCM_SOURCE_DIR)/v92_qc_digital.c \
	$(DSP_PCM_SOURCE_DIR)/v92_qc_analog.c \
	$(DSP_PCM_SOURCE_DIR)/v90_info1.c \
	$(DSP_PCM_SOURCE_DIR)/v92_info1d.c \
	$(DSP_PCM_SOURCE_DIR)/v92_pcm_info1a.c \
	$(DSP_PCM_SOURCE_DIR)/v92_pcm_dil.c \
	$(DSP_PCM_SOURCE_DIR)/v92_pcm_phase3_ru.c \
	$(DSP_PCM_SOURCE_DIR)/v92_ru_rx.c \
	$(DSP_PCM_SOURCE_DIR)/v92_ru_line_rx.c \
	$(DSP_PCM_SOURCE_DIR)/v92_pcm_phase3_opening.c \
	$(DSP_PCM_SOURCE_DIR)/v92_pcm_phase3_trn1u.c \
	$(DSP_PCM_SOURCE_DIR)/v92_pcm_phase3_trn1u_line_rx.c \
	$(DSP_PCM_SOURCE_DIR)/v92_pcm_phase3_ja.c \
	$(DSP_PCM_SOURCE_DIR)/v92_pcm_phase3_initial_tx.c \
	$(DSP_PCM_SOURCE_DIR)/v92_pcm_phase3_initial_rx.c \
	$(DSP_PCM_SOURCE_DIR)/v92_phase3_sync.c \
	$(DSP_PCM_SOURCE_DIR)/v92_ja_decoder.c \
	$(DSP_PCM_SOURCE_DIR)/v92_pcm_phase3_ja_line_rx.c \
	$(DSP_PCM_SOURCE_DIR)/v92_j.c \
	$(DSP_PCM_SOURCE_DIR)/pcm_phase3_digital_tx.c \
	$(DSP_PCM_SOURCE_DIR)/pcm_phase3_j_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v92_su.c \
	$(DSP_PCM_SOURCE_DIR)/v92_su_rx.c \
	$(DSP_PCM_SOURCE_DIR)/v92_cpt.c \
	$(DSP_PCM_SOURCE_DIR)/v92_cpu.c \
	$(DSP_PCM_SOURCE_DIR)/v92_cpd.c \
	$(DSP_PCM_SOURCE_DIR)/v92_pcm_mapping.c \
	$(DSP_PCM_SOURCE_DIR)/v92_pcm_frame.c \
	$(DSP_PCM_SOURCE_DIR)/v92_upstream_data_tx.c \
	$(DSP_PCM_SOURCE_DIR)/v92_upstream_data_rx.c \
	$(DSP_PCM_SOURCE_DIR)/v92_upstream_model.c \
	$(DSP_PCM_SOURCE_DIR)/v92_data_quality.c \
	$(DSP_PCM_SOURCE_DIR)/v92_cpd_direct.c \
	$(DSP_PCM_SOURCE_DIR)/v92_cpd_channel.c \
	$(DSP_PCM_SOURCE_DIR)/v92_phase4_cpd_tx.c \
	$(DSP_PCM_SOURCE_DIR)/v92_cleardown.c \
	$(DSP_PCM_SOURCE_DIR)/v92_cpt_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v92_pcm_phase3_cpt_tx.c \
	$(DSP_PCM_SOURCE_DIR)/v92_pcm_phase3_cpt_rx.c \
	$(DSP_PCM_SOURCE_DIR)/v92_pcm_phase3_cpt_line_rx.c \
	$(DSP_PCM_SOURCE_DIR)/v92_phase3_analog_station.c \
	$(DSP_PCM_SOURCE_DIR)/v92_phase3_digital_station.c \
	$(DSP_PCM_SOURCE_DIR)/v90_phase4_ri_search.c \
	$(DSP_PCM_SOURCE_DIR)/v92_phase4_modulator.c \
	$(DSP_PCM_SOURCE_DIR)/v92_phase4_record_tx.c \
	$(DSP_PCM_SOURCE_DIR)/v92_pcm_phase4_trn2u.c \
	$(DSP_PCM_SOURCE_DIR)/v92_pcm_phase4_trn2u_line_rx.c \
	$(DSP_PCM_SOURCE_DIR)/v92_upstream_rx.c \
	$(DSP_PCM_SOURCE_DIR)/v92_control_target.c \
	$(DSP_PCM_SOURCE_DIR)/v92_e2u_rx.c \
	$(DSP_PCM_SOURCE_DIR)/v92_phase4_e2u.c \
	$(DSP_PCM_SOURCE_DIR)/v92_rate_pause.c \
	$(DSP_PCM_SOURCE_DIR)/v92_upstream_control_line_rx.c \
	$(DSP_PCM_SOURCE_DIR)/v92_phase4_digital_rx.c \
	$(DSP_PCM_SOURCE_DIR)/v92_upstream_channel.c \
	$(DSP_PCM_SOURCE_DIR)/v92_phase4_analog_station.c \
	$(DSP_PCM_SOURCE_DIR)/v92_phase4_digital_station.c \
	$(DSP_PCM_SOURCE_DIR)/v92_downstream_control.c \
	$(DSP_PCM_SOURCE_DIR)/v92_status_update.c \
	$(DSP_PCM_SOURCE_DIR)/v92_suvu.c \
	$(DSP_PCM_SOURCE_DIR)/v92_suvd.c \
	$(DSP_PCM_SOURCE_DIR)/v92_control_rx.c \
	$(DSP_PCM_SOURCE_DIR)/v92_phase4_analog_tx.c \
	$(DSP_PCM_SOURCE_DIR)/v92_upstream_control.c \
	$(DSP_PCM_SOURCE_DIR)/v90_mp.c \
	$(DSP_PCM_SOURCE_DIR)/v90_mp_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/pcm_downstream_rx.c \
	$(DSP_PCM_SOURCE_DIR)/pcm_octet_probe.c \
	$(DSP_PCM_SOURCE_DIR)/v91_profile.c \
	$(DSP_PCM_SOURCE_DIR)/v91_control.c \
	$(DSP_PCM_SOURCE_DIR)/v91_call.c \
	$(DSP_PCM_SOURCE_DIR)/v91_datapump.c \
	$(DSP_PCM_SOURCE_DIR)/pcm_downstream_equalizer.c \
	$(DSP_PCM_SOURCE_DIR)/v90_cp.c \
	$(DSP_PCM_SOURCE_DIR)/v90_cp_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v90_pcm_mapping.c \
	$(DSP_PCM_SOURCE_DIR)/v90_pcm_frame_decoder.c \
	$(DSP_PCM_SOURCE_DIR)/v90_pcm_frame_encoder.c \
	$(DSP_PCM_SOURCE_DIR)/v90_downstream_data_rx.c \
	$(DSP_PCM_SOURCE_DIR)/v90_downstream_data_tx.c \
	$(DSP_PCM_SOURCE_DIR)/v90_downstream_data_handoff.c \
	$(DSP_PCM_SOURCE_DIR)/v90_downstream_data_stream.c \
	$(DSP_PCM_SOURCE_DIR)/pcm_r.c \
	$(DSP_PCM_SOURCE_DIR)/v90_analog_path.c \
	$(DSP_PCM_SOURCE_DIR)/v90_digital_path.c \
	$(DSP_PCM_SOURCE_DIR)/v90_sign_decoder.c \
	$(DSP_PCM_SOURCE_DIR)/v90_sign_encoder.c \
	$(DSP_PCM_SOURCE_DIR)/v90_spectral_shaper.c \
	$(DSP_PCM_SOURCE_DIR)/v90_pcm_stream_rx.c \
	$(DSP_PCM_SOURCE_DIR)/pcm_downstream_tx.c \
	$(DSP_PCM_SOURCE_DIR)/v90_phase4_startup.c \
	$(DSP_PCM_SOURCE_DIR)/v90_phase4_analog_startup.c \
	$(DSP_PCM_SOURCE_DIR)/v90_phase4_digital_startup.c \
	$(DSP_PCM_SOURCE_DIR)/v90_phase4_analog_station.c \
	$(DSP_PCM_SOURCE_DIR)/v90_phase4_cp_line_rx.c \
	$(DSP_PCM_SOURCE_DIR)/v90_phase4_cp_line_tx.c \
	$(DSP_PCM_SOURCE_DIR)/v90_phase4_cp_rx.c \
	$(DSP_PCM_SOURCE_DIR)/v90_phase4_cp_tx.c \
	$(DSP_PCM_SOURCE_DIR)/v90_phase4_e_line_rx.c \
	$(DSP_PCM_SOURCE_DIR)/v90_phase4_e_line_tx.c \
	$(DSP_PCM_SOURCE_DIR)/v90_phase4_digital_station.c \
	$(DSP_PCM_SOURCE_DIR)/v90_upstream_data.c \
	$(DSP_PCM_SOURCE_DIR)/v90_upstream_mode.c \
	$(DSP_PCM_SOURCE_DIR)/v90_upstream_rx.c \
	$(DSP_PCM_SOURCE_DIR)/v90_dil.c \
	$(DSP_PCM_SOURCE_DIR)/v90_dil_sequence.c \
	$(DSP_PCM_SOURCE_DIR)/pcm_dil_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v90_ja.c \
	$(DSP_PCM_SOURCE_DIR)/v90_ja_search.c \
	$(DSP_PCM_SOURCE_DIR)/v90_phase3_ja_rx.c \
	$(DSP_PCM_SOURCE_DIR)/v90_jd.c \
	$(DSP_PCM_SOURCE_DIR)/v90_pcm_symbol.c \
	$(DSP_PCM_SOURCE_DIR)/v90_phase3_analog_line_rx.c \
	$(DSP_PCM_SOURCE_DIR)/v90_upstream_training_rx.c \
	$(DSP_PCM_SOURCE_DIR)/v90_phase3_analog_line_tx.c \
	$(DSP_PCM_SOURCE_DIR)/v90_phase3_analog_station.c \
	$(DSP_PCM_SOURCE_DIR)/v90_phase3_analog_tx.c \
	$(DSP_PCM_SOURCE_DIR)/v90_phase3_digital_station.c \
	$(DSP_PCM_SOURCE_DIR)/pcm_phase3_entry.c \
	$(DSP_PCM_SOURCE_DIR)/v92_phase3_entry.c \
	$(DSP_PCM_SOURCE_DIR)/pcm_phase3_handoff.c \
	$(DSP_PCM_SOURCE_DIR)/v90_phase3_return_line_rx.c \
	$(DSP_PCM_SOURCE_DIR)/v90_phase3_return_line_tx.c \
	$(DSP_PCM_SOURCE_DIR)/v90_phase3_return_marker.c \
	$(DSP_PCM_SOURCE_DIR)/v90_phase3_return_tx.c \
	$(DSP_PCM_SOURCE_DIR)/v90_phase3_sd.c \
	$(DSP_PCM_SOURCE_DIR)/pcm_s_pattern_rx.c \
	$(DSP_PCM_SOURCE_DIR)/v90_phase3_signs.c \
	$(DSP_PCM_SOURCE_DIR)/v90_phase4_ri.c \
	$(DSP_PCM_SOURCE_DIR)/pcm_phase2_info0_exchange.c \
	$(DSP_PCM_SOURCE_DIR)/pcm_phase2_ranging.c \
	$(DSP_PCM_SOURCE_DIR)/v92_short_ranging.c \
	$(DSP_PCM_SOURCE_DIR)/v90_phase2_initial_probe.c \
	$(DSP_PCM_SOURCE_DIR)/v90_phase2_probe_turnaround.c \
	$(DSP_PCM_SOURCE_DIR)/v90_phase2_reciprocal_probe.c \
	$(DSP_PCM_SOURCE_DIR)/pcm_phase2_info1_exchange.c \
	$(DSP_PCM_SOURCE_DIR)/pcm_phase2.c \
	$(DSP_PCM_SOURCE_DIR)/pcm_phase2_info1_handoff.c \
	$(DSP_PCM_SOURCE_DIR)/v34_info1.c \
	$(DSP_PCM_SOURCE_DIR)/v34_mapping_frame_encoder.c \
	$(DSP_PCM_SOURCE_DIR)/v34_mapping_frame_transmitter.c \
	$(DSP_PCM_SOURCE_DIR)/v34_mapping_parameters.c \
	$(DSP_PCM_SOURCE_DIR)/v34_mapper.c \
	$(DSP_PCM_SOURCE_DIR)/v34_mp.c \
	$(DSP_PCM_SOURCE_DIR)/v34_nonlinear.c \
	$(DSP_PCM_SOURCE_DIR)/v34_parser.c \
	$(DSP_PCM_SOURCE_DIR)/v34_power_calibration.c \
	$(DSP_PCM_SOURCE_DIR)/v34_branches.c \
	$(DSP_PCM_SOURCE_DIR)/v34_channel_conditioner.c \
	$(DSP_PCM_SOURCE_DIR)/v34_channel_decoder.c \
	$(DSP_PCM_SOURCE_DIR)/v34_channel_frame_decoder.c \
	$(DSP_PCM_SOURCE_DIR)/v34_channel_frame_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v34_analytic_filter.c \
	$(DSP_PCM_SOURCE_DIR)/v34_carrier_demodulator.c \
	$(DSP_PCM_SOURCE_DIR)/v34_data_channel_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v34_data_decision_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v34_data_line_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v34_equalizer.c \
	$(DSP_PCM_SOURCE_DIR)/v34_equalizer_handoff.c \
	$(DSP_PCM_SOURCE_DIR)/v34_gain_control.c \
	$(DSP_PCM_SOURCE_DIR)/v34_lattice.c \
	$(DSP_PCM_SOURCE_DIR)/v34_line_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v34_receive_timing.c \
	$(DSP_PCM_SOURCE_DIR)/v34_s_tone_detector.c \
	$(DSP_PCM_SOURCE_DIR)/v34_adaptive_equalizer.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase34_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v34_path_decoder.c \
	$(DSP_PCM_SOURCE_DIR)/v34_precoder.c \
	$(DSP_PCM_SOURCE_DIR)/v34_precoder_trainer.c \
	$(DSP_PCM_SOURCE_DIR)/v34_survivor.c \
	$(DSP_PCM_SOURCE_DIR)/v34_shell_mapper.c \
	$(DSP_PCM_SOURCE_DIR)/v34_symbol_power.c \
	$(DSP_PCM_SOURCE_DIR)/v34_timing_control.c \
	$(DSP_PCM_SOURCE_DIR)/v34_timing_filter.c \
	$(DSP_PCM_SOURCE_DIR)/v34_trellis.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_selection.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_dpsk.c \
	$(DSP_PCM_SOURCE_DIR)/phase2_initial_probe.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_initial_probe.c \
	$(DSP_PCM_SOURCE_DIR)/phase2_info0_exchange.c \
	$(DSP_PCM_SOURCE_DIR)/phase2_ranging.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_info0_exchange.c \
	$(DSP_PCM_SOURCE_DIR)/phase2_info1_exchange.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_info1_exchange.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_info_audio.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_info.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_probe.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_probe_acquisition.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_probe_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_probe_spectrum.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_probe_synchronizer.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_probe_sequence.c \
	$(DSP_PCM_SOURCE_DIR)/phase2_probe_turnaround.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_probe_turnaround.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_ranging.c \
	$(DSP_PCM_SOURCE_DIR)/phase2_reciprocal_probe.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_reciprocal_probe.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_tone.c \
	$(DSP_PCM_SOURCE_DIR)/v34_pulse_modulator.c \
	$(DSP_PCM_SOURCE_DIR)/v34_training_line.c \
	$(DSP_PCM_SOURCE_DIR)/v34_training_transmitter.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_transmitter.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_decision_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_exchange.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase4_exchange.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_profile.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_pp_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_pp_sequence.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_s_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_s_sequence.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_j_sequence.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_j_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase4_e_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase4_e_sequence.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase4_j_prime_sequence.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase4_mp.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase4_decision_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase4_mp_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v34_training_sequence.c
DSP_SOURCES := $(SHARED_DSP_SOURCES) \
	$(V21_202_SOURCES) $(V22_SOURCES) \
	$(V32_SOURCES) $(PCM_SOURCES)
SOURCES := $(MODEM_CORE_SOURCES) $(APP_SOURCES) \
	$(DSP_SOURCES)
OBJECTS := $(SOURCES:.c=.o)
DEPENDENCIES := $(SOURCES:.c=.d)

.PHONY: all clean

ifeq ($(PJSIP_AVAILABLE),yes)
all: slmodemd

slmodemd: $(OBJECTS) $(PJSIP_DEPENDENCY)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)
else
.PHONY: slmodemd

all slmodemd:
	@echo "error: $(PJSIP_PACKAGE) development files are required to build slmodemd" >&2
	@false
endif

sdp_pjsip.o sip_signaling_pjsip.o: \
	CPPFLAGS += -D_DEFAULT_SOURCE $(PJSIP_CFLAGS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(DSP_SOURCE_DIR)/fixed_phase.o: $(PHASE_PROFILE_HEADER)

$(PHASE_PROFILE_HEADER): $(DSP_SOURCE_DIR)/generate_phase_profile.py
	$(PYTHON) $< $@

$(DSP_PCM_SOURCE_DIR)/v34_pulse_modulator.o: $(PREEMPHASIS_PROFILE_HEADER)

$(PREEMPHASIS_PROFILE_HEADER): $(DSP_PCM_SOURCE_DIR)/generate_preemphasis_profiles.py
	$(PYTHON) $< $@

clean:
	rm -f slmodemd $(SOURCES:.c=.o) $(DEPENDENCIES)
	rm -f modem_test modem_main.o modem_cmdline.o modem_test.o
	rm -f modem_datafile.o dp_dummy.o dp_sinus.o
	rm -f .build_profile .depend
	rm -f $(PHASE_PROFILE_HEADER) $(PREEMPHASIS_PROFILE_HEADER)

-include $(DEPENDENCIES)
