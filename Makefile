default: all

.DEFAULT:
	$(MAKE) -C src $@
	$(MAKE) -C tests $@

install:
	$(MAKE) -C src $@

uninstall:
	$(MAKE) -C src $@

test:
	$(MAKE) -C tests all
	./tests/cpulimit_test
