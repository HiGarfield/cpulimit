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

# Objects for the shared subset. They are prefixed with load_ because the
# same src/ files are also compiled into the seam objects below with
# different -D flags and must not end up sharing a .o with them.
LOAD_OBJS := $(patsubst $(SRC)/%.c,load_%.o,$(filter %.c,$(LOAD_HELPER_SRC)))

$(LOAD_OBJS): load_%.o: $(SRC)/%.c
	$(CC) $(ALL_CFLAGS) -c $< -o $@

# Target rules: one static pattern rule builds both load helpers from
# their own .o plus the objects of the shared subset above.
busy multi_process_busy: %: %.o $(LOAD_OBJS)
	$(CC) $(ALL_CFLAGS) $^ $(ALL_LDFLAGS) -o $@

%.o: %.c
	$(CC) $(ALL_CFLAGS) -c $< -o $@

# Deterministic timing seam (test binary only)
#
# Mirrors tests/CMakeLists.txt: the race tests replace the program's
# sources of non-determinism by renaming those entry points at compile
# time. cpulimit_test.c itself is left alone so it keeps calling the real
# implementations and can forward to them while the seam is inactive.
#
# A file that DEFINES a renamed entry point must not have that rename
# applied, or its own definition would be renamed too and collide with the
# test's replacement. Every source therefore receives all renames except
# the ones it defines; the two lists below are the definers.
#
# CPULIMIT_TEST_BUILD renames nothing: it opts the platform is_child_of()
# implementations into the getppid_of() test seam (BUG-043).  Without it the
# seam is compiled out and the BUG-043 regression test fails in this build.
SEAM_ALL_FLAGS := -Dkill=cpulimit_test_kill -Drandom=cpulimit_test_random \
	-Dgetloadavg=cpulimit_test_getloadavg -Dwaitpid=cpulimit_test_waitpid -DCPULIMIT_TEST_BUILD
SEAM_TIME_ITER_FLAGS := -Dget_current_time=cpulimit_test_get_current_time \
	-Dsleep_timespec=cpulimit_test_sleep_timespec \
	-Dinit_process_iterator=cpulimit_test_init_process_iterator \
	-Dget_next_process=cpulimit_test_get_next_process \
	-Dclose_process_iterator=cpulimit_test_close_process_iterator
SEAM_LIMIT_FLAGS := -Dlimit_process=cpulimit_test_limit_process
SEAM_UPDATE_FLAGS := -Dupdate_process_set=cpulimit_test_update_process_set

# Everything under src/ except the entry point.
SEAM_SRC := $(filter-out $(SRC)/main.c,$(wildcard $(SRC)/*.c))

# Definers of the clock/sleep/iterator entry points: time_util.c plus the
# platform iterators. process_iterator_common.c only calls them, so it is
# filtered out.
SEAM_TIME_ITER_SRC := $(SRC)/time_util.c \
	$(filter-out $(SRC)/process_iterator_common.c,$(wildcard $(SRC)/process_iterator_*.c))
SEAM_LIMIT_SRC := $(SRC)/limit_process.c
SEAM_UPDATE_SRC := $(SRC)/process_set.c

SEAM_OBJS := $(SEAM_SRC:$(SRC)/%.c=%.o)

# One rule for all of them: each file gets every rename except its own.
$(SEAM_OBJS): %.o: $(SRC)/%.c
	$(CC) $(ALL_CFLAGS) $(SEAM_ALL_FLAGS) $(if $(filter $<,$(SEAM_TIME_ITER_SRC)),,$(SEAM_TIME_ITER_FLAGS)) $(if $(filter $<,$(SEAM_LIMIT_SRC)),,$(SEAM_LIMIT_FLAGS)) $(if $(filter $<,$(SEAM_UPDATE_SRC)),,$(SEAM_UPDATE_FLAGS)) -c $< -o $@

cpulimit_test: cpulimit_test.o $(SEAM_OBJS)
	$(CC) $(ALL_CFLAGS) $^ $(ALL_LDFLAGS) -o $@

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
