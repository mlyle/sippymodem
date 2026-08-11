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
SIPPYMODEM_SOURCES := \
	sippymodem_audio_path.c \
	sippymodem_audio_scheduler.c \
	sippymodem_config.c \
	sippymodem_frame_clock.c \
	sippymodem_g711.c \
	sippymodem_host.c \
	sippymodem_loop.c \
	sippymodem_main.c \
	sippymodem_media.c \
	sippymodem_recording_files.c \
	sippymodem_rtp.c \
	sippymodem_rtp_endpoint.c \
	sippymodem_rtp_receiver.c \
	sippymodem_rtp_runtime.c \
	sippymodem_rtp_stream.c \
	sippymodem_runtime.c \
	sippymodem_sdp.c \
	sippymodem_sdp_pjsip.c \
	sippymodem_session.c \
	sippymodem_sip_media_bridge.c \
	sippymodem_sip_rtp.c \
	sippymodem_sip_signaling_pjsip.c \
	sippymodem_sip_uri.c \
	sippymodem_slmodem_adapter.c \
	sippymodem_slmodem_calls.c \
	sippymodem_slmodem_driver.c \
	sippymodem_slmodem_terminal.c \
	sippymodem_terminal.c \
	sippymodem_v44_compressed_decoder.c \
	sippymodem_v44_decoder.c \
	sippymodem_v44_dictionary_decoder.c \
	sippymodem_v44_stream.c \
	sippymodem_v44_transparent_decoder.c
SIPPYMODEM_SHARED_DSP_SOURCES := \
	$(DSP_SOURCE_DIR)/sippymodem_crc16_ccitt_false.c \
	$(DSP_SOURCE_DIR)/sippymodem_datapumps.c \
	$(DSP_SOURCE_DIR)/sippymodem_dc_filter.c \
	$(DSP_SOURCE_DIR)/sippymodem_differential_qpsk.c \
	$(DSP_SOURCE_DIR)/sippymodem_fsk.c \
	$(DSP_SOURCE_DIR)/sippymodem_qam.c \
	$(DSP_SOURCE_DIR)/sippymodem_qam_gain.c \
	$(DSP_SOURCE_DIR)/sippymodem_self_sync_scrambler.c \
	$(DSP_SOURCE_DIR)/sippymodem_single_tone.c \
	$(DSP_SOURCE_DIR)/sippymodem_v8_ansam.c \
	$(DSP_SOURCE_DIR)/sippymodem_v8_answering.c \
	$(DSP_SOURCE_DIR)/sippymodem_v8_calling.c \
	$(DSP_SOURCE_DIR)/sippymodem_v8_datapump.c \
	$(DSP_SOURCE_DIR)/sippymodem_v8_fsk.c \
	$(DSP_SOURCE_DIR)/sippymodem_v8_framing.c \
	$(DSP_SOURCE_DIR)/sippymodem_v8_menu.c
SIPPYMODEM_V21_202_SOURCES := \
	$(DSP_V21_202_SOURCE_DIR)/sippymodem_baudot.c \
	$(DSP_V21_202_SOURCE_DIR)/sippymodem_baudot_datapump.c \
	$(DSP_V21_202_SOURCE_DIR)/sippymodem_fsk300_datapump.c \
	$(DSP_V21_202_SOURCE_DIR)/sippymodem_v21.c \
	$(DSP_V21_202_SOURCE_DIR)/sippymodem_v23_datapump.c
SIPPYMODEM_V22_SOURCES := \
	$(DSP_V22_SOURCE_DIR)/sippymodem_v22_datapump.c \
	$(DSP_V22_SOURCE_DIR)/sippymodem_v22_framing.c \
	$(DSP_V22_SOURCE_DIR)/sippymodem_v22bis_receiver.c \
	$(DSP_V22_SOURCE_DIR)/sippymodem_v22bis_symbols.c
SIPPYMODEM_V32_SOURCES := \
	$(DSP_V32_SOURCE_DIR)/sippymodem_v32_datapump.c \
	$(DSP_V32_SOURCE_DIR)/sippymodem_v32_9600_receiver.c \
	$(DSP_V32_SOURCE_DIR)/sippymodem_v32_conditioning.c \
	$(DSP_V32_SOURCE_DIR)/sippymodem_v32_conditioning_receiver.c \
	$(DSP_V32_SOURCE_DIR)/sippymodem_v32_constellation.c \
	$(DSP_V32_SOURCE_DIR)/sippymodem_v32_framing.c \
	$(DSP_V32_SOURCE_DIR)/sippymodem_v32_line_startup.c \
	$(DSP_V32_SOURCE_DIR)/sippymodem_v32_rate_signal.c \
	$(DSP_V32_SOURCE_DIR)/sippymodem_v32_startup.c \
	$(DSP_V32_SOURCE_DIR)/sippymodem_v32_trellis.c \
	$(DSP_V32_SOURCE_DIR)/sippymodem_v32_trellis_decoder.c
SIPPYMODEM_PCM_SOURCES := \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_4d_decoder.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_4d_encoder.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_constellation.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_data_carrier.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_data_line_transmitter.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_data_stream.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_data_transmitter.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_frame_schedule.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_framing.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_info0.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_info1.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_mapping_frame_encoder.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_mapping_frame_transmitter.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_mapping_parameters.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_mapper.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_mp.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_nonlinear_encoder.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_parser.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_power_calibration.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_precoder.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_shell_mapper.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_symbol_power.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_trellis.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase2_dpsk.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase2_initial_probe.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase2_info0_exchange.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase2_info1_exchange.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase2_info_audio.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase2_info.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase2_probe.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase2_probe_acquisition.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase2_probe_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase2_probe_spectrum.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase2_probe_synchronizer.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase2_probe_sequence.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase2_probe_turnaround.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase2_ranging.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase2_reciprocal_probe.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase2_tone.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_preemphasis.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase3_profile.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase3_pp_sequence.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase3_s_sequence.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase3_s_transmitter.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase3_trn.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase3_j_sequence.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase4_b1_handoff.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase4_e_channel_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase4_e_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase4_e_sequence.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase4_j_prime_sequence.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase4_mp.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase4_mp_channel_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase4_mp_receiver.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase4_trn.c \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_training_channel.c
SIPPYMODEM_DSP_SOURCES := $(SIPPYMODEM_SHARED_DSP_SOURCES) \
	$(SIPPYMODEM_V21_202_SOURCES) $(SIPPYMODEM_V22_SOURCES) \
	$(SIPPYMODEM_V32_SOURCES) $(SIPPYMODEM_PCM_SOURCES)
SOURCES := $(MODEM_CORE_SOURCES) $(SIPPYMODEM_SOURCES) \
	$(SIPPYMODEM_DSP_SOURCES)
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

sippymodem_sdp_pjsip.o sippymodem_sip_signaling_pjsip.o: \
	CPPFLAGS += -D_DEFAULT_SOURCE $(PJSIP_CFLAGS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

clean:
	rm -f slmodemd $(SOURCES:.c=.o) $(DEPENDENCIES)
	rm -f modem_test modem_main.o modem_cmdline.o modem_test.o
	rm -f modem_datafile.o dp_dummy.o dp_sinus.o
	rm -f .build_profile .depend

-include $(DEPENDENCIES)
