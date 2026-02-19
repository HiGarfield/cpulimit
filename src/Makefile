# Compiler configuration
CC ?= gcc

# Target binary
TARGET := cpulimit

# Destination directory
DESTDIR := /usr/local/bin

# Detect operating system
UNAME ?= $(shell uname -s)

# Detect compiler type
CC_IS_GCC   := $(shell $(CC) -v 2>&1 | grep -q '^gcc version' && echo 1)
CC_IS_CLANG := $(shell $(CC) --version 2>&1 | grep -qi 'clang' && echo 1)

# Compiler major version number (works for both GCC and Clang)
CC_VERSION_MAJOR := $(shell $(CC) -dumpversion 2>/dev/null | cut -d. -f1)

# Test if CC accepts FLAG without error; returns FLAG if yes, empty string if no.
# Usage: $(call cc_supports_flag,FLAG)
cc_supports_flag = $(shell printf 'int x;\n' | \
    $(CC) -x c -std=c89 -Werror $(1) -c -o /dev/null - >/dev/null 2>&1 && echo $(1))

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
    -pedantic \
    -Wall \
    $(call cc_supports_flag,-Wextra) \
    $(call cc_supports_flag,-Wold-style-definition) \
    $(call cc_supports_flag,-Wmissing-prototypes) \
    $(call cc_supports_flag,-Wstrict-prototypes) \
    -Werror \
    $(call cc_supports_flag,-pipe)

# If CHECK is set to 1, add stricter flags
ifeq ($(CHECK),1)
PROJECT_CFLAGS += \
    $(call cc_supports_flag,-Wshadow) \
    $(call cc_supports_flag,-Wcast-qual) \
    $(call cc_supports_flag,-Wconversion) \
    $(call cc_supports_flag,-Wformat=2) \
    $(call cc_supports_flag,-Wstrict-overflow=5) \
    $(call cc_supports_flag,-Wundef) \
    $(call cc_supports_flag,-Wswitch-default) \
    $(call cc_supports_flag,-Wswitch-enum) \
    $(call cc_supports_flag,-Wunreachable-code) \
    $(call cc_supports_flag,-Wfloat-equal) \
    $(call cc_supports_flag,-Wpointer-arith) \
    $(call cc_supports_flag,-Wwrite-strings) \
    $(call cc_supports_flag,-Wredundant-decls) \
    $(call cc_supports_flag,-Wmissing-declarations) \
    $(call cc_supports_flag,-Winline) \
    $(call cc_supports_flag,-Wcast-align) \
    $(call cc_supports_flag,-Wstrict-aliasing=2) \
    $(call cc_supports_flag,-Wsign-conversion) \
    $(call cc_supports_flag,-Wmissing-include-dirs) \
    $(call cc_supports_flag,-Wunused) \
    $(call cc_supports_flag,-Wbad-function-cast) \
    $(call cc_supports_flag,-Waggregate-return) \
    $(call cc_supports_flag,-Wmissing-format-attribute) \
    $(call cc_supports_flag,-Wlarger-than=256) \
    $(call cc_supports_flag,-Wdisabled-optimization) \
    $(call cc_supports_flag,-Wc++-compat) \
    $(call cc_supports_flag,-Wlogical-op) \
    $(call cc_supports_flag,-Wjump-misses-init) \
    $(call cc_supports_flag,-Wduplicated-cond) \
    $(call cc_supports_flag,-Wnull-dereference) \
    $(call cc_supports_flag,-Wduplicated-branches) \
    $(call cc_supports_flag,-Walloca) \
    $(call cc_supports_flag,-Wformat-overflow=2) \
    $(call cc_supports_flag,-Wformat-truncation=2) \
    $(call cc_supports_flag,-Wmultistatement-macros) \
    $(call cc_supports_flag,-Wthread-safety) \
    $(call cc_supports_flag,-Wdocumentation)
endif # CHECK = 1

# Set NOFLAGS to 1 for C++ compilers
NOFLAGS ?= $(if $(findstring ++,$(notdir $(CC))),1)

# If NOFLAGS is set to 1, set CFLAGS and LDFLAGS to empty
ifeq ($(NOFLAGS),1)
override CFLAGS :=
override LDFLAGS :=
PROJECT_CFLAGS :=
PROJECT_LDFLAGS :=
endif # NOFLAGS = 1

# Platform-specific linker flags
PROJECT_LDFLAGS += \
    $(if $(findstring Linux,$(UNAME)),-lrt) \
    $(if $(findstring FreeBSD,$(UNAME)),-lkvm) \
    $(if $(findstring Darwin,$(UNAME)),-lproc)

ALL_CFLAGS := $(filter-out -Wno%,$(PROJECT_CFLAGS) $(CFLAGS))
ALL_LDFLAGS := $(PROJECT_LDFLAGS) $(LDFLAGS)

# cppcheck options
CPPCHECK_OPTS := \
    --enable=all \
    --language=c \
    --inconclusive \
    --check-level=exhaustive \
    --max-ctu-depth=20 \
    --library=gnu \
    --force \
    --std=c89 \
    --project=compile_commands.json \
    --suppress=missingIncludeSystem \
    --suppress=ConfigurationNotChecked \
    --suppress=unmatchedSuppression \
    --suppress=checkersReport

# clang-tidy options
CLANG_TIDY_OPTS :=
ifeq ($(UNAME),Darwin)
MACOS_SDK := $(shell xcrun --show-sdk-path)
CLANG_TIDY_OPTS += --extra-arg=-isysroot --extra-arg=$(MACOS_SDK)
endif

# Check report file
CPPCHECK_REPORT := cppcheck-report.txt
CLANG_TIDY_REPORT := clang-tidy-report.txt

# Phony targets
.PHONY: all clean format check install uninstall

# Default target
all: $(TARGET)

# Build target
$(TARGET): $(wildcard *.c *.h)
	$(CC) $(ALL_CFLAGS) $(filter-out process_iterator_%.c %.h,$^) $(ALL_LDFLAGS) -o $@

# Clean target
clean:
	rm -f *~ *.o *.json $(TARGET) $(CPPCHECK_REPORT) $(CLANG_TIDY_REPORT)

# Format source files with clang-format
format: $(wildcard *.c *.h)
	@command -v clang-format >/dev/null 2>&1 || { \
	echo "clang-format is not installed. Please install it to format the code."; \
	exit 1; \
	}
	clang-format -i $^
	@echo "Source files formatted successfully."

# Check source files with cppcheck
check: $(wildcard *.c *.h) clean
	@command -v bear >/dev/null 2>&1 || { \
	echo "bear is not installed. Please install it to check the code."; \
	exit 1; \
	}
	@command -v cppcheck >/dev/null 2>&1 || { \
	echo "cppcheck is not installed. Please install it to check the code."; \
	exit 1; \
	}
	@command -v clang-tidy >/dev/null 2>&1 || { \
	echo "clang-tidy is not installed. Please install it to check the code."; \
	exit 1; \
	}
	bear --force-wrapper -- $(MAKE) all CHECK=1
	cppcheck $(CPPCHECK_OPTS) 2>"$(CPPCHECK_REPORT)"
	@echo "See $(shell pwd)/$(CPPCHECK_REPORT) for cppcheck report."
	clang-tidy $(CLANG_TIDY_OPTS) *.h *.c >"$(CLANG_TIDY_REPORT)"
	@echo "See $(shell pwd)/$(CLANG_TIDY_REPORT) for clang-tidy report."

# Install cpulimit to /usr/local/bin
install: $(TARGET)
	install -d $(DESTDIR)
	install -m 755 $(TARGET) $(DESTDIR)

# Uninstall cpulimit from /usr/local/bin
uninstall:
	rm -f $(DESTDIR)/$(TARGET)
