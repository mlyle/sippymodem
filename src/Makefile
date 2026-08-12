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

CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -O2 -g
LDFLAGS ?=
LDLIBS ?=

PJSIP_PACKAGE ?= libpjproject
PJSIP_AVAILABLE := $(shell \
	pkg-config --exists $(PJSIP_PACKAGE) 2>/dev/null && echo yes)
PJSIP_CFLAGS := $(shell pkg-config --cflags $(PJSIP_PACKAGE) 2>/dev/null)
PJSIP_LDLIBS := $(shell \
	pkg-config --static --libs $(PJSIP_PACKAGE) 2>/dev/null)

DSP_SOURCE_DIR := ../dsp
DSP_V21_202_SOURCE_DIR := $(DSP_SOURCE_DIR)/v21_202
DSP_V22_SOURCE_DIR := $(DSP_SOURCE_DIR)/v22
DSP_V32_SOURCE_DIR := $(DSP_SOURCE_DIR)/v32
DSP_PCM_SOURCE_DIR := $(DSP_SOURCE_DIR)/pcm

CPPFLAGS += -D_POSIX_C_SOURCE=200809L -I. -I$(DSP_SOURCE_DIR) \
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
	g711.c \
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
	$(DSP_SOURCE_DIR)/crc16_ccitt_false.c \
	$(DSP_SOURCE_DIR)/datapumps.c \
	$(DSP_SOURCE_DIR)/dc_filter.c \
	$(DSP_SOURCE_DIR)/differential_qpsk.c \
	$(DSP_SOURCE_DIR)/fsk.c \
	$(DSP_SOURCE_DIR)/qam_equalizer.c \
	$(DSP_SOURCE_DIR)/qam.c \
	$(DSP_SOURCE_DIR)/qam_gain.c \
	$(DSP_SOURCE_DIR)/self_sync_scrambler.c \
	$(DSP_SOURCE_DIR)/single_tone.c \
	$(DSP_SOURCE_DIR)/v8_ansam.c \
	$(DSP_SOURCE_DIR)/v8_answering.c \
	$(DSP_SOURCE_DIR)/v8_calling.c \
	$(DSP_SOURCE_DIR)/v8_datapump.c \
	$(DSP_SOURCE_DIR)/v8_fsk.c \
	$(DSP_SOURCE_DIR)/v8_framing.c \
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
	$(DSP_V32_SOURCE_DIR)/datapump.c \
	$(DSP_V32_SOURCE_DIR)/uncoded_receiver.c \
	$(DSP_V32_SOURCE_DIR)/conditioning.c \
	$(DSP_V32_SOURCE_DIR)/conditioning_receiver.c \
	$(DSP_V32_SOURCE_DIR)/constellation.c \
	$(DSP_V32_SOURCE_DIR)/framing.c \
	$(DSP_V32_SOURCE_DIR)/line_startup.c \
	$(DSP_V32_SOURCE_DIR)/rate_signal.c \
	$(DSP_V32_SOURCE_DIR)/startup.c \
	$(DSP_V32_SOURCE_DIR)/trellis.c \
	$(DSP_V32_SOURCE_DIR)/trellis_acquirer.c \
	$(DSP_V32_SOURCE_DIR)/trellis_decoder.c \
	$(DSP_V32_SOURCE_DIR)/trellis_receiver.c
PCM_SOURCES := \
	$(DSP_PCM_SOURCE_DIR)/v34_4d_decoder.c \
	$(DSP_PCM_SOURCE_DIR)/v34_4d_encoder.c \
	$(DSP_PCM_SOURCE_DIR)/v34_data_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v34_mapping_frame_decoder.c \
	$(DSP_PCM_SOURCE_DIR)/v34_mapping_frame_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v34_constellation.c \
	$(DSP_PCM_SOURCE_DIR)/v34_data_carrier.c \
	$(DSP_PCM_SOURCE_DIR)/v34_data_points.c \
	$(DSP_PCM_SOURCE_DIR)/v34_data_line_transmitter.c \
	$(DSP_PCM_SOURCE_DIR)/v34_data_stream.c \
	$(DSP_PCM_SOURCE_DIR)/v34_data_transmitter.c \
	$(DSP_PCM_SOURCE_DIR)/v34_frame_schedule.c \
	$(DSP_PCM_SOURCE_DIR)/v34_framing.c \
	$(DSP_PCM_SOURCE_DIR)/v34_info0.c \
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
	$(DSP_PCM_SOURCE_DIR)/v34_data_channel_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v34_data_equalizer_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v34_data_line_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v34_equalizer_handoff.c \
	$(DSP_PCM_SOURCE_DIR)/v34_lattice.c \
	$(DSP_PCM_SOURCE_DIR)/v34_path_decoder.c \
	$(DSP_PCM_SOURCE_DIR)/v34_precoder.c \
	$(DSP_PCM_SOURCE_DIR)/v34_survivor.c \
	$(DSP_PCM_SOURCE_DIR)/v34_shell_mapper.c \
	$(DSP_PCM_SOURCE_DIR)/v34_symbol_power.c \
	$(DSP_PCM_SOURCE_DIR)/v34_trellis.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_dpsk.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_initial_probe.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_info0_exchange.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_info1_exchange.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_info_audio.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_info.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_probe.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_probe_acquisition.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_probe_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_probe_spectrum.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_probe_synchronizer.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_probe_sequence.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_probe_turnaround.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_ranging.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_reciprocal_probe.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_tone.c \
	$(DSP_PCM_SOURCE_DIR)/v34_preemphasis.c \
	$(DSP_PCM_SOURCE_DIR)/v34_training_line.c \
	$(DSP_PCM_SOURCE_DIR)/v34_caller_training.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_caller.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_profile.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_pp_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_pp_sequence.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_s_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_s_sequence.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_s_transmitter.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_trn.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_j_sequence.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_j_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_j_equalizer_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_line_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase4_b1_handoff.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase4_e_channel_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase4_e_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase4_e_sequence.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase4_j_prime_sequence.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase4_mp.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase4_mp_channel_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase4_equalizer_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase4_line_handoff.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase4_line_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase4_mp_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/v34_phase4_trn.c \
	$(DSP_PCM_SOURCE_DIR)/v34_training_channel.c
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

slmodemd: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)
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

clean:
	rm -f slmodemd $(SOURCES:.c=.o) $(DEPENDENCIES)
	rm -f modem_test modem_main.o modem_cmdline.o modem_test.o
	rm -f modem_datafile.o dp_dummy.o dp_sinus.o
	rm -f .build_profile .depend

-include $(DEPENDENCIES)
