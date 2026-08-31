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
endif # NOFLAGS = 1

# Platform-specific linker flags
PROJECT_LDFLAGS += \
    $(if $(findstring Linux,$(UNAME)),-lrt) \
    $(if $(findstring FreeBSD,$(UNAME)),-lkvm) \
    $(if $(findstring Darwin,$(UNAME)),-lproc)

# Add pthread support
PROJECT_CFLAGS += -pthread
PROJECT_LDFLAGS += -pthread

ALL_CFLAGS := $(PROJECT_CFLAGS) $(CFLAGS)
ALL_LDFLAGS := $(LDFLAGS) $(PROJECT_LDFLAGS)

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

# Phony targets
.PHONY: all clean format check

# Default target
all: $(TARGETS)

# Target rules
busy: busy.c $(wildcard $(SRC)/util.*) $(wildcard $(SRC)/signal_handler.*)
	$(CC) $(ALL_CFLAGS) $(filter %.c,$^) $(ALL_LDFLAGS) -o $@

multi_process_busy: multi_process_busy.c $(wildcard $(SRC)/util.*) \
	$(wildcard $(SRC)/signal_handler.*)
	$(CC) $(ALL_CFLAGS) $(filter %.c,$^) $(ALL_LDFLAGS) -o $@

# Deterministic timing seam (test binary only)
#
# Mirrors tests/CMakeLists.txt: the race tests replace the program's
# sources of non-determinism by renaming those entry points at compile
# time.  cpulimit_test.c itself is left alone so it keeps calling the
# real implementations and can forward to them while the seam is
# inactive.  A file that defines a renamed function must be excluded from
# that rename only, otherwise its definition would be renamed too and
# collide with the replacement, so each group is compiled separately.
SEAM_ALL_FLAGS := -Dkill=cpulimit_test_kill -Drandom=cpulimit_test_random \
	-Dgetloadavg=cpulimit_test_getloadavg -Dwaitpid=cpulimit_test_waitpid
SEAM_TIME_ITER_FLAGS := -Dget_current_time=cpulimit_test_get_current_time \
	-Dsleep_timespec=cpulimit_test_sleep_timespec \
	-Dinit_process_iterator=cpulimit_test_init_process_iterator \
	-Dget_next_process=cpulimit_test_get_next_process \
	-Dclose_process_iterator=cpulimit_test_close_process_iterator
SEAM_LIMIT_FLAGS := -Dlimit_process=cpulimit_test_limit_process

SEAM_SRC := $(filter-out $(SRC)/main.c,$(wildcard $(SRC)/*.c))
SEAM_TIME_ITER_SRC := $(SRC)/time_util.c $(SRC)/process_iterator_linux.c \
	$(SRC)/process_iterator_freebsd.c $(SRC)/process_iterator_apple.c
SEAM_LIMIT_SRC := $(SRC)/limit_process.c
SEAM_PLAIN_SRC := $(filter-out $(SEAM_TIME_ITER_SRC) $(SEAM_LIMIT_SRC),$(SEAM_SRC))

$(SEAM_PLAIN_SRC:$(SRC)/%.c=%.o): %.o: $(SRC)/%.c
	$(CC) $(ALL_CFLAGS) $(SEAM_ALL_FLAGS) $(SEAM_TIME_ITER_FLAGS) $(SEAM_LIMIT_FLAGS) -c $< -o $@
$(SEAM_TIME_ITER_SRC:$(SRC)/%.c=%.o): %.o: $(SRC)/%.c
	$(CC) $(ALL_CFLAGS) $(SEAM_ALL_FLAGS) $(SEAM_LIMIT_FLAGS) -c $< -o $@
$(SEAM_LIMIT_SRC:$(SRC)/%.c=%.o): %.o: $(SRC)/%.c
	$(CC) $(ALL_CFLAGS) $(SEAM_ALL_FLAGS) $(SEAM_TIME_ITER_FLAGS) -c $< -o $@

cpulimit_test: cpulimit_test.c $(SEAM_SRC:$(SRC)/%.c=%.o)
	$(CC) $(ALL_CFLAGS) cpulimit_test.c $(SEAM_SRC:$(SRC)/%.c=%.o) $(ALL_LDFLAGS) -o $@

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
	bear --force-wrapper -- $(MAKE) all 2>/dev/null || \
	bear -- $(MAKE) all
	cppcheck $(CPPCHECK_OPTS) 2>"$(CPPCHECK_REPORT)"
	@echo "See $(shell pwd)/$(CPPCHECK_REPORT) for cppcheck report."
	clang-tidy $(CLANG_TIDY_OPTS) *.c >"$(CLANG_TIDY_REPORT)"
	@echo "See $(shell pwd)/$(CLANG_TIDY_REPORT) for clang-tidy report."
