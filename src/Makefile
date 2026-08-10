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

CPPFLAGS += -D_POSIX_C_SOURCE=200809L -I. -Idsp
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
	sippymodem_terminal.c
SIPPYMODEM_DSP_SOURCES := \
	dsp/sippymodem_datapumps.c \
	dsp/sippymodem_baudot.c \
	dsp/sippymodem_baudot_datapump.c \
	dsp/sippymodem_dc_filter.c \
	dsp/sippymodem_differential_qpsk.c \
	dsp/sippymodem_fsk.c \
	dsp/sippymodem_fsk300_datapump.c \
	dsp/sippymodem_v21.c \
	dsp/sippymodem_qam.c \
	dsp/sippymodem_v22_datapump.c \
	dsp/sippymodem_v22_framing.c \
	dsp/sippymodem_v22bis_receiver.c \
	dsp/sippymodem_v22bis_symbols.c \
	dsp/sippymodem_v23_datapump.c \
	dsp/sippymodem_v32_datapump.c \
	dsp/sippymodem_v32_9600_receiver.c \
	dsp/sippymodem_v32_constellation.c \
	dsp/sippymodem_v32_framing.c \
	dsp/sippymodem_v32_rate_signal.c \
	dsp/sippymodem_v32_startup.c \
	dsp/sippymodem_v32_trellis.c \
	dsp/sippymodem_v32_trellis_decoder.c \
	dsp/sippymodem_v8_ansam.c \
	dsp/sippymodem_v8_framing.c \
	dsp/sippymodem_v8_menu.c
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
