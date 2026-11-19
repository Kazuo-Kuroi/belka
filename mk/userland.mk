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
#   /sbin/mount, umount Belka's (usr/sbin/mount), System V's, over vfstab
#   /sbin/rcorder       Belka's (usr/sbin/rcorder), for /etc/rc and rc.d
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

COMPAT_SRCS     := usr/lib/compat/accounts.c usr/lib/compat/misc.c usr/lib/compat/signame.c \
                   usr/lib/compat/lastlogx.c \
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
                   -DBELKA_SHADOW
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

$(SYS_OBJ)/mount.o: usr/sbin/mount/mount.c $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(OWN_CFLAGS) -c -o $@ $<
$(SYS_DIR)/mount: $(SYS_OBJ)/mount.o $(SYS_DEPS)
	$(call SYS_LINK,$(SYS_OBJ)/mount.o)

$(SYS_OBJ)/rcorder.o: usr/sbin/rcorder/rcorder.c $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(OWN_CFLAGS) -c -o $@ $<
$(SYS_DIR)/rcorder: $(SYS_OBJ)/rcorder.o $(SYS_DEPS)
	$(call SYS_LINK,$(SYS_OBJ)/rcorder.o)

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

# tcsh uses libterminfo's termcap interface. Its custom printf accepts
# %S for short strings, so GCC's standard printf format checks do not apply.
TCSH            := contrib/tcsh
TCSH_PORT       := usr/lib/compat/tcsh
TCSH_OBJ        := $(SYS_OBJ)/tcsh
TCSH_SRCS       := sh sh.dir sh.dol sh.err sh.exec sh.char sh.exp sh.file \
                   sh.func sh.glob sh.hist sh.init sh.lex sh.misc sh.parse \
                   sh.print sh.proc sh.sem sh.set sh.time glob dotlock \
                   mi.termios tw.help tw.init tw.parse tw.spell tw.comp \
                   tw.color ed.chared ed.refresh ed.screen ed.init \
                   ed.inputl ed.defns ed.xmap ed.term tc.alloc tc.bind \
                   tc.const tc.disc tc.func tc.nls tc.os tc.printf \
                   tc.prompt tc.sched tc.sig tc.str tc.vers tc.who
TCSH_OBJS       := $(addprefix $(TCSH_OBJ)/,$(addsuffix .o,$(TCSH_SRCS)) machine.o)
TCSH_CFLAGS     := $(IMPORT_CFLAGS) -I$(TCSH_OBJ) -I$(TCSH_PORT) -I$(TCSH) \
                   -I$(TI) -Iusr/lib/compat/terminfo -Wno-format
TCSH_HDRS       := $(wildcard $(TCSH)/*.h $(TCSH_PORT)/*.h) $(TI_HDRS)
TCSH_GEN        := $(addprefix $(TCSH_OBJ)/,ed.defns.h sh.err.h tc.const.h)

$(TCSH_OBJ)/ed.defns.h: $(TCSH)/ed.defns.c
	@mkdir -p $(dir $@)
	LC_ALL=C awk '/^#define/ && /[FV]_/' $< > $@.tmp && mv $@.tmp $@

$(TCSH_OBJ)/sh.err.h: $(TCSH)/sh.err.c
	@mkdir -p $(dir $@)
	LC_ALL=C awk '/^#define/ && /ERR_/' $< > $@.tmp && mv $@.tmp $@

$(TCSH_OBJ)/tc.const.h: $(TCSH)/tc.const.c $(TCSH_HDRS) $(MLIBC) \
                       $(TCSH_OBJ)/ed.defns.h $(TCSH_OBJ)/sh.err.h
	$(LIBC_CC) $(TCSH_CFLAGS) -E -D_h_tc_const $< > $@.i
	LC_ALL=C sed -n 's/^\(Char STR[a-zA-Z0-9_]*\) *\[ *\].*/extern \1[];/p' \
	    $@.i | LC_ALL=C sort -u > $@.tmp
	test -s $@.tmp && mv $@.tmp $@
	rm -f $@.i

$(TCSH_OBJ)/%.o: $(TCSH)/%.c $(TCSH_HDRS) $(TCSH_GEN)
	$(LIBC_CC) $(TCSH_CFLAGS) -c -o $@ $<

$(TCSH_OBJ)/machine.o: $(TCSH_PORT)/machine.c $(TCSH_HDRS) $(TCSH_GEN)
	$(LIBC_CC) $(TCSH_CFLAGS) -c -o $@ $<

$(SYS_DIR)/tcsh: $(TCSH_OBJS) $(TI_LIB) $(SYS_DEPS)
	$(call SYS_LINK,$(TCSH_OBJS) $(TI_LIB))

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
$(SYS_OBJ)/passwd.o: usr/bin/passwd/passwd.c usr/lib/compat/include/accounts.h $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(OWN_CFLAGS) -c -o $@ $<
$(SYS_DIR)/passwd: $(SYS_OBJ)/passwd.o $(SYS_DYNDEPS)
	$(call SYS_DYNLINK,$<)

# su: NetBSD's, without PAM, login classes, Kerberos or S/Key, and so
# without changing groups (ALLOW_GROUP_CHANGE); the hash from
# /etc/shadow. Only those in group wheel may become root, as in NetBSD.
# Set-user-ID root.
SU_OBJS         := $(SYS_OBJ)/su/su.o $(SYS_OBJ)/su/suutil.o
$(SYS_OBJ)/su/%.o: $(NB)/usr.bin/su/%.c $(NB)/usr.bin/su/suutil.h \
                $(COMPAT_HDRS) $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(IMPORT_CFLAGS) -DBELKA_SHADOW -c -o $@ $<
$(SYS_DIR)/su: $(SU_OBJS) $(SYS_DYNDEPS)
	$(call SYS_DYNLINK,$(SU_OBJS))

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

# Hosted cross builds use the same start files and illumos link-editor as userland.
BELKA_CC_ENV = BELKA_SYSROOT="$(abspath $(SYSROOT))" \
    BELKA_CC_INCLUDE="$(CC_INCLUDE)" BELKA_LD="$(abspath $(ILLUMOS_LD))" \
    BELKA_LD_DRIVER="$(abspath tools/ld/illumos-cc)" \
    BELKA_CRTBEGIN="$(abspath $(PROGRAM_BEGIN))" BELKA_CRTEND="$(abspath $(PROGRAM_END))" \
    BELKA_RTLIB="$(LIBC_RTLIB)"
BELKA_CC = $(abspath tools/ld/belka-cc) $(LIBC_CC)
NATIVE_LD_DIR := $(SYS_DIR)/illumos-ld
NATIVE_LD := $(NATIVE_LD_DIR)/ld
NATIVE_LD_STAMP := $(NATIVE_LD_DIR)/built.stamp
NATIVE_LD_SOURCES := $(shell find contrib/illumos-ld/port contrib/illumos-ld/usr/src -type f)

$(NATIVE_LD_STAMP) $(NATIVE_LD) $(NATIVE_LD_DIR)/ar $(NATIVE_LD_DIR)/nm $(NATIVE_LD_DIR)/sgsmsg &: $(NATIVE_LD_SOURCES) tools/ld/belka-cc tools/ld/illumos-cc $(SYS_DYNDEPS)
	$(BELKA_CC_ENV) $(MAKE) --no-print-directory -C contrib/illumos-ld/port \
	    O=$(abspath $(NATIVE_LD_DIR)) HOSTCC="$(BELKA_CC)" \
	    LDLIBS= \
	    SGSMSG_FOR_BUILD=$(abspath $(ILLUMOS_LD_DIR)/sgsmsg) \
	    $(abspath $(NATIVE_LD_DIR)/ld) $(abspath $(NATIVE_LD_DIR)/sgsmsg) \
	    $(abspath $(NATIVE_LD_DIR)/ar) $(abspath $(NATIVE_LD_DIR)/nm)
	touch $(NATIVE_LD_STAMP)

$(SYS_DIR)/ld: $(NATIVE_LD_STAMP)
	cp $(NATIVE_LD) $@

$(SYS_DIR)/ar $(SYS_DIR)/nm: $(NATIVE_LD_STAMP)
	cp $(NATIVE_LD_DIR)/$(@F) $@

$(SYS_OBJ)/dev%.o: usr/tests/dev%.c $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(OWN_CFLAGS) -c -o $@ $<

$(SYS_DIR)/libl.a: $(SYS_OBJ)/lex/libmain.o $(SYS_OBJ)/lex/libyywrap.o
	$(AR) rcs $@ $^

$(SYS_DIR)/sdk.stamp: contrib/illumos-ld/README.md $(NATIVE_LD_STAMP) $(SYS_DIR)/libl.a \
                    $(SYS_OBJ)/devhello.o $(SYS_OBJ)/devvalue.o $(MLIBC) tools/mksdk.sh usr/share/dev/relink.mk
	sh tools/mksdk.sh $(SYS_DIR)/sdk $(SYSROOT) $(NATIVE_LD_DIR) \
	    $(PROGRAM_BEGIN) $(PROGRAM_END) $(LIBC_RTLIB)
	cp $(SYS_DIR)/libl.a $(SYS_DIR)/sdk/usr/lib64/
	ln -s libl.a $(SYS_DIR)/sdk/usr/lib64/libfl.a
	mkdir -p $(SYS_DIR)/sdk/usr/tests/dev
	cp $(SYS_OBJ)/devhello.o $(SYS_OBJ)/devvalue.o $(SYS_DIR)/sdk/usr/tests/dev/
	cp $(NATIVE_LD_DIR)/libelf/xlate.c $(SYS_DIR)/sdk/usr/tests/dev/xlate.expected
	touch $@

# GNU make is needed by the existing project and GCC build systems.
GMAKE_SRC       := contrib/gmake
GMAKE_OBJ       := $(SYS_DIR)/gmake-build
GMAKE_SOURCES   := $(shell find $(GMAKE_SRC)/src $(GMAKE_SRC)/lib $(GMAKE_SRC)/build-aux -type f)

$(GMAKE_OBJ)/Makefile: $(GMAKE_SRC)/configure $(GMAKE_SRC)/build-aux/config.sub \
                      tools/ld/belka-cc $(SYS_DYNDEPS)
	@mkdir -p $(GMAKE_OBJ)
	cd $(GMAKE_OBJ) && $(BELKA_CC_ENV) \
	    ac_cv_func_posix_spawn=no ac_cv_func_posix_spawnattr_setsigmask=no \
	    CC="$(BELKA_CC)" CFLAGS="-O2 -g" \
	    $(abspath $(GMAKE_SRC))/configure --build=$$($(HOSTCC) -dumpmachine) \
	    --host=x86_64-unknown-belka --prefix=/usr --disable-nls --without-guile

$(SYS_DIR)/gmake: $(GMAKE_OBJ)/Makefile $(GMAKE_SOURCES)
	$(BELKA_CC_ENV) $(MAKE) --no-print-directory -C $(GMAKE_OBJ) all
	cp $(GMAKE_OBJ)/make $@

# NetBSD make, installed as bmake.
BMAKE_SRCS      := arch buf compat cond dir for hash job lst main make \
                   make_malloc metachar parse str suff targ trace var util
BMAKE_OBJS      := $(addprefix $(SYS_OBJ)/bmake/,$(addsuffix .o,$(BMAKE_SRCS)))

$(SYS_OBJ)/bmake/%.o: $(NB)/usr.bin/make/%.c $(wildcard $(NB)/usr.bin/make/*.h usr/lib/compat/bmake/*.h) \
                     $(COMPAT_HDRS) $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(IMPORT_CFLAGS) -DMAKE_NATIVE -DMAKE_MACHINE='"amd64"' \
	    -DMAKE_MACHINE_ARCH='"amd64"' -c -o $@ $<

$(SYS_DIR)/bmake: $(BMAKE_OBJS) $(SYS_DYNDEPS)
	$(call SYS_DYNLINK,$(BMAKE_OBJS))

$(SYS_OBJ)/tsort.o: $(NB)/usr.bin/tsort/tsort.c $(COMPAT_HDRS) $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(IMPORT_CFLAGS) -c -o $@ $<

$(SYS_DIR)/tsort: $(SYS_OBJ)/tsort.o $(SYS_DYNDEPS)
	$(call SYS_DYNLINK,$<)

# Berkeley yacc, with its distributed parser skeleton.
YACC            := $(NB)/external/bsd/byacc/dist
YACC_SRCS       := closure error graph lalr lr0 main mkpar mstring output \
                   reader yaccpar symtab verbose warshall
YACC_OBJS       := $(addprefix $(SYS_OBJ)/yacc/,$(addsuffix .o,$(YACC_SRCS)))

$(SYS_OBJ)/yacc/%.o: $(YACC)/%.c $(wildcard $(YACC)/*.h usr/lib/compat/yacc/*.h) \
                    $(COMPAT_HDRS) $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(IMPORT_CFLAGS) -DHAVE_CONFIG_H -Iusr/lib/compat/yacc \
	    -I$(YACC) -c -o $@ $<

$(SYS_DIR)/yacc: $(YACC_OBJS) $(SYS_DYNDEPS)
	$(call SYS_DYNLINK,$(YACC_OBJS))

# Bootstrap generators run on the build host. Their outputs are target-neutral C.
HOST_YACC       := build/tools/yacc/yacc
$(HOST_YACC): $(addprefix $(YACC)/,$(addsuffix .c,$(YACC_SRCS))) \
              $(wildcard $(YACC)/*.h usr/lib/compat/yacc/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu99 -O2 -D_GNU_SOURCE -DHAVE_CONFIG_H \
	    -D'__RCSID(x)=' -D'__dead=__attribute__((__noreturn__))' \
	    -Iusr/lib/compat/yacc -I$(YACC) -o $@ $(filter %.c,$^)

LEX             := $(NB)/external/bsd/flex/dist/src
LEX_SRCS        := buf ccl dfa ecs filter gen main misc nfa options regex \
                   scanflags scanopt sym tables tables_shared tblcmp yylex \
                   initparse initscan
LEX_OBJS        := $(addprefix $(SYS_OBJ)/lex/,$(addsuffix .o,$(LEX_SRCS))) \
                   $(SYS_OBJ)/lex/skel.o
LEX_GEN         := build/tools/lex/skel.c build/tools/lex/parse.h
LEX_HDRS        := $(wildcard $(LEX)/*.h usr/lib/compat/lex/*.h)

build/tools/lex/parse.h: $(LEX)/initparse.h
	@mkdir -p $(dir $@)
	cp $< $@

build/tools/lex/skel.c: $(LEX)/flex.skl $(LEX)/mkskel.sh $(LEX_HDRS)
	@mkdir -p $(dir $@)
	sh $(LEX)/mkskel.sh $(LEX) m4 2.6.4 > $@.tmp && mv $@.tmp $@

HOST_LEX        := build/tools/lex/lex
$(HOST_LEX): $(addprefix $(LEX)/,$(addsuffix .c,$(LEX_SRCS))) $(LEX_GEN) $(LEX_HDRS)
	$(HOSTCC) -std=gnu99 -O2 -D_GNU_SOURCE -DHAVE_CONFIG_H \
	    -D'__RCSID(x)=' -Iusr/lib/compat/lex -Ibuild/tools/lex -I$(LEX) \
	    -o $@ $(filter %.c,$^) -lm

$(SYS_OBJ)/lex/%.o: $(LEX)/%.c $(LEX_GEN) $(LEX_HDRS) $(COMPAT_HDRS) $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(IMPORT_CFLAGS) -DHAVE_CONFIG_H -Iusr/lib/compat/lex \
	    -Ibuild/tools/lex -I$(LEX) -c -o $@ $<

$(SYS_OBJ)/lex/skel.o: build/tools/lex/skel.c $(LEX_HDRS) $(COMPAT_HDRS) $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(IMPORT_CFLAGS) -DHAVE_CONFIG_H -Iusr/lib/compat/lex \
	    -I$(LEX) -c -o $@ $<

$(SYS_DIR)/lex: $(LEX_OBJS) $(SYS_DYNDEPS)
	$(call SYS_DYNLINK,$(LEX_OBJS))

# NetBSD m4 uses the same Berkeley yacc and flex generators as its native build.
M4_SRC          := $(NB)/usr.bin/m4
M4_SRCS         := eval expr look main misc gnum4 trace
M4_HASH_SRCS    := ohash_create_entry ohash_delete ohash_do ohash_entries \
                   ohash_enum ohash_init ohash_interval ohash_lookup_interval \
                   ohash_lookup_memory ohash_qlookup ohash_qlookupi
M4_OBJS         := $(addprefix $(SYS_OBJ)/m4/,$(addsuffix .o,$(M4_SRCS) $(M4_HASH_SRCS) parser tokenizer)) \
                   $(SYS_OBJ)/lex/libyywrap.o
M4_GEN          := $(SYS_OBJ)/m4/parser.c $(SYS_OBJ)/m4/parser.h $(SYS_OBJ)/m4/tokenizer.c
M4_HDRS         := $(wildcard $(M4_SRC)/*.h $(M4_SRC)/lib/*.h)

$(SYS_OBJ)/m4/parser.c $(SYS_OBJ)/m4/parser.h &: $(M4_SRC)/parser.y $(HOST_YACC)
	@mkdir -p $(SYS_OBJ)/m4
	$(HOST_YACC) -d -b $(SYS_OBJ)/m4/parser $<
	mv $(SYS_OBJ)/m4/parser.tab.c $(SYS_OBJ)/m4/parser.c
	mv $(SYS_OBJ)/m4/parser.tab.h $(SYS_OBJ)/m4/parser.h

$(SYS_OBJ)/m4/tokenizer.c: $(M4_SRC)/tokenizer.l $(HOST_LEX)
	@mkdir -p $(dir $@)
	M4=/usr/bin/m4 $(HOST_LEX) -o$@ $<

$(SYS_OBJ)/m4/%.o: $(M4_SRC)/%.c $(M4_GEN) $(M4_HDRS) $(COMPAT_HDRS) $(MLIBC)
	$(LIBC_CC) $(IMPORT_CFLAGS) -DEXTENDED -I$(M4_SRC)/lib -I$(SYS_OBJ)/m4 -I$(M4_SRC) -c -o $@ $<

$(SYS_OBJ)/m4/%.o: $(M4_SRC)/lib/%.c $(M4_HDRS) $(COMPAT_HDRS) $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(IMPORT_CFLAGS) -I$(M4_SRC)/lib -c -o $@ $<

$(SYS_OBJ)/m4/parser.o: $(SYS_OBJ)/m4/parser.c $(M4_HDRS) $(COMPAT_HDRS) $(MLIBC)
	$(LIBC_CC) $(IMPORT_CFLAGS) -DEXTENDED -I$(M4_SRC)/lib -I$(M4_SRC) -c -o $@ $<

$(SYS_OBJ)/m4/tokenizer.o: $(SYS_OBJ)/m4/tokenizer.c $(SYS_OBJ)/m4/parser.h $(M4_HDRS) $(COMPAT_HDRS) $(MLIBC)
	$(LIBC_CC) $(IMPORT_CFLAGS) -DEXTENDED -I$(M4_SRC)/lib -I$(SYS_OBJ)/m4 -I$(M4_SRC) -c -o $@ $<

$(SYS_DIR)/m4: $(M4_OBJS) $(SYS_DYNDEPS)
	$(call SYS_DYNLINK,$(M4_OBJS))

# System V text user/group administration and transaction recovery.
$(SYS_OBJ)/accounts.o: usr/sbin/accounts/accounts.c \
                       usr/lib/compat/include/accounts.h $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(OWN_CFLAGS) -c -o $@ $<

$(SYS_DIR)/accounts: $(SYS_OBJ)/accounts.o $(SYS_DEPS)
	$(call SYS_LINK,$(SYS_OBJ)/accounts.o)

# NetBSD's BSD diff and patch.
DIFF_SRCS       := diff diffdir diffreg xmalloc pr
PATCH_SRCS      := patch pch inp util backupfile mkpath

$(SYS_OBJ)/diff/%.o: $(NB)/usr.bin/diff/%.c $(wildcard $(NB)/usr.bin/diff/*.h) \
                    $(COMPAT_HDRS) $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(IMPORT_CFLAGS) -c -o $@ $<

$(SYS_OBJ)/patch/%.o: $(NB)/usr.bin/patch/%.c $(wildcard $(NB)/usr.bin/patch/*.h) \
                     $(COMPAT_HDRS) $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(IMPORT_CFLAGS) -c -o $@ $<

$(SYS_DIR)/diff: $(addprefix $(SYS_OBJ)/diff/,$(addsuffix .o,$(DIFF_SRCS))) $(SYS_DYNDEPS)
	$(call SYS_DYNLINK,$(filter $(SYS_OBJ)/diff/%.o,$^))

$(SYS_DIR)/patch: $(addprefix $(SYS_OBJ)/patch/,$(addsuffix .o,$(PATCH_SRCS))) $(SYS_DYNDEPS)
	$(call SYS_DYNLINK,$(filter $(SYS_OBJ)/patch/%.o,$^))

# NetBSD pax, with its tar and cpio command-line interfaces.
PAX_SRCS        := ar_io ar_subs buf_subs file_subs ftree gen_subs getoldopt \
                   options pat_rep pax sel_subs tables tar tty_subs cpio
PAX_OBJS        := $(addprefix $(SYS_OBJ)/pax/,$(addsuffix .o,$(PAX_SRCS)))

$(SYS_OBJ)/pax/%.o: $(NB)/bin/pax/%.c $(wildcard $(NB)/bin/pax/*.h) \
                   $(wildcard usr/lib/compat/pax/*.h) $(COMPAT_HDRS) $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(IMPORT_CFLAGS) -DSMALL -DHAVE_NBTOOL_CONFIG_H=1 \
	    -DHAVE_LUTIMES=1 -Iusr/lib/compat/pax \
	    -include usr/lib/compat/pax/config.h -c -o $@ $<

$(SYS_DIR)/pax: $(PAX_OBJS) $(SYS_OBJ)/usr/lib/compat/pax/compat.o $(SYS_DYNDEPS)
	$(call SYS_DYNLINK,$(PAX_OBJS) $(SYS_OBJ)/usr/lib/compat/pax/compat.o)

# zlib is mandoc's dependency for compressed manual pages.
ZLIB            := contrib/zlib
ZLIB_SRCS       := adler32 crc32 deflate infback inffast inflate inftrees \
                   trees zutil compress uncompr gzclose gzlib gzread gzwrite
ZLIB_OBJS       := $(addprefix $(SYS_OBJ)/zlib/,$(addsuffix .o,$(ZLIB_SRCS)))
ZLIB_LIB        := $(SYS_DIR)/libz.a

$(SYS_OBJ)/zlib/%.o: $(ZLIB)/%.c $(wildcard $(ZLIB)/*.h) $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(IMPORT_CFLAGS) -I$(ZLIB) -c -o $@ $<

$(ZLIB_LIB): $(ZLIB_OBJS)
	rm -f $@
	ar rcs $@ $^

# NetBSD gzip, using the existing zlib and legacy Unix decoders.
GZIP            := $(NB)/usr.bin/gzip
$(SYS_OBJ)/gzip.o: $(GZIP)/gzip.c $(wildcard $(GZIP)/*.c) \
                   $(wildcard $(ZLIB)/*.h) $(COMPAT_HDRS) $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(IMPORT_CFLAGS) -I$(ZLIB) -DNO_BZIP2_SUPPORT \
	    -DNO_XZ_SUPPORT -DNO_LZ_SUPPORT -DNO_COMPRESS_SUPPORT -DBELKA_GZIP -c -o $@ $<

$(SYS_DIR)/gzip: $(SYS_OBJ)/gzip.o $(ZLIB_LIB) $(SYS_DYNDEPS)
	$(call SYS_DYNLINK,$< $(ZLIB_LIB))

# mandoc's database and formatting commands share one executable.
MANDOC          := contrib/mandoc
MANDOC_SRCS     := man man_macro man_validate att lib mdoc mdoc_argv \
                   mdoc_macro mdoc_state mdoc_validate st eqn roff roff_validate \
                   tbl tbl_data tbl_layout tbl_opts arch chars mandoc mandoc_aux \
                   mandoc_msg mandoc_ohash mandoc_xr msec preconv read tag \
                   eqn_html html man_html mdoc_html roff_html tbl_html \
                   eqn_term man_term mdoc_term roff_term term term_ascii term_ps \
                   term_tab term_tag tbl_term dbm dbm_map mansearch dba \
                   dba_array dba_read dba_write mandocdb main manpath mdoc_man \
                   mdoc_markdown out tree compat_ohash compat_reallocarray \
                   compat_recallocarray
MANDOC_OBJS     := $(addprefix $(SYS_OBJ)/mandoc/,$(addsuffix .o,$(MANDOC_SRCS)))
MANDOC_HDRS     := $(wildcard $(MANDOC)/*.h $(MANDOC)/*.in \
                   usr/lib/compat/mandoc/*.h)

$(SYS_OBJ)/mandoc/%.o: $(MANDOC)/%.c $(MANDOC_HDRS) $(COMPAT_HDRS) $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(IMPORT_CFLAGS) -Iusr/lib/compat/mandoc -I$(MANDOC) \
	    -I$(ZLIB) -c -o $@ $<

$(SYS_DIR)/mandoc: $(MANDOC_OBJS) $(SYS_OBJ)/usr/lib/compat/mandoc/output.o \
                     $(ZLIB_LIB) $(SYS_DYNDEPS)
	$(call SYS_DYNLINK,$(MANDOC_OBJS) $(SYS_OBJ)/usr/lib/compat/mandoc/output.o $(ZLIB_LIB))

# less also provides its POSIX more interface through argv[0].
LESS            := $(NB)/external/bsd/less/dist
LESS_SRCS       := main screen brac ch charset cmdbuf command cvt decode edit \
                   filename forwback help ifile input jump line linenum lsystem \
                   mark optfunc option opttbl os output pattern position prompt \
                   search signal tags ttyin version
LESS_OBJS       := $(addprefix $(SYS_OBJ)/less/,$(addsuffix .o,$(LESS_SRCS)))
LESS_HDRS       := $(wildcard $(LESS)/*.h usr/lib/compat/less/*.h)

$(SYS_OBJ)/less/%.o: $(LESS)/%.c $(LESS_HDRS) $(TI_HDRS) $(COMPAT_HDRS) $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(IMPORT_CFLAGS) -Iusr/lib/compat/less -I$(LESS) -I$(TI) \
	    -Iusr/lib/compat/terminfo -DBINDIR='"/usr/bin"' -DSYSDIR='"/etc"' \
	    -c -o $@ $<

$(SYS_DIR)/less: $(LESS_OBJS) $(TI_LIB) $(SYS_DYNDEPS)
	$(call SYS_DYNLINK,$(LESS_OBJS) $(TI_LIB))

# chsh edits the System V passwd file using the shared password-file lock.
$(SYS_OBJ)/chsh.o: usr/bin/chsh/chsh.c usr/lib/compat/include/accounts.h $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(OWN_CFLAGS) -c -o $@ $<

$(SYS_DIR)/chsh: $(SYS_OBJ)/chsh.o $(SYS_DEPS)
	$(call SYS_LINK,$(SYS_OBJ)/chsh.o)

# stty: NetBSD's terminal settings utility.
STTY_SRCS       := cchar gfmt key modes print stty
STTY_OBJS       := $(addprefix $(SYS_OBJ)/stty/,$(addsuffix .o,$(STTY_SRCS))) \
                   $(SYS_OBJ)/usr/lib/compat/stty/speed.o

$(SYS_OBJ)/stty/%.o: $(NB)/bin/stty/%.c $(wildcard $(NB)/bin/stty/*.h) \
                    $(COMPAT_HDRS) $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(IMPORT_CFLAGS) -c -o $@ $<

$(SYS_DIR)/stty: $(STTY_OBJS) $(SYS_DYNDEPS)
	$(call SYS_DYNLINK,$(STTY_OBJS))

# hostname: NetBSD's, over sethostname (sysinfo, System V's), in /bin.
$(SYS_OBJ)/hostname.o: $(NB)/bin/hostname/hostname.c $(COMPAT_HDRS) $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(IMPORT_CFLAGS) -c -o $@ $<
$(SYS_DIR)/hostname: $(SYS_OBJ)/hostname.o $(SYS_DYNDEPS)
	$(call SYS_DYNLINK,$(SYS_OBJ)/hostname.o)
# stat as NetBSD builds it for the host: no st_flags, st_gen or birth time.
TOOL_XFLAGS     :=
$(SYS_OBJ)/stat/stat.o: TOOL_XFLAGS := -DHAVE_NBTOOL_CONFIG_H=1
$(SYS_OBJ)/find/function.o: TOOL_XFLAGS := -DBELKA_ALL_LOCAL

TEXT_PROGS      := $(addprefix $(SYS_DIR)/,sed grep awk vi $(FILE_TOOLS) \
                   hostname stty chsh less mandoc pax gzip diff patch \
                   bmake tsort yacc lex m4 gmake ld ar nm samu muon)

TERM_PROGS      := $(addprefix $(SYS_DIR)/,tic infocmp tput ed) \
                   $(SYS_DIR)/passwd $(SYS_DIR)/su $(TEXT_PROGS)

SYS_PROGS       := $(addprefix $(SYS_DIR)/,init login getty reboot mount rcorder \
                   mksh tcsh accounts)

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
ROOT_DEPS       := $(wildcard build/native-compiler-sdk/*.stamp \
                   build/native-libc-sdk/*.stamp build/native-object-sdk/*.stamp \
                   build/native-gcc-source-sdk/*.stamp) $(SYS_PROGS) $(SYS_DIR)/ps \
                   $(TERM_PROGS) $(XN_BINS) \
	           $(SYS_DIR)/sdk.stamp usr/share/mk/sys.mk usr/bin/ranlib/ranlib.sh \
                   contrib/mlibc/LICENSE \
                   $(wildcard contrib/compiler-runtime-licenses/*/*) \
                   $(wildcard contrib/netbsd/usr.bin/make/NOTICE \
                   contrib/netbsd/usr.bin/make/*.1 contrib/netbsd/usr.bin/m4/NOTICE \
                   contrib/netbsd/usr.bin/m4/*.1 contrib/netbsd/usr.bin/tsort/NOTICE \
                   contrib/netbsd/usr.bin/tsort/*.1 contrib/gmake/COPYING \
                   contrib/gmake/doc/make.1 contrib/gmake/build-aux/install-sh \
                   contrib/netbsd/external/bsd/byacc/dist/LICENSE \
                   contrib/netbsd/external/bsd/byacc/dist/NOTICE \
                   contrib/netbsd/external/bsd/byacc/dist/yacc.1 \
                   contrib/netbsd/external/bsd/flex/dist/COPYING \
                   contrib/netbsd/external/bsd/flex/dist/COPYING.LIB \
                   contrib/netbsd/external/bsd/flex/dist/NOTICE \
                   contrib/netbsd/external/bsd/flex/dist/doc/flex.1 \
                   contrib/illumos-ld/usr/src/man/man1/*.1) \
                   $(LIBC_SOS) usr/bin/clear/clear.sh \
                   contrib/netbsd/share/terminfo/terminfo \
                   contrib/tcsh/Copyright contrib/tcsh/NOTICE \
                   contrib/netbsd/bin/stty/NOTICE tools/mkroot.sh \
                   $(wildcard contrib/mandoc/*.[1578] contrib/mandoc/NOTICE \
                   contrib/netbsd/bin/pax/*.1 contrib/netbsd/bin/pax/NOTICE \
                   contrib/netbsd/usr.bin/gzip/gzip.1 contrib/netbsd/usr.bin/gzip/NOTICE \
                   contrib/netbsd/usr.bin/diff/diff.1 contrib/netbsd/usr.bin/diff/NOTICE \
                   contrib/netbsd/usr.bin/patch/patch.1 contrib/netbsd/usr.bin/patch/NOTICE \
                   contrib/netbsd/external/bsd/less/dist/LICENSE \
                   contrib/netbsd/external/bsd/less/dist/less.1 \
                   contrib/zlib/README usr/bin/chsh/chsh.1 usr/sbin/accounts/accounts.8) \
                   $(wildcard etc/* etc/defaults/* etc/rc.d/* \
                   contrib/netbsd/etc/rc.subr contrib/netbsd/etc/rc.d/*)

$(ROOT_IMG): $(ROOT_DEPS)
	sh tools/mkroot.sh $@ $(SYS_DIR) $(LIBC_SODIR)

MKROOT_TEST     = sh tools/mkroot.sh -t $(ROOT_TEST_IMG) $(SYS_DIR) $(LIBC_SODIR)
# The boot test's second disk (vd1), blank, for mount: made afresh.
DATA_TEST_IMG   := build/data-test-$(TOOLCHAIN)$(LDSUFFIX).img
MKDATA_TEST     = rm -f $(DATA_TEST_IMG) && $(MKE2FS) -q -F -t ext2 -b 1024 \
                  -L belkadata $(DATA_TEST_IMG) 8M

root-image: $(ROOT_IMG)

.PHONY: root-image

$(SYS_OBJ)/exectest.o: usr/tests/exectest.c $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(OWN_CFLAGS) -c -o $@ $<

$(SYS_DIR)/exectest: $(SYS_OBJ)/exectest.o $(SYS_DYNDEPS)
	$(call SYS_DYNLINK,$<)
