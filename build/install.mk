# SPDX-License-Identifier: BSD-3-Clause
PREFIX ?= /usr/local
prefix ?= $(PREFIX)
bindir ?= $(prefix)/bin
libdir ?= $(prefix)/lib
includedir ?= $(prefix)/include
datadir ?= $(prefix)/share
docdir ?= $(datadir)/doc/sippymodem
INSTALL ?= install
PYTHON ?= python3

PUBLIC_HEADERS := src/sippymodem.h src/sippymodem_fd.h
PKG_CONFIG_FILE := .build/sippymodem.pc

.PHONY: install uninstall

$(PKG_CONFIG_FILE): build/sippymodem.pc.in tools/pkg_config.py src/modem.c FORCE
	@mkdir -p $(dir $@)
	$(PYTHON) tools/pkg_config.py --prefix "$(prefix)" --libdir "$(libdir)" \
		--includedir "$(includedir)" --pjsip-package "$(PJSIP_PACKAGE)" > "$@"

.PHONY: FORCE
FORCE:

install: all $(PKG_CONFIG_FILE)
	$(INSTALL) -d "$(DESTDIR)$(bindir)" "$(DESTDIR)$(libdir)/pkgconfig" \
		"$(DESTDIR)$(includedir)" "$(DESTDIR)$(docdir)"
	$(INSTALL) -m 755 src/slmodemd "$(DESTDIR)$(bindir)/slmodemd"
	$(INSTALL) -m 755 src/libsippymodem.so "$(DESTDIR)$(libdir)/libsippymodem.so"
	$(INSTALL) -m 644 src/libsippymodem.a "$(DESTDIR)$(libdir)/libsippymodem.a"
	$(INSTALL) -m 644 $(PUBLIC_HEADERS) "$(DESTDIR)$(includedir)/"
	$(INSTALL) -m 644 $(PKG_CONFIG_FILE) "$(DESTDIR)$(libdir)/pkgconfig/sippymodem.pc"
	$(INSTALL) -m 644 src/COPYING "$(DESTDIR)$(docdir)/COPYING"

uninstall:
	rm -f "$(DESTDIR)$(bindir)/slmodemd" \
		"$(DESTDIR)$(libdir)/libsippymodem.so" "$(DESTDIR)$(libdir)/libsippymodem.a" \
		"$(DESTDIR)$(libdir)/pkgconfig/sippymodem.pc" \
		"$(DESTDIR)$(includedir)/sippymodem.h" "$(DESTDIR)$(includedir)/sippymodem_fd.h" \
		"$(DESTDIR)$(docdir)/COPYING"
