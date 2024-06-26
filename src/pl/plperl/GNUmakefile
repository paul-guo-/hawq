# Makefile for PL/Perl
# PostgreSQL: pgsql/src/pl/plperl/GNUmakefile

subdir = src/pl/plperl
top_builddir = ../../..
-include $(top_builddir)/src/Makefile.global

ifeq ($(perl_useshrplib),true)
shared_libperl = yes
endif
ifeq ($(perl_useshrplib),yes)
shared_libperl = yes
endif

# If we don't have a shared library and the platform doesn't allow it
# to work without, we have to skip it.
ifneq (,$(findstring yes, $(shared_libperl)$(allow_nonpic_in_shlib)))

ifeq ($(PORTNAME), win32)
perl_archlibexp := $(subst \,/,$(perl_archlibexp))
perl_privlibexp := $(subst \,/,$(perl_privlibexp))
perl_lib := $(basename $(notdir $(wildcard $(perl_archlibexp)/CORE/perl[5-9]*.lib)))
perl_embed_ldflags = -L$(perl_archlibexp)/CORE -l$(perl_lib)
override CPPFLAGS += -DPLPERL_HAVE_UID_GID
# Perl on win32 contains /* within comment all over the header file,
# so disable this warning.
override CFLAGS += -Wno-comment
endif

perl_privlibexp_map := $(perl_privlibexp)
# ifeq ($(PORTNAME), linux)
# perl_privlibexp := $(perl_privlibexp)/vendor_perl
# endif

override CPPFLAGS := -I$(srcdir) $(CPPFLAGS) -I$(perl_archlibexp)/CORE

rpathdir = $(perl_archlibexp)/CORE


NAME = plperl

OBJS = plperl.o SPI.o Util.o

DATA = plperl.control plperl--1.0.sql plperl--unpackaged--1.0.sql \
       plperlu.control plperlu--1.0.sql plperlu--unpackaged--1.0.sql

PERLCHUNKS = plc_perlboot.pl plc_trusted.pl

SHLIB_LINK = $(perl_embed_ldflags)

REGRESS_OPTS = --dbname=$(PL_TESTDB) --load-language=plperl --load-language=plperlu
REGRESS = plperl plperl_trigger plperl_shared plperl_elog plperl_util plperl_init plperlu plperl_array
STRESS = plperl_stress
# if Perl can support two interpreters in one backend,
# test plperl-and-plperlu cases
ifneq ($(PERL),)
ifeq ($(shell $(PERL) -V:usemultiplicity), usemultiplicity='define';)
	REGRESS += plperl_plperlu
endif
endif
# where to find psql for running the tests
PSQLDIR = $(bindir)

# where to find xsubpp for building XS.
XSUBPPDIR = $(shell $(PERL) -e 'use List::Util qw(first); print first { -r "$$_/ExtUtils/xsubpp" } @INC')

include $(top_srcdir)/src/Makefile.shlib

plperl.o: perlchunks.h plperl_opmask.h

plperl_opmask.h: plperl_opmask.pl
	$(PERL) $< $@

perlchunks.h: $(PERLCHUNKS)
	$(PERL) $(srcdir)/text2macro.pl --strip='^(\#.*|\s*)$$' $^ > $@

all: all-lib

SPI.c: SPI.xs
	$(PERL) $(XSUBPPDIR)/ExtUtils/xsubpp -typemap $(perl_privlibexp)/ExtUtils/typemap $< >$@

Util.c: Util.xs
	$(PERL) $(XSUBPPDIR)/ExtUtils/xsubpp -typemap $(perl_privlibexp)/ExtUtils/typemap $< >$@

install: all installdirs install-lib

installdirs: installdirs-lib

uninstall: uninstall-lib

installcheck: submake
	$(top_builddir)/src/test/regress/pg_regress --inputdir=$(srcdir) --psqldir=$(PSQLDIR) $(REGRESS_OPTS) $(REGRESS)

installcheck-stress: submake
	$(top_builddir)/src/test/regress/pg_regress --inputdir=$(srcdir) --psqldir=$(PSQLDIR) $(REGRESS_OPTS) $(STRESS)


.PHONY: submake
submake:
	$(MAKE) -C $(top_builddir)/src/test/regress pg_regress$(X)

clean distclean maintainer-clean: clean-lib
	rm -f SPI.c Util.c $(OBJS) perlchunks.h plperl_opmask.h
	rm -rf results
	rm -f regression.diffs regression.out

else # can't build

all:
	@echo ""; \
	 echo "*** Cannot build PL/Perl because libperl is not a shared library."; \
	 echo "*** You might have to rebuild your Perl installation.  Refer to"; \
	 echo "*** the documentation for details."; \
	 echo ""

endif # can't build
