# Copyright (C) 2000 Werner Koch (dd9jn)
# Copyright (C) 2001, 2004, 2005, 2009 g10 Code GmbH
# 
# This file is part of GPGME.
# 
# GPGME is free software; you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License as
# published by the Free Software Foundation; either version 2.1 of the
# License, or (at your option) any later version.
# 
# GPGME is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General
# Public License for more details.
# 
# You should have received a copy of the GNU Lesser General Public
# License along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA

## Process this file with automake to produce Makefile.in

GPG = @GPG@

TESTS_ENVIRONMENT = GNUPGHOME=$(abs_builddir) GPG_AGENT_INFO=

# The keylist tests must come after the import and the edit test.
noinst_HEADERS = t-support.h

if HAVE_W32_SYSTEM
tests_unix =
else
tests_unix = t-eventloop t-thread1
endif

TESTS = t-encrypt t-encrypt-sym t-encrypt-sign t-sign t-signers		\
	t-decrypt t-verify t-decrypt-verify t-sig-notation t-export	\
	t-import t-trustlist t-edit t-keylist t-keylist-sig t-wait	\
	t-encrypt-large t-file-name t-gpgconf $(tests_unix)

CLEANFILES = secring.gpg pubring.gpg pubring.kbx trustdb.gpg dirmngr.conf
DISTCLEANFILES = pubring.gpg~ pubring.kbx~ random_seed gpg.conf gpg-agent.conf

EXTRA_DIST = mkdemodirs.in pubdemo.asc secdemo.asc cipher-1.asc cipher-2.asc \
	geheim.txt pubkey-1.asc seckey-1.asc pinentry

INCLUDES = -I$(top_builddir)/src

AM_CPPFLAGS = @GPG_ERROR_CFLAGS@
LDADD = ../../src/libgpgme.la
t_thread1_LDADD = ../../src/libgpgme-pthread.la

# We don't run t-genkey in the test suite, because it takes too long
noinst_PROGRAMS = $(TESTS) t-genkey

mkdemodirs: mkdemodirs.in Makefile
	sed -e 's,[@]GPG[@],$(GPG),g' < $(srcdir)/mkdemodirs.in > mkdemodirs
	chmod +x mkdemodirs

clean-local:
	-./mkdemodirs --clean
	-rm -f mkdemodirs

all-local: ./pubring.gpg ./gpg.conf ./gpg-agent.conf

./pubring.gpg: $(srcdir)/pubdemo.asc ./Alpha/Secret.gpg 
	-$(GPG) --homedir . --no-permission-warning \
                --import $(srcdir)/pubdemo.asc
	-$(GPG) --homedir . --no-permission-warning \
		--import Alpha/Secret.gpg Zulu/Secret.gpg

./Alpha/Secret.gpg: mkdemodirs secdemo.asc 
	srcdir=$(srcdir) ./mkdemodirs 

./gpg.conf:
# This is required for t-sig-notations.
	echo no-force-v3-sigs > ./gpg.conf

./gpg-agent.conf:
# This is required for gpg2, which does not support command fd.
	echo pinentry-program $(abs_srcdir)/pinentry > ./gpg-agent.conf
