CFLAGS=-g -O2 -Wall -Wpointer-arith -Wuninitialized -Wsign-compare -Wformat-security -Wno-pointer-sign -Wno-unused-result -fno-strict-aliasing -D_FORTIFY_SOURCE=2 -fno-builtin-memset -fstack-protector-all -Werror -Wno-attributes -std=gnu99 -D_GNU_SOURCE -D_BSD_SOURCE

CPPFLAGS=`pkg-config --cflags libbsd-overlay` -I../../ssh -I../test_helper
LDFLAGS=-L../test_helper -L../../ssh
LIBS=`pkg-config --libs libbsd-overlay` -lrt -lopenssh -lcrypto -ltest_helper

all: run_test_sshbuf

.PHONY: run_test_sshbuf

OBJS=tests.o
OBJS+=test_sshbuf.o
OBJS+=test_sshbuf_getput_basic.o
OBJS+=test_sshbuf_getput_crypto.o
OBJS+=test_sshbuf_misc.o
OBJS+=test_sshbuf_fuzz.o
OBJS+=test_sshbuf_getput_fuzz.o
OBJS+=test_sshbuf_fixed.o

clean:
	rm -f test_sshbuf $(OBJS) core *.core

test_sshbuf: $(OBJS) ../test_helper/libtest_helper.a
	$(CC) -o $@ $(LDFLAGS) $(OBJS) $(LIBS)

run_test_sshbuf: test_sshbuf
	./test_sshbuf
