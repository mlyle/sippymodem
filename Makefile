.DEFAULT_GOAL := all

include build/compiler.mk
include build/pjsip.mk

.PHONY: all clean install-hooks live-call slmodemd test

all: slmodemd

slmodemd: pjsip-required
	$(MAKE) -C src CC="$(CC)" CXX="$(CXX)" CCACHE="$(CCACHE)" \
		$(PJSIP_SUBMAKE_OPTIONS) $@

clean:
	$(MAKE) -C src CC="$(CC)" CXX="$(CXX)" CCACHE="$(CCACHE)" clean
	$(MAKE) -C tests CC="$(CC)" CXX="$(CXX)" CCACHE="$(CCACHE)" clean

test: pjsip
	$(MAKE) -C tests CC="$(CC)" CXX="$(CXX)" CCACHE="$(CCACHE)" \
		$(PJSIP_SUBMAKE_OPTIONS) test

live-call:
	python3 tools/live_campaign.py

install-hooks:
	git config core.hooksPath .githooks
