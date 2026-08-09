CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -O2 -g
LDFLAGS ?=

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
TEST_BINARIES := $(PACK_TEST_BINARY) $(CONFIG_TEST_BINARY) $(G711_TEST_BINARY) \
	$(RTP_TEST_BINARY) $(MEDIA_TEST_BINARY) $(RECEIVER_TEST_BINARY) \
	$(STREAM_TEST_BINARY) $(ENDPOINT_TEST_BINARY)
DEPENDENCIES := $(PACK_TEST_OBJECTS:.o=.d) $(MODEM_OBJECT:.o=.d) \
	$(CONFIG_TEST_OBJECTS:.o=.d) $(G711_TEST_OBJECTS:.o=.d) \
	$(RTP_TEST_OBJECTS:.o=.d) $(MEDIA_TEST_OBJECTS:.o=.d) \
	$(RECEIVER_TEST_OBJECTS:.o=.d) $(STREAM_TEST_OBJECTS:.o=.d) \
	$(ENDPOINT_TEST_OBJECTS:.o=.d)

.PHONY: all clean test

all: $(TEST_BINARIES)

test: $(TEST_BINARIES)
	./$(PACK_TEST_BINARY)
	./$(CONFIG_TEST_BINARY)
	./$(G711_TEST_BINARY)
	./$(RTP_TEST_BINARY)
	./$(MEDIA_TEST_BINARY)
	./$(RECEIVER_TEST_BINARY)
	./$(STREAM_TEST_BINARY)
	./$(ENDPOINT_TEST_BINARY)

$(PACK_TEST_BINARY): $(PACK_TEST_OBJECTS) $(MODEM_OBJECT)
	$(CC) $(LDFLAGS) -o $@ $^

$(CONFIG_TEST_BINARY): $(CONFIG_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

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

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(MODEM_OBJECT): ../src/modem_pack.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/sippymodem_config.o: ../src/sippymodem_config.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

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

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPENDENCIES)
