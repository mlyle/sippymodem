PJSIP_PROVIDER ?= auto
PJSIP_PACKAGE ?= libpjproject
PKG_CONFIG ?= pkg-config

PJSIP_SOURCE_DIR := third_party/pjproject
PJSIP_BUILD_DIR := .build/pjproject
PJSIP_PREFIX := $(abspath $(PJSIP_BUILD_DIR)/install)
PJSIP_CONFIGURE_STAMP := $(PJSIP_BUILD_DIR)/configured
PJSIP_PC_FILE := $(PJSIP_PREFIX)/lib/pkgconfig/libpjproject.pc
PJSIP_SOURCE_READY := $(if $(wildcard $(PJSIP_SOURCE_DIR)/aconfigure),yes,no)
PJSIP_SYSTEM_READY := $(shell \
	$(PKG_CONFIG) --exists $(PJSIP_PACKAGE) 2>/dev/null && echo yes)

PJSIP_PROVIDERS := auto submodule system none
ifeq ($(filter $(PJSIP_PROVIDER),$(PJSIP_PROVIDERS)),)
$(error PJSIP_PROVIDER must be one of: $(PJSIP_PROVIDERS))
endif

ifeq ($(PJSIP_PROVIDER),auto)
ifeq ($(PJSIP_SOURCE_READY),yes)
PJSIP_SELECTED_PROVIDER := submodule
else ifeq ($(PJSIP_SYSTEM_READY),yes)
PJSIP_SELECTED_PROVIDER := system
else
PJSIP_SELECTED_PROVIDER := none
endif
else
PJSIP_SELECTED_PROVIDER := $(PJSIP_PROVIDER)
endif

ifeq ($(PJSIP_SELECTED_PROVIDER),submodule)
export PKG_CONFIG_PATH := \
	$(PJSIP_PREFIX)/lib/pkgconfig$(if $(PKG_CONFIG_PATH),:$(PKG_CONFIG_PATH))
PJSIP_SUBMAKE_OPTIONS := PJSIP_DEPENDENCY=$(PJSIP_PC_FILE)
else ifneq ($(PJSIP_SELECTED_PROVIDER),system)
PJSIP_SUBMAKE_OPTIONS := PJSIP_AVAILABLE=no
endif

PJSIP_CONFIGURE_OPTIONS := \
	--disable-pjsua2 \
	--disable-upnp \
	--disable-resample \
	--disable-sound \
	--disable-video \
	--disable-speex-aec \
	--disable-l16-codec \
	--disable-gsm-codec \
	--disable-g722-codec \
	--disable-g7221-codec \
	--disable-speex-codec \
	--disable-ilbc-codec \
	--disable-sdl \
	--disable-ffmpeg \
	--disable-v4l2 \
	--disable-openh264 \
	--disable-vpx \
	--disable-ssl \
	--disable-opencore-amr \
	--disable-silk \
	--disable-opus \
	--disable-bcg729 \
	--disable-lyra \
	--disable-libsrtp \
	--disable-libyuv \
	--disable-libwebrtc \
	--disable-libuuid

.PHONY: pjsip pjsip-required pjproject-init clean-pjproject

ifeq ($(PJSIP_SELECTED_PROVIDER),submodule)
ifeq ($(PJSIP_SOURCE_READY),yes)
pjsip: $(PJSIP_PC_FILE)

pjsip-required: pjsip

$(PJSIP_CONFIGURE_STAMP): build/pjsip.mk \
	$(PJSIP_SOURCE_DIR)/aconfigure | $(PJSIP_BUILD_DIR)
	cd $(PJSIP_SOURCE_DIR) && ./configure \
		--prefix="$(PJSIP_PREFIX)" $(PJSIP_CONFIGURE_OPTIONS)
	touch "$@"

$(PJSIP_PC_FILE): $(PJSIP_CONFIGURE_STAMP)
	$(MAKE) -C $(PJSIP_SOURCE_DIR) dep
	$(MAKE) -C $(PJSIP_SOURCE_DIR) lib
	$(MAKE) -C $(PJSIP_SOURCE_DIR) install
	test -f "$@"
else
pjsip:
	@echo "error: the pjproject submodule is not initialized" >&2
	@echo "run 'make pjproject-init', or select PJSIP_PROVIDER=system" >&2
	@false

pjsip-required: pjsip
endif
else ifeq ($(PJSIP_SELECTED_PROVIDER),system)
ifeq ($(PJSIP_SYSTEM_READY),yes)
pjsip pjsip-required:
else
pjsip:

pjsip-required:
	@echo "error: $(PJSIP_PACKAGE) is unavailable through pkg-config" >&2
	@false
endif
else
pjsip:

pjsip-required:
	@echo "error: no PJSIP provider is available" >&2
	@echo "run 'make pjproject-init', or install $(PJSIP_PACKAGE)" >&2
	@false
endif

$(PJSIP_BUILD_DIR):
	mkdir -p $@

pjproject-init:
	git submodule update --init --depth 1 -- $(PJSIP_SOURCE_DIR)

clean-pjproject:
	@if test -f $(PJSIP_SOURCE_DIR)/build.mak; then \
		$(MAKE) -C $(PJSIP_SOURCE_DIR) distclean; \
	fi
	rm -rf $(PJSIP_BUILD_DIR)
