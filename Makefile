default: all

.DEFAULT:
	$(MAKE) -C src $@
	$(MAKE) -C tests $@

install:
	$(MAKE) -C src $@

uninstall:
	$(MAKE) -C src $@
