.DEFAULT_GOAL := all

include build/compiler.mk
include build/platform.mk
include build/pjsip.mk

.PHONY: libsippymodem all clean install-hooks live-call slmodemd test build-modem help

# One recursive build even when several public targets are requested together.
# Installation builds the products supported by the target platform.
MODEM_BUILD_TARGETS := $(if $(filter install,$(MAKECMDGOALS)),all,\
	$(or $(filter all slmodemd libsippymodem,$(MAKECMDGOALS)),all))
all slmodemd libsippymodem: build-modem

build-modem: pjsip-required
	$(MAKE) -C src CC="$(CC)" CXX="$(CXX)" CCACHE="$(CCACHE)" \
		$(PJSIP_SUBMAKE_OPTIONS) $(MODEM_BUILD_TARGETS)

clean:
	$(MAKE) -C src CC="$(CC)" CXX="$(CXX)" CCACHE="$(CCACHE)" clean
	$(MAKE) -C tests CC="$(CC)" CXX="$(CXX)" CCACHE="$(CCACHE)" clean
	rm -f .build/sippymodem.pc

test: pjsip
	$(MAKE) -C tests CC="$(CC)" CXX="$(CXX)" CCACHE="$(CCACHE)" \
		$(PJSIP_SUBMAKE_OPTIONS) test

live-call:
	python3 tools/live_campaign.py

install-hooks:
	git config core.hooksPath .githooks

include build/install.mk

help:
	@printf '%s\n' \
		'make                 Build slmodemd and libsippymodem (static/shared)' \
		'make libsippymodem   Build only the native library' \
		'make slmodemd        Build only the PTY/AT frontend and its core' \
		'make test            Run the native and Python checks' \
		'make install         Install under PREFIX (default /usr/local)' \
		'make uninstall       Remove the files installed under PREFIX' \
		'make pjproject-init  Fetch the pinned PJSIP submodule (network access)' \
		'make install-hooks   Enable the checked-in pre-commit gate' \
		'PJSIP_PROVIDER=auto|submodule|system|none selects the SIP dependency.' \
		'DESTDIR stages installation; CC/CFLAGS/CPPFLAGS/LDFLAGS select the toolchain.'
