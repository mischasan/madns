# INPUTS
#   $(all)	: list of module names that want standard "clean".
#   $BLD	: empty, or one of:   debug  profile
#   $(<module>.clean): list of non-target dirs/files to clean up.
#   $(<module>.test): list of .pass targets for <module>
# TODO
# Mon Feb 25 2013
# - use all += rather than forcing boilerplate dependencies (all : hx.all etc)?
# - CLEAN UP FreeBSD build on knee

ifndef RULES_MK
RULES_MK:=1# Allow repeated "-include".

PS4             := \# # Prefix for "sh -x" output.
export LD_LIBRARY_PATH PS4

# Import from PREFIX, export to DESTDIR.
PREFIX          ?= /usr/local
DESTDIR         ?= $(PREFIX)
OSNAME          := $(shell uname -s)

CFLAGS.         = -O9

CFLAGS.debug    = -O0 -Wno-uninitialized
CPPFLAGS.debug  = -UNDEBUG

CFLAGS.profile  = -pg -DNDEBUG
LDFLAGS.profile = -pg
# PROFILE tests write stats on syscalls to their .pass files.
exec.profile	= strace -cf

LDLIBS.FreeBSD  = 
LDLIBS.Linux    = -lm -lresolv

# Before gcc 4.5, -Wno-unused-result was unknown and causes an error.
Wno-unused-result := $(shell gcc -dumpversion | awk '$$0 >= 4.5 {print "-Wno-unused-result"}')

# XXX -funsigned-char would save time.
CFLAGS          += -ggdb -MMD -fdiagnostics-show-option -fstack-protector --param ssp-buffer-size=4 -fno-strict-aliasing
CFLAGS          += -Wall -Werror -Wextra -Wcast-align -Wcast-qual -Wno-clobbered -Wformat=2 -Wformat-security -Wmissing-prototypes -Wnested-externs -Wpointer-arith -Wredundant-decls -Wshadow -Wstrict-prototypes -Wno-unknown-pragmas -Wunused $(Wno-unused-result) -Wwrite-strings
CFLAGS          += -Wno-attributes $(CFLAGS.$(BLD))

# -D_FORTIFY_SOURCE=2 on some plats rejects any libc call whose return value is ignored.
#   For some calls (system, write) this makes sense. For others (vasprintf), WTF?

CPPFLAGS        += -mtune=native -I$(PREFIX)/include -D_FORTIFY_SOURCE=2 -D_GNU_SOURCE $(CPPFLAGS.$(BLD))
LDFLAGS         += -L$(PREFIX)/lib $(LDFLAGS.$(BLD))
LDLIBS          += -lm $(LDLIBS.$(OSNAME))

#---------------- Explicitly CANCEL EVIL BUILTIN RULES:
%               : %.c 
%.c             : %.l
%.c             : %.y
%.r             : %.l
#----------------
.PHONY          : all clean cover debug gccdefs install profile source tags test
.DEFAULT_GOAL   := all

# $(all) contains all subproject names. It can be used in ACTIONS but not RULES,
# since it accumulates across "include */GNUmakefile"'s.

# All $(BLD) types use the same pathnames for binaries.
# To switch from release to debug, first "make clean".
# To extract and save exports, "make install DESTDIR=rel".

all             :;@echo "$@ done for BLD='$(BLD)'"
clean           :;@rm -rf $(shell $(MAKE) -nps all test cover profile | sed -n '/^# I/,$${/^[^\#\[%.][^ %]*: /s/:.*//p;}') \
			  $(clean)  $(foreach A,$(all), $($A)/{gmon.out,tags,*.fail,*.gcda,*.gcno,*.gcov,*.prof}) \
                          $(filter %.d,$(MAKEFILE_LIST))

cover           : BLD := cover
%.cover         : %.test    ; gcov -bcp $($@) | covsum

debug           : BLD := debug
debug           : all

# $(call Install,TGTDIR,SRCFILES):
Install         = if [ "$(strip $2)" ]; then mkdir -p $1; pax -rw -pe -s:.*/:: $2 $1; fi

# If you believe in magic vars, e.g. "myutil.bin = prog1 prog2 prog3"
# 	causing "myutil.install" to copy those files to $(DESTDIR)/bin
# 	then here's your automagic "install":
#XXX: install man/man3/hx.3 etc!
%.install       : %.all $(%.bin) $(%.etc) $(%.include) $(%.ini) $(%.lib) $(%.sbin) \
                ;@$(call Install,$(DESTDIR)/bin,    $($*.bin))  \
                ; $(call Install,$(DESTDIR)/etc,    $($*.etc))  \
                ; $(call Install,$(DESTDIR)/ini,    $($*.ini))  \
                ; $(call Install,$(DESTDIR)/lib,    $($*.lib))  \
                ; $(call Install,$(DESTDIR)/sbin,   $($*.sbin)) \
                ; $(call Install,$(DESTDIR)/include,$($*.include))

profile         : BLD := profile
%.profile       : %.test    ;@for x in $($*.test:.pass=); do gprof -b $$x >$$x.prof; done

%.test          : $(%.test)
# GMAKE trims leading "./" from $*, hence $*(*D)/$(*F). Sigh.
%.pass          : %         ; rm -f $@; $(exec.$(BLD)) $(*D)/$(*F) >$*.fail 2>&1 && mv -f $*.fail $@

# To build a .so, "make clean" first, to ensure all .o files compiled with -fPIC
%.so            : CFLAGS := -fPIC $(filter-out $(CFLAGS.cover) $(CFLAGS.profile), $(CFLAGS))
%.so            : %.o       ; $(CC) $(LDFLAGS) -o $@ -shared $< $(LDLIBS)
%.so            : %.a       ; $(CC) $(CFLAGS)  -o $@ -shared -Wl,-whole-archive $< $(LDLIBS)
%.a             :           ; [ "$^" ] && ar crs $@ $(filter %.o,$^)
%.yy.c          : %.l       ; flex -o $@ $<
%.tab.c 	: %.y       ; bison $<
%/..            :           ;@mkdir -p $(@D)
%               : %.gz      ; gunzip -c $^ >$@

# Ensure that intermediate files (e.g. the foo.o caused by "foo : foo.c")
#  are not auto-deleted --- which causes a re-compile every second "make".
.SECONDARY  	: 

#---------------- TOOLS:
# NOTE: "source" MUST be set with "=", not ":=", else MAKE recurses infinitely.
#source          = $(filter-out %.d, $(shell ls $((MAKEFILE_LIST); $(MAKE) -nps all test profile | sed -n '/^. Not a target/{n;/^[^.*][^ ]*:/s/:.*//p;}' | LC_ALL=C sort -u)))
source          = $(filter-out %.d, $(shell $(MAKE) -nps all test profile | sed -n '/^. Not a target/{n;/^[^.*][^ ]*:/s/:.*//p;}' | LC_ALL=C sort -u))
# gccdefs : all gcc internal #defines.
gccdefs         :;@$(CC) $(CPPFLAGS) -E -dM - </dev/null | cut -c8- | sort
# sh : invoke a shell within the makefile's env:
tags            :;@ctags `ls $(source) 2>/dev/null | grep '\.[ch]$$' `

# %.ii lists all (recursive) #included files; e.g.: "make /usr/include/errno.h.ii"
%.ii            : %         ;@ls -1 2>&- `$(CC) $(CPPFLAGS) -M $*` ||:
%.i             : %.c       ; $(COMPILE.c) -E -o $@ $<
%.s             : %.c       ; $(COMPILE.c) -S -o $@ $< && deas $@

# "make SomeVar." prints $(SomeVar)
%.              :;@echo '$($*)'

endif
# vim: set nowrap :
