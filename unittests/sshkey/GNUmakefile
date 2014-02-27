CFLAGS=-g -O2 -Wall -Wpointer-arith -Wuninitialized -Wsign-compare -Wformat-security -Wno-pointer-sign -Wno-unused-result -fno-strict-aliasing -D_FORTIFY_SOURCE=2 -fstack-protector-all -Werror -Wno-attributes -std=gnu99 -D_GNU_SOURCE -D_BSD_SOURCE

CPPFLAGS=`pkg-config --cflags libbsd-overlay` -I../../ssh -I../test_helper
LDFLAGS=-L../test_helper -L../../ssh -g
LIBS=-lopenssh -lcrypto -lrt -ltest_helper `pkg-config --libs libbsd-overlay`

#CFLAGS+=-fsanitize=undefined -fno-omit-frame-pointer
#LDFLAGS+=-fsanitize=undefined

all: run_test_sshkey

.PHONY: run_test_sshkey

OBJS=tests.o test_sshkey.o test_file.o test_fuzz.o common.o

clean:
	rm -f test_sshkey $(OBJS) core *.core

test_sshkey: $(OBJS) ../test_helper/libtest_helper.a
	$(CC) -o $@ $(LDFLAGS) $(OBJS) $(LIBS)

run_test_sshkey: test_sshkey
	./test_sshkey -d $(CURDIR)/testdata
