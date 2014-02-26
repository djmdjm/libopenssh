all: ssh unittests

.PHONY: ssh unittests

ssh:
	$(MAKE) -C $@

unittests:
	$(MAKE) -C $@

clean:
	cd ssh && $(MAKE) clean
	cd unittests && $(MAKE) clean
