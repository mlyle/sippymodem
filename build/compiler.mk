CC ?= cc
CXX ?= c++
CCACHE ?= $(shell command -v ccache 2>/dev/null)

ifneq ($(strip $(CCACHE)),)
COMPILER_PROJECT_ROOT := \
	$(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
CCACHE_DIR ?= $(COMPILER_PROJECT_ROOT)/.build/ccache
CCACHE_BASEDIR ?= $(COMPILER_PROJECT_ROOT)
ifneq ($(notdir $(firstword $(CC))),$(notdir $(firstword $(CCACHE))))
override CC := $(CCACHE) $(CC)
endif
ifneq ($(notdir $(firstword $(CXX))),$(notdir $(firstword $(CCACHE))))
override CXX := $(CCACHE) $(CXX)
endif
export CCACHE_BASEDIR
export CCACHE_DIR
else
override CCACHE_BASEDIR :=
override CCACHE_DIR :=
export CCACHE_BASEDIR
export CCACHE_DIR
endif

export CC
export CXX
export CCACHE
