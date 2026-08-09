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
TEST_BINARIES := $(PACK_TEST_BINARY) $(CONFIG_TEST_BINARY) \
	$(SIP_URI_TEST_BINARY) $(SDP_TEST_BINARY) $(G711_TEST_BINARY) \
	$(RTP_TEST_BINARY) $(MEDIA_TEST_BINARY) $(RECEIVER_TEST_BINARY) \
	$(STREAM_TEST_BINARY) $(ENDPOINT_TEST_BINARY) $(SIP_RTP_TEST_BINARY) \
	$(RTP_RUNTIME_TEST_BINARY) $(SIP_MEDIA_BRIDGE_TEST_BINARY) \
	$(SESSION_TEST_BINARY) $(AUDIO_PATH_TEST_BINARY)
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
	$(SESSION_TEST_OBJECTS:.o=.d) $(AUDIO_PATH_TEST_OBJECTS:.o=.d)
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

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPENDENCIES)
