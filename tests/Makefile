CC ?= cc
PYTHON ?= python3
CPPFLAGS ?=
CFLAGS ?= -O2 -g
LDFLAGS ?=

PJSIP_PACKAGE ?= libpjproject
PJSIP_AVAILABLE := $(shell \
	pkg-config --exists $(PJSIP_PACKAGE) 2>/dev/null && echo yes)
PJSIP_CFLAGS := $(shell pkg-config --cflags $(PJSIP_PACKAGE) 2>/dev/null)
PJSIP_LDLIBS := $(shell pkg-config --static --libs $(PJSIP_PACKAGE) 2>/dev/null)

CPPFLAGS += -D_POSIX_C_SOURCE=200809L -I../src -I../dsp \
	-I../dsp/v21_202 -I../dsp/v22 -I../dsp/v32 -I../dsp/pcm
CFLAGS += -std=c11 -Wall -Wextra -Werror

BUILD_DIR := build
DSP_SOURCE_DIR := ../dsp
DSP_V21_202_SOURCE_DIR := $(DSP_SOURCE_DIR)/v21_202
DSP_V22_SOURCE_DIR := $(DSP_SOURCE_DIR)/v22
DSP_V32_SOURCE_DIR := $(DSP_SOURCE_DIR)/v32
DSP_PCM_SOURCE_DIR := $(DSP_SOURCE_DIR)/pcm
PACK_TEST_BINARY := $(BUILD_DIR)/test_modem_pack
PACK_TEST_SOURCES := test_modem_pack.c modem_stubs.c
PACK_TEST_OBJECTS := $(PACK_TEST_SOURCES:%.c=$(BUILD_DIR)/%.o)
MODEM_OBJECT := $(BUILD_DIR)/modem_pack.o
CONFIG_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_config
CONFIG_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_config.o \
	$(BUILD_DIR)/sippymodem_config.o $(BUILD_DIR)/sippymodem_media.o \
	$(BUILD_DIR)/sippymodem_g711.o
SIP_URI_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_sip_uri
SIP_URI_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_sip_uri.o \
	$(BUILD_DIR)/sippymodem_sip_uri.o
SDP_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_sdp
SDP_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_sdp.o \
	$(BUILD_DIR)/sippymodem_sdp.o $(BUILD_DIR)/sippymodem_media.o \
	$(BUILD_DIR)/sippymodem_g711.o
PJSIP_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_sip_signaling
PJSIP_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_sip_signaling.o \
	$(BUILD_DIR)/sippymodem_sip_signaling_pjsip.o \
	$(BUILD_DIR)/sippymodem_sdp_pjsip.o $(BUILD_DIR)/sippymodem_sdp.o \
	$(BUILD_DIR)/sippymodem_sip_uri.o \
	$(BUILD_DIR)/sippymodem_config.o $(BUILD_DIR)/sippymodem_media.o \
	$(BUILD_DIR)/sippymodem_g711.o
G711_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_g711
G711_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_g711.o \
	$(BUILD_DIR)/sippymodem_g711.o
V42_XID_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_v42_xid
V42_XID_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_v42_xid.o \
	$(BUILD_DIR)/sippymodem_v42_xid.o \
	$(BUILD_DIR)/sippymodem_v44_xid.o \
	$(BUILD_DIR)/sippymodem_v44_stream.o
LAPM_V44_SESSION_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_lapm_v44_session
LAPM_V44_SESSION_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_lapm_v44_session.o \
	$(BUILD_DIR)/sippymodem_lapm_v44_session.o \
	$(BUILD_DIR)/sippymodem_v42_xid.o \
	$(BUILD_DIR)/sippymodem_v44_xid.o \
	$(BUILD_DIR)/sippymodem_v44_stream.o
V44_STREAM_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_v44_stream
V44_STREAM_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_v44_stream.o \
	$(BUILD_DIR)/sippymodem_v44_stream.o
V44_XID_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_v44_xid
V44_XID_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_v44_xid.o \
	$(BUILD_DIR)/sippymodem_v44_xid.o \
	$(BUILD_DIR)/sippymodem_v44_stream.o
V44_DECODER_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_v44_decoder
V44_DECODER_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_v44_decoder.o \
	$(BUILD_DIR)/sippymodem_v44_decoder.o \
	$(BUILD_DIR)/sippymodem_v44_compressed_decoder.o \
	$(BUILD_DIR)/sippymodem_v44_transparent_decoder.o \
	$(BUILD_DIR)/sippymodem_v44_dictionary_decoder.o \
	$(BUILD_DIR)/sippymodem_v44_stream.o
V44_CHARACTER_PATH_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v44_character_path
V44_CHARACTER_PATH_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v44_character_path.o \
	$(BUILD_DIR)/sippymodem_v44_character_path.o \
	$(BUILD_DIR)/sippymodem_v44_duplex.o \
	$(BUILD_DIR)/sippymodem_v44_tx_monitor.o \
	$(BUILD_DIR)/sippymodem_v44_encoder.o \
	$(BUILD_DIR)/sippymodem_v44_dictionary_encoder.o \
	$(BUILD_DIR)/sippymodem_v44_decoder.o \
	$(BUILD_DIR)/sippymodem_v44_compressed_decoder.o \
	$(BUILD_DIR)/sippymodem_v44_transparent_decoder.o \
	$(BUILD_DIR)/sippymodem_v44_dictionary_decoder.o \
	$(BUILD_DIR)/sippymodem_v44_stream.o
V44_DUPLEX_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_v44_duplex
V44_DUPLEX_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_v44_duplex.o \
	$(BUILD_DIR)/sippymodem_v44_duplex.o \
	$(BUILD_DIR)/sippymodem_v44_tx_monitor.o \
	$(BUILD_DIR)/sippymodem_v44_encoder.o \
	$(BUILD_DIR)/sippymodem_v44_dictionary_encoder.o \
	$(BUILD_DIR)/sippymodem_v44_decoder.o \
	$(BUILD_DIR)/sippymodem_v44_compressed_decoder.o \
	$(BUILD_DIR)/sippymodem_v44_transparent_decoder.o \
	$(BUILD_DIR)/sippymodem_v44_dictionary_decoder.o \
	$(BUILD_DIR)/sippymodem_v44_stream.o
V44_ENCODER_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_v44_encoder
V44_ENCODER_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_v44_encoder.o \
	$(BUILD_DIR)/sippymodem_v44_test_wire.o \
	$(BUILD_DIR)/sippymodem_v44_encoder.o \
	$(BUILD_DIR)/sippymodem_v44_dictionary_encoder.o \
	$(BUILD_DIR)/sippymodem_v44_decoder.o \
	$(BUILD_DIR)/sippymodem_v44_compressed_decoder.o \
	$(BUILD_DIR)/sippymodem_v44_transparent_decoder.o \
	$(BUILD_DIR)/sippymodem_v44_dictionary_decoder.o \
	$(BUILD_DIR)/sippymodem_v44_stream.o
V44_ENCODER_MODES_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v44_encoder_modes
V44_ENCODER_MODES_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v44_encoder_modes.o \
	$(BUILD_DIR)/sippymodem_v44_encoder.o \
	$(BUILD_DIR)/sippymodem_v44_dictionary_encoder.o \
	$(BUILD_DIR)/sippymodem_v44_decoder.o \
	$(BUILD_DIR)/sippymodem_v44_compressed_decoder.o \
	$(BUILD_DIR)/sippymodem_v44_transparent_decoder.o \
	$(BUILD_DIR)/sippymodem_v44_dictionary_decoder.o \
	$(BUILD_DIR)/sippymodem_v44_stream.o
V44_TX_MONITOR_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v44_tx_monitor
V44_TX_MONITOR_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v44_tx_monitor.o \
	$(BUILD_DIR)/sippymodem_v44_tx_monitor.o \
	$(BUILD_DIR)/sippymodem_v44_encoder.o \
	$(BUILD_DIR)/sippymodem_v44_dictionary_encoder.o \
	$(BUILD_DIR)/sippymodem_v44_stream.o
V44_DICTIONARY_ENCODER_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v44_dictionary_encoder
V44_DICTIONARY_ENCODER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v44_dictionary_encoder.o \
	$(BUILD_DIR)/sippymodem_v44_dictionary_encoder.o \
	$(BUILD_DIR)/sippymodem_v44_stream.o
V44_TRANSPARENT_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v44_transparent_decoder
V44_TRANSPARENT_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v44_transparent_decoder.o \
	$(BUILD_DIR)/sippymodem_v44_transparent_decoder.o
V44_DICTIONARY_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v44_dictionary_decoder
V44_DICTIONARY_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v44_dictionary_decoder.o \
	$(BUILD_DIR)/sippymodem_v44_dictionary_decoder.o \
	$(BUILD_DIR)/sippymodem_v44_stream.o
V44_COMPRESSED_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v44_compressed_decoder
V44_COMPRESSED_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v44_compressed_decoder.o \
	$(BUILD_DIR)/sippymodem_v44_compressed_decoder.o \
	$(BUILD_DIR)/sippymodem_v44_dictionary_decoder.o \
	$(BUILD_DIR)/sippymodem_v44_stream.o
V44_COMPRESSED_CONTROL_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v44_compressed_controls
V44_COMPRESSED_CONTROL_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v44_compressed_controls.o \
	$(BUILD_DIR)/sippymodem_v44_test_wire.o \
	$(BUILD_DIR)/sippymodem_v44_compressed_decoder.o \
	$(BUILD_DIR)/sippymodem_v44_dictionary_decoder.o \
	$(BUILD_DIR)/sippymodem_v44_stream.o
V44_COMPRESSED_EXTENSION_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v44_compressed_extensions
V44_COMPRESSED_EXTENSION_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v44_compressed_extensions.o \
	$(BUILD_DIR)/sippymodem_v44_test_wire.o \
	$(BUILD_DIR)/sippymodem_v44_compressed_decoder.o \
	$(BUILD_DIR)/sippymodem_v44_dictionary_decoder.o \
	$(BUILD_DIR)/sippymodem_v44_stream.o
V44_COMPRESSED_INCREMENTAL_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v44_compressed_incremental
V44_COMPRESSED_INCREMENTAL_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v44_compressed_incremental.o \
	$(BUILD_DIR)/sippymodem_v44_test_wire.o \
	$(BUILD_DIR)/sippymodem_v44_compressed_decoder.o \
	$(BUILD_DIR)/sippymodem_v44_dictionary_decoder.o \
	$(BUILD_DIR)/sippymodem_v44_stream.o
V44_COMPRESSED_VALIDATION_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v44_compressed_validation
V44_COMPRESSED_VALIDATION_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v44_compressed_validation.o \
	$(BUILD_DIR)/sippymodem_v44_test_wire.o \
	$(BUILD_DIR)/sippymodem_v44_compressed_decoder.o \
	$(BUILD_DIR)/sippymodem_v44_dictionary_decoder.o \
	$(BUILD_DIR)/sippymodem_v44_stream.o
RTP_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_rtp
RTP_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_rtp.o \
	$(BUILD_DIR)/sippymodem_rtp.o
MEDIA_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_media
MEDIA_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_media.o \
	$(BUILD_DIR)/sippymodem_media.o $(BUILD_DIR)/sippymodem_g711.o
RECEIVER_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_rtp_receiver
RECEIVER_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_rtp_receiver.o \
	$(BUILD_DIR)/sippymodem_rtp_receiver.o $(BUILD_DIR)/sippymodem_rtp.o
STREAM_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_rtp_stream
STREAM_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_rtp_stream.o \
	$(BUILD_DIR)/sippymodem_rtp_stream.o \
	$(BUILD_DIR)/sippymodem_rtp_receiver.o $(BUILD_DIR)/sippymodem_rtp.o \
	$(BUILD_DIR)/sippymodem_media.o $(BUILD_DIR)/sippymodem_g711.o
ENDPOINT_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_rtp_endpoint
ENDPOINT_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_rtp_endpoint.o \
	$(BUILD_DIR)/sippymodem_rtp_endpoint.o \
	$(BUILD_DIR)/sippymodem_rtp_stream.o \
	$(BUILD_DIR)/sippymodem_rtp_receiver.o $(BUILD_DIR)/sippymodem_rtp.o \
	$(BUILD_DIR)/sippymodem_media.o $(BUILD_DIR)/sippymodem_g711.o
SIP_RTP_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_sip_rtp
SIP_RTP_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_sip_rtp.o \
	$(BUILD_DIR)/sippymodem_sip_rtp.o \
	$(BUILD_DIR)/sippymodem_rtp_endpoint.o \
	$(BUILD_DIR)/sippymodem_rtp_stream.o \
	$(BUILD_DIR)/sippymodem_rtp_receiver.o $(BUILD_DIR)/sippymodem_rtp.o \
	$(BUILD_DIR)/sippymodem_media.o $(BUILD_DIR)/sippymodem_g711.o
RTP_RUNTIME_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_rtp_runtime
RTP_RUNTIME_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_rtp_runtime.o \
	$(BUILD_DIR)/sippymodem_rtp_runtime.o \
	$(BUILD_DIR)/sippymodem_rtp_endpoint.o \
	$(BUILD_DIR)/sippymodem_rtp_stream.o \
	$(BUILD_DIR)/sippymodem_rtp_receiver.o $(BUILD_DIR)/sippymodem_rtp.o \
	$(BUILD_DIR)/sippymodem_config.o \
	$(BUILD_DIR)/sippymodem_media.o $(BUILD_DIR)/sippymodem_g711.o
SIP_MEDIA_BRIDGE_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_sip_media_bridge
SIP_MEDIA_BRIDGE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_sip_media_bridge.o \
	$(BUILD_DIR)/sippymodem_sip_media_bridge.o \
	$(BUILD_DIR)/sippymodem_sip_rtp.o \
	$(BUILD_DIR)/sippymodem_rtp_endpoint.o \
	$(BUILD_DIR)/sippymodem_rtp_stream.o \
	$(BUILD_DIR)/sippymodem_rtp_receiver.o $(BUILD_DIR)/sippymodem_rtp.o \
	$(BUILD_DIR)/sippymodem_media.o $(BUILD_DIR)/sippymodem_g711.o
SESSION_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_session
SESSION_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_session.o \
	$(BUILD_DIR)/sippymodem_session.o \
	$(BUILD_DIR)/sippymodem_sip_media_bridge.o \
	$(BUILD_DIR)/sippymodem_sip_rtp.o \
	$(BUILD_DIR)/sippymodem_rtp_runtime.o \
	$(BUILD_DIR)/sippymodem_rtp_endpoint.o \
	$(BUILD_DIR)/sippymodem_rtp_stream.o \
	$(BUILD_DIR)/sippymodem_rtp_receiver.o $(BUILD_DIR)/sippymodem_rtp.o \
	$(BUILD_DIR)/sippymodem_config.o \
	$(BUILD_DIR)/sippymodem_media.o $(BUILD_DIR)/sippymodem_g711.o
AUDIO_PATH_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_audio_path
AUDIO_PATH_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_audio_path.o \
	$(BUILD_DIR)/sippymodem_audio_path.o \
	$(BUILD_DIR)/sippymodem_media.o $(BUILD_DIR)/sippymodem_g711.o
FRAME_CLOCK_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_frame_clock
FRAME_CLOCK_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_frame_clock.o \
	$(BUILD_DIR)/sippymodem_frame_clock.o
AUDIO_SCHEDULER_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_audio_scheduler
AUDIO_SCHEDULER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_audio_scheduler.o \
	$(BUILD_DIR)/sippymodem_audio_scheduler.o \
	$(BUILD_DIR)/sippymodem_frame_clock.o
HOST_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_host
HOST_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_host.o \
	$(BUILD_DIR)/sippymodem_host.o \
	$(BUILD_DIR)/sippymodem_session.o \
	$(BUILD_DIR)/sippymodem_sip_media_bridge.o \
	$(BUILD_DIR)/sippymodem_sip_rtp.o \
	$(BUILD_DIR)/sippymodem_rtp_runtime.o \
	$(BUILD_DIR)/sippymodem_rtp_endpoint.o \
	$(BUILD_DIR)/sippymodem_rtp_stream.o \
	$(BUILD_DIR)/sippymodem_rtp_receiver.o \
	$(BUILD_DIR)/sippymodem_rtp.o \
	$(BUILD_DIR)/sippymodem_audio_path.o \
	$(BUILD_DIR)/sippymodem_audio_scheduler.o \
	$(BUILD_DIR)/sippymodem_frame_clock.o \
	$(BUILD_DIR)/sippymodem_config.o \
	$(BUILD_DIR)/sippymodem_media.o \
	$(BUILD_DIR)/sippymodem_g711.o
RECORDING_FILES_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_recording_files
RECORDING_FILES_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_recording_files.o \
	$(BUILD_DIR)/sippymodem_recording_files.o \
	$(BUILD_DIR)/sippymodem_media.o $(BUILD_DIR)/sippymodem_g711.o
SLMODEM_ADAPTER_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_slmodem_adapter
SLMODEM_ADAPTER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_slmodem_adapter.o \
	$(BUILD_DIR)/sippymodem_slmodem_adapter.o
SLMODEM_CALLS_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_slmodem_calls
SLMODEM_CALLS_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_slmodem_calls.o \
	$(BUILD_DIR)/sippymodem_slmodem_calls.o
SLMODEM_DRIVER_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_slmodem_driver
SLMODEM_DRIVER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_slmodem_driver.o \
	$(BUILD_DIR)/sippymodem_slmodem_driver.o
SLMODEM_TERMINAL_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_slmodem_terminal
SLMODEM_TERMINAL_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_slmodem_terminal.o \
	$(BUILD_DIR)/sippymodem_slmodem_terminal.o
RUNTIME_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_runtime
RUNTIME_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_runtime.o \
	$(BUILD_DIR)/sippymodem_runtime.o \
	$(BUILD_DIR)/sippymodem_slmodem_terminal.o \
	$(BUILD_DIR)/sippymodem_terminal.o
TERMINAL_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_terminal
TERMINAL_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_terminal.o \
	$(BUILD_DIR)/sippymodem_terminal.o
LOOP_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_loop
LOOP_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_loop.o \
	$(BUILD_DIR)/sippymodem_loop.o
MAIN_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_main
MAIN_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_main.o \
	$(BUILD_DIR)/sippymodem_main_testable.o
DC_FILTER_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_dc_filter
DC_FILTER_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_dc_filter.o \
	$(BUILD_DIR)/sippymodem_dc_filter.o
SINGLE_TONE_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_single_tone
SINGLE_TONE_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_single_tone.o \
	$(BUILD_DIR)/sippymodem_single_tone.o \
	$(BUILD_DIR)/sippymodem_g711.o
DIFFERENTIAL_QPSK_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_differential_qpsk
DIFFERENTIAL_QPSK_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_differential_qpsk.o \
	$(BUILD_DIR)/sippymodem_differential_qpsk.o
SELF_SYNC_SCRAMBLER_OBJECT := \
	$(BUILD_DIR)/sippymodem_self_sync_scrambler.o
CRC16_CCITT_FALSE_OBJECT := \
	$(BUILD_DIR)/sippymodem_crc16_ccitt_false.o
CRC16_CCITT_FALSE_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_crc16_ccitt_false
CRC16_CCITT_FALSE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_crc16_ccitt_false.o \
	$(CRC16_CCITT_FALSE_OBJECT)
SELF_SYNC_SCRAMBLER_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_self_sync_scrambler
SELF_SYNC_SCRAMBLER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_self_sync_scrambler.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT)
NATIVE_CORE_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_native_core
NATIVE_CORE_SOURCE_NAMES := homolog_data modem modem_at modem_comp \
	modem_debug modem_ec modem_pack modem_param modem_timer sysdep_common
NATIVE_CORE_OBJECTS := $(addprefix $(BUILD_DIR)/native_, \
	$(addsuffix .o, $(NATIVE_CORE_SOURCE_NAMES))) \
	$(BUILD_DIR)/sippymodem_lapm_v44_session.o \
	$(BUILD_DIR)/sippymodem_v42_xid.o \
	$(BUILD_DIR)/sippymodem_v44_xid.o \
	$(BUILD_DIR)/sippymodem_v44_stream.o
NATIVE_CORE_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_native_core.o \
	$(NATIVE_CORE_OBJECTS) \
	$(BUILD_DIR)/qam_equalizer.o \
	$(BUILD_DIR)/sippymodem_datapumps.o \
	$(BUILD_DIR)/baudot.o \
	$(BUILD_DIR)/baudot_datapump.o \
	$(BUILD_DIR)/sippymodem_dc_filter.o \
	$(BUILD_DIR)/sippymodem_differential_qpsk.o \
	$(BUILD_DIR)/sippymodem_fsk.o \
	$(BUILD_DIR)/fsk300_datapump.o \
	$(BUILD_DIR)/v21.o \
	$(BUILD_DIR)/sippymodem_v8_ansam.o \
	$(BUILD_DIR)/sippymodem_v8_answering.o \
	$(BUILD_DIR)/sippymodem_v8_calling.o \
	$(BUILD_DIR)/sippymodem_v8_datapump.o \
	$(BUILD_DIR)/sippymodem_v8_fsk.o \
	$(BUILD_DIR)/sippymodem_v8_framing.o \
	$(BUILD_DIR)/sippymodem_v8_menu.o \
	$(BUILD_DIR)/sippymodem_qam.o \
	$(BUILD_DIR)/sippymodem_qam_gain.o \
	$(BUILD_DIR)/sippymodem_single_tone.o \
	$(BUILD_DIR)/sippymodem_v22_datapump.o \
	$(BUILD_DIR)/sippymodem_v22_framing.o \
	$(BUILD_DIR)/sippymodem_v22bis_receiver.o \
	$(BUILD_DIR)/sippymodem_v22bis_symbols.o \
	$(BUILD_DIR)/sippymodem_v32_datapump.o \
	$(BUILD_DIR)/sippymodem_v32_9600_receiver.o \
	$(BUILD_DIR)/sippymodem_v32_conditioning.o \
	$(BUILD_DIR)/sippymodem_v32_conditioning_receiver.o \
	$(BUILD_DIR)/sippymodem_v32_constellation.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/sippymodem_v32_framing.o \
	$(BUILD_DIR)/sippymodem_v32_line_startup.o \
	$(BUILD_DIR)/sippymodem_v32_rate_signal.o \
	$(BUILD_DIR)/sippymodem_v32_startup.o \
	$(BUILD_DIR)/sippymodem_v32_trellis.o \
	$(BUILD_DIR)/sippymodem_v32_trellis_decoder.o \
	$(BUILD_DIR)/v23_datapump.o
FSK_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_fsk
FSK_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_fsk.o \
	$(BUILD_DIR)/sippymodem_fsk.o \
	$(BUILD_DIR)/v21.o
V8_ANSAM_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_v8_ansam
V8_ANSAM_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_v8_ansam.o \
	$(BUILD_DIR)/sippymodem_v8_ansam.o \
	$(BUILD_DIR)/sippymodem_g711.o
V8_ANSWERING_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_v8_answering
V8_ANSWERING_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_v8_answering.o \
	$(BUILD_DIR)/sippymodem_v8_answering.o \
	$(BUILD_DIR)/sippymodem_v8_calling.o \
	$(BUILD_DIR)/sippymodem_v8_ansam.o \
	$(BUILD_DIR)/sippymodem_v8_fsk.o \
	$(BUILD_DIR)/sippymodem_v8_menu.o \
	$(BUILD_DIR)/sippymodem_v8_framing.o \
	$(BUILD_DIR)/v21.o \
	$(BUILD_DIR)/sippymodem_fsk.o
V8_CALLING_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_v8_calling
V8_CALLING_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_v8_calling.o \
	$(BUILD_DIR)/sippymodem_v8_calling.o \
	$(BUILD_DIR)/sippymodem_v8_ansam.o \
	$(BUILD_DIR)/sippymodem_v8_fsk.o \
	$(BUILD_DIR)/sippymodem_v8_menu.o \
	$(BUILD_DIR)/sippymodem_v8_framing.o \
	$(BUILD_DIR)/v21.o \
	$(BUILD_DIR)/sippymodem_fsk.o
V8_FSK_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_v8_fsk
V8_FSK_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_v8_fsk.o \
	$(BUILD_DIR)/sippymodem_v8_fsk.o \
	$(BUILD_DIR)/sippymodem_v8_menu.o \
	$(BUILD_DIR)/sippymodem_v8_framing.o \
	$(BUILD_DIR)/v21.o \
	$(BUILD_DIR)/sippymodem_fsk.o \
	$(BUILD_DIR)/sippymodem_g711.o
V8_FRAMING_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_v8_framing
V8_FRAMING_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_v8_framing.o \
	$(BUILD_DIR)/sippymodem_v8_framing.o
V8_MENU_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_v8_menu
V8_MENU_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_v8_menu.o \
	$(BUILD_DIR)/sippymodem_v8_menu.o \
	$(BUILD_DIR)/sippymodem_v8_framing.o
BAUDOT_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_baudot
BAUDOT_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_baudot.o \
	$(BUILD_DIR)/baudot.o $(BUILD_DIR)/sippymodem_fsk.o
V23_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_v23
V23_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_v23.o \
	$(NATIVE_CORE_OBJECTS) \
	$(BUILD_DIR)/sippymodem_dc_filter.o \
	$(BUILD_DIR)/sippymodem_fsk.o \
	$(BUILD_DIR)/v23_datapump.o
V22_FRAMING_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_v22_framing
V22_FRAMING_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v22_framing.o \
	$(BUILD_DIR)/sippymodem_differential_qpsk.o \
	$(BUILD_DIR)/sippymodem_v22_framing.o \
	$(BUILD_DIR)/sippymodem_v22bis_symbols.o
V22BIS_RECEIVER_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v22bis_receiver
V22BIS_RECEIVER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v22bis_receiver.o \
	$(BUILD_DIR)/sippymodem_differential_qpsk.o \
	$(BUILD_DIR)/sippymodem_qam_gain.o \
	$(BUILD_DIR)/sippymodem_v22bis_receiver.o \
	$(BUILD_DIR)/sippymodem_v22bis_symbols.o \
	$(BUILD_DIR)/sippymodem_v22_framing.o
V32_FRAMING_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_v32_framing
V32_FRAMING_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v32_framing.o \
	$(BUILD_DIR)/sippymodem_differential_qpsk.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/sippymodem_v32_framing.o
V32_CONDITIONING_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v32_conditioning
V32_CONDITIONING_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v32_conditioning.o \
	$(BUILD_DIR)/sippymodem_differential_qpsk.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/sippymodem_v32_conditioning.o \
	$(BUILD_DIR)/sippymodem_v32_framing.o
V32_CONDITIONING_RECEIVER_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v32_conditioning_receiver
V32_CONDITIONING_RECEIVER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v32_conditioning_receiver.o \
	$(BUILD_DIR)/sippymodem_qam.o \
	$(BUILD_DIR)/sippymodem_qam_gain.o \
	$(BUILD_DIR)/sippymodem_differential_qpsk.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/sippymodem_v32_conditioning.o \
	$(BUILD_DIR)/sippymodem_v32_conditioning_receiver.o \
	$(BUILD_DIR)/sippymodem_v32_constellation.o \
	$(BUILD_DIR)/sippymodem_v32_framing.o
V32_CONSTELLATION_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v32_constellation
V32_CONSTELLATION_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v32_constellation.o \
	$(BUILD_DIR)/sippymodem_v32_constellation.o
V32_9600_RECEIVER_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v32_9600_receiver
V32_9600_RECEIVER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v32_9600_receiver.o \
	$(BUILD_DIR)/sippymodem_differential_qpsk.o \
	$(BUILD_DIR)/qam_equalizer.o \
	$(BUILD_DIR)/sippymodem_v32_9600_receiver.o \
	$(BUILD_DIR)/sippymodem_v32_constellation.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/sippymodem_v32_framing.o
V32_RATE_SIGNAL_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v32_rate_signal
V32_RATE_SIGNAL_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v32_rate_signal.o \
	$(BUILD_DIR)/sippymodem_v32_rate_signal.o
V32_STARTUP_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_v32_startup
V32_STARTUP_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v32_startup.o \
	$(BUILD_DIR)/sippymodem_v32_rate_signal.o \
	$(BUILD_DIR)/sippymodem_v32_startup.o
V32_LINE_STARTUP_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v32_line_startup
V32_LINE_STARTUP_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v32_line_startup.o \
	$(BUILD_DIR)/sippymodem_v32_line_startup.o \
	$(BUILD_DIR)/sippymodem_single_tone.o \
	$(BUILD_DIR)/sippymodem_g711.o
V32_DATAPUMP_CONDITIONING_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v32_datapump_conditioning
V32_DATAPUMP_CONDITIONING_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v32_datapump_conditioning.o \
	$(NATIVE_CORE_OBJECTS) \
	$(BUILD_DIR)/qam_equalizer.o \
	$(BUILD_DIR)/sippymodem_dc_filter.o \
	$(BUILD_DIR)/sippymodem_differential_qpsk.o \
	$(BUILD_DIR)/sippymodem_qam.o \
	$(BUILD_DIR)/sippymodem_qam_gain.o \
	$(BUILD_DIR)/sippymodem_single_tone.o \
	$(BUILD_DIR)/sippymodem_v32_datapump.o \
	$(BUILD_DIR)/sippymodem_v32_9600_receiver.o \
	$(BUILD_DIR)/sippymodem_v32_conditioning.o \
	$(BUILD_DIR)/sippymodem_v32_conditioning_receiver.o \
	$(BUILD_DIR)/sippymodem_v32_constellation.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/sippymodem_v32_framing.o \
	$(BUILD_DIR)/sippymodem_v32_line_startup.o \
	$(BUILD_DIR)/sippymodem_v32_rate_signal.o \
	$(BUILD_DIR)/sippymodem_v32_startup.o \
	$(BUILD_DIR)/sippymodem_v32_trellis.o \
	$(BUILD_DIR)/sippymodem_v32_trellis_decoder.o
V32_TRELLIS_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_v32_trellis
V32_TRELLIS_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v32_trellis.o \
	$(BUILD_DIR)/sippymodem_v32_trellis.o
V32_TRELLIS_DECODER_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v32_trellis_decoder
V32_TRELLIS_DECODER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v32_trellis_decoder.o \
	$(BUILD_DIR)/sippymodem_v32_constellation.o \
	$(BUILD_DIR)/sippymodem_v32_trellis.o \
	$(BUILD_DIR)/sippymodem_v32_trellis_decoder.o
V32_CARRIER_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_v32_carrier
V32_CARRIER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v32_carrier.o \
	$(BUILD_DIR)/sippymodem_differential_qpsk.o \
	$(BUILD_DIR)/sippymodem_g711.o \
	$(BUILD_DIR)/sippymodem_qam.o \
	$(BUILD_DIR)/sippymodem_v32_constellation.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/sippymodem_v32_framing.o
V34_FRAMING_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_v34_framing
V34_FRAMING_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_framing.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/sippymodem_v34_framing.o
V34_FRAME_SCHEDULE_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_frame_schedule
V34_FRAME_SCHEDULE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_frame_schedule.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/sippymodem_v34_framing.o \
	$(BUILD_DIR)/sippymodem_v34_frame_schedule.o
V34_MAPPING_PARAMETERS_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_mapping_parameters
V34_MAPPING_PARAMETERS_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_mapping_parameters.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/sippymodem_v34_framing.o \
	$(BUILD_DIR)/sippymodem_v34_frame_schedule.o \
	$(BUILD_DIR)/sippymodem_v34_mapping_parameters.o
V34_PARSER_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_v34_parser
V34_PARSER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_parser.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/sippymodem_v34_framing.o \
	$(BUILD_DIR)/sippymodem_v34_frame_schedule.o \
	$(BUILD_DIR)/sippymodem_v34_mapping_parameters.o \
	$(BUILD_DIR)/sippymodem_v34_parser.o
V34_SHELL_MAPPER_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_shell_mapper
V34_SHELL_MAPPER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_shell_mapper.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/sippymodem_v34_framing.o \
	$(BUILD_DIR)/sippymodem_v34_frame_schedule.o \
	$(BUILD_DIR)/sippymodem_v34_mapping_parameters.o \
	$(BUILD_DIR)/sippymodem_v34_shell_mapper.o
V34_MAPPER_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_v34_mapper
V34_MAPPER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_mapper.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/sippymodem_v34_framing.o \
	$(BUILD_DIR)/sippymodem_v34_frame_schedule.o \
	$(BUILD_DIR)/sippymodem_v34_mapping_parameters.o \
	$(BUILD_DIR)/sippymodem_v34_constellation.o \
	$(BUILD_DIR)/sippymodem_v34_mapper.o
V34_TRELLIS_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_v34_trellis
V34_TRELLIS_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_trellis.o \
	$(BUILD_DIR)/sippymodem_v34_trellis.o
V34_PRECODER_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_v34_precoder
V34_PRECODER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_precoder.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/sippymodem_v34_framing.o \
	$(BUILD_DIR)/sippymodem_v34_frame_schedule.o \
	$(BUILD_DIR)/sippymodem_v34_mapping_parameters.o \
	$(BUILD_DIR)/sippymodem_v34_precoder.o
V34_4D_ENCODER_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_4d_encoder
V34_4D_ENCODER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_4d_encoder.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/sippymodem_v34_framing.o \
	$(BUILD_DIR)/sippymodem_v34_frame_schedule.o \
	$(BUILD_DIR)/sippymodem_v34_mapping_parameters.o \
	$(BUILD_DIR)/sippymodem_v34_constellation.o \
	$(BUILD_DIR)/sippymodem_v34_mapper.o \
	$(BUILD_DIR)/sippymodem_v34_precoder.o \
	$(BUILD_DIR)/sippymodem_v34_trellis.o \
	$(BUILD_DIR)/sippymodem_v34_4d_encoder.o
V34_4D_DECODER_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_4d_decoder
V34_4D_DECODER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_4d_decoder.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/sippymodem_v34_framing.o \
	$(BUILD_DIR)/sippymodem_v34_frame_schedule.o \
	$(BUILD_DIR)/sippymodem_v34_mapping_parameters.o \
	$(BUILD_DIR)/sippymodem_v34_constellation.o \
	$(BUILD_DIR)/sippymodem_v34_mapper.o \
	$(BUILD_DIR)/sippymodem_v34_4d_decoder.o
V34_MAPPING_FRAME_ENCODER_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_mapping_frame_encoder
V34_MAPPING_FRAME_ENCODER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_mapping_frame_encoder.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/sippymodem_v34_framing.o \
	$(BUILD_DIR)/sippymodem_v34_frame_schedule.o \
	$(BUILD_DIR)/sippymodem_v34_mapping_parameters.o \
	$(BUILD_DIR)/sippymodem_v34_parser.o \
	$(BUILD_DIR)/sippymodem_v34_shell_mapper.o \
	$(BUILD_DIR)/sippymodem_v34_constellation.o \
	$(BUILD_DIR)/sippymodem_v34_mapper.o \
	$(BUILD_DIR)/sippymodem_v34_precoder.o \
	$(BUILD_DIR)/sippymodem_v34_trellis.o \
	$(BUILD_DIR)/sippymodem_v34_4d_encoder.o \
	$(BUILD_DIR)/sippymodem_v34_mapping_frame_encoder.o
V34_NONLINEAR_ENCODER_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_nonlinear_encoder
V34_NONLINEAR_ENCODER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_nonlinear_encoder.o \
	$(BUILD_DIR)/sippymodem_v34_nonlinear_encoder.o
V34_SYMBOL_POWER_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_symbol_power
V34_SYMBOL_POWER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_symbol_power.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/sippymodem_v34_framing.o \
	$(BUILD_DIR)/sippymodem_v34_frame_schedule.o \
	$(BUILD_DIR)/sippymodem_v34_mapping_parameters.o \
	$(BUILD_DIR)/sippymodem_v34_constellation.o \
	$(BUILD_DIR)/sippymodem_v34_nonlinear_encoder.o \
	$(BUILD_DIR)/sippymodem_v34_symbol_power.o
V34_MAPPING_FRAME_TRANSMITTER_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_mapping_frame_transmitter
V34_MAPPING_FRAME_TRANSMITTER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_mapping_frame_transmitter.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/sippymodem_v34_framing.o \
	$(BUILD_DIR)/sippymodem_v34_frame_schedule.o \
	$(BUILD_DIR)/sippymodem_v34_mapping_parameters.o \
	$(BUILD_DIR)/sippymodem_v34_parser.o \
	$(BUILD_DIR)/sippymodem_v34_shell_mapper.o \
	$(BUILD_DIR)/sippymodem_v34_constellation.o \
	$(BUILD_DIR)/sippymodem_v34_mapper.o \
	$(BUILD_DIR)/sippymodem_v34_precoder.o \
	$(BUILD_DIR)/sippymodem_v34_trellis.o \
	$(BUILD_DIR)/sippymodem_v34_4d_encoder.o \
	$(BUILD_DIR)/sippymodem_v34_mapping_frame_encoder.o \
	$(BUILD_DIR)/sippymodem_v34_nonlinear_encoder.o \
	$(BUILD_DIR)/sippymodem_v34_mapping_frame_transmitter.o
V34_POWER_CALIBRATION_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_power_calibration
V34_POWER_CALIBRATION_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_power_calibration.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/sippymodem_v34_framing.o \
	$(BUILD_DIR)/sippymodem_v34_frame_schedule.o \
	$(BUILD_DIR)/sippymodem_v34_mapping_parameters.o \
	$(BUILD_DIR)/sippymodem_v34_parser.o \
	$(BUILD_DIR)/sippymodem_v34_shell_mapper.o \
	$(BUILD_DIR)/sippymodem_v34_constellation.o \
	$(BUILD_DIR)/sippymodem_v34_mapper.o \
	$(BUILD_DIR)/sippymodem_v34_precoder.o \
	$(BUILD_DIR)/sippymodem_v34_trellis.o \
	$(BUILD_DIR)/sippymodem_v34_4d_encoder.o \
	$(BUILD_DIR)/sippymodem_v34_mapping_frame_encoder.o \
	$(BUILD_DIR)/sippymodem_v34_nonlinear_encoder.o \
	$(BUILD_DIR)/sippymodem_v34_symbol_power.o \
	$(BUILD_DIR)/sippymodem_v34_mapping_frame_transmitter.o \
	$(BUILD_DIR)/sippymodem_v34_power_calibration.o \
	$(BUILD_DIR)/sippymodem_qam.o \
	$(BUILD_DIR)/sippymodem_v34_info1.o \
	$(BUILD_DIR)/sippymodem_v34_preemphasis.o \
	$(BUILD_DIR)/sippymodem_v34_phase3_profile.o \
	$(BUILD_DIR)/sippymodem_v34_data_carrier.o
V34_DATA_CARRIER_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_data_carrier
V34_DATA_CARRIER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_data_carrier.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/sippymodem_qam.o \
	$(BUILD_DIR)/sippymodem_v34_framing.o \
	$(BUILD_DIR)/sippymodem_v34_frame_schedule.o \
	$(BUILD_DIR)/sippymodem_v34_mapping_parameters.o \
	$(BUILD_DIR)/sippymodem_v34_parser.o \
	$(BUILD_DIR)/sippymodem_v34_shell_mapper.o \
	$(BUILD_DIR)/sippymodem_v34_constellation.o \
	$(BUILD_DIR)/sippymodem_v34_mapper.o \
	$(BUILD_DIR)/sippymodem_v34_precoder.o \
	$(BUILD_DIR)/sippymodem_v34_trellis.o \
	$(BUILD_DIR)/sippymodem_v34_4d_encoder.o \
	$(BUILD_DIR)/sippymodem_v34_mapping_frame_encoder.o \
	$(BUILD_DIR)/sippymodem_v34_nonlinear_encoder.o \
	$(BUILD_DIR)/sippymodem_v34_symbol_power.o \
	$(BUILD_DIR)/sippymodem_v34_mapping_frame_transmitter.o \
	$(BUILD_DIR)/sippymodem_v34_power_calibration.o \
	$(BUILD_DIR)/sippymodem_v34_info1.o \
	$(BUILD_DIR)/sippymodem_v34_preemphasis.o \
	$(BUILD_DIR)/sippymodem_v34_phase3_profile.o \
	$(BUILD_DIR)/sippymodem_v34_data_carrier.o \
	$(BUILD_DIR)/sippymodem_v34_data_line_transmitter.o
V34_DATA_TRANSMITTER_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_data_transmitter
V34_DATA_TRANSMITTER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_data_transmitter.o \
	$(filter-out $(BUILD_DIR)/test_sippymodem_v34_data_carrier.o, \
		$(V34_DATA_CARRIER_TEST_OBJECTS)) \
	$(BUILD_DIR)/sippymodem_v34_data_transmitter.o
V34_DATA_STREAM_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_data_stream
V34_DATA_STREAM_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_data_stream.o \
	$(filter-out $(BUILD_DIR)/test_sippymodem_v34_data_transmitter.o, \
		$(V34_DATA_TRANSMITTER_TEST_OBJECTS)) \
	$(BUILD_DIR)/sippymodem_v34_data_stream.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/sippymodem_v34_mp.o \
	$(BUILD_DIR)/sippymodem_v34_phase4_trn.o \
	$(BUILD_DIR)/sippymodem_v34_phase4_mp.o \
	$(BUILD_DIR)/sippymodem_v34_phase4_e_sequence.o \
	$(BUILD_DIR)/sippymodem_v34_phase4_b1_handoff.o
V34_CONSTELLATION_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_constellation
V34_CONSTELLATION_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_constellation.o \
	$(BUILD_DIR)/sippymodem_v34_constellation.o
V34_INFO1_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_v34_info1
V34_INFO1_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_info1.o \
	$(BUILD_DIR)/sippymodem_v34_info1.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/sippymodem_v34_phase2_info.o
V34_PREEMPHASIS_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_preemphasis
V34_PREEMPHASIS_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_preemphasis.o \
	$(BUILD_DIR)/sippymodem_v34_preemphasis.o
V34_PHASE3_PROFILE_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_phase3_profile
V34_PHASE3_PROFILE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_phase3_profile.o \
	$(BUILD_DIR)/sippymodem_v34_info1.o \
	$(BUILD_DIR)/sippymodem_v34_phase3_profile.o
V34_PHASE3_PP_SEQUENCE_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_phase3_pp_sequence
V34_PHASE3_PP_SEQUENCE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_phase3_pp_sequence.o \
	$(BUILD_DIR)/sippymodem_v34_phase3_pp_sequence.o
V34_PHASE3_S_SEQUENCE_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_phase3_s_sequence
V34_PHASE3_S_SEQUENCE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_phase3_s_sequence.o \
	$(BUILD_DIR)/sippymodem_qam.o \
	$(BUILD_DIR)/sippymodem_v34_constellation.o \
	$(BUILD_DIR)/sippymodem_v34_phase3_s_sequence.o
V34_PHASE3_S_TRANSMITTER_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_phase3_s_transmitter
V34_PHASE3_S_TRANSMITTER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_phase3_s_transmitter.o \
	$(BUILD_DIR)/sippymodem_qam.o \
	$(BUILD_DIR)/sippymodem_v34_constellation.o \
	$(BUILD_DIR)/sippymodem_v34_info1.o \
	$(BUILD_DIR)/sippymodem_v34_preemphasis.o \
	$(BUILD_DIR)/sippymodem_v34_phase3_profile.o \
	$(BUILD_DIR)/sippymodem_v34_phase3_s_sequence.o \
	$(BUILD_DIR)/sippymodem_v34_phase3_s_transmitter.o
V34_PHASE3_TRN_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_phase3_trn
V34_PHASE3_TRN_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_phase3_trn.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/sippymodem_v34_constellation.o \
	$(BUILD_DIR)/sippymodem_v34_framing.o \
	$(BUILD_DIR)/sippymodem_v34_phase3_trn.o
V34_PHASE3_J_SEQUENCE_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_phase3_j_sequence
V34_PHASE3_J_SEQUENCE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_phase3_j_sequence.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/sippymodem_v34_constellation.o \
	$(BUILD_DIR)/sippymodem_v34_framing.o \
	$(BUILD_DIR)/sippymodem_v34_phase3_trn.o \
	$(BUILD_DIR)/sippymodem_v34_phase3_j_sequence.o
V34_PHASE4_J_PRIME_SEQUENCE_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_phase4_j_prime_sequence
V34_PHASE4_J_PRIME_SEQUENCE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_phase4_j_prime_sequence.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/sippymodem_v34_constellation.o \
	$(BUILD_DIR)/sippymodem_v34_framing.o \
	$(BUILD_DIR)/sippymodem_v34_phase3_trn.o \
	$(BUILD_DIR)/sippymodem_v34_phase3_j_sequence.o \
	$(BUILD_DIR)/sippymodem_v34_phase4_j_prime_sequence.o
V34_PHASE4_TRN_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_phase4_trn
V34_PHASE4_TRN_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_phase4_trn.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/sippymodem_v34_constellation.o \
	$(BUILD_DIR)/sippymodem_v34_framing.o \
	$(BUILD_DIR)/sippymodem_v34_phase4_trn.o
V34_PHASE4_MP_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_phase4_mp
V34_PHASE4_MP_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_phase4_mp.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/sippymodem_v34_constellation.o \
	$(BUILD_DIR)/sippymodem_v34_framing.o \
	$(BUILD_DIR)/sippymodem_v34_mp.o \
	$(BUILD_DIR)/sippymodem_v34_phase4_trn.o \
	$(BUILD_DIR)/sippymodem_v34_phase4_mp.o \
	$(BUILD_DIR)/sippymodem_v34_phase4_mp_receiver.o
V34_PHASE4_MP_CHANNEL_RECEIVER_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_phase4_mp_channel_receiver
V34_PHASE4_MP_CHANNEL_RECEIVER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_phase4_mp_channel_receiver.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/sippymodem_qam_gain.o \
	$(BUILD_DIR)/sippymodem_v34_constellation.o \
	$(BUILD_DIR)/sippymodem_v34_framing.o \
	$(BUILD_DIR)/sippymodem_v34_mp.o \
	$(BUILD_DIR)/sippymodem_v34_phase4_trn.o \
	$(BUILD_DIR)/sippymodem_v34_phase4_mp.o \
	$(BUILD_DIR)/sippymodem_v34_phase4_mp_receiver.o \
	$(BUILD_DIR)/sippymodem_v34_training_channel.o \
	$(BUILD_DIR)/sippymodem_v34_phase4_mp_channel_receiver.o \
	$(BUILD_DIR)/sippymodem_v34_phase4_e_sequence.o \
	$(BUILD_DIR)/sippymodem_v34_phase4_e_receiver.o \
	$(BUILD_DIR)/sippymodem_v34_phase4_e_channel_receiver.o
V34_PHASE4_E_SEQUENCE_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_phase4_e_sequence
V34_PHASE4_E_SEQUENCE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_phase4_e_sequence.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/sippymodem_v34_constellation.o \
	$(BUILD_DIR)/sippymodem_v34_framing.o \
	$(BUILD_DIR)/sippymodem_v34_mp.o \
	$(BUILD_DIR)/sippymodem_v34_phase4_trn.o \
	$(BUILD_DIR)/sippymodem_v34_phase4_mp.o \
	$(BUILD_DIR)/sippymodem_v34_phase4_e_sequence.o
V34_PHASE4_E_RECEIVER_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_phase4_e_receiver
V34_PHASE4_E_RECEIVER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_phase4_e_receiver.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/sippymodem_v34_constellation.o \
	$(BUILD_DIR)/sippymodem_v34_framing.o \
	$(BUILD_DIR)/sippymodem_v34_mp.o \
	$(BUILD_DIR)/sippymodem_v34_phase4_trn.o \
	$(BUILD_DIR)/sippymodem_v34_phase4_mp.o \
	$(BUILD_DIR)/sippymodem_v34_phase4_mp_receiver.o \
	$(BUILD_DIR)/sippymodem_v34_phase4_e_sequence.o \
	$(BUILD_DIR)/sippymodem_v34_phase4_e_receiver.o
V34_MP_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_v34_mp
V34_MP_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_mp.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/sippymodem_v34_mp.o
V34_PHASE2_INFO_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_phase2_info
V34_PHASE2_INFO_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_phase2_info.o \
	$(BUILD_DIR)/sippymodem_v34_info0.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/sippymodem_v34_phase2_info.o
V34_PHASE2_DPSK_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_phase2_dpsk
V34_PHASE2_DPSK_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_phase2_dpsk.o \
	$(BUILD_DIR)/sippymodem_qam.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_dpsk.o
V34_PHASE2_INFO_AUDIO_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_phase2_info_audio
V34_PHASE2_INFO_AUDIO_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_phase2_info_audio.o \
	$(BUILD_DIR)/sippymodem_g711.o \
	$(BUILD_DIR)/sippymodem_qam.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_dpsk.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/sippymodem_v34_phase2_info.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_info_audio.o
V34_PHASE2_INFO0_EXCHANGE_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_phase2_info0_exchange
V34_PHASE2_INFO0_EXCHANGE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_phase2_info0_exchange.o \
	$(BUILD_DIR)/sippymodem_g711.o \
	$(BUILD_DIR)/sippymodem_qam.o \
	$(BUILD_DIR)/sippymodem_v34_info0.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_dpsk.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/sippymodem_v34_phase2_info.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_info_audio.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_info0_exchange.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_tone.o
V34_PHASE2_RANGING_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_phase2_ranging
V34_PHASE2_RANGING_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_phase2_ranging.o \
	$(BUILD_DIR)/sippymodem_g711.o \
	$(BUILD_DIR)/sippymodem_qam.o \
	$(BUILD_DIR)/sippymodem_v34_info0.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_dpsk.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/sippymodem_v34_phase2_info.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_info_audio.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_info0_exchange.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_ranging.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_tone.o
V34_PHASE2_PROBE_FLOW_OBJECTS := \
	$(BUILD_DIR)/sippymodem_v34_phase2_test_pair.o \
	$(BUILD_DIR)/sippymodem_g711.o \
	$(BUILD_DIR)/sippymodem_qam.o \
	$(BUILD_DIR)/sippymodem_v34_info0.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_dpsk.o \
	$(CRC16_CCITT_FALSE_OBJECT) \
	$(BUILD_DIR)/sippymodem_v34_phase2_info.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_info_audio.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_info0_exchange.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_initial_probe.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_probe.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_probe_acquisition.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_probe_receiver.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_probe_spectrum.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_probe_synchronizer.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_probe_sequence.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_probe_turnaround.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_ranging.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_reciprocal_probe.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_tone.o
V34_PHASE2_INITIAL_PROBE_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_phase2_initial_probe
V34_PHASE2_INITIAL_PROBE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_phase2_initial_probe.o \
	$(V34_PHASE2_PROBE_FLOW_OBJECTS)
V34_PHASE2_RECIPROCAL_PROBE_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_phase2_reciprocal_probe
V34_PHASE2_RECIPROCAL_PROBE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_phase2_reciprocal_probe.o \
	$(V34_PHASE2_PROBE_FLOW_OBJECTS)
V34_PHASE2_INFO1_EXCHANGE_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_phase2_info1_exchange
V34_PHASE2_INFO1_EXCHANGE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_phase2_info1_exchange.o \
	$(BUILD_DIR)/sippymodem_v34_info1.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_info1_exchange.o \
	$(V34_PHASE2_PROBE_FLOW_OBJECTS)
V34_PHASE2_PROBE_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_phase2_probe
V34_PHASE2_PROBE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_phase2_probe.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_probe.o
V34_PHASE2_PROBE_ACQUISITION_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_phase2_probe_acquisition
V34_PHASE2_PROBE_ACQUISITION_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_phase2_probe_acquisition.o \
	$(BUILD_DIR)/sippymodem_g711.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_probe.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_probe_acquisition.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_probe_receiver.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_probe_spectrum.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_probe_synchronizer.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_tone.o
V34_PHASE2_PROBE_SPECTRUM_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_phase2_probe_spectrum
V34_PHASE2_PROBE_SPECTRUM_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_phase2_probe_spectrum.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_probe.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_probe_spectrum.o
V34_PHASE2_PROBE_RECEIVER_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_phase2_probe_receiver
V34_PHASE2_PROBE_RECEIVER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_phase2_probe_receiver.o \
	$(BUILD_DIR)/sippymodem_g711.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_probe.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_probe_receiver.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_probe_spectrum.o
V34_PHASE2_PROBE_SYNCHRONIZER_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_phase2_probe_synchronizer
V34_PHASE2_PROBE_SYNCHRONIZER_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_phase2_probe_synchronizer.o \
	$(BUILD_DIR)/sippymodem_g711.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_probe.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_probe_receiver.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_probe_spectrum.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_probe_synchronizer.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_tone.o
V34_PHASE2_PROBE_SEQUENCE_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_phase2_probe_sequence
V34_PHASE2_PROBE_SEQUENCE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_phase2_probe_sequence.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_probe.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_probe_sequence.o
V34_PHASE2_TONE_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_v34_phase2_tone
V34_PHASE2_TONE_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_v34_phase2_tone.o \
	$(BUILD_DIR)/sippymodem_g711.o \
	$(BUILD_DIR)/sippymodem_v34_phase2_tone.o
QAM_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_qam
QAM_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_qam.o \
	$(BUILD_DIR)/sippymodem_qam.o \
	$(BUILD_DIR)/sippymodem_qam_gain.o
QAM_EQUALIZER_TEST_BINARY := $(BUILD_DIR)/test_qam_equalizer
QAM_EQUALIZER_TEST_OBJECTS := $(BUILD_DIR)/test_qam_equalizer.o \
	$(BUILD_DIR)/qam_equalizer.o
FSK_INTEGRATION_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_fsk_integration
FSK_INTEGRATION_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_fsk_integration.o \
	$(NATIVE_CORE_OBJECTS) \
	$(BUILD_DIR)/qam_equalizer.o \
	$(BUILD_DIR)/sippymodem_datapumps.o \
	$(BUILD_DIR)/baudot.o \
	$(BUILD_DIR)/baudot_datapump.o \
	$(BUILD_DIR)/sippymodem_dc_filter.o \
	$(BUILD_DIR)/sippymodem_differential_qpsk.o \
	$(BUILD_DIR)/sippymodem_fsk.o \
	$(BUILD_DIR)/fsk300_datapump.o \
	$(BUILD_DIR)/v21.o \
	$(BUILD_DIR)/sippymodem_v8_ansam.o \
	$(BUILD_DIR)/sippymodem_v8_answering.o \
	$(BUILD_DIR)/sippymodem_v8_calling.o \
	$(BUILD_DIR)/sippymodem_v8_datapump.o \
	$(BUILD_DIR)/sippymodem_v8_fsk.o \
	$(BUILD_DIR)/sippymodem_v8_framing.o \
	$(BUILD_DIR)/sippymodem_v8_menu.o \
	$(BUILD_DIR)/sippymodem_qam.o \
	$(BUILD_DIR)/sippymodem_qam_gain.o \
	$(BUILD_DIR)/sippymodem_single_tone.o \
	$(BUILD_DIR)/sippymodem_v22_datapump.o \
	$(BUILD_DIR)/sippymodem_v22_framing.o \
	$(BUILD_DIR)/sippymodem_v22bis_receiver.o \
	$(BUILD_DIR)/sippymodem_v22bis_symbols.o \
	$(BUILD_DIR)/sippymodem_v32_datapump.o \
	$(BUILD_DIR)/sippymodem_v32_9600_receiver.o \
	$(BUILD_DIR)/sippymodem_v32_conditioning.o \
	$(BUILD_DIR)/sippymodem_v32_conditioning_receiver.o \
	$(BUILD_DIR)/sippymodem_v32_constellation.o \
	$(SELF_SYNC_SCRAMBLER_OBJECT) \
	$(BUILD_DIR)/sippymodem_v32_framing.o \
	$(BUILD_DIR)/sippymodem_v32_line_startup.o \
	$(BUILD_DIR)/sippymodem_v32_rate_signal.o \
	$(BUILD_DIR)/sippymodem_v32_startup.o \
	$(BUILD_DIR)/sippymodem_v32_trellis.o \
	$(BUILD_DIR)/sippymodem_v32_trellis_decoder.o \
	$(BUILD_DIR)/v23_datapump.o \
	$(BUILD_DIR)/sippymodem_media.o \
	$(BUILD_DIR)/sippymodem_g711.o
TEST_BINARIES := $(PACK_TEST_BINARY) $(CONFIG_TEST_BINARY) \
	$(SIP_URI_TEST_BINARY) $(SDP_TEST_BINARY) $(G711_TEST_BINARY) \
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
	$(V32_CARRIER_TEST_BINARY) \
	$(V34_FRAMING_TEST_BINARY) \
	$(V34_FRAME_SCHEDULE_TEST_BINARY) \
	$(V34_MAPPING_PARAMETERS_TEST_BINARY) \
	$(V34_PARSER_TEST_BINARY) \
	$(V34_SHELL_MAPPER_TEST_BINARY) \
	$(V34_MAPPER_TEST_BINARY) \
	$(V34_TRELLIS_TEST_BINARY) \
	$(V34_PRECODER_TEST_BINARY) \
	$(V34_4D_ENCODER_TEST_BINARY) \
	$(V34_4D_DECODER_TEST_BINARY) \
	$(V34_MAPPING_FRAME_ENCODER_TEST_BINARY) \
	$(V34_NONLINEAR_ENCODER_TEST_BINARY) \
	$(V34_SYMBOL_POWER_TEST_BINARY) \
	$(V34_MAPPING_FRAME_TRANSMITTER_TEST_BINARY) \
	$(V34_POWER_CALIBRATION_TEST_BINARY) \
	$(V34_DATA_CARRIER_TEST_BINARY) \
	$(V34_DATA_TRANSMITTER_TEST_BINARY) \
	$(V34_DATA_STREAM_TEST_BINARY) \
	$(V34_CONSTELLATION_TEST_BINARY) \
	$(V34_INFO1_TEST_BINARY) \
	$(V34_PREEMPHASIS_TEST_BINARY) \
	$(V34_PHASE3_PROFILE_TEST_BINARY) \
	$(V34_PHASE3_PP_SEQUENCE_TEST_BINARY) \
	$(V34_PHASE3_S_SEQUENCE_TEST_BINARY) \
	$(V34_PHASE3_S_TRANSMITTER_TEST_BINARY) \
	$(V34_PHASE3_TRN_TEST_BINARY) \
	$(V34_PHASE3_J_SEQUENCE_TEST_BINARY) \
	$(V34_PHASE4_J_PRIME_SEQUENCE_TEST_BINARY) \
	$(V34_PHASE4_TRN_TEST_BINARY) \
	$(V34_PHASE4_MP_TEST_BINARY) \
	$(V34_PHASE4_MP_CHANNEL_RECEIVER_TEST_BINARY) \
	$(V34_PHASE4_E_SEQUENCE_TEST_BINARY) \
	$(V34_PHASE4_E_RECEIVER_TEST_BINARY) \
	$(V34_MP_TEST_BINARY) \
	$(V34_PHASE2_INFO_TEST_BINARY) \
	$(V34_PHASE2_DPSK_TEST_BINARY) \
	$(V34_PHASE2_INFO_AUDIO_TEST_BINARY) \
	$(V34_PHASE2_INFO0_EXCHANGE_TEST_BINARY) \
	$(V34_PHASE2_PROBE_TEST_BINARY) \
	$(V34_PHASE2_PROBE_ACQUISITION_TEST_BINARY) \
	$(V34_PHASE2_PROBE_SPECTRUM_TEST_BINARY) \
	$(V34_PHASE2_PROBE_RECEIVER_TEST_BINARY) \
	$(V34_PHASE2_PROBE_SYNCHRONIZER_TEST_BINARY) \
	$(V34_PHASE2_PROBE_SEQUENCE_TEST_BINARY) \
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
	$(V32_CARRIER_TEST_OBJECTS:.o=.d) \
	$(V34_FRAMING_TEST_OBJECTS:.o=.d) \
	$(V34_FRAME_SCHEDULE_TEST_OBJECTS:.o=.d) \
	$(V34_MAPPING_PARAMETERS_TEST_OBJECTS:.o=.d) \
	$(V34_PARSER_TEST_OBJECTS:.o=.d) \
	$(V34_SHELL_MAPPER_TEST_OBJECTS:.o=.d) \
	$(V34_MAPPER_TEST_OBJECTS:.o=.d) \
	$(V34_TRELLIS_TEST_OBJECTS:.o=.d) \
	$(V34_PRECODER_TEST_OBJECTS:.o=.d) \
	$(V34_4D_ENCODER_TEST_OBJECTS:.o=.d) \
	$(V34_4D_DECODER_TEST_OBJECTS:.o=.d) \
	$(V34_MAPPING_FRAME_ENCODER_TEST_OBJECTS:.o=.d) \
	$(V34_NONLINEAR_ENCODER_TEST_OBJECTS:.o=.d) \
	$(V34_SYMBOL_POWER_TEST_OBJECTS:.o=.d) \
	$(V34_MAPPING_FRAME_TRANSMITTER_TEST_OBJECTS:.o=.d) \
	$(V34_POWER_CALIBRATION_TEST_OBJECTS:.o=.d) \
	$(V34_DATA_CARRIER_TEST_OBJECTS:.o=.d) \
	$(V34_CONSTELLATION_TEST_OBJECTS:.o=.d) \
	$(V34_INFO1_TEST_OBJECTS:.o=.d) \
	$(V34_PREEMPHASIS_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE3_PROFILE_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE3_PP_SEQUENCE_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE3_S_SEQUENCE_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE3_S_TRANSMITTER_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE3_TRN_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE3_J_SEQUENCE_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE4_J_PRIME_SEQUENCE_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE4_TRN_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE4_MP_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE4_MP_CHANNEL_RECEIVER_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE4_E_SEQUENCE_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE4_E_RECEIVER_TEST_OBJECTS:.o=.d) \
	$(V34_MP_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE2_INFO_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE2_DPSK_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE2_INFO_AUDIO_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE2_INFO0_EXCHANGE_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE2_PROBE_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE2_PROBE_ACQUISITION_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE2_PROBE_SPECTRUM_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE2_PROBE_RECEIVER_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE2_PROBE_SYNCHRONIZER_TEST_OBJECTS:.o=.d) \
	$(V34_PHASE2_PROBE_SEQUENCE_TEST_OBJECTS:.o=.d) \
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
	$(PYTHON) test_sippymodem_live_config.py
	$(PYTHON) test_sippymodem_live_terminal.py
	$(PYTHON) test_sippymodem_live_calls.py
	./$(PACK_TEST_BINARY)
	./$(CONFIG_TEST_BINARY)
	./$(SIP_URI_TEST_BINARY)
	./$(SDP_TEST_BINARY)
	./$(G711_TEST_BINARY)
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
	./$(V32_CARRIER_TEST_BINARY)
	./$(V34_FRAMING_TEST_BINARY)
	./$(V34_FRAME_SCHEDULE_TEST_BINARY)
	./$(V34_MAPPING_PARAMETERS_TEST_BINARY)
	./$(V34_PARSER_TEST_BINARY)
	./$(V34_SHELL_MAPPER_TEST_BINARY)
	./$(V34_MAPPER_TEST_BINARY)
	./$(V34_TRELLIS_TEST_BINARY)
	./$(V34_PRECODER_TEST_BINARY)
	./$(V34_4D_ENCODER_TEST_BINARY)
	./$(V34_4D_DECODER_TEST_BINARY)
	./$(V34_MAPPING_FRAME_ENCODER_TEST_BINARY)
	./$(V34_NONLINEAR_ENCODER_TEST_BINARY)
	./$(V34_SYMBOL_POWER_TEST_BINARY)
	./$(V34_MAPPING_FRAME_TRANSMITTER_TEST_BINARY)
	./$(V34_POWER_CALIBRATION_TEST_BINARY)
	./$(V34_DATA_CARRIER_TEST_BINARY)
	./$(V34_DATA_TRANSMITTER_TEST_BINARY)
	./$(V34_DATA_STREAM_TEST_BINARY)
	./$(V34_CONSTELLATION_TEST_BINARY)
	./$(V34_INFO1_TEST_BINARY)
	./$(V34_PREEMPHASIS_TEST_BINARY)
	./$(V34_PHASE3_PROFILE_TEST_BINARY)
	./$(V34_PHASE3_PP_SEQUENCE_TEST_BINARY)
	./$(V34_PHASE3_S_SEQUENCE_TEST_BINARY)
	./$(V34_PHASE3_S_TRANSMITTER_TEST_BINARY)
	./$(V34_PHASE3_TRN_TEST_BINARY)
	./$(V34_PHASE3_J_SEQUENCE_TEST_BINARY)
	./$(V34_PHASE4_J_PRIME_SEQUENCE_TEST_BINARY)
	./$(V34_PHASE4_TRN_TEST_BINARY)
	./$(V34_PHASE4_MP_TEST_BINARY)
	./$(V34_PHASE4_MP_CHANNEL_RECEIVER_TEST_BINARY)
	./$(V34_PHASE4_E_SEQUENCE_TEST_BINARY)
	./$(V34_PHASE4_E_RECEIVER_TEST_BINARY)
	./$(V34_MP_TEST_BINARY)
	./$(V34_PHASE2_INFO_TEST_BINARY)
	./$(V34_PHASE2_DPSK_TEST_BINARY)
	./$(V34_PHASE2_INFO_AUDIO_TEST_BINARY)
	./$(V34_PHASE2_INFO0_EXCHANGE_TEST_BINARY)
	./$(V34_PHASE2_PROBE_TEST_BINARY)
	./$(V34_PHASE2_PROBE_ACQUISITION_TEST_BINARY)
	./$(V34_PHASE2_PROBE_SPECTRUM_TEST_BINARY)
	./$(V34_PHASE2_PROBE_RECEIVER_TEST_BINARY)
	./$(V34_PHASE2_PROBE_SYNCHRONIZER_TEST_BINARY)
	./$(V34_PHASE2_PROBE_SEQUENCE_TEST_BINARY)
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

$(PJSIP_TEST_BINARY): $(PJSIP_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ $(PJSIP_LDLIBS)

$(G711_TEST_BINARY): $(G711_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

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

$(V34_PRECODER_TEST_BINARY): $(V34_PRECODER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V34_4D_ENCODER_TEST_BINARY): $(V34_4D_ENCODER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V34_4D_DECODER_TEST_BINARY): $(V34_4D_DECODER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V34_MAPPING_FRAME_ENCODER_TEST_BINARY): \
	$(V34_MAPPING_FRAME_ENCODER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V34_NONLINEAR_ENCODER_TEST_BINARY): \
	$(V34_NONLINEAR_ENCODER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_SYMBOL_POWER_TEST_BINARY): $(V34_SYMBOL_POWER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_MAPPING_FRAME_TRANSMITTER_TEST_BINARY): \
	$(V34_MAPPING_FRAME_TRANSMITTER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_POWER_CALIBRATION_TEST_BINARY): \
	$(V34_POWER_CALIBRATION_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_DATA_CARRIER_TEST_BINARY): $(V34_DATA_CARRIER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_DATA_TRANSMITTER_TEST_BINARY): $(V34_DATA_TRANSMITTER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_DATA_STREAM_TEST_BINARY): $(V34_DATA_STREAM_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_CONSTELLATION_TEST_BINARY): $(V34_CONSTELLATION_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_INFO1_TEST_BINARY): $(V34_INFO1_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V34_PREEMPHASIS_TEST_BINARY): $(V34_PREEMPHASIS_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE3_PROFILE_TEST_BINARY): $(V34_PHASE3_PROFILE_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V34_PHASE3_PP_SEQUENCE_TEST_BINARY): \
	$(V34_PHASE3_PP_SEQUENCE_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE3_S_SEQUENCE_TEST_BINARY): $(V34_PHASE3_S_SEQUENCE_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE3_S_TRANSMITTER_TEST_BINARY): \
	$(V34_PHASE3_S_TRANSMITTER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE3_TRN_TEST_BINARY): $(V34_PHASE3_TRN_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE3_J_SEQUENCE_TEST_BINARY): \
	$(V34_PHASE3_J_SEQUENCE_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE4_J_PRIME_SEQUENCE_TEST_BINARY): \
	$(V34_PHASE4_J_PRIME_SEQUENCE_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE4_TRN_TEST_BINARY): $(V34_PHASE4_TRN_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE4_MP_TEST_BINARY): $(V34_PHASE4_MP_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE4_MP_CHANNEL_RECEIVER_TEST_BINARY): \
	$(V34_PHASE4_MP_CHANNEL_RECEIVER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE4_E_SEQUENCE_TEST_BINARY): \
	$(V34_PHASE4_E_SEQUENCE_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_PHASE4_E_RECEIVER_TEST_BINARY): \
	$(V34_PHASE4_E_RECEIVER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V34_MP_TEST_BINARY): $(V34_MP_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(V34_PHASE2_INFO_TEST_BINARY): $(V34_PHASE2_INFO_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

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

$(BUILD_DIR)/sippymodem_config.o: ../src/sippymodem_config.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_sip_uri.o: \
	../src/sippymodem_sip_uri.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_sdp.o: ../src/sippymodem_sdp.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_sip_signaling_pjsip.o: \
	../src/sippymodem_sip_signaling_pjsip.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -D_DEFAULT_SOURCE $(CFLAGS) $(PJSIP_CFLAGS) \
		-MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_sdp_pjsip.o: \
	../src/sippymodem_sdp_pjsip.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -D_DEFAULT_SOURCE $(CFLAGS) $(PJSIP_CFLAGS) \
		-MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_g711.o: ../src/sippymodem_g711.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v42_xid.o: \
	../src/sippymodem_v42_xid.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_lapm_v44_session.o: \
	../src/sippymodem_lapm_v44_session.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v44_stream.o: \
	../src/sippymodem_v44_stream.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v44_xid.o: \
	../src/sippymodem_v44_xid.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v44_decoder.o: \
	../src/sippymodem_v44_decoder.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v44_character_path.o: \
	../src/sippymodem_v44_character_path.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v44_duplex.o: \
	../src/sippymodem_v44_duplex.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v44_encoder.o: \
	../src/sippymodem_v44_encoder.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v44_tx_monitor.o: \
	../src/sippymodem_v44_tx_monitor.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v44_dictionary_encoder.o: \
	../src/sippymodem_v44_dictionary_encoder.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v44_transparent_decoder.o: \
	../src/sippymodem_v44_transparent_decoder.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v44_dictionary_decoder.o: \
	../src/sippymodem_v44_dictionary_decoder.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v44_compressed_decoder.o: \
	../src/sippymodem_v44_compressed_decoder.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_rtp.o: ../src/sippymodem_rtp.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_media.o: ../src/sippymodem_media.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_rtp_receiver.o: \
	../src/sippymodem_rtp_receiver.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_rtp_stream.o: \
	../src/sippymodem_rtp_stream.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_rtp_endpoint.o: \
	../src/sippymodem_rtp_endpoint.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_sip_rtp.o: \
	../src/sippymodem_sip_rtp.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_rtp_runtime.o: \
	../src/sippymodem_rtp_runtime.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_sip_media_bridge.o: \
	../src/sippymodem_sip_media_bridge.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_session.o: \
	../src/sippymodem_session.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_audio_path.o: \
	../src/sippymodem_audio_path.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_frame_clock.o: \
	../src/sippymodem_frame_clock.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_audio_scheduler.o: \
	../src/sippymodem_audio_scheduler.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_host.o: \
	../src/sippymodem_host.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_recording_files.o: \
	../src/sippymodem_recording_files.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_slmodem_adapter.o: \
	../src/sippymodem_slmodem_adapter.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_slmodem_calls.o: \
	../src/sippymodem_slmodem_calls.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_slmodem_driver.o: \
	../src/sippymodem_slmodem_driver.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_slmodem_terminal.o: \
	../src/sippymodem_slmodem_terminal.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_runtime.o: ../src/sippymodem_runtime.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_terminal.o: \
	../src/sippymodem_terminal.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_loop.o: ../src/sippymodem_loop.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_main_testable.o: \
	../src/sippymodem_main.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -DSIPPYMODEM_NO_PROCESS_ENTRY $(CFLAGS) \
		-MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_dc_filter.o: \
	$(DSP_SOURCE_DIR)/sippymodem_dc_filter.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_single_tone.o: \
	$(DSP_SOURCE_DIR)/sippymodem_single_tone.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_differential_qpsk.o: \
	$(DSP_SOURCE_DIR)/sippymodem_differential_qpsk.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(SELF_SYNC_SCRAMBLER_OBJECT): \
	$(DSP_SOURCE_DIR)/sippymodem_self_sync_scrambler.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(CRC16_CCITT_FALSE_OBJECT): \
	$(DSP_SOURCE_DIR)/sippymodem_crc16_ccitt_false.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_datapumps.o: \
	$(DSP_SOURCE_DIR)/sippymodem_datapumps.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_fsk.o: \
	$(DSP_SOURCE_DIR)/sippymodem_fsk.c | $(BUILD_DIR)
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

$(BUILD_DIR)/sippymodem_v8_datapump.o: \
	$(DSP_SOURCE_DIR)/sippymodem_v8_datapump.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/v23_datapump.o: \
	$(DSP_V21_202_SOURCE_DIR)/v23_datapump.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v22_framing.o: \
	$(DSP_V22_SOURCE_DIR)/sippymodem_v22_framing.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v22bis_symbols.o: \
	$(DSP_V22_SOURCE_DIR)/sippymodem_v22bis_symbols.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v22bis_receiver.o: \
	$(DSP_V22_SOURCE_DIR)/sippymodem_v22bis_receiver.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v32_framing.o: \
	$(DSP_V32_SOURCE_DIR)/sippymodem_v32_framing.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v32_conditioning.o: \
	$(DSP_V32_SOURCE_DIR)/sippymodem_v32_conditioning.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v32_conditioning_receiver.o: \
	$(DSP_V32_SOURCE_DIR)/sippymodem_v32_conditioning_receiver.c | \
	$(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v32_constellation.o: \
	$(DSP_V32_SOURCE_DIR)/sippymodem_v32_constellation.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v32_9600_receiver.o: \
	$(DSP_V32_SOURCE_DIR)/sippymodem_v32_9600_receiver.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v32_rate_signal.o: \
	$(DSP_V32_SOURCE_DIR)/sippymodem_v32_rate_signal.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v32_startup.o: \
	$(DSP_V32_SOURCE_DIR)/sippymodem_v32_startup.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v32_line_startup.o: \
	$(DSP_V32_SOURCE_DIR)/sippymodem_v32_line_startup.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v32_trellis.o: \
	$(DSP_V32_SOURCE_DIR)/sippymodem_v32_trellis.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v32_trellis_decoder.o: \
	$(DSP_V32_SOURCE_DIR)/sippymodem_v32_trellis_decoder.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v32_datapump.o: \
	$(DSP_V32_SOURCE_DIR)/sippymodem_v32_datapump.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_framing.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_framing.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_frame_schedule.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_frame_schedule.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_mapping_parameters.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_mapping_parameters.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_parser.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_parser.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_shell_mapper.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_shell_mapper.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_mapper.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_mapper.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_trellis.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_trellis.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_precoder.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_precoder.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_4d_encoder.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_4d_encoder.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_4d_decoder.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_4d_decoder.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_mapping_frame_encoder.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_mapping_frame_encoder.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_nonlinear_encoder.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_nonlinear_encoder.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_symbol_power.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_symbol_power.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_mapping_frame_transmitter.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_mapping_frame_transmitter.c | \
	$(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_power_calibration.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_power_calibration.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_data_carrier.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_data_carrier.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_data_line_transmitter.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_data_line_transmitter.c | \
	$(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_data_transmitter.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_data_transmitter.c | \
	$(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_data_stream.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_data_stream.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_constellation.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_constellation.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_info0.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_info0.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_info1.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_info1.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_preemphasis.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_preemphasis.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_phase3_profile.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase3_profile.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_phase3_pp_sequence.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase3_pp_sequence.c | \
	$(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_phase3_s_sequence.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase3_s_sequence.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_phase3_s_transmitter.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase3_s_transmitter.c | \
	$(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_phase3_trn.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase3_trn.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_phase3_j_sequence.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase3_j_sequence.c | \
	$(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_phase4_j_prime_sequence.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase4_j_prime_sequence.c | \
	$(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_phase4_b1_handoff.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase4_b1_handoff.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_phase4_trn.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase4_trn.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_phase4_mp.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase4_mp.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_phase4_mp_receiver.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase4_mp_receiver.c | \
	$(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_phase4_mp_channel_receiver.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase4_mp_channel_receiver.c | \
	$(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_training_channel.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_training_channel.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_phase4_e_channel_receiver.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase4_e_channel_receiver.c | \
	$(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_phase4_e_sequence.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase4_e_sequence.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_phase4_e_receiver.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase4_e_receiver.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_mp.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_mp.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_phase2_info.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase2_info.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_phase2_dpsk.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase2_dpsk.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_phase2_info_audio.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase2_info_audio.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_phase2_info0_exchange.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase2_info0_exchange.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_phase2_info1_exchange.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase2_info1_exchange.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_phase2_initial_probe.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase2_initial_probe.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_phase2_probe.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase2_probe.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_phase2_probe_acquisition.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase2_probe_acquisition.c | \
	$(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_phase2_probe_receiver.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase2_probe_receiver.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_phase2_probe_spectrum.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase2_probe_spectrum.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_phase2_probe_synchronizer.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase2_probe_synchronizer.c | \
	$(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_phase2_probe_sequence.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase2_probe_sequence.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_phase2_probe_turnaround.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase2_probe_turnaround.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_phase2_reciprocal_probe.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase2_reciprocal_probe.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_phase2_ranging.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase2_ranging.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v34_phase2_tone.o: \
	$(DSP_PCM_SOURCE_DIR)/sippymodem_v34_phase2_tone.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v22_datapump.o: \
	$(DSP_V22_SOURCE_DIR)/sippymodem_v22_datapump.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_qam.o: \
	$(DSP_SOURCE_DIR)/sippymodem_qam.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_qam_gain.o: \
	$(DSP_SOURCE_DIR)/sippymodem_qam_gain.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/qam_equalizer.o: \
	$(DSP_SOURCE_DIR)/qam_equalizer.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v8_ansam.o: \
	$(DSP_SOURCE_DIR)/sippymodem_v8_ansam.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v8_answering.o: \
	$(DSP_SOURCE_DIR)/sippymodem_v8_answering.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v8_calling.o: \
	$(DSP_SOURCE_DIR)/sippymodem_v8_calling.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v8_fsk.o: \
	$(DSP_SOURCE_DIR)/sippymodem_v8_fsk.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v8_framing.o: \
	$(DSP_SOURCE_DIR)/sippymodem_v8_framing.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v8_menu.o: \
	$(DSP_SOURCE_DIR)/sippymodem_v8_menu.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/native_%.o: ../src/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(filter-out -Werror,$(CFLAGS)) \
		-MMD -MP -c -o $@ $<

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPENDENCIES)
