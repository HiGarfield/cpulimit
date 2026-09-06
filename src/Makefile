include ../common.mk

# Target binary
TARGET := cpulimit

# Destination directory
DESTDIR := /usr/local/bin

# Phony targets
.PHONY: all clean format check install uninstall

# Default target
all: $(TARGET)

# Build target
#
# Every .c in this directory is compiled on every supported platform; the
# platform is resolved by conditional compilation inside the sources (see
# the process_iterator_*.c files). That is what keeps this Makefile free of
# both a filename list and per-platform source filtering, and it is also
# why a new module needs no change here -- only
# cmake/CpulimitOptions.cmake lists the sources explicitly.
$(TARGET): $(wildcard *.c *.h)
	$(CC) $(ALL_CFLAGS) $(filter %.c,$^) $(ALL_LDFLAGS) -o $@

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
	$(call require-tool,bear)
	$(call require-tool,cppcheck)
	$(call require-tool,clang-tidy)
	bear --force-wrapper -- $(MAKE) all 2>/dev/null || \
	bear -- $(MAKE) all
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
