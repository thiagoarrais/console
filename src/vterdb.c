/*
 * Copyright (C) 2003 Red Hat, Inc.
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


#include "../config.h"

#include <limits.h>
#include <string.h>
#include <gtk/gtk.h>
#ifndef X_DISPLAY_MISSING
#include <gdk/gdkx.h>
#endif
#include "vterdb.h"

#define DEFAULT_ANTIALIAS	TRUE
#define DEFAULT_DPI		-1
#define DEFAULT_RGBA		"none"
#define DEFAULT_HINTING		TRUE
#define DEFAULT_HINTSTYLE	"hintfull"

static gboolean
_vte_property_get_string(GdkWindow *window, GdkAtom atom,
			 GdkAtom *type, int *size,
			 char **retval)
{
	return gdk_property_get(window, atom, GDK_TARGET_STRING,
				0, INT_MAX - 3,
				FALSE,
				type, NULL, size,
				(guchar**) retval);
}

static gchar **
_vte_rdb_get(GtkWidget *widget)
{
	GdkWindow *root;
	char *prop_data;
	int prop_length;
	GdkAtom atom, prop_type;
	gboolean result;
	gchar **ret;

	/* Retrieve the window and the property which we're going to read. */
	GdkDisplay *display;
	GdkScreen *screen;

	display = gtk_widget_get_display(widget);
	screen = gtk_widget_get_screen(widget);

	root = gdk_screen_get_root_window(screen);
	if (root == NULL) {
		root = gdk_get_default_root_window();
	}

	ret = g_object_get_data (G_OBJECT (root), "_vte_rdb_get");
	if (ret) {
		return ret == (gchar **) 0x1 ? NULL : ret;
	}


	atom = gdk_atom_intern("RESOURCE_MANAGER", TRUE);
	if (atom == 0) {
		return NULL;
	}

	/* Read the string property off of the window. */
	prop_data = NULL;
	gdk_error_trap_push();
	result = _vte_property_get_string(root, atom,
				 &prop_type, &prop_length,
				 &prop_data);
	gdk_display_sync(display);
	gdk_error_trap_pop();

	/* Only parse the information if we got a string. */
	if (result && prop_type == GDK_TARGET_STRING && prop_data != NULL) {
		gchar *tmp = g_strndup(prop_data, prop_length);
		ret = g_strsplit(tmp, "\n", -1);
		g_free(tmp);
		g_free(prop_data);
		g_object_set_data_full (G_OBJECT (root),
				        "_vte_rdb_get", ret,
					(GDestroyNotify) g_strfreev);
		return ret;
	}

	g_object_set_data (G_OBJECT (root), "_vte_rdb_get", NULL);

	return NULL;
}

static gchar *
_vte_rdb_search(GtkWidget *widget, const char *setting)
{
	gchar *ret = NULL;
	gchar **rdb;

	rdb = _vte_rdb_get(widget);
	if (rdb != NULL) {
		guint i, len = strlen(setting);
		for (i = 0; rdb[i] != NULL; i++) {
			if ((strncmp(rdb[i], setting, len) == 0) &&
			    (rdb[i][len] == ':') &&
			    (rdb[i][len + 1] == '\t')) {
				ret = g_strdup(rdb[i] + len + 2);
				break;
			}
		}
	}

	return ret;
}

static double
_vte_rdb_double(GtkWidget *widget, const char *setting, double default_value)
{
	char *start, *endptr = NULL;
	double dbl;
	start = _vte_rdb_search(widget, setting);
	if (start == NULL) {
		return default_value;
	}
	dbl = g_ascii_strtod(start, &endptr);
	if ((endptr == NULL) || (*endptr != '\0')) {
		dbl = default_value;
	}
	g_free(start);
	return dbl;
}

#if 0
static int
_vte_rdb_integer(GtkWidget *widget, const char *setting, int default_value)
{
	char *start, *endptr = NULL;
	int n;
	start = _vte_rdb_search(widget, setting);
	if (start == NULL) {
		return default_value;
	}
	n = CLAMP(g_ascii_strtoull(start, &endptr, 10), 0, INT_MAX);
	if ((endptr == NULL) || (*endptr != '\0')) {
		n = default_value;
	}
	g_free(start);
	return n;
}
#endif

static gboolean
_vte_rdb_boolean(GtkWidget *widget, const char *setting, gboolean default_value)
{
	char *start, *endptr = NULL;
	gboolean n;
	start = _vte_rdb_search(widget, setting);
	if (start == NULL) {
		return default_value;
	}
	n = g_ascii_strtoull(start, &endptr, 10) != 0;
	if ((endptr != NULL) && (*endptr == '\0')) {
		/* use current value of n */
	} else
	if (g_ascii_strcasecmp(start, "true") == 0) {
		n = TRUE;
	} else
	if (g_ascii_strcasecmp(start, "false") == 0) {
		n = FALSE;
	} else {
		n = default_value;
	}
	g_free(start);
	return n;
}

static GQuark
_vte_rdb_quark(GtkWidget *widget, const char *setting, GQuark default_value)
{
	char *start;
	GQuark q;
	start = _vte_rdb_search(widget, setting);
	if (start == NULL) {
		return default_value;
	}
	q = g_quark_from_string(start);
	g_free(start);
	return q;
}

double
_vte_rdb_get_dpi(GtkWidget *widget)
{
	return _vte_rdb_double(widget, "Xft.dpi", DEFAULT_DPI);
}

gboolean
_vte_rdb_get_antialias(GtkWidget *widget)
{
	return _vte_rdb_boolean(widget, "Xft.antialias", DEFAULT_ANTIALIAS);
}

gboolean
_vte_rdb_get_hinting(GtkWidget *widget)
{
	return _vte_rdb_boolean(widget, "Xft.hinting", DEFAULT_HINTING);
}

const char *
_vte_rdb_get_rgba(GtkWidget *widget)
{
	GQuark q = g_quark_from_string(DEFAULT_RGBA);
	return g_quark_to_string(_vte_rdb_quark(widget, "Xft.rgba", q));
}

const char *
_vte_rdb_get_hintstyle(GtkWidget *widget)
{
	GQuark q = g_quark_from_string(DEFAULT_HINTSTYLE);
	return g_quark_to_string(_vte_rdb_quark(widget, "Xft.hintstyle", q));
}

void
_vte_rdb_release (GtkWidget *widget)
{
	GdkDisplay *display;
	GdkScreen *screen;
	GdkWindow *root;

	display = gtk_widget_get_display(widget);
	screen = gtk_widget_get_screen(widget);

	root = gdk_screen_get_root_window(screen);
	if (root == NULL) {
		root = gdk_get_default_root_window();
	}

	g_object_set_data (G_OBJECT (root), "_vte_rdb_get", NULL);
}

#ifdef VTERDB_MAIN
int
main(int argc, char **argv)
{
	GtkWidget *window;
	gtk_init(&argc, &argv);
	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	g_print("DPI: %lf\n",
		_vte_rdb_get_dpi(window));
	g_print("Antialias: %s\n",
		_vte_rdb_get_antialias(window) ? "TRUE" : "FALSE");
	g_print("Hinting: %s\n",
		_vte_rdb_get_hinting(window) ? "TRUE" : "FALSE");
	g_print("Hint style: %s\n",
		_vte_rdb_get_hintstyle(window));
	g_print("RGBA: %s\n",
		_vte_rdb_get_rgba(window));
	return 0;
}
#endif
