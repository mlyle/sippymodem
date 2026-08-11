.PHONY: all clean install-hooks live-call slmodemd test

all:
	$(MAKE) -C src all

slmodemd:
	$(MAKE) -C src $@

clean:
	$(MAKE) -C src clean
	$(MAKE) -C tests clean

test:
	$(MAKE) -C tests test

live-call:
	python3 tools/live_calls.py

install-hooks:
	git config core.hooksPath .githooks
