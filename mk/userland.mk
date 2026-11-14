#
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-14
# SPDX-License-Identifier: MIT
#
# The system programs on the root image (tools/mkroot.sh), linked
# statically against mlibc, and the utilities, most linked dynamically:
#
#   /sbin/init          NetBSD's (contrib/netbsd/sbin/init)
#   /sbin/reboot, halt  Belka's (usr/sbin/reboot), through uadmin
#   /usr/libexec/getty  Belka's (usr/sbin/getty)
#   /usr/bin/login      NetBSD's (contrib/netbsd/usr.bin/login)
#   /bin/sh, /bin/mksh  mksh R59c (contrib/mksh)
#   /bin/ls, cat, ...  ksenon-utils (contrib/ksenon-utils), below
#
# The imported programs build over a small compat library of the BSD
# interfaces mlibc lacks (usr/lib/compat), with NetBSD's getttyent and
# SHA1 crypt in it. They are built with warnings, but not as errors:
# their code is kept as it came.
#

SYS_DIR         := build/sys-$(TOOLCHAIN)$(LDSUFFIX)
SYS_OBJ         := $(SYS_DIR)/obj

NB              := contrib/netbsd
COMPAT_INC      := -Iusr/lib/compat/include -I$(NB)/include -I$(NB)/sys \
                   -include usr/lib/compat/include/compat.h
IMPORT_CFLAGS   := -std=gnu99 -O2 -g -Wall -nostdinc \
                   -isystem $(SYSROOT)/usr/include -isystem $(CC_INCLUDE) \
                   -fno-stack-protector -fno-pic -fno-pie -D_GNU_SOURCE \
                   -D_DEFAULT_SOURCE $(COMPAT_INC) \
                   -Wno-unused-parameter -Wno-sign-compare
OWN_CFLAGS      := $(LIBC_CFLAGS) -fno-pic -fno-pie

COMPAT_SRCS     := usr/lib/compat/db.c usr/lib/compat/misc.c \
                   usr/lib/compat/vis.c usr/lib/compat/crypt.c \
                   usr/lib/compat/crypt_to64.c \
                   $(NB)/lib/libc/gen/getttyent.c \
                   $(NB)/lib/libc/stdio/fparseln.c \
                   $(NB)/lib/libcrypt/crypt-sha1.c \
                   $(NB)/lib/libcrypt/hmac_sha1.c \
                   $(NB)/common/lib/libc/hash/sha1/sha1.c
COMPAT_OBJS     := $(patsubst %.c,$(SYS_OBJ)/%.o,$(COMPAT_SRCS))
COMPAT_LIB      := $(SYS_DIR)/libcompat.a

COMPAT_HDRS     := $(wildcard usr/lib/compat/include/*.h \
                   usr/lib/compat/include/*/*.h)

$(SYS_OBJ)/%.o: %.c $(COMPAT_HDRS) $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(IMPORT_CFLAGS) -I$(NB)/lib/libcrypt \
	    $(if $(findstring fparseln,$<),-DHAVE_NBTOOL_CONFIG_H=1) -c -o $@ $<

$(COMPAT_LIB): $(COMPAT_OBJS)
	rm -f $@
	ar rcs $@ $^

# A static program from objects: $(call SYS_LINK,objects).
SYS_LINK        = $(LINK_PROGRAM) $(LIBC_CRT0) $(PROGRAM_BEGIN) $(1) \
                  $(COMPAT_LIB) $(MLIBC) $(LIBC_RTLIB) $(PROGRAM_END) \
                  $(LIBC_CRTN)
SYS_DEPS        := $(COMPAT_LIB) $(MLIBC) $(PROGRAM_BEGIN) $(PROGRAM_END) \
                   $(LD_DEPS)

# init: no chroot, utmp or securelevel; asks for root's password before
# a single-user shell if the console is not marked secure in /etc/ttys.
INIT_OBJS       := $(SYS_OBJ)/init.o
$(SYS_OBJ)/init.o: $(NB)/sbin/init/init.c $(COMPAT_HDRS) $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(IMPORT_CFLAGS) -DALTSHELL -DSECURE -c -o $@ $<

$(SYS_DIR)/init: $(INIT_OBJS) $(SYS_DEPS)
	$(call SYS_LINK,$(INIT_OBJS))

# login: no PAM, login classes, utmp, Kerberos or S/Key.
LOGIN_CFLAGS    := -DNO_PW_AGING
LOGIN_OBJS      := $(SYS_OBJ)/login.o $(SYS_OBJ)/login-common.o \
                   $(SYS_OBJ)/copyrightstr.o
$(SYS_OBJ)/login.o: $(NB)/usr.bin/login/login.c $(COMPAT_HDRS) $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(IMPORT_CFLAGS) $(LOGIN_CFLAGS) -c -o $@ $<
$(SYS_OBJ)/login-common.o: $(NB)/usr.bin/login/common.c $(COMPAT_HDRS) \
                $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(IMPORT_CFLAGS) $(LOGIN_CFLAGS) -c -o $@ $<
$(SYS_OBJ)/copyrightstr.o: usr/lib/compat/copyrightstr.c
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(OWN_CFLAGS) -c -o $@ $<

$(SYS_DIR)/login: $(LOGIN_OBJS) $(SYS_DEPS)
	$(call SYS_LINK,$(LOGIN_OBJS))

# Belka's own.
$(SYS_OBJ)/getty.o: usr/sbin/getty/getty.c $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(OWN_CFLAGS) -c -o $@ $<
$(SYS_DIR)/getty: $(SYS_OBJ)/getty.o $(SYS_DEPS)
	$(call SYS_LINK,$(SYS_OBJ)/getty.o)

$(SYS_OBJ)/reboot.o: usr/sbin/reboot/reboot.c $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(OWN_CFLAGS) -c -o $@ $<
$(SYS_DIR)/reboot: $(SYS_OBJ)/reboot.o $(SYS_DEPS)
	$(call SYS_LINK,$(SYS_OBJ)/reboot.o)

# mksh, configured by hand (usr/lib/compat/mksh-config.h) rather than
# by its Build.sh, with its option tables made by tools/mksh-genopt.awk
# and its signal names by tools/mksh-signames.sh.
MKSH_SRCS       := lalloc edit eval exec expr funcs histrap jobs lex main \
                   misc shf syn tree var ulimit
MKSH_OBJS       := $(patsubst %,$(SYS_OBJ)/mksh/%.o,$(MKSH_SRCS))
MKSH_GEN        := $(SYS_OBJ)/mksh/sh_flags.gen $(SYS_OBJ)/mksh/rlimits.gen \
                   $(SYS_OBJ)/mksh/ulimits.gen $(SYS_OBJ)/mksh/signames.inc
MKSH_CFLAGS     := $(IMPORT_CFLAGS) -I$(SYS_OBJ)/mksh -Icontrib/mksh \
                   -include usr/lib/compat/mksh-config.h \
                   -Wno-char-subscripts

$(SYS_OBJ)/mksh/%.gen: contrib/mksh/%.opt tools/mksh-genopt.awk
	@mkdir -p $(dir $@)
	LC_ALL=C awk -f tools/mksh-genopt.awk $< > $@.tmp && mv $@.tmp $@

$(SYS_OBJ)/mksh/signames.inc: tools/mksh-signames.sh $(MLIBC)
	@mkdir -p $(dir $@)
	sh tools/mksh-signames.sh "$(LIBC_CC) $(IMPORT_CFLAGS)" > $@.tmp && \
	    mv $@.tmp $@

$(SYS_OBJ)/mksh/%.o: contrib/mksh/%.c contrib/mksh/sh.h \
                usr/lib/compat/mksh-config.h $(MKSH_GEN) $(COMPAT_HDRS)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(MKSH_CFLAGS) -c -o $@ $<

$(SYS_DIR)/mksh: $(MKSH_OBJS) $(SYS_DEPS)
	$(call SYS_LINK,$(MKSH_OBJS))

# The host's password hasher for the image's password file, from the
# same crypt sources (tools/mkpasswd.c).
MKPASSWD        := build/tools/mkpasswd
MKPASSWD_SRCS   := tools/mkpasswd.c usr/lib/compat/crypt_to64.c \
                   $(NB)/lib/libcrypt/crypt-sha1.c \
                   $(NB)/lib/libcrypt/hmac_sha1.c \
                   $(NB)/common/lib/libc/hash/sha1/sha1.c

$(MKPASSWD): $(MKPASSWD_SRCS) $(wildcard tools/mkpasswd-inc/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -w -include tools/mkpasswd-inc/host.h -Itools/mkpasswd-inc \
	    -I$(NB)/sys -I$(NB)/lib/libcrypt -D'__RCSID(s)=' \
	    -D'_DIAGASSERT(e)=((void)0)' -o $@ $(MKPASSWD_SRCS)

# The boot test (usr/tests/boottest.c), for the test root image only.
$(SYS_OBJ)/boottest.o: usr/tests/boottest.c sys/sys/procfs.h $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(OWN_CFLAGS) -c -o $@ $<
$(SYS_DIR)/boottest: $(SYS_OBJ)/boottest.o $(SYS_DEPS)
	$(call SYS_LINK,$(SYS_OBJ)/boottest.o)

SYS_PROGS       := $(addprefix $(SYS_DIR)/,init login getty reboot mksh)

sys-progs: $(SYS_PROGS)

.PHONY: sys-progs

# ksenon-utils, the 45 System V-style utilities (contrib/ksenon-utils,
# its belka branch): built with their own flags (strict POSIX-2001/XSI)
# and their own library, libxn, with lib/plat_belka.c, against mlibc
# alone, without the compat library (see below for linking). cp and ln
# are mv, chgrp is chown, and [ is test, as hard links on the image.
XN              := contrib/ksenon-utils
XN_OBJ          := $(SYS_OBJ)/xn
XN_DIR          := $(SYS_DIR)/xn
XN_CFLAGS       := -std=c99 -pedantic -D_POSIX_C_SOURCE=200112L \
                   -D_XOPEN_SOURCE=600 -O2 -g -Wall -Wextra -Wshadow \
                   -Wstrict-prototypes -Wmissing-prototypes -nostdinc \
                   -isystem $(SYSROOT)/usr/include -isystem $(CC_INCLUDE) \
                   -fno-stack-protector -fno-pic -fno-pie -I$(XN)/include
XN_LIBSRCS      := getopt mode util walk plat_belka
XN_PROGS        := basename cat chmod chown cmp comm cut date dd df dirname \
                   du echo env expr false head id ls mkdir mkfifo mv nice \
                   od paste printf pwd rm rmdir sleep sort tail tee test \
                   touch tr true tty uname uniq wc who
XN_LIB          := $(XN_DIR)/libxn.a
XN_BINS         := $(addprefix $(XN_DIR)/,$(XN_PROGS))

$(XN_OBJ)/%.o: $(XN)/lib/%.c $(XN)/include/xn.h $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(XN_CFLAGS) -c -o $@ $<

$(XN_OBJ)/cmd/%.o: $(XN)/cmd/%.c $(XN)/include/xn.h $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(XN_CFLAGS) -c -o $@ $<

$(XN_LIB): $(patsubst %,$(XN_OBJ)/%.o,$(XN_LIBSRCS))
	@mkdir -p $(dir $@)
	rm -f $@
	ar rcs $@ $^

# Linked dynamically against libc.so.1, as ksenon-utils' own Makefile
# does, but for mv (and so cp and ln), which is static so that it still
# runs while the shared libraries are being moved.
$(XN_BINS): $(XN_DIR)/%: $(XN_OBJ)/cmd/%.o $(XN_LIB) $(MLIBC) \
                $(MLIBC_SHARED) $(PROGRAM_BEGIN) $(PROGRAM_END) $(LD_DEPS)
	$(if $(filter mv,$*),$(LINK_PROGRAM) $(LIBC_CRT0) $(PROGRAM_BEGIN) $< \
	    $(XN_LIB) $(MLIBC),$(call LINK_DYNAMIC,$(DYN_INTERP),) \
	    $(LIBC_CRT0) $(PROGRAM_BEGIN) $< $(XN_LIB) $(SYSLIB)/libc.so) \
	    $(LIBC_RTLIB) $(PROGRAM_END) $(LIBC_CRTN)

xn-progs: $(XN_BINS)

.PHONY: xn-progs

# ps, Belka's own (usr/bin/ps), over /proc/pinfo; dynamic, like the
# utilities.
$(SYS_OBJ)/ps.o: usr/bin/ps/ps.c sys/sys/procfs.h $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(OWN_CFLAGS) -c -o $@ $<
$(SYS_DIR)/ps: $(SYS_OBJ)/ps.o $(MLIBC_SHARED) $(PROGRAM_BEGIN) $(PROGRAM_END) \
                $(LD_DEPS)
	$(call LINK_DYNAMIC,$(DYN_INTERP),) $(LIBC_CRT0) $(PROGRAM_BEGIN) $< \
	    $(SYSLIB)/libc.so $(LIBC_RTLIB) $(PROGRAM_END) $(LIBC_CRTN)

# The root images (tools/mkroot.sh): for "run", kept between boots and
# made again when what goes in changes; for "test-boot", made afresh
# for each boot.
ROOT_IMG        := build/root-$(TOOLCHAIN)$(LDSUFFIX).img
ROOT_TEST_IMG   := build/root-test-$(TOOLCHAIN)$(LDSUFFIX).img
ROOT_DEPS       := $(SYS_PROGS) $(SYS_DIR)/ps $(XN_BINS) $(LIBC_SOS) \
                   $(MKPASSWD) tools/mkroot.sh \
                   $(wildcard etc/* etc/defaults/*)

$(ROOT_IMG): $(ROOT_DEPS)
	sh tools/mkroot.sh $@ $(SYS_DIR) $(MKPASSWD) $(LIBC_SODIR)

MKROOT_TEST     = sh tools/mkroot.sh -t $(ROOT_TEST_IMG) $(SYS_DIR) \
                  $(MKPASSWD) $(LIBC_SODIR)

root-image: $(ROOT_IMG)

.PHONY: root-image
