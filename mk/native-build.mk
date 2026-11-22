# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-19
# SPDX-License-Identifier: MIT
SAMURAI_SRC := contrib/samurai
SAMURAI_NAMES := build deps env graph htab log parse samu scan tool tree util \
        os-posix
SAMURAI_OBJS := $(addprefix $(SYS_OBJ)/samurai/,$(addsuffix .o,$(SAMURAI_NAMES)))

$(SYS_OBJ)/samurai/%.o: $(SAMURAI_SRC)/%.c $(wildcard $(SAMURAI_SRC)/*.h) $(MLIBC)
	@mkdir -p $(dir $@)
	$(LIBC_CC) $(OWN_CFLAGS) -Wno-unused-parameter -D__belka__ -c -o $@ $<

$(SYS_DIR)/samu: $(SAMURAI_OBJS) $(SYS_DYNDEPS)
	$(call SYS_DYNLINK,$(SAMURAI_OBJS))

MUON_SRC := contrib/muon
MUON_NATIVE_BUILD := $(SYS_DIR)/muon-build
MUON_NATIVE_CROSS := $(SYS_DIR)/muon.cross
MUON_NATIVE_OPTS := -Dprefix=/usr -Dbuildtype=release -Dsamurai=disabled \
        -Dlibcurl=disabled -Dlibarchive=disabled -Dlibpkgconf=disabled \
        -Dnative_backtrace=disabled -Ddocs=disabled -Dmeson-docs=disabled \
        -Dmeson-tests=disabled

$(MUON_NATIVE_CROSS): tools/native-build.cross FORCE
	@mkdir -p $(dir $@)
	@sed -e 's|@BELKA_CC@|$(abspath tools/ld/belka-cc)|' \
	    -e 's|@CC@|$(CC)|' $< > $@.new && \
	    { cmp -s $@.new $@ && rm -f $@.new || mv $@.new $@; }

$(MUON_NATIVE_BUILD)/build.ninja: $(MUON_NATIVE_CROSS) $(MUON_SRC)/meson.build \
        $(MUON_SRC)/meson_options.txt tools/ld/belka-cc \
        $(BELKA_CRT_BEGIN) $(BELKA_CRT_END)
	$(BELKA_CC_ENV) $(MUON) -C $(MUON_SRC) setup \
	    -pcross:$(abspath $(MUON_NATIVE_CROSS)) $(MUON_NATIVE_OPTS) \
	    $(abspath $(MUON_NATIVE_BUILD))

$(SYS_DIR)/muon: $(MUON_NATIVE_BUILD)/build.ninja $(SYS_DYNDEPS) FORCE
	$(BELKA_CC_ENV) $(SAMU) -j $(NATIVE_JOBS) -C $(MUON_NATIVE_BUILD) muon
	@cmp -s $(MUON_NATIVE_BUILD)/muon $@ || cp $(MUON_NATIVE_BUILD)/muon $@

# Explicit source packaging; regular images consume it only when staged.
native-libc-kit: $(MLIBC) $(MLIBC_SHARED)
	sh tools/toolchain/package-libc.sh

.PHONY: native-libc-kit
