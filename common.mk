# common.mk -- build settings shared by src/Makefile and tests/Makefile
#
# Both Makefiles include this file so that the compiler configuration, the
# Y2038 defines, the platform libraries and the static-analysis options have
# a single definition. Only what genuinely differs -- the targets and the
# per-file compile rules -- stays in the Makefiles themselves.

# Compiler configuration
CC ?= gcc

# Detect operating system
UNAME ?= $(shell uname -s)

CFLAGS ?=
LDFLAGS ?=
PROJECT_CFLAGS :=
PROJECT_LDFLAGS :=

# Force 64-bit time_t and file offsets to prevent Y2038 issue
PROJECT_CFLAGS += -D_TIME_BITS=64 -D_FILE_OFFSET_BITS=64

# Compiler flags
ifeq ($(filter -std=% -ansi,$(CFLAGS)),)
PROJECT_CFLAGS += -std=c89
endif

ifeq ($(filter -O%,$(CFLAGS)),)
PROJECT_CFLAGS += -O2
endif

PROJECT_CFLAGS += \
    -Wall \
    -Wextra \
    -pipe

# Set NOFLAGS to 1 for C++ compilers
NOFLAGS ?= $(if $(findstring ++,$(notdir $(CC))),1)

# If NOFLAGS is set to 1, set CFLAGS and LDFLAGS to empty
ifeq ($(NOFLAGS),1)
override CFLAGS :=
override LDFLAGS :=
PROJECT_CFLAGS :=
PROJECT_LDFLAGS :=
endif

# Platform-specific linker flags
PROJECT_LDFLAGS += \
    $(if $(findstring Linux,$(UNAME)),-lrt) \
    $(if $(findstring FreeBSD,$(UNAME)),-lkvm) \
    $(if $(findstring Darwin,$(UNAME)),-lproc)

# Recursive assignment on purpose: tests/Makefile appends -pthread to
# PROJECT_CFLAGS after including this file and must see it here too.
ALL_CFLAGS = $(PROJECT_CFLAGS) $(CFLAGS)
ALL_LDFLAGS = $(LDFLAGS) $(PROJECT_LDFLAGS)

# cppcheck options
CPPCHECK_OPTS := \
    --enable=all \
    --language=c \
    --inconclusive \
    --check-level=exhaustive \
    --library=gnu \
    --force \
    --std=c89 \
    --project=compile_commands.json \
    --suppress=missingIncludeSystem \
    --suppress=ConfigurationNotChecked \
    --suppress=unmatchedSuppression \
    --suppress=checkersReport \
    --inline-suppr

# clang-tidy options
CLANG_TIDY_OPTS :=
ifeq ($(UNAME),Darwin)
MACOS_SDK := $(shell xcrun --show-sdk-path 2>/dev/null || true)
ifneq ($(MACOS_SDK),)
CLANG_TIDY_OPTS += --extra-arg=-isysroot --extra-arg=$(MACOS_SDK)
endif
endif

# Check report file
CPPCHECK_REPORT := cppcheck-report.txt
CLANG_TIDY_REPORT := clang-tidy-report.txt

# Fail with a clear message when a tool the check target needs is missing.
# Usage: $(call require-tool,bear)
define require-tool
	@command -v $(1) >/dev/null 2>&1 || { \
	echo "$(1) is not installed. Please install it to check the code."; \
	exit 1; \
	}
endef
