# Build output directory, consistent with cmake output path.
BUILD_DIR := build

# Check report file names (same as in src/Makefile and tests/Makefile).
CPPCHECK_REPORT := cppcheck-report.txt
CLANG_TIDY_REPORT := clang-tidy-report.txt

default: all

.PHONY: default all clean install uninstall test check

all:
	$(MAKE) -C src all
	$(MAKE) -C tests all
	@mkdir -p $(BUILD_DIR)
	@mv -f src/cpulimit $(BUILD_DIR)/
	@mv -f tests/busy $(BUILD_DIR)/
	@mv -f tests/multi_process_busy $(BUILD_DIR)/
	@mv -f tests/cpulimit_test $(BUILD_DIR)/

clean:
	$(MAKE) -C src clean
	$(MAKE) -C tests clean
	rm -rf $(BUILD_DIR)

.DEFAULT:
	$(MAKE) -C src $@
	$(MAKE) -C tests $@

install:
	$(MAKE) -C src $@

uninstall:
	$(MAKE) -C src $@

test: all
	./$(BUILD_DIR)/cpulimit_test

# Run static analysis in src/ and tests/, then merge the reports into
# the build directory, consistent with cmake --build . --target check.
check:
	$(MAKE) -C src check
	$(MAKE) -C tests check
	@mkdir -p $(BUILD_DIR)
	@cat src/$(CPPCHECK_REPORT) tests/$(CPPCHECK_REPORT) \
	    > $(BUILD_DIR)/$(CPPCHECK_REPORT)
	@cat src/$(CLANG_TIDY_REPORT) tests/$(CLANG_TIDY_REPORT) \
	    > $(BUILD_DIR)/$(CLANG_TIDY_REPORT)
	@echo "See $(CURDIR)/$(BUILD_DIR)/$(CPPCHECK_REPORT) for cppcheck report."
	@echo "See $(CURDIR)/$(BUILD_DIR)/$(CLANG_TIDY_REPORT) for clang-tidy report."
