# This is used in PANEL_CFLAGS
cappletname = updates

AM_CPPFLAGS = 						\
	$(PANEL_CFLAGS)					\
	$(UPDATES_PANEL_CFLAGS)				\
	-DGNOMELOCALEDIR="\"$(datadir)/locale\""	\
	$(NULL)

BUILT_SOURCES =			\
	cc-updates-resources.c	\
	cc-updates-resources.h

noinst_LTLIBRARIES = libupdates.la

nodist_libupdates_la_SOURCES =	\
	$(BUILT_SOURCES)

libupdates_la_SOURCES =	\
	cc-tariff-editor.c		\
	cc-tariff-editor.h		\
	cc-updates-panel.c		\
	cc-updates-panel.h

libupdates_la_LIBADD = $(PANEL_LIBS) $(UPDATES_PANEL_LIBS)

resource_files = $(shell glib-compile-resources --sourcedir=$(srcdir) --generate-dependencies $(srcdir)/updates.gresource.xml)
cc-updates-resources.c: updates.gresource.xml $(resource_files)
	$(AM_V_GEN) glib-compile-resources --target=$@ --sourcedir=$(srcdir) --generate-source --c-name cc_updates $<
cc-updates-resources.h: updates.gresource.xml $(resource_files)
	$(AM_V_GEN) glib-compile-resources --target=$@ --sourcedir=$(srcdir) --generate-header --c-name cc_updates $<

@INTLTOOL_DESKTOP_RULE@

desktopdir = $(datadir)/applications
desktop_in_files = gnome-updates-panel.desktop.in
desktop_DATA = $(desktop_in_files:.desktop.in=.desktop)

CLEANFILES = $(desktop_in_files) $(desktop_DATA) $(BUILT_SOURCES)
EXTRA_DIST = $(resource_files) updates.gresource.xml

-include $(top_srcdir)/git.mk
