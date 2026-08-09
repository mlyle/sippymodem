.PHONY: all clean slmodemd modem_test

all:
	$(MAKE) -C src all

slmodemd modem_test:
	$(MAKE) -C src $@

clean:
	$(MAKE) -C src clean
