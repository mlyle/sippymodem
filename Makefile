.PHONY: all clean install-hooks slmodemd test

all:
	$(MAKE) -C src all

slmodemd:
	$(MAKE) -C src $@

clean:
	$(MAKE) -C src clean
	$(MAKE) -C tests clean

test:
	$(MAKE) -C tests test

install-hooks:
	git config core.hooksPath .githooks
