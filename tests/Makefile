include ../common.mk

# Target binaries and source directory
TARGETS := $(patsubst %.c,%,$(wildcard *.c))
SRC := ../src

# The test binary is multi-threaded
PROJECT_CFLAGS += -pthread
PROJECT_LDFLAGS += -pthread

# Phony targets
.PHONY: all clean format check

# Default target
all: $(TARGETS)

# The load helpers need a subset of src/: the CPU count, the whole-file
# read it uses on Linux, signal handling and the generic helpers.
LOAD_HELPER_SRC := \
	$(wildcard $(SRC)/util.*) \
	$(wildcard $(SRC)/signal_handler.*) \
	$(wildcard $(SRC)/cpu_count.*) \
	$(wildcard $(SRC)/file_io.*)

# Target rules: one static pattern rule builds both load helpers from
# their own .c plus the shared subset above.
busy multi_process_busy: %: %.c $(LOAD_HELPER_SRC)
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
# The three platform iterators are named explicitly because they are the
# files that DEFINE init_process_iterator() and friends; the
# platform-independent process_iterator_common.c must stay in the plain
# group. A wildcard here would sweep it in and change which renames apply
# to it, so this is one place a list is unavoidable.
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
	$(call require-tool,bear)
	$(call require-tool,cppcheck)
	$(call require-tool,clang-tidy)
	bear --force-wrapper -- $(MAKE) all 2>/dev/null || \
	bear -- $(MAKE) all
	cppcheck $(CPPCHECK_OPTS) 2>"$(CPPCHECK_REPORT)"
	@echo "See $(shell pwd)/$(CPPCHECK_REPORT) for cppcheck report."
	clang-tidy $(CLANG_TIDY_OPTS) *.c >"$(CLANG_TIDY_REPORT)"
	@echo "See $(shell pwd)/$(CLANG_TIDY_REPORT) for clang-tidy report."
