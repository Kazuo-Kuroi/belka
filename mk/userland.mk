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
#   /usr/bin/sed, grep, awk, vi (ex, view)   NetBSD's, below, vi over
#                       NetBSD's curses
#
# The imported programs build over a small compat library of the BSD
# interfaces mlibc lacks (usr/lib/compat), with NetBSD's getttyent,
# SHA1 crypt, regex, fts and db in it. They are built with warnings,
# but not as errors: their code is kept as it came.
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

# db(3): NetBSD's, all but ndbm.
DB              := $(NB)/lib/libc/db
DB_SRCS         := $(DB)/db/db.c $(DB)/db/dbfile.c $(DB)/mpool/mpool.c \
                   $(addprefix $(DB)/hash/,hash.c hash_bigkey.c hash_buf.c \
                   hash_func.c hash_log2.c hash_page.c) \
                   $(addprefix $(DB)/btree/,bt_close.c bt_conv.c bt_debug.c \
                   bt_delete.c bt_get.c bt_open.c bt_overflow.c bt_page.c \
                   bt_put.c bt_search.c bt_seq.c bt_split.c bt_utils.c) \
                   $(addprefix $(DB)/recno/,rec_close.c rec_delete.c \
                   rec_get.c rec_open.c rec_put.c rec_search.c rec_seq.c \
                   rec_utils.c)

COMPAT_SRCS     := usr/lib/compat/misc.c usr/lib/compat/signame.c \
                   usr/lib/compat/parsedate.c \
                   $(NB)/lib/libc/gen/setmode.c \
                   usr/lib/compat/vis.c usr/lib/compat/crypt.c \
                   usr/lib/compat/crypt_to64.c \
                   $(NB)/lib/libc/gen/getttyent.c \
                   $(NB)/lib/libc/stdio/fparseln.c \
                   $(NB)/lib/libcrypt/crypt-sha1.c \
                   $(NB)/lib/libcrypt/hmac_sha1.c \
                   $(NB)/common/lib/libc/hash/sha1/sha1.c \
                   usr/lib/compat/strtoi.c \
                   $(NB)/lib/libc/cdb/cdbw.c \
                   $(NB)/common/lib/libc/cdb/cdbr.c \
                   $(NB)/common/lib/libc/stdlib/mi_vector_hash.c \
                   $(NB)/lib/libc/stdlib/reallocarr.c \
                   $(NB)/lib/libutil/efun.c \
                   $(NB)/lib/libutil/loginx.c \
                   $(NB)/lib/libutil/logoutx.c \
                   $(NB)/lib/libutil/logwtmpx.c \
                   $(NB)/lib/libc/regex/regcomp.c \
                   $(NB)/lib/libc/regex/regerror.c \
                   $(NB)/lib/libc/regex/regexec.c \
                   $(NB)/lib/libc/regex/regfree.c \
                   $(NB)/lib/libc/regex/regsub.c \
                   $(NB)/lib/libc/gen/fts.c \
                   $(NB)/lib/libc/gen/errc.c $(NB)/lib/libc/gen/verrc.c \
                   $(NB)/lib/libc/gen/warnc.c $(NB)/lib/libc/gen/vwarnc.c \
                   $(DB_SRCS)
COMPAT_OBJS     := $(patsubst %.c,$(SYS_OBJ)/%.o,$(COMPAT_SRCS))
COMPAT_LIB      := $(SYS_DIR)/libcompat.a

COMPAT_HDRS     := $(wildcard usr/lib/compat/include/*.h \
                   usr/lib/compat/include/*/*.h $(NB)/include/regex.h \
                   $(NB)/include/fts.h $(NB)/include/db.h \
                   $(NB)/include/mpool.h)

# Flags of their own: fparseln and fts are built as NetBSD builds its
# host tools (see nbtool_config.h), db with its private interface.
COMPAT_XFLAGS   :=
$(SYS_OBJ)/$(NB)/lib/libc/stdio/fparseln.o \
$(SYS_OBJ)/$(NB)/lib/libc/gen/fts.o: COMPAT_XFLAGS := -DHAVE_NBTOOL_CONFIG_H=1
$(patsubst %.c,$(SYS_OBJ)/%.o,$(DB_SRCS)): COMPAT_XFLAGS := \
                -D__DBINTERFACE_PRIVATE

$(SYS_OBJ)/%.o: %.c $(COMPAT_HDRS) $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(IMPORT_CFLAGS) -I$(NB)/lib/libcrypt $(COMPAT_XFLAGS) \
	    -c -o $@ $<

$(COMPAT_LIB): $(COMPAT_OBJS)
	rm -f $@
	ar rcs $@ $^

# A static program from objects: $(call SYS_LINK,objects).
SYS_LINK        = $(LINK_PROGRAM) $(LIBC_CRT0) $(PROGRAM_BEGIN) $(1) \
                  $(COMPAT_LIB) $(MLIBC) $(LIBC_RTLIB) $(PROGRAM_END) \
                  $(LIBC_CRTN)
SYS_DEPS        := $(COMPAT_LIB) $(MLIBC) $(PROGRAM_BEGIN) $(PROGRAM_END) \
                   $(LD_DEPS)

# init: no chroot or securelevel; utmpx, but not the old utmp. It asks
# for root's password (from /etc/shadow, BELKA_SHADOW) before a
# single-user shell if the console is not marked secure in /etc/ttys.
INIT_OBJS       := $(SYS_OBJ)/init.o
$(SYS_OBJ)/init.o: $(NB)/sbin/init/init.c $(COMPAT_HDRS) $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(IMPORT_CFLAGS) -DALTSHELL -DSECURE -DSUPPORT_UTMPX \
	    -DBELKA_SHADOW -c -o $@ $<

$(SYS_DIR)/init: $(INIT_OBJS) $(SYS_DEPS)
	$(call SYS_LINK,$(INIT_OBJS))

# login: utmpx (not the old utmp); hashes from /etc/shadow; no PAM,
# login classes, Kerberos or S/Key.
LOGIN_CFLAGS    := -DNO_PW_AGING -DSUPPORT_UTMPX -DNO_UT_SS -DUPDWTMPX_VOID \
                   -DNO_LASTLOGX -DBELKA_SHADOW
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
# The boot test (usr/tests/boottest.c), for the test root image only.
$(SYS_OBJ)/boottest.o: usr/tests/boottest.c sys/sys/procfs.h $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(OWN_CFLAGS) -c -o $@ $<
$(SYS_DIR)/boottest: $(SYS_OBJ)/boottest.o $(SYS_DEPS)
	$(call SYS_LINK,$(SYS_OBJ)/boottest.o)

# terminfo (NetBSD's libterminfo, contrib/netbsd/lib/libterminfo), with
# the files NetBSD generates made Belka's way: hash.c by
# tools/terminfo-hash.awk, termcap_hash.c and compiled_terms.c (no
# built-in descriptions: the database is compiled on Belka, by /etc/rc)
# in usr/lib/compat/terminfo. tic, infocmp and tput are linked
# dynamically; the library is static.
TI              := $(NB)/lib/libterminfo
TI_OBJ          := $(SYS_OBJ)/terminfo
TI_CFLAGS       := $(IMPORT_CFLAGS) -DTERMINFO_COMPILE -DTERMINFO_DB \
                   -DTERMINFO_COMPAT -I$(TI) -I$(TI_OBJ) -I$(NB)/lib/libcurses \
                   -Iusr/lib/compat/terminfo
TI_SRCS         := compile curterm setupterm term termcap ti tparm tputs
TI_LIB          := $(SYS_DIR)/libterminfo.a
TI_HDRS         := $(wildcard $(TI)/*.h usr/lib/compat/terminfo/*)

$(TI_OBJ)/hash.c: $(TI)/term.h tools/terminfo-hash.awk
	@mkdir -p $(dir $@)
	LC_ALL=C awk -f tools/terminfo-hash.awk $(TI)/term.h > $@.tmp && \
	    mv $@.tmp $@

$(TI_OBJ)/hash.o: $(TI_OBJ)/hash.c $(TI_HDRS) $(COMPAT_HDRS) $(MLIBC)
	$(LIBC_CC) $(TI_CFLAGS) -c -o $@ $<

$(TI_OBJ)/%.o: $(TI)/%.c $(TI_HDRS) $(COMPAT_HDRS) $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(TI_CFLAGS) -c -o $@ $<

$(TI_LIB): $(patsubst %,$(TI_OBJ)/%.o,$(TI_SRCS) hash)
	rm -f $@
	ar rcs $@ $^

# curses: NetBSD's libcurses over libterminfo, with wide characters,
# static. Its fileio.h, which NetBSD makes from shlib_version, and the
# instr and inchstr families, are Belka's (usr/lib/compat/curses).
CU              := $(NB)/lib/libcurses
CU_OBJ          := $(SYS_OBJ)/curses
CU_CFLAGS       := $(IMPORT_CFLAGS) -DHAVE_WCHAR -DBELKA_BYTE_PUTWC \
                   -I$(CU) -I$(TI) \
                   -Iusr/lib/compat/curses -Iusr/lib/compat/terminfo
CU_SRCS         := acs addbytes addch addchnstr addnstr attributes \
                   background bell border box chgat clear clearok clrtobot \
                   clrtoeol color copywin cr_put ctrace cur_hash curs_set \
                   curses delch deleteln delwin echochar erase fileio \
                   flushok fullname getch getstr getyx id_subwins idlok \
                   idcok immedok inch initscr insch insdelln insertln \
                   insstr keypad keyname leaveok line meta mouse move mvwin \
                   newwin nodelay notimeout overlay overwrite pause printw \
                   putchar refresh resize ripoffline scanw screen scroll \
                   scrollok setterm slk standout syncok timeout toucholap \
                   touchwin tstp tty unctrl underscore version \
                   cchar add_wch add_wchstr addwstr echo_wchar ins_wch \
                   ins_wstr get_wch get_wstr in_wch in_wchstr inwstr
CU_LIB          := $(SYS_DIR)/libcurses.a
CU_HDRS         := $(wildcard $(CU)/*.h usr/lib/compat/curses/*.h) $(TI_HDRS)

$(CU_OBJ)/%.o: $(CU)/%.c $(CU_HDRS) $(COMPAT_HDRS) $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(CU_CFLAGS) -c -o $@ $<
$(CU_OBJ)/instr.o: usr/lib/compat/curses/instr.c $(CU_HDRS) $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(CU_CFLAGS) -c -o $@ $<

$(CU_LIB): $(patsubst %,$(CU_OBJ)/%.o,$(CU_SRCS) instr)
	rm -f $@
	ar rcs $@ $^

# A dynamic program over the compat library and libc.so.1.
SYS_DYNLINK     = $(call LINK_DYNAMIC,$(DYN_INTERP),) $(LIBC_CRT0) \
                  $(PROGRAM_BEGIN) $(1) $(COMPAT_LIB) $(SYSLIB)/libc.so \
                  $(LIBC_RTLIB) $(PROGRAM_END) $(LIBC_CRTN)
SYS_DYNDEPS     := $(COMPAT_LIB) $(MLIBC_SHARED) $(PROGRAM_BEGIN) \
                   $(PROGRAM_END) $(LD_DEPS)

$(TI_OBJ)/prog/tic.o: $(NB)/usr.bin/tic/tic.c
$(TI_OBJ)/prog/infocmp.o: $(NB)/usr.bin/infocmp/infocmp.c
$(TI_OBJ)/prog/tput.o: $(NB)/usr.bin/tput/tput.c
$(TI_OBJ)/prog/tic.o $(TI_OBJ)/prog/infocmp.o $(TI_OBJ)/prog/tput.o: \
                $(TI_HDRS) $(COMPAT_HDRS) $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(TI_CFLAGS) -c -o $@ $(filter $(NB)/usr.bin/%,$^)

$(SYS_DIR)/tic $(SYS_DIR)/infocmp $(SYS_DIR)/tput: $(SYS_DIR)/%: \
                $(TI_OBJ)/prog/%.o $(TI_LIB) $(SYS_DYNDEPS)
	$(call SYS_DYNLINK,$< $(TI_LIB))

# ed: NetBSD's, with its old command forms (BACKWARDS) and without DES;
# static, so that it still works when little else does.
ED_SRCS         := buf cbc glbl io main re sub undo
ED_OBJS         := $(patsubst %,$(SYS_OBJ)/ed/%.o,$(ED_SRCS))

$(SYS_OBJ)/ed/%.o: $(NB)/bin/ed/%.c $(NB)/bin/ed/ed.h $(COMPAT_HDRS) \
                $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(IMPORT_CFLAGS) -DBACKWARDS -c -o $@ $<

$(SYS_DIR)/ed: $(ED_OBJS) $(SYS_DEPS)
	$(call SYS_LINK,$(ED_OBJS))

# vi (and ex and view): nvi as NetBSD builds it, with wide characters,
# its own regex, and its lines kept with db(3)'s recno, over curses;
# linked dynamically. Its configuration is NetBSD's, but for what
# usr/lib/compat/nvi/config.h changes. Its generated headers are made
# Belka's way: options_def.h and ex_def.h by tools/nvi-defs.awk, each
# directory's prototypes (the "PUBLIC:" comments) by sed, version.h here.
NVI             := $(NB)/external/bsd/nvi/dist
NVI_OBJ         := $(SYS_OBJ)/nvi
NVI_VERSION     := 1.81.6-2013-11-20nb4
NVI_SRCS        := api cl_bsd cl_funcs cl_main cl_read cl_screen cl_term \
                   conv cut delete ex ex_abbrev ex_append ex_args ex_argv \
                   ex_at ex_bang ex_cd ex_cmd ex_cscope ex_delete \
                   ex_display ex_edit ex_equal ex_file ex_filter ex_global \
                   ex_init ex_join ex_map ex_mark ex_mkexrc ex_move ex_open \
                   ex_perl ex_preserve ex_print ex_put ex_quit ex_read \
                   ex_screen ex_script ex_set ex_shell ex_shift ex_source \
                   ex_stop ex_subst ex_tag ex_tcl ex_txt ex_undo ex_usage \
                   ex_util ex_version ex_visual ex_write ex_yank ex_z exf \
                   getc gs key trace main mark msg nothread options \
                   options_f put recover screen search seq util v_at v_ch \
                   v_cmd v_delete v_event v_ex v_increment v_init v_itxt \
                   v_left v_mark v_match v_paragraph v_put v_redraw \
                   v_replace v_right v_screen v_scroll v_search v_section \
                   v_sentence v_status v_txt v_ulcase v_undo v_util v_word \
                   v_xchar v_yank v_z v_zexit vi vs_line vs_msg vs_refresh \
                   vs_relative vs_smap vs_split \
                   regcomp regerror regexec regfree vi_db1 log1
NVI_DIRS        := cl common ex vi regex
# Each source's directory in the dist.
nvi_src         = $(firstword $(wildcard \
                  $(patsubst %,$(NVI)/%/$(1).c,$(NVI_DIRS))))
NVI_CFILES      := $(foreach s,$(NVI_SRCS),$(call nvi_src,$(s)))
NVI_GEN         := $(addprefix $(NVI_OBJ)/,options_def.h ex_def.h version.h \
                   cl_extern.h common_extern.h ex_extern.h vi_extern.h \
                   ipc_def.h ipc_extern.h ip_extern.h)
NVI_CFLAGS      := -I$(NVI)/regex -Iusr/lib/compat/nvi -I$(NVI_OBJ) \
                   -I$(NVI)/../usr.bin/nvi $(CU_CFLAGS) \
                   -DGTAGS -DIMCTRL -DHAVE_OPENPTY -DHAVE_UTIL_H \
                   -DHAVE_MVWCHGAT -DUSE_WIDECHAR -D__REGEX_PRIVATE \
                   -DUSE_BUNDLED_DB -Wno-pointer-sign
NVI_HDRS        := $(wildcard $(patsubst %,$(NVI)/%/*.h,$(NVI_DIRS)) \
                   $(NVI)/../usr.bin/nvi/*.h usr/lib/compat/nvi/*.h) \
                   $(NVI_GEN) $(CU_HDRS) $(COMPAT_HDRS)

$(NVI_OBJ)/options_def.h: $(NVI)/common/options.c tools/nvi-defs.awk
	@mkdir -p $(dir $@)
	LC_ALL=C awk -v kind=O -f tools/nvi-defs.awk $< > $@.tmp && mv $@.tmp $@
$(NVI_OBJ)/ex_def.h: $(NVI)/ex/ex_cmd.c tools/nvi-defs.awk
	@mkdir -p $(dir $@)
	LC_ALL=C awk -v kind=C -f tools/nvi-defs.awk $< > $@.tmp && mv $@.tmp $@
$(NVI_OBJ)/version.h:
	@mkdir -p $(dir $@)
	echo '#define VI_VERSION "($(NVI_VERSION))"' > $@
$(NVI_OBJ)/ipc_def.h: $(NVI)/ipc/ipc_cmd.c $(NVI)/ip/ip_read.c \
                tools/nvi-defs.awk
	@mkdir -p $(dir $@)
	(LC_ALL=C awk -v kind=SI -f tools/nvi-defs.awk $(NVI)/ipc/ipc_cmd.c && \
	    LC_ALL=C awk -v kind=VI -f tools/nvi-defs.awk $(NVI)/ip/ip_read.c) \
	    > $@.tmp && mv $@.tmp $@
# The ipc and ip directories are not built, but vi uses their headers.
$(NVI_OBJ)/%_extern.h: $(NVI_CFILES) $(wildcard $(NVI)/ipc/*.c $(NVI)/ip/*.c)
	@mkdir -p $(dir $@)
	sed -n 's/^ \* PUBLIC: \(.*\)/\1/p' $(filter $(NVI)/$*/%,$^) > $@.tmp && \
	    mv $@.tmp $@

define nvi_obj
$(NVI_OBJ)/$(1).o: $(call nvi_src,$(1)) $(NVI_HDRS) $(MLIBC)
	@mkdir -p $$(dir $$@)
	$$(LIBC_CC) $$(NVI_CFLAGS) -c -o $$@ $$<
endef
$(foreach s,$(NVI_SRCS),$(eval $(call nvi_obj,$(s))))

$(SYS_DIR)/vi: $(patsubst %,$(NVI_OBJ)/%.o,$(NVI_SRCS)) $(CU_LIB) $(TI_LIB) \
                $(SYS_DYNDEPS)
	$(call SYS_DYNLINK,$(filter $(NVI_OBJ)/%.o,$^) $(CU_LIB) $(TI_LIB))

# passwd: Belka's own (usr/bin/passwd), over the compat library's crypt.
$(SYS_OBJ)/passwd.o: usr/bin/passwd/passwd.c $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(OWN_CFLAGS) -c -o $@ $<
$(SYS_DIR)/passwd: $(SYS_OBJ)/passwd.o $(SYS_DYNDEPS)
	$(call SYS_DYNLINK,$<)

# sed, grep and awk: NetBSD's, linked dynamically. grep has no gzip,
# bzip2 or message catalogues; egrep and fgrep are links to it. awk is
# the One True AWK, with the parser its dist ships (ytab.c, proctab.c),
# so no yacc runs.
SED_SRCS        := compile main misc process
GREP_SRCS       := fastgrep file grep queue util
AWK_SRCS        := b lex lib main parse proctab run tran ytab
AWK_DIST        := $(NB)/external/historical/nawk/dist
TEXT_CFLAGS     := $(IMPORT_CFLAGS) -DWITHOUT_GZIP -DWITHOUT_BZ2 \
                   -DWITHOUT_NLS -DHAS_ISBLANK -I$(AWK_DIST) \
                   -Iusr/lib/compat/awk

TEXT_CC         = @mkdir -p $(dir $@); $(LIBC_CC) $(TEXT_CFLAGS) -c -o $@ $<

$(SYS_OBJ)/sed/%.o: $(NB)/usr.bin/sed/%.c $(wildcard $(NB)/usr.bin/sed/*.h) \
                $(COMPAT_HDRS) $(MLIBC)
	$(TEXT_CC)
$(SYS_OBJ)/grep/%.o: $(NB)/usr.bin/grep/%.c $(NB)/usr.bin/grep/grep.h \
                $(COMPAT_HDRS) $(MLIBC)
	$(TEXT_CC)
$(SYS_OBJ)/awk/%.o: $(AWK_DIST)/%.c $(AWK_DIST)/awk.h $(AWK_DIST)/proto.h \
                $(AWK_DIST)/ytab.h $(COMPAT_HDRS) $(MLIBC)
	$(TEXT_CC)

$(SYS_DIR)/sed: $(patsubst %,$(SYS_OBJ)/sed/%.o,$(SED_SRCS)) $(SYS_DYNDEPS)
	$(call SYS_DYNLINK,$(filter $(SYS_OBJ)/%.o,$^))
$(SYS_DIR)/grep: $(patsubst %,$(SYS_OBJ)/grep/%.o,$(GREP_SRCS)) \
                $(SYS_DYNDEPS)
	$(call SYS_DYNLINK,$(filter $(SYS_OBJ)/%.o,$^))
$(SYS_DIR)/awk: $(patsubst %,$(SYS_OBJ)/awk/%.o,$(AWK_SRCS)) $(SYS_DYNDEPS)
	$(call SYS_DYNLINK,$(filter $(SYS_OBJ)/%.o,$^))

# mktemp, seq, stat (readlink is a link to it), find, xargs, getconf:
# NetBSD's, linked dynamically, each from its directory in usr.bin.
# xargs's pathnames.h is Belka's (usr/lib/compat/xargs).
FILE_TOOLS      := mktemp seq stat find xargs getconf
FIND_SRCS       := find function ls main misc operator option
XARGS_SRCS      := xargs strnsubst
tool_srcs       = $(if $(filter find,$(1)),$(FIND_SRCS),$(if \
                  $(filter xargs,$(1)),$(XARGS_SRCS),$(1)))

define file_tool
$(SYS_OBJ)/$(1)/%.o: $(NB)/usr.bin/$(1)/%.c $(wildcard $(NB)/usr.bin/$(1)/*.h) \
                $(COMPAT_HDRS) $(MLIBC)
	@mkdir -p $$(dir $$@)
	$$(LIBC_CC) $$(IMPORT_CFLAGS) $$(TOOL_XFLAGS) -I$(NB)/usr.bin/$(1) \
	    -Iusr/lib/compat/$(1) -c -o $$@ $$<
$(SYS_DIR)/$(1): $(patsubst %,$(SYS_OBJ)/$(1)/%.o,$(call tool_srcs,$(1))) \
                $(SYS_DYNDEPS)
	$$(call SYS_DYNLINK,$$(filter $(SYS_OBJ)/%.o,$$^))
endef
$(foreach t,$(FILE_TOOLS),$(eval $(call file_tool,$(t))))
# stat as NetBSD builds it for the host: no st_flags, st_gen or birth time.
TOOL_XFLAGS     :=
$(SYS_OBJ)/stat/stat.o: TOOL_XFLAGS := -DHAVE_NBTOOL_CONFIG_H=1
$(SYS_OBJ)/find/function.o: TOOL_XFLAGS := -DBELKA_ALL_LOCAL

TEXT_PROGS      := $(addprefix $(SYS_DIR)/,sed grep awk vi $(FILE_TOOLS))

TERM_PROGS      := $(addprefix $(SYS_DIR)/,tic infocmp tput ed) \
                   $(SYS_DIR)/passwd $(TEXT_PROGS)

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
ROOT_DEPS       := $(SYS_PROGS) $(SYS_DIR)/ps $(TERM_PROGS) $(XN_BINS) \
                   $(LIBC_SOS) usr/bin/clear/clear.sh \
                   contrib/netbsd/share/terminfo/terminfo \
                   tools/mkroot.sh \
                   $(wildcard etc/* etc/defaults/*)

$(ROOT_IMG): $(ROOT_DEPS)
	sh tools/mkroot.sh $@ $(SYS_DIR) $(LIBC_SODIR)

MKROOT_TEST     = sh tools/mkroot.sh -t $(ROOT_TEST_IMG) $(SYS_DIR) $(LIBC_SODIR)

root-image: $(ROOT_IMG)

.PHONY: root-image
