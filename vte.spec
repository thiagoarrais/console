Name: vte
Version: 0.10.15
Release: 1
Summary: An experimental terminal emulator.
License: LGPL
Group: User Interface/X
BuildRoot: %{_tmppath}/%{name}-root
Source: %{name}-%{version}.tar.gz
BuildPrereq: gtk2-devel, pygtk2-devel, python-devel
Requires: bitmap-fonts

%description
VTE is an experimental terminal emulator widget for use with GTK+ 2.0.

%package devel
Summary: Files needed for developing applications which use vte.
Group: Development/Libraries
Requires: %{name} = %{version}-%{release}, gtk2-devel

%description devel
VTE is an experimental terminal emulator widget for use with GTK+ 2.0.  This
package contains the files needed for building applications using VTE.

%prep
%setup -q

%build
if [ -x %{_bindir}/python2.2 ]; then
	PYTHON=%{_bindir}/python2.2; export PYTHON
fi
%configure --enable-shared --enable-static --libexecdir=%{_libdir}/%{name}
make

%clean
rm -fr $RPM_BUILD_ROOT

%install
rm -fr $RPM_BUILD_ROOT
make install DESTDIR=$RPM_BUILD_ROOT

# Remove the sample app.
rm $RPM_BUILD_ROOT/%{_bindir}/%{name}

# Remove the .la file.
rm $RPM_BUILD_ROOT/%{_libdir}/lib%{name}.la

# Work around AM_PATH_PYTHON from automake 1.6.3 not being multilib-aware.
if test %{_libdir} != %{_prefix}/lib ; then
	badpyexecdir=`ls -d $RPM_BUILD_ROOT/%{_prefix}/lib/python* 2> /dev/null`
	if test -n "$badpyexecdir" ; then
		pyexecdirver=`basename $badpyexecdir`
		install -d -m755 $RPM_BUILD_ROOT/%{_libdir}/${pyexecdirver}
		mv ${badpyexecdir}/site-packages $RPM_BUILD_ROOT/%{_libdir}/${pyexecdirver}/
	fi
fi

# Remove static python modules and la files, which are probably useless to
# Python anyway.
rm -f $RPM_BUILD_ROOT/%{_libdir}/python*/site-packages/*.la
rm -f $RPM_BUILD_ROOT/%{_libdir}/python*/site-packages/*.a

%find_lang %{name}

%post -p /sbin/ldconfig

%postun -p /sbin/ldconfig

%files -f %{name}.lang
%defattr(-,root,root)
%doc ChangeLog COPYING HACKING NEWS README doc/utmpwtmp.txt doc/boxes.txt
%{_libdir}/*.so.*.*
%dir %{_libdir}/%{name}
%attr(2711,root,utmp) %{_libdir}/%{name}/gnome-pty-helper
%{_datadir}/%{name}
%{_libdir}/python*/site-packages/*

%files devel
%defattr(-,root,root)
%{_datadir}/gtk-doc/html/%{name}
%{_includedir}/*
%dir %{_libdir}/%{name}
%{_libdir}/%{name}/decset
%{_libdir}/%{name}/interpret
%{_libdir}/%{name}/iso8859mode
%{_libdir}/%{name}/nativeecho
%{_libdir}/%{name}/osc
%{_libdir}/%{name}/slowcat
%{_libdir}/%{name}/utf8echo
%{_libdir}/%{name}/utf8mode
%{_libdir}/%{name}/window
%{_libdir}/*.a
%{_libdir}/*.so
%{_libdir}/pkgconfig/*

%changelog
* Wed Jan 22 2003 Nalin Dahyabhai <nalin@redhat.com> 0.10.15-1
- make mouse modes mutually-exclusive
- update background immediately on realize
- fix compile error on older versions of gcc
- fix cursor hiding

* Wed Jan 22 2003 Nalin Dahyabhai <nalin@redhat.com> 0.10.14-1
- fix assorted mouse event bugs

* Wed Jan 22 2003 Tim Powers <timp@redhat.com> 0.10.12-2
- rebuilt
 
* Tue Jan 21 2003 Nalin Dahyabhai <nalin@redhat.com> 0.10.13-1
- use less memory when setting up pseudo-transparent backgrounds

* Mon Jan 20 2003 Nalin Dahyabhai <nalin@redhat.com> 0.10.12-1
- fix a few accessibility bugs
- fix colors 90-97,100-107 not bright (GNOME #103713)

* Fri Jan 17 2003 Nalin Dahyabhai <nalin@redhat.com> 0.10.11-1
- fix overzealous clearing when drawing the cursor

* Tue Jan 14 2003 Nalin Dahyabhai <nalin@redhat.com> 0.10.10-1
- add text that scrolls off of a restricted scrolling area which goes to the
  top of the visible screen to the scrollback buffer (#75900)

* Mon Jan 13 2003 Nalin Dahyabhai <nalin@redhat.com> 0.10.9-1
- fix scrolling through the accessibility layer
- stop heeding NumLock when mapping cursor keys
- steal keypress events from the input method if Meta modifier is in effect

* Mon Jan  6 2003 Nalin Dahyabhai <nalin@redhat.com> 0.10.8-1
- report changes to the accessibility layer when text is removed or moved
  around, still needs work
- don't use XftNameUnparse, it might not always be there

* Fri Dec 13 2002 Nalin Dahyabhai <nalin@redhat.com> 0.10.7-2
- rebuild

* Wed Dec 11 2002 Nalin Dahyabhai <nalin@redhat.com> 0.10.7-1
- distinguish line-drawing character set code points from the same code points
  received from the local encoding

* Tue Dec 10 2002 Nalin Dahyabhai <nalin@redhat.com> 0.10.6-1
- handle ambiguous-width line-drawing characters

* Tue Dec 10 2002 Nalin Dahyabhai <nalin@redhat.com> 0.10.5-3
- rebuild

* Mon Dec  9 2002 Nalin Dahyabhai <nalin@redhat.com> 0.10.5-2
- work around AM_PATH_PYTHON not being multilib-aware

* Tue Dec  3 2002 Nalin Dahyabhai <nalin@redhat.com> 0.10.5-1
- cleaned up the keyboard

* Mon Nov 11 2002 Nalin Dahyabhai <nalin@redhat.com> 0.10.4-1
- make selection wrap like XTerm

* Thu Nov  7 2002 Nalin Dahyabhai <nalin@redhat.com> 0.10.3-1
- get selection sorted out, really this time

* Tue Nov  5 2002 Nalin Dahyabhai <nalin@redhat.com> 0.10.2-1
- get selection sorted out

* Tue Oct 29 2002 Nalin Dahyabhai <nalin@redhat.com> 0.10.1-1
- add the ability to remove matching patterns

* Thu Oct 24 2002 Nalin Dahyabhai <nalin@redhat.com> 0.10-1
- allow setting the working directory (#76529)

* Thu Oct 17 2002 Nalin Dahyabhai <nalin@redhat.com> 0.9.2-1
- fix the crash-on-resize bug (#75871)
- add bold
- implement sun/hp/legacy function key modes
- recognize cs with no parameters
- fix ring buffer manipulation bugs
- cut down on overly-frequent invalidates

* Wed Sep 11 2002 Nalin Dahyabhai <nalin@redhat.com> 0.9.1-1
- refresh gnome-pty-helper from libzvt

* Wed Sep 11 2002 Nalin Dahyabhai <nalin@redhat.com> 0.9.0-1
- build fixes from Jacob Berkman
- warning fixes from Brian Cameron
- gnome-pty-helper integration

* Fri Sep  6 2002 Nalin Dahyabhai <nalin@redhat.com> 0.8.20-1
- build fixups from Jacob Berkman
- move the python module into the gtk-2.0 subdirectory, from James Henstridge

* Thu Sep  5 2002 Nalin Dahyabhai <nalin@redhat.com> 0.8.19-1
- possible fix for focusing bugs

* Thu Sep  5 2002 Nalin Dahyabhai <nalin@redhat.com> 0.8.18-1
- fix for worst-case when stripping termcap entries from Brian Cameron
- add docs

* Tue Sep  3 2002 Nalin Dahyabhai <nalin@redhat.com> 0.8.17-1
- track Xft color deallocation to prevent freeing of colors we don't own

* Tue Sep  3 2002 Nalin Dahyabhai <nalin@redhat.com> 0.8.16-1
- handle color allocation failures better

* Mon Sep  2 2002 Nalin Dahyabhai <nalin@redhat.com> 0.8.15-1
- cleanups

* Fri Aug 30 2002 Nalin Dahyabhai <nalin@redhat.com> 0.8.14-1
- get smarter about adjusting our adjustments (#73091)

* Fri Aug 30 2002 Nalin Dahyabhai <nalin@redhat.com> 0.8.13-1
- restore the IM status window by restoring our own focus-in/focus-out
  handlers (#72946)

* Fri Aug 30 2002 Nalin Dahyabhai <nalin@redhat.com> 0.8.12-1
- cleanups

* Thu Aug 29 2002 Nalin Dahyabhai <nalin@redhat.com> 0.8.11-1
- clean up autoscroll (#70481)
- add Korean text examples to docs

* Tue Aug 27 2002 Nalin Dahyabhai <nalin@redhat.com> 0.8.10-1
- autoscroll (#70481)
- only perform cr-lf substitutions when pasting (#72639)
- bind GDK_ISO_Left_Tab to "kB" (part of #70340)

* Tue Aug 27 2002 Nalin Dahyabhai <nalin@redhat.com> 0.8.9-1
- handle forward scrolling again (#73409)

* Tue Aug 27 2002 Nalin Dahyabhai <nalin@redhat.com> 0.8.8-1
- fix crashes on resize

* Mon Aug 26 2002 Nalin Dahyabhai <nalin@redhat.com>
- fix missing spaces on full lines

* Mon Aug 26 2002 Nalin Dahyabhai <nalin@redhat.com> 0.8.7-1
- fix deadlock when substitutions fail

* Mon Aug 26 2002 Nalin Dahyabhai <nalin@redhat.com> 0.8.6-1
- one-liner segfault bug fix

* Sun Aug 25 2002 Nalin Dahyabhai <nalin@redhat.com> 0.8.5-1
- fix reverse video mode, which broke during the rendering rewrite

* Fri Aug 23 2002 Nalin Dahyabhai <nalin@redhat.com> 0.8.4-1
- prevent up/UP/DO from scrolling
- bind shift+insert to "paste PRIMARY", per xterm/kterm/hanterm

* Thu Aug 22 2002 Nalin Dahyabhai <nalin@redhat.com> 0.8.3-1
- track changes to the style's font
- always open fonts right away so that the metric information is correct
- make audible and visual bell independent options

* Wed Aug 21 2002 Nalin Dahyabhai <nalin@redhat.com> 0.8.2-1
- don't perform text substitution on text that is part of a control sequence

* Tue Aug 20 2002 Nalin Dahyabhai <nalin@redhat.com> 0.8.1-1
- dispose of the updated iso2022 context properly when processing incoming text

* Tue Aug 20 2002 Nalin Dahyabhai <nalin@redhat.com> 0.8.0-1
- rework font handling to use just-in-time loading
- handle iso-2022 escape sequences, perhaps as much as they might make sense
  in a Unicode environment

* Wed Aug 14 2002 Nalin Dahyabhai <nalin@redhat.com> 0.7.4-1
- handle massive amounts of invalid data better (the /dev/urandom case)
- munged up patch from Owen to fix language matching
- fix initialization of new rows when deleting lines

* Mon Aug 12 2002 Nalin Dahyabhai <nalin@redhat.com> 0.7.3-1
- more fixes for behavior when not realized
- require bitmap-fonts
- escape a control sequence properly

* Thu Aug  8 2002 Nalin Dahyabhai <nalin@redhat.com> 0.7.2-1
- fix cursor over reversed text
- fix character positioning in Xft1
- add border padding
- fix lack of shift-in when resetting

* Tue Aug  6 2002 Nalin Dahyabhai <nalin@redhat.com> 0.7.1-1
- rework rendering with Pango
- special-case monospaced Xft1 rendering, hopefully making it faster
- modify pasting to use carriage returns instead of linefeeds

* Thu Aug  1 2002 Nalin Dahyabhai <nalin@redhat.com> 0.7.0-1
- rework drawing to minimize round trips to the server

* Tue Jul 30 2002 Nalin Dahyabhai <nalin@redhat.com> 0.6.0-1
- rework parsing to use tables instead of tries
- implement more xterm-specific behaviors

* Thu Jul 25 2002 Nalin Dahyabhai <nalin@redhat.com> 0.5.4-1
- fix default PTY size bug

* Wed Jul 24 2002 Nalin Dahyabhai <nalin@redhat.com> 0.5.3-1
- open PTYs with the proper size (#69606)

* Tue Jul 23 2002 Nalin Dahyabhai <nalin@redhat.com> 0.5.2-1
- fix imbalanced realize/unrealize routines causing crashes (#69605)

* Thu Jul 18 2002 Nalin Dahyabhai <nalin@redhat.com> 0.5.1-1
- fix a couple of scrolling artifacts

* Thu Jul 18 2002 Nalin Dahyabhai <nalin@redhat.com> 0.5.0-1
- use gunichars internally
- scroll regions more effectively
- implement part of set-mode/reset-mode (maybe fixes #69143)
- fix corner case in dingus hiliting (#67930, really this time)

* Thu Jul 18 2002 Jeremy Katz <katzj@redhat.com> 0.4.9-3
- free trip through the build system

* Tue Jul 16 2002 Nalin Dahyabhai <nalin@redhat.com> 0.4.9-2
- build in different environment

* Tue Jul 16 2002 Nalin Dahyabhai <nalin@redhat.com> 0.4.9-1
- check for iconv failures properly and report them more aggressively
- guess at a proper default bold color (#68965)

* Mon Jul 15 2002 Nalin Dahyabhai <nalin@redhat.com>
- cosmetic fixes

* Sat Jul 13 2002 Nalin Dahyabhai <nalin@redhat.com>
- fix segfaulting during dingus highlighting when the buffer contains non-ASCII
  characters (#67930)

* Fri Jul 12 2002 Nalin Dahyabhai <nalin@redhat.com> 0.4.8-1
- implement BCE (#68414)
- bind F13-F35 per termcap

* Thu Jul 11 2002 Nalin Dahyabhai <nalin@redhat.com>
- rework default color selection
- provide a means for apps to just set the foreground/background text colors
- don't scroll-on-keystroke when it's just alt, hyper, meta, or super (#68986)

* Tue Jul  2 2002 Nalin Dahyabhai <nalin@redhat.com>
- allow shift+click to extend the selection (re: gnome 86246)

* Mon Jul  1 2002 Nalin Dahyabhai <nalin@redhat.com> 0.4.7-1
- recover from encoding errors more gracefully

* Mon Jul  1 2002 Nalin Dahyabhai <nalin@redhat.com> 0.4.6-1
- draw unicode line-drawing characters natively

* Tue Jun 25 2002 Nalin Dahyabhai <nalin@redhat.com> 0.4.5-1
- don't append spaces to multicolumn characters when reading the screen's
  contents (part of #67379)
- fix overexpose of neighboring cells (part of #67379)
- prevent backscroll on the alternate screen for consistency with xterm
- bind F10 to "k;", not "k0" (#67133)

* Tue Jun 25 2002 Nalin Dahyabhai <nalin@redhat.com> 0.4.4-1
- clear alternate buffer when switching screens (#67094)
- fix setting of titles, but crept in when cleaning up GIConv usage (#67236)

* Tue Jun 18 2002 Nalin Dahyabhai <nalin@redhat.com> 0.4.3-1
- correct referencing/dereferencing of I/O channels (#66248)
- correct package description to not mention the sample app which is no longer
  included

* Tue Jun 18 2002 Nalin Dahyabhai <nalin@redhat.com> 0.4.2-1
- fix "cursor mistakenly hidden when app exits" by making cursor visibility
  a widget-wide (as opposed to per-screen) setting

* Tue Jun 18 2002 Nalin Dahyabhai <nalin@redhat.com> 0.4.1-1
- fix use of alternate buffer in xterm emulation mode

* Fri Jun 14 2002 Nalin Dahyabhai <nalin@redhat.com> 0.4.0-1
- add a means for apps to add environment variables

* Fri Jun 14 2002 Nalin Dahyabhai <nalin@redhat.com> 0.3.30-1
- package up the python module

* Mon Jun 10 2002 Nalin Dahyabhai <nalin@redhat.com> 0.3.29-1
- compute padding correctly

* Mon Jun 10 2002 Nalin Dahyabhai <nalin@redhat.com> 0.3.28-1
- finish merging otaylor's Xft2 patch

* Mon Jun 10 2002 Nalin Dahyabhai <nalin@redhat.com> 0.3.27-1
- rework accessibility

* Thu Jun  6 2002 Nalin Dahyabhai <nalin@redhat.com> 0.3.26-1
- don't package up the test program
- emit "child-exited" signals properly
- try to allow building with either pangoxft-with-Xft1 or pangoxft-with-Xft2

* Wed Jun  5 2002 Nalin Dahyabhai <nalin@redhat.com> 0.3.25-1
- compute font widths better

* Mon Jun  3 2002 Nalin Dahyabhai <nalin@redhat.com> 0.3.24-1
- tweak handling of invalid sequences again

* Fri May 31 2002 Nalin Dahyabhai <nalin@redhat.com> 0.3.23-1
- switch to g_convert (again?)
- fix use of core fonts

* Tue May 28 2002 Nalin Dahyabhai <nalin@redhat.com> 0.3.22-1
- plug some memory leaks

* Tue May 28 2002 Nalin Dahyabhai <nalin@redhat.com> 0.3.21-1
- fix matching, fix async background updates

* Fri May 24 2002 Nalin Dahyabhai <nalin@redhat.com> 0.3.20-1
- fixes from notting and otaylor

* Tue May 21 2002 Nalin Dahyabhai <nalin@redhat.com> 0.3.19-1
- fixes from andersca and Hidetoshi Tajima

* Thu May 16 2002 Nalin Dahyabhai <nalin@redhat.com> 0.3.18-1
- finish implementing matching

* Thu May 16 2002 Nalin Dahyabhai <nalin@redhat.com> 0.3.17-1
- tweak finding of selected text

* Wed May 15 2002 Nalin Dahyabhai <nalin@redhat.com> 0.3.16-1
- hook up Insert->kI
- convert scroll events to button 4/5 if an app wants mouse events
- don't send drag events when apps only want click events
- fix selection crashbug

* Tue May 14 2002 Nalin Dahyabhai <nalin@redhat.com> 0.3.15-1
- fix ce, implement save/restore mode

* Tue May 14 2002 Nalin Dahyabhai <nalin@redhat.com> 0.3.14-1
- don't draw nul chars

* Mon May 13 2002 Nalin Dahyabhai <nalin@redhat.com> 0.3.13-1
- fix insert mode, implement visual bells

* Thu May  9 2002 Nalin Dahyabhai <nalin@redhat.com> 0.3.12-1
- iconv and remapping from otaylor
- implement custom tabstopping

* Wed May  8 2002 Nalin Dahyabhai <nalin@redhat.com> 0.3.11-1
- add mouse drag event handling

* Mon May  6 2002 Nalin Dahyabhai <nalin@redhat.com> 0.3.10-1
- do mouse autohide and cursor switching

* Mon May  6 2002 Nalin Dahyabhai <nalin@redhat.com> 0.3.9-1
- start handling mouse events

* Mon May  6 2002 Nalin Dahyabhai <nalin@redhat.com> 0.3.8-1
- handle window manipulation sequences

* Fri May  3 2002 Nalin Dahyabhai <nalin@redhat.com> 0.3.7-1
- discard invalid control sequences
- recognize designate-??-character-set correctly

* Thu May  2 2002 Nalin Dahyabhai <nalin@redhat.com> 0.3.6-1
- add a couple of sequence handlers, fix a couple of accessibility crashbugs

* Thu May  2 2002 Nalin Dahyabhai <nalin@redhat.com> 0.3.5-1
- fix cap parsing error and "handle" long invalid multibyte sequences better

* Thu May  2 2002 Nalin Dahyabhai <nalin@redhat.com> 0.3.4-1
- try to speed up sequence recognition a bit
- disable some window_scroll speedups that appear to cause flickering

* Wed May  1 2002 Nalin Dahyabhai <nalin@redhat.com> 0.3.2-1
- include a small default termcap for systems without termcap files

* Tue Apr 30 2002 Nalin Dahyabhai <nalin@redhat.com> 0.3.1-1
- disconnect from the configure_toplevel signal at finalize-time

* Tue Apr 30 2002 Nalin Dahyabhai <nalin@redhat.com> 0.3-1
- add an accessiblity object

* Mon Apr 29 2002 Nalin Dahyabhai <nalin@redhat.com> 0.2.3-1
- fix color resetting
- fix idle handlers not being disconnected

* Mon Apr 29 2002 Nalin Dahyabhai <nalin@redhat.com> 0.2.2-1
- bug fixes

* Thu Apr 25 2002 Nalin Dahyabhai <nalin@redhat.com> 0.1-1
- finish initial packaging, start the changelog
