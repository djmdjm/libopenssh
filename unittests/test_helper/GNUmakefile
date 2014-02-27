CFLAGS=-g -O2 -Wall -Wpointer-arith -Wuninitialized -Wsign-compare -Wformat-security -Wno-pointer-sign -Wno-unused-result -fno-strict-aliasing -D_FORTIFY_SOURCE=2 -fstack-protector-all -Werror -Wno-attributes -std=gnu99 -D_GNU_SOURCE -D_BSD_SOURCE

CPPFLAGS=`pkg-config --cflags libbsd-overlay` -I../../ssh

#CFLAGS+=-fsanitize=undefined -fno-omit-frame-pointer
#LDFLAGS+=-fsanitize=undefined

OBJS=test_helper.o fuzz.o

all: libtest_helper.a

clean:
	rm -f libtest_helper.a *.core core $(OBJS)

libtest_helper.a: $(OBJS)
	$(AR) rv $@ $(OBJS)

