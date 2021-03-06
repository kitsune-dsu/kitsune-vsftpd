# Makefile for systems with GNU tools

# KITSUNE
SHARED = -fPIC -shared -rdynamic
KITSUNE_INSTALL = ../../../../bin
KITSUNE_LIB = $(KITSUNE_INSTALL)/libkitsune.a
CILLY_PATH = ../../../../tools/bin/ktcc

EKCC 	=	$(CILLY_PATH) --dostackvars --keepunused --doglobalreg
INSTALL	=	install
IFLAGS  = -idirafter dummyinc
CFLAGS = -O2 -fvisibility=hidden -Wall -W -Wshadow -I$(KITSUNE_INSTALL) $(SHARED) 
#CFLAGS	=	-O2 -Wall -W -Wshadow #-pedantic -Werror -Wconversion

LIBS	=	`./vsf_findlibs.sh` -lcrypt
LINK	=	-Wl

OBJS	=	main.o utility.o prelogin.o ftpcmdio.o postlogin.o privsock.o \
		tunables.o ftpdataio.o secbuf.o ls.o \
		postprivparent.o logging.o str.o netstr.o sysstr.o strlist.o \
    banner.o filestr.o parseconf.o secutil.o dsu.o\
    ascii.o oneprocess.o twoprocess.o privops.o standalone.o hash.o \
    tcpwrap.o \
    sysutil.o sysdeputil.o


.c.o:
	$(EKCC) $(CFLAGS) $(IFLAGS) -c $*.c -include kitsune.h

vsftpd: $(OBJS) 
	$(CC) -ggdb3 -o vsftpd.so $(OBJS) $(LINK) $(LIBS) $(KITSUNE_LIB) $(SHARED)
	cp vsftpd.so vsftpd2.so

install:
	if [ -x /usr/local/sbin ]; then \
		$(INSTALL) -m 755 vsftpd /usr/local/sbin/vsftpd; \
	else \
		$(INSTALL) -m 755 vsftpd /usr/sbin/vsftpd; fi
	if [ -x /usr/local/man ]; then \
		$(INSTALL) -D -m 644 vsftpd.8 /usr/local/man/man8/vsftpd.8; \
		$(INSTALL) -D -m 644 vsftpd.conf.5 /usr/local/man/man5/vsftpd.conf.5; \
	elif [ -x /usr/share/man ]; then \
		$(INSTALL) -D -m 644 vsftpd.8 /usr/share/man/man8/vsftpd.8; \
		$(INSTALL) -D -m 644 vsftpd.conf.5 /usr/share/man/man5/vsftpd.conf.5; \
	else \
		$(INSTALL) -D -m 644 vsftpd.8 /usr/man/man8/vsftpd.8; \
		$(INSTALL) -D -m 644 vsftpd.conf.5 /usr/man/man5/vsftpd.conf.5; fi
	if [ -x /etc/xinetd.d ]; then \
		$(INSTALL) -m 644 xinetd.d/vsftpd /etc/xinetd.d/vsftpd; fi

clean:
	rm -f *.o *.swp vsftpd vsftpd.so *.cil.c *.cil.i *.i

