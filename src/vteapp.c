/*
 * Copyright (C) 2001,2002 Red Hat, Inc.
 *
 * This is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Library General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ident "$Id$"
#include "../config.h"
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>
#include <gtk/gtk.h>
#include <glib-object.h>
#include "debug.h"
#include "vte.h"

static void
window_title_changed(GtkWidget *widget, gpointer win)
{
	GtkWindow *window;

	g_return_if_fail(VTE_TERMINAL(widget));
	g_return_if_fail(GTK_IS_WINDOW(win));
	g_return_if_fail(VTE_TERMINAL (widget)->window_title != NULL);
	window = GTK_WINDOW(win);

	gtk_window_set_title(window, VTE_TERMINAL (widget)->window_title);
}

static void
char_size_changed(GtkWidget *widget, guint width, guint height, gpointer data)
{
	VteTerminal *terminal;
	GtkWindow *window;
	GdkGeometry geometry;
	int xpad, ypad;
	g_return_if_fail(GTK_IS_WINDOW(data));
	g_return_if_fail(VTE_IS_TERMINAL(widget));
	terminal = VTE_TERMINAL(widget);
	window = GTK_WINDOW(data);
	vte_terminal_get_padding(terminal, &xpad, &ypad);
	geometry.width_inc = terminal->char_width;
	geometry.height_inc = terminal->char_height;
	geometry.base_width = xpad;
	geometry.base_height = ypad;
	geometry.min_width = xpad + terminal->char_width;
	geometry.min_height = ypad + terminal->char_height;
	gtk_window_set_geometry_hints(window, widget, &geometry,
				      GDK_HINT_RESIZE_INC |
				      GDK_HINT_BASE_SIZE |
				      GDK_HINT_MIN_SIZE);
}

static void
destroy_and_quit(GtkWidget *widget, gpointer data)
{
	if (GTK_IS_CONTAINER(data)) {
		gtk_container_remove(GTK_CONTAINER(data), widget);
	} else {
		gtk_widget_destroy(widget);
	}
	gtk_main_quit();
}
static void
destroy_and_quit_eof(GtkWidget *widget, gpointer data)
{
	g_print("Detected EOF.\n");
	destroy_and_quit(widget, data);
}
static void
destroy_and_quit_exited(GtkWidget *widget, gpointer data)
{
	g_print("Detected child exit.\n");
	destroy_and_quit(widget, data);
}

static void
status_line_changed(GtkWidget *widget, gpointer data)
{
	g_print("Status = `%s'.\n",
		vte_terminal_get_status_line(VTE_TERMINAL(widget)));
}

int
main(int argc, char **argv)
{
	GtkWidget *window, *hbox, *scrollbar, *widget;
	char *env_add[] = {"FOO=BAR", "BOO=BIZ", NULL};
	const char *background = NULL;
	gboolean transparent = FALSE, audible = TRUE, blink = TRUE,
		 debug = FALSE, dingus = FALSE, geometry = TRUE;
	long lines = 100;
	const char *message = "Launching interactive shell...\r\n";
	const char *font = NULL;
	const char *terminal = NULL;
	const char *command = NULL;
	char **argv2;
	int opt;
	int i, j;
	GList *args = NULL;
	GdkColor fore, back;
	const char *usage = "Usage: %s "
			    "[ [-B image] | [-T] ] "
			    "[-D] "
			    "[-a] "
			    "[-b] "
			    "[-c command] "
			    "[-d] "
			    "[-f font] "
			    "[-g] "
			    "[-h] "
			    "[-n] "
			    "[-t terminaltype]\n";
	back.red = back.green = back.blue = 0xffff;
	fore.red = fore.green = fore.blue = 0x3000;

	/* Have to do this early. */
	if (getenv("VTE_PROFILE_MEMORY")) {
		if (atol(getenv("VTE_PROFILE_MEMORY")) != 0) {
			g_mem_set_vtable(glib_mem_profiler_table);
		}
	}

	/* Pull out long options for GTK+. */
	for (i = j = 1; i < argc; i++) {
		if (g_ascii_strncasecmp("--", argv[i], 2) == 0) {
			args = g_list_append(args, argv[i]);
			for (j = i; j < argc; j++) {
				argv[j] = argv[j + 1];
			}
			argc--;
			i--;
		}
	}
	argv2 = g_malloc0(sizeof(char*) * (g_list_length(args) + 2));
	argv2[0] = argv[0];
	for (i = 1; i <= g_list_length(args); i++) {
		argv2[i] = (char*) g_list_nth(args, i - 1);
	}
	argv2[i] = NULL;
	g_assert(i < (g_list_length(args) + 2));
	/* Parse some command-line options. */
	while ((opt = getopt(argc, argv, "B:DTabc:df:ghn:t:")) != -1) {
		switch (opt) {
			case 'B':
				background = optarg;
				break;
			case 'D':
				dingus = TRUE;
				break;
			case 'T':
				transparent = TRUE;
				break;
			case 'a':
				audible = !audible;
				break;
			case 'b':
				blink = !blink;
				break;
			case 'c':
				command = optarg;
				break;
			case 'd':
				debug = !debug;
				break;
			case 'f':
				font = optarg;
				break;
			case 'g':
				geometry = !geometry;
				break;
			case 'n':
				lines = atol(optarg);
				if (lines == 0) {
					lines = 100;
				}
				break;
			case 't':
				terminal = optarg;
				break;
			case 'h':
			default:
				g_print(usage, argv[0]);
				exit(1);
				break;
		}
	}

	gtk_init(&argc, &argv);
	gdk_window_set_debug_updates(debug);

	/* Create a window to hold the scrolling shell, and hook its
	 * delete event to the quit function.. */
	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	g_signal_connect(G_OBJECT(window), "delete_event",
			 GTK_SIGNAL_FUNC(gtk_main_quit), NULL);

	/* Create a box to hold everything. */
	hbox = gtk_hbox_new(0, FALSE);
	gtk_container_add(GTK_CONTAINER(window), hbox);

	/* Create the terminal widget and add it to the scrolling shell. */
	widget = vte_terminal_new();
	gtk_box_pack_start(GTK_BOX(hbox), widget, TRUE, TRUE, 0);

	/* Connect to the "char_size_changed" signal to set geometry hints
	 * whenever the font used by the terminal is changed. */
	if (geometry) {
		char_size_changed(widget, 0, 0, window);
		g_signal_connect(G_OBJECT(widget), "char-size-changed",
				 G_CALLBACK(char_size_changed), window);
	}

	/* Connect to the "window_title_changed" signal to set the main
	 * window's title. */
	g_signal_connect(G_OBJECT(widget), "window-title-changed",
			 G_CALLBACK(window_title_changed), window);

	/* Connect to the "eof" signal to quit when the session ends. */
	g_signal_connect(G_OBJECT(widget), "eof",
			 G_CALLBACK(destroy_and_quit_eof), widget);
	g_signal_connect(G_OBJECT(widget), "child-exited",
			 G_CALLBACK(destroy_and_quit_exited), widget);

	/* Connect to the "status-line-changed" signal. */
	g_signal_connect(G_OBJECT(widget), "status-line-changed",
			 G_CALLBACK(status_line_changed), widget);

	/* Create the scrollbar for the widget. */
	scrollbar = gtk_vscrollbar_new((VTE_TERMINAL(widget))->adjustment);
	gtk_box_pack_start(GTK_BOX(hbox), scrollbar, FALSE, FALSE, 0);

	/* Set some defaults. */
	vte_terminal_set_audible_bell(VTE_TERMINAL(widget), audible);
	vte_terminal_set_cursor_blinks(VTE_TERMINAL(widget), blink);
	vte_terminal_set_scroll_on_output(VTE_TERMINAL(widget), FALSE);
	vte_terminal_set_scroll_on_keystroke(VTE_TERMINAL(widget), TRUE);
	vte_terminal_set_scrollback_lines(VTE_TERMINAL(widget), lines);
	vte_terminal_set_mouse_autohide(VTE_TERMINAL(widget), TRUE);
	if (background != NULL) {
		vte_terminal_set_background_image_file(VTE_TERMINAL(widget),
						       background);
	}
	if (transparent) {
		vte_terminal_set_background_transparent(VTE_TERMINAL(widget),
							TRUE);
	}
	vte_terminal_set_colors(VTE_TERMINAL(widget), &fore, &back, NULL, 0);
	if (terminal != NULL) {
		vte_terminal_set_emulation(VTE_TERMINAL(widget), terminal);
	}

	/* Set the default font. */
	if (font != NULL) {
		vte_terminal_set_font_from_string(VTE_TERMINAL(widget), font);
	}

	/* Match "abcdefg". */
	vte_terminal_match_add(VTE_TERMINAL(widget), "abcdefg");
	if (dingus) {
		vte_terminal_match_add(VTE_TERMINAL(widget),
				       "(((news|telnet|nttp|file|http|ftp|https)://)|(www|ftp)[-A-Za-z0-9]*\\.)[-A-Za-z0-9\\.]+(:[0-9]*)?");
		vte_terminal_match_add(VTE_TERMINAL(widget),
				       "(((news|telnet|nttp|file|http|ftp|https)://)|(www|ftp)[-A-Za-z0-9]*\\.)[-A-Za-z0-9\\.]+(:[0-9]*)?/[-A-Za-z0-9_\\$\\.\\+\\!\\*\\(\\),;:@&=\\?/~\\#\\%]*[^]'\\.}>\\) ,\\\"]");
	}

	/* Launch a shell. */
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_MISC)) {
		vte_terminal_feed(VTE_TERMINAL(widget), message,
				  strlen(message));
	}
#endif
	vte_terminal_fork_logged_command(VTE_TERMINAL(widget),
					 command, NULL, env_add,
					 TRUE, TRUE, TRUE);
	if (command == NULL) {
		vte_terminal_feed_child(VTE_TERMINAL(widget), "pwd\n", -1);
	}

	/* Go for it! */
	gtk_widget_show_all(window);

	gtk_main();

	return 0;
}
