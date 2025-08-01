# Compiler configuration
CC ?= gcc

# Force 64-bit time_t and file offsets to prevent Y2038 issue
override CFLAGS += -D_TIME_BITS=64 -D_FILE_OFFSET_BITS=64

# Compiler flags
override CFLAGS := $(filter-out -Wno%,$(CFLAGS)) \
                   $(if $(filter -O%,$(CFLAGS)),,-O2) \
                   $(if $(filter -ansi -std=%,$(CFLAGS)),,-std=c89) \
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
override CFLAGS += -Wshadow \
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
NOFLAGS ?= $(if $(findstring ++,$(CC)),1)

# If NOFLAGS is set to 1, set CFLAGS and LDFLAGS to empty
ifeq ($(NOFLAGS),1)
override CFLAGS :=
override LDFLAGS :=
endif # NOFLAGS = 1

# Target binary
TARGET := cpulimit

# Destination directory
DESTDIR := /usr/local/bin

# Detect operating system
UNAME ?= $(shell uname)

# Platform-specific linker flags
override LDFLAGS += $(if $(findstring Linux,$(UNAME)),-lrt) \
                    $(if $(findstring FreeBSD,$(UNAME)),-lkvm) \
                    $(if $(findstring Darwin,$(UNAME)),-lproc)

# cppcheck options
CPPCHECK_OPTS := $(shell \
    echo | $(CC) -E -x c -v - 2>&1 | \
    grep "^[[:space:]]*/[[:graph:]]*/include[[:graph:]]*$$" | \
    sed 's/^/-I /' \
)
CPPCHECK_OPTS += $(if $(findstring Linux,$(UNAME)),-D__linux__) \
                 $(if $(findstring FreeBSD,$(UNAME)),-D__FreeBSD__) \
                 $(if $(findstring Darwin,$(UNAME)),-D__APPLE__)
CPPCHECK_OPTS += -D_GNU_SOURCE
CPPCHECK_OPTS += --enable=all \
                 --inconclusive \
                 --check-level=exhaustive \
                 --max-ctu-depth=10 \
                 --library=gnu \
                 --force \
                 --std=c89 \
                 --suppress=missingIncludeSystem \
                 --suppress=ConfigurationNotChecked \
                 --suppress=unmatchedSuppression \
                 --suppress=*:/*/include/* \
                 --suppress=checkersReport

# Check report file
CHECK_REPORT := check-report.txt

# Phony targets
.PHONY: all clean format check install uninstall

# Default target
all: $(TARGET)

# Build target
$(TARGET): $(wildcard *.c *.h)
	$(CC) $(CFLAGS) $(filter-out process_iterator_%.c %.h,$^) $(LDFLAGS) -o $@

# Clean target
clean:
	rm -f *~ *.o *.json $(TARGET) $(CHECK_REPORT)

# Format source files with clang-format
format: $(wildcard *.c *.h)
	@command -v clang-format >/dev/null 2>&1 || { \
        echo "clang-format is not installed. Please install it to format the code."; \
        exit 1; \
    }
	clang-format -i $^
	@echo "Source files formatted successfully."

# Check source files with cppcheck
check: $(wildcard *.c *.h)
	@command -v cppcheck >/dev/null 2>&1 || { \
        echo "cppcheck is not installed. Please install it to check the code."; \
        exit 1; \
    }
	cppcheck $(CPPCHECK_OPTS) . 2>"$(CHECK_REPORT)"
	@echo "See $(shell pwd)/$(CHECK_REPORT) for cppcheck report."

# Install cpulimit to /usr/local/bin
install: $(TARGET)
	install -d $(DESTDIR)
	install -m 755 $(TARGET) $(DESTDIR)

# Uninstall cpulimit from /usr/local/bin
uninstall:
	rm -f $(DESTDIR)/$(TARGET)
