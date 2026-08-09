.PHONY: all clean install-hooks slmodemd modem_test test

all:
	$(MAKE) -C src all

slmodemd modem_test:
	$(MAKE) -C src $@

clean:
	$(MAKE) -C src clean
	$(MAKE) -C tests clean

test:
	$(MAKE) -C tests test

install-hooks:
	git config core.hooksPath .githooks
