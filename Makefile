include build/pjsip.mk

.PHONY: all clean install-hooks live-call slmodemd test

all: pjsip-required
	$(MAKE) -C src $(PJSIP_SUBMAKE_OPTIONS) all

slmodemd: pjsip-required
	$(MAKE) -C src $(PJSIP_SUBMAKE_OPTIONS) $@

clean:
	$(MAKE) -C src clean
	$(MAKE) -C tests clean

test: pjsip
	$(MAKE) -C tests $(PJSIP_SUBMAKE_OPTIONS) test

live-call:
	python3 tools/live_calls.py

install-hooks:
	git config core.hooksPath .githooks
