CC ?= cc
PYTHON ?= python3
CPPFLAGS ?=
CFLAGS ?= -O2 -g
LDFLAGS ?=
PKG_CONFIG ?= pkg-config

PJSIP_PACKAGE ?= libpjproject
PJSIP_DEPENDENCY ?=
PJSIP_AVAILABLE := $(shell \
	$(PKG_CONFIG) --exists $(PJSIP_PACKAGE) 2>/dev/null && echo yes)
PJSIP_CFLAGS := $(shell \
	$(PKG_CONFIG) --cflags $(PJSIP_PACKAGE) 2>/dev/null)
PJSIP_LDLIBS := $(shell \
	$(PKG_CONFIG) --static --libs $(PJSIP_PACKAGE) 2>/dev/null)

CPPFLAGS += -D_POSIX_C_SOURCE=200809L -I../src -I../dsp \
	-I../dsp/v21_202 -I../dsp/v22 -I../dsp/v32 -I../dsp/pcm
CFLAGS += -std=c11 -Wall -Wextra -Werror

BUILD_DIR := build
DSP_SOURCE_DIR := ../dsp
DSP_V21_202_SOURCE_DIR := $(DSP_SOURCE_DIR)/v21_202
DSP_V22_SOURCE_DIR := $(DSP_SOURCE_DIR)/v22
DSP_V32_SOURCE_DIR := $(DSP_SOURCE_DIR)/v32
DSP_PCM_SOURCE_DIR := $(DSP_SOURCE_DIR)/pcm
V34_NATIVE_SOURCE_NAMES := \
	phase2_info0_exchange.c \
	phase2_info1_exchange.c \
	phase2_initial_probe.c \
	phase2_probe_turnaround.c \
	phase2_reciprocal_probe.c \
	phase2_ranging.c \
	$(notdir $(wildcard $(DSP_PCM_SOURCE_DIR)/v34_*.c))
V34_NATIVE_OBJECTS := \
	$(V34_NATIVE_SOURCE_NAMES:%.c=$(BUILD_DIR)/%.o)
PACK_TEST_BINARY := $(BUILD_DIR)/test_modem_pack
PACK_TEST_SOURCES := test_modem_pack.c modem_stubs.c
PACK_TEST_OBJECTS := $(PACK_TEST_SOURCES:%.c=$(BUILD_DIR)/%.o)
MODEM_OBJECT := $(BUILD_DIR)/modem_pack.o
CONFIG_TEST_BINARY := $(BUILD_DIR)/test_config
CONFIG_TEST_OBJECTS := $(BUILD_DIR)/test_config.o \
	$(BUILD_DIR)/config.o $(BUILD_DIR)/media.o \
	$(BUILD_DIR)/g711.o
SIP_URI_TEST_BINARY := $(BUILD_DIR)/test_sip_uri
SIP_URI_TEST_OBJECTS := $(BUILD_DIR)/test_sip_uri.o \
	$(BUILD_DIR)/sip_uri.o
SDP_TEST_BINARY := $(BUILD_DIR)/test_sdp
SDP_TEST_OBJECTS := $(BUILD_DIR)/test_sdp.o \
	$(BUILD_DIR)/sdp.o $(BUILD_DIR)/media.o \
	$(BUILD_DIR)/g711.o
PJSIP_TEST_BINARY := $(BUILD_DIR)/test_sip_signaling
PJSIP_TEST_OBJECTS := $(BUILD_DIR)/test_sip_signaling.o \
	$(BUILD_DIR)/sip_signaling_pjsip.o \
	$(BUILD_DIR)/sdp_pjsip.o $(BUILD_DIR)/sdp.o \
	$(BUILD_DIR)/sip_uri.o \
	$(BUILD_DIR)/config.o $(BUILD_DIR)/media.o \
	$(BUILD_DIR)/g711.o
G711_TEST_BINARY := $(BUILD_DIR)/test_g711
G711_TEST_OBJECTS := $(BUILD_DIR)/test_g711.o \
	$(BUILD_DIR)/g711.o
CHANNEL_BENCH_TEST_BINARY := $(BUILD_DIR)/test_channel_bench
CHANNEL_BENCH_TEST_OBJECTS := $(BUILD_DIR)/test_channel_bench.o \
	$(BUILD_DIR)/channel_bench.o $(BUILD_DIR)/g711.o
V42_XID_TEST_BINARY := $(BUILD_DIR)/test_v42_xid
V42_XID_TEST_OBJECTS := $(BUILD_DIR)/test_v42_xid.o \
	$(BUILD_DIR)/v42_xid.o \
	$(BUILD_DIR)/v44_xid.o \
	$(BUILD_DIR)/v44_stream.o
LAPM_V44_SESSION_TEST_BINARY := \
	$(BUILD_DIR)/test_lapm_v44_session
LAPM_V44_SESSION_TEST_OBJECTS := \
	$(BUILD_DIR)/test_lapm_v44_session.o \
	$(BUILD_DIR)/lapm_v44_session.o \
	$(BUILD_DIR)/v42_xid.o \
	$(BUILD_DIR)/v44_xid.o \
	$(BUILD_DIR)/v44_stream.o
V44_STREAM_TEST_BINARY := $(BUILD_DIR)/test_v44_stream
V44_STREAM_TEST_OBJECTS := $(BUILD_DIR)/test_v44_stream.o \
	$(BUILD_DIR)/v44_stream.o
V44_XID_TEST_BINARY := $(BUILD_DIR)/test_v44_xid
V44_XID_TEST_OBJECTS := $(BUILD_DIR)/test_v44_xid.o \
	$(BUILD_DIR)/v44_xid.o \
	$(BUILD_DIR)/v44_stream.o
V44_DECODER_TEST_BINARY := $(BUILD_DIR)/test_v44_decoder
V44_DECODER_TEST_OBJECTS := $(BUILD_DIR)/test_v44_decoder.o \
	$(BUILD_DIR)/v44_decoder.o \
	$(BUILD_DIR)/v44_compressed_decoder.o \
	$(BUILD_DIR)/v44_transparent_decoder.o \
	$(BUILD_DIR)/v44_dictionary_decoder.o \
	$(BUILD_DIR)/v44_stream.o
V44_CHARACTER_PATH_TEST_BINARY := \
	$(BUILD_DIR)/test_v44_character_path
V44_CHARACTER_PATH_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v44_character_path.o \
	$(BUILD_DIR)/v44_character_path.o \
	$(BUILD_DIR)/v44_duplex.o \
	$(BUILD_DIR)/v44_tx_monitor.o \
	$(BUILD_DIR)/v44_encoder.o \
	$(BUILD_DIR)/v44_dictionary_encoder.o \
	$(BUILD_DIR)/v44_decoder.o \
	$(BUILD_DIR)/v44_compressed_decoder.o \
	$(BUILD_DIR)/v44_transparent_decoder.o \
	$(BUILD_DIR)/v44_dictionary_decoder.o \
	$(BUILD_DIR)/v44_stream.o
V44_DUPLEX_TEST_BINARY := $(BUILD_DIR)/test_v44_duplex
V44_DUPLEX_TEST_OBJECTS := $(BUILD_DIR)/test_v44_duplex.o \
	$(BUILD_DIR)/v44_duplex.o \
	$(BUILD_DIR)/v44_tx_monitor.o \
	$(BUILD_DIR)/v44_encoder.o \
	$(BUILD_DIR)/v44_dictionary_encoder.o \
	$(BUILD_DIR)/v44_decoder.o \
	$(BUILD_DIR)/v44_compressed_decoder.o \
	$(BUILD_DIR)/v44_transparent_decoder.o \
	$(BUILD_DIR)/v44_dictionary_decoder.o \
	$(BUILD_DIR)/v44_stream.o
V44_ENCODER_TEST_BINARY := $(BUILD_DIR)/test_v44_encoder
V44_ENCODER_TEST_OBJECTS := $(BUILD_DIR)/test_v44_encoder.o \
	$(BUILD_DIR)/v44_test_wire.o \
	$(BUILD_DIR)/v44_encoder.o \
	$(BUILD_DIR)/v44_dictionary_encoder.o \
	$(BUILD_DIR)/v44_decoder.o \
	$(BUILD_DIR)/v44_compressed_decoder.o \
	$(BUILD_DIR)/v44_transparent_decoder.o \
	$(BUILD_DIR)/v44_dictionary_decoder.o \
	$(BUILD_DIR)/v44_stream.o
V44_ENCODER_MODES_TEST_BINARY := \
	$(BUILD_DIR)/test_v44_encoder_modes
V44_ENCODER_MODES_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v44_encoder_modes.o \
	$(BUILD_DIR)/v44_encoder.o \
	$(BUILD_DIR)/v44_dictionary_encoder.o \
	$(BUILD_DIR)/v44_decoder.o \
	$(BUILD_DIR)/v44_compressed_decoder.o \
	$(BUILD_DIR)/v44_transparent_decoder.o \
	$(BUILD_DIR)/v44_dictionary_decoder.o \
	$(BUILD_DIR)/v44_stream.o
V44_TX_MONITOR_TEST_BINARY := \
	$(BUILD_DIR)/test_v44_tx_monitor
V44_TX_MONITOR_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v44_tx_monitor.o \
	$(BUILD_DIR)/v44_tx_monitor.o \
	$(BUILD_DIR)/v44_encoder.o \
	$(BUILD_DIR)/v44_dictionary_encoder.o \
	$(BUILD_DIR)/v44_stream.o
V44_DICTIONARY_ENCODER_TEST_BINARY := \
	$(BUILD_DIR)/test_v44_dictionary_encoder
V44_DICTIONARY_ENCODER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v44_dictionary_encoder.o \
	$(BUILD_DIR)/v44_dictionary_encoder.o \
	$(BUILD_DIR)/v44_stream.o
V44_TRANSPARENT_TEST_BINARY := \
	$(BUILD_DIR)/test_v44_transparent_decoder
V44_TRANSPARENT_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v44_transparent_decoder.o \
	$(BUILD_DIR)/v44_transparent_decoder.o
V44_DICTIONARY_TEST_BINARY := \
	$(BUILD_DIR)/test_v44_dictionary_decoder
V44_DICTIONARY_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v44_dictionary_decoder.o \
	$(BUILD_DIR)/v44_dictionary_decoder.o \
	$(BUILD_DIR)/v44_stream.o
V44_COMPRESSED_TEST_BINARY := \
	$(BUILD_DIR)/test_v44_compressed_decoder
V44_COMPRESSED_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v44_compressed_decoder.o \
	$(BUILD_DIR)/v44_compressed_decoder.o \
	$(BUILD_DIR)/v44_dictionary_decoder.o \
	$(BUILD_DIR)/v44_stream.o
V44_COMPRESSED_CONTROL_TEST_BINARY := \
	$(BUILD_DIR)/test_v44_compressed_controls
V44_COMPRESSED_CONTROL_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v44_compressed_controls.o \
	$(BUILD_DIR)/v44_test_wire.o \
	$(BUILD_DIR)/v44_compressed_decoder.o \
	$(BUILD_DIR)/v44_dictionary_decoder.o \
	$(BUILD_DIR)/v44_stream.o
V44_COMPRESSED_EXTENSION_TEST_BINARY := \
	$(BUILD_DIR)/test_v44_compressed_extensions
V44_COMPRESSED_EXTENSION_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v44_compressed_extensions.o \
	$(BUILD_DIR)/v44_test_wire.o \
	$(BUILD_DIR)/v44_compressed_decoder.o \
	$(BUILD_DIR)/v44_dictionary_decoder.o \
	$(BUILD_DIR)/v44_stream.o
V44_COMPRESSED_INCREMENTAL_TEST_BINARY := \
	$(BUILD_DIR)/test_v44_compressed_incremental
V44_COMPRESSED_INCREMENTAL_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v44_compressed_incremental.o \
	$(BUILD_DIR)/v44_test_wire.o \
	$(BUILD_DIR)/v44_compressed_decoder.o \
	$(BUILD_DIR)/v44_dictionary_decoder.o \
	$(BUILD_DIR)/v44_stream.o
V44_COMPRESSED_VALIDATION_TEST_BINARY := \
	$(BUILD_DIR)/test_v44_compressed_validation
V44_COMPRESSED_VALIDATION_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v44_compressed_validation.o \
	$(BUILD_DIR)/v44_test_wire.o \
	$(BUILD_DIR)/v44_compressed_decoder.o \
	$(BUILD_DIR)/v44_dictionary_decoder.o \
	$(BUILD_DIR)/v44_stream.o
RTP_TEST_BINARY := $(BUILD_DIR)/test_rtp
RTP_TEST_OBJECTS := $(BUILD_DIR)/test_rtp.o \
	$(BUILD_DIR)/rtp.o
MEDIA_TEST_BINARY := $(BUILD_DIR)/test_media
MEDIA_TEST_OBJECTS := $(BUILD_DIR)/test_media.o \
	$(BUILD_DIR)/media.o $(BUILD_DIR)/g711.o
RECEIVER_TEST_BINARY := $(BUILD_DIR)/test_rtp_receiver
RECEIVER_TEST_OBJECTS := $(BUILD_DIR)/test_rtp_receiver.o \
	$(BUILD_DIR)/rtp_receiver.o $(BUILD_DIR)/rtp.o
STREAM_TEST_BINARY := $(BUILD_DIR)/test_rtp_stream
STREAM_TEST_OBJECTS := $(BUILD_DIR)/test_rtp_stream.o \
	$(BUILD_DIR)/rtp_stream.o \
	$(BUILD_DIR)/rtp_receiver.o $(BUILD_DIR)/rtp.o \
	$(BUILD_DIR)/media.o $(BUILD_DIR)/g711.o
ENDPOINT_TEST_BINARY := $(BUILD_DIR)/test_rtp_endpoint
ENDPOINT_TEST_OBJECTS := $(BUILD_DIR)/test_rtp_endpoint.o \
	$(BUILD_DIR)/rtp_endpoint.o \
	$(BUILD_DIR)/rtp_stream.o \
	$(BUILD_DIR)/rtp_receiver.o $(BUILD_DIR)/rtp.o \
	$(BUILD_DIR)/media.o $(BUILD_DIR)/g711.o
SIP_RTP_TEST_BINARY := $(BUILD_DIR)/test_sip_rtp
SIP_RTP_TEST_OBJECTS := $(BUILD_DIR)/test_sip_rtp.o \
	$(BUILD_DIR)/sip_rtp.o \
	$(BUILD_DIR)/rtp_endpoint.o \
	$(BUILD_DIR)/rtp_stream.o \
	$(BUILD_DIR)/rtp_receiver.o $(BUILD_DIR)/rtp.o \
	$(BUILD_DIR)/media.o $(BUILD_DIR)/g711.o
RTP_RUNTIME_TEST_BINARY := $(BUILD_DIR)/test_rtp_runtime
RTP_RUNTIME_TEST_OBJECTS := $(BUILD_DIR)/test_rtp_runtime.o \
	$(BUILD_DIR)/rtp_runtime.o \
	$(BUILD_DIR)/rtp_endpoint.o \
	$(BUILD_DIR)/rtp_stream.o \
	$(BUILD_DIR)/rtp_receiver.o $(BUILD_DIR)/rtp.o \
	$(BUILD_DIR)/config.o \
	$(BUILD_DIR)/media.o $(BUILD_DIR)/g711.o
SIP_MEDIA_BRIDGE_TEST_BINARY := \
	$(BUILD_DIR)/test_sip_media_bridge
SIP_MEDIA_BRIDGE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sip_media_bridge.o \
	$(BUILD_DIR)/sip_media_bridge.o \
	$(BUILD_DIR)/sip_rtp.o \
	$(BUILD_DIR)/rtp_endpoint.o \
	$(BUILD_DIR)/rtp_stream.o \
	$(BUILD_DIR)/rtp_receiver.o $(BUILD_DIR)/rtp.o \
	$(BUILD_DIR)/media.o $(BUILD_DIR)/g711.o
SESSION_TEST_BINARY := $(BUILD_DIR)/test_session
SESSION_TEST_OBJECTS := $(BUILD_DIR)/test_session.o \
	$(BUILD_DIR)/session.o \
	$(BUILD_DIR)/sip_media_bridge.o \
	$(BUILD_DIR)/sip_rtp.o \
	$(BUILD_DIR)/rtp_runtime.o \
	$(BUILD_DIR)/rtp_endpoint.o \
	$(BUILD_DIR)/rtp_stream.o \
	$(BUILD_DIR)/rtp_receiver.o $(BUILD_DIR)/rtp.o \
	$(BUILD_DIR)/config.o \
	$(BUILD_DIR)/media.o $(BUILD_DIR)/g711.o
AUDIO_PATH_TEST_BINARY := $(BUILD_DIR)/test_audio_path
AUDIO_PATH_TEST_OBJECTS := $(BUILD_DIR)/test_audio_path.o \
	$(BUILD_DIR)/audio_path.o \
	$(BUILD_DIR)/media.o $(BUILD_DIR)/g711.o
FRAME_CLOCK_TEST_BINARY := $(BUILD_DIR)/test_frame_clock
FRAME_CLOCK_TEST_OBJECTS := $(BUILD_DIR)/test_frame_clock.o \
	$(BUILD_DIR)/frame_clock.o
AUDIO_SCHEDULER_TEST_BINARY := $(BUILD_DIR)/test_audio_scheduler
AUDIO_SCHEDULER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_audio_scheduler.o \
	$(BUILD_DIR)/audio_scheduler.o \
	$(BUILD_DIR)/frame_clock.o
HOST_TEST_BINARY := $(BUILD_DIR)/test_host
HOST_TEST_OBJECTS := $(BUILD_DIR)/test_host.o \
	$(BUILD_DIR)/host.o \
	$(BUILD_DIR)/session.o \
	$(BUILD_DIR)/sip_media_bridge.o \
	$(BUILD_DIR)/sip_rtp.o \
	$(BUILD_DIR)/rtp_runtime.o \
	$(BUILD_DIR)/rtp_endpoint.o \
	$(BUILD_DIR)/rtp_stream.o \
	$(BUILD_DIR)/rtp_receiver.o \
	$(BUILD_DIR)/rtp.o \
	$(BUILD_DIR)/audio_path.o \
	$(BUILD_DIR)/audio_scheduler.o \
	$(BUILD_DIR)/frame_clock.o \
	$(BUILD_DIR)/config.o \
	$(BUILD_DIR)/media.o \
	$(BUILD_DIR)/g711.o
RECORDING_FILES_TEST_BINARY := \
	$(BUILD_DIR)/test_recording_files
RECORDING_FILES_TEST_OBJECTS := \
	$(BUILD_DIR)/test_recording_files.o \
	$(BUILD_DIR)/recording_files.o \
	$(BUILD_DIR)/media.o $(BUILD_DIR)/g711.o
SLMODEM_ADAPTER_TEST_BINARY := \
	$(BUILD_DIR)/test_slmodem_adapter
SLMODEM_ADAPTER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_slmodem_adapter.o \
	$(BUILD_DIR)/slmodem_adapter.o
SLMODEM_CALLS_TEST_BINARY := $(BUILD_DIR)/test_slmodem_calls
SLMODEM_CALLS_TEST_OBJECTS := \
	$(BUILD_DIR)/test_slmodem_calls.o \
	$(BUILD_DIR)/slmodem_calls.o
SLMODEM_DRIVER_TEST_BINARY := $(BUILD_DIR)/test_slmodem_driver
SLMODEM_DRIVER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_slmodem_driver.o \
	$(BUILD_DIR)/slmodem_driver.o
SLMODEM_TERMINAL_TEST_BINARY := \
	$(BUILD_DIR)/test_slmodem_terminal
SLMODEM_TERMINAL_TEST_OBJECTS := \
	$(BUILD_DIR)/test_slmodem_terminal.o \
	$(BUILD_DIR)/slmodem_terminal.o
RUNTIME_TEST_BINARY := $(BUILD_DIR)/test_runtime
RUNTIME_TEST_OBJECTS := $(BUILD_DIR)/test_runtime.o \
	$(BUILD_DIR)/runtime.o \
	$(BUILD_DIR)/slmodem_terminal.o \
	$(BUILD_DIR)/terminal.o
TERMINAL_TEST_BINARY := $(BUILD_DIR)/test_terminal
TERMINAL_TEST_OBJECTS := $(BUILD_DIR)/test_terminal.o \
	$(BUILD_DIR)/terminal.o
LOOP_TEST_BINARY := $(BUILD_DIR)/test_loop
LOOP_TEST_OBJECTS := $(BUILD_DIR)/test_loop.o \
	$(BUILD_DIR)/loop.o
MAIN_TEST_BINARY := $(BUILD_DIR)/test_main
MAIN_TEST_OBJECTS := $(BUILD_DIR)/test_main.o \
	$(BUILD_DIR)/main_testable.o
DC_FILTER_TEST_BINARY := $(BUILD_DIR)/test_dc_filter
DC_FILTER_TEST_OBJECTS := $(BUILD_DIR)/test_dc_filter.o \
	$(BUILD_DIR)/dc_filter.o
SINGLE_TONE_TEST_BINARY := $(BUILD_DIR)/test_single_tone
SINGLE_TONE_TEST_OBJECTS := $(BUILD_DIR)/test_single_tone.o \
	$(BUILD_DIR)/single_tone.o \
	$(BUILD_DIR)/g711.o
DIFFERENTIAL_QPSK_TEST_BINARY := \
	$(BUILD_DIR)/test_differential_qpsk
DIFFERENTIAL_QPSK_TEST_OBJECTS := \
	$(BUILD_DIR)/test_differential_qpsk.o \
	$(BUILD_DIR)/differential_qpsk.o
SELF_SYNC_SCRAMBLER_OBJECT := \
	$(BUILD_DIR)/self_sync_scrambler.o
CRC16_CCITT_FALSE_OBJECT := \
	$(BUILD_DIR)/crc16_ccitt_false.o
CRC16_CCITT_FALSE_TEST_BINARY := \
	$(BUILD_DIR)/test_crc16_ccitt_false
CRC16_CCITT_FALSE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_crc16_ccitt_false.o \
	$(CRC16_CCITT_FALSE_OBJECT)
SELF_SYNC_SCRAMBLER_TEST_BINARY := \
	$(BUILD_DIR)/test_self_sync_scrambler
SELF_SYNC_SCRAMBLER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_self_sync_scrambler.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT)
NATIVE_CORE_TEST_BINARY := $(BUILD_DIR)/test_native_core
NATIVE_CORE_SOURCE_NAMES := homolog_data modem modem_at modem_comp \
	modem_debug modem_ec modem_pack modem_param modem_timer sysdep_common
NATIVE_CORE_OBJECTS := $(addprefix $(BUILD_DIR)/native_, \
	$(addsuffix .o, $(NATIVE_CORE_SOURCE_NAMES))) \
	$(BUILD_DIR)/lapm_v44_session.o \
	$(BUILD_DIR)/v42_xid.o \
	$(BUILD_DIR)/v44_character_path.o \
	$(BUILD_DIR)/v44_compressed_decoder.o \
	$(BUILD_DIR)/v44_decoder.o \
	$(BUILD_DIR)/v44_dictionary_decoder.o \
	$(BUILD_DIR)/v44_dictionary_encoder.o \
	$(BUILD_DIR)/v44_duplex.o \
	$(BUILD_DIR)/v44_encoder.o \
	$(BUILD_DIR)/v44_transparent_decoder.o \
	$(BUILD_DIR)/v44_tx_monitor.o \
	$(BUILD_DIR)/v44_xid.o \
	$(BUILD_DIR)/v44_stream.o
NATIVE_CORE_TEST_OBJECTS := $(BUILD_DIR)/test_native_core.o \
	$(NATIVE_CORE_OBJECTS) \
	$(V34_NATIVE_OBJECTS) \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/qam_equalizer.o \
	$(BUILD_DIR)/datapumps.o \
	$(BUILD_DIR)/baudot.o \
	$(BUILD_DIR)/baudot_datapump.o \
	$(BUILD_DIR)/dc_filter.o \
	$(BUILD_DIR)/differential_qpsk.o \
	$(BUILD_DIR)/fsk.o \
	$(BUILD_DIR)/fsk300_datapump.o \
	$(BUILD_DIR)/v21.o \
	$(BUILD_DIR)/v8_ansam.o \
	$(BUILD_DIR)/v8_answering.o \
	$(BUILD_DIR)/v8_calling.o \
	$(BUILD_DIR)/v8_datapump.o \
	$(BUILD_DIR)/v8_fsk.o \
	$(BUILD_DIR)/v8_framing.o \
	$(BUILD_DIR)/v8_menu.o \
	$(BUILD_DIR)/qam.o \
	$(BUILD_DIR)/qam_gain.o \
	$(BUILD_DIR)/single_tone.o \
	$(BUILD_DIR)/v22_datapump.o \
	$(BUILD_DIR)/v22_framing.o \
	$(BUILD_DIR)/v22bis_receiver.o \
	$(BUILD_DIR)/v22bis_symbols.o \
	$(BUILD_DIR)/datapump.o \
	$(BUILD_DIR)/uncoded_receiver.o \
	$(BUILD_DIR)/conditioning.o \
	$(BUILD_DIR)/conditioning_receiver.o \
	$(BUILD_DIR)/constellation.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/framing.o \
	$(BUILD_DIR)/line_startup.o \
	$(BUILD_DIR)/rate_signal.o \
	$(BUILD_DIR)/startup.o \
	$(BUILD_DIR)/trellis.o \
	$(BUILD_DIR)/trellis_decoder.o \
	$(BUILD_DIR)/trellis_acquirer.o \
	$(BUILD_DIR)/trellis_receiver.o \
	$(BUILD_DIR)/v23_datapump.o
FSK_TEST_BINARY := $(BUILD_DIR)/test_fsk
FSK_TEST_OBJECTS := $(BUILD_DIR)/test_fsk.o \
	$(BUILD_DIR)/fsk.o \
	$(BUILD_DIR)/v21.o
V8_ANSAM_TEST_BINARY := $(BUILD_DIR)/test_v8_ansam
V8_ANSAM_TEST_OBJECTS := $(BUILD_DIR)/test_v8_ansam.o \
	$(BUILD_DIR)/v8_ansam.o \
	$(BUILD_DIR)/g711.o
V8_ANSWERING_TEST_BINARY := $(BUILD_DIR)/test_v8_answering
V8_ANSWERING_TEST_OBJECTS := $(BUILD_DIR)/test_v8_answering.o \
	$(BUILD_DIR)/v8_answering.o \
	$(BUILD_DIR)/v8_calling.o \
	$(BUILD_DIR)/v8_ansam.o \
	$(BUILD_DIR)/v8_fsk.o \
	$(BUILD_DIR)/v8_menu.o \
	$(BUILD_DIR)/v8_framing.o \
	$(BUILD_DIR)/v21.o \
	$(BUILD_DIR)/fsk.o
V8_CALLING_TEST_BINARY := $(BUILD_DIR)/test_v8_calling
V8_CALLING_TEST_OBJECTS := $(BUILD_DIR)/test_v8_calling.o \
	$(BUILD_DIR)/v8_calling.o \
	$(BUILD_DIR)/v8_ansam.o \
	$(BUILD_DIR)/v8_fsk.o \
	$(BUILD_DIR)/v8_menu.o \
	$(BUILD_DIR)/v8_framing.o \
	$(BUILD_DIR)/v21.o \
	$(BUILD_DIR)/fsk.o
V8_FSK_TEST_BINARY := $(BUILD_DIR)/test_v8_fsk
V8_FSK_TEST_OBJECTS := $(BUILD_DIR)/test_v8_fsk.o \
	$(BUILD_DIR)/v8_fsk.o \
	$(BUILD_DIR)/v8_menu.o \
	$(BUILD_DIR)/v8_framing.o \
	$(BUILD_DIR)/v21.o \
	$(BUILD_DIR)/fsk.o \
	$(BUILD_DIR)/g711.o
V8_FRAMING_TEST_BINARY := $(BUILD_DIR)/test_v8_framing
V8_FRAMING_TEST_OBJECTS := $(BUILD_DIR)/test_v8_framing.o \
	$(BUILD_DIR)/v8_framing.o
V8_MENU_TEST_BINARY := $(BUILD_DIR)/test_v8_menu
V8_MENU_TEST_OBJECTS := $(BUILD_DIR)/test_v8_menu.o \
	$(BUILD_DIR)/v8_menu.o \
	$(BUILD_DIR)/v8_framing.o
BAUDOT_TEST_BINARY := $(BUILD_DIR)/test_baudot
BAUDOT_TEST_OBJECTS := $(BUILD_DIR)/test_baudot.o \
	$(BUILD_DIR)/baudot.o $(BUILD_DIR)/fsk.o
V23_TEST_BINARY := $(BUILD_DIR)/test_v23
V23_TEST_OBJECTS := $(BUILD_DIR)/test_v23.o \
	$(NATIVE_CORE_OBJECTS) \
	$(BUILD_DIR)/dc_filter.o \
	$(BUILD_DIR)/fsk.o \
	$(BUILD_DIR)/v23_datapump.o
V22_FRAMING_TEST_BINARY := $(BUILD_DIR)/test_v22_framing
V22_FRAMING_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v22_framing.o \
	$(BUILD_DIR)/differential_qpsk.o \
	$(BUILD_DIR)/v22_framing.o \
	$(BUILD_DIR)/v22bis_symbols.o
V22BIS_RECEIVER_TEST_BINARY := \
	$(BUILD_DIR)/test_v22bis_receiver
V22BIS_RECEIVER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v22bis_receiver.o \
	$(BUILD_DIR)/differential_qpsk.o \
	$(BUILD_DIR)/qam_gain.o \
	$(BUILD_DIR)/v22bis_receiver.o \
	$(BUILD_DIR)/v22bis_symbols.o \
	$(BUILD_DIR)/v22_framing.o
V32_FRAMING_TEST_BINARY := $(BUILD_DIR)/test_v32_framing
V32_FRAMING_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v32_framing.o \
	$(BUILD_DIR)/differential_qpsk.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/framing.o
V32_CONDITIONING_TEST_BINARY := \
	$(BUILD_DIR)/test_v32_conditioning
V32_CONDITIONING_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v32_conditioning.o \
	$(BUILD_DIR)/differential_qpsk.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/conditioning.o \
	$(BUILD_DIR)/framing.o
V32_CONDITIONING_RECEIVER_TEST_BINARY := \
	$(BUILD_DIR)/test_v32_conditioning_receiver
V32_CONDITIONING_RECEIVER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v32_conditioning_receiver.o \
	$(BUILD_DIR)/qam.o \
	$(BUILD_DIR)/qam_gain.o \
	$(BUILD_DIR)/differential_qpsk.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/conditioning.o \
	$(BUILD_DIR)/conditioning_receiver.o \
	$(BUILD_DIR)/constellation.o \
	$(BUILD_DIR)/framing.o
V32_CONSTELLATION_TEST_BINARY := \
	$(BUILD_DIR)/test_v32_constellation
V32_CONSTELLATION_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v32_constellation.o \
	$(BUILD_DIR)/constellation.o
V32_9600_RECEIVER_TEST_BINARY := \
	$(BUILD_DIR)/test_v32_9600_receiver
V32_9600_RECEIVER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v32_9600_receiver.o \
	$(BUILD_DIR)/differential_qpsk.o \
	$(BUILD_DIR)/qam_equalizer.o \
	$(BUILD_DIR)/uncoded_receiver.o \
	$(BUILD_DIR)/constellation.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/framing.o
V32_RATE_SIGNAL_TEST_BINARY := \
	$(BUILD_DIR)/test_v32_rate_signal
V32_RATE_SIGNAL_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v32_rate_signal.o \
	$(BUILD_DIR)/rate_signal.o
V32_STARTUP_TEST_BINARY := $(BUILD_DIR)/test_v32_startup
V32_STARTUP_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v32_startup.o \
	$(BUILD_DIR)/rate_signal.o \
	$(BUILD_DIR)/startup.o
V32_LINE_STARTUP_TEST_BINARY := \
	$(BUILD_DIR)/test_v32_line_startup
V32_LINE_STARTUP_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v32_line_startup.o \
	$(BUILD_DIR)/line_startup.o \
	$(BUILD_DIR)/single_tone.o \
	$(BUILD_DIR)/g711.o
V32_DATAPUMP_CONDITIONING_TEST_BINARY := \
	$(BUILD_DIR)/test_v32_datapump_conditioning
V32_DATAPUMP_CONDITIONING_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v32_datapump_conditioning.o \
	$(NATIVE_CORE_OBJECTS) \
	$(BUILD_DIR)/qam_equalizer.o \
	$(BUILD_DIR)/dc_filter.o \
	$(BUILD_DIR)/differential_qpsk.o \
	$(BUILD_DIR)/qam.o \
	$(BUILD_DIR)/qam_gain.o \
	$(BUILD_DIR)/single_tone.o \
	$(BUILD_DIR)/datapump.o \
	$(BUILD_DIR)/uncoded_receiver.o \
	$(BUILD_DIR)/conditioning.o \
	$(BUILD_DIR)/conditioning_receiver.o \
	$(BUILD_DIR)/constellation.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/framing.o \
	$(BUILD_DIR)/line_startup.o \
	$(BUILD_DIR)/rate_signal.o \
	$(BUILD_DIR)/startup.o \
	$(BUILD_DIR)/trellis.o \
	$(BUILD_DIR)/trellis_decoder.o \
	$(BUILD_DIR)/trellis_acquirer.o \
	$(BUILD_DIR)/trellis_receiver.o
V32_TRELLIS_TEST_BINARY := $(BUILD_DIR)/test_v32_trellis
V32_TRELLIS_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v32_trellis.o \
	$(BUILD_DIR)/trellis.o
V32_TRELLIS_DECODER_TEST_BINARY := \
	$(BUILD_DIR)/test_v32_trellis_decoder
V32_TRELLIS_DECODER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v32_trellis_decoder.o \
	$(BUILD_DIR)/constellation.o \
	$(BUILD_DIR)/trellis.o \
	$(BUILD_DIR)/trellis_decoder.o
V32_TRELLIS_RECEIVER_TEST_BINARY := \
	$(BUILD_DIR)/test_v32_trellis_receiver
V32_TRELLIS_RECEIVER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v32_trellis_receiver.o \
	$(BUILD_DIR)/differential_qpsk.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/qam_equalizer.o \
	$(BUILD_DIR)/constellation.o \
	$(BUILD_DIR)/framing.o \
	$(BUILD_DIR)/trellis.o \
	$(BUILD_DIR)/trellis_decoder.o \
	$(BUILD_DIR)/trellis_receiver.o
V32_TRELLIS_ACQUIRER_TEST_BINARY := \
	$(BUILD_DIR)/test_v32_trellis_acquirer
V32_TRELLIS_ACQUIRER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v32_trellis_acquirer.o \
	$(BUILD_DIR)/differential_qpsk.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/qam_equalizer.o \
	$(BUILD_DIR)/constellation.o \
	$(BUILD_DIR)/framing.o \
	$(BUILD_DIR)/trellis.o \
	$(BUILD_DIR)/trellis_decoder.o \
	$(BUILD_DIR)/trellis_receiver.o \
	$(BUILD_DIR)/trellis_acquirer.o
V32_CARRIER_TEST_BINARY := $(BUILD_DIR)/test_v32_carrier
V32_CARRIER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v32_carrier.o \
	$(BUILD_DIR)/differential_qpsk.o \
	$(BUILD_DIR)/g711.o \
	$(BUILD_DIR)/qam.o \
	$(BUILD_DIR)/constellation.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/framing.o
V34_FRAMING_TEST_BINARY := $(BUILD_DIR)/test_v34_framing
V34_FRAMING_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_framing.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/v34_framing.o
V34_FRAME_SCHEDULE_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_frame_schedule
V34_FRAME_SCHEDULE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_frame_schedule.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/v34_framing.o \
	$(BUILD_DIR)/v34_frame_schedule.o
V34_MAPPING_PARAMETERS_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_mapping_parameters
V34_MAPPING_PARAMETERS_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_mapping_parameters.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/v34_framing.o \
	$(BUILD_DIR)/v34_frame_schedule.o \
	$(BUILD_DIR)/v34_mapping_parameters.o
V34_PARSER_TEST_BINARY := $(BUILD_DIR)/test_v34_parser
V34_PARSER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_parser.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/v34_framing.o \
	$(BUILD_DIR)/v34_frame_schedule.o \
	$(BUILD_DIR)/v34_mapping_parameters.o \
	$(BUILD_DIR)/v34_parser.o
V34_SHELL_MAPPER_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_shell_mapper
V34_SHELL_MAPPER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_shell_mapper.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/v34_framing.o \
	$(BUILD_DIR)/v34_frame_schedule.o \
	$(BUILD_DIR)/v34_mapping_parameters.o \
	$(BUILD_DIR)/v34_shell_mapper.o
V34_MAPPER_TEST_BINARY := $(BUILD_DIR)/test_v34_mapper
V34_MAPPER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_mapper.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/v34_framing.o \
	$(BUILD_DIR)/v34_frame_schedule.o \
	$(BUILD_DIR)/v34_mapping_parameters.o \
	$(BUILD_DIR)/v34_constellation.o \
	$(BUILD_DIR)/v34_mapper.o
V34_TRELLIS_TEST_BINARY := $(BUILD_DIR)/test_v34_trellis
V34_TRELLIS_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_trellis.o \
	$(BUILD_DIR)/v34_trellis.o
V34_LATTICE_TEST_BINARY := $(BUILD_DIR)/test_v34_lattice
V34_LATTICE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_lattice.o \
	$(BUILD_DIR)/v34_lattice.o \
	$(BUILD_DIR)/v34_trellis.o
V34_PRECODER_TEST_BINARY := $(BUILD_DIR)/test_v34_precoder
V34_PRECODER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_precoder.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/v34_framing.o \
	$(BUILD_DIR)/v34_frame_schedule.o \
	$(BUILD_DIR)/v34_mapping_parameters.o \
	$(BUILD_DIR)/v34_precoder.o
V34_PRECODER_INVERSE_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_precoder_inverse
V34_PRECODER_INVERSE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_precoder_inverse.o \
	$(filter-out $(BUILD_DIR)/test_v34_precoder.o, \
		$(V34_PRECODER_TEST_OBJECTS))
V34_4D_ENCODER_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_4d_encoder
V34_4D_ENCODER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_4d_encoder.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/v34_framing.o \
	$(BUILD_DIR)/v34_frame_schedule.o \
	$(BUILD_DIR)/v34_mapping_parameters.o \
	$(BUILD_DIR)/v34_constellation.o \
	$(BUILD_DIR)/v34_mapper.o \
	$(BUILD_DIR)/v34_precoder.o \
	$(BUILD_DIR)/v34_trellis.o \
	$(BUILD_DIR)/v34_4d_encoder.o
V34_BRANCHES_TEST_BINARY := $(BUILD_DIR)/test_v34_branches
V34_BRANCHES_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_branches.o \
	$(BUILD_DIR)/v34_branches.o \
	$(BUILD_DIR)/v34_lattice.o \
	$(filter-out $(BUILD_DIR)/test_v34_4d_encoder.o, \
		$(V34_4D_ENCODER_TEST_OBJECTS))
V34_SURVIVOR_TEST_BINARY := $(BUILD_DIR)/test_v34_survivor
V34_SURVIVOR_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_survivor.o \
	$(BUILD_DIR)/v34_survivor.o \
	$(filter-out $(BUILD_DIR)/test_v34_branches.o, \
		$(V34_BRANCHES_TEST_OBJECTS))
V34_PATH_DECODER_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_path_decoder
V34_PATH_DECODER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_path_decoder.o \
	$(BUILD_DIR)/v34_path_decoder.o \
	$(BUILD_DIR)/v34_4d_decoder.o \
	$(filter-out $(BUILD_DIR)/test_v34_survivor.o, \
		$(V34_SURVIVOR_TEST_OBJECTS))
V34_CHANNEL_DECODER_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_channel_decoder
V34_CHANNEL_DECODER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_channel_decoder.o \
	$(BUILD_DIR)/v34_channel_decoder.o \
	$(filter-out $(BUILD_DIR)/test_v34_path_decoder.o, \
		$(V34_PATH_DECODER_TEST_OBJECTS))
V34_4D_DECODER_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_4d_decoder
V34_4D_DECODER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_4d_decoder.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/v34_framing.o \
	$(BUILD_DIR)/v34_frame_schedule.o \
	$(BUILD_DIR)/v34_mapping_parameters.o \
	$(BUILD_DIR)/v34_constellation.o \
	$(BUILD_DIR)/v34_mapper.o \
	$(BUILD_DIR)/v34_4d_decoder.o
V34_MAPPING_FRAME_ENCODER_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_mapping_frame_encoder
V34_MAPPING_FRAME_ENCODER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_mapping_frame_encoder.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/v34_framing.o \
	$(BUILD_DIR)/v34_frame_schedule.o \
	$(BUILD_DIR)/v34_mapping_parameters.o \
	$(BUILD_DIR)/v34_parser.o \
	$(BUILD_DIR)/v34_shell_mapper.o \
	$(BUILD_DIR)/v34_constellation.o \
	$(BUILD_DIR)/v34_mapper.o \
	$(BUILD_DIR)/v34_precoder.o \
	$(BUILD_DIR)/v34_trellis.o \
	$(BUILD_DIR)/v34_4d_encoder.o \
	$(BUILD_DIR)/v34_mapping_frame_encoder.o
V34_MAPPING_FRAME_DECODER_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_mapping_frame_decoder
V34_MAPPING_FRAME_DECODER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_mapping_frame_decoder.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/v34_framing.o \
	$(BUILD_DIR)/v34_frame_schedule.o \
	$(BUILD_DIR)/v34_mapping_parameters.o \
	$(BUILD_DIR)/v34_parser.o \
	$(BUILD_DIR)/v34_shell_mapper.o \
	$(BUILD_DIR)/v34_constellation.o \
	$(BUILD_DIR)/v34_mapper.o \
	$(BUILD_DIR)/v34_precoder.o \
	$(BUILD_DIR)/v34_trellis.o \
	$(BUILD_DIR)/v34_4d_encoder.o \
	$(BUILD_DIR)/v34_4d_decoder.o \
	$(BUILD_DIR)/v34_mapping_frame_encoder.o \
	$(BUILD_DIR)/v34_mapping_frame_decoder.o
V34_MAPPING_FRAME_RECEIVER_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_mapping_frame_receiver
V34_MAPPING_FRAME_RECEIVER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_mapping_frame_receiver.o \
	$(filter-out \
		$(BUILD_DIR)/test_v34_mapping_frame_decoder.o, \
		$(V34_MAPPING_FRAME_DECODER_TEST_OBJECTS)) \
	$(BUILD_DIR)/v34_nonlinear.o \
	$(BUILD_DIR)/v34_mapping_frame_transmitter.o \
	$(BUILD_DIR)/v34_mapping_frame_receiver.o
V34_DATA_RECEIVER_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_data_receiver
V34_DATA_RECEIVER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_data_receiver.o \
	$(filter-out \
		$(BUILD_DIR)/test_v34_mapping_frame_receiver.o, \
		$(V34_MAPPING_FRAME_RECEIVER_TEST_OBJECTS)) \
	$(BUILD_DIR)/v34_data_receiver.o
V34_CHANNEL_FRAME_DECODER_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_channel_frame_decoder
V34_CHANNEL_FRAME_DECODER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_channel_frame_decoder.o \
	$(BUILD_DIR)/v34_channel_frame_decoder.o \
	$(sort $(filter-out \
		$(BUILD_DIR)/test_v34_channel_decoder.o \
		$(BUILD_DIR)/test_v34_data_receiver.o, \
		$(V34_CHANNEL_DECODER_TEST_OBJECTS) \
		$(V34_DATA_RECEIVER_TEST_OBJECTS)))
V34_NONLINEAR_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_nonlinear
V34_NONLINEAR_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_nonlinear.o \
	$(BUILD_DIR)/v34_nonlinear.o
V34_SYMBOL_POWER_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_symbol_power
V34_SYMBOL_POWER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_symbol_power.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/v34_framing.o \
	$(BUILD_DIR)/v34_frame_schedule.o \
	$(BUILD_DIR)/v34_mapping_parameters.o \
	$(BUILD_DIR)/v34_constellation.o \
	$(BUILD_DIR)/v34_nonlinear.o \
	$(BUILD_DIR)/v34_symbol_power.o
V34_CHANNEL_CONDITIONER_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_channel_conditioner
V34_CHANNEL_CONDITIONER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_channel_conditioner.o \
	$(BUILD_DIR)/v34_channel_conditioner.o \
	$(filter-out \
		$(BUILD_DIR)/test_v34_symbol_power.o, \
		$(V34_SYMBOL_POWER_TEST_OBJECTS))
V34_CHANNEL_FRAME_RECEIVER_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_channel_frame_receiver
V34_CHANNEL_FRAME_RECEIVER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_channel_frame_receiver.o \
	$(BUILD_DIR)/v34_channel_frame_receiver.o \
	$(BUILD_DIR)/v34_data_channel_receiver.o \
	$(BUILD_DIR)/v34_data_equalizer_receiver.o \
	$(BUILD_DIR)/v34_equalizer_handoff.o \
	$(BUILD_DIR)/qam_equalizer.o \
	$(sort $(filter-out \
		$(BUILD_DIR)/test_v34_channel_frame_decoder.o \
		$(BUILD_DIR)/test_v34_channel_conditioner.o, \
		$(V34_CHANNEL_FRAME_DECODER_TEST_OBJECTS) \
		$(V34_CHANNEL_CONDITIONER_TEST_OBJECTS)))
V34_MAPPING_FRAME_TRANSMITTER_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_mapping_frame_transmitter
V34_MAPPING_FRAME_TRANSMITTER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_mapping_frame_transmitter.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/v34_framing.o \
	$(BUILD_DIR)/v34_frame_schedule.o \
	$(BUILD_DIR)/v34_mapping_parameters.o \
	$(BUILD_DIR)/v34_parser.o \
	$(BUILD_DIR)/v34_shell_mapper.o \
	$(BUILD_DIR)/v34_constellation.o \
	$(BUILD_DIR)/v34_mapper.o \
	$(BUILD_DIR)/v34_precoder.o \
	$(BUILD_DIR)/v34_trellis.o \
	$(BUILD_DIR)/v34_4d_encoder.o \
	$(BUILD_DIR)/v34_mapping_frame_encoder.o \
	$(BUILD_DIR)/v34_nonlinear.o \
	$(BUILD_DIR)/v34_mapping_frame_transmitter.o
V34_DATA_POINTS_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_data_points
V34_DATA_POINTS_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_data_points.o \
	$(filter-out \
		$(BUILD_DIR)/test_v34_mapping_frame_transmitter.o, \
		$(V34_MAPPING_FRAME_TRANSMITTER_TEST_OBJECTS)) \
	$(BUILD_DIR)/v34_symbol_power.o \
	$(BUILD_DIR)/v34_power_calibration.o \
	$(BUILD_DIR)/qam.o \
	$(BUILD_DIR)/v34_info1.o \
	$(BUILD_DIR)/v34_preemphasis.o \
	$(BUILD_DIR)/v34_phase3_profile.o \
	$(BUILD_DIR)/v34_data_carrier.o \
	$(BUILD_DIR)/v34_data_points.o
V34_POWER_CALIBRATION_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_power_calibration
V34_POWER_CALIBRATION_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_power_calibration.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/v34_framing.o \
	$(BUILD_DIR)/v34_frame_schedule.o \
	$(BUILD_DIR)/v34_mapping_parameters.o \
	$(BUILD_DIR)/v34_parser.o \
	$(BUILD_DIR)/v34_shell_mapper.o \
	$(BUILD_DIR)/v34_constellation.o \
	$(BUILD_DIR)/v34_mapper.o \
	$(BUILD_DIR)/v34_precoder.o \
	$(BUILD_DIR)/v34_trellis.o \
	$(BUILD_DIR)/v34_4d_encoder.o \
	$(BUILD_DIR)/v34_mapping_frame_encoder.o \
	$(BUILD_DIR)/v34_nonlinear.o \
	$(BUILD_DIR)/v34_symbol_power.o \
	$(BUILD_DIR)/v34_mapping_frame_transmitter.o \
	$(BUILD_DIR)/v34_data_points.o \
	$(BUILD_DIR)/v34_power_calibration.o \
	$(BUILD_DIR)/qam.o \
	$(BUILD_DIR)/v34_info1.o \
	$(BUILD_DIR)/v34_preemphasis.o \
	$(BUILD_DIR)/v34_phase3_profile.o \
	$(BUILD_DIR)/v34_data_carrier.o
V34_DATA_CARRIER_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_data_carrier
V34_DATA_CARRIER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_data_carrier.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/qam.o \
	$(BUILD_DIR)/v34_framing.o \
	$(BUILD_DIR)/v34_frame_schedule.o \
	$(BUILD_DIR)/v34_mapping_parameters.o \
	$(BUILD_DIR)/v34_parser.o \
	$(BUILD_DIR)/v34_shell_mapper.o \
	$(BUILD_DIR)/v34_constellation.o \
	$(BUILD_DIR)/v34_mapper.o \
	$(BUILD_DIR)/v34_precoder.o \
	$(BUILD_DIR)/v34_trellis.o \
	$(BUILD_DIR)/v34_4d_encoder.o \
	$(BUILD_DIR)/v34_mapping_frame_encoder.o \
	$(BUILD_DIR)/v34_nonlinear.o \
	$(BUILD_DIR)/v34_symbol_power.o \
	$(BUILD_DIR)/v34_mapping_frame_transmitter.o \
	$(BUILD_DIR)/v34_data_points.o \
	$(BUILD_DIR)/v34_power_calibration.o \
	$(BUILD_DIR)/v34_info1.o \
	$(BUILD_DIR)/v34_preemphasis.o \
	$(BUILD_DIR)/v34_phase3_profile.o \
	$(BUILD_DIR)/v34_phase3_s_sequence.o \
	$(BUILD_DIR)/v34_training_line.o \
	$(BUILD_DIR)/v34_data_carrier.o \
	$(BUILD_DIR)/v34_data_line_transmitter.o
V34_DATA_TRANSMITTER_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_data_transmitter
V34_DATA_TRANSMITTER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_data_transmitter.o \
	$(filter-out $(BUILD_DIR)/test_v34_data_carrier.o, \
		$(V34_DATA_CARRIER_TEST_OBJECTS)) \
	$(BUILD_DIR)/v34_data_transmitter.o
V34_DATA_MAPPING_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_data_mapping
V34_DATA_MAPPING_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_data_mapping.o \
	$(filter-out $(BUILD_DIR)/test_v34_data_transmitter.o, \
		$(V34_DATA_TRANSMITTER_TEST_OBJECTS)) \
	$(BUILD_DIR)/v34_data_mapping.o \
	$(BUILD_DIR)/v34_data_mode.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/v34_mp.o
V34_DATA_STARTUP_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_data_startup
V34_DATA_STARTUP_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_data_startup.o \
	$(filter-out $(BUILD_DIR)/test_v34_data_mapping.o, \
		$(V34_DATA_MAPPING_TEST_OBJECTS)) \
	$(BUILD_DIR)/v34_data_startup.o
V34_DATA_STREAM_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_data_stream
V34_DATA_STREAM_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_data_stream.o \
	$(filter-out $(BUILD_DIR)/test_v34_data_mapping.o, \
		$(V34_DATA_MAPPING_TEST_OBJECTS)) \
	$(BUILD_DIR)/v34_data_stream.o \
	$(BUILD_DIR)/v34_phase3_pp_sequence.o \
	$(BUILD_DIR)/v34_phase3_trn.o \
	$(BUILD_DIR)/v34_phase3_j_sequence.o \
	$(BUILD_DIR)/v34_phase3_transmitter.o \
	$(BUILD_DIR)/v34_phase4_j_prime_sequence.o \
	$(BUILD_DIR)/v34_phase4_trn.o \
	$(BUILD_DIR)/v34_phase4_mp.o \
	$(BUILD_DIR)/v34_phase4_e_sequence.o \
	$(BUILD_DIR)/v34_training_transmitter.o \
	$(BUILD_DIR)/v34_phase4_b1_handoff.o
V34_DATA_LINE_RECEIVER_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_data_line_receiver
V34_DATA_LINE_RECEIVER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_data_line_receiver.o \
	$(BUILD_DIR)/v34_data_line_receiver.o \
	$(sort $(filter-out \
		$(BUILD_DIR)/test_v34_channel_frame_receiver.o \
		$(BUILD_DIR)/test_v34_data_stream.o, \
		$(V34_CHANNEL_FRAME_RECEIVER_TEST_OBJECTS) \
		$(V34_DATA_STREAM_TEST_OBJECTS)))
V34_CONSTELLATION_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_constellation
V34_CONSTELLATION_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_constellation.o \
	$(BUILD_DIR)/v34_constellation.o
V34_INFO1_TEST_BINARY := $(BUILD_DIR)/test_v34_info1
V34_INFO1_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_info1.o \
	$(BUILD_DIR)/v34_info1.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/v34_phase2_info.o
V34_PREEMPHASIS_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_preemphasis
V34_PREEMPHASIS_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_preemphasis.o \
	$(BUILD_DIR)/v34_preemphasis.o
V34_PHASE3_PROFILE_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase3_profile
V34_PHASE3_PROFILE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase3_profile.o \
	$(BUILD_DIR)/v34_info1.o \
	$(BUILD_DIR)/v34_phase3_profile.o
V34_PHASE2_SELECTION_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase2_selection
V34_PHASE2_SELECTION_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase2_selection.o \
	$(BUILD_DIR)/v34_info0.o \
	$(BUILD_DIR)/v34_info1.o \
	$(BUILD_DIR)/v34_phase2_selection.o \
	$(BUILD_DIR)/v34_phase3_profile.o
V34_PHASE3_PP_SEQUENCE_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase3_pp_sequence
V34_PHASE3_PP_SEQUENCE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase3_pp_sequence.o \
	$(BUILD_DIR)/v34_phase3_pp_sequence.o
V34_PHASE3_PP_RECEIVER_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase3_pp_receiver
V34_PHASE3_PP_RECEIVER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase3_pp_receiver.o \
	$(BUILD_DIR)/qam_equalizer.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/v34_constellation.o \
	$(BUILD_DIR)/v34_equalizer_handoff.o \
	$(BUILD_DIR)/v34_framing.o \
	$(BUILD_DIR)/v34_phase3_pp_receiver.o \
	$(BUILD_DIR)/v34_phase3_pp_sequence.o \
	$(BUILD_DIR)/v34_phase3_trn.o
V34_PHASE3_S_SEQUENCE_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase3_s_sequence
V34_PHASE3_S_SEQUENCE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase3_s_sequence.o \
	$(BUILD_DIR)/qam.o \
	$(BUILD_DIR)/v34_constellation.o \
	$(BUILD_DIR)/v34_phase3_s_sequence.o
V34_PHASE3_S_RECEIVER_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase3_s_receiver
V34_PHASE3_S_RECEIVER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase3_s_receiver.o \
	$(BUILD_DIR)/qam.o \
	$(BUILD_DIR)/v34_constellation.o \
	$(BUILD_DIR)/v34_phase3_s_sequence.o \
	$(BUILD_DIR)/v34_phase3_s_receiver.o
V34_TRAINING_LINE_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_training_line
V34_TRAINING_LINE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_training_line.o \
	$(BUILD_DIR)/qam.o \
	$(BUILD_DIR)/v34_constellation.o \
	$(BUILD_DIR)/v34_info1.o \
	$(BUILD_DIR)/v34_preemphasis.o \
	$(BUILD_DIR)/v34_phase3_profile.o \
	$(BUILD_DIR)/v34_phase3_s_sequence.o \
	$(BUILD_DIR)/v34_training_line.o
V34_PHASE3_S_TRANSMITTER_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase3_s_transmitter
V34_PHASE3_S_TRANSMITTER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase3_s_transmitter.o \
	$(BUILD_DIR)/qam.o \
	$(BUILD_DIR)/v34_constellation.o \
	$(BUILD_DIR)/v34_info1.o \
	$(BUILD_DIR)/v34_preemphasis.o \
	$(BUILD_DIR)/v34_phase3_profile.o \
	$(BUILD_DIR)/v34_phase3_s_sequence.o \
	$(BUILD_DIR)/v34_training_line.o \
	$(BUILD_DIR)/v34_phase3_s_transmitter.o
V34_PHASE3_TRN_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase3_trn
V34_PHASE3_TRN_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase3_trn.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/v34_constellation.o \
	$(BUILD_DIR)/v34_framing.o \
	$(BUILD_DIR)/v34_phase3_trn.o
V34_PHASE3_J_SEQUENCE_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase3_j_sequence
V34_PHASE3_J_SEQUENCE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase3_j_sequence.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/v34_constellation.o \
	$(BUILD_DIR)/v34_framing.o \
	$(BUILD_DIR)/v34_phase3_trn.o \
	$(BUILD_DIR)/v34_phase3_j_sequence.o
V34_PHASE3_J_RECEIVER_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase3_j_receiver
V34_PHASE3_J_RECEIVER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase3_j_receiver.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/v34_constellation.o \
	$(BUILD_DIR)/v34_framing.o \
	$(BUILD_DIR)/v34_phase3_trn.o \
	$(BUILD_DIR)/v34_phase3_j_sequence.o \
	$(BUILD_DIR)/v34_phase3_j_receiver.o
V34_PHASE3_J_EQUALIZER_RECEIVER_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase3_j_equalizer_receiver
V34_PHASE3_J_EQUALIZER_RECEIVER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase3_j_equalizer_receiver.o \
	$(BUILD_DIR)/qam_equalizer.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/v34_constellation.o \
	$(BUILD_DIR)/v34_equalizer_handoff.o \
	$(BUILD_DIR)/v34_framing.o \
	$(BUILD_DIR)/v34_phase3_j_equalizer_receiver.o \
	$(BUILD_DIR)/v34_phase3_j_receiver.o \
	$(BUILD_DIR)/v34_phase3_j_sequence.o \
	$(BUILD_DIR)/v34_phase3_pp_receiver.o \
	$(BUILD_DIR)/v34_phase3_pp_sequence.o \
	$(BUILD_DIR)/v34_phase3_trn.o
V34_PHASE3_RECEIVER_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase3_receiver
V34_PHASE3_RECEIVER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase3_receiver.o \
	$(BUILD_DIR)/v34_phase3_receiver.o \
	$(BUILD_DIR)/qam_gain.o \
	$(BUILD_DIR)/v34_training_channel.o \
	$(BUILD_DIR)/v34_phase3_s_sequence.o \
	$(BUILD_DIR)/v34_phase3_s_receiver.o \
	$(filter-out \
		$(BUILD_DIR)/test_v34_phase3_j_equalizer_receiver.o, \
		$(V34_PHASE3_J_EQUALIZER_RECEIVER_TEST_OBJECTS))
V34_PHASE4_J_PRIME_SEQUENCE_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase4_j_prime_sequence
V34_PHASE4_J_PRIME_SEQUENCE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase4_j_prime_sequence.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/v34_constellation.o \
	$(BUILD_DIR)/v34_framing.o \
	$(BUILD_DIR)/v34_phase3_trn.o \
	$(BUILD_DIR)/v34_phase3_j_sequence.o \
	$(BUILD_DIR)/v34_phase4_j_prime_sequence.o
V34_PHASE3_TRANSMITTER_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase3_transmitter
V34_PHASE3_TRANSMITTER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase3_transmitter.o \
	$(BUILD_DIR)/v34_phase3_transmitter.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/v34_constellation.o \
	$(BUILD_DIR)/v34_framing.o \
	$(BUILD_DIR)/v34_phase3_s_sequence.o \
	$(BUILD_DIR)/v34_phase3_pp_sequence.o \
	$(BUILD_DIR)/v34_phase3_trn.o \
	$(BUILD_DIR)/v34_phase3_j_sequence.o \
	$(BUILD_DIR)/v34_phase4_j_prime_sequence.o
V34_PHASE3_LINE_TRANSMITTER_OBJECTS := \
	$(BUILD_DIR)/v34_phase3_line_transmitter.o \
	$(filter-out \
		$(BUILD_DIR)/test_v34_training_line.o, \
		$(V34_TRAINING_LINE_TEST_OBJECTS)) \
	$(filter-out \
		$(BUILD_DIR)/test_v34_phase3_transmitter.o, \
		$(V34_PHASE3_TRANSMITTER_TEST_OBJECTS))
V34_PHASE3_LINE_RECEIVER_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase3_line_receiver
V34_PHASE3_LINE_RECEIVER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase3_line_receiver.o \
	$(BUILD_DIR)/v34_phase3_line_receiver.o \
	$(filter-out \
		$(BUILD_DIR)/test_v34_phase3_receiver.o, \
		$(V34_PHASE3_RECEIVER_TEST_OBJECTS)) \
	$(V34_PHASE3_LINE_TRANSMITTER_OBJECTS)
V34_PHASE3_EXCHANGE_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase3_exchange
V34_PHASE3_EXCHANGE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase3_exchange.o \
	$(BUILD_DIR)/g711.o \
	$(BUILD_DIR)/v34_phase3_exchange.o \
	$(filter-out \
		$(BUILD_DIR)/test_v34_phase3_line_receiver.o, \
		$(V34_PHASE3_LINE_RECEIVER_TEST_OBJECTS))
V34_TRAINING_TRANSMITTER_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_training_transmitter
V34_TRAINING_TRANSMITTER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_training_transmitter.o \
	$(BUILD_DIR)/v34_training_transmitter.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/qam.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/v34_constellation.o \
	$(BUILD_DIR)/v34_framing.o \
	$(BUILD_DIR)/v34_info1.o \
	$(BUILD_DIR)/v34_preemphasis.o \
	$(BUILD_DIR)/v34_phase3_profile.o \
	$(BUILD_DIR)/v34_phase3_s_sequence.o \
	$(BUILD_DIR)/v34_phase3_pp_sequence.o \
	$(BUILD_DIR)/v34_phase3_trn.o \
	$(BUILD_DIR)/v34_phase3_j_sequence.o \
	$(BUILD_DIR)/v34_phase3_transmitter.o \
	$(BUILD_DIR)/v34_phase4_j_prime_sequence.o \
	$(BUILD_DIR)/v34_phase4_trn.o \
	$(BUILD_DIR)/v34_mp.o \
	$(BUILD_DIR)/v34_phase4_mp.o \
	$(BUILD_DIR)/v34_phase4_e_sequence.o \
	$(BUILD_DIR)/v34_training_line.o
V34_PHASE4_TRN_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase4_trn
V34_PHASE4_TRN_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase4_trn.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/v34_constellation.o \
	$(BUILD_DIR)/v34_framing.o \
	$(BUILD_DIR)/v34_phase4_trn.o
V34_PHASE4_MP_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase4_mp
V34_PHASE4_MP_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase4_mp.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/v34_constellation.o \
	$(BUILD_DIR)/v34_framing.o \
	$(BUILD_DIR)/v34_mp.o \
	$(BUILD_DIR)/v34_phase4_trn.o \
	$(BUILD_DIR)/v34_phase4_mp.o \
	$(BUILD_DIR)/v34_phase4_mp_receiver.o
V34_PHASE4_MP_CHANNEL_RECEIVER_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase4_mp_channel_receiver
V34_PHASE4_MP_CHANNEL_RECEIVER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase4_mp_channel_receiver.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/qam_gain.o \
	$(BUILD_DIR)/v34_constellation.o \
	$(BUILD_DIR)/v34_framing.o \
	$(BUILD_DIR)/v34_mp.o \
	$(BUILD_DIR)/v34_phase4_trn.o \
	$(BUILD_DIR)/v34_phase4_mp.o \
	$(BUILD_DIR)/v34_phase4_mp_receiver.o \
	$(BUILD_DIR)/v34_training_channel.o \
	$(BUILD_DIR)/v34_phase4_mp_channel_receiver.o \
	$(BUILD_DIR)/v34_phase4_e_sequence.o \
	$(BUILD_DIR)/v34_phase4_e_receiver.o \
	$(BUILD_DIR)/v34_phase4_e_channel_receiver.o
V34_PHASE4_EQUALIZER_RECEIVER_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase4_equalizer_receiver
V34_PHASE4_EQUALIZER_RECEIVER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase4_equalizer_receiver.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/qam_equalizer.o \
	$(BUILD_DIR)/v34_equalizer_handoff.o \
	$(BUILD_DIR)/v34_constellation.o \
	$(BUILD_DIR)/v34_framing.o \
	$(BUILD_DIR)/v34_phase3_s_sequence.o \
	$(BUILD_DIR)/v34_phase3_s_receiver.o \
	$(BUILD_DIR)/v34_mp.o \
	$(BUILD_DIR)/v34_phase4_trn.o \
	$(BUILD_DIR)/v34_phase4_mp.o \
	$(BUILD_DIR)/v34_phase4_mp_receiver.o \
	$(BUILD_DIR)/v34_phase4_e_sequence.o \
	$(BUILD_DIR)/v34_phase4_e_receiver.o \
	$(BUILD_DIR)/v34_phase4_equalizer_receiver.o
V34_PHASE4_LINE_RECEIVER_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase4_line_receiver
V34_PHASE4_LINE_RECEIVER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase4_line_receiver.o \
	$(BUILD_DIR)/v34_phase4_line_receiver.o \
	$(sort $(filter-out \
		$(BUILD_DIR)/test_v34_data_line_receiver.o \
		$(BUILD_DIR)/test_v34_phase4_equalizer_receiver.o, \
		$(V34_DATA_LINE_RECEIVER_TEST_OBJECTS) \
		$(V34_PHASE4_EQUALIZER_RECEIVER_TEST_OBJECTS)))
V34_PHASE3_PHASE4_LINE_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase3_phase4_line
V34_PHASE3_PHASE4_LINE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase3_phase4_line.o \
	$(BUILD_DIR)/v34_phase4_line_handoff.o \
	$(BUILD_DIR)/v34_training_receiver.o \
	$(sort $(filter-out \
		$(BUILD_DIR)/test_v34_phase3_line_receiver.o \
		$(BUILD_DIR)/test_v34_phase4_line_receiver.o, \
		$(V34_PHASE3_LINE_RECEIVER_TEST_OBJECTS) \
		$(V34_PHASE4_LINE_RECEIVER_TEST_OBJECTS)))
V34_PHASE4_EXCHANGE_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase4_exchange
V34_PHASE4_EXCHANGE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase4_exchange.o \
	$(BUILD_DIR)/v34_phase4_exchange.o \
	$(sort $(filter-out \
		$(BUILD_DIR)/test_v34_data_startup.o \
		$(BUILD_DIR)/test_v34_phase3_exchange.o \
		$(BUILD_DIR)/test_v34_phase3_phase4_line.o \
		$(BUILD_DIR)/test_v34_training_transmitter.o, \
		$(V34_DATA_STARTUP_TEST_OBJECTS) \
		$(V34_PHASE3_EXCHANGE_TEST_OBJECTS) \
		$(V34_PHASE3_PHASE4_LINE_TEST_OBJECTS) \
		$(V34_TRAINING_TRANSMITTER_TEST_OBJECTS)))
V34_CALL_TEST_BINARY := $(BUILD_DIR)/test_v34_call
V34_PHASE4_E_SEQUENCE_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase4_e_sequence
V34_PHASE4_E_SEQUENCE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase4_e_sequence.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/v34_constellation.o \
	$(BUILD_DIR)/v34_framing.o \
	$(BUILD_DIR)/v34_mp.o \
	$(BUILD_DIR)/v34_phase4_trn.o \
	$(BUILD_DIR)/v34_phase4_mp.o \
	$(BUILD_DIR)/v34_phase4_e_sequence.o
V34_PHASE4_E_RECEIVER_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase4_e_receiver
V34_PHASE4_E_RECEIVER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase4_e_receiver.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/v34_constellation.o \
	$(BUILD_DIR)/v34_framing.o \
	$(BUILD_DIR)/v34_mp.o \
	$(BUILD_DIR)/v34_phase4_trn.o \
	$(BUILD_DIR)/v34_phase4_mp.o \
	$(BUILD_DIR)/v34_phase4_mp_receiver.o \
	$(BUILD_DIR)/v34_phase4_e_sequence.o \
	$(BUILD_DIR)/v34_phase4_e_receiver.o
V34_MP_TEST_BINARY := $(BUILD_DIR)/test_v34_mp
V34_MP_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_mp.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/v34_mp.o
V34_DATA_MODE_TEST_BINARY := $(BUILD_DIR)/test_v34_data_mode
V34_DATA_MODE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_data_mode.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/v34_mp.o \
	$(BUILD_DIR)/v34_data_mode.o
V34_PHASE2_INFO_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase2_info
V34_PHASE2_INFO_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase2_info.o \
	$(BUILD_DIR)/v34_info0.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/v34_phase2_info.o
V90_INFO0_TEST_BINARY := $(BUILD_DIR)/test_v90_info0
V90_INFO0_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v90_info0.o \
	$(BUILD_DIR)/v90_info0.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/v34_phase2_info.o
V90_INFO1_TEST_BINARY := $(BUILD_DIR)/test_v90_info1
V90_INFO1_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v90_info1.o \
	$(BUILD_DIR)/v90_info1.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/v34_phase2_info.o
V90_MP_TEST_BINARY := $(BUILD_DIR)/test_v90_mp
V90_MP_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v90_mp.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/v90_mp.o
V90_MP_RECEIVER_TEST_BINARY := $(BUILD_DIR)/test_v90_mp_receiver
V90_MP_RECEIVER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v90_mp_receiver.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/v90_mp.o \
	$(BUILD_DIR)/v90_mp_receiver.o
V90_DOWNSTREAM_RX_TEST_BINARY := $(BUILD_DIR)/test_v90_downstream_rx
V90_DOWNSTREAM_RX_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v90_downstream_rx.o \
	$(BUILD_DIR)/v90_downstream_rx.o \
	$(BUILD_DIR)/v90_trn2d_rx.o \
	$(BUILD_DIR)/v90_mp_receiver.o \
	$(BUILD_DIR)/v90_mp.o \
	$(BUILD_DIR)/v90_pcm_frame_decoder.o \
	$(BUILD_DIR)/v90_pcm_frame_encoder.o \
	$(BUILD_DIR)/v90_pcm_mapping.o \
	$(BUILD_DIR)/v90_sign_decoder.o \
	$(BUILD_DIR)/v90_sign_encoder.o \
	$(BUILD_DIR)/v90_spectral_shaper.o \
	$(BUILD_DIR)/v90_pcm_symbol.o \
	$(BUILD_DIR)/v90_cp.o $(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/v34_framing.o $(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/g711.o
V90_CP_TEST_BINARY := $(BUILD_DIR)/test_v90_cp
V90_CP_TEST_OBJECTS := $(BUILD_DIR)/test_v90_cp.o \
	$(BUILD_DIR)/v90_cp.o $(CRC16_CCITT_FALSE_OBJECT)
V90_PCM_MAPPING_TEST_BINARY := \
	$(BUILD_DIR)/test_v90_pcm_mapping
V90_PCM_MAPPING_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v90_pcm_mapping.o \
	$(BUILD_DIR)/v90_pcm_mapping.o \
	$(BUILD_DIR)/v90_cp.o $(CRC16_CCITT_FALSE_OBJECT)
V90_PCM_FRAME_ENCODER_TEST_BINARY := \
	$(BUILD_DIR)/test_v90_pcm_frame_encoder
V90_PCM_FRAME_ENCODER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v90_pcm_frame_encoder.o \
	$(BUILD_DIR)/v90_pcm_frame_encoder.o \
	$(BUILD_DIR)/v90_pcm_mapping.o \
	$(BUILD_DIR)/v90_sign_encoder.o \
	$(BUILD_DIR)/v90_spectral_shaper.o \
	$(BUILD_DIR)/v90_pcm_symbol.o \
	$(BUILD_DIR)/v90_cp.o $(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/g711.o
V90_PCM_FRAME_DECODER_TEST_BINARY := \
	$(BUILD_DIR)/test_v90_pcm_frame_decoder
V90_PCM_FRAME_DECODER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v90_pcm_frame_decoder.o \
	$(BUILD_DIR)/v90_pcm_frame_decoder.o \
	$(BUILD_DIR)/v90_pcm_frame_encoder.o \
	$(BUILD_DIR)/v90_pcm_mapping.o \
	$(BUILD_DIR)/v90_sign_decoder.o \
	$(BUILD_DIR)/v90_sign_encoder.o \
	$(BUILD_DIR)/v90_spectral_shaper.o \
	$(BUILD_DIR)/v90_pcm_symbol.o \
	$(BUILD_DIR)/v90_cp.o $(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/g711.o
V90_SIGN_ENCODER_TEST_BINARY := \
	$(BUILD_DIR)/test_v90_sign_encoder
V90_SIGN_ENCODER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v90_sign_encoder.o \
	$(BUILD_DIR)/v90_sign_encoder.o
V90_SIGN_DECODER_TEST_BINARY := \
	$(BUILD_DIR)/test_v90_sign_decoder
V90_SIGN_DECODER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v90_sign_decoder.o \
	$(BUILD_DIR)/v90_sign_decoder.o \
	$(BUILD_DIR)/v90_sign_encoder.o
V90_SPECTRAL_SHAPER_TEST_BINARY := \
	$(BUILD_DIR)/test_v90_spectral_shaper
V90_SPECTRAL_SHAPER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v90_spectral_shaper.o \
	$(BUILD_DIR)/v90_spectral_shaper.o \
	$(BUILD_DIR)/v90_pcm_mapping.o \
	$(BUILD_DIR)/v90_pcm_symbol.o \
	$(BUILD_DIR)/v90_cp.o $(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/g711.o
V90_DOWNSTREAM_TX_TEST_BINARY := $(BUILD_DIR)/test_v90_downstream_tx
V90_DOWNSTREAM_TX_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v90_downstream_tx.o \
	$(BUILD_DIR)/v90_downstream_tx.o \
	$(BUILD_DIR)/v90_mp.o \
	$(BUILD_DIR)/v90_pcm_frame_encoder.o \
	$(BUILD_DIR)/v90_pcm_mapping.o \
	$(BUILD_DIR)/v90_sign_encoder.o \
	$(BUILD_DIR)/v90_spectral_shaper.o \
	$(BUILD_DIR)/v90_pcm_symbol.o \
	$(BUILD_DIR)/v90_cp.o $(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/v34_framing.o $(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/g711.o
V90_DOWNSTREAM_STREAM_TEST_BINARY := \
	$(BUILD_DIR)/test_v90_downstream_stream
V90_DOWNSTREAM_STREAM_TEST_OBJECTS := $(sort \
	$(BUILD_DIR)/test_v90_downstream_stream.o \
	$(filter-out $(BUILD_DIR)/test_v90_downstream_tx.o, \
		$(V90_DOWNSTREAM_TX_TEST_OBJECTS)) \
	$(filter-out $(BUILD_DIR)/test_v90_downstream_rx.o, \
		$(V90_DOWNSTREAM_RX_TEST_OBJECTS)))
V90_TRN2D_RX_TEST_BINARY := $(BUILD_DIR)/test_v90_trn2d_rx
V90_TRN2D_RX_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v90_trn2d_rx.o \
	$(BUILD_DIR)/v90_trn2d_rx.o \
	$(BUILD_DIR)/v90_downstream_tx.o \
	$(BUILD_DIR)/v90_mp.o \
	$(BUILD_DIR)/v90_pcm_frame_decoder.o \
	$(BUILD_DIR)/v90_pcm_frame_encoder.o \
	$(BUILD_DIR)/v90_pcm_mapping.o \
	$(BUILD_DIR)/v90_sign_decoder.o \
	$(BUILD_DIR)/v90_sign_encoder.o \
	$(BUILD_DIR)/v90_spectral_shaper.o \
	$(BUILD_DIR)/v90_pcm_symbol.o \
	$(BUILD_DIR)/v90_cp.o $(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/v34_framing.o $(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/g711.o
V90_CP_RECEIVER_TEST_BINARY := $(BUILD_DIR)/test_v90_cp_receiver
V90_CP_RECEIVER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v90_cp_receiver.o \
	$(BUILD_DIR)/v90_cp_receiver.o \
	$(BUILD_DIR)/v90_cp.o $(CRC16_CCITT_FALSE_OBJECT)
V90_PHASE4_CP_TX_TEST_BINARY := \
	$(BUILD_DIR)/test_v90_phase4_cp_tx
V90_PHASE4_CP_TX_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v90_phase4_cp_tx.o \
	$(BUILD_DIR)/v90_phase4_cp_tx.o \
	$(BUILD_DIR)/v90_cp.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/v34_constellation.o \
	$(BUILD_DIR)/v34_framing.o
V90_PHASE4_CP_RX_TEST_BINARY := \
	$(BUILD_DIR)/test_v90_phase4_cp_rx
V90_PHASE4_CP_RX_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v90_phase4_cp_rx.o \
	$(BUILD_DIR)/v90_phase4_cp_rx.o \
	$(BUILD_DIR)/v90_phase4_cp_tx.o \
	$(BUILD_DIR)/v90_cp_receiver.o \
	$(BUILD_DIR)/v90_cp.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/v34_constellation.o \
	$(BUILD_DIR)/v34_framing.o
V90_PHASE4_CP_LINE_TX_TEST_BINARY := \
	$(BUILD_DIR)/test_v90_phase4_cp_line_tx
V90_PHASE4_CP_LINE_TX_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v90_phase4_cp_line_tx.o \
	$(BUILD_DIR)/v90_phase4_cp_line_tx.o \
	$(BUILD_DIR)/v90_phase4_cp_tx.o \
	$(BUILD_DIR)/v90_cp.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/qam.o \
	$(BUILD_DIR)/v34_constellation.o \
	$(BUILD_DIR)/v34_framing.o \
	$(BUILD_DIR)/v34_info1.o \
	$(BUILD_DIR)/v34_preemphasis.o \
	$(BUILD_DIR)/v34_phase3_profile.o \
	$(BUILD_DIR)/v34_phase3_s_sequence.o \
	$(BUILD_DIR)/v34_training_line.o
V90_PHASE4_ANALOG_STATION_TEST_BINARY := \
	$(BUILD_DIR)/test_v90_phase4_analog_station
V90_PHASE4_ANALOG_STATION_TEST_OBJECTS := $(sort \
	$(BUILD_DIR)/test_v90_phase4_analog_station.o \
	$(BUILD_DIR)/v90_phase4_analog_station.o \
	$(BUILD_DIR)/v90_phase4_ri.o \
	$(BUILD_DIR)/v90_downstream_rx.o \
	$(BUILD_DIR)/v90_trn2d_rx.o \
	$(BUILD_DIR)/v90_mp_receiver.o \
	$(BUILD_DIR)/v90_mp.o \
	$(BUILD_DIR)/v90_pcm_frame_decoder.o \
	$(BUILD_DIR)/v90_pcm_mapping.o \
	$(BUILD_DIR)/v90_pcm_symbol.o \
	$(BUILD_DIR)/v90_sign_decoder.o \
	$(BUILD_DIR)/v90_jd.o \
	$(BUILD_DIR)/g711.o \
	$(filter-out $(BUILD_DIR)/test_v90_phase4_cp_line_tx.o, \
		$(V90_PHASE4_CP_LINE_TX_TEST_OBJECTS)))
V90_DIL_TEST_BINARY := $(BUILD_DIR)/test_v90_dil
V90_DIL_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v90_dil.o \
	$(BUILD_DIR)/v90_dil.o
V90_JA_TEST_BINARY := $(BUILD_DIR)/test_v90_ja
V90_JA_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v90_ja.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/v34_constellation.o \
	$(BUILD_DIR)/v34_framing.o \
	$(BUILD_DIR)/v34_phase3_trn.o \
	$(BUILD_DIR)/v90_dil.o \
	$(BUILD_DIR)/v90_ja.o
V90_JA_SEARCH_TEST_BINARY := $(BUILD_DIR)/test_v90_ja_search
V90_JA_SEARCH_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v90_ja_search.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/v34_constellation.o \
	$(BUILD_DIR)/v34_framing.o \
	$(BUILD_DIR)/v34_phase3_trn.o \
	$(BUILD_DIR)/v90_dil.o \
	$(BUILD_DIR)/v90_ja.o \
	$(BUILD_DIR)/v90_ja_search.o
V90_JD_TEST_BINARY := $(BUILD_DIR)/test_v90_jd
V90_JD_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v90_jd.o \
	$(BUILD_DIR)/v90_jd.o \
	$(CRC16_CCITT_FALSE_OBJECT)
V90_PHASE3_SD_TEST_BINARY := $(BUILD_DIR)/test_v90_phase3_sd
V90_PHASE3_SD_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v90_phase3_sd.o \
	$(BUILD_DIR)/v90_phase3_sd.o \
	$(BUILD_DIR)/v90_phase3_sd_receiver.o \
	$(BUILD_DIR)/v90_pcm_symbol.o \
	$(BUILD_DIR)/g711.o
V90_PHASE3_SIGNS_TEST_BINARY := $(BUILD_DIR)/test_v90_phase3_signs
V90_PHASE3_SIGNS_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v90_phase3_signs.o \
	$(BUILD_DIR)/v90_phase3_sign_receiver.o \
	$(BUILD_DIR)/v90_phase3_signs.o \
	$(BUILD_DIR)/v90_pcm_symbol.o \
	$(BUILD_DIR)/v90_jd.o \
	$(BUILD_DIR)/v34_framing.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/g711.o
V90_PHASE3_DIGITAL_TX_TEST_BINARY := \
	$(BUILD_DIR)/test_v90_phase3_digital_tx
V90_PHASE3_DIGITAL_TX_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v90_phase3_digital_tx.o \
	$(BUILD_DIR)/v90_phase3_digital_tx.o \
	$(BUILD_DIR)/v90_phase3_sd.o \
	$(BUILD_DIR)/v90_phase3_signs.o \
	$(BUILD_DIR)/v90_pcm_symbol.o \
	$(BUILD_DIR)/v90_jd.o \
	$(BUILD_DIR)/v34_framing.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/g711.o
V90_PHASE3_RETURN_TX_TEST_BINARY := \
	$(BUILD_DIR)/test_v90_phase3_return_tx
V90_PHASE3_RETURN_TX_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v90_phase3_return_tx.o \
	$(BUILD_DIR)/v34_constellation.o \
	$(BUILD_DIR)/v90_phase3_return_tx.o
V90_PHASE3_ANALOG_TX_TEST_BINARY := \
	$(BUILD_DIR)/test_v90_phase3_analog_tx
V90_PHASE3_ANALOG_TX_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v90_phase3_analog_tx.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/v34_constellation.o \
	$(BUILD_DIR)/v34_framing.o \
	$(BUILD_DIR)/v34_phase3_s_sequence.o \
	$(BUILD_DIR)/v34_phase3_pp_sequence.o \
	$(BUILD_DIR)/v34_phase3_trn.o \
	$(BUILD_DIR)/v34_phase3_j_sequence.o \
	$(BUILD_DIR)/v34_phase4_j_prime_sequence.o \
	$(BUILD_DIR)/v34_phase3_transmitter.o \
	$(BUILD_DIR)/v90_dil.o \
	$(BUILD_DIR)/v90_ja.o \
	$(BUILD_DIR)/v90_phase3_analog_tx.o
V90_PHASE3_ANALOG_RX_TEST_BINARY := \
	$(BUILD_DIR)/test_v90_phase3_analog_rx
V90_PHASE3_ANALOG_RX_TEST_OBJECTS := $(sort \
	$(BUILD_DIR)/test_v90_phase3_analog_rx.o \
	$(BUILD_DIR)/v90_phase3_analog_rx.o \
	$(BUILD_DIR)/v90_ja_search.o \
	$(filter-out $(BUILD_DIR)/test_v34_phase3_receiver.o, \
		$(V34_PHASE3_RECEIVER_TEST_OBJECTS)) \
	$(filter-out $(BUILD_DIR)/test_v90_phase3_analog_tx.o, \
		$(V90_PHASE3_ANALOG_TX_TEST_OBJECTS)))
V90_PHASE3_ANALOG_LINE_TX_TEST_BINARY := \
	$(BUILD_DIR)/test_v90_phase3_analog_line_tx
V90_PHASE3_ANALOG_LINE_TX_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v90_phase3_analog_line_tx.o \
	$(filter-out $(BUILD_DIR)/test_v90_phase3_analog_tx.o, \
		$(V90_PHASE3_ANALOG_TX_TEST_OBJECTS)) \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/qam.o \
	$(BUILD_DIR)/v34_info1.o \
	$(BUILD_DIR)/v34_preemphasis.o \
	$(BUILD_DIR)/v34_phase3_profile.o \
	$(BUILD_DIR)/v34_training_line.o \
	$(BUILD_DIR)/v90_phase3_analog_line_tx.o
V90_PHASE3_RETURN_LINE_TX_TEST_BINARY := \
	$(BUILD_DIR)/test_v90_phase3_return_line_tx
V90_PHASE3_RETURN_LINE_TX_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v90_phase3_return_line_tx.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/qam.o \
	$(BUILD_DIR)/v34_constellation.o \
	$(BUILD_DIR)/v34_info1.o \
	$(BUILD_DIR)/v34_preemphasis.o \
	$(BUILD_DIR)/v34_phase3_profile.o \
	$(BUILD_DIR)/v34_phase3_s_sequence.o \
	$(BUILD_DIR)/v34_training_line.o \
	$(BUILD_DIR)/v90_phase3_return_line_tx.o \
	$(BUILD_DIR)/v90_phase3_return_tx.o
V90_PHASE3_RETURN_RX_TEST_BINARY := \
	$(BUILD_DIR)/test_v90_phase3_return_rx
V90_PHASE3_RETURN_RX_TEST_OBJECTS := $(sort \
	$(BUILD_DIR)/test_v90_phase3_return_rx.o \
	$(BUILD_DIR)/qam_equalizer.o \
	$(BUILD_DIR)/v34_equalizer_handoff.o \
	$(BUILD_DIR)/v34_phase3_s_receiver.o \
	$(BUILD_DIR)/v90_phase3_return_rx.o \
	$(filter-out $(BUILD_DIR)/test_v90_phase3_analog_tx.o, \
		$(V90_PHASE3_ANALOG_TX_TEST_OBJECTS)) \
	$(filter-out $(BUILD_DIR)/test_v90_phase3_return_tx.o, \
		$(V90_PHASE3_RETURN_TX_TEST_OBJECTS)))
V90_PHASE3_ANALOG_LINE_RX_TEST_BINARY := \
	$(BUILD_DIR)/test_v90_phase3_analog_line_rx
V90_PHASE3_ANALOG_LINE_RX_TEST_OBJECTS := $(sort \
	$(BUILD_DIR)/test_v90_phase3_analog_line_rx.o \
	$(BUILD_DIR)/v90_phase3_analog_line_rx.o \
	$(BUILD_DIR)/g711.o \
	$(filter-out $(BUILD_DIR)/test_v90_phase3_analog_rx.o, \
		$(V90_PHASE3_ANALOG_RX_TEST_OBJECTS)) \
	$(filter-out $(BUILD_DIR)/test_v90_phase3_analog_line_tx.o, \
		$(V90_PHASE3_ANALOG_LINE_TX_TEST_OBJECTS)))
V90_PHASE3_RETURN_LINE_RX_TEST_BINARY := \
	$(BUILD_DIR)/test_v90_phase3_return_line_rx
V90_PHASE3_RETURN_LINE_RX_TEST_OBJECTS := $(sort \
	$(BUILD_DIR)/test_v90_phase3_return_line_rx.o \
	$(BUILD_DIR)/v90_phase3_return_line_rx.o \
	$(filter-out $(BUILD_DIR)/test_v90_phase3_analog_line_tx.o, \
		$(V90_PHASE3_ANALOG_LINE_TX_TEST_OBJECTS)) \
	$(filter-out $(BUILD_DIR)/test_v90_phase3_analog_line_rx.o, \
		$(V90_PHASE3_ANALOG_LINE_RX_TEST_OBJECTS)) \
	$(filter-out $(BUILD_DIR)/test_v90_phase3_return_line_tx.o, \
		$(V90_PHASE3_RETURN_LINE_TX_TEST_OBJECTS)) \
	$(filter-out $(BUILD_DIR)/test_v90_phase3_return_rx.o, \
		$(V90_PHASE3_RETURN_RX_TEST_OBJECTS)))
V90_PHASE4_CP_LINE_RX_TEST_BINARY := \
	$(BUILD_DIR)/test_v90_phase4_cp_line_rx
V90_PHASE4_CP_LINE_RX_TEST_OBJECTS := $(sort \
	$(BUILD_DIR)/test_v90_phase4_cp_line_rx.o \
	$(BUILD_DIR)/v90_phase4_line_fixture.o \
	$(BUILD_DIR)/v90_phase4_cp_line_rx.o \
	$(BUILD_DIR)/v90_phase4_cp_rx.o \
	$(BUILD_DIR)/v90_cp_receiver.o \
	$(filter-out $(BUILD_DIR)/test_v90_phase3_return_line_rx.o, \
		$(V90_PHASE3_RETURN_LINE_RX_TEST_OBJECTS)) \
	$(filter-out $(BUILD_DIR)/test_v90_phase4_cp_line_tx.o, \
		$(V90_PHASE4_CP_LINE_TX_TEST_OBJECTS)))
V90_PHASE4_DIGITAL_STATION_TEST_BINARY := \
	$(BUILD_DIR)/test_v90_phase4_digital_station
V90_PHASE4_DIGITAL_STATION_TEST_OBJECTS := $(sort \
	$(BUILD_DIR)/test_v90_phase4_digital_station.o \
	$(BUILD_DIR)/v90_phase4_digital_station.o \
	$(BUILD_DIR)/v90_phase4_ri.o \
	$(BUILD_DIR)/v90_pcm_mapping.o \
	$(BUILD_DIR)/v90_pcm_symbol.o \
	$(BUILD_DIR)/v90_jd.o \
	$(filter-out $(BUILD_DIR)/test_v90_downstream_tx.o, \
		$(V90_DOWNSTREAM_TX_TEST_OBJECTS)) \
	$(filter-out $(BUILD_DIR)/test_v90_phase4_cp_line_rx.o, \
		$(V90_PHASE4_CP_LINE_RX_TEST_OBJECTS)))
V90_PHASE4_STATION_EXCHANGE_TEST_BINARY := \
	$(BUILD_DIR)/test_v90_phase4_station_exchange
V90_PHASE4_STATION_EXCHANGE_TEST_OBJECTS := $(sort \
	$(BUILD_DIR)/test_v90_phase4_station_exchange.o \
	$(BUILD_DIR)/v90_phase4_station_fixture.o \
	$(filter-out $(BUILD_DIR)/test_v90_phase4_analog_station.o, \
		$(V90_PHASE4_ANALOG_STATION_TEST_OBJECTS)) \
	$(filter-out $(BUILD_DIR)/test_v90_phase4_digital_station.o, \
		$(V90_PHASE4_DIGITAL_STATION_TEST_OBJECTS)))
V90_PHASE3_ANALOG_STATION_TEST_BINARY := \
	$(BUILD_DIR)/test_v90_phase3_analog_station
V90_PHASE3_ANALOG_STATION_TEST_OBJECTS := $(sort \
	$(BUILD_DIR)/test_v90_phase3_analog_station.o \
	$(BUILD_DIR)/v90_phase3_analog_station.o \
	$(BUILD_DIR)/v90_phase3_digital_station.o \
	$(BUILD_DIR)/v90_phase3_sd_receiver.o \
	$(BUILD_DIR)/v90_phase3_sign_receiver.o \
	$(filter-out $(BUILD_DIR)/test_v90_phase3_digital_tx.o, \
		$(V90_PHASE3_DIGITAL_TX_TEST_OBJECTS)) \
	$(filter-out $(BUILD_DIR)/test_v90_phase3_return_line_rx.o, \
		$(V90_PHASE3_RETURN_LINE_RX_TEST_OBJECTS)))
V90_PHASE4_RI_TEST_BINARY := $(BUILD_DIR)/test_v90_phase4_ri
V90_PHASE4_RI_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v90_phase4_ri.o \
	$(BUILD_DIR)/v90_phase4_ri.o \
	$(BUILD_DIR)/v90_pcm_symbol.o \
	$(BUILD_DIR)/g711.o
V90_PHASE2_INFO0_EXCHANGE_TEST_BINARY := \
	$(BUILD_DIR)/test_v90_phase2_info0_exchange
V90_PHASE2_INFO0_EXCHANGE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v90_phase2_info0_exchange.o \
	$(BUILD_DIR)/g711.o \
	$(BUILD_DIR)/qam.o \
	$(BUILD_DIR)/v90_info0.o \
	$(BUILD_DIR)/v34_phase2_dpsk.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/v34_phase2_info.o \
	$(BUILD_DIR)/v34_phase2_info_audio.o \
	$(BUILD_DIR)/phase2_info0_exchange.o \
	$(BUILD_DIR)/v90_phase2_info0_exchange.o \
	$(BUILD_DIR)/v34_phase2_tone.o
V90_PHASE2_RANGING_TEST_BINARY := \
	$(BUILD_DIR)/test_v90_phase2_ranging
V90_PHASE2_RANGING_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v90_phase2_ranging.o \
	$(BUILD_DIR)/g711.o \
	$(BUILD_DIR)/qam.o \
	$(BUILD_DIR)/v90_info0.o \
	$(BUILD_DIR)/v34_phase2_dpsk.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/v34_phase2_info.o \
	$(BUILD_DIR)/v34_phase2_info_audio.o \
	$(BUILD_DIR)/phase2_info0_exchange.o \
	$(BUILD_DIR)/phase2_ranging.o \
	$(BUILD_DIR)/v90_phase2_info0_exchange.o \
	$(BUILD_DIR)/v90_phase2_ranging.o \
	$(BUILD_DIR)/v34_phase2_tone.o
V90_PHASE2_INITIAL_PROBE_TEST_BINARY := \
	$(BUILD_DIR)/test_v90_phase2_initial_probe
V90_PHASE2_INITIAL_PROBE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v90_phase2_initial_probe.o \
	$(BUILD_DIR)/g711.o \
	$(BUILD_DIR)/qam.o \
	$(BUILD_DIR)/v90_info0.o \
	$(BUILD_DIR)/v90_info1.o \
	$(BUILD_DIR)/v34_info1.o \
	$(BUILD_DIR)/v34_phase2_dpsk.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/v34_phase2_info.o \
	$(BUILD_DIR)/v34_phase2_info_audio.o \
	$(BUILD_DIR)/phase2_info0_exchange.o \
	$(BUILD_DIR)/phase2_ranging.o \
	$(BUILD_DIR)/v90_phase2_info0_exchange.o \
	$(BUILD_DIR)/v90_phase2_ranging.o \
	$(BUILD_DIR)/phase2_initial_probe.o \
	$(BUILD_DIR)/v90_phase2_initial_probe.o \
	$(BUILD_DIR)/phase2_probe_turnaround.o \
	$(BUILD_DIR)/v90_phase2_probe_turnaround.o \
	$(BUILD_DIR)/phase2_reciprocal_probe.o \
	$(BUILD_DIR)/v90_phase2_reciprocal_probe.o \
	$(BUILD_DIR)/phase2_info1_exchange.o \
	$(BUILD_DIR)/v90_phase2_info1_exchange.o \
	$(BUILD_DIR)/v90_phase2.o \
	$(BUILD_DIR)/v34_phase2_probe.o \
	$(BUILD_DIR)/v34_phase2_probe_acquisition.o \
	$(BUILD_DIR)/v34_phase2_probe_receiver.o \
	$(BUILD_DIR)/v34_phase2_probe_spectrum.o \
	$(BUILD_DIR)/v34_phase2_probe_synchronizer.o \
	$(BUILD_DIR)/v34_phase2_probe_sequence.o \
	$(BUILD_DIR)/v34_phase2_tone.o
V34_PHASE2_DPSK_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase2_dpsk
V34_PHASE2_DPSK_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase2_dpsk.o \
	$(BUILD_DIR)/qam.o \
	$(BUILD_DIR)/v34_phase2_dpsk.o
V34_PHASE2_INFO_AUDIO_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase2_info_audio
V34_PHASE2_INFO_AUDIO_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase2_info_audio.o \
	$(BUILD_DIR)/g711.o \
	$(BUILD_DIR)/qam.o \
	$(BUILD_DIR)/v34_phase2_dpsk.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/v34_phase2_info.o \
	$(BUILD_DIR)/v34_phase2_info_audio.o
V34_PHASE2_INFO0_EXCHANGE_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase2_info0_exchange
V34_PHASE2_INFO0_EXCHANGE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase2_info0_exchange.o \
	$(BUILD_DIR)/g711.o \
	$(BUILD_DIR)/qam.o \
	$(BUILD_DIR)/v34_info0.o \
	$(BUILD_DIR)/v34_phase2_dpsk.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/v34_phase2_info.o \
	$(BUILD_DIR)/v34_phase2_info_audio.o \
	$(BUILD_DIR)/phase2_info0_exchange.o \
	$(BUILD_DIR)/v34_phase2_info0_exchange.o \
	$(BUILD_DIR)/v34_phase2_tone.o
V34_PHASE2_RANGING_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase2_ranging
V34_PHASE2_RANGING_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase2_ranging.o \
	$(BUILD_DIR)/g711.o \
	$(BUILD_DIR)/qam.o \
	$(BUILD_DIR)/v34_info0.o \
	$(BUILD_DIR)/v34_phase2_dpsk.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/v34_phase2_info.o \
	$(BUILD_DIR)/v34_phase2_info_audio.o \
	$(BUILD_DIR)/phase2_info0_exchange.o \
	$(BUILD_DIR)/phase2_ranging.o \
	$(BUILD_DIR)/v34_phase2_info0_exchange.o \
	$(BUILD_DIR)/v34_phase2_ranging.o \
	$(BUILD_DIR)/v34_phase2_tone.o
V34_PHASE2_PROBE_FLOW_OBJECTS := \
	$(BUILD_DIR)/v34_phase2_test_pair.o \
	$(BUILD_DIR)/g711.o \
	$(BUILD_DIR)/qam.o \
	$(BUILD_DIR)/v34_info0.o \
	$(BUILD_DIR)/v34_phase2_dpsk.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/v34_phase2_info.o \
	$(BUILD_DIR)/v34_phase2_info_audio.o \
	$(BUILD_DIR)/phase2_info0_exchange.o \
	$(BUILD_DIR)/phase2_initial_probe.o \
	$(BUILD_DIR)/phase2_ranging.o \
	$(BUILD_DIR)/v34_phase2_info0_exchange.o \
	$(BUILD_DIR)/v34_phase2_initial_probe.o \
	$(BUILD_DIR)/v34_phase2_probe.o \
	$(BUILD_DIR)/v34_phase2_probe_acquisition.o \
	$(BUILD_DIR)/v34_phase2_probe_receiver.o \
	$(BUILD_DIR)/v34_phase2_probe_spectrum.o \
	$(BUILD_DIR)/v34_phase2_probe_synchronizer.o \
	$(BUILD_DIR)/v34_phase2_probe_sequence.o \
	$(BUILD_DIR)/phase2_probe_turnaround.o \
	$(BUILD_DIR)/v34_phase2_probe_turnaround.o \
	$(BUILD_DIR)/v34_phase2_ranging.o \
	$(BUILD_DIR)/phase2_reciprocal_probe.o \
	$(BUILD_DIR)/v34_phase2_reciprocal_probe.o \
	$(BUILD_DIR)/v34_phase2_tone.o
V34_PHASE2_TEST_BINARY := $(BUILD_DIR)/test_v34_phase2
V34_PHASE2_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase2.o \
	$(BUILD_DIR)/v34_info1.o \
	$(BUILD_DIR)/v34_phase2.o \
	$(BUILD_DIR)/phase2_info1_exchange.o \
	$(BUILD_DIR)/v34_phase2_info1_exchange.o \
	$(BUILD_DIR)/v34_phase2_selection.o \
	$(BUILD_DIR)/v34_phase3_profile.o \
	$(V34_PHASE3_LINE_TRANSMITTER_OBJECTS) \
	$(V34_PHASE2_PROBE_FLOW_OBJECTS)
V34_CALL_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_call.o \
	$(BUILD_DIR)/v34_call.o \
	$(sort $(filter-out \
		$(BUILD_DIR)/test_v34_phase2.o \
		$(BUILD_DIR)/test_v34_phase4_exchange.o \
		$(BUILD_DIR)/v34_phase2_test_pair.o, \
		$(V34_PHASE2_TEST_OBJECTS) \
		$(V34_PHASE4_EXCHANGE_TEST_OBJECTS)))
V34_PHASE2_INITIAL_PROBE_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase2_initial_probe
V34_PHASE2_INITIAL_PROBE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase2_initial_probe.o \
	$(V34_PHASE2_PROBE_FLOW_OBJECTS)
V34_PHASE2_RECIPROCAL_PROBE_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase2_reciprocal_probe
V34_PHASE2_RECIPROCAL_PROBE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase2_reciprocal_probe.o \
	$(V34_PHASE2_PROBE_FLOW_OBJECTS)
V34_PHASE2_INFO1_EXCHANGE_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase2_info1_exchange
V34_PHASE2_INFO1_EXCHANGE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase2_info1_exchange.o \
	$(BUILD_DIR)/v34_info1.o \
	$(BUILD_DIR)/phase2_info1_exchange.o \
	$(BUILD_DIR)/v34_phase2_info1_exchange.o \
	$(V34_PHASE2_PROBE_FLOW_OBJECTS)
V34_PHASE2_PROBE_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase2_probe
V34_PHASE2_PROBE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase2_probe.o \
	$(BUILD_DIR)/v34_phase2_probe.o
V34_PHASE2_PROBE_ACQUISITION_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase2_probe_acquisition
V34_PHASE2_PROBE_ACQUISITION_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase2_probe_acquisition.o \
	$(BUILD_DIR)/g711.o \
	$(BUILD_DIR)/v34_phase2_probe.o \
	$(BUILD_DIR)/v34_phase2_probe_acquisition.o \
	$(BUILD_DIR)/v34_phase2_probe_receiver.o \
	$(BUILD_DIR)/v34_phase2_probe_spectrum.o \
	$(BUILD_DIR)/v34_phase2_probe_synchronizer.o \
	$(BUILD_DIR)/v34_phase2_tone.o
V34_PHASE2_PROBE_SPECTRUM_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase2_probe_spectrum
V34_PHASE2_PROBE_SPECTRUM_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase2_probe_spectrum.o \
	$(BUILD_DIR)/v34_phase2_probe.o \
	$(BUILD_DIR)/v34_phase2_probe_spectrum.o
V34_PHASE2_PROBE_RECEIVER_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase2_probe_receiver
V34_PHASE2_PROBE_RECEIVER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase2_probe_receiver.o \
	$(BUILD_DIR)/g711.o \
	$(BUILD_DIR)/v34_phase2_probe.o \
	$(BUILD_DIR)/v34_phase2_probe_receiver.o \
	$(BUILD_DIR)/v34_phase2_probe_spectrum.o
V34_PHASE2_PROBE_SYNCHRONIZER_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase2_probe_synchronizer
V34_PHASE2_PROBE_SYNCHRONIZER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase2_probe_synchronizer.o \
	$(BUILD_DIR)/g711.o \
	$(BUILD_DIR)/v34_phase2_probe.o \
	$(BUILD_DIR)/v34_phase2_probe_receiver.o \
	$(BUILD_DIR)/v34_phase2_probe_spectrum.o \
	$(BUILD_DIR)/v34_phase2_probe_synchronizer.o \
	$(BUILD_DIR)/v34_phase2_tone.o
V34_PHASE2_PROBE_SEQUENCE_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase2_probe_sequence
V34_PHASE2_PROBE_SEQUENCE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase2_probe_sequence.o \
	$(BUILD_DIR)/v34_phase2_probe.o \
	$(BUILD_DIR)/v34_phase2_probe_sequence.o
V34_PHASE2_TONE_TEST_BINARY := \
	$(BUILD_DIR)/test_v34_phase2_tone
V34_PHASE2_TONE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_v34_phase2_tone.o \
	$(BUILD_DIR)/g711.o \
	$(BUILD_DIR)/v34_phase2_tone.o
QAM_TEST_BINARY := $(BUILD_DIR)/test_qam
QAM_TEST_OBJECTS := $(BUILD_DIR)/test_qam.o \
	$(BUILD_DIR)/qam.o \
	$(BUILD_DIR)/qam_gain.o
QAM_EQUALIZER_TEST_BINARY := $(BUILD_DIR)/test_qam_equalizer
QAM_EQUALIZER_TEST_OBJECTS := $(BUILD_DIR)/test_qam_equalizer.o \
	$(BUILD_DIR)/qam_equalizer.o
FSK_INTEGRATION_TEST_BINARY := \
	$(BUILD_DIR)/test_fsk_integration
FSK_INTEGRATION_TEST_OBJECTS := \
	$(BUILD_DIR)/test_fsk_integration.o \
	$(NATIVE_CORE_OBJECTS) \
	$(V34_NATIVE_OBJECTS) \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/qam_equalizer.o \
	$(BUILD_DIR)/datapumps.o \
	$(BUILD_DIR)/baudot.o \
	$(BUILD_DIR)/baudot_datapump.o \
	$(BUILD_DIR)/dc_filter.o \
	$(BUILD_DIR)/differential_qpsk.o \
	$(BUILD_DIR)/fsk.o \
	$(BUILD_DIR)/fsk300_datapump.o \
	$(BUILD_DIR)/v21.o \
	$(BUILD_DIR)/v8_ansam.o \
	$(BUILD_DIR)/v8_answering.o \
	$(BUILD_DIR)/v8_calling.o \
	$(BUILD_DIR)/v8_datapump.o \
	$(BUILD_DIR)/v8_fsk.o \
	$(BUILD_DIR)/v8_framing.o \
	$(BUILD_DIR)/v8_menu.o \
	$(BUILD_DIR)/qam.o \
	$(BUILD_DIR)/qam_gain.o \
	$(BUILD_DIR)/single_tone.o \
	$(BUILD_DIR)/v22_datapump.o \
	$(BUILD_DIR)/v22_framing.o \
	$(BUILD_DIR)/v22bis_receiver.o \
	$(BUILD_DIR)/v22bis_symbols.o \
	$(BUILD_DIR)/datapump.o \
	$(BUILD_DIR)/uncoded_receiver.o \
	$(BUILD_DIR)/conditioning.o \
	$(BUILD_DIR)/conditioning_receiver.o \
	$(BUILD_DIR)/constellation.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/framing.o \
	$(BUILD_DIR)/line_startup.o \
	$(BUILD_DIR)/rate_signal.o \
	$(BUILD_DIR)/startup.o \
	$(BUILD_DIR)/trellis.o \
	$(BUILD_DIR)/trellis_decoder.o \
	$(BUILD_DIR)/trellis_acquirer.o \
	$(BUILD_DIR)/trellis_receiver.o \
	$(BUILD_DIR)/v23_datapump.o \
	$(BUILD_DIR)/channel_bench.o \
	$(BUILD_DIR)/media.o \
	$(BUILD_DIR)/g711.o
TEST_BINARIES := $(PACK_TEST_BINARY) $(CONFIG_TEST_BINARY) \
	$(SIP_URI_TEST_BINARY) $(SDP_TEST_BINARY) $(G711_TEST_BINARY) \
	$(CHANNEL_BENCH_TEST_BINARY) \
	$(V42_XID_TEST_BINARY) $(LAPM_V44_SESSION_TEST_BINARY) \
	$(V44_STREAM_TEST_BINARY) $(V44_XID_TEST_BINARY) \
	$(V44_DECODER_TEST_BINARY) $(V44_CHARACTER_PATH_TEST_BINARY) \
	$(V44_DUPLEX_TEST_BINARY) \
	$(V44_ENCODER_TEST_BINARY) $(V44_ENCODER_MODES_TEST_BINARY) \
	$(V44_TX_MONITOR_TEST_BINARY) \
	$(V44_DICTIONARY_ENCODER_TEST_BINARY) \
	$(V44_TRANSPARENT_TEST_BINARY) \
	$(V44_DICTIONARY_TEST_BINARY) \
	$(V44_COMPRESSED_TEST_BINARY) \
	$(V44_COMPRESSED_CONTROL_TEST_BINARY) \
	$(V44_COMPRESSED_EXTENSION_TEST_BINARY) \
	$(V44_COMPRESSED_INCREMENTAL_TEST_BINARY) \
	$(V44_COMPRESSED_VALIDATION_TEST_BINARY) \
	$(RTP_TEST_BINARY) $(MEDIA_TEST_BINARY) $(RECEIVER_TEST_BINARY) \
	$(STREAM_TEST_BINARY) $(ENDPOINT_TEST_BINARY) $(SIP_RTP_TEST_BINARY) \
	$(RTP_RUNTIME_TEST_BINARY) $(SIP_MEDIA_BRIDGE_TEST_BINARY) \
	$(SESSION_TEST_BINARY) $(AUDIO_PATH_TEST_BINARY) \
	$(FRAME_CLOCK_TEST_BINARY) $(AUDIO_SCHEDULER_TEST_BINARY) \
	$(HOST_TEST_BINARY) $(RECORDING_FILES_TEST_BINARY) \
	$(SLMODEM_ADAPTER_TEST_BINARY) $(SLMODEM_CALLS_TEST_BINARY) \
	$(SLMODEM_DRIVER_TEST_BINARY) $(SLMODEM_TERMINAL_TEST_BINARY) \
	$(RUNTIME_TEST_BINARY) \
	$(TERMINAL_TEST_BINARY) $(LOOP_TEST_BINARY) $(MAIN_TEST_BINARY) \
	$(DC_FILTER_TEST_BINARY) $(SINGLE_TONE_TEST_BINARY) \
	$(DIFFERENTIAL_QPSK_TEST_BINARY) \
	$(CRC16_CCITT_FALSE_TEST_BINARY) \
	$(SELF_SYNC_SCRAMBLER_TEST_BINARY) \
	$(NATIVE_CORE_TEST_BINARY) \
	$(FSK_TEST_BINARY) $(V8_ANSAM_TEST_BINARY) \
	$(V8_ANSWERING_TEST_BINARY) \
	$(V8_CALLING_TEST_BINARY) \
	$(V8_FSK_TEST_BINARY) \
	$(V8_FRAMING_TEST_BINARY) $(V8_MENU_TEST_BINARY) \
	$(BAUDOT_TEST_BINARY) $(V23_TEST_BINARY) \
	$(V22_FRAMING_TEST_BINARY) $(V22BIS_RECEIVER_TEST_BINARY) \
	$(V32_FRAMING_TEST_BINARY) $(V32_CONDITIONING_TEST_BINARY) \
	$(V32_CONDITIONING_RECEIVER_TEST_BINARY) \
	$(V32_CONSTELLATION_TEST_BINARY) \
	$(V32_9600_RECEIVER_TEST_BINARY) \
	$(V32_RATE_SIGNAL_TEST_BINARY) \
	$(V32_STARTUP_TEST_BINARY) \
	$(V32_LINE_STARTUP_TEST_BINARY) \
	$(V32_DATAPUMP_CONDITIONING_TEST_BINARY) \
	$(V32_TRELLIS_TEST_BINARY) \
	$(V32_TRELLIS_DECODER_TEST_BINARY) \
	$(V32_TRELLIS_RECEIVER_TEST_BINARY) \
	$(V32_TRELLIS_ACQUIRER_TEST_BINARY) \
	$(V32_CARRIER_TEST_BINARY) \
	$(V34_FRAMING_TEST_BINARY) \
	$(V34_FRAME_SCHEDULE_TEST_BINARY) \
	$(V34_MAPPING_PARAMETERS_TEST_BINARY) \
	$(V34_PARSER_TEST_BINARY) \
	$(V34_SHELL_MAPPER_TEST_BINARY) \
	$(V34_MAPPER_TEST_BINARY) \
	$(V34_TRELLIS_TEST_BINARY) \
	$(V34_LATTICE_TEST_BINARY) \
	$(V34_PRECODER_TEST_BINARY) \
	$(V34_PRECODER_INVERSE_TEST_BINARY) \
	$(V34_4D_ENCODER_TEST_BINARY) \
	$(V34_BRANCHES_TEST_BINARY) \
	$(V34_SURVIVOR_TEST_BINARY) \
	$(V34_PATH_DECODER_TEST_BINARY) \
	$(V34_CHANNEL_DECODER_TEST_BINARY) \
	$(V34_CHANNEL_FRAME_DECODER_TEST_BINARY) \
	$(V34_CHANNEL_CONDITIONER_TEST_BINARY) \
	$(V34_CHANNEL_FRAME_RECEIVER_TEST_BINARY) \
	$(V34_4D_DECODER_TEST_BINARY) \
	$(V34_MAPPING_FRAME_ENCODER_TEST_BINARY) \
	$(V34_MAPPING_FRAME_DECODER_TEST_BINARY) \
	$(V34_MAPPING_FRAME_RECEIVER_TEST_BINARY) \
	$(V34_DATA_RECEIVER_TEST_BINARY) \
	$(V34_NONLINEAR_TEST_BINARY) \
	$(V34_SYMBOL_POWER_TEST_BINARY) \
	$(V34_MAPPING_FRAME_TRANSMITTER_TEST_BINARY) \
	$(V34_DATA_POINTS_TEST_BINARY) \
	$(V34_POWER_CALIBRATION_TEST_BINARY) \
	$(V34_DATA_CARRIER_TEST_BINARY) \
	$(V34_DATA_TRANSMITTER_TEST_BINARY) \
	$(V34_DATA_MAPPING_TEST_BINARY) \
	$(V34_DATA_STARTUP_TEST_BINARY) \
	$(V34_DATA_STREAM_TEST_BINARY) \
	$(V34_DATA_LINE_RECEIVER_TEST_BINARY) \
	$(V34_CONSTELLATION_TEST_BINARY) \
	$(V34_INFO1_TEST_BINARY) \
	$(V34_PREEMPHASIS_TEST_BINARY) \
	$(V34_PHASE3_PROFILE_TEST_BINARY) \
	$(V34_PHASE2_SELECTION_TEST_BINARY) \
	$(V34_PHASE3_PP_SEQUENCE_TEST_BINARY) \
	$(V34_PHASE3_PP_RECEIVER_TEST_BINARY) \
	$(V34_PHASE3_S_SEQUENCE_TEST_BINARY) \
	$(V34_PHASE3_S_RECEIVER_TEST_BINARY) \
	$(V34_TRAINING_LINE_TEST_BINARY) \
	$(V34_PHASE3_S_TRANSMITTER_TEST_BINARY) \
	$(V34_PHASE3_TRN_TEST_BINARY) \
	$(V34_PHASE3_J_SEQUENCE_TEST_BINARY) \
	$(V34_PHASE3_J_RECEIVER_TEST_BINARY) \
	$(V34_PHASE3_J_EQUALIZER_RECEIVER_TEST_BINARY) \
	$(V34_PHASE3_RECEIVER_TEST_BINARY) \
	$(V34_PHASE4_J_PRIME_SEQUENCE_TEST_BINARY) \
	$(V34_PHASE3_TRANSMITTER_TEST_BINARY) \
	$(V34_PHASE3_LINE_RECEIVER_TEST_BINARY) \
	$(V34_PHASE3_EXCHANGE_TEST_BINARY) \
	$(V34_TRAINING_TRANSMITTER_TEST_BINARY) \
	$(V34_PHASE4_TRN_TEST_BINARY) \
	$(V34_PHASE4_MP_TEST_BINARY) \
	$(V34_PHASE4_MP_CHANNEL_RECEIVER_TEST_BINARY) \
	$(V34_PHASE4_EQUALIZER_RECEIVER_TEST_BINARY) \
	$(V34_PHASE4_LINE_RECEIVER_TEST_BINARY) \
	$(V34_PHASE3_PHASE4_LINE_TEST_BINARY) \
	$(V34_PHASE4_EXCHANGE_TEST_BINARY) \
	$(V34_CALL_TEST_BINARY) \
	$(V34_PHASE4_E_SEQUENCE_TEST_BINARY) \
	$(V34_PHASE4_E_RECEIVER_TEST_BINARY) \
	$(V34_MP_TEST_BINARY) \
	$(V34_DATA_MODE_TEST_BINARY) \
	$(V34_PHASE2_INFO_TEST_BINARY) \
	$(V90_INFO0_TEST_BINARY) \
	$(V90_INFO1_TEST_BINARY) \
	$(V90_MP_TEST_BINARY) \
	$(V90_MP_RECEIVER_TEST_BINARY) \
	$(V90_DOWNSTREAM_RX_TEST_BINARY) \
	$(V90_CP_TEST_BINARY) \
	$(V90_PCM_MAPPING_TEST_BINARY) \
	$(V90_PCM_FRAME_ENCODER_TEST_BINARY) \
	$(V90_PCM_FRAME_DECODER_TEST_BINARY) \
	$(V90_SIGN_DECODER_TEST_BINARY) \
	$(V90_SIGN_ENCODER_TEST_BINARY) \
	$(V90_SPECTRAL_SHAPER_TEST_BINARY) \
	$(V90_DOWNSTREAM_TX_TEST_BINARY) \
	$(V90_DOWNSTREAM_STREAM_TEST_BINARY) \
	$(V90_TRN2D_RX_TEST_BINARY) \
	$(V90_CP_RECEIVER_TEST_BINARY) \
	$(V90_PHASE4_CP_TX_TEST_BINARY) \
	$(V90_PHASE4_CP_RX_TEST_BINARY) \
	$(V90_PHASE4_CP_LINE_TX_TEST_BINARY) \
	$(V90_PHASE4_ANALOG_STATION_TEST_BINARY) \
	$(V90_PHASE4_DIGITAL_STATION_TEST_BINARY) \
	$(V90_PHASE4_STATION_EXCHANGE_TEST_BINARY) \
	$(V90_DIL_TEST_BINARY) \
	$(V90_JA_TEST_BINARY) \
	$(V90_JA_SEARCH_TEST_BINARY) \
	$(V90_JD_TEST_BINARY) \
	$(V90_PHASE3_SD_TEST_BINARY) \
	$(V90_PHASE3_SIGNS_TEST_BINARY) \
	$(V90_PHASE3_ANALOG_TX_TEST_BINARY) \
	$(V90_PHASE3_ANALOG_RX_TEST_BINARY) \
	$(V90_PHASE3_ANALOG_LINE_TX_TEST_BINARY) \
	$(V90_PHASE3_ANALOG_LINE_RX_TEST_BINARY) \
	$(V90_PHASE3_DIGITAL_TX_TEST_BINARY) \
	$(V90_PHASE3_ANALOG_STATION_TEST_BINARY) \
	$(V90_PHASE4_RI_TEST_BINARY) \
	$(V90_PHASE3_RETURN_LINE_RX_TEST_BINARY) \
	$(V90_PHASE4_CP_LINE_RX_TEST_BINARY) \
	$(V90_PHASE3_RETURN_LINE_TX_TEST_BINARY) \
	$(V90_PHASE3_RETURN_RX_TEST_BINARY) \
	$(V90_PHASE3_RETURN_TX_TEST_BINARY) \
	$(V90_PHASE2_INFO0_EXCHANGE_TEST_BINARY) \
	$(V90_PHASE2_RANGING_TEST_BINARY) \
	$(V90_PHASE2_INITIAL_PROBE_TEST_BINARY) \
	$(V34_PHASE2_DPSK_TEST_BINARY) \
	$(V34_PHASE2_INFO_AUDIO_TEST_BINARY) \
	$(V34_PHASE2_INFO0_EXCHANGE_TEST_BINARY) \
	$(V34_PHASE2_PROBE_TEST_BINARY) \
	$(V34_PHASE2_PROBE_ACQUISITION_TEST_BINARY) \
	$(V34_PHASE2_PROBE_SPECTRUM_TEST_BINARY) \
	$(V34_PHASE2_PROBE_RECEIVER_TEST_BINARY) \
	$(V34_PHASE2_PROBE_SYNCHRONIZER_TEST_BINARY) \
	$(V34_PHASE2_PROBE_SEQUENCE_TEST_BINARY) \
	$(V34_PHASE2_TEST_BINARY) \
	$(V34_PHASE2_RANGING_TEST_BINARY) \
	$(V34_PHASE2_INITIAL_PROBE_TEST_BINARY) \
	$(V34_PHASE2_RECIPROCAL_PROBE_TEST_BINARY) \
	$(V34_PHASE2_INFO1_EXCHANGE_TEST_BINARY) \
	$(V34_PHASE2_TONE_TEST_BINARY) \
	$(QAM_TEST_BINARY) \
	$(QAM_EQUALIZER_TEST_BINARY) \
	$(FSK_INTEGRATION_TEST_BINARY)
ifeq ($(PJSIP_AVAILABLE),yes)
TEST_BINARIES += $(PJSIP_TEST_BINARY)
endif
DEPENDENCIES := $(PACK_TEST_OBJECTS:.o=.d) $(MODEM_OBJECT:.o=.d) \
	$(CONFIG_TEST_OBJECTS:.o=.d) $(SIP_URI_TEST_OBJECTS:.o=.d) \
	$(SDP_TEST_OBJECTS:.o=.d) $(G711_TEST_OBJECTS:.o=.d) \
	$(CHANNEL_BENCH_TEST_OBJECTS:.o=.d) \
	$(V42_XID_TEST_OBJECTS:.o=.d) \
	$(LAPM_V44_SESSION_TEST_OBJECTS:.o=.d) \
	$(V44_STREAM_TEST_OBJECTS:.o=.d) \
	$(V44_XID_TEST_OBJECTS:.o=.d) \
	$(V44_DECODER_TEST_OBJECTS:.o=.d) \
	$(V44_CHARACTER_PATH_TEST_OBJECTS:.o=.d) \
	$(V44_DUPLEX_TEST_OBJECTS:.o=.d) \
	$(V44_ENCODER_TEST_OBJECTS:.o=.d) \
	$(V44_ENCODER_MODES_TEST_OBJECTS:.o=.d) \
	$(V44_TX_MONITOR_TEST_OBJECTS:.o=.d) \
	$(V44_DICTIONARY_ENCODER_TEST_OBJECTS:.o=.d) \
	$(V44_TRANSPARENT_TEST_OBJECTS:.o=.d) \
	$(V44_DICTIONARY_TEST_OBJECTS:.o=.d) \
	$(V44_COMPRESSED_TEST_OBJECTS:.o=.d) \
	$(V44_COMPRESSED_CONTROL_TEST_OBJECTS:.o=.d) \
	$(V44_COMPRESSED_EXTENSION_TEST_OBJECTS:.o=.d) \
	$(V44_COMPRESSED_INCREMENTAL_TEST_OBJECTS:.o=.d) \
	$(V44_COMPRESSED_VALIDATION_TEST_OBJECTS:.o=.d) \
	$(RTP_TEST_OBJECTS:.o=.d) $(MEDIA_TEST_OBJECTS:.o=.d) \
	$(RECEIVER_TEST_OBJECTS:.o=.d) $(STREAM_TEST_OBJECTS:.o=.d) \
	$(ENDPOINT_TEST_OBJECTS:.o=.d) $(SIP_RTP_TEST_OBJECTS:.o=.d) \
	$(RTP_RUNTIME_TEST_OBJECTS:.o=.d) \
	$(SIP_MEDIA_BRIDGE_TEST_OBJECTS:.o=.d) \
	$(SESSION_TEST_OBJECTS:.o=.d) $(AUDIO_PATH_TEST_OBJECTS:.o=.d) \
	$(FRAME_CLOCK_TEST_OBJECTS:.o=.d) \
	$(AUDIO_SCHEDULER_TEST_OBJECTS:.o=.d) \
	$(HOST_TEST_OBJECTS:.o=.d) \
	$(RECORDING_FILES_TEST_OBJECTS:.o=.d) \
	$(SLMODEM_ADAPTER_TEST_OBJECTS:.o=.d) \
	$(SLMODEM_CALLS_TEST_OBJECTS:.o=.d) \
	$(SLMODEM_DRIVER_TEST_OBJECTS:.o=.d) \
	$(SLMODEM_TERMINAL_TEST_OBJECTS:.o=.d) \
	$(RUNTIME_TEST_OBJECTS:.o=.d) $(TERMINAL_TEST_OBJECTS:.o=.d) \
	$(LOOP_TEST_OBJECTS:.o=.d) $(MAIN_TEST_OBJECTS:.o=.d) \
	$(DC_FILTER_TEST_OBJECTS:.o=.d) \
	$(SINGLE_TONE_TEST_OBJECTS:.o=.d) \
	$(DIFFERENTIAL_QPSK_TEST_OBJECTS:.o=.d) \
	$(CRC16_CCITT_FALSE_TEST_OBJECTS:.o=.d) \
	$(SELF_SYNC_SCRAMBLER_TEST_OBJECTS:.o=.d) \
	$(NATIVE_CORE_TEST_OBJECTS:.o=.d) \
	$(FSK_TEST_OBJECTS:.o=.d) $(V8_ANSAM_TEST_OBJECTS:.o=.d) \
	$(V8_ANSWERING_TEST_OBJECTS:.o=.d) \
	$(V8_CALLING_TEST_OBJECTS:.o=.d) \
	$(V8_FSK_TEST_OBJECTS:.o=.d) \
	$(V8_FRAMING_TEST_OBJECTS:.o=.d) $(V8_MENU_TEST_OBJECTS:.o=.d) \
	$(BAUDOT_TEST_OBJECTS:.o=.d) \
	$(V23_TEST_OBJECTS:.o=.d) \
	$(V22_FRAMING_TEST_OBJECTS:.o=.d) \
	$(V22BIS_RECEIVER_TEST_OBJECTS:.o=.d) \
	$(V32_FRAMING_TEST_OBJECTS:.o=.d) \
	$(V32_CONDITIONING_TEST_OBJECTS:.o=.d) \
	$(V32_CONDITIONING_RECEIVER_TEST_OBJECTS:.o=.d) \
	$(V32_CONSTELLATION_TEST_OBJECTS:.o=.d) \
	$(V32_9600_RECEIVER_TEST_OBJECTS:.o=.d) \
	$(V32_RATE_SIGNAL_TEST_OBJECTS:.o=.d) \
	$(V32_STARTUP_TEST_OBJECTS:.o=.d) \
	$(V32_LINE_STARTUP_TEST_OBJECTS:.o=.d) \
	$(V32_DATAPUMP_CONDITIONING_TEST_OBJECTS:.o=.d) \
	$(V32_TRELLIS_TEST_OBJECTS:.o=.d) \
	$(V32_TRELLIS_DECODER_TEST_OBJECTS:.o=.d) \
	$(V32_TRELLIS_RECEIVER_TEST_OBJECTS:.o=.d) \
	$(V32_TRELLIS_ACQUIRER_TEST_OBJECTS:.o=.d) \
	$(V32_CARRIER_TEST_OBJECTS:.o=.d) \
	$(V34_FRAMING_TEST_OBJECTS:.o=.d) \
	$(V34_FRAME_SCHEDULE_TEST_OBJECTS:.o=.d) \
	$(V34_MAPPING_PARAMETERS_TEST_OBJECTS:.o=.d) \
	$(V34_PARSER_TEST_OBJECTS:.o=.d) \
	$(V34_SHELL_MAPPER_TEST_OBJECTS:.o=.d) \
	$(V34_MAPPER_TEST_OBJECTS:.o=.d) \
	$(V34_TRELLIS_TEST_OBJECTS:.o=.d) \
	$(V34_LATTICE_TEST_OBJECTS:.o=.d) \
	$(V34_PRECODER_TEST_OBJECTS:.o=.d) \
	$(V34_PRECODER_INVERSE_TEST_OBJECTS:.o=.d) \
	$(V34_4D_ENCODER_TEST_OBJECTS:.o=.d) \
	$(V34_BRANCHES_TEST_OBJECTS:.o=.d) \
	$(V34_SURVIVOR_TEST_OBJECTS:.o=.d) \
	$(V34_PATH_DECODER_TEST_OBJECTS:.o=.d) \
	$(V34_CHANNEL_DECODER_TEST_OBJECTS:.o=.d) \
	$(V34_CHANNEL_FRAME_DECODER_TEST_OBJECTS:.o=.d) \
	$(V34_CHANNEL_CONDITIONER_TEST_OBJECTS:.o=.d) \
	$(V34_CHANNEL_FRAME_RECEIVER_TEST_OBJECTS:.o=.d) \
	$(V34_4D_DECODER_TEST_OBJECTS:.o=.d) \
	$(V34_MAPPING_FRAME_ENCODER_TEST_OBJECTS:.o=.d) \
	$(V34_MAPPING_FRAME_DECODER_TEST_OBJECTS:.o=.d) \
	$(V34_MAPPING_FRAME_RECEIVER_TEST_OBJECTS:.o=.d) \
	$(V34_DATA_RECEIVER_TEST_OBJECTS:.o=.d) \
	$(V34_NONLINEAR_TEST_OBJECTS:.o=.d) \
	$(V34_SYMBOL_POWER_TEST_OBJECTS:.o=.d) \
	$(V34_MAPPING_FRAME_TRANSMITTER_TEST_OBJECTS:.o=.d) \
	$(V34_DATA_POINTS_TEST_OBJECTS:.o=.d) \
	$(V34_POWER_CALIBRATION_TEST_OBJECTS:.o=.d) \
	$(V34_DATA_CARRIER_TEST_OBJECTS:.o=.d) \
	$(V34_DATA_STREAM_TEST_OBJECTS:.o=.d) \
	$(V34_DATA_LINE_RECEIVER_TEST_OBJECTS:.o=.d) \
	$(V34_CONSTELLATION_TEST_OBJECTS:.o=.d) \
	$(V34_INFO1_TEST_OBJECTS:.o=.d) \
	$(V34_PREEMPHASIS_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE3_PROFILE_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE2_SELECTION_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE3_PP_SEQUENCE_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE3_PP_RECEIVER_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE3_S_SEQUENCE_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE3_S_RECEIVER_TEST_OBJECTS:.o=.d) \
	$(V34_TRAINING_LINE_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE3_S_TRANSMITTER_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE3_TRN_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE3_J_SEQUENCE_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE3_J_RECEIVER_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE3_J_EQUALIZER_RECEIVER_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE3_RECEIVER_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE4_J_PRIME_SEQUENCE_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE3_TRANSMITTER_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE3_LINE_RECEIVER_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE3_EXCHANGE_TEST_OBJECTS:.o=.d) \
	$(V34_TRAINING_TRANSMITTER_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE4_TRN_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE4_MP_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE4_MP_CHANNEL_RECEIVER_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE4_EQUALIZER_RECEIVER_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE4_LINE_RECEIVER_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE3_PHASE4_LINE_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE4_EXCHANGE_TEST_OBJECTS:.o=.d) \
	$(V34_CALL_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE4_E_SEQUENCE_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE4_E_RECEIVER_TEST_OBJECTS:.o=.d) \
	$(V34_MP_TEST_OBJECTS:.o=.d) \
	$(V34_DATA_MODE_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE2_INFO_TEST_OBJECTS:.o=.d) \
	$(V90_INFO0_TEST_OBJECTS:.o=.d) \
	$(V90_INFO1_TEST_OBJECTS:.o=.d) \
	$(V90_MP_TEST_OBJECTS:.o=.d) \
	$(V90_MP_RECEIVER_TEST_OBJECTS:.o=.d) \
	$(V90_DOWNSTREAM_RX_TEST_OBJECTS:.o=.d) \
	$(V90_CP_TEST_OBJECTS:.o=.d) \
	$(V90_PCM_MAPPING_TEST_OBJECTS:.o=.d) \
	$(V90_PCM_FRAME_ENCODER_TEST_OBJECTS:.o=.d) \
	$(V90_PCM_FRAME_DECODER_TEST_OBJECTS:.o=.d) \
	$(V90_SIGN_DECODER_TEST_OBJECTS:.o=.d) \
	$(V90_SIGN_ENCODER_TEST_OBJECTS:.o=.d) \
	$(V90_SPECTRAL_SHAPER_TEST_OBJECTS:.o=.d) \
	$(V90_DOWNSTREAM_TX_TEST_OBJECTS:.o=.d) \
	$(V90_DOWNSTREAM_STREAM_TEST_OBJECTS:.o=.d) \
	$(V90_TRN2D_RX_TEST_OBJECTS:.o=.d) \
	$(V90_CP_RECEIVER_TEST_OBJECTS:.o=.d) \
	$(V90_PHASE4_CP_TX_TEST_OBJECTS:.o=.d) \
	$(V90_PHASE4_CP_RX_TEST_OBJECTS:.o=.d) \
	$(V90_PHASE4_CP_LINE_TX_TEST_OBJECTS:.o=.d) \
	$(V90_PHASE4_ANALOG_STATION_TEST_OBJECTS:.o=.d) \
	$(V90_PHASE4_DIGITAL_STATION_TEST_OBJECTS:.o=.d) \
	$(V90_PHASE4_STATION_EXCHANGE_TEST_OBJECTS:.o=.d) \
	$(V90_DIL_TEST_OBJECTS:.o=.d) \
	$(V90_JA_TEST_OBJECTS:.o=.d) \
	$(V90_JA_SEARCH_TEST_OBJECTS:.o=.d) \
	$(V90_JD_TEST_OBJECTS:.o=.d) \
	$(V90_PHASE3_SD_TEST_OBJECTS:.o=.d) \
	$(V90_PHASE3_SIGNS_TEST_OBJECTS:.o=.d) \
	$(V90_PHASE3_ANALOG_TX_TEST_OBJECTS:.o=.d) \
	$(V90_PHASE3_ANALOG_RX_TEST_OBJECTS:.o=.d) \
	$(V90_PHASE3_ANALOG_LINE_TX_TEST_OBJECTS:.o=.d) \
	$(V90_PHASE3_ANALOG_LINE_RX_TEST_OBJECTS:.o=.d) \
	$(V90_PHASE3_DIGITAL_TX_TEST_OBJECTS:.o=.d) \
	$(V90_PHASE3_ANALOG_STATION_TEST_OBJECTS:.o=.d) \
	$(V90_PHASE4_RI_TEST_OBJECTS:.o=.d) \
	$(V90_PHASE3_RETURN_LINE_RX_TEST_OBJECTS:.o=.d) \
	$(V90_PHASE4_CP_LINE_RX_TEST_OBJECTS:.o=.d) \
	$(V90_PHASE3_RETURN_LINE_TX_TEST_OBJECTS:.o=.d) \
	$(V90_PHASE3_RETURN_RX_TEST_OBJECTS:.o=.d) \
	$(V90_PHASE3_RETURN_TX_TEST_OBJECTS:.o=.d) \
	$(V90_PHASE2_INFO0_EXCHANGE_TEST_OBJECTS:.o=.d) \
	$(V90_PHASE2_RANGING_TEST_OBJECTS:.o=.d) \
	$(V90_PHASE2_INITIAL_PROBE_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE2_DPSK_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE2_INFO_AUDIO_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE2_INFO0_EXCHANGE_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE2_PROBE_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE2_PROBE_ACQUISITION_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE2_PROBE_SPECTRUM_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE2_PROBE_RECEIVER_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE2_PROBE_SYNCHRONIZER_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE2_PROBE_SEQUENCE_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE2_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE2_RANGING_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE2_INITIAL_PROBE_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE2_RECIPROCAL_PROBE_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE2_INFO1_EXCHANGE_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE2_TONE_TEST_OBJECTS:.o=.d) \
	$(QAM_TEST_OBJECTS:.o=.d) \
	$(QAM_EQUALIZER_TEST_OBJECTS:.o=.d) \
	$(FSK_INTEGRATION_TEST_OBJECTS:.o=.d)
ifeq ($(PJSIP_AVAILABLE),yes)
DEPENDENCIES += $(PJSIP_TEST_OBJECTS:.o=.d)
endif

.PHONY: all clean test

all: $(TEST_BINARIES)

test: $(TEST_BINARIES)
	$(PYTHON) test_live_config.py
	$(PYTHON) test_live_profiles.py
	$(PYTHON) test_live_terminal.py
	$(PYTHON) test_live_calls.py
	$(PYTHON) test_interop_config.py
	$(PYTHON) test_interop_endpoint.py
	$(PYTHON) test_local_interop.py
	./$(PACK_TEST_BINARY)
	./$(CONFIG_TEST_BINARY)
	./$(SIP_URI_TEST_BINARY)
	./$(SDP_TEST_BINARY)
	./$(G711_TEST_BINARY)
	./$(CHANNEL_BENCH_TEST_BINARY)
	./$(V42_XID_TEST_BINARY)
	./$(LAPM_V44_SESSION_TEST_BINARY)
	./$(V44_STREAM_TEST_BINARY)
	./$(V44_XID_TEST_BINARY)
	./$(V44_DECODER_TEST_BINARY)
	./$(V44_CHARACTER_PATH_TEST_BINARY)
	./$(V44_DUPLEX_TEST_BINARY)
	./$(V44_ENCODER_TEST_BINARY)
	./$(V44_ENCODER_MODES_TEST_BINARY)
	./$(V44_TX_MONITOR_TEST_BINARY)
	./$(V44_DICTIONARY_ENCODER_TEST_BINARY)
	./$(V44_TRANSPARENT_TEST_BINARY)
	./$(V44_DICTIONARY_TEST_BINARY)
	./$(V44_COMPRESSED_TEST_BINARY)
	./$(V44_COMPRESSED_CONTROL_TEST_BINARY)
	./$(V44_COMPRESSED_EXTENSION_TEST_BINARY)
	./$(V44_COMPRESSED_INCREMENTAL_TEST_BINARY)
	./$(V44_COMPRESSED_VALIDATION_TEST_BINARY)
	./$(RTP_TEST_BINARY)
	./$(MEDIA_TEST_BINARY)
	./$(RECEIVER_TEST_BINARY)
	./$(STREAM_TEST_BINARY)
	./$(ENDPOINT_TEST_BINARY)
	./$(SIP_RTP_TEST_BINARY)
	./$(RTP_RUNTIME_TEST_BINARY)
	./$(SIP_MEDIA_BRIDGE_TEST_BINARY)
	./$(SESSION_TEST_BINARY)
	./$(AUDIO_PATH_TEST_BINARY)
	./$(FRAME_CLOCK_TEST_BINARY)
	./$(AUDIO_SCHEDULER_TEST_BINARY)
	./$(HOST_TEST_BINARY)
	./$(RECORDING_FILES_TEST_BINARY)
	./$(SLMODEM_ADAPTER_TEST_BINARY)
	./$(SLMODEM_CALLS_TEST_BINARY)
	./$(SLMODEM_DRIVER_TEST_BINARY)
	./$(SLMODEM_TERMINAL_TEST_BINARY)
	./$(RUNTIME_TEST_BINARY)
	./$(TERMINAL_TEST_BINARY)
	./$(LOOP_TEST_BINARY)
	./$(MAIN_TEST_BINARY)
	./$(DC_FILTER_TEST_BINARY)
	./$(SINGLE_TONE_TEST_BINARY)
	./$(DIFFERENTIAL_QPSK_TEST_BINARY)
	./$(CRC16_CCITT_FALSE_TEST_BINARY)
	./$(SELF_SYNC_SCRAMBLER_TEST_BINARY)
	./$(NATIVE_CORE_TEST_BINARY)
	./$(FSK_TEST_BINARY)
	./$(V8_ANSAM_TEST_BINARY)
	./$(V8_ANSWERING_TEST_BINARY)
	./$(V8_CALLING_TEST_BINARY)
	./$(V8_FSK_TEST_BINARY)
	./$(V8_FRAMING_TEST_BINARY)
	./$(V8_MENU_TEST_BINARY)
	./$(BAUDOT_TEST_BINARY)
	./$(V23_TEST_BINARY)
	./$(V22_FRAMING_TEST_BINARY)
	./$(V22BIS_RECEIVER_TEST_BINARY)
	./$(V32_FRAMING_TEST_BINARY)
	./$(V32_CONDITIONING_TEST_BINARY)
	./$(V32_CONDITIONING_RECEIVER_TEST_BINARY)
	./$(V32_CONSTELLATION_TEST_BINARY)
	./$(V32_9600_RECEIVER_TEST_BINARY)
	./$(V32_RATE_SIGNAL_TEST_BINARY)
	./$(V32_STARTUP_TEST_BINARY)
	./$(V32_LINE_STARTUP_TEST_BINARY)
	./$(V32_DATAPUMP_CONDITIONING_TEST_BINARY)
	./$(V32_TRELLIS_TEST_BINARY)
	./$(V32_TRELLIS_DECODER_TEST_BINARY)
	./$(V32_TRELLIS_RECEIVER_TEST_BINARY)
	./$(V32_TRELLIS_ACQUIRER_TEST_BINARY)
	./$(V32_CARRIER_TEST_BINARY)
	./$(V34_FRAMING_TEST_BINARY)
	./$(V34_FRAME_SCHEDULE_TEST_BINARY)
	./$(V34_MAPPING_PARAMETERS_TEST_BINARY)
	./$(V34_PARSER_TEST_BINARY)
	./$(V34_SHELL_MAPPER_TEST_BINARY)
	./$(V34_MAPPER_TEST_BINARY)
	./$(V34_TRELLIS_TEST_BINARY)
	./$(V34_LATTICE_TEST_BINARY)
	./$(V34_PRECODER_TEST_BINARY)
	./$(V34_PRECODER_INVERSE_TEST_BINARY)
	./$(V34_4D_ENCODER_TEST_BINARY)
	./$(V34_BRANCHES_TEST_BINARY)
	./$(V34_SURVIVOR_TEST_BINARY)
	./$(V34_PATH_DECODER_TEST_BINARY)
	./$(V34_CHANNEL_DECODER_TEST_BINARY)
	./$(V34_CHANNEL_FRAME_DECODER_TEST_BINARY)
	./$(V34_CHANNEL_CONDITIONER_TEST_BINARY)
	./$(V34_CHANNEL_FRAME_RECEIVER_TEST_BINARY)
	./$(V34_4D_DECODER_TEST_BINARY)
	./$(V34_MAPPING_FRAME_ENCODER_TEST_BINARY)
	./$(V34_MAPPING_FRAME_DECODER_TEST_BINARY)
	./$(V34_MAPPING_FRAME_RECEIVER_TEST_BINARY)
	./$(V34_DATA_RECEIVER_TEST_BINARY)
	./$(V34_NONLINEAR_TEST_BINARY)
	./$(V34_SYMBOL_POWER_TEST_BINARY)
	./$(V34_MAPPING_FRAME_TRANSMITTER_TEST_BINARY)
	./$(V34_DATA_POINTS_TEST_BINARY)
	./$(V34_POWER_CALIBRATION_TEST_BINARY)
	./$(V34_DATA_CARRIER_TEST_BINARY)
	./$(V34_DATA_TRANSMITTER_TEST_BINARY)
	./$(V34_DATA_MAPPING_TEST_BINARY)
	./$(V34_DATA_STARTUP_TEST_BINARY)
	./$(V34_DATA_STREAM_TEST_BINARY)
	./$(V34_DATA_LINE_RECEIVER_TEST_BINARY)
	./$(V34_CONSTELLATION_TEST_BINARY)
	./$(V34_INFO1_TEST_BINARY)
	./$(V34_PREEMPHASIS_TEST_BINARY)
	./$(V34_PHASE3_PROFILE_TEST_BINARY)
	./$(V34_PHASE2_SELECTION_TEST_BINARY)
	./$(V34_PHASE3_PP_SEQUENCE_TEST_BINARY)
	./$(V34_PHASE3_PP_RECEIVER_TEST_BINARY)
	./$(V34_PHASE3_S_SEQUENCE_TEST_BINARY)
	./$(V34_PHASE3_S_RECEIVER_TEST_BINARY)
	./$(V34_TRAINING_LINE_TEST_BINARY)
	./$(V34_PHASE3_S_TRANSMITTER_TEST_BINARY)
	./$(V34_PHASE3_TRN_TEST_BINARY)
	./$(V34_PHASE3_J_SEQUENCE_TEST_BINARY)
	./$(V34_PHASE3_J_RECEIVER_TEST_BINARY)
	./$(V34_PHASE3_J_EQUALIZER_RECEIVER_TEST_BINARY)
	./$(V34_PHASE3_RECEIVER_TEST_BINARY)
	./$(V34_PHASE4_J_PRIME_SEQUENCE_TEST_BINARY)
	./$(V34_PHASE3_TRANSMITTER_TEST_BINARY)
	./$(V34_PHASE3_LINE_RECEIVER_TEST_BINARY)
	./$(V34_PHASE3_EXCHANGE_TEST_BINARY)
	./$(V34_TRAINING_TRANSMITTER_TEST_BINARY)
	./$(V34_PHASE4_TRN_TEST_BINARY)
	./$(V34_PHASE4_MP_TEST_BINARY)
	./$(V34_PHASE4_MP_CHANNEL_RECEIVER_TEST_BINARY)
	./$(V34_PHASE4_EQUALIZER_RECEIVER_TEST_BINARY)
	./$(V34_PHASE4_LINE_RECEIVER_TEST_BINARY)
	./$(V34_PHASE3_PHASE4_LINE_TEST_BINARY)
	./$(V34_PHASE4_EXCHANGE_TEST_BINARY)
	./$(V34_CALL_TEST_BINARY)
	./$(V34_PHASE4_E_SEQUENCE_TEST_BINARY)
	./$(V34_PHASE4_E_RECEIVER_TEST_BINARY)
	./$(V34_MP_TEST_BINARY)
	./$(V34_DATA_MODE_TEST_BINARY)
	./$(V34_PHASE2_INFO_TEST_BINARY)
	./$(V90_INFO0_TEST_BINARY)
	./$(V90_INFO1_TEST_BINARY)
	./$(V90_MP_TEST_BINARY)
	./$(V90_MP_RECEIVER_TEST_BINARY)
	./$(V90_DOWNSTREAM_RX_TEST_BINARY)
	./$(V90_CP_TEST_BINARY)
	./$(V90_PCM_MAPPING_TEST_BINARY)
	./$(V90_PCM_FRAME_ENCODER_TEST_BINARY)
	./$(V90_PCM_FRAME_DECODER_TEST_BINARY)
	./$(V90_SIGN_DECODER_TEST_BINARY)
	./$(V90_SIGN_ENCODER_TEST_BINARY)
	./$(V90_SPECTRAL_SHAPER_TEST_BINARY)
	./$(V90_DOWNSTREAM_TX_TEST_BINARY)
	./$(V90_DOWNSTREAM_STREAM_TEST_BINARY)
	./$(V90_TRN2D_RX_TEST_BINARY)
	./$(V90_CP_RECEIVER_TEST_BINARY)
	./$(V90_PHASE4_CP_TX_TEST_BINARY)
	./$(V90_PHASE4_CP_RX_TEST_BINARY)
	./$(V90_PHASE4_CP_LINE_TX_TEST_BINARY)
	./$(V90_PHASE4_ANALOG_STATION_TEST_BINARY)
	./$(V90_PHASE4_DIGITAL_STATION_TEST_BINARY)
	./$(V90_PHASE4_STATION_EXCHANGE_TEST_BINARY)
	./$(V90_DIL_TEST_BINARY)
	./$(V90_JA_TEST_BINARY)
	./$(V90_JA_SEARCH_TEST_BINARY)
	./$(V90_JD_TEST_BINARY)
	./$(V90_PHASE3_SD_TEST_BINARY)
	./$(V90_PHASE3_SIGNS_TEST_BINARY)
	./$(V90_PHASE3_ANALOG_TX_TEST_BINARY)
	./$(V90_PHASE3_ANALOG_RX_TEST_BINARY)
	./$(V90_PHASE3_ANALOG_LINE_TX_TEST_BINARY)
	./$(V90_PHASE3_ANALOG_LINE_RX_TEST_BINARY)
	./$(V90_PHASE3_DIGITAL_TX_TEST_BINARY)
	./$(V90_PHASE3_ANALOG_STATION_TEST_BINARY)
	./$(V90_PHASE4_RI_TEST_BINARY)
	./$(V90_PHASE3_RETURN_LINE_RX_TEST_BINARY)
	./$(V90_PHASE4_CP_LINE_RX_TEST_BINARY)
	./$(V90_PHASE3_RETURN_LINE_TX_TEST_BINARY)
	./$(V90_PHASE3_RETURN_RX_TEST_BINARY)
	./$(V90_PHASE3_RETURN_TX_TEST_BINARY)
	./$(V90_PHASE2_INFO0_EXCHANGE_TEST_BINARY)
	./$(V90_PHASE2_RANGING_TEST_BINARY)
	./$(V90_PHASE2_INITIAL_PROBE_TEST_BINARY)
	./$(V34_PHASE2_DPSK_TEST_BINARY)
	./$(V34_PHASE2_INFO_AUDIO_TEST_BINARY)
	./$(V34_PHASE2_INFO0_EXCHANGE_TEST_BINARY)
	./$(V34_PHASE2_PROBE_TEST_BINARY)
	./$(V34_PHASE2_PROBE_ACQUISITION_TEST_BINARY)
	./$(V34_PHASE2_PROBE_SPECTRUM_TEST_BINARY)
	./$(V34_PHASE2_PROBE_RECEIVER_TEST_BINARY)
	./$(V34_PHASE2_PROBE_SYNCHRONIZER_TEST_BINARY)
	./$(V34_PHASE2_PROBE_SEQUENCE_TEST_BINARY)
	./$(V34_PHASE2_TEST_BINARY)
	./$(V34_PHASE2_RANGING_TEST_BINARY)
	./$(V34_PHASE2_INITIAL_PROBE_TEST_BINARY)
	./$(V34_PHASE2_RECIPROCAL_PROBE_TEST_BINARY)
	./$(V34_PHASE2_INFO1_EXCHANGE_TEST_BINARY)
	./$(V34_PHASE2_TONE_TEST_BINARY)
	./$(QAM_TEST_BINARY)
	./$(QAM_EQUALIZER_TEST_BINARY)
	./$(FSK_INTEGRATION_TEST_BINARY)
ifeq ($(PJSIP_AVAILABLE),yes)
	./$(PJSIP_TEST_BINARY)
else
	@echo "[  SKIPPED ] SIP signaling lifecycle (libpjproject unavailable)"
endif

$(PACK_TEST_BINARY): $(PACK_TEST_OBJECTS) $(MODEM_OBJECT)
	$(CC) $(LDFLAGS) -o $@ $^

$(CONFIG_TEST_BINARY): $(CONFIG_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(SIP_URI_TEST_BINARY): $(SIP_URI_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(SDP_TEST_BINARY): $(SDP_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(PJSIP_TEST_BINARY): $(PJSIP_TEST_OBJECTS) $(PJSIP_DEPENDENCY)
	$(CC) $(LDFLAGS) -o $@ $(PJSIP_TEST_OBJECTS) $(PJSIP_LDLIBS)

$(G711_TEST_BINARY): $(G711_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(CHANNEL_BENCH_TEST_BINARY): $(CHANNEL_BENCH_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V42_XID_TEST_BINARY): $(V42_XID_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(LAPM_V44_SESSION_TEST_BINARY): $(LAPM_V44_SESSION_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V44_STREAM_TEST_BINARY): $(V44_STREAM_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V44_XID_TEST_BINARY): $(V44_XID_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V44_DECODER_TEST_BINARY): $(V44_DECODER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V44_CHARACTER_PATH_TEST_BINARY): $(V44_CHARACTER_PATH_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V44_DUPLEX_TEST_BINARY): $(V44_DUPLEX_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V44_ENCODER_TEST_BINARY): $(V44_ENCODER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V44_ENCODER_MODES_TEST_BINARY): $(V44_ENCODER_MODES_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V44_TX_MONITOR_TEST_BINARY): $(V44_TX_MONITOR_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V44_DICTIONARY_ENCODER_TEST_BINARY): \
	$(V44_DICTIONARY_ENCODER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V44_TRANSPARENT_TEST_BINARY): $(V44_TRANSPARENT_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V44_DICTIONARY_TEST_BINARY): $(V44_DICTIONARY_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V44_COMPRESSED_TEST_BINARY): $(V44_COMPRESSED_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V44_COMPRESSED_CONTROL_TEST_BINARY): \
	$(V44_COMPRESSED_CONTROL_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V44_COMPRESSED_EXTENSION_TEST_BINARY): \
	$(V44_COMPRESSED_EXTENSION_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V44_COMPRESSED_INCREMENTAL_TEST_BINARY): \
	$(V44_COMPRESSED_INCREMENTAL_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V44_COMPRESSED_VALIDATION_TEST_BINARY): \
	$(V44_COMPRESSED_VALIDATION_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(RTP_TEST_BINARY): $(RTP_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(MEDIA_TEST_BINARY): $(MEDIA_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(RECEIVER_TEST_BINARY): $(RECEIVER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(STREAM_TEST_BINARY): $(STREAM_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(ENDPOINT_TEST_BINARY): $(ENDPOINT_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(SIP_RTP_TEST_BINARY): $(SIP_RTP_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(RTP_RUNTIME_TEST_BINARY): $(RTP_RUNTIME_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(SIP_MEDIA_BRIDGE_TEST_BINARY): $(SIP_MEDIA_BRIDGE_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(SESSION_TEST_BINARY): $(SESSION_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(AUDIO_PATH_TEST_BINARY): $(AUDIO_PATH_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(FRAME_CLOCK_TEST_BINARY): $(FRAME_CLOCK_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(AUDIO_SCHEDULER_TEST_BINARY): $(AUDIO_SCHEDULER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(HOST_TEST_BINARY): $(HOST_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(RECORDING_FILES_TEST_BINARY): $(RECORDING_FILES_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(SLMODEM_ADAPTER_TEST_BINARY): $(SLMODEM_ADAPTER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(SLMODEM_CALLS_TEST_BINARY): $(SLMODEM_CALLS_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(SLMODEM_DRIVER_TEST_BINARY): $(SLMODEM_DRIVER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(SLMODEM_TERMINAL_TEST_BINARY): $(SLMODEM_TERMINAL_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(RUNTIME_TEST_BINARY): $(RUNTIME_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(TERMINAL_TEST_BINARY): $(TERMINAL_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(LOOP_TEST_BINARY): $(LOOP_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(MAIN_TEST_BINARY): $(MAIN_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(DC_FILTER_TEST_BINARY): $(DC_FILTER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(SINGLE_TONE_TEST_BINARY): $(SINGLE_TONE_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(DIFFERENTIAL_QPSK_TEST_BINARY): $(DIFFERENTIAL_QPSK_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(CRC16_CCITT_FALSE_TEST_BINARY): $(CRC16_CCITT_FALSE_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(SELF_SYNC_SCRAMBLER_TEST_BINARY): $(SELF_SYNC_SCRAMBLER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(NATIVE_CORE_TEST_BINARY): $(NATIVE_CORE_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(FSK_TEST_BINARY): $(FSK_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V8_ANSAM_TEST_BINARY): $(V8_ANSAM_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V8_ANSWERING_TEST_BINARY): $(V8_ANSWERING_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V8_CALLING_TEST_BINARY): $(V8_CALLING_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V8_FSK_TEST_BINARY): $(V8_FSK_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V8_FRAMING_TEST_BINARY): $(V8_FRAMING_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V8_MENU_TEST_BINARY): $(V8_MENU_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(BAUDOT_TEST_BINARY): $(BAUDOT_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V23_TEST_BINARY): $(V23_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V22_FRAMING_TEST_BINARY): $(V22_FRAMING_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V22BIS_RECEIVER_TEST_BINARY): $(V22BIS_RECEIVER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V32_FRAMING_TEST_BINARY): $(V32_FRAMING_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V32_CONDITIONING_TEST_BINARY): $(V32_CONDITIONING_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V32_CONDITIONING_RECEIVER_TEST_BINARY): \
	$(V32_CONDITIONING_RECEIVER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V32_CONSTELLATION_TEST_BINARY): $(V32_CONSTELLATION_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V32_9600_RECEIVER_TEST_BINARY): $(V32_9600_RECEIVER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V32_RATE_SIGNAL_TEST_BINARY): $(V32_RATE_SIGNAL_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V32_STARTUP_TEST_BINARY): $(V32_STARTUP_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V32_LINE_STARTUP_TEST_BINARY): $(V32_LINE_STARTUP_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V32_DATAPUMP_CONDITIONING_TEST_BINARY): \
	$(V32_DATAPUMP_CONDITIONING_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V32_TRELLIS_TEST_BINARY): $(V32_TRELLIS_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V32_TRELLIS_DECODER_TEST_BINARY): \
	$(V32_TRELLIS_DECODER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V32_TRELLIS_RECEIVER_TEST_BINARY): \
	$(V32_TRELLIS_RECEIVER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V32_TRELLIS_ACQUIRER_TEST_BINARY): \
	$(V32_TRELLIS_ACQUIRER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V32_CARRIER_TEST_BINARY): $(V32_CARRIER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_FRAMING_TEST_BINARY): $(V34_FRAMING_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V34_FRAME_SCHEDULE_TEST_BINARY): $(V34_FRAME_SCHEDULE_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V34_MAPPING_PARAMETERS_TEST_BINARY): \
	$(V34_MAPPING_PARAMETERS_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PARSER_TEST_BINARY): $(V34_PARSER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V34_SHELL_MAPPER_TEST_BINARY): $(V34_SHELL_MAPPER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V34_MAPPER_TEST_BINARY): $(V34_MAPPER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V34_TRELLIS_TEST_BINARY): $(V34_TRELLIS_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V34_LATTICE_TEST_BINARY): $(V34_LATTICE_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PRECODER_TEST_BINARY): $(V34_PRECODER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V34_PRECODER_INVERSE_TEST_BINARY): $(V34_PRECODER_INVERSE_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V34_4D_ENCODER_TEST_BINARY): $(V34_4D_ENCODER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V34_BRANCHES_TEST_BINARY): $(V34_BRANCHES_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_SURVIVOR_TEST_BINARY): $(V34_SURVIVOR_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PATH_DECODER_TEST_BINARY): $(V34_PATH_DECODER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_CHANNEL_DECODER_TEST_BINARY): $(V34_CHANNEL_DECODER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_CHANNEL_FRAME_DECODER_TEST_BINARY): \
	$(V34_CHANNEL_FRAME_DECODER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_CHANNEL_CONDITIONER_TEST_BINARY): \
	$(V34_CHANNEL_CONDITIONER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_CHANNEL_FRAME_RECEIVER_TEST_BINARY): \
	$(V34_CHANNEL_FRAME_RECEIVER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_4D_DECODER_TEST_BINARY): $(V34_4D_DECODER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V34_MAPPING_FRAME_ENCODER_TEST_BINARY): \
	$(V34_MAPPING_FRAME_ENCODER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V34_MAPPING_FRAME_DECODER_TEST_BINARY): \
	$(V34_MAPPING_FRAME_DECODER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V34_MAPPING_FRAME_RECEIVER_TEST_BINARY): \
	$(V34_MAPPING_FRAME_RECEIVER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_DATA_RECEIVER_TEST_BINARY): $(V34_DATA_RECEIVER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_NONLINEAR_TEST_BINARY): $(V34_NONLINEAR_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_SYMBOL_POWER_TEST_BINARY): $(V34_SYMBOL_POWER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_MAPPING_FRAME_TRANSMITTER_TEST_BINARY): \
	$(V34_MAPPING_FRAME_TRANSMITTER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_DATA_POINTS_TEST_BINARY): $(V34_DATA_POINTS_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_POWER_CALIBRATION_TEST_BINARY): \
	$(V34_POWER_CALIBRATION_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_DATA_CARRIER_TEST_BINARY): $(V34_DATA_CARRIER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_DATA_TRANSMITTER_TEST_BINARY): $(V34_DATA_TRANSMITTER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_DATA_MAPPING_TEST_BINARY): $(V34_DATA_MAPPING_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_DATA_STARTUP_TEST_BINARY): $(V34_DATA_STARTUP_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_DATA_STREAM_TEST_BINARY): $(V34_DATA_STREAM_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_DATA_LINE_RECEIVER_TEST_BINARY): \
	$(V34_DATA_LINE_RECEIVER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_CONSTELLATION_TEST_BINARY): $(V34_CONSTELLATION_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_INFO1_TEST_BINARY): $(V34_INFO1_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V34_PREEMPHASIS_TEST_BINARY): $(V34_PREEMPHASIS_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE3_PROFILE_TEST_BINARY): $(V34_PHASE3_PROFILE_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V34_PHASE2_SELECTION_TEST_BINARY): \
	$(V34_PHASE2_SELECTION_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V34_PHASE3_PP_SEQUENCE_TEST_BINARY): \
	$(V34_PHASE3_PP_SEQUENCE_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE3_PP_RECEIVER_TEST_BINARY): \
	$(V34_PHASE3_PP_RECEIVER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE3_S_SEQUENCE_TEST_BINARY): $(V34_PHASE3_S_SEQUENCE_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE3_S_RECEIVER_TEST_BINARY): $(V34_PHASE3_S_RECEIVER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_TRAINING_LINE_TEST_BINARY): $(V34_TRAINING_LINE_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE3_S_TRANSMITTER_TEST_BINARY): \
	$(V34_PHASE3_S_TRANSMITTER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE3_TRN_TEST_BINARY): $(V34_PHASE3_TRN_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE3_J_SEQUENCE_TEST_BINARY): \
	$(V34_PHASE3_J_SEQUENCE_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE3_J_RECEIVER_TEST_BINARY): \
	$(V34_PHASE3_J_RECEIVER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE3_J_EQUALIZER_RECEIVER_TEST_BINARY): \
	$(V34_PHASE3_J_EQUALIZER_RECEIVER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE3_RECEIVER_TEST_BINARY): $(V34_PHASE3_RECEIVER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE4_J_PRIME_SEQUENCE_TEST_BINARY): \
	$(V34_PHASE4_J_PRIME_SEQUENCE_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE3_TRANSMITTER_TEST_BINARY): $(V34_PHASE3_TRANSMITTER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE3_LINE_RECEIVER_TEST_BINARY): \
	$(V34_PHASE3_LINE_RECEIVER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE3_EXCHANGE_TEST_BINARY): $(V34_PHASE3_EXCHANGE_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE4_EXCHANGE_TEST_BINARY): $(V34_PHASE4_EXCHANGE_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_CALL_TEST_BINARY): $(V34_CALL_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_TRAINING_TRANSMITTER_TEST_BINARY): $(V34_TRAINING_TRANSMITTER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE4_TRN_TEST_BINARY): $(V34_PHASE4_TRN_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE4_MP_TEST_BINARY): $(V34_PHASE4_MP_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE4_MP_CHANNEL_RECEIVER_TEST_BINARY): \
	$(V34_PHASE4_MP_CHANNEL_RECEIVER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE4_EQUALIZER_RECEIVER_TEST_BINARY): \
	$(V34_PHASE4_EQUALIZER_RECEIVER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE4_LINE_RECEIVER_TEST_BINARY): \
	$(V34_PHASE4_LINE_RECEIVER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE3_PHASE4_LINE_TEST_BINARY): \
	$(V34_PHASE3_PHASE4_LINE_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE4_E_SEQUENCE_TEST_BINARY): \
	$(V34_PHASE4_E_SEQUENCE_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE4_E_RECEIVER_TEST_BINARY): \
	$(V34_PHASE4_E_RECEIVER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_MP_TEST_BINARY): $(V34_MP_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V34_DATA_MODE_TEST_BINARY): $(V34_DATA_MODE_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V34_PHASE2_INFO_TEST_BINARY): $(V34_PHASE2_INFO_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V90_INFO0_TEST_BINARY): $(V90_INFO0_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V90_INFO1_TEST_BINARY): $(V90_INFO1_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V90_MP_TEST_BINARY): $(V90_MP_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V90_MP_RECEIVER_TEST_BINARY): $(V90_MP_RECEIVER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V90_CP_TEST_BINARY): $(V90_CP_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V90_PCM_MAPPING_TEST_BINARY): $(V90_PCM_MAPPING_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V90_PCM_FRAME_ENCODER_TEST_BINARY): \
	$(V90_PCM_FRAME_ENCODER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V90_PCM_FRAME_DECODER_TEST_BINARY): \
	$(V90_PCM_FRAME_DECODER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V90_SIGN_DECODER_TEST_BINARY): $(V90_SIGN_DECODER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V90_SIGN_ENCODER_TEST_BINARY): $(V90_SIGN_ENCODER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V90_SPECTRAL_SHAPER_TEST_BINARY): \
	$(V90_SPECTRAL_SHAPER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V90_DOWNSTREAM_TX_TEST_BINARY): $(V90_DOWNSTREAM_TX_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V90_DOWNSTREAM_STREAM_TEST_BINARY): \
	$(V90_DOWNSTREAM_STREAM_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V90_TRN2D_RX_TEST_BINARY): $(V90_TRN2D_RX_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V90_DOWNSTREAM_RX_TEST_BINARY): $(V90_DOWNSTREAM_RX_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V90_CP_RECEIVER_TEST_BINARY): $(V90_CP_RECEIVER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V90_PHASE4_CP_TX_TEST_BINARY): $(V90_PHASE4_CP_TX_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V90_PHASE4_CP_RX_TEST_BINARY): $(V90_PHASE4_CP_RX_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V90_PHASE4_CP_LINE_TX_TEST_BINARY): \
	$(V90_PHASE4_CP_LINE_TX_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V90_DIL_TEST_BINARY): $(V90_DIL_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V90_JA_TEST_BINARY): $(V90_JA_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V90_JA_SEARCH_TEST_BINARY): $(V90_JA_SEARCH_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V90_JD_TEST_BINARY): $(V90_JD_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V90_PHASE3_SD_TEST_BINARY): $(V90_PHASE3_SD_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V90_PHASE3_SIGNS_TEST_BINARY): $(V90_PHASE3_SIGNS_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V90_PHASE3_ANALOG_TX_TEST_BINARY): \
	$(V90_PHASE3_ANALOG_TX_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V90_PHASE3_ANALOG_RX_TEST_BINARY): \
	$(V90_PHASE3_ANALOG_RX_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V90_PHASE3_ANALOG_LINE_TX_TEST_BINARY): \
	$(V90_PHASE3_ANALOG_LINE_TX_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V90_PHASE3_ANALOG_LINE_RX_TEST_BINARY): \
	$(V90_PHASE3_ANALOG_LINE_RX_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V90_PHASE3_DIGITAL_TX_TEST_BINARY): \
	$(V90_PHASE3_DIGITAL_TX_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V90_PHASE3_ANALOG_STATION_TEST_BINARY): \
	$(V90_PHASE3_ANALOG_STATION_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V90_PHASE4_ANALOG_STATION_TEST_BINARY): \
	$(V90_PHASE4_ANALOG_STATION_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V90_PHASE4_DIGITAL_STATION_TEST_BINARY): \
	$(V90_PHASE4_DIGITAL_STATION_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V90_PHASE4_STATION_EXCHANGE_TEST_BINARY): \
	$(V90_PHASE4_STATION_EXCHANGE_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V90_PHASE4_RI_TEST_BINARY): $(V90_PHASE4_RI_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V90_PHASE4_CP_LINE_RX_TEST_BINARY): \
	$(V90_PHASE4_CP_LINE_RX_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V90_PHASE3_RETURN_LINE_RX_TEST_BINARY): \
	$(V90_PHASE3_RETURN_LINE_RX_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V90_PHASE3_RETURN_LINE_TX_TEST_BINARY): \
	$(V90_PHASE3_RETURN_LINE_TX_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V90_PHASE3_RETURN_RX_TEST_BINARY): \
	$(V90_PHASE3_RETURN_RX_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V90_PHASE3_RETURN_TX_TEST_BINARY): \
	$(V90_PHASE3_RETURN_TX_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V90_PHASE2_INFO0_EXCHANGE_TEST_BINARY): \
	$(V90_PHASE2_INFO0_EXCHANGE_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V90_PHASE2_RANGING_TEST_BINARY): $(V90_PHASE2_RANGING_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V90_PHASE2_INITIAL_PROBE_TEST_BINARY): \
	$(V90_PHASE2_INITIAL_PROBE_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE2_DPSK_TEST_BINARY): $(V34_PHASE2_DPSK_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE2_INFO_AUDIO_TEST_BINARY): \
	$(V34_PHASE2_INFO_AUDIO_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE2_INFO0_EXCHANGE_TEST_BINARY): \
	$(V34_PHASE2_INFO0_EXCHANGE_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE2_PROBE_TEST_BINARY): $(V34_PHASE2_PROBE_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE2_PROBE_ACQUISITION_TEST_BINARY): \
	$(V34_PHASE2_PROBE_ACQUISITION_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE2_PROBE_SPECTRUM_TEST_BINARY): \
	$(V34_PHASE2_PROBE_SPECTRUM_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE2_PROBE_RECEIVER_TEST_BINARY): \
	$(V34_PHASE2_PROBE_RECEIVER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE2_PROBE_SYNCHRONIZER_TEST_BINARY): \
	$(V34_PHASE2_PROBE_SYNCHRONIZER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE2_PROBE_SEQUENCE_TEST_BINARY): \
	$(V34_PHASE2_PROBE_SEQUENCE_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE2_TEST_BINARY): $(V34_PHASE2_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE2_RANGING_TEST_BINARY): $(V34_PHASE2_RANGING_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE2_INITIAL_PROBE_TEST_BINARY): \
	$(V34_PHASE2_INITIAL_PROBE_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE2_RECIPROCAL_PROBE_TEST_BINARY): \
	$(V34_PHASE2_RECIPROCAL_PROBE_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE2_INFO1_EXCHANGE_TEST_BINARY): \
	$(V34_PHASE2_INFO1_EXCHANGE_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE2_TONE_TEST_BINARY): $(V34_PHASE2_TONE_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(QAM_TEST_BINARY): $(QAM_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(QAM_EQUALIZER_TEST_BINARY): $(QAM_EQUALIZER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(FSK_INTEGRATION_TEST_BINARY): $(FSK_INTEGRATION_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(MODEM_OBJECT): ../src/modem_pack.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/config.o: ../src/config.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sip_uri.o: \
	../src/sip_uri.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sdp.o: ../src/sdp.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sip_signaling_pjsip.o: \
	../src/sip_signaling_pjsip.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -D_DEFAULT_SOURCE $(CFLAGS) $(PJSIP_CFLAGS) \
		-MMD -MP -c -o $@ $<

$(BUILD_DIR)/sdp_pjsip.o: \
	../src/sdp_pjsip.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -D_DEFAULT_SOURCE $(CFLAGS) $(PJSIP_CFLAGS) \
		-MMD -MP -c -o $@ $<

$(BUILD_DIR)/g711.o: ../src/g711.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v42_xid.o: \
	../src/v42_xid.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/lapm_v44_session.o: \
	../src/lapm_v44_session.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v44_stream.o: \
	../src/v44_stream.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v44_xid.o: \
	../src/v44_xid.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v44_decoder.o: \
	../src/v44_decoder.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v44_character_path.o: \
	../src/v44_character_path.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v44_duplex.o: \
	../src/v44_duplex.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v44_encoder.o: \
	../src/v44_encoder.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v44_tx_monitor.o: \
	../src/v44_tx_monitor.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v44_dictionary_encoder.o: \
	../src/v44_dictionary_encoder.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v44_transparent_decoder.o: \
	../src/v44_transparent_decoder.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v44_dictionary_decoder.o: \
	../src/v44_dictionary_decoder.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v44_compressed_decoder.o: \
	../src/v44_compressed_decoder.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/rtp.o: ../src/rtp.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/media.o: ../src/media.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/rtp_receiver.o: \
	../src/rtp_receiver.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/rtp_stream.o: \
	../src/rtp_stream.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/rtp_endpoint.o: \
	../src/rtp_endpoint.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sip_rtp.o: \
	../src/sip_rtp.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/rtp_runtime.o: \
	../src/rtp_runtime.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sip_media_bridge.o: \
	../src/sip_media_bridge.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/session.o: \
	../src/session.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/audio_path.o: \
	../src/audio_path.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/frame_clock.o: \
	../src/frame_clock.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/audio_scheduler.o: \
	../src/audio_scheduler.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/host.o: \
	../src/host.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/recording_files.o: \
	../src/recording_files.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/slmodem_adapter.o: \
	../src/slmodem_adapter.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/slmodem_calls.o: \
	../src/slmodem_calls.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/slmodem_driver.o: \
	../src/slmodem_driver.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/slmodem_terminal.o: \
	../src/slmodem_terminal.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/runtime.o: ../src/runtime.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/terminal.o: \
	../src/terminal.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/loop.o: ../src/loop.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/main_testable.o: \
	../src/main.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -DPROGRAM_NO_PROCESS_ENTRY $(CFLAGS) \
		-MMD -MP -c -o $@ $<

$(BUILD_DIR)/dc_filter.o: \
	$(DSP_SOURCE_DIR)/dc_filter.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/single_tone.o: \
	$(DSP_SOURCE_DIR)/single_tone.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/differential_qpsk.o: \
	$(DSP_SOURCE_DIR)/differential_qpsk.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(SELF_SYNC_SCRAMBLER_OBJECT): \
	$(DSP_SOURCE_DIR)/self_sync_scrambler.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(CRC16_CCITT_FALSE_OBJECT): \
	$(DSP_SOURCE_DIR)/crc16_ccitt_false.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/datapumps.o: \
	$(DSP_SOURCE_DIR)/datapumps.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/fsk.o: \
	$(DSP_SOURCE_DIR)/fsk.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v21.o: \
	$(DSP_V21_202_SOURCE_DIR)/v21.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/baudot.o: \
	$(DSP_V21_202_SOURCE_DIR)/baudot.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/baudot_datapump.o: \
	$(DSP_V21_202_SOURCE_DIR)/baudot_datapump.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/fsk300_datapump.o: \
	$(DSP_V21_202_SOURCE_DIR)/fsk300_datapump.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v8_datapump.o: \
	$(DSP_SOURCE_DIR)/v8_datapump.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v23_datapump.o: \
	$(DSP_V21_202_SOURCE_DIR)/v23_datapump.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v22_framing.o: \
	$(DSP_V22_SOURCE_DIR)/v22_framing.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v22bis_symbols.o: \
	$(DSP_V22_SOURCE_DIR)/v22bis_symbols.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v22bis_receiver.o: \
	$(DSP_V22_SOURCE_DIR)/v22bis_receiver.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/framing.o: \
	$(DSP_V32_SOURCE_DIR)/framing.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/conditioning.o: \
	$(DSP_V32_SOURCE_DIR)/conditioning.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/conditioning_receiver.o: \
	$(DSP_V32_SOURCE_DIR)/conditioning_receiver.c | \
	$(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/constellation.o: \
	$(DSP_V32_SOURCE_DIR)/constellation.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/uncoded_receiver.o: \
	$(DSP_V32_SOURCE_DIR)/uncoded_receiver.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/rate_signal.o: \
	$(DSP_V32_SOURCE_DIR)/rate_signal.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/startup.o: \
	$(DSP_V32_SOURCE_DIR)/startup.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/line_startup.o: \
	$(DSP_V32_SOURCE_DIR)/line_startup.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/trellis.o: \
	$(DSP_V32_SOURCE_DIR)/trellis.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/trellis_decoder.o: \
	$(DSP_V32_SOURCE_DIR)/trellis_decoder.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/trellis_receiver.o: \
	$(DSP_V32_SOURCE_DIR)/trellis_receiver.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/trellis_acquirer.o: \
	$(DSP_V32_SOURCE_DIR)/trellis_acquirer.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/datapump.o: \
	$(DSP_V32_SOURCE_DIR)/datapump.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_framing.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_framing.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_frame_schedule.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_frame_schedule.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_mapping_parameters.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_mapping_parameters.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_parser.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_parser.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_shell_mapper.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_shell_mapper.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_mapper.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_mapper.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_lattice.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_lattice.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_branches.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_branches.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_survivor.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_survivor.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_path_decoder.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_path_decoder.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_channel_decoder.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_channel_decoder.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_channel_frame_decoder.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_channel_frame_decoder.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_channel_conditioner.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_channel_conditioner.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_channel_frame_receiver.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_channel_frame_receiver.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_data_channel_receiver.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_data_channel_receiver.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_data_equalizer_receiver.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_data_equalizer_receiver.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_data_line_receiver.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_data_line_receiver.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_equalizer_handoff.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_equalizer_handoff.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_trellis.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_trellis.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_precoder.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_precoder.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_4d_encoder.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_4d_encoder.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_4d_decoder.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_4d_decoder.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_mapping_frame_encoder.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_mapping_frame_encoder.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_mapping_frame_decoder.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_mapping_frame_decoder.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_mapping_frame_receiver.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_mapping_frame_receiver.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_data_receiver.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_data_receiver.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_nonlinear.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_nonlinear.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_symbol_power.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_symbol_power.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_mapping_frame_transmitter.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_mapping_frame_transmitter.c | \
	$(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_data_points.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_data_points.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_power_calibration.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_power_calibration.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_data_carrier.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_data_carrier.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_data_line_transmitter.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_data_line_transmitter.c | \
	$(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_data_transmitter.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_data_transmitter.c | \
	$(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_data_stream.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_data_stream.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_constellation.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_constellation.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_info0.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_info0.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_info0.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_info0.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_info1.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_info1.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_cp.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_cp.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_pcm_mapping.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_pcm_mapping.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_pcm_frame_encoder.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_pcm_frame_encoder.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_pcm_frame_decoder.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_pcm_frame_decoder.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_sign_decoder.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_sign_decoder.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_sign_encoder.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_sign_encoder.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_spectral_shaper.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_spectral_shaper.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_downstream_tx.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_downstream_tx.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_trn2d_rx.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_trn2d_rx.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_downstream_rx.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_downstream_rx.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_cp_receiver.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_cp_receiver.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_phase4_cp_tx.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_phase4_cp_tx.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_phase4_cp_rx.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_phase4_cp_rx.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_phase4_cp_line_tx.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_phase4_cp_line_tx.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_phase4_analog_station.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_phase4_analog_station.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_phase4_digital_station.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_phase4_digital_station.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_phase4_cp_line_rx.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_phase4_cp_line_rx.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_dil.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_dil.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_ja.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_ja.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_ja_search.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_ja_search.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_jd.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_jd.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_phase3_sd.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_phase3_sd.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_phase3_sd_receiver.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_phase3_sd_receiver.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_pcm_symbol.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_pcm_symbol.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_phase3_signs.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_phase3_signs.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_phase3_sign_receiver.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_phase3_sign_receiver.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_phase4_ri.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_phase4_ri.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_phase3_analog_tx.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_phase3_analog_tx.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_phase3_analog_rx.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_phase3_analog_rx.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_phase3_analog_line_rx.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_phase3_analog_line_rx.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_phase3_analog_line_tx.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_phase3_analog_line_tx.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_phase3_digital_tx.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_phase3_digital_tx.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_phase3_analog_station.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_phase3_analog_station.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_phase3_digital_station.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_phase3_digital_station.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_phase3_return_line_rx.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_phase3_return_line_rx.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_phase3_return_line_tx.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_phase3_return_line_tx.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_phase3_return_rx.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_phase3_return_rx.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_phase3_return_tx.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_phase3_return_tx.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_phase2_info0_exchange.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_phase2_info0_exchange.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_phase2_ranging.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_phase2_ranging.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_phase2_initial_probe.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_phase2_initial_probe.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_phase2_probe_turnaround.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_phase2_probe_turnaround.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_phase2_reciprocal_probe.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_phase2_reciprocal_probe.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_phase2_info1_exchange.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_phase2_info1_exchange.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_phase2.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_phase2.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_info1.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_info1.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_preemphasis.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_preemphasis.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase3_profile.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_profile.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase2_selection.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_selection.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase3_transmitter.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_transmitter.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase3_line_transmitter.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_line_transmitter.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase3_exchange.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_exchange.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase4_exchange.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase4_exchange.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_call.o: $(DSP_PCM_SOURCE_DIR)/v34_call.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_datapump.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_datapump.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_training_transmitter.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_training_transmitter.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase3_pp_sequence.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_pp_sequence.c | \
	$(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase3_pp_receiver.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_pp_receiver.c | \
	$(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase3_s_sequence.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_s_sequence.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase3_s_receiver.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_s_receiver.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_training_line.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_training_line.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase3_s_transmitter.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_s_transmitter.c | \
	$(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase3_trn.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_trn.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase3_j_sequence.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_j_sequence.c | \
	$(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase3_j_receiver.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_j_receiver.c | \
	$(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase3_j_equalizer_receiver.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_j_equalizer_receiver.c | \
	$(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase3_receiver.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_receiver.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase3_line_receiver.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase3_line_receiver.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase4_j_prime_sequence.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase4_j_prime_sequence.c | \
	$(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase4_b1_handoff.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase4_b1_handoff.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase4_trn.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase4_trn.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase4_mp.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase4_mp.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase4_mp_receiver.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase4_mp_receiver.c | \
	$(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase4_mp_channel_receiver.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase4_mp_channel_receiver.c | \
	$(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase4_equalizer_receiver.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase4_equalizer_receiver.c | \
	$(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase4_line_receiver.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase4_line_receiver.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase4_line_handoff.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase4_line_handoff.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_training_receiver.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_training_receiver.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_training_channel.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_training_channel.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase4_e_channel_receiver.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase4_e_channel_receiver.c | \
	$(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase4_e_sequence.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase4_e_sequence.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase4_e_receiver.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase4_e_receiver.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_mp.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_mp.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_mp.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_mp.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v90_mp_receiver.o: \
	$(DSP_PCM_SOURCE_DIR)/v90_mp_receiver.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_data_mapping.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_data_mapping.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_data_mode.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_data_mode.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_data_startup.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_data_startup.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase2_info.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_info.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase2.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase2_dpsk.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_dpsk.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase2_info_audio.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_info_audio.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/phase2_info0_exchange.o: \
	$(DSP_PCM_SOURCE_DIR)/phase2_info0_exchange.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/phase2_initial_probe.o: \
	$(DSP_PCM_SOURCE_DIR)/phase2_initial_probe.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/phase2_ranging.o: \
	$(DSP_PCM_SOURCE_DIR)/phase2_ranging.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase2_info0_exchange.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_info0_exchange.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/phase2_info1_exchange.o: \
	$(DSP_PCM_SOURCE_DIR)/phase2_info1_exchange.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase2_info1_exchange.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_info1_exchange.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase2_initial_probe.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_initial_probe.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase2_probe.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_probe.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase2_probe_acquisition.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_probe_acquisition.c | \
	$(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase2_probe_receiver.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_probe_receiver.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase2_probe_spectrum.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_probe_spectrum.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase2_probe_synchronizer.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_probe_synchronizer.c | \
	$(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase2_probe_sequence.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_probe_sequence.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/phase2_probe_turnaround.o: \
	$(DSP_PCM_SOURCE_DIR)/phase2_probe_turnaround.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase2_probe_turnaround.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_probe_turnaround.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/phase2_reciprocal_probe.o: \
	$(DSP_PCM_SOURCE_DIR)/phase2_reciprocal_probe.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase2_reciprocal_probe.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_reciprocal_probe.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase2_ranging.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_ranging.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v34_phase2_tone.o: \
	$(DSP_PCM_SOURCE_DIR)/v34_phase2_tone.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v22_datapump.o: \
	$(DSP_V22_SOURCE_DIR)/v22_datapump.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/qam.o: \
	$(DSP_SOURCE_DIR)/qam.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/qam_gain.o: \
	$(DSP_SOURCE_DIR)/qam_gain.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/qam_equalizer.o: \
	$(DSP_SOURCE_DIR)/qam_equalizer.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v8_ansam.o: \
	$(DSP_SOURCE_DIR)/v8_ansam.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v8_answering.o: \
	$(DSP_SOURCE_DIR)/v8_answering.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v8_calling.o: \
	$(DSP_SOURCE_DIR)/v8_calling.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v8_fsk.o: \
	$(DSP_SOURCE_DIR)/v8_fsk.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v8_framing.o: \
	$(DSP_SOURCE_DIR)/v8_framing.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v8_menu.o: \
	$(DSP_SOURCE_DIR)/v8_menu.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/native_%.o: ../src/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(filter-out -Werror,$(CFLAGS)) \
		-MMD -MP -c -o $@ $<

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPENDENCIES)
