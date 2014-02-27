CFLAGS=-g -O2 -Wall -Wpointer-arith -Wuninitialized -Wsign-compare -Wformat-security -Wno-pointer-sign -Wno-unused-result -fno-strict-aliasing -D_FORTIFY_SOURCE=2 -fstack-protector-all -Werror -Wno-attributes -std=gnu99 -D_GNU_SOURCE -D_BSD_SOURCE

CPPFLAGS=`pkg-config --cflags libbsd-overlay` -I../../ssh

#CFLAGS+=-fsanitize=undefined -fno-omit-frame-pointer
#LDFLAGS+=-fsanitize=undefined

OBJS=authfd.o authfile.o canohost.o cipher.o \
	cipher-3des1.o cipher-bf1.o cleanup.o crc32.o \
	deattack.o fatal.o match.o nchan.o \
	rsa.o ttymodes.o xmalloc.o atomicio.o \
	key.o dispatch.o kex.o mac.o uidswap.o uuencode.o \
	misc.o ssh-dss.o ssh-rsa.o ssh-ecdsa.o dh.o kexdh.o \
	kexgex.o kexecdh.o kexdhc.o kexgexc.o kexecdhc.o msg.o \
	progressmeter.o dns.o monitor_fdpass.o umac.o addrmatch.o \
	schnorr.o jpake.o ssh-pkcs11.o krl.o sshbuf-getput-basic.o \
	sshbuf-getput-crypto.o sshbuf-misc.o sshbuf.o err.o \
	kexdhs.o kexgexs.o kexecdhs.o ssh_api.o roaming_dummy.o \
	umax128.o readpass.o packet.o log.o hostfile.o compat.o channels.o

OBJS=sshbuf.o err.o sshbuf-misc.o sshbuf-getput-crypto.o sshbuf-getput-basic.o
OBJS+=key.o authfile.o base64.o ssh-dss.o ssh-rsa.o ssh-ecdsa.o log.o xmalloc.o
OBJS+=timingsafe_bcmp.o atomicio.o cipher.o cipher-3des1.o cipher-bf1.o misc.o
OBJS+=fatal.o cleanup.o rsa.o

all: libopenssh.a

clean:
	rm -f libopenssh.a *.core core $(OBJS) umac128.c

libopenssh.a: $(OBJS)
	$(AR) rv $@ $(OBJS)

umac128.c: umac.c Makefile
	sed \
	    -e "s/^#define UMAC_OUTPUT_LEN     8/#define UMAC_OUTPUT_LEN 16/" \
	    -e s/umac_new/umac128_new/g \
	    -e s/umac_update/umac128_update/g \
	    -e s/umac_final/umac128_final/g \
	    -e s/umac_delete/umac128_delete/g \
	    < ${.CURDIR}/../umac.c > ${.TARGET}

