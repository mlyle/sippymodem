CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -O2 -g
LDFLAGS ?=

PJSIP_PACKAGE ?= libpjproject
PJSIP_AVAILABLE := $(shell \
	pkg-config --exists $(PJSIP_PACKAGE) 2>/dev/null && echo yes)
PJSIP_CFLAGS := $(shell pkg-config --cflags $(PJSIP_PACKAGE) 2>/dev/null)
PJSIP_LDLIBS := $(shell pkg-config --static --libs $(PJSIP_PACKAGE) 2>/dev/null)

CPPFLAGS += -D_POSIX_C_SOURCE=200809L -I../src
CFLAGS += -std=c11 -Wall -Wextra -Werror

BUILD_DIR := build
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
NATIVE_CORE_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_native_core
NATIVE_CORE_SOURCE_NAMES := homolog_data modem modem_at modem_comp \
	modem_debug modem_ec modem_pack modem_param modem_timer sysdep_common
NATIVE_CORE_OBJECTS := $(addprefix $(BUILD_DIR)/native_, \
	$(addsuffix .o, $(NATIVE_CORE_SOURCE_NAMES)))
NATIVE_CORE_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_native_core.o \
	$(NATIVE_CORE_OBJECTS) \
	$(BUILD_DIR)/sippymodem_datapumps.o \
	$(BUILD_DIR)/sippymodem_dc_filter.o \
	$(BUILD_DIR)/sippymodem_fsk.o \
	$(BUILD_DIR)/sippymodem_fsk300_datapump.o
FSK_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_fsk
FSK_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_fsk.o \
	$(BUILD_DIR)/sippymodem_fsk.o
V23_TEST_BINARY := $(BUILD_DIR)/test_sippymodem_v23
V23_TEST_OBJECTS := $(BUILD_DIR)/test_sippymodem_v23.o \
	$(NATIVE_CORE_OBJECTS) \
	$(BUILD_DIR)/sippymodem_dc_filter.o \
	$(BUILD_DIR)/sippymodem_fsk.o \
	$(BUILD_DIR)/sippymodem_v23_datapump.o
FSK_INTEGRATION_TEST_BINARY := \
	$(BUILD_DIR)/test_sippymodem_fsk_integration
FSK_INTEGRATION_TEST_OBJECTS := \
	$(BUILD_DIR)/test_sippymodem_fsk_integration.o \
	$(NATIVE_CORE_OBJECTS) \
	$(BUILD_DIR)/sippymodem_datapumps.o \
	$(BUILD_DIR)/sippymodem_dc_filter.o \
	$(BUILD_DIR)/sippymodem_fsk.o \
	$(BUILD_DIR)/sippymodem_fsk300_datapump.o \
	$(BUILD_DIR)/sippymodem_media.o \
	$(BUILD_DIR)/sippymodem_g711.o
TEST_BINARIES := $(PACK_TEST_BINARY) $(CONFIG_TEST_BINARY) \
	$(SIP_URI_TEST_BINARY) $(SDP_TEST_BINARY) $(G711_TEST_BINARY) \
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
	$(DC_FILTER_TEST_BINARY) $(NATIVE_CORE_TEST_BINARY) \
	$(FSK_TEST_BINARY) $(V23_TEST_BINARY) $(FSK_INTEGRATION_TEST_BINARY)
ifeq ($(PJSIP_AVAILABLE),yes)
TEST_BINARIES += $(PJSIP_TEST_BINARY)
endif
DEPENDENCIES := $(PACK_TEST_OBJECTS:.o=.d) $(MODEM_OBJECT:.o=.d) \
	$(CONFIG_TEST_OBJECTS:.o=.d) $(SIP_URI_TEST_OBJECTS:.o=.d) \
	$(SDP_TEST_OBJECTS:.o=.d) $(G711_TEST_OBJECTS:.o=.d) \
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
	$(DC_FILTER_TEST_OBJECTS:.o=.d) $(NATIVE_CORE_TEST_OBJECTS:.o=.d) \
	$(FSK_TEST_OBJECTS:.o=.d) $(V23_TEST_OBJECTS:.o=.d) \
	$(FSK_INTEGRATION_TEST_OBJECTS:.o=.d)
ifeq ($(PJSIP_AVAILABLE),yes)
DEPENDENCIES += $(PJSIP_TEST_OBJECTS:.o=.d)
endif

.PHONY: all clean test

all: $(TEST_BINARIES)

test: $(TEST_BINARIES)
	./$(PACK_TEST_BINARY)
	./$(CONFIG_TEST_BINARY)
	./$(SIP_URI_TEST_BINARY)
	./$(SDP_TEST_BINARY)
	./$(G711_TEST_BINARY)
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
	./$(NATIVE_CORE_TEST_BINARY)
	./$(FSK_TEST_BINARY)
	./$(V23_TEST_BINARY)
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

$(NATIVE_CORE_TEST_BINARY): $(NATIVE_CORE_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(FSK_TEST_BINARY): $(FSK_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(V23_TEST_BINARY): $(V23_TEST_OBJECTS)
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
	../src/sippymodem_dc_filter.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_datapumps.o: \
	../src/sippymodem_datapumps.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_fsk.o: ../src/sippymodem_fsk.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_fsk300_datapump.o: \
	../src/sippymodem_fsk300_datapump.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_v23_datapump.o: \
	../src/sippymodem_v23_datapump.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/native_%.o: ../src/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(filter-out -Werror,$(CFLAGS)) \
		-MMD -MP -c -o $@ $<

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPENDENCIES)
