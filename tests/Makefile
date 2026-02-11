# Compiler configuration
CC ?= gcc

# Target binaries and source directory
TARGETS := $(patsubst %.c,%,$(wildcard *.c))
SRC := ../src

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
    -pedantic \
    -Wall \
    -Wextra \
    -Wold-style-definition \
    -Wmissing-prototypes \
    -Wstrict-prototypes \
    -Werror \
    -pipe

# If CHECK is set to 1, add stricter flags
ifeq ($(CHECK),1)
PROJECT_CFLAGS += \
    -Wshadow \
    -Wcast-qual \
    -Wconversion \
    -Wformat=2 \
    -Wstrict-overflow=5 \
    -Wundef \
    -Wswitch-default \
    -Wswitch-enum \
    -Wunreachable-code \
    -Wfloat-equal \
    -Wpointer-arith \
    -Wwrite-strings \
    -Wredundant-decls \
    -Wmissing-declarations \
    -Winline \
    -Wcast-align \
    -Wstrict-aliasing=2 \
    -Wsign-conversion \
    -Wmissing-include-dirs \
    -Wunused \
    -Wbad-function-cast \
    -Waggregate-return \
    -Wmissing-format-attribute \
    -Wlarger-than=256 \
    -Wdisabled-optimization \
    -Wc++-compat
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

# Add pthread support
PROJECT_CFLAGS += -pthread
PROJECT_LDFLAGS += -pthread

ALL_CFLAGS := $(filter-out -Wno%,$(PROJECT_CFLAGS) $(CFLAGS))
ALL_LDFLAGS := $(PROJECT_LDFLAGS) $(LDFLAGS)

# cppcheck options
CPPCHECK_OPTS := $(shell \
    echo | $(CC) -E -x c -v - 2>&1 | \
    grep "^[[:space:]]*/[[:graph:]]*/include[[:graph:]]*$$" | \
    sed 's/^/-I /' \
    )
CPPCHECK_OPTS += --enable=all \
                 --inconclusive \
                 --check-level=exhaustive \
                 --max-ctu-depth=10 \
                 --library=gnu \
                 --force \
                 --std=c89 \
                 --project=compile_commands.json \
                 --suppress=missingIncludeSystem \
                 --suppress=ConfigurationNotChecked \
                 --suppress=unmatchedSuppression \
                 --suppress=*:/*/include/* \
                 --suppress=checkersReport

# Check report file
CPPCHECK_REPORT := cppcheck-report.txt
CLANG_TIDY_REPORT := clang-tidy-report.txt

# Phony targets
.PHONY: all clean format check

# Default target
all: $(TARGETS)

# Target rules
busy: busy.c $(wildcard $(SRC)/util.*) $(wildcard $(SRC)/signal_handler.*)
	$(CC) $(ALL_CFLAGS) $(filter-out %.h,$^) $(ALL_LDFLAGS) -o $@

multi_process_busy: multi_process_busy.c $(wildcard $(SRC)/util.*) $(wildcard $(SRC)/signal_handler.*)
	$(CC) $(ALL_CFLAGS) $(filter-out %.h,$^) $(ALL_LDFLAGS) -o $@

cpulimit_test: cpulimit_test.c \
	$(filter-out $(SRC)/main.c $(SRC)/cli.c $(SRC)/limiter.c, \
	$(wildcard $(SRC)/*.c $(SRC)/*.h))
	$(CC) $(ALL_CFLAGS) $(filter-out $(SRC)/process_iterator_%.c %.h,$^) $(ALL_LDFLAGS) -o $@

# Clean target
clean:
	rm -f *~ *.o *.json $(TARGETS) $(CPPCHECK_REPORT) $(CLANG_TIDY_REPORT)

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
	bear -- $(MAKE) all CHECK=1
	cppcheck $(CPPCHECK_OPTS) 2>"$(CPPCHECK_REPORT)"
	@echo "See $(shell pwd)/$(CPPCHECK_REPORT) for cppcheck report."
	clang-tidy *.c >"$(CLANG_TIDY_REPORT)"
	@echo "See $(shell pwd)/$(CLANG_TIDY_REPORT) for clang-tidy report."
