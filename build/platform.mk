# SPDX-License-Identifier: BSD-3-Clause
# Follow the compiler target when cross-compiling.
ifndef BUILD_PLATFORM_INCLUDED
BUILD_PLATFORM_INCLUDED := yes
TARGET_TRIPLE := $(shell $(CC) -dumpmachine 2>/dev/null)
TARGET_SYSTEM ?= $(if $(findstring mingw,$(TARGET_TRIPLE)),Windows,$(if $(findstring apple,$(TARGET_TRIPLE)),Darwin,$(if $(findstring linux,$(TARGET_TRIPLE)),Linux,$(shell uname -s))))

ifeq ($(TARGET_SYSTEM),Windows)
SHARED_LIBRARY := libsippymodem.dll
SHARED_IMPORT_LIBRARY := libsippymodem.dll.a
SHARED_EXPORTS := ../.build/generated/library.def
SHARED_LINK_FLAGS := -shared -Wl,--no-undefined $(SHARED_EXPORTS)
PLATFORM_LIBS := -lws2_32 -lbcrypt
DLLTOOL ?= $(TARGET_TRIPLE)-dlltool
ifeq ($(origin AR),default)
AR := $(TARGET_TRIPLE)-ar
endif
else ifeq ($(TARGET_SYSTEM),Darwin)
SHARED_LIBRARY := libsippymodem.dylib
SHARED_EXPORTS := ../build/library.exports
SHARED_LINK_FLAGS := -dynamiclib -Wl,-undefined,error \
	-Wl,-install_name,@rpath/$(SHARED_LIBRARY) \
	-Wl,-exported_symbols_list,$(SHARED_EXPORTS)
else
SHARED_LIBRARY := libsippymodem.so
SHARED_EXPORTS := ../build/library.map
SHARED_LINK_FLAGS := -shared -Wl,-z,defs -Wl,--version-script=$(SHARED_EXPORTS)
endif

endif
