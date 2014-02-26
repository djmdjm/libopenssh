SUBDIRS=test_helper sshbuf sshkey # kex

all: $(SUBDIRS)

.PHONY: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@

clean:
	cd test_helper && $(MAKE) clean
	cd sshbuf && $(MAKE) clean
	cd sshkey && $(MAKE) clean
