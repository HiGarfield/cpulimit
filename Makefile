# Build output directory, consistent with cmake output path.
BUILD_DIR := build

default: all

.PHONY: default all clean install uninstall test

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
