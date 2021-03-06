EXTRA_DIST = iso2022.txt


# The library

pkginclude_HEADERS = console.h controller.h vte.h vtetypebuiltins.h vteversion.h


lib_LTLIBRARIES = libconsole.la

EXTRA_libconsole_la_SOURCES = \
	genkeysyms.py \
	keysyms.c \
	marshal.list \
	mkunitables.sh \
	unitable.CNS11643 \
	unitable.CP437 \
	unitable.GB12345 \
	unitable.GB2312 \
	unitable.JIS0201 \
	unitable.JIS0208 \
	unitable.JIS0212 \
	unitable.KSX1001 \
	vteseq-2.gperf \
	vteseq-2.c \
	vteseq-n.gperf \
	vteseq-n.c \
	vtetypebuiltins.c.template \
	vtetypebuiltins.h.template \
	$(NULL)

libconsole_la_SOURCES = \
	buffer.c \
	buffer.h \
	caps.c \
	caps.h \
	console.c \
	console.h \
	controller.c \
	controller.h \
	debug.c \
	debug.h \
	iso2022.c \
	iso2022.h \
	keymap.c \
	keymap.h \
	marshal.c \
	marshal.h \
	matcher.c \
	matcher.h \
	ring.c \
	ring.h \
	table.c \
	table.h \
	trie.c \
	trie.h \
	vte.c \
	vte.h \
	vte-private.h \
	vteaccess.c \
	vteaccess.h \
	vtebg.c \
	vtebg.h \
	vteconv.c \
	vteconv.h \
	vtedraw.c \
	vtedraw.h \
	vtefc.c \
	vtefc.h \
	vteft2.c \
	vteft2.h \
	vtegl.c \
	vtegl.h \
	vteglyph.c \
	vteglyph.h \
	vteint.h \
	vtepango.c \
	vtepango.h \
	vterdb.c \
	vterdb.h \
	vteregex.c \
	vteregex.h \
	vtergb.c \
	vtergb.h \
	vteseq.c \
	vteseq-list.h \
	vteskel.c \
	vteskel.h \
	vtetree.c \
	vtetree.h \
	vtetypebuiltins.c \
	vtetypebuiltins.h \
	vteversion.h
	$(NULL)

if BUILD_XFT
libconsole_la_SOURCES += \
	vtexft.c \
	vtexft.h
endif

libconsole_la_CPPFLAGS = \
	-DG_DISABLE_ASSERT \
	-DG_LOG_DOMAIN=\"Vte\" \
 	-DDATADIR='"$(datadir)"' \
	-DLIBEXECDIR='"$(libexecdir)"' \
	-DLOCALEDIR='"$(localedir)"' \
	-DVTE_COMPILATION
libconsole_la_CFLAGS = $(CONSOLE_CFLAGS)
libconsole_la_LIBADD = $(CONSOLE_LIBS)
libconsole_la_LDFLAGS = \
	-version-info $(LIBCONSOLE_LTVERSION) \
	-export-symbols-regex "^console_console_.*" \
	@LIBTOOL_EXPORT_OPTIONS@ @LIBTOOL_FLAGS@


# Generated sources

VTESEQ_BUILTSOURCES = vteseq-list.h vteseq-2.c vteseq-n.c
BUILT_SOURCES = marshal.c marshal.h vtetypebuiltins.c vtetypebuiltins.h $(VTESEQ_BUILTSOURCES)
MAINTAINERCLEANFILES = $(VTESEQ_BUILTSOURCES)
EXTRA_DIST += $(VTESEQ_BUILTSOURCES)
CLEANFILES = marshal.c marshal.h vtetypebuiltins.c vtetypebuiltins.h stamp-vtetypebuiltins.h

marshal.c marshal.h: marshal.list
	$(GLIB_GENMARSHAL) --prefix=_vte_marshal --header --body --internal $^ > marshal.c
	$(GLIB_GENMARSHAL) --prefix=_vte_marshal --header --internal $^ > marshal.h

vtetypebuiltins.h: stamp-vtetypebuiltins.h
	@true
stamp-vtetypebuiltins.h: vtetypebuiltins.h.template vte.h
	$(GLIB_MKENUMS) --template $< $(filter-out $<,$^) > xgen-vtbh \
	&& (cmp -s xgen-vtbh vtetypebuiltins.h || cp xgen-vtbh vtetypebuiltins.h ) \
	&& rm -f xgen-vtbh \
	&& echo timestamp > $(@F)

vtetypebuiltins.c: vtetypebuiltins.c.template vte.h
	$(GLIB_MKENUMS) --template $< $(filter-out $<,$^) > xgen-vtbc \
	&& (cmp -s xgen-vtbc vtetypebuiltins.c || cp xgen-vtbc vtetypebuiltins.c ) \
	&& rm -f xgen-vtbc

vteseq-2.c: vteseq-2.gperf
	@echo creating $@
	$(top_srcdir)/missing --run gperf -m 100 --no-strlen $< > $@.tmp && \
	mv -f $@.tmp $@

vteseq-n.c: vteseq-n.gperf
	@echo creating $@
	$(top_srcdir)/missing --run gperf -m 100 $< > $@.tmp && \
	mv -f $@.tmp $@

vteseq-list.h: vteseq-2.gperf vteseq-n.gperf
	@echo creating $@
	echo '/* Generated file.  Do not edit */' > $@.tmp && \
	cat $^ | grep -v '^#' | grep '\<VTE_SEQUENCE_HANDLER\>'| sed 's/.*, //' | LANG=C sort -u >> $@.tmp && \
	mv -f $@.tmp $@


# Misc unit tests and utilities

pkglib_PROGRAMS = vterdb
pkglib_SCRIPTS = decset osc window
EXTRA_DIST += $(pkglib_SCRIPTS)

TEST_SH = check-doc-syntax.sh
EXTRA_DIST += $(TEST_SH)

check_PROGRAMS = buffer dumpkeys iso2022 reflect-text-view reflect-vte ring mev ssfe table trie xticker vteconv
TESTS = buffer ring table trie $(TEST_SH)

AM_CFLAGS = $(GLIB_CFLAGS) $(GOBJECT_CFLAGS)
LDADD = $(GLIB_LIBS) $(GOBJECT_LIBS)

reflect_text_view_CPPFLAGS = -DUSE_TEXT_VIEW
reflect_text_view_CFLAGS = $(CONSOLE_CFLAGS) $(X_CFLAGS)
reflect_text_view_SOURCES = reflect.c
reflect_text_view_LDADD = $(LIBS) $(CONSOLE_LIBS) $(X_LIBS)

reflect_vte_CPPFLAGS = -DUSE_CONSOLE
reflect_vte_CFLAGS = $(CONSOLE_CFLAGS) $(X_CFLAGS)
reflect_vte_SOURCES = reflect.c
reflect_vte_LDADD = libconsole.la $(LIBS) $(CONSOLE_LIBS) $(X_LIBS)

iso2022_SOURCES = \
	buffer.c \
	buffer.h \
	caps.c \
	caps.h \
	debug.c \
	debug.h \
	iso2022.c \
	iso2022.h \
	matcher.c \
	matcher.h \
	table.c \
	table.h \
	trie.c \
	trie.h \
	vteconv.c \
	vteconv.h \
	vtetree.c \
	vtetree.h
iso2022_CPPFLAGS = -DISO2022_MAIN
iso2022_CFLAGS = $(GTK_CFLAGS)
iso2022_LDADD = $(LIBS) $(GTK_LIBS)

ring_SOURCES = ring.c ring.h debug.c debug.h
ring_CPPFLAGS = -DRING_MAIN

table_SOURCES = \
	buffer.c \
	buffer.h \
	caps.c \
	caps.h \
	debug.c \
	debug.h \
	matcher.c \
	matcher.h \
	table.c \
	table.h \
	trie.c \
	trie.h \
	vteconv.c \
	vteconv.h \
table_CPPFLAGS = -DTABLE_MAIN
table_CFLAGS = $(CONSOLE_CFLAGS)
table_LDADD = $(LIBS) $(CONSOLE_LIBS)

trie_SOURCES = \
	trie.c \
	trie.h \
	buffer.c \
	buffer.h \
	caps.c \
	caps.h \
	debug.c \
	debug.h \
	matcher.c \
	matcher.h \
	table.c \
	table.h \
	vteconv.c \
	vteconv.h
trie_CPPFLAGS = -DTRIE_MAIN
trie_CFLAGS = $(CONSOLE_CFLAGS)
trie_LDADD = $(LIBS) $(CONSOLE_LIBS)

vterdb_CFLAGS = $(GTK_CFLAGS) $(X_CFLAGS) -DVTERDB_MAIN
vterdb_SOURCES = vterdb.c
vterdb_LDADD = $(LDFLAGS) $(LIBS) $(GTK_LIBS)

vteconv_SOURCES = buffer.c buffer.h debug.c debug.h vteconv.c vteconv.h
vteconv_CPPFLAGS = -DVTECONV_MAIN
vteconv_CFLAGS = $(CONSOLE_CFLAGS)
vteconv_LDADD = $(LIBS) $(CONSOLE_LIBS)

buffer_SOURCES = buffer.c
buffer_CPPFLAGS = -DBUFFER_MAIN

dumpkeys_SOURCES = dumpkeys.c
mev_SOURCES = mev.c
ssfe_SOURCES = ssfe.c
xticker_SOURCES = xticker.c
