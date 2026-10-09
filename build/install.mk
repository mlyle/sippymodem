# SPDX-License-Identifier: BSD-3-Clause
include $(dir $(lastword $(MAKEFILE_LIST)))platform.mk
PREFIX ?= /usr/local
prefix ?= $(PREFIX)
bindir ?= $(prefix)/bin
libdir ?= $(prefix)/lib
includedir ?= $(prefix)/include
datadir ?= $(prefix)/share
docdir ?= $(datadir)/doc/sippymodem
exampledir ?= $(datadir)/sippymodem/examples/systemd
INSTALL ?= install
PYTHON ?= python3

PUBLIC_HEADERS := src/sippymodem.h src/sippymodem_fd.h
ifeq ($(TARGET_SYSTEM),Windows)
sharedir = $(bindir)
else
sharedir = $(libdir)
endif

SERVICE_EXAMPLES := examples/systemd/sippymodem@.service \
	examples/systemd/default.env.example examples/systemd/README.md
PKG_CONFIG_FILE := .build/sippymodem.pc

.PHONY: install uninstall

$(PKG_CONFIG_FILE): build/sippymodem.pc.in tools/pkg_config.py src/modem.c FORCE
	@mkdir -p $(dir $@)
	$(PYTHON) tools/pkg_config.py --prefix "$(prefix)" --libdir "$(libdir)" \
		--includedir "$(includedir)" --pjsip-package "$(PJSIP_PACKAGE)" \
		--private-libs="-lm $(PLATFORM_LIBS)" > "$@"

.PHONY: FORCE
FORCE:

install: all $(PKG_CONFIG_FILE)
	$(INSTALL) -d "$(DESTDIR)$(bindir)" "$(DESTDIR)$(libdir)/pkgconfig" \
		"$(DESTDIR)$(includedir)" "$(DESTDIR)$(docdir)"
ifneq ($(TARGET_SYSTEM),Windows)
	$(INSTALL) -m 755 src/slmodemd "$(DESTDIR)$(bindir)/slmodemd"
endif
	$(INSTALL) -m 755 src/$(SHARED_LIBRARY) "$(DESTDIR)$(sharedir)/$(SHARED_LIBRARY)"
	$(INSTALL) -m 644 src/libsippymodem.a "$(DESTDIR)$(libdir)/libsippymodem.a"
ifeq ($(TARGET_SYSTEM),Windows)
	$(INSTALL) -m 644 src/$(SHARED_IMPORT_LIBRARY) "$(DESTDIR)$(libdir)/$(SHARED_IMPORT_LIBRARY)"
endif
	$(INSTALL) -m 644 $(PUBLIC_HEADERS) "$(DESTDIR)$(includedir)/"
	$(INSTALL) -m 644 $(PKG_CONFIG_FILE) "$(DESTDIR)$(libdir)/pkgconfig/sippymodem.pc"
	$(INSTALL) -m 644 src/COPYING "$(DESTDIR)$(docdir)/COPYING"
ifeq ($(TARGET_SYSTEM),Linux)
	$(INSTALL) -d "$(DESTDIR)$(exampledir)"
	$(INSTALL) -m 644 $(SERVICE_EXAMPLES) "$(DESTDIR)$(exampledir)/"
endif

uninstall:
	rm -f "$(DESTDIR)$(sharedir)/$(SHARED_LIBRARY)" "$(DESTDIR)$(libdir)/libsippymodem.a" \
		"$(DESTDIR)$(libdir)/pkgconfig/sippymodem.pc" \
		"$(DESTDIR)$(includedir)/sippymodem.h" "$(DESTDIR)$(includedir)/sippymodem_fd.h" \
		"$(DESTDIR)$(docdir)/COPYING"
ifeq ($(TARGET_SYSTEM),Windows)
	rm -f "$(DESTDIR)$(libdir)/$(SHARED_IMPORT_LIBRARY)"
else
	rm -f "$(DESTDIR)$(bindir)/slmodemd"
endif
ifeq ($(TARGET_SYSTEM),Linux)
	rm -f "$(DESTDIR)$(exampledir)/sippymodem@.service" \
		"$(DESTDIR)$(exampledir)/default.env.example" "$(DESTDIR)$(exampledir)/README.md"
endif
