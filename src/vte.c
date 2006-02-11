/*
 * Copyright (C) 2001-2004 Red Hat, Inc.
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

#include "vte.h"
#include "vte-private.h"

#ifdef HAVE_WCHAR_H
#include <wchar.h>
#endif
#include <glib.h>
#include <glib-object.h>
#include <gdk/gdk.h>
#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <pango/pango.h>
#include "iso2022.h"
#include "keymap.h"
#include "marshal.h"
#include "matcher.h"
#include "pty.h"
#include "vteaccess.h"
#include "vteint.h"
#include "vteregex.h"
#include "vtetc.h"
#include "vteseq.h"
#include <fontconfig/fontconfig.h>

#ifdef HAVE_LOCALE_H
#include <locale.h>
#endif

#ifndef HAVE_WINT_T
typedef gunichar wint_t;
#endif

static void vte_terminal_set_termcap(VteTerminal *terminal, const char *path,
				     gboolean reset);
static void vte_terminal_paste(VteTerminal *terminal, GdkAtom board);
static gboolean vte_terminal_io_read(GIOChannel *channel,
				     GIOCondition condition,
				     gpointer data);
static gboolean vte_terminal_io_write(GIOChannel *channel,
				      GIOCondition condition,
				      gpointer data);
static void vte_terminal_match_hilite_clear(VteTerminal *terminal);
static gboolean vte_terminal_background_update(gpointer data);
static void vte_terminal_queue_background_update(VteTerminal *terminal);
static void vte_terminal_queue_adjustment_changed(VteTerminal *terminal);
static gboolean vte_terminal_process_incoming(VteTerminal *terminal);
static gboolean vte_cell_is_selected(VteTerminal *terminal,
				     glong col, glong row, gpointer data);
static char *vte_terminal_get_text_range_maybe_wrapped(VteTerminal *terminal,
						       glong start_row,
						       glong start_col,
						       glong end_row,
						       glong end_col,
						       gboolean wrap,
						       gboolean(*is_selected)(VteTerminal *,
									      glong,
									      glong,
									      gpointer),
						       gpointer data,
						       GArray *attributes,
						       gboolean include_trailing_spaces);
static char *vte_terminal_get_text_maybe_wrapped(VteTerminal *terminal,
						 gboolean wrap,
						 gboolean(*is_selected)(VteTerminal *,
									glong,
									glong,
									gpointer),
						 gpointer data,
						 GArray *attributes,
						 gboolean include_trailing_spaces);
static void _vte_terminal_disconnect_pty_read(VteTerminal *terminal);
static void _vte_terminal_disconnect_pty_write(VteTerminal *terminal);
static void vte_terminal_stop_processing (VteTerminal *terminal);
static void vte_terminal_start_processing (VteTerminal *terminal);
static gboolean vte_terminal_is_processing (VteTerminal *terminal);

static gpointer parent_class;

/* Free a no-longer-used row data array. */
static void
vte_free_row_data(gpointer freeing, gpointer data)
{
	if (freeing) {
		VteRowData *row = (VteRowData*) freeing;
		g_array_free(row->cells, TRUE);
		g_free(row);
	}
}

/* Append a single item to a GArray a given number of times. Centralizing all
 * of the places we do this may let me do something more clever later. */
static void
vte_g_array_fill(GArray *array, gpointer item, guint final_size)
{
	g_assert(array != NULL);
	if (array->len >= final_size) {
		return;
	}
	g_assert(item != NULL);

	while (array->len < final_size) {
		g_array_append_vals(array, item, 1);
	}
}

/* Allocate a new line. */
VteRowData *
_vte_new_row_data(VteTerminal *terminal)
{
	VteRowData *row = NULL;
	row = g_malloc0(sizeof(VteRowData));
#ifdef VTE_DEBUG
	row->cells = g_array_new(FALSE, TRUE, sizeof(struct vte_charcell));
#else
	row->cells = g_array_new(FALSE, FALSE, sizeof(struct vte_charcell));
#endif
	row->soft_wrapped = 0;
	return row;
}

/* Allocate a new line of a given size. */
VteRowData *
_vte_new_row_data_sized(VteTerminal *terminal, gboolean fill)
{
	VteRowData *row = NULL;
	row = g_malloc0(sizeof(VteRowData));
#ifdef VTE_DEBUG
	row->cells = g_array_sized_new(FALSE, TRUE,
				       sizeof(struct vte_charcell),
				       terminal->column_count);
#else
	row->cells = g_array_sized_new(FALSE, FALSE,
				       sizeof(struct vte_charcell),
				       terminal->column_count);
#endif
	row->soft_wrapped = 0;
	if (fill) {
		vte_g_array_fill(row->cells,
				 &terminal->pvt->screen->fill_defaults,
				 terminal->column_count);
	}
	return row;
}

/* Reset defaults for character insertion. */
void
_vte_terminal_set_default_attributes(VteTerminal *terminal)
{
	g_assert(VTE_IS_TERMINAL(terminal));
	terminal->pvt->screen->defaults.c = ' ';
	terminal->pvt->screen->defaults.columns = 1;
	terminal->pvt->screen->defaults.fragment = 0;
	terminal->pvt->screen->defaults.fore = VTE_DEF_FG;
	terminal->pvt->screen->defaults.back = VTE_DEF_BG;
	terminal->pvt->screen->defaults.reverse = 0;
	terminal->pvt->screen->defaults.bold = 0;
	terminal->pvt->screen->defaults.invisible = 0;
	terminal->pvt->screen->defaults.protect = 0;
	terminal->pvt->screen->defaults.standout = 0;
	terminal->pvt->screen->defaults.underline = 0;
	terminal->pvt->screen->defaults.strikethrough = 0;
	terminal->pvt->screen->defaults.half = 0;
	terminal->pvt->screen->defaults.blink = 0;
	/* Alternate charset isn't an attribute, though we treat it as one.
	 * terminal->pvt->screen->defaults.alternate = 0; */
	terminal->pvt->screen->basic_defaults = terminal->pvt->screen->defaults;
	terminal->pvt->screen->color_defaults = terminal->pvt->screen->defaults;
	terminal->pvt->screen->fill_defaults = terminal->pvt->screen->defaults;
}

static gboolean
vte_update_timeout(VteTerminal *terminal)
{
	terminal->pvt->update_timer = 0;
	if (terminal->pvt->update_region) {
		gdk_window_invalidate_region(GTK_WIDGET(terminal)->window,
					     terminal->pvt->update_region, FALSE);
		gdk_region_destroy (terminal->pvt->update_region);
		terminal->pvt->update_region = NULL;
	}

	return FALSE;
}

static void
vte_free_update_timer (VteTerminal *terminal)
{
	if (terminal->pvt->update_timer) {
		g_source_remove (terminal->pvt->update_timer);
		terminal->pvt->update_timer = 0;
	}

	if (terminal->pvt->update_region) {
		gdk_region_destroy (terminal->pvt->update_region);
		terminal->pvt->update_region = NULL;
	}
}

/* Cause certain cells to be repainted. */
void
_vte_invalidate_cells(VteTerminal *terminal,
		      glong column_start, gint column_count,
		      glong row_start, gint row_count)
{
	GdkRectangle rect;
	GtkWidget *widget;
	gint i;

	g_assert(VTE_IS_TERMINAL(terminal));
	widget = GTK_WIDGET(terminal);
	if (!GTK_WIDGET_REALIZED(widget)) {
		return;
	}
	if (terminal->pvt->visibility_state == GDK_VISIBILITY_FULLY_OBSCURED) {
		return;
	}

	/* Subtract the scrolling offset from the row start so that the
	 * resulting rectangle is relative to the visible portion of the
	 * buffer. */
	row_start -= terminal->pvt->screen->scroll_delta;

	/* Clamp the start values to reasonable numbers. */
	i = MIN (row_start + row_count, terminal->row_count);
	row_start = MAX (0, row_start);
	row_count = MAX (0, i - row_start);

	i = MIN (column_start + column_count, terminal->column_count);
	column_start = MAX (0, column_start);
	column_count = MAX (0, i - column_start);

	/* Convert the column and row start and end to pixel values
	 * by multiplying by the size of a character cell. */
	rect.x = column_start * terminal->char_width + VTE_PAD_WIDTH;
	rect.width = column_count * terminal->char_width;
	if (column_start == 0) {
		/* Include the left border. */
		rect.x -= VTE_PAD_WIDTH;
		rect.width += VTE_PAD_WIDTH;
	}
	if (column_start + column_count == terminal->column_count) {
		/* Include the right border. */
		rect.width += VTE_PAD_WIDTH;
	}

	rect.y = row_start * terminal->char_height + VTE_PAD_WIDTH;
	rect.height = row_count * terminal->char_height;
	if (row_start == 0) {
		/* Include the top border. */
		rect.y -= VTE_PAD_WIDTH;
		rect.height += VTE_PAD_WIDTH;
	}
	if (row_start + row_count == terminal->row_count) {
		/* Include the bottom border. */
		rect.height += VTE_PAD_WIDTH;
	}

	if (terminal->pvt->update_timer) {
		if (!terminal->pvt->update_region)
			terminal->pvt->update_region = gdk_region_rectangle (&rect);
		else
			gdk_region_union_with_rect (terminal->pvt->update_region, &rect);
	} else {
		/* Invalidate the rectangle. */
		gdk_window_invalidate_rect(widget->window, &rect, FALSE);

		/* Set a timer such that we do not invalidate for a while. */
		/* This limits the number of times we draw to 40fps. */
		terminal->pvt->update_timer = g_timeout_add (25, vte_update_timeout, terminal);
	}

}

/* Redraw the entire visible portion of the window. */
void
_vte_invalidate_all(VteTerminal *terminal)
{
	GdkRectangle rect;
	GtkWidget *widget;
	int width, height;

	g_assert(VTE_IS_TERMINAL(terminal));
	if (!GTK_IS_WIDGET(terminal)) {
	       return;
	}
	widget = GTK_WIDGET(terminal);
	if (!GTK_WIDGET_REALIZED(widget)) {
		return;
	}
	if (terminal->pvt->visibility_state == GDK_VISIBILITY_FULLY_OBSCURED) {
		return;
	}

	if (terminal->pvt->update_timer) {
		vte_free_update_timer (terminal);
	}

	/* Expose the entire widget area. */
	width = height = 0;
	gdk_drawable_get_size(widget->window, &width, &height);
	rect.x = 0;
	rect.y = 0;
	rect.width = width;
	rect.height = height;
	gdk_window_invalidate_rect(widget->window, &rect, FALSE);
}

/* Scroll a rectangular region up or down by a fixed number of lines,
 * negative = up, positive = down. */
void
_vte_terminal_scroll_region(VteTerminal *terminal,
			   long row, glong count, glong delta)
{
	if ((delta == 0) || (count == 0)) {
		/* Shenanigans! */
		return;
	}

	if (terminal->pvt->scroll_background) {
		/* We have to repaint the entire window. */
		_vte_invalidate_all(terminal);
	} else {
		/* We have to repaint the area which is to be
		 * scrolled. */
		_vte_invalidate_cells(terminal,
				     0, terminal->column_count,
				     row, count);
	}
}

/* Find the character an the given position in the backscroll buffer. */
static struct vte_charcell *
vte_terminal_find_charcell(VteTerminal *terminal, glong col, glong row)
{
	VteRowData *rowdata;
	struct vte_charcell *ret = NULL;
	VteScreen *screen;
	g_assert(VTE_IS_TERMINAL(terminal));
	screen = terminal->pvt->screen;
	if (_vte_ring_contains(screen->row_data, row)) {
		rowdata = _vte_ring_index(screen->row_data, VteRowData *, row);
		if (rowdata->cells->len > col) {
			ret = &g_array_index(rowdata->cells,
					     struct vte_charcell,
					     col);
		}
	}
	return ret;
}

/* Determine the width of the portion of the preedit string which lies
 * to the left of the cursor, or the entire string, in columns. */
static gssize
vte_terminal_preedit_width(VteTerminal *terminal, gboolean left_only)
{
	gunichar c;
	int i;
	gssize ret = 0;
	const char *preedit = NULL;

	g_assert(VTE_IS_TERMINAL(terminal));

	if (terminal->pvt->im_preedit != NULL) {
		preedit = terminal->pvt->im_preedit;
		for (i = 0;
		     (preedit != NULL) &&
		     (preedit[0] != '\0') &&
		     (!left_only || (i < terminal->pvt->im_preedit_cursor));
		     i++) {
			c = g_utf8_get_char(preedit);
			ret += _vte_iso2022_unichar_width(c);
			preedit = g_utf8_next_char(preedit);
		}
	}

	return ret;
}

/* Determine the length of the portion of the preedit string which lies
 * to the left of the cursor, or the entire string, in gunichars. */
static gssize
vte_terminal_preedit_length(VteTerminal *terminal, gboolean left_only)
{
	int i = 0;
	const char *preedit = NULL;

	g_assert(VTE_IS_TERMINAL(terminal));

	if (terminal->pvt->im_preedit != NULL) {
		preedit = terminal->pvt->im_preedit;
		for (i = 0;
		     (preedit != NULL) &&
		     (preedit[0] != '\0') &&
		     (!left_only || (i < terminal->pvt->im_preedit_cursor));
		     i++) {
			preedit = g_utf8_next_char(preedit);
		}
	}

	return i;
}

/* Cause the cursor to be redrawn. */
void
_vte_invalidate_cursor_once(gpointer data, gboolean periodic)
{
	VteTerminal *terminal;
	VteScreen *screen;
	struct vte_charcell *cell;
	gssize preedit_width;
	int column, columns, row;

	if (!VTE_IS_TERMINAL(data)) {
		return;
	}

	terminal = VTE_TERMINAL(data);

	if (terminal->pvt->visibility_state == GDK_VISIBILITY_FULLY_OBSCURED) {
		return;
	}

	if (periodic) {
		if (!terminal->pvt->cursor_blinks) {
			return;
		}
	}

	if (terminal->pvt->cursor_visible &&
	    GTK_WIDGET_REALIZED(GTK_WIDGET(terminal))) {
		preedit_width = vte_terminal_preedit_width(terminal, FALSE);

		screen = terminal->pvt->screen;
		row = screen->cursor_current.row;
		column = screen->cursor_current.col;
		columns = 1;
		cell = vte_terminal_find_charcell(terminal,
						  column,
						  screen->cursor_current.row);
		while ((cell != NULL) && (cell->fragment) && (column > 0)) {
			column--;
			cell = vte_terminal_find_charcell(terminal,
							  column,
							  row);
		}
		if (cell != NULL) {
			columns = cell->columns;
			if (_vte_draw_get_char_width(terminal->pvt->draw,
						     cell->c,
						     cell->columns) >
			    terminal->char_width * columns) {
				columns++;
			}
		}
		if (preedit_width > 0) {
			columns += preedit_width;
			columns++; /* one more for the preedit cursor */
		}
		if (column + columns > terminal->column_count) {
			column = MAX(0, terminal->column_count - columns);
		}

		_vte_invalidate_cells(terminal,
				     column, columns,
				     row, 1);
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_UPDATES)) {
			fprintf(stderr, "Invalidating cursor at (%ld,%d-%d)."
				"\n", screen->cursor_current.row,
				column,
				column + columns);
		}
#endif
	}
}

/* Invalidate the cursor repeatedly. */
static gboolean
vte_invalidate_cursor_periodic(gpointer data)
{
	VteTerminal *terminal;
	GtkWidget *widget;
	GtkSettings *settings;
	gint blink_cycle = 1000;

	g_assert(VTE_IS_TERMINAL(data));
	widget = GTK_WIDGET(data);
	if (!GTK_WIDGET_REALIZED(widget)) {
		return TRUE;
	}
	if (!GTK_WIDGET_HAS_FOCUS(widget)) {
		return TRUE;
	}

	terminal = VTE_TERMINAL(widget);
	if (terminal->pvt->cursor_blinks) {
		_vte_invalidate_cursor_once(terminal, TRUE);
	}

	settings = gtk_widget_get_settings(GTK_WIDGET(data));
	if (G_IS_OBJECT(settings)) {
		g_object_get(G_OBJECT(settings), "gtk-cursor-blink-time",
			     &blink_cycle, NULL);
	}

	if (terminal->pvt->cursor_blink_timeout != blink_cycle) {
		terminal->pvt->cursor_blink_tag = g_timeout_add_full(G_PRIORITY_LOW,
								     blink_cycle / 2,
								     vte_invalidate_cursor_periodic,
								     terminal,
								     NULL);
		terminal->pvt->cursor_blink_timeout = blink_cycle;
		return FALSE;
	} else {
		return TRUE;
	}
}

/* Emit a "selection_changed" signal. */
static void
vte_terminal_emit_selection_changed(VteTerminal *terminal)
{
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_SIGNALS)) {
		fprintf(stderr, "Emitting `selection-changed'.\n");
	}
#endif
	g_signal_emit_by_name(terminal, "selection-changed");
}

/* Emit a "commit" signal. */
static void
vte_terminal_emit_commit(VteTerminal *terminal, gchar *text, guint length)
{
	char *wrapped = NULL;
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_SIGNALS)) {
		fprintf(stderr, "Emitting `commit' of %d bytes.\n", length);
	}
#endif
	if (length == -1) {
		length = strlen(text);
		wrapped = text;
	} else {
		wrapped = g_malloc0(length + 1);
		memcpy(wrapped, text, length);
	}
	g_signal_emit_by_name(terminal, "commit", wrapped, length);
	if (wrapped != text) {
		g_free(wrapped);
	}
}

/* Emit an "emulation-changed" signal. */
static void
vte_terminal_emit_emulation_changed(VteTerminal *terminal)
{
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_SIGNALS)) {
		fprintf(stderr, "Emitting `emulation-changed'.\n");
	}
#endif
	g_signal_emit_by_name(terminal, "emulation-changed");
}

/* Emit an "encoding-changed" signal. */
static void
vte_terminal_emit_encoding_changed(VteTerminal *terminal)
{
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_SIGNALS)) {
		fprintf(stderr, "Emitting `encoding-changed'.\n");
	}
#endif
	g_signal_emit_by_name(terminal, "encoding-changed");
}

/* Emit a "child-exited" signal. */
static void
vte_terminal_emit_child_exited(VteTerminal *terminal)
{
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_SIGNALS)) {
		fprintf(stderr, "Emitting `child-exited'.\n");
	}
#endif
	g_signal_emit_by_name(terminal, "child-exited");
}

/* Emit a "contents_changed" signal. */
void
_vte_terminal_emit_contents_changed(VteTerminal *terminal)
{
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_SIGNALS)) {
		fprintf(stderr, "Emitting `contents-changed'.\n");
	}
#endif
	g_signal_emit_by_name(terminal, "contents-changed");
}

/* Emit a "cursor_moved" signal. */
static void
vte_terminal_emit_cursor_moved(VteTerminal *terminal)
{
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_SIGNALS)) {
		fprintf(stderr, "Emitting `cursor-moved'.\n");
	}
#endif
	g_signal_emit_by_name(terminal, "cursor-moved");
}

/* Emit a "eof" signal. */
static void
vte_terminal_emit_eof(VteTerminal *terminal)
{
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_SIGNALS)) {
		fprintf(stderr, "Emitting `eof'.\n");
	}
#endif
	g_signal_emit_by_name(terminal, "eof");
}

/* Emit a "char-size-changed" signal. */
static void
vte_terminal_emit_char_size_changed(VteTerminal *terminal,
				    guint width, guint height)
{
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_SIGNALS)) {
		fprintf(stderr, "Emitting `char-size-changed'.\n");
	}
#endif
	g_signal_emit_by_name(terminal, "char-size-changed",
			      width, height);
}

/* Emit a "status-line-changed" signal. */
void
_vte_terminal_emit_status_line_changed(VteTerminal *terminal)
{
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_SIGNALS)) {
		fprintf(stderr, "Emitting `status-line-changed'.\n");
	}
#endif
	g_signal_emit_by_name(terminal, "status-line-changed");
}

/* Emit an "increase-font-size" signal. */
static void
vte_terminal_emit_increase_font_size(VteTerminal *terminal)
{
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_SIGNALS)) {
		fprintf(stderr, "Emitting `increase-font-size'.\n");
	}
#endif
	g_signal_emit_by_name(terminal, "increase-font-size");
}

/* Emit a "decrease-font-size" signal. */
static void
vte_terminal_emit_decrease_font_size(VteTerminal *terminal)
{
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_SIGNALS)) {
		fprintf(stderr, "Emitting `decrease-font-size'.\n");
	}
#endif
	g_signal_emit_by_name(terminal, "decrease-font-size");
}

/* Emit a "text-inserted" signal. */
void
_vte_terminal_emit_text_inserted(VteTerminal *terminal)
{
	if (!terminal->pvt->accessible_emit) {
		return;
	}
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_SIGNALS)) {
		fprintf(stderr, "Emitting `text-inserted'.\n");
	}
#endif
	g_signal_emit_by_name(terminal, "text-inserted");
}

/* Emit a "text-deleted" signal. */
void
_vte_terminal_emit_text_deleted(VteTerminal *terminal)
{
	if (!terminal->pvt->accessible_emit) {
		return;
	}
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_SIGNALS)) {
		fprintf(stderr, "Emitting `text-deleted'.\n");
	}
#endif
	g_signal_emit_by_name(terminal, "text-deleted");
}

/* Emit a "text-modified" signal. */
static void
vte_terminal_emit_text_modified(VteTerminal *terminal)
{
	if (!terminal->pvt->accessible_emit) {
		return;
	}
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_SIGNALS)) {
		fprintf(stderr, "Emitting `text-modified'.\n");
	}
#endif
	g_signal_emit_by_name(terminal, "text-modified");
}

/* Emit a "text-scrolled" signal. */
static void
vte_terminal_emit_text_scrolled(VteTerminal *terminal, gint delta)
{
	if (!terminal->pvt->accessible_emit) {
		return;
	}
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_SIGNALS)) {
		fprintf(stderr, "Emitting `text-scrolled'(%d).\n", delta);
	}
#endif
	g_signal_emit_by_name(terminal, "text-scrolled", delta);
}

/* Deselect anything which is selected and refresh the screen if needed. */
static void
vte_terminal_deselect_all(VteTerminal *terminal)
{
	g_assert(VTE_IS_TERMINAL(terminal));
	if (terminal->pvt->has_selection) {
		terminal->pvt->has_selection = FALSE;
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_SELECTION)) {
			fprintf(stderr, "Deselecting all text.\n");
		}
#endif
		vte_terminal_emit_selection_changed(terminal);
		_vte_invalidate_all(terminal);
	}
}

/* Remove a tabstop. */
void
_vte_terminal_clear_tabstop(VteTerminal *terminal, int column)
{
	g_assert(VTE_IS_TERMINAL(terminal));
	if (terminal->pvt->tabstops != NULL) {
		/* Remove a tab stop from the hash table. */
		g_hash_table_remove(terminal->pvt->tabstops,
				    GINT_TO_POINTER(2 * column + 1));
	}
}

/* Check if we have a tabstop at a given position. */
gboolean
_vte_terminal_get_tabstop(VteTerminal *terminal, int column)
{
	gpointer hash;
	g_assert(VTE_IS_TERMINAL(terminal));
	if (terminal->pvt->tabstops != NULL) {
		hash = g_hash_table_lookup(terminal->pvt->tabstops,
					   GINT_TO_POINTER(2 * column + 1));
		return (hash != NULL);
	} else {
		return FALSE;
	}
}

/* Reset the set of tab stops to the default. */
void
_vte_terminal_set_tabstop(VteTerminal *terminal, int column)
{
	g_assert(VTE_IS_TERMINAL(terminal));
	if (terminal->pvt->tabstops != NULL) {
		/* Just set a non-NULL pointer for this column number. */
		g_hash_table_insert(terminal->pvt->tabstops,
				    GINT_TO_POINTER(2 * column + 1),
				    terminal);
	}
}

/* Reset the set of tab stops to the default. */
static void
vte_terminal_set_default_tabstops(VteTerminal *terminal)
{
	int i, width;
	g_assert(VTE_IS_TERMINAL(terminal));
	if (terminal->pvt->tabstops != NULL) {
		g_hash_table_destroy(terminal->pvt->tabstops);
	}
	terminal->pvt->tabstops = g_hash_table_new(g_direct_hash,
						   g_direct_equal);
	width = _vte_termcap_find_numeric(terminal->pvt->termcap,
					  terminal->pvt->emulation,
					  "it");
	if (width == 0) {
		width = VTE_TAB_WIDTH;
	}
	for (i = 0; i <= VTE_TAB_MAX; i += width) {
		_vte_terminal_set_tabstop(terminal, i);
	}
}

/* Clear the cache of the screen contents we keep. */
void
_vte_terminal_match_contents_clear(VteTerminal *terminal)
{
	g_assert(VTE_IS_TERMINAL(terminal));
	if (terminal->pvt->match_contents != NULL) {
		g_free(terminal->pvt->match_contents);
		terminal->pvt->match_contents = NULL;;
	}
	if (terminal->pvt->match_attributes != NULL) {
		g_array_free(terminal->pvt->match_attributes, TRUE);
		terminal->pvt->match_attributes = NULL;
	}
	vte_terminal_match_hilite_clear(terminal);
}

/* Refresh the cache of the screen contents we keep. */
static gboolean
always_selected(VteTerminal *terminal, glong row, glong column, gpointer data)
{
	return TRUE;
}
static void
vte_terminal_match_contents_refresh(VteTerminal *terminal)
{
	GArray *array;
	g_assert(VTE_IS_TERMINAL(terminal));
	_vte_terminal_match_contents_clear(terminal);
	array = g_array_new(FALSE, TRUE, sizeof(struct _VteCharAttributes));
	terminal->pvt->match_contents = vte_terminal_get_text(terminal,
							      always_selected,
							      NULL,
							      array);
	terminal->pvt->match_attributes = array;
}

/**
 * vte_terminal_match_clear_all:
 * @terminal: a #VteTerminal
 *
 * Clears the list of regular expressions the terminal uses to highlight text
 * when the user moves the mouse cursor.
 *
 */
void
vte_terminal_match_clear_all(VteTerminal *terminal)
{
	struct vte_match_regex *regex;
	int i;
	g_return_if_fail(VTE_IS_TERMINAL(terminal));
	for (i = 0; i < terminal->pvt->match_regexes->len; i++) {
		regex = &g_array_index(terminal->pvt->match_regexes,
				       struct vte_match_regex,
				       i);
		/* Unless this is a hole, clean it up. */
		if (regex->tag >= 0) {
			if (regex->cursor != NULL) {
				gdk_cursor_unref(regex->cursor);
				regex->cursor = NULL;
			}
			_vte_regex_free(regex->reg);
			regex->reg = NULL;
			regex->tag = -1;
		}
	}
	g_array_set_size(terminal->pvt->match_regexes, 0);
	vte_terminal_match_hilite_clear(terminal);
}

/**
 * vte_terminal_match_remove:
 * @terminal: a #VteTerminal
 * @tag: the tag of the regex to remove
 *
 * Removes the regular expression which is associated with the given @tag from
 * the list of expressions which the terminal will highlight when the user
 * moves the mouse cursor over matching text.
 *
 */
void
vte_terminal_match_remove(VteTerminal *terminal, int tag)
{
	struct vte_match_regex *regex;
	g_return_if_fail(VTE_IS_TERMINAL(terminal));
	if (terminal->pvt->match_regexes->len > tag) {
		/* The tag is an index, so find the corresponding struct. */
		regex = &g_array_index(terminal->pvt->match_regexes,
				       struct vte_match_regex,
				       tag);
		/* If it's already been removed, return. */
		if (regex->tag < 0) {
			return;
		}
		/* Remove this item and leave a hole in its place. */
		if (regex->cursor != NULL) {
			gdk_cursor_unref(regex->cursor);
			regex->cursor = NULL;
		}
		_vte_regex_free(regex->reg);
		regex->reg = NULL;
		regex->tag = -1;
	}
	vte_terminal_match_hilite_clear(terminal);
}

static GdkCursor *
vte_terminal_cursor_new(VteTerminal *terminal, GdkCursorType cursor_type)
{
#if GTK_CHECK_VERSION(2,2,0)
	GdkDisplay *display;
	GdkCursor *cursor;

	g_assert(VTE_IS_TERMINAL(terminal));

	display = gtk_widget_get_display(GTK_WIDGET(terminal));
	cursor = gdk_cursor_new_for_display(display, cursor_type);
#else
	GdkCursor *cursor;

	g_assert(VTE_IS_TERMINAL(terminal));

	cursor = gdk_cursor_new(cursor_type);
#endif
	return cursor;
}

/**
 * vte_terminal_match_add:
 * @terminal: a #VteTerminal
 * @match: a regular expression
 *
 * Adds a regular expression to the list of matching expressions.  When the
 * user moves the mouse cursor over a section of displayed text which matches
 * this expression, the text will be highlighted.
 *
 * Returns: an integer associated with this expression
 */
int
vte_terminal_match_add(VteTerminal *terminal, const char *match)
{
	struct vte_match_regex new_regex, *regex;
	int ret;
	g_return_val_if_fail(VTE_IS_TERMINAL(terminal), -1);
	g_return_val_if_fail(match != NULL, -1);
	g_return_val_if_fail(strlen(match) > 0, -1);
	memset(&new_regex, 0, sizeof(new_regex));
	new_regex.reg = _vte_regex_compile(match);
	if (new_regex.reg == NULL) {
		g_warning(_("Error compiling regular expression \"%s\"."),
			  match);
		return -1;
	}

	/* Search for a hole. */
	for (ret = 0; ret < terminal->pvt->match_regexes->len; ret++) {
		regex = &g_array_index(terminal->pvt->match_regexes,
				       struct vte_match_regex,
				       ret);
		if (regex->tag == -1) {
			break;
		}
	}
	/* Set the tag to the insertion point. */
	new_regex.tag = ret;
	new_regex.cursor = vte_terminal_cursor_new(terminal,
						   VTE_DEFAULT_CURSOR);
	if (ret < terminal->pvt->match_regexes->len) {
		/* Overwrite. */
		g_array_index(terminal->pvt->match_regexes,
			      struct vte_match_regex,
			      ret) = new_regex;
	} else {
		/* Append. */
		g_array_append_val(terminal->pvt->match_regexes, new_regex);
	}
	return new_regex.tag;
}

/**
 * vte_terminal_match_set_cursor:
 * @terminal: a #VteTerminal
 * @tag: the tag of the regex which should use the specified cursor
 * @cursor: the #GdkCursor which the terminal should use when the pattern is
 * highlighted
 *
 * Sets which cursor the terminal will use if the pointer is over the pattern
 * specified by @tag.  The terminal keeps a reference to @cursor.
 *
 * Since: 0.11
 *
 */
void
vte_terminal_match_set_cursor(VteTerminal *terminal, int tag, GdkCursor *cursor)
{
	struct vte_match_regex *regex;
	g_return_if_fail(VTE_IS_TERMINAL(terminal));
	g_return_if_fail(tag < terminal->pvt->match_regexes->len);
	regex = &g_array_index(terminal->pvt->match_regexes,
			       struct vte_match_regex,
			       tag);
	if (regex->cursor != NULL) {
		gdk_cursor_unref(regex->cursor);
	}
	regex->cursor = gdk_cursor_ref(cursor);
	vte_terminal_match_hilite_clear(terminal);
}

/**
 * vte_terminal_match_set_cursor_type:
 * @terminal: a #VteTerminal
 * @tag: the tag of the regex which should use the specified cursor
 * @cursor_type: a #GdkCursorType
 *
 * Sets which cursor the terminal will use if the pointer is over the pattern
 * specified by @tag.  A convenience wrapper for
 * vte_terminal_match_set_cursor().
 *
 * Since: 0.11.9
 *
 */
void
vte_terminal_match_set_cursor_type(VteTerminal *terminal,
				   int tag, GdkCursorType cursor_type)
{
	GdkCursor *cursor;
	cursor = vte_terminal_cursor_new(terminal, cursor_type);
	vte_terminal_match_set_cursor(terminal, tag, cursor);
	gdk_cursor_unref(cursor);
}

/* Check if a given cell on the screen contains part of a matched string.  If
 * it does, return the string, and store the match tag in the optional tag
 * argument. */
static char *
vte_terminal_match_check_internal(VteTerminal *terminal,
				  long column, glong row,
				  int *tag, int *start, int *end)
{
	int i, j, ret, offset;
	struct vte_match_regex *regex = NULL;
	struct _VteCharAttributes *attr = NULL;
	gssize coffset;
	struct _vte_regex_match matches[256];
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_EVENTS)) {
		fprintf(stderr, "Checking for match at (%ld,%ld).\n",
			row, column);
	}
#endif
	if (tag != NULL) {
		*tag = -1;
	}
	if (start != NULL) {
		*start = 0;
	}
	if (end != NULL) {
		*end = 0;
	}
	g_assert(VTE_IS_TERMINAL(terminal));
	if (terminal->pvt->match_contents == NULL) {
		vte_terminal_match_contents_refresh(terminal);
	}
	/* Map the pointer position to a portion of the string. */
	for (offset = terminal->pvt->match_attributes->len - 1;
	     offset >= 0;
	     offset--) {
		attr = &g_array_index(terminal->pvt->match_attributes,
				      struct _VteCharAttributes,
				      offset);
		if ((row == attr->row) &&
		    (column == attr->column) &&
		    (terminal->pvt->match_contents[offset] != ' ')) {
			break;
		}
	}
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_EVENTS)) {
		if (offset < 0) {
			fprintf(stderr, "Cursor is not on a character.\n");
		} else {
			fprintf(stderr, "Cursor is on character %d.\n", offset);
		}
	}
#endif

	/* If the pointer isn't on a matchable character, bug out. */
	if (offset < 0) {
		terminal->pvt->match_previous = -1;
		return NULL;
	}

	/* If the pointer is on a newline, bug out. */
	if ((g_ascii_isspace(terminal->pvt->match_contents[offset])) ||
	    (terminal->pvt->match_contents[offset] == '\0')) {
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_EVENTS)) {
			fprintf(stderr, "Cursor is on whitespace.\n");
		}
#endif
		terminal->pvt->match_previous = -1;
		return NULL;
	}

	/* Now iterate over each regex we need to match against. */
	for (i = 0; i < terminal->pvt->match_regexes->len; i++) {
		regex = &g_array_index(terminal->pvt->match_regexes,
				       struct vte_match_regex,
				       i);
		/* Skip holes. */
		if (regex->tag < 0) {
			continue;
		}
		/* We'll only match the first item in the buffer which
		 * matches, so we'll have to skip each match until we
		 * stop getting matches. */
		coffset = 0;
		ret = _vte_regex_exec(regex->reg,
				      terminal->pvt->match_contents + coffset,
				      G_N_ELEMENTS(matches),
				      matches);
		while (ret == 0) {
			for (j = 0;
			     (j < G_N_ELEMENTS(matches)) &&
			     (matches[j].rm_so != -1);
			     j++) {
				/* The offsets should be "sane". */
				g_assert(matches[j].rm_so + coffset <
					 terminal->pvt->match_attributes->len);
				g_assert(matches[j].rm_eo + coffset <=
					 terminal->pvt->match_attributes->len);
#ifdef VTE_DEBUG
				if (_vte_debug_on(VTE_DEBUG_MISC)) {
					char *match;
					struct _VteCharAttributes *sattr, *eattr;
					match = g_strndup(terminal->pvt->match_contents + matches[j].rm_so + coffset,
							  matches[j].rm_eo - matches[j].rm_so);
					sattr = &g_array_index(terminal->pvt->match_attributes,
							       struct _VteCharAttributes,
							       matches[j].rm_so + coffset);
					eattr = &g_array_index(terminal->pvt->match_attributes,
							       struct _VteCharAttributes,
							       matches[j].rm_eo + coffset - 1);
					fprintf(stderr, "Match %d `%s' from %d(%ld,%ld) to %d(%ld,%ld) (%d).\n",
						j, match,
						matches[j].rm_so + coffset,
						sattr->column,
						sattr->row,
						matches[j].rm_eo + coffset - 1,
						eattr->column,
						eattr->row,
						offset);
					g_free(match);

				}
#endif
				/* Snip off any final newlines. */
				while ((matches[j].rm_eo > matches[j].rm_so) &&
				       (terminal->pvt->match_contents[coffset + matches[j].rm_eo - 1] == '\n')) {
					matches[j].rm_eo--;
				}
				/* If the pointer is in this substring,
				 * then we're done. */
				if ((offset >= (matches[j].rm_so + coffset)) &&
				    (offset < (matches[j].rm_eo + coffset))) {
					if (tag != NULL) {
						*tag = regex->tag;
					}
					if (start != NULL) {
						*start = coffset +
							 matches[j].rm_so;
					}
					if (end != NULL) {
						*end = coffset +
						       matches[j].rm_eo - 1;
					}
					if (GTK_WIDGET_REALIZED(GTK_WIDGET(terminal))) {
						gdk_window_set_cursor((GTK_WIDGET(terminal))->window,
								      regex->cursor);
					}
					terminal->pvt->match_previous = regex->tag;
					return g_strndup(terminal->pvt->match_contents + coffset + matches[j].rm_so,
							 matches[j].rm_eo - matches[j].rm_so);
				}
			}
			/* Skip past the beginning of this match to
			 * look for more. */
			coffset += (matches[0].rm_so + 1);
			ret = _vte_regex_exec(regex->reg,
					      terminal->pvt->match_contents +
					      coffset,
					      G_N_ELEMENTS(matches),
					      matches);
		}
	}
	terminal->pvt->match_previous = -1;
	return NULL;
}

/**
 * vte_terminal_match_check:
 * @terminal: a #VteTerminal
 * @column: the text column
 * @row: the text row
 * @tag: pointer to an integer
 *
 * Checks if the text in and around the specified position matches any of the
 * regular expressions previously set using vte_terminal_match_add().  If a
 * match exists, the text string is returned and if @tag is not NULL, the number
 * associated with the matched regular expression will be stored in @tag.
 *
 * If more than one regular expression has been set with
 * vte_terminal_match_add(), then expressions are checked in the order in
 * which they were added.
 *
 * Returns: a string which matches one of the previously set regular
 * expressions, and which must be freed by the caller.
 */
char *
vte_terminal_match_check(VteTerminal *terminal, glong column, glong row,
			 int *tag)
{
	long delta;
	char *ret;
	g_return_val_if_fail(VTE_IS_TERMINAL(terminal), NULL);
	delta = terminal->pvt->screen->scroll_delta;
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_EVENTS)) {
		fprintf(stderr, "Checking for match at (%ld,%ld).\n",
			row, column);
	}
#endif
	ret = vte_terminal_match_check_internal(terminal,
						column, row + delta,
						tag, NULL, NULL);
#ifdef VTE_DEBUG
	if ((ret != NULL) && _vte_debug_on(VTE_DEBUG_EVENTS)) {
		fprintf(stderr, "Matched `%s'.\n", ret);
	}
#endif
	return ret;
}

/* Emit an adjustment changed signal on our adjustment object. */
static gboolean
vte_terminal_emit_adjustment_changed(gpointer data)
{
	VteTerminal *terminal;
	terminal = VTE_TERMINAL(data);
	if (terminal->pvt->adjustment_changed_tag) {
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_SIGNALS)) {
			fprintf(stderr, "Emitting adjustment_changed.\n");
		}
#endif
		terminal->pvt->adjustment_changed_tag = 0;
		gtk_adjustment_changed(terminal->adjustment);
	}
	return FALSE;
}

/* Queue an adjustment-changed signal to be delivered when convenient. */
static void
vte_terminal_queue_adjustment_changed(VteTerminal *terminal)
{
	if (terminal->pvt->adjustment_changed_tag == 0) {
		terminal->pvt->adjustment_changed_tag =
				g_idle_add_full(VTE_ADJUSTMENT_PRIORITY,
						vte_terminal_emit_adjustment_changed,
						terminal,
						NULL);
	} else {
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_EVENTS)) {
			fprintf(stderr, "Swallowing duplicate "
				"adjustment-changed signal.\n");
		}
#endif
	}
}

/* Update the adjustment field of the widget.  This function should be called
 * whenever we add rows to or remove rows from the history or switch screens. */
void
_vte_terminal_adjust_adjustments(VteTerminal *terminal, gboolean immediate)
{
	VteScreen *screen;
	gboolean changed;
	long delta;
	long rows;

	g_assert(terminal->pvt->screen != NULL);
	g_assert(terminal->pvt->screen->row_data != NULL);

	/* Adjust the vertical, uh, adjustment. */
	changed = FALSE;

	/* The lower value should be the first row in the buffer. */
	screen = terminal->pvt->screen;
	delta = _vte_ring_delta(screen->row_data);
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_IO)) {
		fprintf(stderr, "Changing adjustment values "
			"(delta = %ld, scroll = %ld).\n",
			delta, screen->scroll_delta);
	}
#endif
	if (terminal->adjustment->lower != delta) {
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_IO)) {
			fprintf(stderr, "Changing lower bound from %lf to %ld\n",
				terminal->adjustment->lower,
				delta);

		}
#endif 

		terminal->adjustment->lower = delta;
		changed = TRUE;
	}

	/* Snap the insert delta and the cursor position to be in the visible
	 * area.  Leave the scrolling delta alone because it will be updated
	 * when the adjustment changes. */
	screen->insert_delta = MAX(screen->insert_delta, delta);
	screen->cursor_current.row = MAX(screen->cursor_current.row,
					 screen->insert_delta);

	/* The upper value is the number of rows which might be visible.  (Add
	 * one to the cursor offset because it's zero-based.) */
	rows = MAX(_vte_ring_next(terminal->pvt->screen->row_data),
		   terminal->pvt->screen->cursor_current.row + 1);
	if (terminal->adjustment->upper != rows) {
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_IO)) {
			fprintf(stderr, "Changing upper bound from %f to %ld\n",
				terminal->adjustment->upper,
				rows);

		}
#endif 

		terminal->adjustment->upper = rows;
		changed = TRUE;
	}

	/* The step increment should always be one. */
	if (terminal->adjustment->step_increment != 1) {
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_IO)) {
			fprintf(stderr, "Changing step increment from %lf to %ld\n",
				terminal->adjustment->step_increment,
				terminal->row_count);

		}
#endif 

		terminal->adjustment->step_increment = 1;
		changed = TRUE;
	}

	/* Set the number of rows the user sees to the number of rows the
	 * user sees. */
	if (terminal->adjustment->page_size != terminal->row_count) {
#ifdef VTE_DEBUG
	      if (_vte_debug_on(VTE_DEBUG_IO)) {
		    fprintf(stderr, "Changing page size from %f to %ld\n",
			    terminal->adjustment->page_size,
			    terminal->row_count);

	      }
#endif 

		terminal->adjustment->page_size = terminal->row_count;
		changed = TRUE;
	}

	/* Clicking in the empty area should scroll one screen, so set the
	 * page size to the number of visible rows. */
	if (terminal->adjustment->page_increment != terminal->row_count) {
#ifdef VTE_DEBUG
	      if (_vte_debug_on(VTE_DEBUG_IO)) {
		    fprintf(stderr, "Changing page increment from "
			    "%f to %ld\n", 
			    terminal->adjustment->page_increment,
			    terminal->row_count);

	      }
#endif 

		terminal->adjustment->page_increment = terminal->row_count;
		changed = TRUE;
	}

	/* Set the scrollbar adjustment to where the screen wants it to be. */
	if (floor(terminal->adjustment->value) !=
	    screen->scroll_delta) {
#ifdef VTE_DEBUG
	      if (_vte_debug_on(VTE_DEBUG_IO)) {
		    fprintf(stderr, "Changing adjustment scroll position: "
			    "%ld\n", screen->scroll_delta);
	      }
#endif 
		/* This emits a "value-changed" signal, so no need to screw
		 * with anything else for just this. */
		gtk_adjustment_set_value(terminal->adjustment,
				       screen->scroll_delta);

#ifdef VTE_DEBUG
	      if (_vte_debug_on(VTE_DEBUG_IO)) {
		    fprintf(stderr, "Changed adjustment scroll position: "
			    "%ld\n", screen->scroll_delta);
	      }
#endif 

	}

	/* If anything changed, signal that there was a change. */
	if (changed == TRUE) {
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_IO)) {
			fprintf(stderr, "Changed adjustment values "
				"(delta = %ld, scroll = %ld).\n",
				delta, terminal->pvt->screen->scroll_delta);
		}
#endif
		if (immediate) {
			gtk_adjustment_changed(terminal->adjustment);
		} else {
			vte_terminal_queue_adjustment_changed(terminal);
		}
	}
}

/* Scroll up or down in the current screen. */
static void
vte_terminal_scroll_pages(VteTerminal *terminal, gint pages)
{
	glong destination;
	g_assert(VTE_IS_TERMINAL(terminal));
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_IO)) {
		fprintf(stderr, "Scrolling %d pages.\n", pages);
	}
#endif
	/* Calculate the ideal position where we want to be before clamping. */
	destination = floor(gtk_adjustment_get_value(terminal->adjustment));
	destination += (pages * terminal->row_count);
	/* Can't scroll past data we have. */
	destination = CLAMP(destination,
			    terminal->adjustment->lower,
			    terminal->adjustment->upper - terminal->row_count);
	/* Tell the scrollbar to adjust itself. */
	gtk_adjustment_set_value(terminal->adjustment, destination);
	/* Clear dingus match set. */
	_vte_terminal_match_contents_clear(terminal);
	/* Notify viewers that the contents have changed. */
	_vte_terminal_emit_contents_changed(terminal);
}

/* Scroll so that the scroll delta is the minimum value. */
static void
vte_terminal_maybe_scroll_to_top(VteTerminal *terminal)
{
	long delta;
	g_assert(VTE_IS_TERMINAL(terminal));
	if (floor(gtk_adjustment_get_value(terminal->adjustment)) !=
	    _vte_ring_delta(terminal->pvt->screen->row_data)) {
		delta = _vte_ring_delta(terminal->pvt->screen->row_data);
		gtk_adjustment_set_value(terminal->adjustment, delta);
	}
}

static void
vte_terminal_maybe_scroll_to_bottom(VteTerminal *terminal)
{
	glong delta;
	g_assert(VTE_IS_TERMINAL(terminal));
	if ((terminal->pvt->screen->scroll_delta !=
	    terminal->pvt->screen->insert_delta)) {
		delta = terminal->pvt->screen->insert_delta;
		gtk_adjustment_set_value(terminal->adjustment, delta);

#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_IO)) {
		      fprintf (stderr, "Snapping to bottom of screen\n");
		}
#endif
	}
}

/**
 * vte_terminal_set_encoding:
 * @terminal: a #VteTerminal
 * @codeset: a valid #g_iconv target
 *
 * Changes the encoding the terminal will expect data from the child to
 * be encoded with.  For certain terminal types, applications executing in the
 * terminal can change the encoding.  The default encoding is defined by the
 * application's locale settings.
 *
 */
void
vte_terminal_set_encoding(VteTerminal *terminal, const char *codeset)
{
	const char *old_codeset;
	GQuark encoding_quark;
	VteConv conv;
	char *obuf1, *obuf2;
	gsize bytes_written;

	old_codeset = terminal->pvt->encoding;
	if (codeset == NULL) {
		g_get_charset(&codeset);
	}
	if ((old_codeset != NULL) && (strcmp(codeset, old_codeset) == 0)) {
		/* Nothing to do! */
		return;
	}

	/* Open new conversions. */
	conv = _vte_conv_open(codeset, "UTF-8");
	if (conv == ((VteConv) -1)) {
		g_warning(_("Unable to convert characters from %s to %s."),
			  "UTF-8", codeset);
		return;
	}
	if (terminal->pvt->outgoing_conv != (VteConv) -1) {
		_vte_conv_close(terminal->pvt->outgoing_conv);
	}
	terminal->pvt->outgoing_conv = conv;

	/* Set the terminal's encoding to the new value. */
	encoding_quark = g_quark_from_string(codeset);
	terminal->pvt->encoding = g_quark_to_string(encoding_quark);
	_vte_pty_set_utf8(terminal->pvt->pty_master,
			  (strcmp(codeset, "UTF-8") == 0));

	/* Convert any buffered output bytes. */
	if ((_vte_buffer_length(terminal->pvt->outgoing) > 0) &&
	    (old_codeset != NULL)) {
		/* Convert back to UTF-8. */
		obuf1 = g_convert(terminal->pvt->outgoing->bytes,
				  _vte_buffer_length(terminal->pvt->outgoing),
				  "UTF-8",
				  old_codeset,
				  NULL,
				  &bytes_written,
				  NULL);
		if (obuf1 != NULL) {
			/* Convert to the new encoding. */
			obuf2 = g_convert(obuf1,
					  bytes_written,
					  codeset,
					  "UTF-8",
					  NULL,
					  &bytes_written,
					  NULL);
			if (obuf2 != NULL) {
				_vte_buffer_clear(terminal->pvt->outgoing);
				_vte_buffer_append(terminal->pvt->outgoing,
						   obuf2, bytes_written);
				g_free(obuf2);
			}
			g_free(obuf1);
		}
	}

	/* Set the encoding for incoming text. */
	_vte_iso2022_state_set_codeset(terminal->pvt->iso2022,
				       terminal->pvt->encoding);

#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_IO)) {
		fprintf(stderr, "Set terminal encoding to `%s'.\n",
			terminal->pvt->encoding);
	}
#endif
	vte_terminal_emit_encoding_changed(terminal);
}

/**
 * vte_terminal_get_encoding:
 * @terminal: a #VteTerminal
 *
 * Determines the name of the encoding in which the terminal expects data to be
 * encoded.
 *
 * Returns: the current encoding for the terminal.
 */
const char *
vte_terminal_get_encoding(VteTerminal *terminal)
{
	g_return_val_if_fail(VTE_IS_TERMINAL(terminal), NULL);
	return terminal->pvt->encoding;
}

/* Make sure we have enough rows and columns to hold data at the current
 * cursor position. */
void
_vte_terminal_ensure_cursor(VteTerminal *terminal, gboolean current)
{
	VteRowData *row;
	VteScreen *screen;
	gboolean readjust = FALSE, fill = FALSE;

	/* Must make sure we're in a sane area. */
	screen = terminal->pvt->screen;

	/* Figure out how many rows we need to add. */
	fill = (terminal->pvt->screen->defaults.back != VTE_DEF_BG);
	while (screen->cursor_current.row >= _vte_ring_next(screen->row_data)) {
		/* Create a new row. */
		if (fill) {
			row = _vte_new_row_data_sized(terminal, TRUE);
		} else {
			row = _vte_new_row_data(terminal);
		}
		_vte_ring_append(screen->row_data, row);
		readjust = TRUE;
	}
	if (readjust) {
		_vte_terminal_adjust_adjustments(terminal, FALSE);
	}

	/* Find the row the cursor is in. */
	row = _vte_ring_index(screen->row_data,
			      VteRowData *,
			      screen->cursor_current.row);
	g_assert(row != NULL);
	if ((row->cells->len <= screen->cursor_current.col) &&
	    (row->cells->len < terminal->column_count)) {
		/* Set up defaults we'll use when adding new cells. */
		if (current) {
			/* Add new cells until we have one here. */
			vte_g_array_fill(row->cells,
					 &screen->color_defaults,
					 screen->cursor_current.col + 1);
		} else {
			/* Add enough cells at the end to make sure we have
			 * enough for all visible columns. */
			vte_g_array_fill(row->cells,
					 &screen->basic_defaults,
					 screen->cursor_current.col + 1);
		}
	}
}

/* Update the insert delta so that the screen which includes it also
 * includes the end of the buffer. */
void
_vte_terminal_update_insert_delta(VteTerminal *terminal)
{
	long delta, rows;
	VteScreen *screen;

	screen = terminal->pvt->screen;

	/* The total number of lines.  Add one to the cursor offset
	 * because it's zero-based. */
	rows = MAX(_vte_ring_next(terminal->pvt->screen->row_data),
		   terminal->pvt->screen->cursor_current.row + 1);

	/* Make sure that the bottom row is visible, and that it's in
	 * the buffer (even if it's empty).  This usually causes the
	 * top row to become a history-only row. */
	delta = screen->insert_delta;
	delta = MIN(delta, rows - terminal->row_count);
	delta = MAX(delta,
		    screen->cursor_current.row - (terminal->row_count - 1));
	delta = MAX(delta, _vte_ring_delta(screen->row_data));

	/* Adjust the insert delta and scroll if needed. */
	if (delta != screen->insert_delta) {
		_vte_terminal_ensure_cursor(terminal, FALSE);
		screen->insert_delta = delta;
		_vte_terminal_adjust_adjustments(terminal, TRUE);
	}
}

/* Show or hide the pointer. */
void
_vte_terminal_set_pointer_visible(VteTerminal *terminal, gboolean visible)
{
	GdkCursor *cursor = NULL;
	struct vte_match_regex *regex = NULL;
	if (visible || !terminal->pvt->mouse_autohide) {
		if (terminal->pvt->mouse_send_xy_on_click ||
		    terminal->pvt->mouse_send_xy_on_button ||
		    terminal->pvt->mouse_hilite_tracking ||
		    terminal->pvt->mouse_cell_motion_tracking ||
		    terminal->pvt->mouse_all_motion_tracking) {
#ifdef VTE_DEBUG
			if (_vte_debug_on(VTE_DEBUG_CURSOR)) {
				fprintf(stderr, "Setting mousing cursor.\n");
			}
#endif
			cursor = terminal->pvt->mouse_mousing_cursor;
		} else
		if ((terminal->pvt->match_previous > -1) &&
		    (terminal->pvt->match_previous < terminal->pvt->match_regexes->len)) {
			regex = &g_array_index(terminal->pvt->match_regexes,
					       struct vte_match_regex,
					       terminal->pvt->match_previous);
			cursor = regex->cursor;
		} else {
#ifdef VTE_DEBUG
			if (_vte_debug_on(VTE_DEBUG_CURSOR)) {
				fprintf(stderr, "Setting default mouse "
					"cursor.\n");
			}
#endif
			cursor = terminal->pvt->mouse_default_cursor;
		}
	} else {
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_CURSOR)) {
			fprintf(stderr, "Setting to invisible cursor.\n");
		}
#endif
		cursor = terminal->pvt->mouse_inviso_cursor;
	}
	if (cursor) {
		if (GTK_WIDGET_REALIZED(GTK_WIDGET(terminal))) {
			gdk_window_set_cursor((GTK_WIDGET(terminal))->window,
					      cursor);
		}
	}
	terminal->pvt->mouse_cursor_visible = visible;
}

/**
 * vte_terminal_new:
 *
 * Create a new terminal widget.
 *
 * Returns: a new #VteTerminal object
 */
GtkWidget *
vte_terminal_new(void)
{
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_LIFECYCLE)) {
		fprintf(stderr, "vte_terminal_new()\n");
	}
#endif

	return GTK_WIDGET(g_object_new(vte_terminal_get_type(), NULL));
}

/* Set up a palette entry with a more-or-less match for the requested color. */
static void
vte_terminal_set_color_internal(VteTerminal *terminal, int entry,
				const GdkColor *proposed)
{
	GtkWidget *widget;

	g_assert(VTE_IS_TERMINAL(terminal));
	g_assert(entry >= 0);
	g_assert(entry < G_N_ELEMENTS(terminal->pvt->palette));

	/* Save the requested color. */
	terminal->pvt->palette[entry].red = proposed->red;
	terminal->pvt->palette[entry].green = proposed->green;
	terminal->pvt->palette[entry].blue = proposed->blue;

	/* If we're not realized yet, there's nothing else to do. */
	widget = GTK_WIDGET(terminal);
	if (!GTK_WIDGET_REALIZED(widget)) {
		return;
	}

	/* If we're setting the background color, set the background color
	 * on the widget as well. */
	if ((entry == VTE_DEF_BG)) {
		vte_terminal_queue_background_update(terminal);
	}
}

static void
vte_terminal_generate_bold(const struct vte_palette_entry *foreground,
			   const struct vte_palette_entry *background,
			   double factor,
			   GdkColor *bold)
{
	double fy, fcb, fcr, by, bcb, bcr, r, g, b;
	g_assert(foreground != NULL);
	g_assert(background != NULL);
	g_assert(bold != NULL);
	fy =   0.2990 * foreground->red +
	       0.5870 * foreground->green +
	       0.1140 * foreground->blue;
	fcb = -0.1687 * foreground->red +
	      -0.3313 * foreground->green +
	       0.5000 * foreground->blue;
	fcr =  0.5000 * foreground->red +
	      -0.4187 * foreground->green +
	      -0.0813 * foreground->blue;
	by =   0.2990 * background->red +
	       0.5870 * background->green +
	       0.1140 * background->blue;
	bcb = -0.1687 * background->red +
	      -0.3313 * background->green +
	       0.5000 * background->blue;
	bcr =  0.5000 * background->red +
	      -0.4187 * background->green +
	      -0.0813 * background->blue;
	fy = (factor * fy) + ((1.0 - factor) * by);
	fcb = (factor * fcb) + ((1.0 - factor) * bcb);
	fcr = (factor * fcr) + ((1.0 - factor) * bcr);
	r = fy + 1.402 * fcr;
	g = fy + 0.34414 * fcb - 0.71414 * fcr;
	b = fy + 1.722 * fcb;
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_MISC)) {
		fprintf(stderr, "Calculated bold (%d, %d, %d) = (%lf,%lf,%lf)",
			foreground->red, foreground->green, foreground->blue,
			r, g, b);
	}
#endif
	bold->red = CLAMP(r, 0, 0xffff);
	bold->green = CLAMP(g, 0, 0xffff);
	bold->blue = CLAMP(b, 0, 0xffff);
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_MISC)) {
		fprintf(stderr, "= (%04x,%04x,%04x).\n",
			bold->red, bold->green, bold->blue);
	}
#endif
}

/**
 * vte_terminal_set_color_bold
 * @terminal: a #VteTerminal
 * @bold: the new bold color
 *
 * Sets the color used to draw bold text in the default foreground color.
 *
 */
void
vte_terminal_set_color_bold(VteTerminal *terminal, const GdkColor *bold)
{
	vte_terminal_set_color_internal(terminal, VTE_BOLD_FG, bold);
}

/**
 * vte_terminal_set_color_dim
 * @terminal: a #VteTerminal
 * @dim: the new dim color
 *
 * Sets the color used to draw dim text in the default foreground color.
 *
 */
void
vte_terminal_set_color_dim(VteTerminal *terminal, const GdkColor *dim)
{
	vte_terminal_set_color_internal(terminal, VTE_DIM_FG, dim);
}

/**
 * vte_terminal_set_color_foreground
 * @terminal: a #VteTerminal
 * @foreground: the new foreground color
 *
 * Sets the foreground color used to draw normal text
 *
 */
void
vte_terminal_set_color_foreground(VteTerminal *terminal,
				  const GdkColor *foreground)
{
	vte_terminal_set_color_internal(terminal, VTE_DEF_FG, foreground);
}

/**
 * vte_terminal_set_color_background
 * @terminal: a #VteTerminal
 * @background: the new background color
 *
 * Sets the background color for text which does not have a specific background
 * color assigned.  Only has effect when no background image is set and when
 * the terminal is not transparent.
 *
 */
void
vte_terminal_set_color_background(VteTerminal *terminal,
				  const GdkColor *background)
{
	vte_terminal_set_color_internal(terminal, VTE_DEF_BG, background);
}

/**
 * vte_terminal_set_color_cursor
 * @terminal: a #VteTerminal
 * @cursor_background: the new color to use for the text cursor
 *
 * Sets the background color for text which is under the cursor.  If NULL, text
 * under the cursor will be drawn with foreground and background colors
 * reversed.
 *
 * Since: 0.11.11
 *
 */
void
vte_terminal_set_color_cursor(VteTerminal *terminal,
			      const GdkColor *cursor_background)
{
	if (cursor_background != NULL) {
		vte_terminal_set_color_internal(terminal, VTE_CUR_BG,
						cursor_background);
		terminal->pvt->cursor_color_set = TRUE;
	} else {
		terminal->pvt->cursor_color_set = FALSE;
	}
}

/**
 * vte_terminal_set_color_highlight
 * @terminal: a #VteTerminal
 * @highlight_background: the new color to use for highlighted text
 *
 * Sets the background color for text which is highlighted.  If NULL,
 * highlighted text (which is usually highlighted because it is selected) will
 * be drawn with foreground and background colors reversed.
 *
 * Since: 0.11.11
 *
 */
void
vte_terminal_set_color_highlight(VteTerminal *terminal,
				 const GdkColor *highlight_background)
{
	if (highlight_background != NULL) {
		vte_terminal_set_color_internal(terminal, VTE_DEF_HL,
						highlight_background);
		terminal->pvt->highlight_color_set = TRUE;
	} else {
		terminal->pvt->highlight_color_set = FALSE;
	}
}

/**
 * vte_terminal_set_colors
 * @terminal: a #VteTerminal
 * @foreground: the new foreground color, or #NULL
 * @background: the new background color, or #NULL
 * @palette: the color palette
 * @palette_size: the number of entries in @palette
 *
 * The terminal widget uses a 28-color model comprised of the default foreground
 * and background colors, the bold foreground color, the dim foreground
 * color, an eight color palette, bold versions of the eight color palette,
 * and a dim version of the the eight color palette.
 *
 * @palette_size must be either 0, 8, 16, or 24.  If @foreground is NULL and
 * @palette_size is greater than 0, the new foreground color is taken from
 * @palette[7].  If @background is NULL and @palette_size is greater than 0,
 * the new background color is taken from @palette[0].  If
 * @palette_size is 8 or 16, the third (dim) and possibly the second (bold)
 * 8-color palettes are extrapolated from the new background color and the items
 * in @palette.
 *
 */
void
vte_terminal_set_colors(VteTerminal *terminal,
			const GdkColor *foreground,
			const GdkColor *background,
			const GdkColor *palette,
			glong palette_size)
{
	int i;
	GdkColor color;

	g_return_if_fail(VTE_IS_TERMINAL(terminal));

	g_return_if_fail(palette_size >= 0);
	g_return_if_fail((palette_size == 0) ||
			 (palette_size == 8) ||
			 (palette_size == 16) ||
			 (palette_size == G_N_ELEMENTS(terminal->pvt->palette)));

	/* Accept NULL as the default foreground and background colors if we
	 * got a palette. */
	if ((foreground == NULL) && (palette_size >= 8)) {
		foreground = &palette[7];
	}
	if ((background == NULL) && (palette_size >= 8)) {
		background = &palette[0];
	}

	memset(&color, 0, sizeof(color));

	/* Initialize each item in the palette if we got any entries to work
	 * with. */
	for (i = 0; (i < G_N_ELEMENTS(terminal->pvt->palette)); i++) {
		switch (i) {
		case VTE_DEF_FG:
			if (foreground != NULL) {
				color = *foreground;
			} else {
				color.red = 0xc000;
				color.blue = 0xc000;
				color.green = 0xc000;
			}
			break;
		case VTE_DEF_BG:
			if (background != NULL) {
				color = *background;
			} else {
				color.red = 0;
				color.blue = 0;
				color.green = 0;
			}
			break;
		case VTE_BOLD_FG:
			vte_terminal_generate_bold(&terminal->pvt->palette[VTE_DEF_FG],
						   &terminal->pvt->palette[VTE_DEF_BG],
						   1.8,
						   &color);
			break;
		case VTE_DIM_FG:
			vte_terminal_generate_bold(&terminal->pvt->palette[VTE_DEF_FG],
						   &terminal->pvt->palette[VTE_DEF_BG],
						   0.5,
						   &color);
			break;
		case VTE_DEF_HL:
			color.red = 0xc000;
			color.blue = 0xc000;
			color.green = 0xc000;
			break;
		case VTE_CUR_BG:
			color.red = 0x0000;
			color.blue = 0x0000;
			color.green = 0x0000;
			break;
		case 0 + 0:
		case 0 + 1:
		case 0 + 2:
		case 0 + 3:
		case 0 + 4:
		case 0 + 5:
		case 0 + 6:
		case 0 + 7:
		case 8 + 0:
		case 8 + 1:
		case 8 + 2:
		case 8 + 3:
		case 8 + 4:
		case 8 + 5:
		case 8 + 6:
		case 8 + 7:
			color.blue = (i & 4) ? 0xc000 : 0;
			color.green = (i & 2) ? 0xc000 : 0;
			color.red = (i & 1) ? 0xc000 : 0;
			if (i > 8) {
				color.blue += 0x3fff;
				color.green += 0x3fff;
				color.red += 0x3fff;
			}
			break;
		case 16 + 0:
		case 16 + 1:
		case 16 + 2:
		case 16 + 3:
		case 16 + 4:
		case 16 + 5:
		case 16 + 6:
		case 16 + 7:
			color.blue = (i & 4) ? 0x8000 : 0;
			color.green = (i & 2) ? 0x8000 : 0;
			color.red = (i & 1) ? 0x8000 : 0;
			break;
		default:
			g_assert_not_reached();
			break;
		}

		/* Override from the supplied palette if there is one. */
		if (i < palette_size) {
			color = palette[i];
		}

		/* Set up the color entry. */
		vte_terminal_set_color_internal(terminal, i, &color);
	}

	/* We may just have changed the default background color, so queue
	 * a repaint of the entire viewable area. */
	_vte_invalidate_all(terminal);

	/* Track that we had a color palette set. */
	terminal->pvt->palette_initialized = TRUE;
}

/**
 * vte_terminal_set_default_colors:
 * @terminal: a #VteTerminal
 *
 * Reset the terminal palette to reasonable compiled-in defaults.
 *
 */
void
vte_terminal_set_default_colors(VteTerminal *terminal)
{
	vte_terminal_set_colors(terminal, NULL, NULL, NULL, 0);
}

/* Insert a single character into the stored data array. */
void
_vte_terminal_insert_char(VteTerminal *terminal, gunichar c,
			 gboolean force_insert_mode, gboolean invalidate_now,
			 gboolean paint_cells, gboolean ensure_after,
			 gint forced_width)
{
	VteRowData *row;
	struct vte_charcell cell, *pcell;
	int columns, i;
	long col;
	VteScreen *screen;
	gboolean insert, clean;

	screen = terminal->pvt->screen;
	insert = screen->insert_mode || force_insert_mode;
	invalidate_now = insert || invalidate_now;

	/* If we've enabled the special drawing set, map the characters to
	 * Unicode. */
	if (screen->defaults.alternate) {
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_SUBSTITUTION)) {
			fprintf(stderr, "Attempting charset substitution"
				"for 0x%04x.\n", c);
		}
#endif
		/* See if there's a mapping for it. */
		cell.c = _vte_iso2022_process_single(terminal->pvt->iso2022,
						     c, '0');
		if (cell.c != c) {
			forced_width = _vte_iso2022_get_encoded_width(cell.c);
			c = cell.c & ~(VTE_ISO2022_ENCODED_WIDTH_MASK);
		}
	}

	/* If this character is destined for the status line, save it. */
	if (terminal->pvt->screen->status_line) {
		g_string_append_unichar(terminal->pvt->screen->status_line_contents,
					c);
		_vte_terminal_emit_status_line_changed(terminal);
		return;
	}

	/* Figure out how many columns this character should occupy. */
	if (forced_width == 0) {
		if (VTE_ISO2022_HAS_ENCODED_WIDTH(c)) {
			columns = _vte_iso2022_get_encoded_width(c);
		} else {
			columns = _vte_iso2022_unichar_width(c);
		}
	} else {
		columns = MIN(forced_width, 1);
	}
	c &= ~(VTE_ISO2022_ENCODED_WIDTH_MASK);

#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_IO) && _vte_debug_on(VTE_DEBUG_PARSE)) {
		fprintf(stderr, "Inserting %ld %c (%d/%d)(%d), delta = %ld, ",
			(long)c,
			c < 256 ? c : ' ',
			screen->defaults.fore, screen->defaults.back,
			columns, (long)screen->insert_delta);
	}
#endif

	/* If we're autowrapping here, do it. */
	col = screen->cursor_current.col;
	if (col + columns > terminal->column_count) {
		if (terminal->pvt->flags.am) {
			/* Mark this line as soft-wrapped. */
			row = _vte_ring_index(screen->row_data,
					      VteRowData *,
					      screen->cursor_current.row);
			if (row != NULL) {
				row->soft_wrapped = 1;
			}
			/* Wrap. */
			_vte_sequence_handler_sf(terminal, NULL, 0, NULL);
			screen->cursor_current.col = 0;
		} else {
			/* Don't wrap, stay at the rightmost column. */
			screen->cursor_current.col = terminal->column_count -
						     columns;
		}
	}

	/* Make sure we have enough rows to hold this data. */
	_vte_terminal_ensure_cursor(terminal, FALSE);

	/* Get a handle on the array for the insertion row. */
	row = _vte_ring_index(screen->row_data,
			      VteRowData *,
			      screen->cursor_current.row);
	g_assert(row != NULL);

	/* Insert the right number of columns. */
	for (i = 0; i < columns; i++) {
		col = screen->cursor_current.col;

		/* Make sure we have enough columns in this row. */
		if (row->cells->len <= col) {
			/* Add enough cells to fill out the row to at least out
			 * to (and including) the insertion point. */
			if (paint_cells) {
				vte_g_array_fill(row->cells,
						 &screen->color_defaults,
						 col + 1);
			} else {
				vte_g_array_fill(row->cells,
						 &screen->basic_defaults,
						 col + 1);
			}
			clean = FALSE;
		} else {
			/* If we're in insert mode, insert a new cell here
			 * and use it. */
			if (insert) {
				cell = screen->color_defaults;
				g_array_insert_val(row->cells, col, cell);
				clean = FALSE;
			} else {
				/* We're in overtype mode, so we can use the
				 * existing character. */
				clean = TRUE;
			}
		}

		/* Set the character cell's attributes to match the current
		 * defaults, preserving any previous contents. */
		cell = g_array_index(row->cells, struct vte_charcell, col);
		pcell = &g_array_index(row->cells, struct vte_charcell, col);
		*pcell = screen->defaults;
		if (!paint_cells) {
			pcell->fore = cell.fore;
			pcell->back = cell.back;
		}
		pcell->c = cell.c;
		pcell->columns = cell.columns;
		pcell->fragment = cell.fragment;
		pcell->alternate = 0;

		/* Now set the character and column count. */
		if (i == 0) {
			/* This is an entire character or the first column of
			 * a multi-column character. */
			if ((pcell->c != 0) &&
			    (c == '_') &&
			    (terminal->pvt->flags.ul)) {
				/* Handle overstrike-style underlining. */
				pcell->underline = 1;
			} else {
				/* Insert the character. */
				pcell->c = c;
				pcell->columns = columns;
				pcell->fragment = 0;
			}
		} else {
			/* This is a continuation cell. */
			pcell->c = c;
			pcell->columns = columns;
			pcell->fragment = 1;
		}

		/* And take a step to the to the right. */
		screen->cursor_current.col++;

		/* Make sure we're not getting random stuff past the right
		 * edge of the screen at this point, because the user can't
		 * see it. */
		if (row->cells->len > terminal->column_count) {
			g_array_set_size(row->cells, terminal->column_count);
		}
	}

	/* If we're autowrapping *here*, do it. */
	col = screen->cursor_current.col;
	if (col >= terminal->column_count) {
		if (terminal->pvt->flags.am && !terminal->pvt->flags.xn) {
			/* Mark this line as soft-wrapped. */
			row = _vte_ring_index(screen->row_data,
					      VteRowData *,
					      screen->cursor_current.row);
			if (row != NULL) {
				row->soft_wrapped = 1;
			}
			/* Wrap. */
			_vte_sequence_handler_sf(terminal, NULL, 0, NULL);
			screen->cursor_current.col = 0;
		}
	}

	/* Signal that this part of the window needs drawing. */
	if (invalidate_now) {
		col = screen->cursor_current.col - columns;
		if (insert) {
			_vte_invalidate_cells(terminal,
					     col, terminal->column_count - col,
					     screen->cursor_current.row, 1);
		} else {
			_vte_invalidate_cells(terminal,
					     col, columns,
					     screen->cursor_current.row, 1);
		}
	}

	/* Make sure the location the cursor is on exists. */
	if (ensure_after) {
		_vte_terminal_ensure_cursor(terminal, FALSE);
	}

	/* We added text, so make a note of it. */
	terminal->pvt->text_inserted_count++;

#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_IO) && _vte_debug_on(VTE_DEBUG_PARSE)) {
		fprintf(stderr, "insertion delta => %ld.\n",
			(long)screen->insert_delta);
	}
#endif
}

#ifdef VTE_DEBUG
static void
display_control_sequence(const char *name, GValueArray *params)
{
	/* Display the control sequence with its parameters, to
	 * help me debug this thing.  I don't have all of the
	 * sequences implemented yet. */
	guint i;
	long l;
	const char *s;
	const gunichar *w;
	GValue *value;
	fprintf(stderr, "%s(", name);
	if (params != NULL) {
		for (i = 0; i < params->n_values; i++) {
			value = g_value_array_get_nth(params, i);
			if (i > 0) {
				fprintf(stderr, ", ");
			}
			if (G_VALUE_HOLDS_LONG(value)) {
				l = g_value_get_long(value);
				fprintf(stderr, "%ld", l);
			} else
			if (G_VALUE_HOLDS_STRING(value)) {
				s = g_value_get_string(value);
				fprintf(stderr, "\"%s\"", s);
			} else
			if (G_VALUE_HOLDS_POINTER(value)) {
				w = g_value_get_pointer(value);
				fprintf(stderr, "\"%ls\"", (const wchar_t*) w);
			}
		}
	}
	fprintf(stderr, ")\n");
}
#endif

/* Handle a terminal control sequence and its parameters. */
static gboolean
vte_terminal_handle_sequence(GtkWidget *widget,
			     const char *match_s,
			     GQuark match,
			     GValueArray *params)
{
	VteTerminal *terminal;
	VteTerminalSequenceHandler handler;
	VteScreen *screen;
	struct vte_cursor_position position;
	gboolean ret;

	g_assert(widget != NULL);
	g_assert(VTE_IS_TERMINAL(widget));
	terminal = VTE_TERMINAL(widget);
	screen = terminal->pvt->screen;

	/* This may generate multiple redraws, so freeze it while we do them. */
	if (GTK_WIDGET_REALIZED (widget)) {
		gdk_window_freeze_updates(widget->window);
	}

	/* Save the cursor's current position for future use. */
	position = screen->cursor_current;

	/* Find the handler for this control sequence. */
	handler = _vte_sequence_get_handler (match_s, match);

#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_PARSE)) {
		display_control_sequence(match_s, params);
	}
#endif
	if (handler != NULL) {
		/* Let the handler handle it. */
		ret = handler(terminal, match_s, match, params);
	} else {
		g_warning(_("No handler for control sequence `%s' defined."),
			  match_s);
		ret = FALSE;
	}

	/* Let the updating begin. */
	if (GTK_WIDGET_REALIZED (widget)) {
		gdk_window_thaw_updates(widget->window);
	}

	return ret;
}

/* Catch a VteReaper child-exited signal, and if it matches the one we're
 * looking for, emit one of our own. */
static void
vte_terminal_catch_child_exited(VteReaper *reaper, int pid, int status,
				VteTerminal *data)
{
	VteTerminal *terminal;
	g_assert(VTE_IS_TERMINAL(data));
	terminal = VTE_TERMINAL(data);
	if (pid == terminal->pvt->pty_pid) {
		/* Disconnect from the reaper. */
		if (VTE_IS_REAPER(terminal->pvt->pty_reaper)) {
			g_signal_handlers_disconnect_by_func(terminal->pvt->pty_reaper,
							     (gpointer)vte_terminal_catch_child_exited,
							     terminal);
			g_object_unref(G_OBJECT(terminal->pvt->pty_reaper));
		}
		terminal->pvt->pty_reaper = NULL;
		terminal->pvt->pty_pid = -1;

		/* Close out the PTY. */
		_vte_terminal_disconnect_pty_read(terminal);
		_vte_terminal_disconnect_pty_write(terminal);
		if (terminal->pvt->pty_master != -1) {
			_vte_pty_close(terminal->pvt->pty_master);
			close(terminal->pvt->pty_master);
			terminal->pvt->pty_master = -1;
		}

		/* Take one last shot at processing whatever data is pending,
		 * then flush the buffers in case we're about to run a new
		 * command, disconnecting the timeout. */
		vte_terminal_stop_processing (terminal);
		if (_vte_buffer_length(terminal->pvt->incoming) > 0) {
			vte_terminal_process_incoming(terminal);
		}
		_vte_buffer_clear(terminal->pvt->incoming);
		g_array_set_size(terminal->pvt->pending, 0);

		/* Clear the outgoing buffer as well. */
		_vte_buffer_clear(terminal->pvt->outgoing);

		/* Tell observers what's happened. */
		vte_terminal_emit_child_exited(terminal);
	}
}

static void
_vte_terminal_connect_pty_read(VteTerminal *terminal)
{
	if (terminal->pvt->pty_master == -1) {
		return;
	}
	if (terminal->pvt->pty_input == NULL) {
		terminal->pvt->pty_input =
			g_io_channel_unix_new(terminal->pvt->pty_master);
	}
	if (terminal->pvt->pty_input_source == VTE_INVALID_SOURCE) {
		terminal->pvt->pty_input_source =
			g_io_add_watch_full(terminal->pvt->pty_input,
					    VTE_CHILD_INPUT_PRIORITY,
					    G_IO_IN | G_IO_HUP,
					    vte_terminal_io_read,
					    terminal,
					    NULL);
	}
}

static void
_vte_terminal_connect_pty_write(VteTerminal *terminal)
{
	if (terminal->pvt->pty_master == -1) {
		return;
	}
	if (terminal->pvt->pty_output == NULL) {
		terminal->pvt->pty_output =
			g_io_channel_unix_new(terminal->pvt->pty_master);
	}
	if (terminal->pvt->pty_output_source == VTE_INVALID_SOURCE) {
		terminal->pvt->pty_output_source =
			g_io_add_watch_full(terminal->pvt->pty_output,
					    VTE_CHILD_OUTPUT_PRIORITY,
					    G_IO_OUT,
					    vte_terminal_io_write,
					    terminal,
					    NULL);
	}
}

static void
_vte_terminal_disconnect_pty_read(VteTerminal *terminal)
{
	if (terminal->pvt->pty_master == -1) {
		return;
	}
	if (terminal->pvt->pty_input != NULL) {
		g_io_channel_unref(terminal->pvt->pty_input);
		terminal->pvt->pty_input = NULL;
	}
	if (terminal->pvt->pty_input_source != VTE_INVALID_SOURCE) {
		g_source_remove(terminal->pvt->pty_input_source);
		terminal->pvt->pty_input_source = VTE_INVALID_SOURCE;
	}
}

static void
_vte_terminal_disconnect_pty_write(VteTerminal *terminal)
{
	if (terminal->pvt->pty_master == -1) {
		return;
	}
	if (terminal->pvt->pty_output != NULL) {
		g_io_channel_unref(terminal->pvt->pty_output);
		terminal->pvt->pty_output = NULL;
	}
	if (terminal->pvt->pty_output_source != VTE_INVALID_SOURCE) {
		g_source_remove(terminal->pvt->pty_output_source);
		terminal->pvt->pty_output_source = VTE_INVALID_SOURCE;
	}
}

/* Basic wrapper around _vte_pty_open, which handles the pipefitting. */
static pid_t
_vte_terminal_fork_basic(VteTerminal *terminal, const char *command,
			 char **argv, char **envv,
			 const char *directory,
			 gboolean lastlog, gboolean utmp, gboolean wtmp)
{
	char **env_add;
	int i;
	pid_t pid;
	GtkWidget *widget;
	VteReaper *reaper;

	widget = GTK_WIDGET(terminal);

	/* Duplicate the environment, and add one more variable. */
	for (i = 0; (envv != NULL) && (envv[i] != NULL); i++) {
		/* nothing */ ;
	}
	env_add = g_malloc0(sizeof(char*) * (i + 2));
	env_add[0] = g_strdup_printf("TERM=%s", terminal->pvt->emulation);
	for (i = 0; (envv != NULL) && (envv[i] != NULL); i++) {
		env_add[i + 1] = g_strdup(envv[i]);
	}
	env_add[i + 1] = NULL;

	/* Close any existing ptys. */
	if (terminal->pvt->pty_master != -1) {
		_vte_pty_close(terminal->pvt->pty_master);
		close(terminal->pvt->pty_master);
	}

	/* Open the new pty. */
	pid = -1;
	i = _vte_pty_open(&pid, env_add, command, argv, directory,
			  terminal->column_count, terminal->row_count,
			  lastlog, utmp, wtmp);
	switch (i) {
	case -1:
		return -1;
		break;
	default:
		if (pid != 0) {
			terminal->pvt->pty_master = i;
		}
	}

	/* If we successfully started the process, set up to listen for its
	 * output. */
	if ((pid != -1) && (pid != 0)) {
		/* Set this as the child's pid. */
		terminal->pvt->pty_pid = pid;

		/* Catch a child-exited signal from the child pid. */
		reaper = vte_reaper_get();
		vte_reaper_add_child((GPid) pid);
		g_object_ref(G_OBJECT(reaper));
		if (VTE_IS_REAPER(terminal->pvt->pty_reaper)) {
			g_signal_handlers_disconnect_by_func(terminal->pvt->pty_reaper,
							     (gpointer)vte_terminal_catch_child_exited,
							     terminal);
			g_object_unref(G_OBJECT(terminal->pvt->pty_reaper));
		}
		g_signal_connect(G_OBJECT(reaper), "child-exited",
				 G_CALLBACK(vte_terminal_catch_child_exited),
				 terminal);
		terminal->pvt->pty_reaper = reaper;

		/* Set the pty to be non-blocking. */
		i = fcntl(terminal->pvt->pty_master, F_GETFL);
		fcntl(terminal->pvt->pty_master, F_SETFL, i | O_NONBLOCK);

		/* Set the PTY size. */
		vte_terminal_set_size(terminal,
				      terminal->column_count,
				      terminal->row_count);
		if (GTK_WIDGET_REALIZED(widget)) {
			gtk_widget_queue_resize(widget);
		}

		/* Open a channel to listen for input on. */
		_vte_terminal_connect_pty_read(terminal);
	}

	/* Clean up. */
	for (i = 0; env_add[i] != NULL; i++) {
		g_free(env_add[i]);
	}
	g_free(env_add);

	/* Return the pid to the caller. */
	return pid;
}

/**
 * vte_terminal_fork_command:
 * @terminal: a #VteTerminal
 * @command: the name of a binary to run
 * @argv: the argument list to be passed to @command
 * @envv: a list of environment variables to be added to the environment before
 * starting @command, or NULL
 * @directory: the name of a directory the command should start in, or NULL
 * @lastlog: TRUE if the session should be logged to the lastlog
 * @utmp: TRUE if the session should be logged to the utmp/utmpx log
 * @wtmp: TRUE if the session should be logged to the wtmp/wtmpx log
 *
 * Starts the specified command under a newly-allocated controlling
 * pseudo-terminal.  The @argv and @envv lists should be NULL-terminated, and
 * argv[0] is expected to be the name of the file being run, as it would be if
 * execve() were being called.  TERM is automatically set to reflect the
 * terminal widget's emulation setting.  If @lastlog, @utmp, or @wtmp are TRUE,
 * logs the session to the specified system log files.
 *
 * Returns: the ID of the new process
 */
pid_t
vte_terminal_fork_command(VteTerminal *terminal,
			  const char *command, char **argv, char **envv,
			  const char *directory,
			  gboolean lastlog, gboolean utmp, gboolean wtmp)
{
	g_return_val_if_fail(VTE_IS_TERMINAL(terminal), -1);

	/* Make the user's shell the default command. */
	if (command == NULL) {
		if (terminal->pvt->shell == NULL) {
			struct passwd *pwd;

			pwd = getpwuid(getuid());
			if (pwd != NULL) {
				terminal->pvt->shell = pwd->pw_shell;
#ifdef VTE_DEBUG
				if (_vte_debug_on(VTE_DEBUG_MISC)) {
					fprintf(stderr,
						"Using user's shell (%s).\n",
						terminal->pvt->shell);
				}
#endif
			}
		}
		if (terminal->pvt->shell == NULL) {
			if (getenv ("SHELL")) {
				terminal->pvt->shell = getenv ("SHELL");
#ifdef VTE_DEBUG
				if (_vte_debug_on(VTE_DEBUG_MISC)) {
					fprintf(stderr, "Using $SHELL shell (%s).\n",
						terminal->pvt->shell);
				}
#endif
			} else {
				terminal->pvt->shell = "/bin/sh";
#ifdef VTE_DEBUG
				if (_vte_debug_on(VTE_DEBUG_MISC)) {
					fprintf(stderr, "Using default shell (%s).\n",
						terminal->pvt->shell);
				}
#endif
			}
		}
		command = terminal->pvt->shell;
	}

	/* Start up the command and get the PTY of the master. */
	return _vte_terminal_fork_basic(terminal, command, argv, envv,
				        directory, lastlog, utmp, wtmp);
}

/**
 * vte_terminal_forkpty:
 * @terminal: a #VteTerminal
 * @envv: a list of environment variables to be added to the environment before
 * starting returning in the child process, or NULL
 * @directory: the name of a directory the child process should change to, or
 * NULL
 * @lastlog: TRUE if the session should be logged to the lastlog
 * @utmp: TRUE if the session should be logged to the utmp/utmpx log
 * @wtmp: TRUE if the session should be logged to the wtmp/wtmpx log
 *
 * Starts a new child process under a newly-allocated controlling
 * pseudo-terminal.  TERM is automatically set to reflect the terminal widget's
 * emulation setting.  If @lastlog, @utmp, or @wtmp are TRUE, logs the session
 * to the specified system log files.
 *
 * Returns: the ID of the new process in the parent, 0 in the child, and -1 if
 * there was an error
 *
 * Since: 0.11.11
 */
pid_t
vte_terminal_forkpty(VteTerminal *terminal,
		     char **envv, const char *directory,
		     gboolean lastlog, gboolean utmp, gboolean wtmp)
{
	pid_t ret;

	g_return_val_if_fail(VTE_IS_TERMINAL(terminal), -1);

	ret = _vte_terminal_fork_basic(terminal, NULL, NULL, envv,
				       directory, lastlog, utmp, wtmp);

	return ret;
}

/* Handle an EOF from the client. */
static void
vte_terminal_eof(GIOChannel *channel, gpointer data)
{
	VteTerminal *terminal;

	g_assert(VTE_IS_TERMINAL(data));
	terminal = VTE_TERMINAL(data);

	/* Close the connections to the child -- note that the source channel
	 * has already been dereferenced. */
	if (channel == terminal->pvt->pty_input) {
		_vte_terminal_disconnect_pty_read(terminal);
	}

	/* Close out the PTY. */
	_vte_terminal_disconnect_pty_write(terminal);
	if (terminal->pvt->pty_master != -1) {
		_vte_pty_close(terminal->pvt->pty_master);
		close(terminal->pvt->pty_master);
		terminal->pvt->pty_master = -1;
	}

	/* Take one last shot at processing whatever data is pending, then
	 * flush the buffers in case we're about to run a new command,
	 * disconnecting the timeout. */
	vte_terminal_stop_processing (terminal);
	if (_vte_buffer_length(terminal->pvt->incoming) > 0) {
		vte_terminal_process_incoming(terminal);
	}
	_vte_buffer_clear(terminal->pvt->incoming);
	g_array_set_size(terminal->pvt->pending, 0);

	/* Clear the outgoing buffer as well. */
	_vte_buffer_clear(terminal->pvt->outgoing);

	/* Emit a signal that we read an EOF. */
	vte_terminal_emit_eof(terminal);
}

/* Reset the input method context. */
static void
vte_terminal_im_reset(VteTerminal *terminal)
{
	g_assert(VTE_IS_TERMINAL(terminal));
	if (GTK_WIDGET_REALIZED(GTK_WIDGET(terminal))) {
		gtk_im_context_reset(terminal->pvt->im_context);
		if (terminal->pvt->im_preedit != NULL) {
			g_free(terminal->pvt->im_preedit);
			terminal->pvt->im_preedit = NULL;
		}
		if (terminal->pvt->im_preedit_attrs != NULL) {
			pango_attr_list_unref(terminal->pvt->im_preedit_attrs);
			terminal->pvt->im_preedit_attrs = NULL;
		}
	}
}

/* Emit whichever signals are called for here. */
static void
vte_terminal_emit_pending_text_signals(VteTerminal *terminal, GQuark quark)
{
	static struct {
		const char *name;
		GQuark quark;
	} non_visual_quarks[] = {
		{"mb", 0},
		{"md", 0},
		{"mr", 0},
		{"mu", 0},
		{"se", 0},
		{"so", 0},
		{"ta", 0},
		{"character-attributes", 0},
	};
	GQuark tmp;
	int i;

	if (quark != 0) {
		for (i = 0; i < G_N_ELEMENTS(non_visual_quarks); i++) {
			if (non_visual_quarks[i].quark == 0) {
				tmp = g_quark_from_string(non_visual_quarks[i].name);
				non_visual_quarks[i].quark = tmp;
			}
			if (quark == non_visual_quarks[i].quark) {
				return;
			}
		}
	}

	if (terminal->pvt->text_modified_flag) {
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_SIGNALS)) {
			fprintf(stderr, "Emitting buffered `text-modified'.\n");
		}
#endif
		vte_terminal_emit_text_modified(terminal);
		terminal->pvt->text_modified_flag = FALSE;
	}
	if (terminal->pvt->text_inserted_count) {
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_SIGNALS)) {
			fprintf(stderr, "Emitting buffered `text-inserted' "
				"(%ld).\n", terminal->pvt->text_inserted_count);
		}
#endif
		_vte_terminal_emit_text_inserted(terminal);
		terminal->pvt->text_inserted_count = 0;
	}
	if (terminal->pvt->text_deleted_count) {
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_SIGNALS)) {
			fprintf(stderr, "Emitting buffered `text-deleted' "
				"(%ld).\n", terminal->pvt->text_deleted_count);
		}
#endif
		_vte_terminal_emit_text_deleted(terminal);
		terminal->pvt->text_deleted_count = 0;
	}
}

/* Process incoming data, first converting it to unicode characters, and then
 * processing control sequences. */
static gboolean
vte_terminal_process_incoming(VteTerminal *terminal)
{
	GValueArray *params = NULL;
	VteScreen *screen;
	struct vte_cursor_position cursor;
	GtkWidget *widget;
	GdkRectangle rect;
	GdkPoint bbox_topleft, bbox_bottomright;
	gunichar *wbuf, c;
	long wcount, start;
	const char *match;
	GQuark quark;
	const gunichar *next;
	gboolean leftovers, modified, bottom, inserted, again;
	GArray *unichars;

	g_assert(GTK_IS_WIDGET(terminal));
	g_assert(VTE_IS_TERMINAL(terminal));
	widget = GTK_WIDGET(terminal);

	bottom = (terminal->pvt->screen->insert_delta ==
		  terminal->pvt->screen->scroll_delta);

#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_IO)) {
		fprintf(stderr, "Handler processing %d bytes.\n",
			_vte_buffer_length(terminal->pvt->incoming));
	}
#endif
	/* Save the current cursor position. */
	screen = terminal->pvt->screen;
	cursor = screen->cursor_current;

	/* Estimate how much of the screen we'll need to update. */
	bbox_topleft.x = cursor.col;
	bbox_topleft.y = cursor.row;
	bbox_bottomright.x = cursor.col + 1; /* Assume it's on a wide char. */
	bbox_bottomright.y = cursor.row;

	/* We're going to check if the text was modified once we're done here,
	 * so keep a flag. */
	terminal->pvt->text_modified_flag = FALSE;
	terminal->pvt->text_inserted_count = 0;
	terminal->pvt->text_deleted_count = 0;

	/* We should only be called when there's data to process. */
	g_assert((_vte_buffer_length(terminal->pvt->incoming) > 0) ||
		 (terminal->pvt->pending->len > 0));

	/* Convert the data into unicode characters. */
	unichars = terminal->pvt->pending;
	_vte_iso2022_process(terminal->pvt->iso2022,
			     terminal->pvt->incoming,
			     unichars);

	/* Compute the number of unicode characters we got. */
	wbuf = &g_array_index(unichars, gunichar, 0);
	wcount = unichars->len;

	/* Try initial substrings. */
	start = 0;
	modified = leftovers = inserted = again = FALSE;

	while ((start < wcount) && !leftovers && !again) {
		/* Try to match any control sequences. */
		_vte_matcher_match(terminal->pvt->matcher,
				   &wbuf[start],
				   wcount - start,
				   &match,
				   &next,
				   &quark,
				   &params);
		/* We're in one of three possible situations now.
		 * First, the match string is a non-empty string and next
		 * points to the first character which isn't part of this
		 * sequence. */
		if ((match != NULL) && (match[0] != '\0')) {
			/* If we inserted text without sanity-checking the
			 * buffer, do so now. */
			if (inserted) {
				_vte_terminal_ensure_cursor(terminal, FALSE);
				inserted = FALSE;
			}
			/* Call the right sequence handler for the requested
			 * behavior. */
			again = vte_terminal_handle_sequence(GTK_WIDGET(terminal),
							     match,
							     quark,
							     params);
			/* Skip over the proper number of unicode chars. */
			start = (next - wbuf);
			modified = TRUE;
		} else
		/* Second, we have a NULL match, and next points to the very
		 * next character in the buffer.  Insert the character which
		 * we're currently examining into the screen. */
		if (match == NULL) {
			c = wbuf[start];
			/* If it's a control character, permute the order, per
			 * vttest. */
			if ((c != *next) &&
			    ((*next & 0x1f) == *next) &&
			    (start + 1 < next - wbuf)) {
				const gunichar *tnext = NULL;
				const char *tmatch = NULL;
				GQuark tquark = 0;
				gunichar ctrl;
				int i;
				/* We don't want to permute it if it's another
				 * control sequence, so check if it is. */
				_vte_matcher_match(terminal->pvt->matcher,
						   next,
						   wcount - (next - wbuf),
						   &tmatch,
						   &tnext,
						   &tquark,
						   NULL);
				/* We only do this for non-control-sequence
				 * characters and random garbage. */
				if (tnext == next + 1) {
					/* Save the control character. */
					ctrl = *next;
					/* Move everything before it up a
					 * slot.  */
					for (i = next - wbuf; i > start; i--) {
						wbuf[i] = wbuf[i - 1];
					}
					/* Move the control character to the
					 * front. */
					wbuf[i] = ctrl;
					continue;
				}
			}
#ifdef VTE_DEBUG
			c = c & ~VTE_ISO2022_ENCODED_WIDTH_MASK;
			if (_vte_debug_on(VTE_DEBUG_PARSE)) {
				if (c > 255) {
					fprintf(stderr, "U+%04lx\n", (long) c);
				} else {
					if (c > 127) {
						fprintf(stderr, "%ld = ",
							(long) c);
					}
					if (c < 32) {
						fprintf(stderr, "^%lc\n",
							(wint_t)c + 64);
					} else {
						fprintf(stderr, "`%lc'\n",
							(wint_t)c);
					}
				}
			}
			c = wbuf[start];
#endif
			if (c != 0) {
				/* Insert the character. */
				_vte_terminal_insert_char(terminal, c,
							 FALSE, FALSE,
							 TRUE, FALSE, 0);
				inserted = TRUE;
			}

			/* We *don't* emit flush pending signals here. */
			modified = TRUE;
			start++;
		} else {
			/* Case three: the read broke in the middle of a
			 * control sequence, so we're undecided with no more
			 * data to consult. If we have data following the
			 * middle of the sequence, then it's just garbage data,
			 * and for compatibility, we should discard it. */
			if (wbuf + wcount > next) {
#ifdef VTE_DEBUG
				if (_vte_debug_on(VTE_DEBUG_PARSE)) {
					fprintf(stderr, "Invalid control "
						"sequence, discarding %d "
						"characters.\n",
						next - (wbuf + start));
				}
#endif
				/* Discard. */
				start = next - wbuf + 1;
			} else {
				/* Pause processing here and wait for more
				 * data before continuing. */
				leftovers = TRUE;
			}
		}

		/* Add the cell into which we just moved to the region which we
		 * need to refresh for the user. */
		bbox_topleft.x = MIN(bbox_topleft.x,
				     screen->cursor_current.col);
		bbox_topleft.y = MIN(bbox_topleft.y,
				     screen->cursor_current.row);
		bbox_bottomright.x = MAX(bbox_bottomright.x,
					 screen->cursor_current.col + 1);
		bbox_bottomright.y = MAX(bbox_bottomright.y,
					 screen->cursor_current.row);

#ifdef VTE_DEBUG
		/* Some safety checks: ensure the visible parts of the buffer
		 * are all in the buffer. */
		g_assert(screen->insert_delta >=
			 _vte_ring_delta(screen->row_data));
		/* The cursor shouldn't be above or below the addressable
		 * part of the display buffer. */
		g_assert(screen->cursor_current.row >= screen->insert_delta);
#endif

		/* Free any parameters we don't care about any more. */
		_vte_matcher_free_params_array(params);
		params = NULL;
	}

	/* If we inserted text without sanity-checking the buffer, do so now. */
	if (inserted) {
		_vte_terminal_ensure_cursor(terminal, FALSE);
		inserted = FALSE;
	}

	/* Remove most of the processed characters. */
	if (start < wcount) {
		unichars = g_array_new(TRUE, TRUE, sizeof(gunichar));
		g_array_append_vals(unichars,
				    &g_array_index(terminal->pvt->pending,
						   gunichar,
						   start),
				    wcount - start);
		g_array_free(terminal->pvt->pending, TRUE);
		terminal->pvt->pending = unichars;
	} else {
		g_array_set_size(terminal->pvt->pending, 0);
		/* If we're out of data, we needn't pause to let the
		 * controlling application respond to incoming data, because
		 * the main loop is already going to do that. */
		again = FALSE;
	}

	/* Flush any pending "inserted" signals. */
	vte_terminal_emit_pending_text_signals(terminal, 0);

	/* Clip off any part of the box which isn't already on-screen. */
	bbox_topleft.x = MAX(bbox_topleft.x, 0);
	bbox_topleft.y = MAX(bbox_topleft.y, screen->scroll_delta);
	bbox_bottomright.x = MIN(bbox_bottomright.x,
				 terminal->column_count - 1);
	bbox_bottomright.y = MIN(bbox_bottomright.y,
				 screen->scroll_delta +
				 terminal->row_count);

	/* Update the screen to draw any modified areas.  This includes
	 * the current location of the cursor. */
	_vte_invalidate_cells(terminal,
			     bbox_topleft.x - 1,
			     bbox_bottomright.x - (bbox_topleft.x - 1) + 1,
			     bbox_topleft.y,
			     bbox_bottomright.y - bbox_topleft.y + 1);

	if (modified) {
		/* Keep the cursor on-screen if we scroll on output, or if
		 * we're currently at the bottom of the buffer. */
		_vte_terminal_update_insert_delta(terminal);
		if (terminal->pvt->scroll_on_output || bottom) {
			vte_terminal_maybe_scroll_to_bottom(terminal);
		}
		/* Deselect the current selection if its contents are changed
		 * by this insertion. */
		if (terminal->pvt->has_selection) {
			char *selection;
			selection =
			vte_terminal_get_text_range(terminal,
						    terminal->pvt->selection_start.y,
						    0,
						    terminal->pvt->selection_end.y,
						    terminal->column_count,
						    vte_cell_is_selected,
						    NULL,
						    NULL);
			if ((selection == NULL) ||
			    (strcmp(selection, terminal->pvt->selection) != 0)) {
				vte_terminal_deselect_all(terminal);
			}
			g_free(selection);
		}
	}

	if (modified || (screen != terminal->pvt->screen)) {
		/* Signal that the visible contents changed. */
		_vte_terminal_match_contents_clear(terminal);
		_vte_terminal_emit_contents_changed(terminal);
	}

	if ((cursor.col != terminal->pvt->screen->cursor_current.col) ||
	    (cursor.row != terminal->pvt->screen->cursor_current.row)) {
		/* Signal that the cursor moved. */
		vte_terminal_emit_cursor_moved(terminal);
	}

	/* Tell the input method where the cursor is. */
	if (GTK_WIDGET_REALIZED(GTK_WIDGET(terminal))) {
		rect.x = terminal->pvt->screen->cursor_current.col *
			 terminal->char_width + VTE_PAD_WIDTH;
		rect.width = terminal->char_width;
		rect.y = (terminal->pvt->screen->cursor_current.row -
			  terminal->pvt->screen->scroll_delta) *
			 terminal->char_height + VTE_PAD_WIDTH;
		rect.height = terminal->char_height;
		gtk_im_context_set_cursor_location(terminal->pvt->im_context,
						   &rect);
	}

#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_IO)) {
		fprintf(stderr, "%ld chars and %ld bytes left to process.\n",
			(long) unichars->len,
			(long) _vte_buffer_length(terminal->pvt->incoming));
	}
#endif
	return again;
}

/* Read and handle data from the child. */
static gboolean
vte_terminal_io_read(GIOChannel *channel,
		     GIOCondition condition,
		     gpointer data)
{
	VteTerminal *terminal;
	GtkWidget *widget;
	char buf[VTE_INPUT_CHUNK_SIZE];
	int bcount, fd;
	gboolean eof, leave_open = TRUE;

	g_assert(VTE_IS_TERMINAL(data));
	widget = GTK_WIDGET(data);
	terminal = VTE_TERMINAL(data);

	/* Check that the channel is still open. */
	fd = g_io_channel_unix_get_fd(channel);

	/* Read some data in from this channel. */
	bcount = 0;
	if (condition & G_IO_IN) {
		/* We try not to overfill the incoming buffer below by cutting
		 * down the read size if we already have pending data. */
		bcount = sizeof(buf);
		if (_vte_buffer_length(terminal->pvt->incoming) < sizeof(buf)) {
			/* Shoot for exactly one "chunk" for processing. */
			bcount -= _vte_buffer_length(terminal->pvt->incoming);
		} else {
			/* Read half of the chunk size. */
			bcount = sizeof(buf) / 2;
		}
		g_assert(bcount >= 0);
		g_assert(bcount <= sizeof(buf));
		bcount = read(fd, buf, MAX(bcount, sizeof(buf) / 2));
	}

	/* Check for end-of-file. */
	eof = FALSE;
	if (condition & G_IO_HUP) {
		eof = TRUE;
	}

	/* Catch errors. */
	switch (bcount) {
	case 0:
		/* EOF */
		eof = TRUE;
		break;
	case -1:
		/* Error! */
		switch (errno) {
			case EIO: /* Fake an EOF. */
				eof = TRUE;
				break;
			case EAGAIN:
			case EBUSY: /* do nothing */
				break;
			default:
				/* Translators: %s is replaced with error message returned by strerror(). */
				g_warning(_("Error reading from child: "
					    "%s."), strerror(errno));
				leave_open = TRUE;
				break;
		}
		break;
	default:
		/* Queue up the data for processing. */
		vte_terminal_feed(terminal, buf, bcount);
		break;
	}

	/* If we detected an eof condition, signal one. */
	if (eof) {
		vte_terminal_eof(channel, terminal);
		leave_open = FALSE;
	}

	/* If there's more data coming, return TRUE, otherwise return FALSE. */
	return leave_open;
}

/**
 * vte_terminal_feed:
 * @terminal: a #VteTerminal
 * @data: a string in the terminal's current encoding
 * @length: the length of the string
 *
 * Interprets @data as if it were data received from a child process.  This
 * can either be used to drive the terminal without a child process, or just
 * to mess with your users.
 *
 */
void
vte_terminal_feed(VteTerminal *terminal, const char *data, glong length)
{
	/* If length == -1, use the length of the data string. */
	if (length == ((gssize)-1)) {
		length = strlen(data);
	}

	/* If we got data, modify the pending buffer. */
	if (length > 0) {
		_vte_buffer_append(terminal->pvt->incoming, data, length);
	}

	vte_terminal_start_processing (terminal);
}

/* Send locally-encoded characters to the child. */
static gboolean
vte_terminal_io_write(GIOChannel *channel,
		      GIOCondition condition,
		      gpointer data)
{
	VteTerminal *terminal;
	gssize count;
	int fd;
	gboolean leave_open;

	g_assert(VTE_IS_TERMINAL(data));
	terminal = VTE_TERMINAL(data);

	fd = g_io_channel_unix_get_fd(channel);

	count = write(fd, terminal->pvt->outgoing->bytes,
		      _vte_buffer_length(terminal->pvt->outgoing));
	if (count != -1) {
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_IO)) {
			int i;
			for (i = 0; i < count; i++) {
				fprintf(stderr, "Wrote %c%c\n",
					((guint8)terminal->pvt->outgoing->bytes[i]) >= 32 ?
					' ' : '^',
					((guint8)terminal->pvt->outgoing->bytes[i]) >= 32 ?
					terminal->pvt->outgoing->bytes[i] :
					((guint8)terminal->pvt->outgoing->bytes[i])  + 64);
			}
		}
#endif
		_vte_buffer_consume(terminal->pvt->outgoing, count);
	}

	if (_vte_buffer_length(terminal->pvt->outgoing) == 0) {
		_vte_terminal_disconnect_pty_write(terminal);
		leave_open = FALSE;
	} else {
		leave_open = TRUE;
	}

	return leave_open;
}

/* Convert some arbitrarily-encoded data to send to the child. */
static void
vte_terminal_send(VteTerminal *terminal, const char *encoding,
		  const void *data, gssize length,
		  gboolean local_echo, gboolean newline_stuff)
{
	gssize icount, ocount;
	char *ibuf, *obuf, *obufptr, *cooked;
	VteConv *conv;
	long crcount, cooked_length, i;

	g_assert(VTE_IS_TERMINAL(terminal));
	g_assert(strcmp(encoding, "UTF-8") == 0);

	conv = NULL;
	if (strcmp(encoding, "UTF-8") == 0) {
		conv = &terminal->pvt->outgoing_conv;
	}
	g_assert(conv != NULL);
	g_assert(*conv != ((VteConv) -1));

	icount = length;
	ibuf = (char *) data;
	ocount = ((length + 1) * VTE_UTF8_BPC) + 1;
	_vte_buffer_set_minimum_size(terminal->pvt->conv_buffer, ocount);
	obuf = obufptr = terminal->pvt->conv_buffer->bytes;

	if (_vte_conv(*conv, &ibuf, &icount, &obuf, &ocount) == -1) {
		g_warning(_("Error (%s) converting data for child, dropping."),
			  strerror(errno));
	} else {
		crcount = 0;
		if (newline_stuff) {
			for (i = 0; i < obuf - obufptr; i++) {
				switch (obufptr[i]) {
				case '\015':
					crcount++;
					break;
				default:
					break;
				}
			}
		}
		if (crcount > 0) {
			cooked = g_malloc(obuf - obufptr + crcount);
			cooked_length = 0;
			for (i = 0; i < obuf - obufptr; i++) {
				switch (obufptr[i]) {
				case '\015':
					cooked[cooked_length++] = '\015';
					cooked[cooked_length++] = '\012';
					break;
				default:
					cooked[cooked_length++] = obufptr[i];
					break;
				}
			}
		} else {
			cooked = obufptr;
			cooked_length = obuf - obufptr;
		}
		/* Tell observers that we're sending this to the child. */
		if (cooked_length > 0) {
			vte_terminal_emit_commit(terminal,
						 cooked, cooked_length);
		}
		/* Echo the text if we've been asked to do so. */
		if ((cooked_length > 0) && local_echo) {
			gunichar *ucs4;
			int i, len;
			len = g_utf8_strlen(cooked, cooked_length);
			ucs4 = g_utf8_to_ucs4(cooked, cooked_length,
					      NULL, NULL, NULL);
			if (ucs4 != NULL) {
				for (i = 0; i < len; i++) {
					_vte_terminal_insert_char(terminal,
								 ucs4[i],
								 FALSE,
								 TRUE,
								 TRUE,
								 TRUE,
								 0);
				}
				g_free(ucs4);
			}
		}
		/* If there's a place for it to go, add the data to the
		 * outgoing buffer. */
		if ((cooked_length > 0) && (terminal->pvt->pty_master != -1)) {
			_vte_buffer_append(terminal->pvt->outgoing,
					   cooked, cooked_length);
#ifdef VTE_DEBUG
			if (_vte_debug_on(VTE_DEBUG_KEYBOARD)) {
				for (i = 0; i < cooked_length; i++) {
					if ((((guint8) cooked[i]) < 32) ||
					    (((guint8) cooked[i]) > 127)) {
						fprintf(stderr,
							"Sending <%02x> "
							"to child.\n",
							cooked[i]);
					} else {
						fprintf(stderr,
							"Sending '%c' "
							"to child.\n",
							cooked[i]);
					}
				}
			}
#endif
			/* If we need to start waiting for the child pty to
			 * become available for writing, set that up here. */
			_vte_terminal_connect_pty_write(terminal);
		}
		if (crcount > 0) {
			g_free(cooked);
		}
	}
	return;
}

/**
 * vte_terminal_feed_child:
 * @terminal: a #VteTerminal
 * @data: data to send to the child
 * @length: length of @text
 *
 * Sends a block of UTF-8 text to the child as if it were entered by the user
 * at the keyboard.
 *
 */
void
vte_terminal_feed_child(VteTerminal *terminal, const char *data, glong length)
{
	g_return_if_fail(VTE_IS_TERMINAL(terminal));
	if (length == ((gssize)-1)) {
		length = strlen(data);
	}
	if (length > 0) {
		vte_terminal_send(terminal, "UTF-8", data, length,
				  FALSE, FALSE);
	}
}

static void
vte_terminal_feed_child_using_modes(VteTerminal *terminal,
				    const char *data, glong length)
{
	g_assert(VTE_IS_TERMINAL(terminal));
	if (length == ((gssize)-1)) {
		length = strlen(data);
	}
	if (length > 0) {
		vte_terminal_send(terminal, "UTF-8", data, length,
				  !terminal->pvt->screen->sendrecv_mode,
				  terminal->pvt->screen->linefeed_mode);
	}
}

/* Send text from the input method to the child. */
static void
vte_terminal_im_commit(GtkIMContext *im_context, gchar *text, gpointer data)
{
	VteTerminal *terminal;

	g_assert(VTE_IS_TERMINAL(data));
	g_assert(GTK_IS_IM_CONTEXT(im_context));
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_EVENTS)) {
		fprintf(stderr, "Input method committed `%s'.\n", text);
	}
#endif
	terminal = VTE_TERMINAL(data);
	vte_terminal_feed_child_using_modes(terminal, text, -1);
	/* Committed text was committed because the user pressed a key, so
	 * we need to obey the scroll-on-keystroke setting. */
	if (terminal->pvt->scroll_on_keystroke) {
		vte_terminal_maybe_scroll_to_bottom(terminal);
	}
}

/* We've started pre-editing. */
static void
vte_terminal_im_preedit_start(GtkIMContext *im_context, gpointer data)
{
	VteTerminal *terminal;

	g_assert(VTE_IS_TERMINAL(data));
	terminal = VTE_TERMINAL(data);
	g_assert(GTK_IS_IM_CONTEXT(im_context));

#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_EVENTS)) {
		fprintf(stderr, "Input method pre-edit started.\n");
	}
#endif
	terminal->pvt->im_preedit_active = TRUE;
}

/* We've stopped pre-editing. */
static void
vte_terminal_im_preedit_end(GtkIMContext *im_context, gpointer data)
{
	VteTerminal *terminal;

	g_assert(VTE_IS_TERMINAL(data));
	terminal = VTE_TERMINAL(data);
	g_assert(GTK_IS_IM_CONTEXT(im_context));

#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_EVENTS)) {
		fprintf(stderr, "Input method pre-edit ended.\n");
	}
#endif
	terminal->pvt->im_preedit_active = FALSE;
}

/* The pre-edit string changed. */
static void
vte_terminal_im_preedit_changed(GtkIMContext *im_context, gpointer data)
{
	gchar *str;
	PangoAttrList *attrs;
	VteTerminal *terminal;
	gint cursor;

	g_assert(VTE_IS_TERMINAL(data));
	terminal = VTE_TERMINAL(data);
	g_assert(GTK_IS_IM_CONTEXT(im_context));

	gtk_im_context_get_preedit_string(im_context, &str, &attrs, &cursor);
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_EVENTS)) {
		fprintf(stderr, "Input method pre-edit changed (%s,%d).\n",
			str, cursor);
	}
#endif

	/* Queue the area where the current preedit string is being displayed
	 * for repainting. */
	_vte_invalidate_cursor_once(terminal, FALSE);

	if (terminal->pvt->im_preedit != NULL) {
		g_free(terminal->pvt->im_preedit);
	}
	terminal->pvt->im_preedit = str;

	if (terminal->pvt->im_preedit_attrs != NULL) {
		pango_attr_list_unref(terminal->pvt->im_preedit_attrs);
	}
	terminal->pvt->im_preedit_attrs = attrs;

	terminal->pvt->im_preedit_cursor = cursor;

	_vte_invalidate_cursor_once(terminal, FALSE);
}

/* Handle the toplevel being reconfigured. */
static gboolean
vte_terminal_configure_toplevel(GtkWidget *widget, GdkEventConfigure *event,
				gpointer data)
{
	VteTerminal *terminal;

#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_EVENTS)) {
		fprintf(stderr, "Top level parent configured.\n");
	}
#endif
	g_assert(GTK_IS_WIDGET(widget));
	g_assert(GTK_WIDGET_TOPLEVEL(widget));
	g_assert(VTE_IS_TERMINAL(data));
	terminal = VTE_TERMINAL(data);

	if (terminal->pvt->bg_transparent) {
		/* We have to repaint the entire window, because we don't get
		 * an expose event unless some portion of our visible area
		 * moved out from behind another window. */
		_vte_invalidate_all(terminal);
	}

	return FALSE;
}

/* Handle a hierarchy-changed signal. */
static void
vte_terminal_hierarchy_changed(GtkWidget *widget, GtkWidget *old_toplevel,
			       gpointer data)
{
	VteTerminal *terminal;
	GtkWidget *toplevel;

#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_EVENTS)) {
		fprintf(stderr, "Hierarchy changed.\n");
	}
#endif

	g_assert(VTE_IS_TERMINAL(widget));
	terminal = VTE_TERMINAL(widget);

	if (GTK_IS_WIDGET(old_toplevel)) {
		g_signal_handlers_disconnect_by_func(G_OBJECT(old_toplevel),
						     (gpointer)vte_terminal_configure_toplevel,
						     terminal);
	}

	toplevel = gtk_widget_get_toplevel(widget);
	if (GTK_IS_WIDGET(toplevel)) {
		g_signal_connect(G_OBJECT(toplevel), "configure-event",
				 G_CALLBACK(vte_terminal_configure_toplevel),
				 terminal);
	}
}

/* Handle a style-changed signal. */
static void
vte_terminal_style_changed(GtkWidget *widget, GtkStyle *style, gpointer data)
{
	VteTerminal *terminal;
	g_assert(VTE_IS_TERMINAL(widget));
	if (!GTK_WIDGET_REALIZED(widget)) {
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_MISC)) {
			fprintf(stderr, "Don't change style if we aren't realized.\n");
		}
#endif
		return;
	}

	terminal = VTE_TERMINAL(widget);

	/* If the font we're using is the same as the old default, then we
	 * need to pick up the new default. */
	if (pango_font_description_equal(style->font_desc,
					 widget->style->font_desc) ||
	    (terminal->pvt->fontdesc == NULL)) {
		vte_terminal_set_font_full(terminal, terminal->pvt->fontdesc,
					   terminal->pvt->fontantialias);
	}
}

/* Read and handle a keypress event. */
static gint
vte_terminal_key_press(GtkWidget *widget, GdkEventKey *event)
{
	VteTerminal *terminal;
	struct _vte_termcap *termcap;
	const char *tterm;
	char *normal = NULL, *output;
	gssize normal_length = 0;
	int i;
	const char *special = NULL;
	struct termios tio;
	struct timeval tv;
	struct timezone tz;
	gboolean scrolled = FALSE, steal = FALSE, modifier = FALSE, handled,
		 suppress_meta_esc = FALSE;
	guint keyval = 0;
	gunichar keychar = 0;
	char keybuf[VTE_UTF8_BPC];
	GdkModifierType modifiers;
	GtkWidgetClass *widget_class;

	g_assert(widget != NULL);
	g_assert(VTE_IS_TERMINAL(widget));
	terminal = VTE_TERMINAL(widget);

	/* First, check if GtkWidget's behavior already does something with
	 * this key. */
	widget_class = g_type_class_peek(GTK_TYPE_WIDGET);
	if (GTK_WIDGET_CLASS(widget_class)->key_press_event) {
		if ((GTK_WIDGET_CLASS(widget_class))->key_press_event(widget,
								      event)) {
			return TRUE;
		}
	}

	/* If it's a keypress, record that we got the event, in case the
	 * input method takes the event from us. */
	if (event->type == GDK_KEY_PRESS) {
		/* Store a copy of the key. */
		keyval = event->keyval;
		if (gdk_event_get_state((GdkEvent*)event, &modifiers)) {
			terminal->pvt->modifiers = modifiers;
		} else {
			modifiers = terminal->pvt->modifiers;
		}

		/* If we're in margin bell mode and on the border of the
		 * margin, bell. */
		if (terminal->pvt->margin_bell) {
			if ((terminal->pvt->screen->cursor_current.col +
			     terminal->pvt->bell_margin) ==
			    terminal->column_count) {
				_vte_sequence_handler_bl(terminal,
							 "bl",
							 0,
							 NULL);
			}
		}

		/* Log the time of the last keypress. */
		if (gettimeofday(&tv, &tz) == 0) {
			terminal->pvt->last_keypress_time =
				(tv.tv_sec * 1000) + (tv.tv_usec / 1000);
		}

		/* Determine if this is just a modifier key. */
		modifier = _vte_keymap_key_is_modifier(keyval);

		/* Unless it's a modifier key, hide the pointer. */
		if (!modifier) {
			_vte_terminal_set_pointer_visible(terminal, FALSE);
		}

#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_EVENTS)) {
			fprintf(stderr, "Keypress, modifiers=0x%x, "
				"keyval=0x%x, raw string=`%s'.\n",
				terminal->pvt->modifiers,
				keyval, event->string);
		}
#endif

		/* We steal many keypad keys here. */
		if (!terminal->pvt->im_preedit_active) {
			switch (keyval) {
			case GDK_KP_Add:
			case GDK_KP_Subtract:
			case GDK_KP_Multiply:
			case GDK_KP_Divide:
			case GDK_KP_Enter:
				steal = TRUE;
				break;
			default:
				break;
			}
			if (modifiers & VTE_META_MASK) {
				steal = TRUE;
			}
			switch (keyval) {
			case GDK_Multi_key:
			case GDK_Codeinput:
			case GDK_SingleCandidate:
			case GDK_MultipleCandidate:
			case GDK_PreviousCandidate:
			case GDK_Kanji:
			case GDK_Muhenkan:
			case GDK_Henkan:
			case GDK_Romaji:
			case GDK_Hiragana:
			case GDK_Katakana:
			case GDK_Hiragana_Katakana:
			case GDK_Zenkaku:
			case GDK_Hankaku:
			case GDK_Zenkaku_Hankaku:
			case GDK_Touroku:
			case GDK_Massyo:
			case GDK_Kana_Lock:
			case GDK_Kana_Shift:
			case GDK_Eisu_Shift:
			case GDK_Eisu_toggle:
				steal = FALSE;
				break;
			default:
				break;
			}
		}
	}

	/* Let the input method at this one first. */
	if (!steal) {
		if (GTK_WIDGET_REALIZED(terminal) &&
		    gtk_im_context_filter_keypress(terminal->pvt->im_context,
						   event)) {
#ifdef VTE_DEBUG
			if (_vte_debug_on(VTE_DEBUG_EVENTS)) {
				fprintf(stderr, "Keypress taken by IM.\n");
			}
#endif
			return TRUE;
		}
	}

	/* Now figure out what to send to the child. */
	if ((event->type == GDK_KEY_PRESS) && !modifier) {
		handled = FALSE;
		/* Map the key to a sequence name if we can. */
		switch (keyval) {
		case GDK_BackSpace:
			switch (terminal->pvt->backspace_binding) {
			case VTE_ERASE_ASCII_BACKSPACE:
				normal = g_strdup("");
				normal_length = 1;
				suppress_meta_esc = FALSE;
				break;
			case VTE_ERASE_ASCII_DELETE:
				normal = g_strdup("");
				normal_length = 1;
				suppress_meta_esc = FALSE;
				break;
			case VTE_ERASE_DELETE_SEQUENCE:
				special = "kD";
				suppress_meta_esc = TRUE;
				break;
			/* Use the tty's erase character. */
			case VTE_ERASE_AUTO:
			default:
				if (terminal->pvt->pty_master != -1) {
					if (tcgetattr(terminal->pvt->pty_master,
						      &tio) != -1) {
						normal = g_strdup_printf("%c",
									 tio.c_cc[VERASE]);
						normal_length = 1;
					}
				}
				suppress_meta_esc = FALSE;
				break;
			}
			handled = TRUE;
			break;
		case GDK_KP_Delete:
		case GDK_Delete:
			switch (terminal->pvt->delete_binding) {
			case VTE_ERASE_ASCII_BACKSPACE:
				normal = g_strdup("\010");
				normal_length = 1;
				break;
			case VTE_ERASE_ASCII_DELETE:
				normal = g_strdup("\177");
				normal_length = 1;
				break;
			case VTE_ERASE_DELETE_SEQUENCE:
			case VTE_ERASE_AUTO:
			default:
				special = "kD";
				break;
			}
			handled = TRUE;
			suppress_meta_esc = TRUE;
			break;
		case GDK_KP_Insert:
		case GDK_Insert:
			if (terminal->pvt->modifiers & GDK_SHIFT_MASK) {
				vte_terminal_paste_clipboard(terminal);
				handled = TRUE;
				suppress_meta_esc = TRUE;
			} else if (terminal->pvt->modifiers & GDK_CONTROL_MASK) {
				vte_terminal_copy_clipboard(terminal);
				handled = TRUE;
				suppress_meta_esc = TRUE;
			}
			break;
		/* Keypad/motion keys. */
		case GDK_KP_Page_Up:
		case GDK_Page_Up:
			if (terminal->pvt->modifiers & GDK_SHIFT_MASK) {
				vte_terminal_scroll_pages(terminal, -1);
				scrolled = TRUE;
				handled = TRUE;
				suppress_meta_esc = TRUE;
			}
			break;
		case GDK_KP_Page_Down:
		case GDK_Page_Down:
			if (terminal->pvt->modifiers & GDK_SHIFT_MASK) {
				vte_terminal_scroll_pages(terminal, 1);
				scrolled = TRUE;
				handled = TRUE;
				suppress_meta_esc = TRUE;
			}
			break;
		case GDK_KP_Home:
		case GDK_Home:
			if (terminal->pvt->modifiers & GDK_SHIFT_MASK) {
				vte_terminal_maybe_scroll_to_top(terminal);
				scrolled = TRUE;
				handled = TRUE;
			}
			break;
		case GDK_KP_End:
		case GDK_End:
			if (terminal->pvt->modifiers & GDK_SHIFT_MASK) {
				vte_terminal_maybe_scroll_to_bottom(terminal);
				scrolled = TRUE;
				handled = TRUE;
			}
			break;
		/* Let Shift +/- tweak the font, like XTerm does. */
		case GDK_KP_Add:
		case GDK_KP_Subtract:
			if (terminal->pvt->modifiers &
			    (GDK_SHIFT_MASK | GDK_CONTROL_MASK)) {
				switch (keyval) {
				case GDK_KP_Add:
					vte_terminal_emit_increase_font_size(terminal);
					handled = TRUE;
					suppress_meta_esc = TRUE;
					break;
				case GDK_KP_Subtract:
					vte_terminal_emit_decrease_font_size(terminal);
					handled = TRUE;
					suppress_meta_esc = TRUE;
					break;
				}
			}
			break;
		default:
			break;
		}
		/* If the above switch statement didn't do the job, try mapping
		 * it to a literal or capability name. */
		if (handled == FALSE) {
			_vte_keymap_map(keyval, terminal->pvt->modifiers,
					terminal->pvt->sun_fkey_mode,
					terminal->pvt->hp_fkey_mode,
					terminal->pvt->legacy_fkey_mode,
					terminal->pvt->vt220_fkey_mode,
					terminal->pvt->cursor_mode == VTE_KEYMODE_APPLICATION,
					terminal->pvt->keypad_mode == VTE_KEYMODE_APPLICATION,
					terminal->pvt->termcap,
					terminal->pvt->emulation ?
					terminal->pvt->emulation : vte_terminal_get_default_emulation(terminal),
					&normal,
					&normal_length,
					&special);
			/* If we found something this way, suppress
			 * escape-on-meta. */
			if (((normal != NULL) && (normal_length > 0)) ||
			    (special != NULL)) {
				suppress_meta_esc = TRUE;
			}
		}
		/* If we didn't manage to do anything, try to salvage a
		 * printable string. */
		if (!handled && (normal == NULL) && (special == NULL)) {
			/* Convert the keyval to a gunichar. */
			keychar = gdk_keyval_to_unicode(keyval);
			normal_length = 0;
			if (keychar != 0) {
				/* Convert the gunichar to a string. */
				normal_length = g_unichar_to_utf8(keychar,
								  keybuf);
				if (normal_length != 0) {
					normal = g_malloc0(normal_length + 1);
					memcpy(normal, keybuf, normal_length);
				} else {
					normal = NULL;
				}
			}
			if ((normal != NULL) &&
			    (terminal->pvt->modifiers & GDK_CONTROL_MASK)) {
				/* Replace characters which have "control"
				 * counterparts with those counterparts. */
				for (i = 0; i < normal_length; i++) {
					if ((((guint8)normal[i]) >= 0x40) &&
					    (((guint8)normal[i]) <  0x80)) {
						normal[i] &= (~(0x60));
					}
				}
			}
#ifdef VTE_DEBUG
			if (normal && _vte_debug_on(VTE_DEBUG_EVENTS)) {
				fprintf(stderr, "Keypress, modifiers=0x%x, "
					"keyval=0x%x, cooked string=`%s'.\n",
					terminal->pvt->modifiers,
					keyval, normal);
			}
#endif
		}
		/* If we got normal characters, send them to the child. */
		if (normal != NULL) {
			if (terminal->pvt->meta_sends_escape &&
			    !suppress_meta_esc &&
			    (normal_length > 0) &&
			    (terminal->pvt->modifiers & VTE_META_MASK)) {
				vte_terminal_feed_child(terminal,
							_VTE_CAP_ESC,
							1);
			}
			if (normal_length > 0) {
				vte_terminal_feed_child_using_modes(terminal,
								    normal,
								    normal_length);
			}
			g_free(normal);
		} else
		/* If the key maps to characters, send them to the child. */
		if (special != NULL) {
			termcap = terminal->pvt->termcap;
			tterm = terminal->pvt->emulation;
			normal = _vte_termcap_find_string_length(termcap,
								 tterm,
								 special,
								 &normal_length);
			_vte_keymap_key_add_key_modifiers(keyval,
							  terminal->pvt->modifiers,
							  terminal->pvt->sun_fkey_mode,
							  terminal->pvt->hp_fkey_mode,
							  terminal->pvt->legacy_fkey_mode,
							  terminal->pvt->vt220_fkey_mode,
							  &normal,
							  &normal_length);
			output = g_strdup_printf(normal, 1);
			vte_terminal_feed_child_using_modes(terminal,
							    output, -1);
			g_free(output);
			g_free(normal);
		}
		/* Keep the cursor on-screen. */
		if (!scrolled && !modifier &&
		    terminal->pvt->scroll_on_keystroke) {
			vte_terminal_maybe_scroll_to_bottom(terminal);
		}
		return TRUE;
	}
	return FALSE;
}

static gboolean
vte_terminal_key_release(GtkWidget *widget, GdkEventKey *event)
{
	VteTerminal *terminal;
	GdkModifierType modifiers;

	terminal = VTE_TERMINAL(widget);

	if (gdk_event_get_state((GdkEvent*)event, &modifiers)) {
		terminal->pvt->modifiers = modifiers;
	}

	return GTK_WIDGET_REALIZED(terminal) &&
	       gtk_im_context_filter_keypress(terminal->pvt->im_context, event);
}

/**
 * vte_terminal_is_word_char:
 * @terminal: a #VteTerminal
 * @c: a candidate Unicode code point
 *
 * Checks if a particular character is considered to be part of a word or not,
 * based on the values last passed to vte_terminal_set_word_chars().
 *
 * Returns: TRUE if the character is considered to be part of a word
 */
gboolean
vte_terminal_is_word_char(VteTerminal *terminal, gunichar c)
{
	int i;
	VteWordCharRange *range;
	g_return_val_if_fail(VTE_IS_TERMINAL(terminal), FALSE);
	/* If we have no array, or it's empty, assume the defaults. */
	if ((terminal->pvt->word_chars == NULL) ||
	    (terminal->pvt->word_chars->len == 0)) {
		return g_unichar_isgraph(c) &&
		       !g_unichar_ispunct(c) &&
		       !g_unichar_isspace(c) &&
		       (c != '\0');
	}
	/* Go through each range and check if the character is included. */
	for (i = 0; i < terminal->pvt->word_chars->len; i++) {
		range = &g_array_index(terminal->pvt->word_chars,
				       VteWordCharRange,
				       i);
		if ((c >= range->start) && (c <= range->end)) {
			return TRUE;
		}
	}
	/* Default. */
	return FALSE;
}

/* Check if the characters in the given block are in the same class (word vs.
 * non-word characters). */
static gboolean
vte_uniform_class(VteTerminal *terminal, glong row, glong scol, glong ecol)
{
	struct vte_charcell *pcell = NULL;
	long col;
	gboolean word_char;
	g_assert(VTE_IS_TERMINAL(terminal));
	if ((pcell = vte_terminal_find_charcell(terminal, scol, row)) != NULL) {
		word_char = vte_terminal_is_word_char(terminal, pcell->c);
		for (col = scol + 1; col <= ecol; col++) {
			pcell = vte_terminal_find_charcell(terminal, col, row);
			if (pcell == NULL) {
				return FALSE;
			}
			if (word_char != vte_terminal_is_word_char(terminal,
								   pcell->c)) {
				return FALSE;
			}
		}
		return TRUE;
	}
	return FALSE;
}

/* Check if we soft-wrapped on the given line. */
static gboolean
vte_line_is_wrappable(VteTerminal *terminal, glong row)
{
	VteRowData *rowdata;
	VteScreen *screen;
	g_assert(VTE_IS_TERMINAL(terminal));
	screen = terminal->pvt->screen;
	if (_vte_ring_contains(screen->row_data, row)) {
		rowdata = _vte_ring_index(screen->row_data, VteRowData *, row);
		if (rowdata->soft_wrapped) {
			return TRUE;
		}
	}
	return FALSE;
}

/* Check if the given point is in the region between the two points,
 * optionally treating the second point as included in the region or not. */
static gboolean
vte_cell_is_between(glong col, glong row,
		    glong acol, glong arow, glong bcol, glong brow,
		    gboolean inclusive)
{
	/* Negative between never allowed. */
	if ((arow > brow) || ((arow == brow) && (acol > bcol))) {
		return FALSE;
	}
	/* Zero-length between only allowed if we're being inclusive. */
	if ((row == arow) && (row == brow) && (col == acol) && (col == bcol)) {
		return inclusive;
	}
	/* A cell is between two points if it's on a line after the
	 * specified area starts, or before the line where it ends,
	 * or any of the lines in between. */
	if ((row > arow) && (row < brow)) {
		return TRUE;
	}
	/* It's also between the two points if they're on the same row
	 * the cell lies between the start and end columns. */
	if ((row == arow) && (row == brow)) {
		if (col >= acol) {
			if (col < bcol) {
				return TRUE;
			} else {
				if ((col == bcol) && inclusive) {
					return TRUE;
				} else {
					return FALSE;
				}
			}
		} else {
			return FALSE;
		}
	}
	/* It's also "between" if it's on the line where the area starts and
	 * at or after the start column, or on the line where the area ends and
	 * before the end column. */
	if ((row == arow) && (col >= acol)) {
		return TRUE;
	} else {
		if (row == brow) {
			if (col < bcol) {
				return TRUE;
			} else {
				if ((col == bcol) && inclusive) {
					return TRUE;
				} else {
					return FALSE;
				}
			}
		} else {
			return FALSE;
		}
	}
	return FALSE;
}

/* Check if a cell is selected or not. */
static gboolean
vte_cell_is_selected(VteTerminal *terminal, glong col, glong row, gpointer data)
{
	struct selection_cell_coords ss, se;

	/* If there's nothing selected, it's an easy question to answer. */
	if (!terminal->pvt->has_selection) {
		return FALSE;
	}

	/* If the selection is obviously bogus, then it's also very easy. */
	ss = terminal->pvt->selection_start;
	se = terminal->pvt->selection_end;
	if ((ss.y < 0) || (se.y < 0)) {
		return FALSE;
	}

	/* Now it boils down to whether or not the point is between the
	 * begin and endpoint of the selection. */
	return vte_cell_is_between(col, row, ss.x, ss.y, se.x, se.y, TRUE);
}

/* Once we get text data, actually paste it in. */
static void
vte_terminal_paste_cb(GtkClipboard *clipboard, const gchar *text, gpointer data)
{
	VteTerminal *terminal;
	gchar *paste, *p;
	long length;
	g_assert(VTE_IS_TERMINAL(data));
	terminal = VTE_TERMINAL(data);
	if (text != NULL) {
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_SELECTION)) {
			fprintf(stderr, "Pasting %d UTF-8 bytes.\n",
				strlen(text));
		}
#endif
		if (!g_utf8_validate(text, -1, NULL)) {
			g_warning(_("Error (%s) converting data for child, dropping."), strerror(EINVAL));
			return;
		}

		/* Convert newlines to carriage returns, which more software
		 * is able to cope with (cough, pico, cough). */
		paste = g_strdup(text);
		length = strlen(paste);
		p = paste;
		while ((p != NULL) && (p - paste < length)) {
			p = memchr(p, '\n', length - (p - paste));
			if (p != NULL) {
				*p = '\r';
				p++;
			}
		}
		vte_terminal_feed_child(terminal, paste, length);
		g_free(paste);
	}
}

/* Send a button down or up notification. */
static void
vte_terminal_send_mouse_button_internal(VteTerminal *terminal,
					int button,
					double x, double y)
{
	unsigned char cb = 0, cx = 0, cy = 0;
	char buf[LINE_MAX];

	g_assert(VTE_IS_TERMINAL(terminal));

	/* Encode the button information in cb. */
	switch (button) {
	case 0:			/* Release/no buttons. */
		cb = 3;
		break;
	case 1:			/* Left. */
		cb = 0;
		break;
	case 2:			/* Middle. */
		cb = 1;
		break;
	case 3:			/* Right. */
		cb = 2;
		break;
	case 4:
		cb = 64;	/* Scroll up. FIXME: check */
		break;
	case 5:
		cb = 65;	/* Scroll down. FIXME: check */
		break;
	}
	cb += 32; /* 32 for normal */

	/* Encode the modifiers. */
	if (terminal->pvt->modifiers & GDK_SHIFT_MASK) {
		cb |= 4;
	}
	if (terminal->pvt->modifiers & VTE_META_MASK) {
		cb |= 8;
	}
	if (terminal->pvt->modifiers & GDK_CONTROL_MASK) {
		cb |= 16;
	}

	/* Encode the cursor coordinates. */
	cx = 32 + CLAMP(1 + (x / terminal->char_width),
			1, terminal->column_count);
	cy = 32 + CLAMP(1 + (y / terminal->char_height),
			1, terminal->row_count);;

	/* Send the event to the child. */
	snprintf(buf, sizeof(buf), _VTE_CAP_CSI "M%c%c%c", cb, cx, cy);
	vte_terminal_feed_child(terminal, buf, strlen(buf));
}

/* Send a mouse button click/release notification. */
static void
vte_terminal_maybe_send_mouse_button(VteTerminal *terminal,
				     GdkEventButton *event)
{
	GdkModifierType modifiers;

	/* Read the modifiers. */
	if (gdk_event_get_state((GdkEvent*)event, &modifiers)) {
		terminal->pvt->modifiers = modifiers;
	}

	/* Decide whether or not to do anything. */
	switch (event->type) {
	case GDK_BUTTON_PRESS:
		if (!terminal->pvt->mouse_send_xy_on_button &&
		    !terminal->pvt->mouse_send_xy_on_click &&
		    !terminal->pvt->mouse_hilite_tracking &&
		    !terminal->pvt->mouse_cell_motion_tracking &&
		    !terminal->pvt->mouse_all_motion_tracking) {
			return;
		}
		break;
	case GDK_BUTTON_RELEASE:
		if (!terminal->pvt->mouse_send_xy_on_button &&
		    !terminal->pvt->mouse_hilite_tracking &&
		    !terminal->pvt->mouse_cell_motion_tracking &&
		    !terminal->pvt->mouse_all_motion_tracking) {
			return;
		}
		break;
	default:
		return;
		break;
	}

	/* Encode the parameters and send them to the app. */
	vte_terminal_send_mouse_button_internal(terminal,
						(event->type == GDK_BUTTON_PRESS) ?
						event->button : 0,
						event->x - VTE_PAD_WIDTH,
						event->y - VTE_PAD_WIDTH);
}

/* Send a mouse motion notification. */
static void
vte_terminal_maybe_send_mouse_drag(VteTerminal *terminal, GdkEventMotion *event)
{
	unsigned char cb = 0, cx = 0, cy = 0;
	char buf[LINE_MAX];

	g_assert(VTE_IS_TERMINAL(terminal));

	/* First determine if we even want to send notification. */
	switch (event->type) {
	case GDK_MOTION_NOTIFY:
		if (!terminal->pvt->mouse_cell_motion_tracking &&
		    !terminal->pvt->mouse_all_motion_tracking) {
			return;
		}
		if (!terminal->pvt->mouse_all_motion_tracking) {
			if (terminal->pvt->mouse_last_button == 0) {
				return;
			}
			if ((floor((event->x - VTE_PAD_WIDTH) /
				   terminal->char_width) ==
			     floor(terminal->pvt->mouse_last_x /
				   terminal->char_width)) &&
			    (floor((event->y - VTE_PAD_WIDTH) /
				   terminal->char_height) ==
			     floor(terminal->pvt->mouse_last_y /
				   terminal->char_height))) {
				return;
			}
		}
		break;
	default:
		return;
		break;
	}

	/* Encode which button we're being dragged with. */
	switch (terminal->pvt->mouse_last_button) {
	case 0:
		cb = 3;
		break;
	case 1:
		cb = 0;
		break;
	case 2:
		cb = 1;
		break;
	case 3:
		cb = 2;
		break;
	case 4:
		cb = 64;	/* FIXME: check */
		break;
	case 5:
		cb = 65;	/* FIXME: check */
		break;
	}
	cb += 64; /* 32 for normal, 32 for movement */

	/* Encode the modifiers. */
	if (terminal->pvt->modifiers & GDK_SHIFT_MASK) {
		cb |= 4;
	}
	if (terminal->pvt->modifiers & VTE_META_MASK) {
		cb |= 8;
	}
	if (terminal->pvt->modifiers & GDK_CONTROL_MASK) {
		cb |= 16;
	}

	/* Encode the cursor coordinates. */
	cx = 32 + CLAMP(1 + ((event->x - VTE_PAD_WIDTH) / terminal->char_width),
			1, terminal->column_count);
	cy = 32 + CLAMP(1 + ((event->y - VTE_PAD_WIDTH) / terminal->char_height),
			1, terminal->row_count);;

	/* Send the event to the child. */
	snprintf(buf, sizeof(buf), "%sM%c%c%c", _VTE_CAP_CSI, cb, cx, cy);
	vte_terminal_feed_child(terminal, buf, strlen(buf));
}

/* Clear all match hilites. */
static void
vte_terminal_match_hilite_clear(VteTerminal *terminal)
{
	long srow, scolumn, erow, ecolumn;
	srow = terminal->pvt->match_start.row;
	scolumn = terminal->pvt->match_start.column;
	erow = terminal->pvt->match_end.row;
	ecolumn = terminal->pvt->match_end.column;
	terminal->pvt->match_start.row = -1;
	terminal->pvt->match_start.column = -1;
	terminal->pvt->match_end.row = -2;
	terminal->pvt->match_end.column = -2;
	if ((srow < erow) || ((srow == erow) && (scolumn < ecolumn))) {
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_EVENTS)) {
			fprintf(stderr, "Repainting (%ld,%ld) to (%ld,%ld).\n",
				srow, scolumn, erow, ecolumn);
		}
#endif
		_vte_invalidate_cells(terminal,
				     0, terminal->column_count,
				     srow, erow - srow + 1);
	}
}

/* Update the hilited text if the pointer has moved to a new character cell. */
static void
vte_terminal_match_hilite(VteTerminal *terminal, double x, double y)
{
	int start, end, width, height;
	long rows, rowe;
	char *match;
	struct _VteCharAttributes *attr;
	VteScreen *screen;
	long delta;

	width = terminal->char_width;
	height = terminal->char_height;

	/* If the pointer hasn't moved to another character cell, then we
	 * need do nothing. */
	if ((x / width == terminal->pvt->mouse_last_x / width) &&
	    (y / height == terminal->pvt->mouse_last_y / height)) {
		return;
	}

	/* Check for matches. */
	screen = terminal->pvt->screen;
	delta = screen->scroll_delta;
	match = vte_terminal_match_check_internal(terminal,
						  floor(x) / width,
						  floor(y) / height + delta,
						  NULL,
						  &start,
						  &end);

	/* If there are no matches, repaint what we had matched before. */
	if (match == NULL) {
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_EVENTS)) {
			fprintf(stderr, "No matches.\n");
		}
#endif
		vte_terminal_match_hilite_clear(terminal);
	} else {
		/* Save the old hilite area. */
		rows = terminal->pvt->match_start.row;
		rowe = terminal->pvt->match_end.row;
		/* Read the new locations. */
		attr = &g_array_index(terminal->pvt->match_attributes,
				      struct _VteCharAttributes,
				      start);
		terminal->pvt->match_start.row = attr->row;
		terminal->pvt->match_start.column = attr->column;
		attr = &g_array_index(terminal->pvt->match_attributes,
				      struct _VteCharAttributes,
				      end);
		terminal->pvt->match_end.row = attr->row;
		terminal->pvt->match_end.column = attr->column;
		/* Repaint the newly-hilited area. */
		_vte_invalidate_cells(terminal,
				     0,
				     terminal->column_count,
				     terminal->pvt->match_start.row,
				     terminal->pvt->match_end.row -
				     terminal->pvt->match_start.row + 1);
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_EVENTS)) {
			fprintf(stderr, "Matched (%ld,%ld) to (%ld,%ld).\n",
				terminal->pvt->match_start.column,
				terminal->pvt->match_start.row,
				terminal->pvt->match_end.column,
				terminal->pvt->match_end.row);
		}
#endif
		/* Repaint what used to be hilited, if anything. */
		_vte_invalidate_cells(terminal,
				     0, terminal->column_count,
				     rows, rowe - rows + 1);
	}
}


/* Note that the clipboard has cleared. */
static void
vte_terminal_clear_cb(GtkClipboard *clipboard, gpointer owner)
{
	VteTerminal *terminal;
	g_assert(VTE_IS_TERMINAL(owner));
	terminal = VTE_TERMINAL(owner);
	if (terminal->pvt->has_selection) {
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_SELECTION)) {
			fprintf(stderr, "Lost selection.\n");
		}
#endif
		vte_terminal_deselect_all(terminal);
	}
}

/* Supply the selected text to the clipboard. */
static void
vte_terminal_copy_cb(GtkClipboard *clipboard, GtkSelectionData *data,
		     guint info, gpointer owner)
{
	VteTerminal *terminal;
	g_assert(VTE_IS_TERMINAL(owner));
	terminal = VTE_TERMINAL(owner);
	if (terminal->pvt->selection != NULL) {
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_SELECTION)) {
			int i;
			fprintf(stderr, "Setting selection (%d UTF-8 bytes.)\n",
				strlen(terminal->pvt->selection));
			for (i = 0; terminal->pvt->selection[i] != '\0'; i++) {
				fprintf(stderr, "0x%04x\n",
					terminal->pvt->selection[i]);
			}
		}
#endif
		gtk_selection_data_set_text(data, terminal->pvt->selection, -1);
	}
}

/**
 * vte_terminal_get_text_range:
 * @terminal: a #VteTerminal
 * @start_row: first row to search for data
 * @start_col: first column to search for data
 * @end_row: last row to search for data
 * @end_col: last column to search for data
 * @is_selected: a callback
 * @data: user data to be passed to the callback
 * @attributes: location for storing text attributes
 *
 * Extracts a view of the visible part of the terminal.  If @is_selected is not
 * NULL, characters will only be read if @is_selected returns TRUE after being
 * passed the column and row, respectively.  A #VteCharAttributes structure
 * is added to @attributes for each byte added to the returned string detailing
 * the character's position, colors, and other characteristics.  The
 * entire scrollback buffer is scanned, so it is possible to read the entire
 * contents of the buffer using this function.
 *
 * Returns: a text string which must be freed by the caller, or NULL.
 */
char *
vte_terminal_get_text_range(VteTerminal *terminal,
			    glong start_row, glong start_col,
			    glong end_row, glong end_col,
			    gboolean(*is_selected)(VteTerminal *,
						   glong,
						   glong,
						   gpointer),
			    gpointer data,
			    GArray *attributes)
{
	g_return_val_if_fail(VTE_IS_TERMINAL(terminal), NULL);
	return vte_terminal_get_text_range_maybe_wrapped(terminal,
							 start_row, start_col,
							 end_row, end_col,
							 TRUE,
							 is_selected,
							 data,
							 attributes,
							 FALSE);
}

static char *
vte_terminal_get_text_range_maybe_wrapped(VteTerminal *terminal,
					  glong start_row, glong start_col,
					  glong end_row, glong end_col,
					  gboolean wrap,
					  gboolean(*is_selected)(VteTerminal *,
								 glong,
								 glong,
								 gpointer),
					  gpointer data,
					  GArray *attributes,
					  gboolean include_trailing_spaces)
{
	long col, row, last_space, last_spacecol,
	     last_nonspace, last_nonspacecol, line_start;
	VteScreen *screen;
	struct vte_charcell *pcell = NULL;
	GString *string;
	struct _VteCharAttributes attr;
	struct vte_palette_entry fore, back, *palette;

	g_assert(VTE_IS_TERMINAL(terminal));
	g_assert(is_selected != NULL);
	screen = terminal->pvt->screen;

	string = g_string_new("");
	memset(&attr, 0, sizeof(attr));

	palette = terminal->pvt->palette;
	for (row = start_row; row <= end_row; row++) {
		col = (row == start_row) ? start_col : 0;
		last_space = last_nonspace = -1;
		last_spacecol = last_nonspacecol = -1;
		attr.row = row;
		line_start = string->len;
		pcell = NULL;
		do {
			/* If it's not part of a multi-column character,
			 * and passes the selection criterion, add it to
			 * the selection. */
			attr.column = col;
			pcell = vte_terminal_find_charcell(terminal, col, row);
			if (pcell == NULL) {
				/* No more characters on this line. */
				break;
			}
			if (!pcell->fragment &&
			    is_selected(terminal, col, row, data)) {
				/* Store the attributes of this character. */
				fore = palette[pcell->fore];
				back = palette[pcell->back];
				attr.fore.red = fore.red;
				attr.fore.green = fore.green;
				attr.fore.blue = fore.blue;
				attr.back.red = back.red;
				attr.back.green = back.green;
				attr.back.blue = back.blue;
				attr.underline = pcell->underline;
				attr.strikethrough = pcell->strikethrough;
				/* Store the character. */
				string = g_string_append_unichar(string,
								 pcell->c ?
								 pcell->c :
								 ' ');
				/* Record whether or not this was a
				 * whitespace cell. */
				if ((pcell->c == ' ') || (pcell->c == '\0')) {
					last_space = string->len - 1;
					last_spacecol = col;
				} else {
					last_nonspace = string->len - 1;
					last_nonspacecol = col;
				}
			}
			/* If we added a character to the string, record its
			 * attributes, one per byte. */
			if (attributes) {
				vte_g_array_fill(attributes,
						 &attr, string->len);
			}
			/* If we're on the last line, and have just looked in
			 * the last column, stop. */
			if ((row == end_row) && (col == end_col)) {
				break;
			}
			col++;
		} while (pcell != NULL);
		/* If the last thing we saw was a space, and we stopped at the
		 * right edge of the selected area, trim the trailing spaces
		 * off of the line. */
		if ((last_space != -1) &&
		    (last_nonspace != -1) &&
		    (last_space > last_nonspace)) {
			/* Check for non-space after this point on the line. */
			col = MAX(0, last_spacecol);
			do {
				/* Check that we have data here. */
				pcell = vte_terminal_find_charcell(terminal,
								   col, row);
				/* Stop if we ran out of data. */
				if (pcell == NULL) {
					break;
				}
				/* Skip over fragments. */
				if (pcell->fragment) {
					col++;
					continue;
				}
				/* Check whether or not it holds whitespace. */
				if ((pcell->c != ' ') && (pcell->c != '\0')) {
					/* It holds non-whitespace, stop. */
					break;
				}
				col++;
			} while (pcell != NULL);
			/* If pcell is NULL, then there was no printing
			 * character to the right of the endpoint, so truncate
			 * the string at the end of the printing chars. */
			if ((pcell == NULL) && !include_trailing_spaces) {
				g_string_truncate(string, last_nonspace + 1);
			}
		}
		/* If we found no non-whitespace characters on this line, trim
		 * it, as xterm does. */
		if (last_nonspacecol == -1) {
			g_string_truncate(string, line_start);
		}
		/* Make sure that the attributes array is as long as the
		 * string. */
		if (attributes) {
			g_array_set_size(attributes, string->len);
		}
		/* If the last visible column on this line was selected and
		 * it contained whitespace, append a newline. */
		if (is_selected(terminal, terminal->column_count - 1,
				row, data)) {
			pcell = vte_terminal_find_charcell(terminal,
							   terminal->column_count - 1,
							   row);
			/* If it's whitespace, we snipped it off, so add a
			 * newline, unless we soft-wrapped. */
			if ((pcell == NULL) ||
			    (pcell->c == '\0') ||
			    (pcell->c == ' ')) {
				if (vte_line_is_wrappable(terminal, row) &&
				    wrap) {
					string = g_string_append_c(string,
								   pcell ?
								   pcell->c :
								   ' ');
				} else {
					string = g_string_append_c(string,
								   '\n');
				}
			}
			/* Move this last character to the end of the line. */
			attr.column = MAX(terminal->column_count,
					  attr.column + 1);
			/* If we broke out of the loop, there's at least one
			 * character with missing attributes. */
			if (attributes) {
				vte_g_array_fill(attributes, &attr,
						 string->len);
			}
		}
	}
	/* Sanity check. */
	if (attributes) {
		g_assert(string->len == attributes->len);
	}
	return g_string_free(string, FALSE);
}

static char *
vte_terminal_get_text_maybe_wrapped(VteTerminal *terminal,
				    gboolean wrap,
				    gboolean(*is_selected)(VteTerminal *,
							   glong,
							   glong,
							   gpointer),
				    gpointer data,
				    GArray *attributes,
				    gboolean include_trailing_spaces)
{
	long start_row, start_col, end_row, end_col;
	start_row = terminal->pvt->screen->scroll_delta;
	start_col = 0;
	end_row = start_row + terminal->row_count - 1;
	end_col = terminal->column_count - 1;
	return vte_terminal_get_text_range_maybe_wrapped(terminal,
							 start_row, start_col,
							 end_row, end_col,
							 wrap,
							 is_selected ?
							 is_selected :
							 always_selected,
							 data,
							 attributes,
							 include_trailing_spaces);
}

/**
 * vte_terminal_get_text:
 * @terminal: a #VteTerminal
 * @is_selected: a callback
 * @data: user data to be passed to the callback
 * @attributes: location for storing text attributes
 *
 * Extracts a view of the visible part of the terminal.  If @is_selected is not
 * NULL, characters will only be read if @is_selected returns TRUE after being
 * passed the column and row, respectively.  A #VteCharAttributes structure
 * is added to @attributes for each byte added to the returned string detailing
 * the character's position, colors, and other characteristics.
 *
 * Returns: a text string which must be freed by the caller, or NULL.
 */
char *
vte_terminal_get_text(VteTerminal *terminal,
		      gboolean(*is_selected)(VteTerminal *,
					     glong,
					     glong,
					     gpointer),
		      gpointer data,
		      GArray *attributes)
{
	g_return_val_if_fail(VTE_IS_TERMINAL(terminal), NULL);
	return vte_terminal_get_text_maybe_wrapped(terminal,
						   TRUE,
						   is_selected ?
						   is_selected :
						   always_selected,
						   data,
						   attributes,
						   FALSE);
}

/**
 * vte_terminal_get_text_include_trailing_spaces:
 * @terminal: a #VteTerminal
 * @is_selected: a callback
 * @data: user data to be passed to the callback
 * @attributes: location for storing text attributes
 *
 * Extracts a view of the visible part of the terminal.  If @is_selected is not
 * NULL, characters will only be read if @is_selected returns TRUE after being
 * passed the column and row, respectively.  A #VteCharAttributes structure
 * is added to @attributes for each byte added to the returned string detailing
 * the character's position, colors, and other characteristics. This function
 * differs from vte_terminal_get_text in that trailing spaces at the end of
 * lines are included.
 *
 * Returns: a text string which must be freed by the caller, or NULL.
 *
 * Since 0.11.11
 */
char *
vte_terminal_get_text_include_trailing_spaces(VteTerminal *terminal,
					      gboolean(*is_selected)(VteTerminal *,
								     glong,
								     glong,
								     gpointer),
					      gpointer data,
					      GArray *attributes)
{
	g_return_val_if_fail(VTE_IS_TERMINAL(terminal), NULL);
	return vte_terminal_get_text_maybe_wrapped(terminal,
						   TRUE,
						   is_selected ?
						   is_selected :
						   always_selected,
						   data,
						   attributes,
						   TRUE);
}

/**
 * vte_terminal_get_cursor_position:
 * @terminal: a #VteTerminal
 * @column: long which will hold the column
 * @row : long which will hold the row
 *
 * Reads the location of the insertion cursor and returns it.  The row
 * coordinate is absolute.
 *
 */
void
vte_terminal_get_cursor_position(VteTerminal *terminal,
				 glong *column, glong *row)
{
	g_return_if_fail(VTE_IS_TERMINAL(terminal));
	if (column) {
		*column = terminal->pvt->screen->cursor_current.col;
	}
	if (row) {
		*row = terminal->pvt->screen->cursor_current.row;
	}
}

static GtkClipboard *
vte_terminal_clipboard_get(VteTerminal *terminal, GdkAtom board)
{
#if GTK_CHECK_VERSION(2,2,0)
	GdkDisplay *display;
	display = gtk_widget_get_display(GTK_WIDGET(terminal));
	return gtk_clipboard_get_for_display(display, board);
#else
	return gtk_clipboard_get(board);
#endif
}

/* Place the selected text onto the clipboard.  Do this asynchronously so that
 * we get notified when the selection we placed on the clipboard is replaced. */
static void
vte_terminal_copy(VteTerminal *terminal, GdkAtom board)
{
	GtkClipboard *clipboard;
	static GtkTargetEntry *targets = NULL;
	static gint n_targets = 0;

	g_assert(VTE_IS_TERMINAL(terminal));
	clipboard = vte_terminal_clipboard_get(terminal, board);

	/* Chuck old selected text and retrieve the newly-selected text. */
	if (terminal->pvt->selection != NULL) {
		g_free(terminal->pvt->selection);
	}
	terminal->pvt->selection =
		vte_terminal_get_text_range(terminal,
					    terminal->pvt->selection_start.y,
					    0,
					    terminal->pvt->selection_end.y,
					    terminal->column_count,
					    vte_cell_is_selected,
					    NULL,
					    NULL);

	/* Place the text on the clipboard. */
	if (terminal->pvt->selection != NULL) {
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_SELECTION)) {
			fprintf(stderr, "Assuming ownership of selection.\n");
		}
#endif
		if (!targets) {
			GtkTargetList *list;
			GList *l;
			int i;

			list = gtk_target_list_new (NULL, 0);
			gtk_target_list_add_text_targets (list, 0);
			
			n_targets = g_list_length (list->list);
			targets = g_new0 (GtkTargetEntry, n_targets);
			for (l = list->list, i = 0; l; l = l->next, i++) {
				GtkTargetPair *pair = (GtkTargetPair *)l->data;
				targets[i].target = gdk_atom_name (pair->target);
			}
			gtk_target_list_unref (list);
		}

		gtk_clipboard_set_with_owner(clipboard,
					     targets,
					     n_targets,
					     vte_terminal_copy_cb,
					     vte_terminal_clear_cb,
					     G_OBJECT(terminal));
		gtk_clipboard_set_can_store(clipboard, NULL, 0);
	}
}

/* Paste from the given clipboard. */
static void
vte_terminal_paste(VteTerminal *terminal, GdkAtom board)
{
	GtkClipboard *clipboard;
	g_assert(VTE_IS_TERMINAL(terminal));
	clipboard = vte_terminal_clipboard_get(terminal, board);
	if (clipboard != NULL) {
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_SELECTION)) {
			fprintf(stderr, "Requesting clipboard contents.\n");
		}
#endif
		gtk_clipboard_request_text(clipboard,
					   vte_terminal_paste_cb,
					   terminal);
	}
}

/* Start selection at the location of the event. */
static void
vte_terminal_start_selection(VteTerminal *terminal, GdkEventButton *event,
			     enum vte_selection_type selection_type)
{
	long cellx, celly, delta;

	/* Convert the event coordinates to cell coordinates. */
	delta = terminal->pvt->screen->scroll_delta;
	cellx = (event->x - VTE_PAD_WIDTH) / terminal->char_width;
	celly = (event->y - VTE_PAD_WIDTH) / terminal->char_height + delta;

	/* Record that we have the selection, and where it started. */
	terminal->pvt->has_selection = TRUE;
	terminal->pvt->selection_last.x = event->x - VTE_PAD_WIDTH;
	terminal->pvt->selection_last.y = event->y - VTE_PAD_WIDTH +
					  (terminal->char_height * delta);

	/* Decide whether or not to restart on the next drag. */
	switch (selection_type) {
	case selection_type_char:
		/* Restart selection once we register a drag. */
		terminal->pvt->selecting_restart = TRUE;
		terminal->pvt->has_selection = FALSE;
		terminal->pvt->selecting_had_delta = FALSE;

		terminal->pvt->selection_restart_origin =
			terminal->pvt->selection_last;
		break;
	case selection_type_word:
	case selection_type_line:
		/* Mark the newly-selected areas now. */
		terminal->pvt->selecting_restart = FALSE;
		terminal->pvt->has_selection = TRUE;
		terminal->pvt->selecting_had_delta = FALSE;

		terminal->pvt->selection_start.x = cellx;
		terminal->pvt->selection_start.y = celly;
		terminal->pvt->selection_end = terminal->pvt->selection_start;
		terminal->pvt->selection_origin =
			terminal->pvt->selection_last;
		break;
	}

	/* Record the selection type. */
	terminal->pvt->selection_type = selection_type;
	terminal->pvt->selecting = TRUE;

	/* Draw the row the selection started on. */
	_vte_invalidate_cells(terminal,
			     0, terminal->column_count,
			     terminal->pvt->selection_start.y, 1);
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_SELECTION)) {
		fprintf(stderr, "Selection started at (%ld,%ld).\n",
			terminal->pvt->selection_start.x,
			terminal->pvt->selection_start.y);
	}
#endif
	vte_terminal_emit_selection_changed(terminal);

	/* Temporarily stop caring about input from the child. */
	_vte_terminal_disconnect_pty_read(terminal);
}

/* Extend selection to include the given event coordinates. */
static void
vte_terminal_extend_selection(VteTerminal *terminal, double x, double y,
			      gboolean always_grow)
{
	VteScreen *screen;
	VteRowData *rowdata;
	long delta, height, width, last_nonspace, i, j;
	struct vte_charcell *cell;
	struct selection_event_coords *origin, *last, *start, *end;
	struct selection_cell_coords old_start, old_end, *sc, *ec, tc;
	gboolean invalidate_selected = FALSE;

	screen = terminal->pvt->screen;
	old_start = terminal->pvt->selection_start;
	old_end = terminal->pvt->selection_end;
	height = terminal->char_height;
	width = terminal->char_width;

	/* Convert the event coordinates to cell coordinates. */
	delta = screen->scroll_delta;

	/* If we're restarting on a drag, then mark this as the start of
	 * the selected block. */
	if (terminal->pvt->selecting_restart) {
		vte_terminal_deselect_all(terminal);
		invalidate_selected = TRUE;
		/* Record the origin of the selection. */
		terminal->pvt->selection_origin =
			terminal->pvt->selection_restart_origin;
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_SELECTION)) {
			fprintf(stderr, "Selection delayed start at (%lf,%lf).\n",
				terminal->pvt->selection_origin.x / width,
				terminal->pvt->selection_origin.y / height);
		}
#endif
	}

	/* Recognize that we've got a selected block. */
	terminal->pvt->has_selection = TRUE;
	terminal->pvt->selecting_had_delta = TRUE;
	terminal->pvt->selecting_restart = FALSE;

	/* If we're not in always-grow mode, update the last location of
	 * the selection. */
	last = &terminal->pvt->selection_last;
	if (!always_grow) {
		last->x = x;
		last->y = y + height * delta;
	}

	/* Map the origin and last selected points to a start and end. */
	origin = &terminal->pvt->selection_origin;
	if ((origin->y / height < last->y / height) ||
	    ((origin->y / height == last->y / height) &&
	     (origin->x / width < last->x / width ))) {
		/* The origin point is "before" the last point. */
		start = origin;
		end = last;
	} else {
		/* The last point is "before" the origin point. */
		start = last;
		end = origin;
	}

	/* Extend the selection by moving whichever end of the selection is
	 * closer to the new point. */
	if (always_grow) {
		/* New endpoint is before existing selection. */
		if ((y / height < ((start->y / height) - delta)) ||
		    ((y / height == ((start->y / height) - delta)) &&
		     (x / width < start->x / width))) {
			start->x = x;
			start->y = y + height * delta;
		} else {
			/* New endpoint is after existing selection. */
			end->x = x;
			end->y = y + height * delta;
		}
	}

#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_SELECTION)) {
		fprintf(stderr, "Selection is (%lf,%lf) to (%lf,%lf).\n",
			start->x, start->y, end->x, end->y);
	}
#endif

	/* Recalculate the selection area in terms of cell positions. */
	terminal->pvt->selection_start.x = MAX(0, start->x / width);
	terminal->pvt->selection_start.y = MAX(0, start->y / height);
	terminal->pvt->selection_end.x = MAX(0, end->x / width);
	terminal->pvt->selection_end.y = MAX(0, end->y / height);

	/* Re-sort using cell coordinates to catch round-offs that make two
	 * coordinates "the same". */
	sc = &terminal->pvt->selection_start;
	ec = &terminal->pvt->selection_end;
	if ((sc->y > ec->y) || ((sc->y == ec->y) && (sc->x > ec->x))) {
		tc = *sc;
		*sc = *ec;
		*ec = tc;
	}

	/* Extend the selection to handle end-of-line cases, word, and line
	 * selection.  We do this here because calculating it once is cheaper
	 * than recalculating for each cell as we render it. */

	/* Handle end-of-line at the start-cell. */
	if (_vte_ring_contains(screen->row_data, sc->y)) {
		rowdata = _vte_ring_index(screen->row_data,
					  VteRowData *, sc->y);
	} else {
		rowdata = NULL;
	}
	if (rowdata != NULL) {
		/* Find the last non-space character on the first line. */
		last_nonspace = -1;
		for (i = 0; i < rowdata->cells->len; i++) {
			cell = &g_array_index(rowdata->cells,
					      struct vte_charcell, i);
			if (!g_unichar_isspace(cell->c) && (cell->c != '\0')) {
				last_nonspace = i;
			}
		}
		/* Now find the first space after it. */
		i = last_nonspace + 1;
		/* If the start point is to its right, then move the
		 * startpoint up to the beginning of the next line
		 * unless that would move the startpoint after the end
		 * point, or we're in select-by-line mode. */
		if ((sc->x > i) &&
		    (terminal->pvt->selection_type != selection_type_line)) {
			if (sc->y < ec->y) {
				sc->x = 0;
				sc->y++;
			} else {
				sc->x = i;
			}
		}
	} else {
		/* Snap to the leftmost column. */
		sc->x = 0;
	}

	/* Handle end-of-line at the end-cell. */
	if (_vte_ring_contains(screen->row_data, ec->y)) {
		rowdata = _vte_ring_index(screen->row_data,
					  VteRowData *, ec->y);
	} else {
		rowdata = NULL;
	}
	if (rowdata != NULL) {
		/* Find the last non-space character on the last line. */
		last_nonspace = -1;
		for (i = 0; i < rowdata->cells->len; i++) {
			cell = &g_array_index(rowdata->cells,
					      struct vte_charcell, i);
			if (!g_unichar_isspace(cell->c) && (cell->c != '\0')) {
				last_nonspace = i;
			}
		}
		/* Now find the first space after it. */
		i = last_nonspace + 1;
		/* If the end point is to its right, then extend the
		 * endpoint as far right as we can expect. */
		if (ec->x >= i) {
			ec->x = MAX(ec->x,
				    MAX(terminal->column_count - 1,
					rowdata->cells->len));
		}
	} else {
		/* Snap to the rightmost column. */
		ec->x = MAX(ec->x, terminal->column_count - 1);
	}

	/* Now extend again based on selection type. */
	switch (terminal->pvt->selection_type) {
	case selection_type_char:
		/* Nothing more to do. */
		break;
	case selection_type_word:
		/* Keep selecting to the left as long as the next character we
		 * look at is of the same class as the current start point. */
		i = sc->x;
		j = sc->y;
		while (_vte_ring_contains(screen->row_data, j)) {
			/* Get the data for the row we're looking at. */
			rowdata = _vte_ring_index(screen->row_data,
						  VteRowData *, j);
			if (rowdata == NULL) {
				break;
			}
			/* Back up. */
			for (i = (j == sc->y) ?
				 sc->x :
				 terminal->column_count - 1;
			     i > 0;
			     i--) {
				if (vte_uniform_class(terminal,
						      j,
						      i - 1,
						      i)) {
					sc->x = i - 1;
					sc->y = j;
				} else {
					break;
				}
			}
			if (i > 0) {
				/* We hit a stopping point, so stop. */
				break;
			} else {
				if (vte_line_is_wrappable(terminal, j - 1)) {
					/* Move on to the previous line. */
					j--;
					sc->x = terminal->column_count - 1;
					sc->y = j;
				} else {
					break;
				}
			}
		}
		/* Keep selecting to the right as long as the next character we
		 * look at is of the same class as the current end point. */
		i = ec->x;
		j = ec->y;
		while (_vte_ring_contains(screen->row_data, j)) {
			/* Get the data for the row we're looking at. */
			rowdata = _vte_ring_index(screen->row_data,
						  VteRowData *, j);
			if (rowdata == NULL) {
				break;
			}
			/* Move forward. */
			for (i = (j == ec->y) ?
				 ec->x :
				 0;
			     i < terminal->column_count - 1;
			     i++) {
				if (vte_uniform_class(terminal,
						      j,
						      i,
						      i + 1)) {
					ec->x = i + 1;
					ec->y = j;
				} else {
					break;
				}
			}
			if (i < terminal->column_count - 1) {
				/* We hit a stopping point, so stop. */
				break;
			} else {
				if (vte_line_is_wrappable(terminal, j)) {
					/* Move on to the next line. */
					j++;
					ec->x = 0;
					ec->y = j;
				} else {
					break;
				}
			}
		}
		break;
	case selection_type_line:
		/* Extend the selection to the beginning of the start line. */
		sc->x = 0;
		/* Now back up as far as we can go. */
		j = sc->y;
		while (_vte_ring_contains(screen->row_data, j - 1) &&
		       vte_line_is_wrappable(terminal, j - 1)) {
			j--;
			sc->y = j;
		}
		/* And move forward as far as we can go. */
		j = ec->y;
		while (_vte_ring_contains(screen->row_data, j) &&
		       vte_line_is_wrappable(terminal, j)) {
			j++;
			ec->y = j;
		}
		/* Make sure we include all of the last line. */
		ec->x = terminal->column_count - 1;
		if (_vte_ring_contains(screen->row_data, ec->y)) {
			rowdata = _vte_ring_index(screen->row_data,
						  VteRowData *, ec->y);
			if (rowdata != NULL) {
				ec->x = MAX(ec->x, rowdata->cells->len);
			}
		}
		break;
	}

	/* Redraw the rows which contain cells which have changed their
	 * is-selected status. */
	if ((old_start.x != terminal->pvt->selection_start.x) ||
	    (old_start.y != terminal->pvt->selection_start.y)) {
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_SELECTION)) {
			fprintf(stderr, "Refreshing lines %ld to %ld.\n",
				MIN(old_start.y,
				    terminal->pvt->selection_start.y),
				MAX(old_start.y,
				    terminal->pvt->selection_start.y));
		}
#endif
		_vte_invalidate_cells(terminal,
				     0,
				     terminal->column_count,
				     MIN(old_start.y,
					 terminal->pvt->selection_start.y),
				     MAX(old_start.y,
					 terminal->pvt->selection_start.y) -
				     MIN(old_start.y,
					 terminal->pvt->selection_start.y) + 1);
	}
	if ((old_end.x != terminal->pvt->selection_end.x) ||
	    (old_end.y != terminal->pvt->selection_end.y)) {
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_SELECTION)) {
			fprintf(stderr, "Refreshing lines %ld to %ld.\n",
				MIN(old_end.y, terminal->pvt->selection_end.y),
				MAX(old_end.y, terminal->pvt->selection_end.y));
		}
#endif
		_vte_invalidate_cells(terminal,
				     0,
				     terminal->column_count,
				     MIN(old_end.y,
					 terminal->pvt->selection_end.y),
				     MAX(old_end.y,
					 terminal->pvt->selection_end.y) -
				     MIN(old_end.y,
					 terminal->pvt->selection_end.y) + 1);
	}
	if (invalidate_selected) {
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_SELECTION)) {
			fprintf(stderr, "Refreshing lines %ld to %ld.\n",
				MIN(terminal->pvt->selection_start.y,
				    terminal->pvt->selection_end.y),
				MAX(terminal->pvt->selection_start.y,
				    terminal->pvt->selection_end.y));
		}
#endif
		_vte_invalidate_cells(terminal,
				     0,
				     terminal->column_count,
				     MIN(terminal->pvt->selection_start.y,
				         terminal->pvt->selection_end.y),
				     MAX(terminal->pvt->selection_start.y,
					 terminal->pvt->selection_end.y) -
				     MIN(terminal->pvt->selection_start.y,
				         terminal->pvt->selection_end.y) + 1);
	}

#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_SELECTION)) {
		fprintf(stderr, "Selection changed to "
			"(%ld,%ld) to (%ld,%ld).\n",
			terminal->pvt->selection_start.x,
			terminal->pvt->selection_start.y,
			terminal->pvt->selection_end.x,
			terminal->pvt->selection_end.y);
	}
#endif
	vte_terminal_emit_selection_changed(terminal);
}

/* Autoscroll a bit. */
static int
vte_terminal_autoscroll(gpointer data)
{
	VteTerminal *terminal;
	GtkWidget *widget;
	gboolean extend = FALSE;
	gdouble x, y, xmax, ymax;
	glong adj;

	terminal = VTE_TERMINAL(data);
	widget = GTK_WIDGET(terminal);

	/* Provide an immediate effect for mouse wigglers. */
	if (terminal->pvt->mouse_last_y < 0) {
		if (terminal->adjustment) {
			/* Try to scroll up by one line. */
			adj = CLAMP(terminal->adjustment->value - 1,
				    terminal->adjustment->lower,
				    terminal->adjustment->upper -
				    terminal->row_count);
			gtk_adjustment_set_value(terminal->adjustment, adj);
			extend = TRUE;
		}
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_EVENTS)) {
			fprintf(stderr, "Autoscrolling down.\n");
		}
#endif
	}
	if (terminal->pvt->mouse_last_y >
	    terminal->row_count * terminal->char_height) {
		if (terminal->adjustment) {
			/* Try to scroll up by one line. */
			adj = CLAMP(terminal->adjustment->value + 1,
				    terminal->adjustment->lower,
				    terminal->adjustment->upper -
				    terminal->row_count);
			gtk_adjustment_set_value(terminal->adjustment, adj);
			extend = TRUE;
		}
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_EVENTS)) {
			fprintf(stderr, "Autoscrolling up.\n");
		}
#endif
	}
	if (extend) {
		/* Don't select off-screen areas.  That just confuses people. */
		xmax = terminal->column_count * terminal->char_width;
		ymax = terminal->row_count * terminal->char_height;

		x = CLAMP(terminal->pvt->mouse_last_x, 0, xmax);
		y = CLAMP(terminal->pvt->mouse_last_y, 0, ymax);
		/* If we clamped the Y, mess with the X to get the entire
		 * lines. */
		if (terminal->pvt->mouse_last_y < 0) {
			x = 0;
		}
		if (terminal->pvt->mouse_last_y > ymax) {
			x = terminal->column_count * terminal->char_width;
		}
		/* Extend selection to cover the newly-scrolled area. */
		vte_terminal_extend_selection(terminal, x, y, FALSE);
	} else {
		/* Stop autoscrolling. */
		terminal->pvt->mouse_autoscroll_tag = 0;
	}
	return (terminal->pvt->mouse_autoscroll_tag != 0);
}

/* Start autoscroll. */
static void
vte_terminal_start_autoscroll(VteTerminal *terminal)
{
	if (terminal->pvt->mouse_autoscroll_tag == 0) {
		terminal->pvt->mouse_autoscroll_tag =
			g_timeout_add_full(G_PRIORITY_LOW,
					   1000 / terminal->row_count,
					   vte_terminal_autoscroll,
					   terminal,
					   NULL);
	}
}

/* Stop autoscroll. */
static void
vte_terminal_stop_autoscroll(VteTerminal *terminal)
{
	if (terminal->pvt->mouse_autoscroll_tag != 0) {
		g_source_remove(terminal->pvt->mouse_autoscroll_tag);
		terminal->pvt->mouse_autoscroll_tag = 0;
	}
}

/* Read and handle a motion event. */
static gint
vte_terminal_motion_notify(GtkWidget *widget, GdkEventMotion *event)
{
	VteTerminal *terminal;
	GdkModifierType modifiers;
	gboolean event_mode;

	g_assert(VTE_IS_TERMINAL(widget));
	terminal = VTE_TERMINAL(widget);

	event_mode = terminal->pvt->mouse_send_xy_on_click ||
		     terminal->pvt->mouse_send_xy_on_button ||
		     terminal->pvt->mouse_hilite_tracking ||
		     terminal->pvt->mouse_cell_motion_tracking ||
		     terminal->pvt->mouse_all_motion_tracking;

#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_EVENTS)) {
		fprintf(stderr, "Motion notify (%lf,%lf).\n",
			event->x, event->y);
	}
#endif

	/* Show the cursor. */
	_vte_terminal_set_pointer_visible(terminal, TRUE);

	/* Read the modifiers. */
	if (gdk_event_get_state((GdkEvent*)event, &modifiers)) {
		terminal->pvt->modifiers = modifiers;
	}

	switch (event->type) {
	case GDK_MOTION_NOTIFY:
		switch (terminal->pvt->mouse_last_button) {
		case 1:
#ifdef VTE_DEBUG
			if (_vte_debug_on(VTE_DEBUG_EVENTS)) {
				fprintf(stderr, "Mousing drag 1.\n");
			}
#endif
			if ((terminal->pvt->modifiers & GDK_SHIFT_MASK) ||
			    !event_mode) {
				vte_terminal_extend_selection(terminal,
							      event->x - VTE_PAD_WIDTH,
							      event->y - VTE_PAD_WIDTH,
							      FALSE);
			} else {
				vte_terminal_maybe_send_mouse_drag(terminal,
								   event);
			}
			break;
		default:
			vte_terminal_maybe_send_mouse_drag(terminal, event);
			break;
		}
		break;
	default:
		break;
	}

	/* Start scrolling if we need to. */
	if ((event->y < VTE_PAD_WIDTH) ||
	    (event->y > (widget->allocation.height - VTE_PAD_WIDTH))) {
		switch (terminal->pvt->mouse_last_button) {
		case 1:
			if (!event_mode) {
				/* Give mouse wigglers something. */
				vte_terminal_autoscroll(terminal);
				/* Start a timed autoscroll if we're not doing it
				 * already. */
				vte_terminal_start_autoscroll(terminal);
			}
			break;
		case 2:
		case 3:
		default:
			break;
		}
	}

	/* Hilite any matches. */
	vte_terminal_match_hilite(terminal,
				  event->x - VTE_PAD_WIDTH,
				  event->y - VTE_PAD_WIDTH);

	/* Save the pointer coordinates for later use. */
	terminal->pvt->mouse_last_x = event->x - VTE_PAD_WIDTH;
	terminal->pvt->mouse_last_y = event->y - VTE_PAD_WIDTH;

	return TRUE;
}

/* Read and handle a pointing device buttonpress event. */
static gint
vte_terminal_button_press(GtkWidget *widget, GdkEventButton *event)
{
	VteTerminal *terminal;
	long height, width, delta;
	GdkModifierType modifiers;
	gboolean handled = FALSE, event_mode;
	gboolean start_selecting = FALSE, extend_selecting = FALSE;
	long cellx, celly;

	g_assert(VTE_IS_TERMINAL(widget));
	terminal = VTE_TERMINAL(widget);
	height = terminal->char_height;
	width = terminal->char_width;
	delta = terminal->pvt->screen->scroll_delta;
	_vte_terminal_set_pointer_visible(terminal, TRUE);

	event_mode = terminal->pvt->mouse_send_xy_on_click ||
		     terminal->pvt->mouse_send_xy_on_button ||
		     terminal->pvt->mouse_hilite_tracking ||
		     terminal->pvt->mouse_cell_motion_tracking ||
		     terminal->pvt->mouse_all_motion_tracking;

	/* Read the modifiers. */
	if (gdk_event_get_state((GdkEvent*)event, &modifiers)) {
		terminal->pvt->modifiers = modifiers;
	}

	/* Convert the event coordinates to cell coordinates. */
	cellx = (event->x - VTE_PAD_WIDTH) / width;
	celly = (event->y - VTE_PAD_WIDTH) / height + delta;

	switch (event->type) {
	case GDK_BUTTON_PRESS:
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_EVENTS)) {
			fprintf(stderr, "Button %d single-click at (%lf,%lf)\n",
				event->button,
				event->x - VTE_PAD_WIDTH,
				event->y - VTE_PAD_WIDTH +
				(terminal->char_height * delta));
		}
#endif
		/* Handle this event ourselves. */
		switch (event->button) {
		case 1:
#ifdef VTE_DEBUG
			if (_vte_debug_on(VTE_DEBUG_EVENTS)) {
				fprintf(stderr, "Handling click ourselves.\n");
			}
#endif
			/* Grab focus. */
			if (!GTK_WIDGET_HAS_FOCUS(widget)) {
				gtk_widget_grab_focus(widget);
			}

			/* If we're in event mode, and the user held down the
			 * shift key, we start selecting. */
			if (event_mode) {
				if (terminal->pvt->modifiers & GDK_SHIFT_MASK) {
					start_selecting = TRUE;
				}
			} else {
				/* If the user hit shift, then extend the
				 * selection instead. */
				if ((terminal->pvt->modifiers & GDK_SHIFT_MASK) &&
				    (terminal->pvt->has_selection ||
				     terminal->pvt->selecting_restart) &&
				    !vte_cell_is_selected(terminal,
							  cellx,
							  celly,
							  NULL)) {
					extend_selecting = TRUE;
				} else {
					start_selecting = TRUE;
				}
			}
			if (start_selecting) {
				vte_terminal_deselect_all(terminal);
				vte_terminal_start_selection(terminal,
							     event,
							     selection_type_char);
				handled = TRUE;
			}
			if (extend_selecting) {
				vte_terminal_extend_selection(terminal,
							      event->x - VTE_PAD_WIDTH,
							      event->y - VTE_PAD_WIDTH,
							      !terminal->pvt->selecting_restart);
				handled = TRUE;
			}
			break;
		/* Paste if the user pressed shift or we're not sending events
		 * to the app. */
		case 2:
			if ((terminal->pvt->modifiers & GDK_SHIFT_MASK) ||
			    !event_mode) {
				vte_terminal_paste_primary(terminal);
				handled = TRUE;
			}
			break;
		case 3:
		default:
			break;
		}
		/* If we haven't done anything yet, try sending the mouse
		 * event to the app. */
		if (handled == FALSE) {
			vte_terminal_maybe_send_mouse_button(terminal, event);
			handled = TRUE;
		}
		break;
	case GDK_2BUTTON_PRESS:
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_EVENTS)) {
			fprintf(stderr, "Button %d double-click at (%lf,%lf)\n",
				event->button,
				event->x - VTE_PAD_WIDTH,
				event->y - VTE_PAD_WIDTH +
				(terminal->char_height * delta));
		}
#endif
		switch (event->button) {
		case 1:
			if ((terminal->pvt->modifiers & GDK_SHIFT_MASK) ||
			    !event_mode) {
				vte_terminal_start_selection(terminal,
							     event,
							     selection_type_word);
				vte_terminal_extend_selection(terminal,
							      event->x - VTE_PAD_WIDTH,
							      event->y - VTE_PAD_WIDTH,
							      FALSE);
			}
			break;
		case 2:
		case 3:
		default:
			break;
		}
		break;
	case GDK_3BUTTON_PRESS:
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_EVENTS)) {
			fprintf(stderr, "Button %d triple-click at (%lf,%lf).\n",
				event->button,
				event->x - VTE_PAD_WIDTH,
				event->y - VTE_PAD_WIDTH +
				(terminal->char_height * delta));
		}
#endif
		switch (event->button) {
		case 1:
			if ((terminal->pvt->modifiers & GDK_SHIFT_MASK) ||
			    !event_mode) {
				vte_terminal_start_selection(terminal,
							     event,
							     selection_type_line);
				vte_terminal_extend_selection(terminal,
							      event->x - VTE_PAD_WIDTH,
							      event->y - VTE_PAD_WIDTH,
							      FALSE);
			}
			break;
		case 2:
		case 3:
		default:
			break;
		}
	default:
		break;
	}

	/* Hilite any matches. */
	vte_terminal_match_hilite(terminal,
				  event->x - VTE_PAD_WIDTH,
				  event->y - VTE_PAD_WIDTH);

	/* Save the pointer state for later use. */
	terminal->pvt->mouse_last_button = event->button;
	terminal->pvt->mouse_last_x = event->x - VTE_PAD_WIDTH;
	terminal->pvt->mouse_last_y = event->y - VTE_PAD_WIDTH;

	return TRUE;
}

/* Read and handle a pointing device buttonrelease event. */
static gint
vte_terminal_button_release(GtkWidget *widget, GdkEventButton *event)
{
	VteTerminal *terminal;
	GdkModifierType modifiers;
	gboolean handled = FALSE;
	gboolean event_mode;

	g_assert(VTE_IS_TERMINAL(widget));
	terminal = VTE_TERMINAL(widget);
	_vte_terminal_set_pointer_visible(terminal, TRUE);

	event_mode = terminal->pvt->mouse_send_xy_on_click ||
		     terminal->pvt->mouse_send_xy_on_button ||
		     terminal->pvt->mouse_hilite_tracking ||
		     terminal->pvt->mouse_cell_motion_tracking ||
		     terminal->pvt->mouse_all_motion_tracking;

	/* Disconnect from autoscroll requests. */
	vte_terminal_stop_autoscroll(terminal);

	/* Read the modifiers. */
	if (gdk_event_get_state((GdkEvent*)event, &modifiers)) {
		terminal->pvt->modifiers = modifiers;
	}

	switch (event->type) {
	case GDK_BUTTON_RELEASE:
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_EVENTS)) {
			fprintf(stderr, "Button %d released at (%lf,%lf).\n",
				event->button,
				event->x - VTE_PAD_WIDTH,
				event->y - VTE_PAD_WIDTH);
		}
#endif
		switch (event->button) {
		case 1:
			/* If Shift is held down, or we're not in events mode,
			 * copy the selected text. */
			if (terminal->pvt->selecting || !event_mode) {
				/* Copy only if something was selected. */
				if (terminal->pvt->has_selection &&
				    !terminal->pvt->selecting_restart &&
				    terminal->pvt->selecting_had_delta) {
					vte_terminal_copy_primary(terminal);
				}
				terminal->pvt->selecting = FALSE;
				handled = TRUE;
			}
			/* Reconnect to input from the child if we paused it. */
			_vte_terminal_connect_pty_read(terminal);
			break;
		case 2:
			if ((terminal->pvt->modifiers & GDK_SHIFT_MASK) ||
			    !event_mode) {
				handled = TRUE;
			}
			break;
		case 3:
		default:
			break;
		}
		if (handled == FALSE) {
			vte_terminal_maybe_send_mouse_button(terminal, event);
			handled = TRUE;
		}
		break;
	default:
		break;
	}

	/* Hilite any matches. */
	vte_terminal_match_hilite(terminal,
				  event->x - VTE_PAD_WIDTH,
				  event->y - VTE_PAD_WIDTH);

	/* Save the pointer state for later use. */
	terminal->pvt->mouse_last_button = 0;
	terminal->pvt->mouse_last_x = event->x - VTE_PAD_WIDTH;
	terminal->pvt->mouse_last_y = event->y - VTE_PAD_WIDTH;

	return TRUE;
}

/* Handle receiving or losing focus. */
static gint
vte_terminal_focus_in(GtkWidget *widget, GdkEventFocus *event)
{
	VteTerminal *terminal;
	GdkModifierType modifiers;
	g_assert(GTK_IS_WIDGET(widget));
	g_assert(VTE_IS_TERMINAL(widget));
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_EVENTS)) {
		fprintf(stderr, "Focus in.\n");
	}
#endif
	terminal = VTE_TERMINAL(widget);
	GTK_WIDGET_SET_FLAGS(widget, GTK_HAS_FOCUS);
	/* Read the keyboard modifiers, though they're probably garbage. */
	if (gdk_event_get_state((GdkEvent*)event, &modifiers)) {
		terminal->pvt->modifiers = modifiers;
	}
	/* We only have an IM context when we're realized, and there's not much
	 * point to painting the cursor if we don't have a window. */
	if (GTK_WIDGET_REALIZED(widget)) {
		gtk_im_context_focus_in(terminal->pvt->im_context);
		/* Force the cursor to be the foreground color twice, in case
		   we're in blinking mode and the next scheduled redraw occurs
		   just after the one we're about to perform. */
		terminal->pvt->cursor_force_fg = 2;
		_vte_invalidate_cursor_once(terminal, FALSE);
	}
	return FALSE;
}

static gint
vte_terminal_focus_out(GtkWidget *widget, GdkEventFocus *event)
{
	VteTerminal *terminal;
	GdkModifierType modifiers;
	g_assert(GTK_WIDGET(widget));
	g_assert(VTE_IS_TERMINAL(widget));
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_EVENTS)) {
		fprintf(stderr, "Focus out.\n");
	}
#endif
	terminal = VTE_TERMINAL(widget);
	GTK_WIDGET_UNSET_FLAGS(widget, GTK_HAS_FOCUS);
	/* Read the keyboard modifiers, though they're probably garbage. */
	if (gdk_event_get_state((GdkEvent*)event, &modifiers)) {
		terminal->pvt->modifiers = modifiers;
	}
	/* We only have an IM context when we're realized, and there's not much
	 * point to painting ourselves if we don't have a window. */
	if (GTK_WIDGET_REALIZED(widget)) {
		gtk_im_context_focus_out(terminal->pvt->im_context);
		_vte_invalidate_cursor_once(terminal, FALSE);
	}
	return FALSE;
}

static gint
vte_terminal_visibility_notify(GtkWidget *widget, GdkEventVisibility *event)
{
	VteTerminal *terminal;
	g_assert(GTK_WIDGET(widget));
	g_assert(VTE_IS_TERMINAL(widget));
	terminal = VTE_TERMINAL(widget);
	terminal->pvt->visibility_state = event->state;
	if (terminal->pvt->visibility_state == GDK_VISIBILITY_UNOBSCURED)
		_vte_invalidate_all(terminal);
	return FALSE;
}

/* Apply the changed metrics, and queue a resize if need be. */
static void
vte_terminal_apply_metrics(VteTerminal *terminal,
			   gint width, gint height, gint ascent, gint descent)
{
	gboolean resize = FALSE, cresize = FALSE;

	/* Sanity check for broken font changes. */
	width = MAX(width, 1);
	height = MAX(height, 2);
	ascent = MAX(ascent, 1);
	descent = MAX(descent, 1);

	/* Change settings, and keep track of when we've changed anything. */
	if (width != terminal->char_width) {
		resize = cresize = TRUE;
		terminal->char_width = width;
	}
	if (height != terminal->char_height) {
		resize = cresize = TRUE;
		terminal->char_height = height;
	}
	if (ascent != terminal->char_ascent) {
		resize = TRUE;
		terminal->char_ascent = ascent;
	}
	if (descent != terminal->char_descent) {
		resize = TRUE;
		terminal->char_descent = descent;
	}
	/* Queue a resize if anything's changed. */
	if (resize) {
		if (GTK_WIDGET_REALIZED(GTK_WIDGET(terminal))) {
			gtk_widget_queue_resize(GTK_WIDGET(terminal));
		}
	}
	/* Emit a signal that the font changed. */
	if (cresize) {
		vte_terminal_emit_char_size_changed(terminal,
						    terminal->char_width,
						    terminal->char_height);
	}
	/* Repaint. */
	_vte_invalidate_all(terminal);
}

/**
 * vte_terminal_set_font_full:
 * @terminal: a #VteTerminal
 * @font_desc: The #PangoFontDescription of the desired font.
 * @antialias: Specify if anti aliasing of the fonts is to be used or not.
 *
 * Sets the font used for rendering all text displayed by the terminal,
 * overriding any fonts set using gtk_widget_modify_font().  The terminal
 * will immediately attempt to load the desired font, retrieve its
 * metrics, and attempt to resize itself to keep the same number of rows
 * and columns.
 *
 * Since: 0.11.11
 */
void
vte_terminal_set_font_full(VteTerminal *terminal,
			   const PangoFontDescription *font_desc,
			   VteTerminalAntiAlias antialias)
{
	GtkWidget *widget;
	PangoFontDescription *desc;

	g_return_if_fail(terminal != NULL);
	g_return_if_fail(VTE_IS_TERMINAL(terminal));
	widget = GTK_WIDGET(terminal);

	/* Create an owned font description. */
	if (font_desc != NULL) {
		desc = pango_font_description_copy(font_desc);
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_MISC)) {
			if (desc) {
				char *tmp;
				tmp = pango_font_description_to_string(desc);
				fprintf(stderr, "Using pango font \"%s\".\n", tmp);
				g_free (tmp);
			}
		}
#endif
	} else {
		gtk_widget_ensure_style(widget);
		desc = pango_font_description_copy(widget->style->font_desc);
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_MISC)) {
			fprintf(stderr, "Using default pango font.\n");
		}
#endif
	}
	terminal->pvt->fontantialias = antialias;

	/* Free the old font description and save the new one. */
	if (terminal->pvt->fontdesc != NULL) {
		pango_font_description_free(terminal->pvt->fontdesc);
	}
	terminal->pvt->fontdesc = desc;
	terminal->pvt->fontantialias = antialias;

	/* Set the drawing font. */
	_vte_draw_set_text_font(terminal->pvt->draw,
				terminal->pvt->fontdesc,
				antialias);
	vte_terminal_apply_metrics(terminal,
				   _vte_draw_get_text_width(terminal->pvt->draw),
				   _vte_draw_get_text_height(terminal->pvt->draw),
				   _vte_draw_get_text_ascent(terminal->pvt->draw),
				   _vte_draw_get_text_height(terminal->pvt->draw) -
				   _vte_draw_get_text_ascent(terminal->pvt->draw));
	/* Repaint with the new font. */
	_vte_invalidate_all(terminal);
}

/**
 * vte_terminal_set_font:
 * @terminal: a #VteTerminal
 * @font_desc: The #PangoFontDescription of the desired font.
 *
 * Sets the font used for rendering all text displayed by the terminal,
 * overriding any fonts set using gtk_widget_modify_font().  The terminal
 * will immediately attempt to load the desired font, retrieve its
 * metrics, and attempt to resize itself to keep the same number of rows
 * and columns.
 *
 */
void
vte_terminal_set_font(VteTerminal *terminal,
		      const PangoFontDescription *font_desc)
{
	vte_terminal_set_font_full(terminal, font_desc,
				   VTE_ANTI_ALIAS_USE_DEFAULT);
}

/**
 * vte_terminal_set_font_from_string_full:
 * @terminal: a #VteTerminal
 * @name: A string describing the font.
 * @antialias: Whether or not to antialias the font (if possible).
 *
 * A convenience function which converts @name into a #PangoFontDescription and
 * passes it to vte_terminal_set_font_full().
 *
 * Since: 0.11.11
 */
void
vte_terminal_set_font_from_string_full(VteTerminal *terminal, const char *name,
				       VteTerminalAntiAlias antialias)
{
	PangoFontDescription *font_desc;
	g_return_if_fail(VTE_IS_TERMINAL(terminal));
	g_return_if_fail(name != NULL);
	g_return_if_fail(strlen(name) > 0);

	font_desc = pango_font_description_from_string(name);
	vte_terminal_set_font_full(terminal, font_desc, antialias);
	pango_font_description_free(font_desc);
}

/**
 * vte_terminal_set_font_from_string:
 * @terminal: a #VteTerminal
 * @name: A string describing the font.
 *
 * A convenience function which converts @name into a #PangoFontDescription and
 * passes it to vte_terminal_set_font().
 *
 */
void
vte_terminal_set_font_from_string(VteTerminal *terminal, const char *name)
{
	vte_terminal_set_font_from_string_full(terminal, name,
					       VTE_ANTI_ALIAS_USE_DEFAULT);
}

/**
 * vte_terminal_get_font:
 * @terminal: a #VteTerminal
 *
 * Queries the terminal for information about the fonts which will be
 * used to draw text in the terminal.
 *
 * Returns: a #PangoFontDescription describing the font the terminal is
 * currently using to render text.
 */
const PangoFontDescription *
vte_terminal_get_font(VteTerminal *terminal)
{
	g_return_val_if_fail(VTE_IS_TERMINAL(terminal), NULL);
	return terminal->pvt->fontdesc;
}

/* Read and refresh our perception of the size of the PTY. */
static void
vte_terminal_refresh_size(VteTerminal *terminal)
{
	int rows, columns;
	g_assert(VTE_IS_TERMINAL(terminal));
	if (terminal->pvt->pty_master != -1) {
		/* Use an ioctl to read the size of the terminal. */
		if (_vte_pty_get_size(terminal->pvt->pty_master, &columns, &rows) != 0) {
			g_warning(_("Error reading PTY size, using defaults: "
				    "%s."), strerror(errno));
		} else {
			terminal->row_count = rows;
			terminal->column_count = columns;
		}
	}
}

/**
 * vte_terminal_set_size:
 * @terminal: a #VteTerminal
 * @columns: the desired number of columns
 * @rows: the desired number of rows
 *
 * Attempts to change the terminal's size in terms of rows and columns.  If
 * the attempt succeeds, the widget will resize itself to the proper size.
 *
 */
void
vte_terminal_set_size(VteTerminal *terminal, glong columns, glong rows)
{
	struct winsize size;
	g_return_if_fail(VTE_IS_TERMINAL(terminal));
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_MISC)) {
		fprintf(stderr, "Setting PTY size to %ldx%ld.\n",
			columns, rows);
	}
#endif
	if (terminal->pvt->pty_master != -1) {
		memset(&size, 0, sizeof(size));
		size.ws_row = rows;
		size.ws_col = columns;
		/* Try to set the terminal size. */
		if (_vte_pty_set_size(terminal->pvt->pty_master, columns, rows) != 0) {
			g_warning(_("Error setting PTY size: %s."),
				    strerror(errno));
		}
	} else {
		terminal->row_count = rows;
		terminal->column_count = columns;
	}
	/* Read the terminal size, in case something went awry. */
	vte_terminal_refresh_size(terminal);
	/* Our visible text changed. */
	vte_terminal_emit_text_modified(terminal);
}

/* Redraw the widget. */
static void
vte_terminal_handle_scroll(VteTerminal *terminal)
{
	long dy, adj;
	GtkWidget *widget;
	VteScreen *screen;

	/* Sanity checks. */
	g_assert(GTK_IS_WIDGET(terminal));
	widget = GTK_WIDGET(terminal);
	screen = terminal->pvt->screen;
	if (GTK_WIDGET_REALIZED(widget) == FALSE) {
		return;
	}

	/* This may generate multiple redraws, so freeze it while we do them. */
	gdk_window_freeze_updates(widget->window);

	/* Read the new adjustment value and save the difference. */
	adj = floor(gtk_adjustment_get_value(terminal->adjustment));
	dy = adj - screen->scroll_delta;
	screen->scroll_delta = adj;
	if (dy != 0) {
#ifdef VTE_DEBUG
	      if (_vte_debug_on(VTE_DEBUG_IO)) {
		    fprintf(stderr, "Scrolling by %ld\n", dy);
	      }
#endif
		_vte_terminal_match_contents_clear(terminal);
		_vte_terminal_scroll_region(terminal, screen->scroll_delta,
					   terminal->row_count, -dy);
		vte_terminal_emit_text_scrolled(terminal, dy);
		_vte_terminal_emit_contents_changed(terminal);
	}
#ifdef VTE_DEBUG
	else if (_vte_debug_on(VTE_DEBUG_IO)) {
	      fprintf(stderr, "Not scrolling\n");
	}
#endif

	/* Let the refreshing begin. */
	gdk_window_thaw_updates(widget->window);
}

/* Set the adjustment objects used by the terminal widget. */
static void
vte_terminal_set_scroll_adjustment(VteTerminal *terminal,
				   GtkAdjustment *adjustment)
{
	g_assert(VTE_IS_TERMINAL(terminal));
	if (adjustment != NULL) {
		/* Add a reference to the new adjustment object. */
		g_object_ref(adjustment);
		/* Get rid of the old adjustment object. */
		if (terminal->adjustment != NULL) {
			/* Disconnect our signal handlers from this object. */
			g_signal_handlers_disconnect_by_func(terminal->adjustment,
							     (gpointer)vte_terminal_handle_scroll,
							     terminal);
			g_object_unref(terminal->adjustment);
		}
		/* Set the new adjustment object. */
		terminal->adjustment = adjustment;

		/* We care about the offset, not the top or bottom. */
		g_signal_connect_swapped(terminal->adjustment,
					 "value_changed",
					 G_CALLBACK(vte_terminal_handle_scroll),
					 terminal);
	}
}

/**
 * vte_terminal_set_emulation:
 * @terminal: a #VteTerminal
 * @emulation: the name of a terminal description
 *
 * Sets what type of terminal the widget attempts to emulate by scanning for
 * control sequences defined in the system's termcap file.  Unless you
 * are interested in this feature, always use "xterm".
 *
 */
void
vte_terminal_set_emulation(VteTerminal *terminal, const char *emulation)
{
	int columns, rows;
	GQuark quark;

	/* Set the emulation type, for reference. */
	if (emulation == NULL) {
		emulation = vte_terminal_get_default_emulation(terminal);
	}
	quark = g_quark_from_string(emulation);
	terminal->pvt->emulation = g_quark_to_string(quark);
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_MISC)) {
		fprintf(stderr, "Setting emulation to `%s'...", emulation);
	}
#endif
	/* Find and read the right termcap file. */
	vte_terminal_set_termcap(terminal, NULL, FALSE);

	/* Create a table to hold the control sequences. */
	if (terminal->pvt->matcher != NULL) {
		_vte_matcher_free(terminal->pvt->matcher);
	}
	terminal->pvt->matcher = _vte_matcher_new(emulation, terminal->pvt->termcap);

	/* Read emulation flags. */
	terminal->pvt->flags.am = _vte_termcap_find_boolean(terminal->pvt->termcap,
							    terminal->pvt->emulation,
							    "am");
	terminal->pvt->flags.bw = _vte_termcap_find_boolean(terminal->pvt->termcap,
							    terminal->pvt->emulation,
							    "bw");
	terminal->pvt->flags.LP = _vte_termcap_find_boolean(terminal->pvt->termcap,
							    terminal->pvt->emulation,
							    "LP");
	terminal->pvt->flags.ul = _vte_termcap_find_boolean(terminal->pvt->termcap,
							    terminal->pvt->emulation,
							    "ul");
	terminal->pvt->flags.xn = _vte_termcap_find_boolean(terminal->pvt->termcap,
							    terminal->pvt->emulation,
							    "xn");

	/* Resize to the given default. */
	columns = _vte_termcap_find_numeric(terminal->pvt->termcap,
					    terminal->pvt->emulation,
					    "co");
	rows = _vte_termcap_find_numeric(terminal->pvt->termcap,
					 terminal->pvt->emulation,
					 "li");
	terminal->pvt->default_column_count = columns;
	terminal->pvt->default_row_count = rows;

	/* Notify observers that we changed our emulation. */
	vte_terminal_emit_emulation_changed(terminal);
}

/**
 * vte_terminal_get_default_emulation:
 * @terminal: a #VteTerminal
 *
 * Queries the terminal for its default emulation, which is attempted if the
 * terminal type passed to vte_terminal_set_emulation() is NULL.
 *
 * Returns: the name of the default terminal type the widget attempts to emulate
 *
 * Since 0.11.11
 */
const char *
vte_terminal_get_default_emulation(VteTerminal *terminal)
{
	return VTE_DEFAULT_EMULATION;
}

/**
 * vte_terminal_get_emulation:
 * @terminal: a #VteTerminal
 *
 * Queries the terminal for its current emulation, as last set by a call to
 * vte_terminal_set_emulation().
 *
 * Returns: the name of the terminal type the widget is attempting to emulate
 */
const char *
vte_terminal_get_emulation(VteTerminal *terminal)
{
	g_return_val_if_fail(VTE_IS_TERMINAL(terminal), NULL);
	return terminal->pvt->emulation;
}

/* Set the path to the termcap file we read, and read it in. */
static void
vte_terminal_set_termcap(VteTerminal *terminal, const char *path,
			 gboolean reset)
{
	struct stat st;
	char *wpath;
	GQuark q = 0;

	if (path == NULL) {
		wpath = g_strdup_printf(DATADIR "/" PACKAGE "/termcap/%s",
					terminal->pvt->emulation ?
					terminal->pvt->emulation :
					vte_terminal_get_default_emulation(terminal));
		if (stat(wpath, &st) != 0) {
			g_free(wpath);
			wpath = g_strdup("/etc/termcap");
		}
		q = g_quark_from_string(wpath);
		g_free(wpath);
	} else {
		q = g_quark_from_string(path);
	}
	terminal->pvt->termcap_path = g_quark_to_string(q);

#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_MISC)) {
		fprintf(stderr, "Loading termcap `%s'...",
			terminal->pvt->termcap_path);
	}
#endif
	if (terminal->pvt->termcap) {
		_vte_termcap_free(terminal->pvt->termcap);
	}
	terminal->pvt->termcap = _vte_termcap_new(terminal->pvt->termcap_path);
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_MISC)) {
		fprintf(stderr, "\n");
	}
#endif
	if (reset) {
		vte_terminal_set_emulation(terminal, terminal->pvt->emulation);
	}
}

static void
vte_terminal_reset_rowdata(VteRing **ring, glong lines)
{
	VteRing *new_ring;
	VteRowData *row;
	long i, next;

#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_MISC)) {
		fprintf(stderr, "Sizing scrollback buffer to %ld lines.\n",
			lines);
	}
#endif
	if (*ring && (_vte_ring_max(*ring) == lines)) {
		return;
	}
	new_ring = _vte_ring_new_with_delta(lines,
					    *ring ? _vte_ring_delta(*ring) : 0,
					    vte_free_row_data,
					    NULL);
	if (*ring) {
		next = _vte_ring_next(*ring);
		for (i = _vte_ring_delta(*ring); i < next; i++) {
			row = _vte_ring_index(*ring, VteRowData *, i);
			_vte_ring_append(new_ring, row);
		}
		_vte_ring_free(*ring, FALSE);
	}
	*ring = new_ring;
}

/* Re-set the font because hinting settings changed. */
static void
vte_terminal_fc_settings_changed(GtkSettings *settings, GParamSpec *spec,
				 VteTerminal *terminal)
{
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_MISC)) {
		fprintf(stderr, "Fontconfig setting \"%s\" changed.\n",
			spec->name);
	}
#endif
	vte_terminal_set_font_full(terminal, terminal->pvt->fontdesc,
				   terminal->pvt->fontantialias);
}

/* Connect to notifications from our settings object that font hints have
 * changed. */
static void
vte_terminal_connect_xft_settings(VteTerminal *terminal)
{
	GtkSettings *settings;
	GObjectClass *klass;
	gpointer func;

	gtk_widget_ensure_style(GTK_WIDGET(terminal));
	settings = gtk_widget_get_settings(GTK_WIDGET(terminal));
	if (settings == NULL) {
		return;
	}

	/* Check that the properties we're looking at are defined. */
	klass = G_OBJECT_CLASS(GTK_SETTINGS_GET_CLASS(settings));
	if (g_object_class_find_property(klass, "gtk-xft-antialias") == NULL) {
		return;
	}

	/* If this is our first time in here, start listening for changes
	 * to the Xft settings. */
	if (terminal->pvt->connected_settings == NULL) {
		terminal->pvt->connected_settings = settings;
		func = (gpointer) vte_terminal_fc_settings_changed;
		g_signal_connect(G_OBJECT(settings),
				"notify::gtk-xft-antialias",
				G_CALLBACK(func), terminal);
		g_signal_connect(G_OBJECT(settings),
				"notify::gtk-xft-hinting",
				G_CALLBACK(func), terminal);
		g_signal_connect(G_OBJECT(settings),
				"notify::gtk-xft-hintstyle",
				G_CALLBACK(func), terminal);
		g_signal_connect(G_OBJECT(settings),
				"notify::gtk-xft-rgba",
				G_CALLBACK(func), terminal);
		g_signal_connect(G_OBJECT(settings),
				"notify::gtk-xft-dpi",
				G_CALLBACK(func), terminal);
	}
}

/* Disconnect from notifications from our settings object that font hints have
 * changed. */
static void
vte_terminal_disconnect_xft_settings(VteTerminal *terminal)
{
	GtkSettings *settings;
	gpointer func;

	if (terminal->pvt->connected_settings != NULL) {
		settings = terminal->pvt->connected_settings;
		func = (gpointer) vte_terminal_fc_settings_changed;
		g_signal_handlers_disconnect_by_func(G_OBJECT(settings),
						     func,
						     terminal);
		terminal->pvt->connected_settings = NULL;
	}
}

static void
_vte_terminal_codeset_changed_cb(struct _vte_iso2022_state *state, gpointer p)
{
	g_assert(VTE_IS_TERMINAL(p));
	vte_terminal_set_encoding(VTE_TERMINAL(p),
				  _vte_iso2022_state_get_codeset(state));
}

/* Initialize the terminal widget after the base widget stuff is initialized.
 * We need to create a new psuedo-terminal pair, read in the termcap file, and
 * set ourselves up to do the interpretation of sequences. */
static void
vte_terminal_init(VteTerminal *terminal, gpointer *klass)
{
	VteTerminalPrivate *pvt;
	GtkWidget *widget;
	GtkAdjustment *adjustment;
	struct timezone tz;
	struct timeval tv;

#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_LIFECYCLE)) {
		fprintf(stderr, "vte_terminal_init()\n");
	}
#endif

	g_assert(VTE_IS_TERMINAL(terminal));
	widget = GTK_WIDGET(terminal);
	GTK_WIDGET_SET_FLAGS(widget, GTK_CAN_FOCUS);

	/* We do our own redrawing. */
	gtk_widget_set_redraw_on_allocate (widget, FALSE);

	/* Set an adjustment for the application to use to control scrolling. */
	adjustment = GTK_ADJUSTMENT(gtk_adjustment_new(0, 0, 0, 0, 0, 0));
	vte_terminal_set_scroll_adjustment(terminal, adjustment);

	/* Initialize the default titles. */
	terminal->window_title = NULL;
	terminal->icon_title = NULL;

	/* Set up dummy metrics. */
	terminal->char_width = 0;
	terminal->char_height = 0;
	terminal->char_ascent = 0;
	terminal->char_descent = 0;

	/* Initialize private data. */
	pvt = terminal->pvt = G_TYPE_INSTANCE_GET_PRIVATE (terminal, VTE_TYPE_TERMINAL, VteTerminalPrivate);

	/* We allocated zeroed memory, just fill in non-zero stuff. */

	/* Load the termcap data and set up the emulation. */
	memset(&pvt->flags, 0, sizeof(pvt->flags));
	pvt->keypad_mode = VTE_KEYMODE_NORMAL;
	pvt->cursor_mode = VTE_KEYMODE_NORMAL;
	pvt->dec_saved = g_hash_table_new(g_direct_hash, g_direct_equal);
	pvt->default_column_count = 80;
	pvt->default_row_count = 24;

	/* Setting the terminal type and size requires the PTY master to
	 * be set up properly first. */
	pvt->pty_master = -1;
	vte_terminal_set_emulation(terminal, NULL);
	vte_terminal_set_size(terminal,
			      pvt->default_column_count,
			      pvt->default_row_count);
	pvt->pty_master = -1;
	pvt->pty_input_source = VTE_INVALID_SOURCE;
	pvt->pty_output_source = VTE_INVALID_SOURCE;
	pvt->pty_pid = -1;

	/* Set up I/O encodings. */
	pvt->iso2022 = _vte_iso2022_state_new(pvt->encoding,
					      &_vte_terminal_codeset_changed_cb,
					      (gpointer)terminal);
	pvt->incoming = _vte_buffer_new();
	pvt->pending = g_array_new(TRUE, TRUE, sizeof(gunichar));
	pvt->coalesce_timeout = VTE_INVALID_SOURCE;
	pvt->display_timeout = VTE_INVALID_SOURCE;
	pvt->outgoing = _vte_buffer_new();
	pvt->outgoing_conv = (VteConv) -1;
	pvt->conv_buffer = _vte_buffer_new();
	vte_terminal_set_encoding(terminal, NULL);
	g_assert(terminal->pvt->encoding != NULL);

	/* Initialize the screens and histories. */
	vte_terminal_reset_rowdata(&pvt->alternate_screen.row_data,
				   pvt->scrollback_lines);
	pvt->alternate_screen.sendrecv_mode = TRUE;
	pvt->alternate_screen.status_line_contents = g_string_new(NULL);
	pvt->screen = &terminal->pvt->alternate_screen;
	_vte_terminal_set_default_attributes(terminal);

	vte_terminal_reset_rowdata(&pvt->normal_screen.row_data,
				   pvt->scrollback_lines);
	pvt->normal_screen.sendrecv_mode = TRUE;
	pvt->normal_screen.status_line_contents = g_string_new(NULL);
	pvt->screen = &terminal->pvt->normal_screen;
	_vte_terminal_set_default_attributes(terminal);

	/* Selection info. */
	vte_terminal_set_word_chars(terminal, NULL);

	/* Miscellaneous options. */
	vte_terminal_set_backspace_binding(terminal, VTE_ERASE_AUTO);
	vte_terminal_set_delete_binding(terminal, VTE_ERASE_AUTO);
	pvt->meta_sends_escape = TRUE;
	pvt->audible_bell = TRUE;
	pvt->bell_margin = 10;
	pvt->allow_bold = TRUE;
	pvt->nrc_mode = TRUE;
	vte_terminal_set_default_tabstops(terminal);

	/* Scrolling options. */
	pvt->scroll_on_keystroke = TRUE;
	pvt->scrollback_lines = VTE_SCROLLBACK_MIN;
	vte_terminal_set_scrollback_lines(terminal,
					  terminal->pvt->scrollback_lines);

	/* Cursor blinking. */
	pvt->cursor_visible = TRUE;
	pvt->cursor_blink_timeout = 1000;

	/* Input options. */
	if (gettimeofday(&tv, &tz) == 0) {
		pvt->last_keypress_time = (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
	} else {
		pvt->last_keypress_time = 0;
	}

	/* Matching data. */
	pvt->match_regexes = g_array_new(FALSE, TRUE,
					 sizeof(struct vte_match_regex));
	pvt->match_previous = -1;
	vte_terminal_match_hilite_clear(terminal);

	/* Rendering data.  Try everything. */
	pvt->draw = _vte_draw_new(GTK_WIDGET(terminal));

	/* The font description. */
	pvt->fontantialias = VTE_ANTI_ALIAS_USE_DEFAULT;
	gtk_widget_ensure_style(widget);
	vte_terminal_connect_xft_settings(terminal);
	vte_terminal_set_font_full(terminal, NULL, VTE_ANTI_ALIAS_USE_DEFAULT);

	/* Set up background information. */
	pvt->bg_update_tag = VTE_INVALID_SOURCE;
	pvt->bg_tint_color.red = 0;
	pvt->bg_tint_color.green = 0;
	pvt->bg_tint_color.blue = 0;
	pvt->bg_saturation = 0.4 * VTE_SATURATION_MAX;

	/* Assume we're visible unless we're told otherwise. */
	pvt->visibility_state = GDK_VISIBILITY_UNOBSCURED;

	/* Listen for hierarchy change notifications. */
	g_signal_connect(G_OBJECT(terminal), "hierarchy-changed",
			 G_CALLBACK(vte_terminal_hierarchy_changed),
			 NULL);

	/* Listen for style changes. */
	g_signal_connect(G_OBJECT(terminal), "style-set",
			 G_CALLBACK(vte_terminal_style_changed),
			 NULL);

#ifdef VTE_DEBUG
	/* In debuggable mode, we always do this. */
	/* gtk_widget_get_accessible(GTK_WIDGET(terminal)); */
#endif
}

/* Tell GTK+ how much space we need. */
static void
vte_terminal_size_request(GtkWidget *widget, GtkRequisition *requisition)
{
	VteTerminal *terminal;

#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_LIFECYCLE)) {
		fprintf(stderr, "vte_terminal_size_request()\n");
	}
#endif

	g_assert(widget != NULL);
	g_assert(VTE_IS_TERMINAL(widget));
	terminal = VTE_TERMINAL(widget);

	if (terminal->pvt->pty_master != -1) {
		vte_terminal_refresh_size(terminal);
		requisition->width = terminal->char_width *
				     terminal->column_count;
		requisition->height = terminal->char_height *
				      terminal->row_count;
	} else {
		requisition->width = terminal->char_width *
				     terminal->pvt->default_column_count;
		requisition->height = terminal->char_height *
				      terminal->pvt->default_row_count;
	}

	requisition->width += VTE_PAD_WIDTH * 2;
	requisition->height += VTE_PAD_WIDTH * 2;

#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_MISC)) {
		fprintf(stderr, "Size request is %dx%d.\n",
			requisition->width, requisition->height);
	}
#endif
}

/* Accept a given size from GTK+. */
static void
vte_terminal_size_allocate(GtkWidget *widget, GtkAllocation *allocation)
{
	VteTerminal *terminal;
	glong width, height;
	gint x, y, w, h;
	gboolean snapped_to_bottom;

#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_LIFECYCLE)) {
		fprintf(stderr, "vte_terminal_size_allocate()\n");
	}
#endif

	g_assert(widget != NULL);
	g_assert(VTE_IS_TERMINAL(widget));

	terminal = VTE_TERMINAL(widget);

	snapped_to_bottom = (terminal->pvt->screen->insert_delta ==
			     terminal->pvt->screen->scroll_delta);

	width = (allocation->width - (2 * VTE_PAD_WIDTH)) /
		terminal->char_width;
	height = (allocation->height - (2 * VTE_PAD_WIDTH)) /
		 terminal->char_height;

#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_MISC)) {
		fprintf(stderr, "Sizing window to %dx%d (%ldx%ld).\n",
			allocation->width, allocation->height,
			width, height);
	}
#endif

	/* Set our allocation to match the structure. */
	widget->allocation = *allocation;

	/* Set the size of the pseudo-terminal. */
	vte_terminal_set_size(terminal, width, height);

	/* Adjust scrolling area in case our boundaries have just been
	 * redefined to be invalid. */
	if (terminal->pvt->screen->scrolling_restricted) {
		terminal->pvt->screen->scrolling_region.start =
			CLAMP(terminal->pvt->screen->scrolling_region.start,
			      terminal->pvt->screen->insert_delta,
			      terminal->pvt->screen->insert_delta +
			      terminal->row_count - 1);
		terminal->pvt->screen->scrolling_region.end =
			CLAMP(terminal->pvt->screen->scrolling_region.end,
			      terminal->pvt->screen->insert_delta,
			      terminal->pvt->screen->insert_delta +
			      terminal->row_count - 1);
	}

	/* Adjust scrollback buffers to ensure that they're big enough. */
	vte_terminal_set_scrollback_lines(terminal,
					  MAX(terminal->pvt->scrollback_lines,
					      terminal->row_count));

	/* Resize the GDK window. */
	if (widget->window != NULL) {
		gdk_window_get_geometry(widget->window,
					&x, &y, &w, &h, NULL);
		gdk_window_move_resize(widget->window,
				       allocation->x,
				       allocation->y,
				       allocation->width,
				       allocation->height);
		/* Repaint if we were resized or moved. */
		if ((x != allocation->x) ||
		    (y != allocation->y) ||
		    (w != allocation->width) ||
		    (h != allocation->height)) {
			_vte_invalidate_all(terminal);
		}
	}

	/* Adjust the adjustments. */
	_vte_terminal_adjust_adjustments(terminal, TRUE);

	_vte_terminal_update_insert_delta (terminal);

	if (snapped_to_bottom) {
		vte_terminal_maybe_scroll_to_bottom (terminal);
	}
}

/* Show the window. */
static void
vte_terminal_show(GtkWidget *widget)
{
	GtkWidgetClass *widget_class;
	VteTerminal *terminal;

#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_LIFECYCLE)) {
		fprintf(stderr, "vte_terminal_show()\n");
	}
#endif

	g_assert(widget != NULL);
	g_assert(VTE_IS_TERMINAL(widget));
	terminal = VTE_TERMINAL(widget);

	widget_class = g_type_class_peek(GTK_TYPE_WIDGET);
	if (GTK_WIDGET_CLASS(widget_class)->show) {
		(GTK_WIDGET_CLASS(widget_class))->show(widget);
	}
}

/* Queue a background update. */
static void
root_pixmap_changed_cb(VteBg *bg, gpointer data)
{
	VteTerminal *terminal;
	if (VTE_IS_TERMINAL(data)) {
		terminal = VTE_TERMINAL(data);
		if (terminal->pvt->bg_transparent) {
			vte_terminal_queue_background_update(terminal);
		}
	}
}

/* The window is being destroyed. */
static void
vte_terminal_unrealize(GtkWidget *widget)
{
	VteTerminal *terminal;

#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_LIFECYCLE)) {
		fprintf(stderr, "vte_terminal_unrealize()\n");
	}
#endif

	g_assert(widget != NULL);
	g_assert(VTE_IS_TERMINAL(widget));
	terminal = VTE_TERMINAL(widget);

	/* Clean up our draw structure. */
	if (terminal->pvt->draw != NULL) {
		_vte_draw_free(terminal->pvt->draw);
	}
	terminal->pvt->draw = NULL;

	/* Disconnect from background-change events. */
	g_signal_handlers_disconnect_by_func(G_OBJECT(vte_bg_get()),
					     root_pixmap_changed_cb,
					     widget);

	/* Deallocate the cursors. */
	terminal->pvt->mouse_cursor_visible = FALSE;
	gdk_cursor_unref(terminal->pvt->mouse_default_cursor);
	terminal->pvt->mouse_default_cursor = NULL;
	gdk_cursor_unref(terminal->pvt->mouse_mousing_cursor);
	terminal->pvt->mouse_mousing_cursor = NULL;
	gdk_cursor_unref(terminal->pvt->mouse_inviso_cursor);
	terminal->pvt->mouse_inviso_cursor = NULL;

	/* Shut down input methods. */
	if (terminal->pvt->im_context != NULL) {
	        g_signal_handlers_disconnect_by_func (G_OBJECT(terminal->pvt->im_context), 
						      vte_terminal_im_preedit_changed,
						      terminal);
		vte_terminal_im_reset(terminal);
		gtk_im_context_set_client_window(terminal->pvt->im_context,
						 NULL);
		g_object_unref(G_OBJECT(terminal->pvt->im_context));
		terminal->pvt->im_context = NULL;
	}
	terminal->pvt->im_preedit_active = FALSE;
	if (terminal->pvt->im_preedit != NULL) {
		g_free(terminal->pvt->im_preedit);
		terminal->pvt->im_preedit = NULL;
	}
	if (terminal->pvt->im_preedit_attrs != NULL) {
		pango_attr_list_unref(terminal->pvt->im_preedit_attrs);
		terminal->pvt->im_preedit_attrs = NULL;
	}
	terminal->pvt->im_preedit_cursor = 0;

	/* Unmap the widget if it hasn't been already. */
	if (GTK_WIDGET_MAPPED(widget)) {
	  
		gtk_widget_unmap(widget);
	}

	/* Remove the GDK window. */
	if (widget->window != NULL) {
	        gdk_window_set_user_data(widget->window, NULL);
		gdk_window_destroy(widget->window);
		widget->window = NULL;
	}

	/* Remove the blink timeout function. */
	if (terminal->pvt->cursor_blink_tag != 0) {
		g_source_remove(terminal->pvt->cursor_blink_tag);
		terminal->pvt->cursor_blink_tag = 0;
	}
	terminal->pvt->cursor_force_fg = 0;

	/* Cancel any pending background updates. */
	if (terminal->pvt->bg_update_tag != VTE_INVALID_SOURCE) {
		g_source_remove(terminal->pvt->bg_update_tag);
		terminal->pvt->bg_update_tag = VTE_INVALID_SOURCE;
		terminal->pvt->bg_update_pending = FALSE;
	}

	/* Clear modifiers. */
	terminal->pvt->modifiers = 0;

	/* Mark that we no longer have a GDK window. */
	GTK_WIDGET_UNSET_FLAGS(widget, GTK_REALIZED);
}

/* Perform final cleanups for the widget before it's freed. */
static void
vte_terminal_finalize(GObject *object)
{
	VteTerminal *terminal;
	GtkWidget *toplevel;
	GObjectClass *object_class;
	GtkWidgetClass *widget_class;
	GtkClipboard *clipboard;
	struct vte_match_regex *regex;
	int i;

#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_LIFECYCLE)) {
		fprintf(stderr, "vte_terminal_finalize()\n");
	}
#endif

	g_assert(VTE_IS_TERMINAL(object));
	terminal = VTE_TERMINAL(object);
	object_class = G_OBJECT_GET_CLASS(G_OBJECT(object));
	widget_class = g_type_class_peek(GTK_TYPE_WIDGET);

	/* Free the draw structure. */
	if (terminal->pvt->draw != NULL) {
		_vte_draw_free(terminal->pvt->draw);
	}
	terminal->pvt->draw = NULL;

	/* The NLS maps. */
	if (terminal->pvt->iso2022 != NULL) {
		_vte_iso2022_state_free(terminal->pvt->iso2022);
		terminal->pvt->iso2022 = NULL;
	}

	/* Free background info. */
	if (terminal->pvt->bg_update_tag != VTE_INVALID_SOURCE) {
		g_source_remove(terminal->pvt->bg_update_tag);
		terminal->pvt->bg_update_tag = VTE_INVALID_SOURCE;
		terminal->pvt->bg_update_pending = FALSE;
		g_free(terminal->pvt->bg_file);
	}

	/* Free the font description. */
	if (terminal->pvt->fontdesc != NULL) {
		pango_font_description_free(terminal->pvt->fontdesc);
		terminal->pvt->fontdesc = NULL;
	}
	terminal->pvt->fontantialias = VTE_ANTI_ALIAS_USE_DEFAULT;
	vte_terminal_disconnect_xft_settings(terminal);

	/* Free matching data. */
	if (terminal->pvt->match_attributes != NULL) {
		g_array_free(terminal->pvt->match_attributes, TRUE);
		terminal->pvt->match_attributes = NULL;
	}
	if (terminal->pvt->match_contents != NULL) {
		g_free(terminal->pvt->match_contents);
		terminal->pvt->match_contents = NULL;
	}
	if (terminal->pvt->match_regexes != NULL) {
		for (i = 0; i < terminal->pvt->match_regexes->len; i++) {
			regex = &g_array_index(terminal->pvt->match_regexes,
					       struct vte_match_regex,
					       i);
			/* Skip holes. */
			if (regex->tag < 0) {
				continue;
			}
			if (regex->cursor != NULL) {
				gdk_cursor_unref(regex->cursor);
				regex->cursor = NULL;
			}
			_vte_regex_free(regex->reg);
			regex->reg = NULL;
			regex->tag = 0;
		}
		g_array_free(terminal->pvt->match_regexes, TRUE);
		terminal->pvt->match_regexes = NULL;
		terminal->pvt->match_previous = -1;
	}

	/* Disconnect from toplevel window configure events. */
	toplevel = gtk_widget_get_toplevel(GTK_WIDGET(object));
	if ((toplevel != NULL) && (G_OBJECT(toplevel) != G_OBJECT(object))) {
		g_signal_handlers_disconnect_by_func(toplevel,
						     (gpointer)vte_terminal_configure_toplevel,
						     terminal);
	}

	/* Disconnect from autoscroll requests. */
	vte_terminal_stop_autoscroll(terminal);

	/* Cancel pending adjustment change notifications. */
	if (terminal->pvt->adjustment_changed_tag) {
		g_source_remove(terminal->pvt->adjustment_changed_tag);
		terminal->pvt->adjustment_changed_tag = 0;
	}

	/* Tabstop information. */
	if (terminal->pvt->tabstops != NULL) {
		g_hash_table_destroy(terminal->pvt->tabstops);
		terminal->pvt->tabstops = NULL;
	}
	terminal->pvt->text_modified_flag = FALSE;
	terminal->pvt->text_inserted_count = 0;
	terminal->pvt->text_deleted_count = 0;

	/* Free any selected text, but if we currently own the selection,
	 * throw the text onto the clipboard without an owner so that it
	 * doesn't just disappear. */
	if (terminal->pvt->selection != NULL) {
		clipboard = vte_terminal_clipboard_get(terminal,
						       GDK_SELECTION_PRIMARY);
		if (gtk_clipboard_get_owner(clipboard) == G_OBJECT(terminal)) {
			gtk_clipboard_set_text(clipboard,
					       terminal->pvt->selection,
					       -1);
		}
		g_free(terminal->pvt->selection);
		terminal->pvt->selection = NULL;
	}
	if (terminal->pvt->word_chars != NULL) {
		g_array_free(terminal->pvt->word_chars, TRUE);
		terminal->pvt->word_chars = NULL;
	}

	/* Clear the output histories. */
	_vte_ring_free(terminal->pvt->normal_screen.row_data, TRUE);
	terminal->pvt->normal_screen.row_data = NULL;
	_vte_ring_free(terminal->pvt->alternate_screen.row_data, TRUE);
	terminal->pvt->alternate_screen.row_data = NULL;

	/* Clear the status lines. */
	terminal->pvt->normal_screen.status_line = FALSE;
	g_string_free(terminal->pvt->normal_screen.status_line_contents,
		      TRUE);
	terminal->pvt->alternate_screen.status_line = FALSE;
	g_string_free(terminal->pvt->alternate_screen.status_line_contents,
		      TRUE);

	/* Free conversion descriptors. */
	if (terminal->pvt->outgoing_conv != ((VteConv) -1)) {
		_vte_conv_close(terminal->pvt->outgoing_conv);
	}
	terminal->pvt->outgoing_conv = ((VteConv) -1);

	/* Stop listening for child-exited signals. */
	if (VTE_IS_REAPER(terminal->pvt->pty_reaper)) {
		g_signal_handlers_disconnect_by_func(terminal->pvt->pty_reaper,
						     (gpointer)vte_terminal_catch_child_exited,
						     terminal);
		g_object_unref(G_OBJECT(terminal->pvt->pty_reaper));
	}
	terminal->pvt->pty_reaper = NULL;

	/* Stop processing input. */
	vte_terminal_stop_processing (terminal);

	/* Discard any pending data. */
	if (terminal->pvt->incoming != NULL) {
		_vte_buffer_free(terminal->pvt->incoming);
	}
	terminal->pvt->incoming = NULL;
	if (terminal->pvt->outgoing != NULL) {
		_vte_buffer_free(terminal->pvt->outgoing);
	}
	terminal->pvt->outgoing = NULL;
	if (terminal->pvt->pending != NULL) {
		g_array_free(terminal->pvt->pending, TRUE);
	}
	terminal->pvt->pending = NULL;
	if (terminal->pvt->conv_buffer != NULL) {
		_vte_buffer_free(terminal->pvt->conv_buffer);
	}
	terminal->pvt->conv_buffer = NULL;

	/* Stop the child and stop watching for input from the child. */
	if (terminal->pvt->pty_pid != -1) {
#ifdef HAVE_GETPGID
		pid_t pgrp;
		pgrp = getpgid(terminal->pvt->pty_pid);
		if (pgrp != -1) {
			kill(-pgrp, SIGHUP);
		}
#endif
		kill(terminal->pvt->pty_pid, SIGHUP);
	}
	terminal->pvt->pty_pid = -1;
	_vte_terminal_disconnect_pty_read(terminal);
	_vte_terminal_disconnect_pty_write(terminal);
	if (terminal->pvt->pty_master != -1) {
		_vte_pty_close(terminal->pvt->pty_master);
		close(terminal->pvt->pty_master);
	}
	terminal->pvt->pty_master = -1;

	/* Clear some of our strings. */
	terminal->pvt->shell = NULL;

	/* Remove hash tables. */
	if (terminal->pvt->dec_saved != NULL) {
		g_hash_table_destroy(terminal->pvt->dec_saved);
		terminal->pvt->dec_saved = NULL;
	}

	/* Clean up emulation structures. */
	memset(&terminal->pvt->flags, 0, sizeof(terminal->pvt->flags));
	terminal->pvt->emulation = NULL;
	terminal->pvt->termcap_path = NULL;
	if (terminal->pvt->matcher != NULL) {
		_vte_matcher_free(terminal->pvt->matcher);
		terminal->pvt->matcher = NULL;
	}
	_vte_termcap_free(terminal->pvt->termcap);
	terminal->pvt->termcap = NULL;

	if (terminal->pvt->update_timer) {
		vte_free_update_timer (terminal);
	}

	/* Done with our private data. */
	terminal->pvt = NULL;

	/* Free public-facing data. */
	if (terminal->window_title != NULL) {
		g_free(terminal->window_title);
		terminal->window_title = NULL;
	}
	if (terminal->icon_title != NULL) {
		g_free(terminal->icon_title);
		terminal->icon_title = NULL;
	}
	if (terminal->adjustment != NULL) {
		g_object_unref(terminal->adjustment);
		terminal->adjustment = NULL;
	}

	/* Call the inherited finalize() method. */
	if (G_OBJECT_CLASS(widget_class)->finalize) {
		(G_OBJECT_CLASS(widget_class))->finalize(object);
	}
}

/* Handle realizing the widget.  Most of this is copy-paste from GGAD. */
static void
vte_terminal_realize(GtkWidget *widget)
{
	VteTerminal *terminal = NULL;
	GdkWindowAttr attributes;
	GdkPixmap *bitmap;
	GdkColor black = {0,0,0}, color;
	GtkSettings *settings;
	int attributes_mask = 0, i;
	gint blink_cycle = 1000;

#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_LIFECYCLE)) {
		fprintf(stderr, "vte_terminal_realize()\n");
	}
#endif

	g_assert(widget != NULL);
	g_assert(VTE_IS_TERMINAL(widget));
	terminal = VTE_TERMINAL(widget);

	/* Create the draw structure if we don't already have one. */
	if (terminal->pvt->draw != NULL) {
		_vte_draw_free(terminal->pvt->draw);
	}
	terminal->pvt->draw = _vte_draw_new(GTK_WIDGET(terminal));

	/* Create the stock cursors. */
	terminal->pvt->mouse_cursor_visible = TRUE;
	terminal->pvt->mouse_default_cursor =
		vte_terminal_cursor_new(terminal, VTE_DEFAULT_CURSOR);
	terminal->pvt->mouse_mousing_cursor =
		vte_terminal_cursor_new(terminal, VTE_MOUSING_CURSOR);

	/* Create a GDK window for the widget. */
	attributes.window_type = GDK_WINDOW_CHILD;
	attributes.x = 0;
	attributes.y = 0;
	attributes.width = widget->allocation.width;
	attributes.height = widget->allocation.height;
	attributes.wclass = GDK_INPUT_OUTPUT;
	attributes.visual = _vte_draw_get_visual(terminal->pvt->draw);
	attributes.colormap = _vte_draw_get_colormap(terminal->pvt->draw,
						     FALSE);
	attributes.event_mask = gtk_widget_get_events(widget) |
				GDK_EXPOSURE_MASK |
				GDK_VISIBILITY_NOTIFY_MASK |
				GDK_FOCUS_CHANGE_MASK |
				GDK_BUTTON_PRESS_MASK |
				GDK_BUTTON_RELEASE_MASK |
				GDK_POINTER_MOTION_MASK |
				GDK_BUTTON1_MOTION_MASK |
				GDK_KEY_PRESS_MASK |
				GDK_KEY_RELEASE_MASK;
	attributes.cursor = terminal->pvt->mouse_default_cursor;
	attributes_mask = GDK_WA_X |
			  GDK_WA_Y |
			  (attributes.visual ? GDK_WA_VISUAL : 0) |
			  (attributes.colormap ? GDK_WA_COLORMAP : 0) |
			  GDK_WA_CURSOR;
	widget->window = gdk_window_new(gtk_widget_get_parent_window(widget),
					&attributes,
					attributes_mask);
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_UPDATES)) {
		gdk_window_set_debug_updates(TRUE);
	}
#endif
	gdk_window_move_resize(widget->window,
			       widget->allocation.x,
			       widget->allocation.y,
			       widget->allocation.width,
			       widget->allocation.height);
	gdk_window_set_user_data(widget->window, widget);
	gdk_window_show(widget->window);

	/* Set up the desired palette. */
	if (!terminal->pvt->palette_initialized) {
		vte_terminal_set_default_colors(terminal);
	}

	/* Set the realized flag. */
	GTK_WIDGET_SET_FLAGS(widget, GTK_REALIZED);

	/* Actually load the font. */
	vte_terminal_set_font_full(terminal, terminal->pvt->fontdesc,
				   terminal->pvt->fontantialias);

	/* Allocate colors. */
	for (i = 0; i < G_N_ELEMENTS(terminal->pvt->palette); i++) {
		color.red = terminal->pvt->palette[i].red;
		color.green = terminal->pvt->palette[i].green;
		color.blue = terminal->pvt->palette[i].blue;
		color.pixel = 0;
		vte_terminal_set_color_internal(terminal, i, &color);
	}

	/* Setup cursor blink */
	settings = gtk_widget_get_settings(GTK_WIDGET(terminal));
	if (G_IS_OBJECT(settings)) {
		g_object_get(G_OBJECT(settings), "gtk-cursor-blink-time",
			     &blink_cycle, NULL);
	}
	terminal->pvt->cursor_blink_tag = g_timeout_add_full(G_PRIORITY_LOW,
							     blink_cycle / 2,
							     vte_invalidate_cursor_periodic,
							     terminal,
							     NULL);

	/* Set up input method support.  FIXME: do we need to handle the
	 * "retrieve-surrounding" and "delete-surrounding" events? */
	if (terminal->pvt->im_context != NULL) {
		vte_terminal_im_reset(terminal);
		g_object_unref(G_OBJECT(terminal->pvt->im_context));
		terminal->pvt->im_context = NULL;
	}
	terminal->pvt->im_preedit_active = FALSE;
	terminal->pvt->im_context = gtk_im_multicontext_new();
	gtk_im_context_set_client_window(terminal->pvt->im_context,
					 widget->window);
	g_signal_connect(G_OBJECT(terminal->pvt->im_context), "commit",
			 GTK_SIGNAL_FUNC(vte_terminal_im_commit), terminal);
	g_signal_connect(G_OBJECT(terminal->pvt->im_context), "preedit-start",
			 GTK_SIGNAL_FUNC(vte_terminal_im_preedit_start),
			 terminal);
	g_signal_connect(G_OBJECT(terminal->pvt->im_context), "preedit-changed",
			 GTK_SIGNAL_FUNC(vte_terminal_im_preedit_changed),
			 terminal);
	g_signal_connect(G_OBJECT(terminal->pvt->im_context), "preedit-end",
			 GTK_SIGNAL_FUNC(vte_terminal_im_preedit_end),
			 terminal);
	gtk_im_context_set_use_preedit(terminal->pvt->im_context, TRUE);

	/* Clear modifiers. */
	terminal->pvt->modifiers = 0;

	/* Assume we're visible unless we're told otherwise. */
	terminal->pvt->visibility_state = GDK_VISIBILITY_UNOBSCURED;

	/* Create our invisible cursor. */
	bitmap = gdk_bitmap_create_from_data(widget->window, "\0", 1, 1);
	terminal->pvt->mouse_inviso_cursor = gdk_cursor_new_from_pixmap(bitmap,
									bitmap,
									&black,
									&black,
									0, 0);

	/* Connect to background-change events. */
	g_signal_connect(G_OBJECT(vte_bg_get()),
			 "root-pixmap-changed",
			 G_CALLBACK(root_pixmap_changed_cb),
			 terminal);

	/* Set up the background, *now*. */
	vte_terminal_background_update(terminal);

	g_object_unref(G_OBJECT(bitmap));
}

static void
vte_terminal_determine_colors(VteTerminal *terminal,
			      const struct vte_charcell *cell,
			      gboolean reverse,
			      gboolean highlight,
			      gboolean cursor,
			      int *fore, int *back)
{
	g_assert(fore != NULL);
	g_assert(back != NULL);

	/* Determine what the foreground and background colors for rendering
	 * text should be.  If highlight is set and we have a highlight color,
	 * use that scheme.  If cursor is set and we have a cursor color, use
	 * that scheme.  If neither is set, and reverse is set, then use
	 * reverse colors, else use the defaults.  This means that many callers
	 * who specify highlight or cursor should also specify reverse. */
	if (cursor && !highlight && terminal->pvt->cursor_color_set) {
		*fore = cell ? cell->back : VTE_DEF_BG;
		*back = VTE_CUR_BG;
	} else
	if (highlight && !cursor && terminal->pvt->highlight_color_set) {
		*fore = cell ? cell->fore : VTE_DEF_FG;
		*back = VTE_DEF_HL;
	} else
	if (reverse ^ ((cell != NULL) && (cell->reverse))) {
		*fore = cell ? cell->back : VTE_DEF_BG;
		*back = cell ? cell->fore : VTE_DEF_FG;
	} else {
		*fore = cell ? cell->fore : VTE_DEF_FG;
		*back = cell ? cell->back : VTE_DEF_BG;
	}

	/* Handle invisible, bold, and standout text by adjusting colors. */
	if (cell && cell->invisible) {
		*fore = *back;
	}
	if (cell && cell->bold) {
		if (*fore == VTE_DEF_FG) {
			*fore = VTE_BOLD_FG;
		} else
		if ((*fore != VTE_DEF_BG) && (*fore < VTE_COLOR_SET_SIZE)) {
			*fore += VTE_COLOR_BRIGHT_OFFSET;
		}
	}
	if (cell && cell->half) {
		if (*fore == VTE_DEF_FG) {
			*fore = VTE_DIM_FG;
		} else
		if ((*fore < VTE_COLOR_SET_SIZE)) {
			*fore += VTE_COLOR_DIM_OFFSET;
		}
	}
	if (cell && cell->standout) {
		if (*back < VTE_COLOR_SET_SIZE) {
			*back += VTE_COLOR_BRIGHT_OFFSET;
		}
	}
}

/* Check if a unicode character is actually a graphic character we draw
 * ourselves to handle cases where fonts don't have glyphs for them. */
static gboolean
vte_unichar_is_local_graphic(gunichar c)
{
	if ((c >= 0x2500) && (c <= 0x257f)) {
		return TRUE;
	}
	switch (c) {
	case 0x00a3: /* british pound */
	case 0x00b0: /* degree */
	case 0x00b1: /* plus/minus */
	case 0x00b7: /* bullet */
	case 0x03c0: /* pi */
	case 0x2190: /* left arrow */
	case 0x2191: /* up arrow */
	case 0x2192: /* right arrow */
	case 0x2193: /* down arrow */
	case 0x2260: /* != */
	case 0x2264: /* <= */
	case 0x2265: /* >= */
	case 0x23ba: /* scanline 1/9 */
	case 0x23bb: /* scanline 3/9 */
	case 0x23bc: /* scanline 7/9 */
	case 0x23bd: /* scanline 9/9 */
	case 0x2409: /* HT symbol */
	case 0x240a: /* LF symbol */
	case 0x240b: /* VT symbol */
	case 0x240c: /* FF symbol */
	case 0x240d: /* CR symbol */
	case 0x2424: /* NL symbol */
	case 0x2592: /* checkerboard */
	case 0x25ae: /* solid rectangle */
	case 0x25c6: /* diamond */
		return TRUE;
		break;
	default:
		break;
	}
	return FALSE;
}

static void
vte_terminal_fill_rectangle_int(VteTerminal *terminal,
				struct vte_palette_entry *entry,
				gint x,
				gint y,
				gint width,
				gint height)
{
	GdkColor color;
	gboolean wasdrawing;

	wasdrawing = terminal->pvt->draw->started;
	if (!wasdrawing) {
		_vte_draw_start(terminal->pvt->draw);
	}
	color.red = entry->red;
	color.green = entry->green;
	color.blue = entry->blue;
	_vte_draw_fill_rectangle(terminal->pvt->draw,
				 x + VTE_PAD_WIDTH, y + VTE_PAD_WIDTH,
				 width, height,
				 &color, VTE_DRAW_OPAQUE);
	if (!wasdrawing) {
		_vte_draw_end(terminal->pvt->draw);
	}
}

static void
vte_terminal_fill_rectangle(VteTerminal *terminal,
			    struct vte_palette_entry *entry,
			    gint x,
			    gint y,
			    gint width,
			    gint height)
{
	vte_terminal_fill_rectangle_int(terminal, entry, x, y, width, height);
}

static void
vte_terminal_draw_line(VteTerminal *terminal,
		       struct vte_palette_entry *entry,
		       gint x,
		       gint y,
		       gint xp,
		       gint yp)
{
	vte_terminal_fill_rectangle(terminal, entry,
				    x, y,
				    MAX(1, xp - x + 1), MAX(1, yp - y + 1));
}

static void
vte_terminal_draw_rectangle(VteTerminal *terminal,
			    struct vte_palette_entry *entry,
			    gint x,
			    gint y,
			    gint width,
			    gint height)
{
	vte_terminal_draw_line(terminal, entry,
			       x, y, x, y + height);
	vte_terminal_draw_line(terminal, entry,
			       x + width, y, x + width, y + height);
	vte_terminal_draw_line(terminal, entry,
			       x, y, x + width, y);
	vte_terminal_draw_line(terminal, entry,
			       x, y + height, x + width, y + height);
}

static void
vte_terminal_draw_point(VteTerminal *terminal,
			struct vte_palette_entry *entry,
			gint x,
			gint y)
{
	vte_terminal_fill_rectangle(terminal, entry, x, y, 1, 1);
}

/* Draw the graphic representation of a line-drawing or special graphics
 * character. */
static gboolean
vte_terminal_draw_graphic(VteTerminal *terminal, gunichar c,
			  gint fore, gint back, gboolean draw_default_bg,
			  gint x, gint y,
			  gint column_width, gint columns, gint row_height)
{
	gboolean ret;
	gint xcenter, xright, ycenter, ybottom, i, j, draw;
	struct _vte_draw_text_request request;
	GdkColor color;

	request.c = c;
	request.x = x;
	request.y = y;
	request.columns = columns;

	color.red = terminal->pvt->palette[fore].red;
	color.green = terminal->pvt->palette[fore].green;
	color.blue = terminal->pvt->palette[fore].blue;
	xright = x + column_width * columns;
	ybottom = y + row_height;
	xcenter = (x + xright) / 2;
	ycenter = (y + ybottom) / 2;

	if ((back != VTE_DEF_BG) || draw_default_bg) {
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[back],
					    x, y,
					    column_width * columns, row_height);
	}

	ret = TRUE;

	switch (c) {
	case 124:
		if (_vte_draw_char(terminal->pvt->draw, &request,
				   &color, VTE_DRAW_OPAQUE)) {
			/* We were able to draw with actual fonts. */
			return TRUE;
		}
		xcenter--;
		ycenter--;
		xright--;
		ybottom--;
		/* != */
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       (x + xcenter) / 2 - 1, ycenter,
				       (xright + xcenter) / 2 + 1, ycenter);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       (x + xcenter) / 2 - 1,
				       (ybottom + ycenter) / 2,
				       (xright + xcenter) / 2 + 1,
				       (ybottom + ycenter) / 2);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       xright - 1, y + 1,
				       x + 1, ybottom - 1);
		break;
	case 127:
		if (_vte_draw_char(terminal->pvt->draw, &request,
				   &color, VTE_DRAW_OPAQUE)) {
			/* We were able to draw with actual fonts. */
			return TRUE;
		}
		xcenter--;
		ycenter--;
		xright--;
		ybottom--;
		/* A "delete" symbol I saw somewhere. */
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       x, ycenter,
				       xcenter, y);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       xcenter, y,
				       xright - 1, ycenter);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       xright - 1, ycenter,
				       xright - 1, ybottom - 1);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       xright - 1, ybottom - 1,
				       x, ybottom - 1);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       x, ybottom - 1,
				       x, ycenter);
		break;
	case 0x00a3:
		if (_vte_draw_char(terminal->pvt->draw, &request,
				   &color, VTE_DRAW_OPAQUE)) {
			/* We were able to draw with actual fonts. */
			return TRUE;
		}
		xcenter--;
		ycenter--;
		xright--;
		ybottom--;
		/* British pound.  An "L" with a hyphen. */
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       (x + xcenter) / 2,
				       (y + ycenter) / 2,
				       (x + xcenter) / 2,
				       (ycenter + ybottom) / 2);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       (x + xcenter) / 2,
				       (ycenter + ybottom) / 2,
				       (xcenter + xright) / 2,
				       (ycenter + ybottom) / 2);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       x, ycenter,
				       xcenter + 1, ycenter);
		break;
	case 0x00b0: /* f */
		if (_vte_draw_char(terminal->pvt->draw, &request,
				   &color, VTE_DRAW_OPAQUE)) {
			/* We were able to draw with actual fonts. */
			return TRUE;
		}
		/* litle circle */
		vte_terminal_draw_point(terminal,
					&terminal->pvt->palette[fore],
					xcenter - 1, ycenter);
		vte_terminal_draw_point(terminal,
					&terminal->pvt->palette[fore],
					xcenter + 1, ycenter);
		vte_terminal_draw_point(terminal,
					&terminal->pvt->palette[fore],
					xcenter, ycenter - 1);
		vte_terminal_draw_point(terminal,
					&terminal->pvt->palette[fore],
					xcenter, ycenter + 1);
		break;
	case 0x00b1: /* g */
		if (_vte_draw_char(terminal->pvt->draw, &request,
				   &color, VTE_DRAW_OPAQUE)) {
			/* We were able to draw with actual fonts. */
			return TRUE;
		}
		xcenter--;
		ycenter--;
		xright--;
		ybottom--;
		/* +/- */
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       xcenter,
				       (y + ycenter) / 2,
				       xcenter,
				       (ycenter + ybottom) / 2);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       (x + xcenter) / 2,
				       ycenter,
				       (xcenter + xright) / 2,
				       ycenter);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       (x + xcenter) / 2,
				       (ycenter + ybottom) / 2,
				       (xcenter + xright) / 2,
				       (ycenter + ybottom) / 2);
		break;
	case 0x00b7:
		if (_vte_draw_char(terminal->pvt->draw, &request,
				   &color, VTE_DRAW_OPAQUE)) {
			/* We were able to draw with actual fonts. */
			return TRUE;
		}
		xcenter--;
		ycenter--;
		xright--;
		ybottom--;
		/* short hyphen? */
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       xcenter - 1, ycenter,
				       xcenter + 1, ycenter);
		break;
	case 0x3c0: /* pi */
		if (_vte_draw_char(terminal->pvt->draw, &request,
				   &color, VTE_DRAW_OPAQUE)) {
			/* We were able to draw with actual fonts. */
			return TRUE;
		}
		xcenter--;
		ycenter--;
		xright--;
		ybottom--;
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       (x + xcenter) / 2 - 1,
				       (y + ycenter) / 2,
				       (xright + xcenter) / 2 + 1,
				       (y + ycenter) / 2);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       (x + xcenter) / 2,
				       (y + ycenter) / 2,
				       (x + xcenter) / 2,
				       (ybottom + ycenter) / 2);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       (xright + xcenter) / 2,
				       (y + ycenter) / 2,
				       (xright + xcenter) / 2,
				       (ybottom + ycenter) / 2);
		break;
	/* case 0x2190: FIXME */
	/* case 0x2191: FIXME */
	/* case 0x2192: FIXME */
	/* case 0x2193: FIXME */
	/* case 0x2260: FIXME */
	case 0x2264: /* y */
		if (_vte_draw_char(terminal->pvt->draw, &request,
				   &color, VTE_DRAW_OPAQUE)) {
			/* We were able to draw with actual fonts. */
			return TRUE;
		}
		xcenter--;
		ycenter--;
		xright--;
		ybottom--;
		/* <= */
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       xright - 1, y,
				       x, (y + ycenter) / 2);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       x, (y + ycenter) / 2,
				       xright - 1, ycenter);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       x, ycenter,
				       xright - 1, (ycenter + ybottom) / 2);
		break;
	case 0x2265: /* z */
		if (_vte_draw_char(terminal->pvt->draw, &request,
				   &color, VTE_DRAW_OPAQUE)) {
			/* We were able to draw with actual fonts. */
			return TRUE;
		}
		xcenter--;
		ycenter--;
		xright--;
		ybottom--;
		/* >= */
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       x, y,
				       xright - 1, (y + ycenter) / 2);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       xright - 1, (y + ycenter) / 2,
				       x, ycenter);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       xright - 1, ycenter,
				       x, (ycenter + ybottom) / 2);
		break;
	case 0x23ba: /* o */
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    x, y,
					    column_width * columns,
					    VTE_LINE_WIDTH);
		break;
	case 0x23bb: /* p */
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    x, (y + ycenter) / 2,
					    column_width * columns,
					    VTE_LINE_WIDTH);
		break;
	case 0x23bc: /* r */
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    x,
					    (ycenter + ybottom) / 2,
					    column_width * columns,
					    VTE_LINE_WIDTH);
		break;
	case 0x23bd: /* s */
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    x,
					    ybottom - 1,
					    column_width * columns,
					    VTE_LINE_WIDTH);
		break;
	case 0x2409:  /* b */
		if (_vte_draw_char(terminal->pvt->draw, &request,
				   &color, VTE_DRAW_OPAQUE)) {
			/* We were able to draw with actual fonts. */
			return TRUE;
		}
		xcenter--;
		ycenter--;
		xright--;
		ybottom--;
		/* H */
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       x, y,
				       x, ycenter);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       xcenter, y,
				       xcenter, ycenter);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       x, (y + ycenter) / 2,
				       xcenter, (y + ycenter) / 2);
		/* T */
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       xcenter, ycenter,
				       xright - 1, ycenter);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       (xcenter + xright) / 2, ycenter,
				       (xcenter + xright) / 2, ybottom - 1);
		break;
	case 0x240a: /* e */
		if (_vte_draw_char(terminal->pvt->draw, &request,
				   &color, VTE_DRAW_OPAQUE)) {
			/* We were able to draw with actual fonts. */
			return TRUE;
		}
		xcenter--;
		ycenter--;
		xright--;
		ybottom--;
		/* L */
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       x, y,
				       x, ycenter);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       x, ycenter,
				       xcenter, ycenter);
		/* F */
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       xcenter, ycenter,
				       xcenter, ybottom - 1);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       xcenter, ycenter,
				       xright - 1, ycenter);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       xcenter, (ycenter + ybottom) / 2,
				       xright - 1, (ycenter + ybottom) / 2);
		break;
	case 0x240b: /* i */
		if (_vte_draw_char(terminal->pvt->draw, &request,
				   &color, VTE_DRAW_OPAQUE)) {
			/* We were able to draw with actual fonts. */
			return TRUE;
		}
		xcenter--;
		ycenter--;
		xright--;
		ybottom--;
		/* V */
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       x, y,
				       (x + xcenter) / 2, ycenter);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       (x + xcenter) / 2, ycenter,
				       xcenter, y);
		/* T */
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       xcenter, ycenter,
				       xright - 1, ycenter);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       (xcenter + xright) / 2, ycenter,
				       (xcenter + xright) / 2, ybottom - 1);
		break;
	case 0x240c:  /* c */
		if (_vte_draw_char(terminal->pvt->draw, &request,
				   &color, VTE_DRAW_OPAQUE)) {
			/* We were able to draw with actual fonts. */
			return TRUE;
		}
		xcenter--;
		ycenter--;
		xright--;
		ybottom--;
		/* F */
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       x, y,
				       x, ycenter);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       x, y,
				       xcenter, y);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       x, (y + ycenter) / 2,
				       xcenter, (y + ycenter) / 2);
		/* F */
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       xcenter, ycenter,
				       xcenter, ybottom - 1);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       xcenter, ycenter,
				       xright - 1, ycenter);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       xcenter, (ycenter + ybottom) / 2,
				       xright - 1, (ycenter + ybottom) / 2);
		break;
	case 0x240d: /* d */
		if (_vte_draw_char(terminal->pvt->draw, &request,
				   &color, VTE_DRAW_OPAQUE)) {
			/* We were able to draw with actual fonts. */
			return TRUE;
		}
		xcenter--;
		ycenter--;
		xright--;
		ybottom--;
		/* C */
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       x, y,
				       x, ycenter);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       x, y,
				       xcenter, y);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       x, ycenter,
				       xcenter, ycenter);
		/* R */
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       xcenter, ycenter,
				       xcenter, ybottom - 1);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       xcenter, ycenter,
				       xright - 1, ycenter);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       xright - 1, ycenter,
				       xright - 1, (ycenter + ybottom) / 2);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       xright - 1, (ycenter + ybottom) / 2,
				       xcenter, (ycenter + ybottom) / 2);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       xcenter, (ycenter + ybottom) / 2,
				       xright - 1, ybottom - 1);
		break;
	case 0x2424: /* h */
		if (_vte_draw_char(terminal->pvt->draw, &request,
				   &color, VTE_DRAW_OPAQUE)) {
			/* We were able to draw with actual fonts. */
			return TRUE;
		}
		xcenter--;
		ycenter--;
		xright--;
		ybottom--;
		/* N */
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       x, y,
				       x, ycenter);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       x, y,
				       xcenter, ycenter);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       xcenter, y,
				       xcenter, ycenter);
		/* L */
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       xcenter, ycenter,
				       xcenter, ybottom - 1);
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       xcenter, ybottom - 1,
				       xright - 1, ybottom - 1);
		break;
	case 0x2500: /* q */
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    x,
					    ycenter,
					    column_width * columns,
					    VTE_LINE_WIDTH);
		break;
	case 0x2501:
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    x,
					    ycenter,
					    column_width * columns,
					    VTE_LINE_WIDTH * 2);
		break;
	case 0x2502: /* x */
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    xcenter,
					    y,
					    VTE_LINE_WIDTH,
					    row_height);
		break;
	case 0x2503:
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    xcenter,
					    y,
					    VTE_LINE_WIDTH * 2,
					    row_height);
		break;
	case 0x250c: /* l */
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    xcenter,
					    ycenter,
					    xright - xcenter,
					    VTE_LINE_WIDTH);
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    xcenter,
					    ycenter,
					    VTE_LINE_WIDTH,
					    ybottom - ycenter);
		break;
	case 0x250f:
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    xcenter,
					    ycenter,
					    xright - xcenter,
					    VTE_LINE_WIDTH * 2);
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    xcenter,
					    ycenter,
					    VTE_LINE_WIDTH * 2,
					    ybottom - ycenter);
		break;
	case 0x2510: /* k */
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    x,
					    ycenter,
					    xcenter - x + VTE_LINE_WIDTH,
					    VTE_LINE_WIDTH);
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    xcenter,
					    ycenter,
					    VTE_LINE_WIDTH,
					    ybottom - ycenter);
		break;
	case 0x2513:
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    x,
					    ycenter,
					    xcenter - x + VTE_LINE_WIDTH * 2,
					    VTE_LINE_WIDTH * 2);
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    xcenter,
					    ycenter,
					    VTE_LINE_WIDTH * 2,
					    ybottom - ycenter);
		break;
	case 0x2514: /* m */
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    xcenter,
					    ycenter,
					    xright - xcenter,
					    VTE_LINE_WIDTH);
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    xcenter,
					    y,
					    VTE_LINE_WIDTH,
					    ycenter - y + VTE_LINE_WIDTH);
		break;
	case 0x2517:
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    xcenter,
					    ycenter,
					    xright - xcenter,
					    VTE_LINE_WIDTH * 2);
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    xcenter,
					    y,
					    VTE_LINE_WIDTH * 2,
					    ycenter - y + VTE_LINE_WIDTH * 2);
		break;
	case 0x2518: /* j */
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    x,
					    ycenter,
					    xcenter - x + VTE_LINE_WIDTH,
					    VTE_LINE_WIDTH);
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    xcenter,
					    y,
					    VTE_LINE_WIDTH,
					    ycenter - y + VTE_LINE_WIDTH);
		break;
	case 0x251b:
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    x,
					    ycenter,
					    xcenter - x + VTE_LINE_WIDTH * 2,
					    VTE_LINE_WIDTH * 2);
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    xcenter,
					    y,
					    VTE_LINE_WIDTH * 2,
					    ycenter - y + VTE_LINE_WIDTH * 2);
		break;
	case 0x251c: /* t */
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    xcenter,
					    y,
					    VTE_LINE_WIDTH,
					    row_height);
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    xcenter,
					    ycenter,
					    xright - xcenter,
					    VTE_LINE_WIDTH);
		break;
	case 0x2523:
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    xcenter,
					    y,
					    VTE_LINE_WIDTH * 2,
					    row_height);
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    xcenter,
					    ycenter,
					    xright - xcenter,
					    VTE_LINE_WIDTH * 2);
		break;
	case 0x2524: /* u */
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    xcenter,
					    y,
					    VTE_LINE_WIDTH,
					    row_height);
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    x,
					    ycenter,
					    xcenter - x + VTE_LINE_WIDTH,
					    VTE_LINE_WIDTH);
		break;
	case 0x252b:
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    xcenter,
					    y,
					    VTE_LINE_WIDTH * 2,
					    row_height);
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    x,
					    ycenter,
					    xcenter - x + VTE_LINE_WIDTH * 2,
					    VTE_LINE_WIDTH * 2);
		break;
	case 0x252c: /* w */
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    xcenter,
					    ycenter,
					    VTE_LINE_WIDTH,
					    ybottom - ycenter);
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    x,
					    ycenter,
					    column_width * columns,
					    VTE_LINE_WIDTH);
		break;
	case 0x2533:
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    xcenter,
					    ycenter,
					    VTE_LINE_WIDTH * 2,
					    ybottom - ycenter);
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    x,
					    ycenter,
					    column_width * columns,
					    VTE_LINE_WIDTH * 2);
		break;
	case 0x2534: /* v */
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    xcenter,
					    y,
					    VTE_LINE_WIDTH,
					    ycenter - y + VTE_LINE_WIDTH);
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    x,
					    ycenter,
					    column_width * columns,
					    VTE_LINE_WIDTH);
		break;
	case 0x253c: /* n */
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    xcenter,
					    y,
					    VTE_LINE_WIDTH,
					    row_height);
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    x,
					    ycenter,
					    column_width * columns,
					    VTE_LINE_WIDTH);
		break;
	case 0x254b:
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    xcenter,
					    y,
					    VTE_LINE_WIDTH * 2,
					    row_height);
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    x,
					    ycenter,
					    column_width * columns,
					    VTE_LINE_WIDTH * 2);
		break;
	case 0x2592:  /* a */
		for (i = x; i <= xright; i++) {
			draw = ((i - x) & 1) == 0;
			for (j = y; j < ybottom; j++) {
				if (draw) {
					vte_terminal_draw_point(terminal,
								&terminal->pvt->palette[fore],
								i, j);
				}
				draw = !draw;
			}
		}
		break;
	case 0x25ae: /* solid rectangle */
		vte_terminal_fill_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    x, y,
					    xright - x, ybottom - y);
		break;
	case 0x25c6:
		/* diamond */
		vte_terminal_draw_point(terminal,
					&terminal->pvt->palette[fore],
					xcenter - 2, ycenter);
		vte_terminal_draw_point(terminal,
					&terminal->pvt->palette[fore],
					xcenter + 2, ycenter);
		vte_terminal_draw_point(terminal,
					&terminal->pvt->palette[fore],
					xcenter, ycenter - 2);
		vte_terminal_draw_point(terminal,
					&terminal->pvt->palette[fore],
					xcenter, ycenter + 2);
		vte_terminal_draw_point(terminal,
					&terminal->pvt->palette[fore],
					xcenter - 1, ycenter - 1);
		vte_terminal_draw_point(terminal,
					&terminal->pvt->palette[fore],
					xcenter - 1, ycenter + 1);
		vte_terminal_draw_point(terminal,
					&terminal->pvt->palette[fore],
					xcenter + 1, ycenter - 1);
		vte_terminal_draw_point(terminal,
					&terminal->pvt->palette[fore],
					xcenter + 1, ycenter + 1);
		break;
	default:
		ret = FALSE;
		break;
	}
	return ret;
}

/* Draw a string of characters with similar attributes. */
static void
vte_terminal_draw_cells(VteTerminal *terminal,
			struct _vte_draw_text_request *items, gssize n,
			gint fore, gint back, gboolean draw_default_bg,
			gboolean bold, gboolean underline,
			gboolean strikethrough, gboolean hilite, gboolean boxed,
			gint column_width, gint row_height)
{
	int i, x, y, ascent;
	gint columns = 0;
	GdkColor color = {0,};
	struct vte_palette_entry *fg, *bg, *defbg;

	g_assert(n > 0);
	x = items[0].x;
	y = items[0].y;

	bold = bold && terminal->pvt->allow_bold;
	fg = &terminal->pvt->palette[fore];
	bg = &terminal->pvt->palette[back];
	defbg = &terminal->pvt->palette[VTE_DEF_BG];
	ascent = terminal->char_ascent;

#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_MISC) && 0) {
		fprintf(stderr, "Rendering");
		for (i = 0; i < n; i++) {
			fprintf(stderr, " (%ld,%ld)",
				(long) items[i].c,
				(long) items[i].columns);
			g_assert(items[i].columns > 0);
		}
		fprintf(stderr, ".\n");
	}
#endif

	columns = 0;
	for (i = 0; i < n; i++) {
		/* Adjust for the border. */
		items[i].x += VTE_PAD_WIDTH;
		items[i].y += VTE_PAD_WIDTH;
		columns += items[i].columns;
	}
	if (bg != defbg) {
		color.red = bg->red;
		color.blue = bg->blue;
		color.green = bg->green;
		_vte_draw_fill_rectangle(terminal->pvt->draw,
					 x + VTE_PAD_WIDTH, y + VTE_PAD_WIDTH,
					 columns * column_width,
					 row_height,
					 &color, VTE_DRAW_OPAQUE);
	}
	color.red = fg->red;
	color.blue = fg->blue;
	color.green = fg->green;
	_vte_draw_text(terminal->pvt->draw,
		       items, n,
		       &color, VTE_DRAW_OPAQUE);
	if (bold) {
		/* Take a step to the right. */
		for (i = 0; i < n; i++) {
			items[i].x++;
		}
		_vte_draw_text(terminal->pvt->draw,
			       items, n,
			       &color, VTE_DRAW_OPAQUE);
		/* Now take a step back. */
		for (i = 0; i < n; i++) {
			items[i].x--;
		}
	}
	for (i = 0; i < n; i++) {
		/* Deadjust for the border. */
		items[i].x -= VTE_PAD_WIDTH;
		items[i].y -= VTE_PAD_WIDTH;
	}

	/* Draw whatever SFX are required. */
	if (underline) {
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       x,
				       y + MIN(ascent + 2, row_height - 1),
				       x + (columns * column_width) - 1,
				       y + ascent + 2);
	}
	if (strikethrough) {
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       x, y + ascent / 2,
				       x + (columns * column_width) - 1,
				       y + (ascent + row_height)/4);
	}
	if (hilite) {
		vte_terminal_draw_line(terminal,
				       &terminal->pvt->palette[fore],
				       x,
				       y + row_height - 1,
				       x + (columns * column_width) - 1,
				       y + row_height - 1);
	}
	if (boxed) {
		vte_terminal_draw_rectangle(terminal,
					    &terminal->pvt->palette[fore],
					    x, y,
					    MAX(0,
						(columns * column_width) - 1),
					    MAX(0,
						row_height - 1));
	}
}

/* Try to map a PangoColor to a palette entry and return its index. */
static int
_vte_terminal_map_pango_color(VteTerminal *terminal, PangoColor *color)
{
	long distance[G_N_ELEMENTS(terminal->pvt->palette)];
	struct vte_palette_entry *entry;
	int i, ret;

	/* Calculate a "distance" value.  Could stand to be improved a bit. */
	for (i = 0; i < G_N_ELEMENTS(distance); i++) {
		entry = &terminal->pvt->palette[i];
		distance[i] = 0;
		distance[i] += ((entry->red >> 8) - (color->red >> 8)) *
			       ((entry->red >> 8) - (color->red >> 8));
		distance[i] += ((entry->blue >> 8) - (color->blue >> 8)) *
			       ((entry->blue >> 8) - (color->blue >> 8));
		distance[i] += ((entry->green >> 8) - (color->green >> 8)) *
			       ((entry->green >> 8) - (color->green >> 8));
	}

	/* Find the index of the minimum value. */
	ret = 0;
	for (i = 1; i < G_N_ELEMENTS(distance); i++) {
		if (distance[i] < distance[ret]) {
			ret = i;
		}
	}

#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_UPDATES)) {
		fprintf(stderr, "mapped PangoColor(%04x,%04x,%04x) to "
			"palette entry (%04x,%04x,%04x)\n",
			color->red, color->green, color->blue,
			terminal->pvt->palette[ret].red,
			terminal->pvt->palette[ret].green,
			terminal->pvt->palette[ret].blue);
	}
#endif

	return ret;
}

/* FIXME: we don't have a way to tell GTK+ what the default text attributes
 * should be, so for now at least it's assuming white-on-black is the norm and
 * is using "black-on-white" to signify "inverse".  Pick up on that state and
 * fix things.  Do this here, so that if we suddenly get red-on-black, we'll do
 * the right thing. */
static void
_vte_terminal_fudge_pango_colors(VteTerminal *terminal, GSList *attributes,
				 struct vte_charcell *cells, gssize n)
{
	gboolean saw_fg, saw_bg;
	PangoAttribute *attr;
	PangoAttrColor *color;
	PangoColor fg, bg;
	int i;

	saw_fg = saw_bg = FALSE;

	while (attributes != NULL) {
		attr = attributes->data;
		switch (attr->klass->type) {
		case PANGO_ATTR_FOREGROUND:
			saw_fg = TRUE;
			color = (PangoAttrColor*) attr;
			fg = color->color;
			break;
		case PANGO_ATTR_BACKGROUND:
			saw_bg = TRUE;
			color = (PangoAttrColor*) attr;
			bg = color->color;
			break;
		default:
			break;
		}
		attributes = g_slist_next(attributes);
	}

	if (saw_fg && saw_bg &&
	    (fg.red == 0xffff) && (fg.green == 0xffff) && (fg.blue == 0xffff) &&
	    (bg.red == 0) && (bg.green == 0) && (bg.blue == 0)) {
		for (i = 0; i < n; i++) {
			cells[i].fore = terminal->pvt->screen->color_defaults.fore;
			cells[i].back = terminal->pvt->screen->color_defaults.back;
			cells[i].reverse = TRUE;
		}
	}
}

/* Apply the attribute given in the PangoAttribute to the list of cells. */
static void
_vte_terminal_apply_pango_attr(VteTerminal *terminal, PangoAttribute *attr,
			       struct vte_charcell *cells, gsize n_cells)
{
	int i, ival;
	PangoAttrInt *attrint;
	PangoAttrColor *attrcolor;

	switch (attr->klass->type) {
	case PANGO_ATTR_FOREGROUND:
	case PANGO_ATTR_BACKGROUND:
		attrcolor = (PangoAttrColor*) attr;
		ival = _vte_terminal_map_pango_color(terminal,
						     &attrcolor->color);
		for (i = attr->start_index;
		     (ival >= 0) && (i < attr->end_index) && (i < n_cells);
		     i++) {
			if (attr->klass->type == PANGO_ATTR_FOREGROUND) {
				cells[i].fore = ival;
			}
			if (attr->klass->type == PANGO_ATTR_BACKGROUND) {
				cells[i].back = ival;
			}
		}
		break;
	case PANGO_ATTR_STRIKETHROUGH:
		attrint = (PangoAttrInt*) attr;
		ival = attrint->value;
		for (i = attr->start_index;
		     (i < attr->end_index) && (i < n_cells);
		     i++) {
			cells[i].strikethrough = (ival != FALSE);
		}
		break;
	case PANGO_ATTR_UNDERLINE:
		attrint = (PangoAttrInt*) attr;
		ival = attrint->value;
		for (i = attr->start_index;
		     (i < attr->end_index) && (i < n_cells);
		     i++) {
			cells[i].underline = (ival != PANGO_UNDERLINE_NONE);
		}
		break;
	case PANGO_ATTR_WEIGHT:
		attrint = (PangoAttrInt*) attr;
		ival = attrint->value;
		for (i = attr->start_index;
		     (i < attr->end_index) && (i < n_cells);
		     i++) {
			cells[i].bold = (ival >= PANGO_WEIGHT_BOLD);
		}
		break;
	default:
		break;
	}
}

/* Convert a PangoAttrList and a location in that list to settings in a
 * charcell structure.  The cells array is assumed to contain enough items
 * so that all ranges in the attribute list can be mapped into the array, which
 * typically means that the cell array should have the same length as the
 * string (byte-wise) which the attributes describe. */
static void
_vte_terminal_pango_attribute_destroy(gpointer attr, gpointer data)
{
	pango_attribute_destroy(attr);
}
static void
_vte_terminal_translate_pango_cells(VteTerminal *terminal, PangoAttrList *attrs,
				    struct vte_charcell *cells, gsize n_cells)
{
	PangoAttribute *attr;
	PangoAttrIterator *attriter;
	GSList *list, *listiter;
	int i;

	for (i = 0; i < n_cells; i++) {
		cells[i] = terminal->pvt->screen->fill_defaults;
	}

	attriter = pango_attr_list_get_iterator(attrs);
	if (attriter != NULL) {
		do {
			list = pango_attr_iterator_get_attrs(attriter);
			if (list != NULL) {
				for (listiter = list;
				     listiter != NULL;
				     listiter = g_slist_next(listiter)) {
					attr = listiter->data;
					_vte_terminal_apply_pango_attr(terminal,
								       attr,
								       cells,
								       n_cells);
				}
				attr = list->data;
				_vte_terminal_fudge_pango_colors(terminal,
								 list,
								 cells +
								 attr->start_index,
								 attr->end_index -
								 attr->start_index);
				g_slist_foreach(list,
						_vte_terminal_pango_attribute_destroy,
						NULL);
				g_slist_free(list);
			}
		} while (pango_attr_iterator_next(attriter) == TRUE);
		pango_attr_iterator_destroy(attriter);
	}
}

/* Draw the listed items using the given attributes.  Tricky because the
 * attribute string is indexed by byte in the UTF-8 representation of the string
 * of characters.  Because we draw a character at a time, this is slower. */
static void
vte_terminal_draw_cells_with_attributes(VteTerminal *terminal,
					struct _vte_draw_text_request *items,
					gssize n,
					PangoAttrList *attrs,
					gboolean draw_default_bg,
					gint column_width, gint height)
{
	int i, j, cell_count;
	struct vte_charcell *cells;
	char scratch_buf[VTE_UTF8_BPC];
	int fore, back;

	for (i = 0, cell_count = 0; i < n; i++) {
		cell_count += g_unichar_to_utf8(items[i].c, scratch_buf);
	}
	cells = g_malloc(cell_count * sizeof(struct vte_charcell));
	_vte_terminal_translate_pango_cells(terminal, attrs, cells, cell_count);
	for (i = 0, j = 0; i < n; i++) {
		vte_terminal_determine_colors(terminal, &cells[j],
					      FALSE,
					      FALSE,
					      FALSE,
					      &fore, &back);
		vte_terminal_draw_cells(terminal, items + i, 1,
					fore,
					back,
					draw_default_bg,
					cells[j].bold,
					cells[j].underline,
					cells[j].strikethrough,
					FALSE, FALSE, column_width, height);
		j += g_unichar_to_utf8(items[i].c, scratch_buf);
	}
	g_free(cells);
}

static gboolean
vte_terminal_get_blink_state(VteTerminal *terminal)
{
	struct timezone tz;
	struct timeval tv;
	gint blink_cycle = 1000;
	GtkSettings *settings;
	time_t daytime;
	gboolean blink;
	GtkWidget *widget;

	/* Determine if blinking text should be shown. */
	if (terminal->pvt->cursor_blinks) {
		if (gettimeofday(&tv, &tz) == 0) {
			widget = GTK_WIDGET(terminal);
			settings = gtk_widget_get_settings(widget);
			if (G_IS_OBJECT(settings)) {
				g_object_get(G_OBJECT(settings),
					     "gtk-cursor-blink-time",
					     &blink_cycle, NULL);
			}
			daytime = (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
			if (daytime >= terminal->pvt->last_keypress_time) {
				daytime -= terminal->pvt->last_keypress_time;
			}
			daytime = daytime % blink_cycle;
			blink = daytime < (blink_cycle / 2);
		} else {
			blink = TRUE;
		}
	} else {
		blink = TRUE;
	}
	if (terminal->pvt->cursor_force_fg > 0) {
		terminal->pvt->cursor_force_fg--;
		blink = TRUE;
	}
	return blink;
}

/* Paint the contents of a given row at the given location.  Take advantage
 * of multiple-draw APIs by finding runs of characters with identical
 * attributes and bundling them together. */
static void
vte_terminal_draw_row(VteTerminal *terminal,
		      VteScreen *screen,
		      gint row,
		      gint column, gint column_count,
		      gint x, gint y,
		      gint column_width, gint row_height)
{
	int i, j, fore, nfore, back, nback;
	gboolean underline, nunderline, bold, nbold, hilite, nhilite, reverse,
		 selected, strikethrough, nstrikethrough, drawn;
	struct _vte_draw_text_request *items, item;
	guint item_count = 0;
	struct vte_charcell *cell;

	/* Allocate an array to hold draw requests. */
	/* FIXME: can this get too big for alloca? */
	items = g_newa (struct _vte_draw_text_request, column_count);

	/* Back up in case this is a multicolumn character, making the drawing
	 * area a little wider. */
	cell = vte_terminal_find_charcell(terminal, column, row);
	while ((cell != NULL) && (cell->fragment) && (column > 0)) {
		column--;
		column_count++;
		x -= column_width;
		cell = vte_terminal_find_charcell(terminal, column, row);
	}

	/* Walk the line. */
	i = column;
	while (i < column + column_count) {
		/* Get the character cell's contents. */
		cell = vte_terminal_find_charcell(terminal, i, row);
		/* Find the colors for this cell. */
		reverse = terminal->pvt->screen->reverse_mode;
		selected = vte_cell_is_selected(terminal, i, row, NULL);
		vte_terminal_determine_colors(terminal, cell,
					      reverse || selected,
					      selected,
					      FALSE,
					      &fore, &back);
		underline = (cell != NULL) ? (cell->underline != 0) : FALSE;
		strikethrough = (cell != NULL) ? (cell->strikethrough != 0) : FALSE;
		bold = (cell != NULL) ? (cell->bold != 0) : FALSE;
		if ((cell != NULL) && (terminal->pvt->match_contents != NULL)) {
			hilite = vte_cell_is_between(i, row,
						     terminal->pvt->match_start.column,
						     terminal->pvt->match_start.row,
						     terminal->pvt->match_end.column,
						     terminal->pvt->match_end.row,
						     TRUE);
		} else {
			hilite = FALSE;
		}

		item.c = cell ? (cell->c ? cell->c : ' ') : ' ';
		item.columns = cell ? cell->columns : 1;
		item.x = x + ((i - column) * column_width);
		item.y = y;

		/* If this is a graphics character, draw it locally. */
		if ((cell != NULL) && vte_unichar_is_local_graphic(cell->c)) {
			drawn = vte_terminal_draw_graphic(terminal,
							  item.c,
							  fore, back,
							  FALSE,
							  item.x,
							  item.y,
							  column_width,
							  item.columns,
							  row_height);
			if (drawn) {
				i += item.columns;
				continue;
			}
		}

		/* If it's not a local graphic character, or if we couldn't
		   draw it, add it to the draw list. */
		items[item_count++] = item;

		/* Now find out how many cells have the same attributes. */
		j = i + item.columns;
		while ((j < column + column_count) &&
		       (j - i < VTE_DRAW_MAX_LENGTH)) {
			/* Retrieve the cell. */
			cell = vte_terminal_find_charcell(terminal, j, row);
			/* Resolve attributes to colors where possible and
			 * compare visual attributes to the first character
			 * in this chunk. */
			reverse = terminal->pvt->screen->reverse_mode;
			selected = vte_cell_is_selected(terminal, j, row, NULL);
			vte_terminal_determine_colors(terminal, cell,
						      reverse || selected,
						      selected,
						      FALSE,
						      &nfore, &nback);
			if ((nfore != fore) || (nback != back)) {
				break;
			}
			nbold = (cell != NULL) ?
				(cell->bold != 0) :
				FALSE;
			if (nbold != bold) {
				break;
			}
			/* Graphic characters must be drawn individually. */
			if ((cell != NULL) &&
			    vte_unichar_is_local_graphic(cell->c)) {
				break;
			}
			/* Don't render fragments of multicolumn characters
			 * which have the same attributes as the initial
			 * portions. */
			if ((cell != NULL) && (cell->fragment)) {
				j++;
				continue;
			}
			/* Break up underlined/not-underlined text. */
			nunderline = (cell != NULL) ?
				     (cell->underline != 0) :
				     FALSE;
			if (nunderline != underline) {
				break;
			}
			nstrikethrough = (cell != NULL) ?
					 (cell->strikethrough != 0) :
					 FALSE;
			if (nstrikethrough != strikethrough) {
				break;
			}
			/* Break up matched/not-matched text. */
			if ((cell != NULL) &&
			    (terminal->pvt->match_contents != NULL)) {
				nhilite = vte_cell_is_between(j, row,
							      terminal->pvt->match_start.column,
							      terminal->pvt->match_start.row,
							      terminal->pvt->match_end.column,
							      terminal->pvt->match_end.row,
							      TRUE);
			} else {
				nhilite = FALSE;
			}
			if (nhilite != hilite) {
				break;
			}
			/* Add this cell to the draw list. */
			item.c = cell ? (cell->c ? cell->c : ' ') : ' ';
			item.columns = cell ? cell->columns : 1;
			item.x = x + ((j - column) * column_width);
			item.y = y;
			items[item_count++] = item;
			j += item.columns;
		}
		/* Draw the cells. */
		vte_terminal_draw_cells(terminal,
					items,
					item_count,
					fore, back, FALSE,
					bold, underline,
					strikethrough, hilite, FALSE,
					column_width, row_height);
		item_count = 0;
		/* We'll need to continue at the first cell which didn't
		 * match the first one in this set. */
		i = j;
	}
}

/* Draw the widget. */
static void
vte_terminal_paint(GtkWidget *widget, GdkRectangle *area)
{
	VteTerminal *terminal = NULL;
	VteScreen *screen;
	struct vte_charcell *cell;
	struct _vte_draw_text_request item, *items;
	int row, drow, col, row_stop, col_stop, columns;
	char *preedit;
	int preedit_cursor;
	long width, height, ascent, descent, delta, cursor_width;
	int i, len, fore, back, x, y;
	GdkRectangle all_area;
	gboolean blink, selected;

#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_LIFECYCLE)) {
		fprintf(stderr, "vte_terminal_paint()\n");
	}
#endif

	/* Make a few sanity checks. */
	g_assert(widget != NULL);
	g_assert(VTE_IS_TERMINAL(widget));
	g_assert(area != NULL);
	terminal = VTE_TERMINAL(widget);

	/* Get going. */
	screen = terminal->pvt->screen;

	/* Keep local copies of rendering information. */
	width = terminal->char_width;
	height = terminal->char_height;
	ascent = terminal->char_ascent;
	descent = terminal->char_descent;
	delta = screen->scroll_delta;

	/* Calculate the bounding rectangle. */
	if (_vte_draw_requires_repaint(terminal->pvt->draw)) {
		all_area.x = 0;
		all_area.y = 0;
		all_area.width = terminal->char_width * terminal->column_count;
		all_area.width += 2 * VTE_PAD_WIDTH;
		all_area.height = terminal->char_height * terminal->row_count;
		all_area.height += 2 * VTE_PAD_WIDTH;
		area = &all_area;
	}
	row = MAX(0, (area->y - VTE_PAD_WIDTH) / height);
	row_stop = MIN(howmany((area->y - VTE_PAD_WIDTH) + area->height,
			       height),
		       terminal->row_count - 1);
	col = MAX(0, (area->x - VTE_PAD_WIDTH) / width);
	col_stop = MIN(howmany((area->x - VTE_PAD_WIDTH) + area->width,
			       width),
		       terminal->column_count - 1);

	/* Designate the start of the drawing operation and clear the area. */
	_vte_draw_start(terminal->pvt->draw);
	if (terminal->pvt->bg_transparent) {
		gdk_window_get_origin(widget->window, &x, &y);
		_vte_draw_set_scroll(terminal->pvt->draw, x, y);
	} else {
		if (terminal->pvt->scroll_background) {
			_vte_draw_set_scroll(terminal->pvt->draw,
					     0,
					     delta * terminal->char_height);
		} else {
			_vte_draw_set_scroll(terminal->pvt->draw, 0, 0);
		}
	}
	_vte_draw_clear(terminal->pvt->draw,
			area->x, area->y, area->width, area->height);

	/* Now we're ready to draw the text.  Iterate over the rows we
	 * need to draw. */
	while (row <= row_stop) {
		vte_terminal_draw_row(terminal,
				      screen,
				      row + delta,
				      col,
				      col_stop - col + 1,
				      col * width,
				      row * height,
				      width,
				      height);
		row++;
	}

	/* Draw the cursor. */
	if (terminal->pvt->cursor_visible &&
	    (CLAMP(screen->cursor_current.col, 0, terminal->column_count - 1) ==
	     screen->cursor_current.col) &&
	    (CLAMP(screen->cursor_current.row,
		   delta, delta + terminal->row_count - 1) ==
	     screen->cursor_current.row)) {
		/* Get the location of the cursor. */
		col = screen->cursor_current.col;
		drow = screen->cursor_current.row;
		row = drow - delta;

		/* Find the character "under" the cursor. */
		cell = vte_terminal_find_charcell(terminal, col, drow);
		while ((cell != NULL) && (cell->fragment) && (col > 0)) {
			col--;
			cell = vte_terminal_find_charcell(terminal, col, drow);
		}

		/* Draw the cursor. */
		item.c = cell ? (cell->c ? cell->c : ' ') : ' ';
		item.columns = cell ? cell->columns : 1;
		item.x = col * width;
		item.y = row * height;
		cursor_width = item.columns * width;
		if (cell) {
			cursor_width = MAX(cursor_width,
					   _vte_draw_get_char_width(terminal->pvt->draw,
								    cell->c,
								    cell->columns));
		}
		if (GTK_WIDGET_HAS_FOCUS(GTK_WIDGET(terminal))) {
			selected = vte_cell_is_selected(terminal, col, drow,
							NULL);
			blink = vte_terminal_get_blink_state(terminal) ^
				terminal->pvt->screen->reverse_mode;
			vte_terminal_determine_colors(terminal, cell,
						      blink,
						      selected,
						      blink,
						      &fore, &back);
			_vte_draw_clear(terminal->pvt->draw,
					col * width + VTE_PAD_WIDTH,
					row * height + VTE_PAD_WIDTH,
					cursor_width,
					height);
			if (blink) {
				GdkColor color;
				color.red = terminal->pvt->palette[back].red;
				color.green = terminal->pvt->palette[back].green;
				color.blue = terminal->pvt->palette[back].blue;
				_vte_draw_fill_rectangle(terminal->pvt->draw,
							 item.x + VTE_PAD_WIDTH,
							 item.y + VTE_PAD_WIDTH,
							 cursor_width, height,
							 &color,
							 VTE_DRAW_OPAQUE);
			}
			if (!vte_unichar_is_local_graphic(item.c) ||
			    !vte_terminal_draw_graphic(terminal,
						       item.c,
						       fore, back,
						       TRUE,
						       item.x,
						       item.y,
						       width,
						       item.columns,
						       height)) {
				vte_terminal_draw_cells(terminal,
							&item, 1,
							fore, back,
							TRUE,
							cell && cell->bold,
							cell && cell->underline,
							cell && cell->strikethrough,
							FALSE,
							FALSE,
							width,
							height);
			}
		} else {
			GdkColor color;
			/* Draw it as a hollow rectangle. */
			vte_terminal_determine_colors(terminal, cell,
						      FALSE,
						      FALSE,
						      FALSE,
						      &fore, &back);
			_vte_draw_clear(terminal->pvt->draw,
					col * width + VTE_PAD_WIDTH,
					row * height + VTE_PAD_WIDTH,
					cursor_width, height);
			vte_terminal_draw_cells(terminal,
						&item, 1,
						fore, back, TRUE,
						cell && cell->bold,
						cell && cell->underline,
						cell && cell->strikethrough,
						FALSE,
						FALSE,
						width,
						height);
			color.red = terminal->pvt->palette[fore].red;
			color.green = terminal->pvt->palette[fore].green;
			color.blue = terminal->pvt->palette[fore].blue;
			_vte_draw_draw_rectangle(terminal->pvt->draw,
						 item.x + VTE_PAD_WIDTH,
						 item.y + VTE_PAD_WIDTH,
						 cursor_width, height,
						 &color,
						 VTE_DRAW_OPAQUE);
		}
	}

	/* Draw the pre-edit string (if one exists) over the cursor. */
	if (terminal->pvt->im_preedit) {
		drow = screen->cursor_current.row;
		row = screen->cursor_current.row - delta;

		/* Find out how many columns the pre-edit string takes up. */
		preedit = terminal->pvt->im_preedit;
		preedit_cursor = -1;
		columns = vte_terminal_preedit_width(terminal, FALSE);
		len = vte_terminal_preedit_length(terminal, FALSE);

		/* If the pre-edit string won't fit on the screen if we start
		 * drawing it at the cursor's position, move it left. */
		col = screen->cursor_current.col;
		if (col + columns > terminal->column_count) {
			col = MAX(0, terminal->column_count - columns);
		}

		/* Draw the preedit string, boxed. */
		if (len > 0) {
			items = g_malloc(sizeof(struct _vte_draw_text_request) *
					 (len + 1));
			preedit = terminal->pvt->im_preedit;
			for (i = columns = 0; i < len; i++) {
				if ((preedit - terminal->pvt->im_preedit) ==
				    terminal->pvt->im_preedit_cursor) {
					preedit_cursor = i;
				}
				items[i].c = g_utf8_get_char(preedit);
				items[i].columns = _vte_iso2022_unichar_width(items[i].c);
				items[i].x = (col + columns) * width;
				items[i].y = row * height;
				columns += items[i].columns;
				preedit = g_utf8_next_char(preedit);
			}
			if ((preedit - terminal->pvt->im_preedit) ==
			    terminal->pvt->im_preedit_cursor) {
				preedit_cursor = i;
			}
			items[len].c = ' ';
			items[len].columns = 1;
			items[len].x = (col + columns) * width;
			items[len].y = row * height;
			_vte_draw_clear(terminal->pvt->draw,
					col * width + VTE_PAD_WIDTH,
					row * height + VTE_PAD_WIDTH,
					width * columns,
					height);
			fore = screen->defaults.fore;
			back = screen->defaults.back;
			vte_terminal_draw_cells_with_attributes(terminal,
								items, len + 1,
								terminal->pvt->im_preedit_attrs,
								TRUE,
								width, height);
			if ((preedit_cursor >= 0) && (preedit_cursor < len)) {
				/* Cursored letter in reverse. */
				vte_terminal_draw_cells(terminal,
							&items[terminal->pvt->im_preedit_cursor], 1,
							back, fore, TRUE,
							FALSE,
							FALSE,
							FALSE,
							FALSE,
							TRUE,
							width, height);
			} else
			if (preedit_cursor == len) {
				/* Empty cursor at the end. */
				vte_terminal_draw_cells(terminal,
							&items[len], 1,
							back, fore, TRUE,
							FALSE,
							FALSE,
							FALSE,
							FALSE,
							FALSE,
							width, height);
			}
			g_free(items);
		}
	}

	/* Done with various structures. */
	_vte_draw_end(terminal->pvt->draw);
}

/* Handle an expose event by painting the exposed area. */
static gint
vte_terminal_expose(GtkWidget *widget, GdkEventExpose *event)
{
	g_assert(VTE_IS_TERMINAL(widget));
	if (event->window == widget->window) {
		if (GTK_WIDGET_REALIZED(widget) &&
		    GTK_WIDGET_VISIBLE(widget) &&
		    GTK_WIDGET_MAPPED(widget)) {
			vte_terminal_paint(widget, &event->area);
		}
	} else {
		g_assert_not_reached();
	}
	return FALSE;
}

/* Handle a scroll event. */
static gboolean
vte_terminal_scroll(GtkWidget *widget, GdkEventScroll *event)
{
	GtkAdjustment *adj;
	VteTerminal *terminal;
	gdouble new_value;
	GdkModifierType modifiers;
	int button;

	g_assert(VTE_IS_TERMINAL(widget));
	terminal = VTE_TERMINAL(widget);

	/* Read the modifiers. */
	if (gdk_event_get_state((GdkEvent*)event, &modifiers)) {
		terminal->pvt->modifiers = modifiers;
	}

#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_EVENTS)) {
		switch (event->direction) {
		case GDK_SCROLL_UP:
			fprintf(stderr, "Scroll up.\n");
			break;
		case GDK_SCROLL_DOWN:
			fprintf(stderr, "Scroll down.\n");
			break;
		default:
			break;
		}
	}
#endif

	/* If we're running a mouse-aware application, map the scroll event
	 * to a button press on buttons four and five. */
	if (terminal->pvt->mouse_send_xy_on_click ||
	    terminal->pvt->mouse_send_xy_on_button ||
	    terminal->pvt->mouse_hilite_tracking ||
	    terminal->pvt->mouse_cell_motion_tracking ||
	    terminal->pvt->mouse_all_motion_tracking) {
		switch (event->direction) {
		case GDK_SCROLL_UP:
			button = 4;
			break;
		case GDK_SCROLL_DOWN:
			button = 5;
			break;
		default:
			button = 0;
			break;
		}
		if (button != 0) {
			/* Encode the parameters and send them to the app. */
			vte_terminal_send_mouse_button_internal(terminal,
								button,
								event->x - VTE_PAD_WIDTH,
								event->y - VTE_PAD_WIDTH);
		}
		if (terminal->pvt->mouse_send_xy_on_button ||
		    terminal->pvt->mouse_hilite_tracking ||
		    terminal->pvt->mouse_cell_motion_tracking ||
		    terminal->pvt->mouse_all_motion_tracking) {
			/* If the app cares, send a release event as well. */
			vte_terminal_send_mouse_button_internal(terminal,
								0,
								event->x - VTE_PAD_WIDTH,
								event->y - VTE_PAD_WIDTH);
		}
		return TRUE;
	}

	/* Perform a history scroll. */
	adj = (VTE_TERMINAL(widget))->adjustment;

	switch (event->direction) {
	case GDK_SCROLL_UP:
		new_value = adj->value - adj->page_increment / 2;
		break;
	case GDK_SCROLL_DOWN:
		new_value = adj->value + adj->page_increment / 2;
		break;
	default:
		return FALSE;
	}

	new_value = CLAMP(new_value, adj->lower, adj->upper - adj->page_size);
	gtk_adjustment_set_value(adj, new_value);

	return TRUE;
}

/* Create a new accessible object associated with ourselves, and return
 * it to the caller. */
static AtkObject *
vte_terminal_get_accessible(GtkWidget *widget)
{
	VteTerminal *terminal;
	static gboolean first_time = TRUE;

	g_assert(VTE_IS_TERMINAL(widget));
	terminal = VTE_TERMINAL(widget);

	if (first_time) {
		AtkObjectFactory *factory;
		AtkRegistry *registry;
		GType derived_type;
		GType derived_atk_type;

		/*
		 * Figure out whether accessibility is enabled by looking at the
		 * type of the accessible object which would be created for
		 * the parent type of VteTerminal.
		 */
		derived_type = g_type_parent (VTE_TYPE_TERMINAL);

		registry = atk_get_default_registry ();
		factory = atk_registry_get_factory (registry,
						    derived_type);

		derived_atk_type = atk_object_factory_get_accessible_type (factory);
		if (g_type_is_a (derived_atk_type, GTK_TYPE_ACCESSIBLE)) {
			atk_registry_set_factory_type (registry,
						       VTE_TYPE_TERMINAL,
						       vte_terminal_accessible_factory_get_type ());
		}
		first_time = FALSE;
	}

	return GTK_WIDGET_CLASS (parent_class)->get_accessible (widget);
}

/* Initialize methods. */
static void
vte_terminal_class_init(VteTerminalClass *klass, gconstpointer data)
{
	GObjectClass *gobject_class;
	GtkWidgetClass *widget_class;

#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_LIFECYCLE)) {
		fprintf(stderr, "vte_terminal_class_init()\n");
	}
#endif

	bindtextdomain(PACKAGE, LOCALEDIR);
#ifdef HAVE_DECL_BIND_TEXTDOMAIN_CODESET
	bind_textdomain_codeset(PACKAGE, "UTF-8");
#endif

	g_type_class_add_private(klass, sizeof (VteTerminalPrivate));

	gobject_class = G_OBJECT_CLASS(klass);
	widget_class = GTK_WIDGET_CLASS(klass);

	parent_class = g_type_class_peek_parent (klass);
	/* Override some of the default handlers. */
	gobject_class->finalize = vte_terminal_finalize;
	widget_class->realize = vte_terminal_realize;
	widget_class->scroll_event = vte_terminal_scroll;
	widget_class->expose_event = vte_terminal_expose;
	widget_class->key_press_event = vte_terminal_key_press;
	widget_class->key_release_event = vte_terminal_key_release;
	widget_class->button_press_event = vte_terminal_button_press;
	widget_class->button_release_event = vte_terminal_button_release;
	widget_class->motion_notify_event = vte_terminal_motion_notify;
	widget_class->focus_in_event = vte_terminal_focus_in;
	widget_class->focus_out_event = vte_terminal_focus_out;
	widget_class->visibility_notify_event = vte_terminal_visibility_notify;
	widget_class->unrealize = vte_terminal_unrealize;
	widget_class->size_request = vte_terminal_size_request;
	widget_class->size_allocate = vte_terminal_size_allocate;
	widget_class->get_accessible = vte_terminal_get_accessible;
	widget_class->show = vte_terminal_show;

	/* Initialize default handlers. */
	klass->eof = NULL;
	klass->child_exited = NULL;
	klass->emulation_changed = NULL;
	klass->encoding_changed = NULL;
	klass->char_size_changed = NULL;
	klass->window_title_changed = NULL;
	klass->icon_title_changed = NULL;
	klass->selection_changed = NULL;
	klass->contents_changed = NULL;
	klass->cursor_moved = NULL;
	klass->status_line_changed = NULL;
	klass->commit = NULL;

	klass->deiconify_window = NULL;
	klass->iconify_window = NULL;
	klass->raise_window = NULL;
	klass->lower_window = NULL;
	klass->refresh_window = NULL;
	klass->restore_window = NULL;
	klass->maximize_window = NULL;
	klass->resize_window = NULL;
	klass->move_window = NULL;

	klass->increase_font_size = NULL;
	klass->decrease_font_size = NULL;

	klass->text_modified = NULL;
	klass->text_inserted = NULL;
	klass->text_deleted = NULL;
	klass->text_scrolled = NULL;

	/* Register some signals of our own. */
	klass->eof_signal =
		g_signal_new("eof",
			     G_OBJECT_CLASS_TYPE(klass),
			     G_SIGNAL_RUN_LAST,
			     G_STRUCT_OFFSET(VteTerminalClass, eof),
			     NULL,
			     NULL,
			     _vte_marshal_VOID__VOID,
			     G_TYPE_NONE, 0);
	klass->child_exited_signal =
		g_signal_new("child-exited",
			     G_OBJECT_CLASS_TYPE(klass),
			     G_SIGNAL_RUN_LAST,
			     G_STRUCT_OFFSET(VteTerminalClass, child_exited),
			     NULL,
			     NULL,
			     _vte_marshal_VOID__VOID,
			     G_TYPE_NONE, 0);
	klass->window_title_changed_signal =
		g_signal_new("window-title-changed",
			     G_OBJECT_CLASS_TYPE(klass),
			     G_SIGNAL_RUN_LAST,
			     G_STRUCT_OFFSET(VteTerminalClass, window_title_changed),
			     NULL,
			     NULL,
			     _vte_marshal_VOID__VOID,
			     G_TYPE_NONE, 0);
	klass->icon_title_changed_signal =
		g_signal_new("icon-title-changed",
			     G_OBJECT_CLASS_TYPE(klass),
			     G_SIGNAL_RUN_LAST,
			     G_STRUCT_OFFSET(VteTerminalClass, icon_title_changed),
			     NULL,
			     NULL,
			     _vte_marshal_VOID__VOID,
			     G_TYPE_NONE, 0);
	klass->encoding_changed_signal =
		g_signal_new("encoding-changed",
			     G_OBJECT_CLASS_TYPE(klass),
			     G_SIGNAL_RUN_LAST,
			     G_STRUCT_OFFSET(VteTerminalClass, encoding_changed),
			     NULL,
			     NULL,
			     _vte_marshal_VOID__VOID,
			     G_TYPE_NONE, 0);
	klass->commit_signal =
		g_signal_new("commit",
			     G_OBJECT_CLASS_TYPE(klass),
			     G_SIGNAL_RUN_LAST,
			     G_STRUCT_OFFSET(VteTerminalClass, commit),
			     NULL,
			     NULL,
			     _vte_marshal_VOID__STRING_UINT,
			     G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_UINT);
	klass->emulation_changed_signal =
		g_signal_new("emulation-changed",
			     G_OBJECT_CLASS_TYPE(klass),
			     G_SIGNAL_RUN_LAST,
			     G_STRUCT_OFFSET(VteTerminalClass, emulation_changed),
			     NULL,
			     NULL,
			     _vte_marshal_VOID__VOID,
			     G_TYPE_NONE, 0);
	klass->char_size_changed_signal =
		g_signal_new("char-size-changed",
			     G_OBJECT_CLASS_TYPE(klass),
			     G_SIGNAL_RUN_LAST,
			     G_STRUCT_OFFSET(VteTerminalClass, char_size_changed),
			     NULL,
			     NULL,
			     _vte_marshal_VOID__UINT_UINT,
			     G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_UINT);
	klass->selection_changed_signal =
		g_signal_new ("selection-changed",
			      G_OBJECT_CLASS_TYPE(klass),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET(VteTerminalClass, selection_changed),
			      NULL,
			      NULL,
			      _vte_marshal_VOID__VOID,
			      G_TYPE_NONE, 0);
	klass->contents_changed_signal =
		g_signal_new("contents-changed",
			     G_OBJECT_CLASS_TYPE(klass),
			     G_SIGNAL_RUN_LAST,
			     G_STRUCT_OFFSET(VteTerminalClass, contents_changed),
			     NULL,
			     NULL,
			     _vte_marshal_VOID__VOID,
			     G_TYPE_NONE, 0);
	klass->cursor_moved_signal =
		g_signal_new("cursor-moved",
			     G_OBJECT_CLASS_TYPE(klass),
			     G_SIGNAL_RUN_LAST,
			     G_STRUCT_OFFSET(VteTerminalClass, cursor_moved),
			     NULL,
			     NULL,
			     _vte_marshal_VOID__VOID,
			     G_TYPE_NONE, 0);
	klass->deiconify_window_signal =
		g_signal_new("deiconify-window",
			     G_OBJECT_CLASS_TYPE(klass),
			     G_SIGNAL_RUN_LAST,
			     G_STRUCT_OFFSET(VteTerminalClass, deiconify_window),
			     NULL,
			     NULL,
			     _vte_marshal_VOID__VOID,
			     G_TYPE_NONE, 0);
	klass->iconify_window_signal =
		g_signal_new("iconify-window",
			     G_OBJECT_CLASS_TYPE(klass),
			     G_SIGNAL_RUN_LAST,
			     G_STRUCT_OFFSET(VteTerminalClass, iconify_window),
			     NULL,
			     NULL,
			     _vte_marshal_VOID__VOID,
			     G_TYPE_NONE, 0);
	klass->raise_window_signal =
		g_signal_new("raise-window",
			     G_OBJECT_CLASS_TYPE(klass),
			     G_SIGNAL_RUN_LAST,
			     G_STRUCT_OFFSET(VteTerminalClass, raise_window),
			     NULL,
			     NULL,
			     _vte_marshal_VOID__VOID,
			     G_TYPE_NONE, 0);
	klass->lower_window_signal =
		g_signal_new("lower-window",
			     G_OBJECT_CLASS_TYPE(klass),
			     G_SIGNAL_RUN_LAST,
			     G_STRUCT_OFFSET(VteTerminalClass, lower_window),
			     NULL,
			     NULL,
			     _vte_marshal_VOID__VOID,
			     G_TYPE_NONE, 0);
	klass->refresh_window_signal =
		g_signal_new("refresh-window",
			     G_OBJECT_CLASS_TYPE(klass),
			     G_SIGNAL_RUN_LAST,
			     G_STRUCT_OFFSET(VteTerminalClass, refresh_window),
			     NULL,
			     NULL,
			     _vte_marshal_VOID__VOID,
			     G_TYPE_NONE, 0);
	klass->restore_window_signal =
		g_signal_new("restore-window",
			     G_OBJECT_CLASS_TYPE(klass),
			     G_SIGNAL_RUN_LAST,
			     G_STRUCT_OFFSET(VteTerminalClass, restore_window),
			     NULL,
			     NULL,
			     _vte_marshal_VOID__VOID,
			     G_TYPE_NONE, 0);
	klass->maximize_window_signal =
		g_signal_new("maximize-window",
			     G_OBJECT_CLASS_TYPE(klass),
			     G_SIGNAL_RUN_LAST,
			     G_STRUCT_OFFSET(VteTerminalClass, maximize_window),
			     NULL,
			     NULL,
			     _vte_marshal_VOID__VOID,
			     G_TYPE_NONE, 0);
	klass->resize_window_signal =
		g_signal_new("resize-window",
			     G_OBJECT_CLASS_TYPE(klass),
			     G_SIGNAL_RUN_LAST,
			     G_STRUCT_OFFSET(VteTerminalClass, resize_window),
			     NULL,
			     NULL,
			     _vte_marshal_VOID__UINT_UINT,
			     G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_UINT);
	klass->move_window_signal =
		g_signal_new("move-window",
			     G_OBJECT_CLASS_TYPE(klass),
			     G_SIGNAL_RUN_LAST,
			     G_STRUCT_OFFSET(VteTerminalClass, move_window),
			     NULL,
			     NULL,
			     _vte_marshal_VOID__UINT_UINT,
			     G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_UINT);
	klass->status_line_changed_signal =
		g_signal_new("status-line-changed",
			     G_OBJECT_CLASS_TYPE(klass),
			     G_SIGNAL_RUN_LAST,
			     G_STRUCT_OFFSET(VteTerminalClass, status_line_changed),
			     NULL,
			     NULL,
			     _vte_marshal_VOID__VOID,
			     G_TYPE_NONE, 0);
	klass->increase_font_size_signal =
		g_signal_new("increase-font-size",
			     G_OBJECT_CLASS_TYPE(klass),
			     G_SIGNAL_RUN_LAST,
			     G_STRUCT_OFFSET(VteTerminalClass, increase_font_size),
			     NULL,
			     NULL,
			     _vte_marshal_VOID__VOID,
			     G_TYPE_NONE, 0);
	klass->decrease_font_size_signal =
		g_signal_new("decrease-font-size",
			     G_OBJECT_CLASS_TYPE(klass),
			     G_SIGNAL_RUN_LAST,
			     G_STRUCT_OFFSET(VteTerminalClass, decrease_font_size),
			     NULL,
			     NULL,
			     _vte_marshal_VOID__VOID,
			     G_TYPE_NONE, 0);
	klass->text_modified_signal =
		g_signal_new("text-modified",
			     G_OBJECT_CLASS_TYPE(klass),
			     G_SIGNAL_RUN_LAST,
			     G_STRUCT_OFFSET(VteTerminalClass, text_modified),
			     NULL,
			     NULL,
			     _vte_marshal_VOID__VOID,
			     G_TYPE_NONE, 0);
	klass->text_inserted_signal =
		g_signal_new("text-inserted",
			     G_OBJECT_CLASS_TYPE(klass),
			     G_SIGNAL_RUN_LAST,
			     G_STRUCT_OFFSET(VteTerminalClass, text_inserted),
			     NULL,
			     NULL,
			     _vte_marshal_VOID__VOID,
			     G_TYPE_NONE, 0);
	klass->text_deleted_signal =
		g_signal_new("text-deleted",
			     G_OBJECT_CLASS_TYPE(klass),
			     G_SIGNAL_RUN_LAST,
			     G_STRUCT_OFFSET(VteTerminalClass, text_deleted),
			     NULL,
			     NULL,
			     _vte_marshal_VOID__VOID,
			     G_TYPE_NONE, 0);
	klass->text_scrolled_signal =
		g_signal_new("text-scrolled",
			     G_OBJECT_CLASS_TYPE(klass),
			     G_SIGNAL_RUN_LAST,
			     G_STRUCT_OFFSET(VteTerminalClass, text_scrolled),
			     NULL,
			     NULL,
			     _vte_marshal_VOID__INT,
			     G_TYPE_NONE, 1, G_TYPE_INT);

#ifdef VTE_DEBUG
	/* Turn on debugging if we were asked to. */
	if (getenv("VTE_DEBUG_FLAGS") != NULL) {
		_vte_debug_parse_string(getenv("VTE_DEBUG_FLAGS"));
	}
#endif
}

GtkType
vte_terminal_erase_binding_get_type(void)
{
	static GtkType terminal_erase_binding_type = 0;
	static GEnumValue values[] = {
		{VTE_ERASE_AUTO, "VTE_ERASE_AUTO", "auto"},
		{VTE_ERASE_ASCII_BACKSPACE, "VTE_ERASE_ASCII_BACKSPACE",
		 "ascii-backspace"},
		{VTE_ERASE_ASCII_DELETE, "VTE_ERASE_ASCII_DELETE",
		 "ascii-delete"},
		{VTE_ERASE_DELETE_SEQUENCE, "VTE_ERASE_DELETE_SEQUENCE",
		 "delete-sequence"},
	};
	if (terminal_erase_binding_type == 0) {
		terminal_erase_binding_type =
			g_enum_register_static("VteTerminalEraseBinding",
					       values);
	}
	return terminal_erase_binding_type;
}

GtkType
vte_terminal_anti_alias_get_type(void)
{
	static GtkType terminal_anti_alias_type = 0;
	static GEnumValue values[] = {
		{VTE_ANTI_ALIAS_USE_DEFAULT, "VTE_ANTI_ALIAS_USE_DEFAULT", "use-default"},
		{VTE_ANTI_ALIAS_FORCE_ENABLE, "VTE_ANTI_ALIAS_FORCE_ENABLE", "force-enable"},
		{VTE_ANTI_ALIAS_FORCE_DISABLE, "VTE_ANTI_ALIAS_FORCE_DISABLE", "force-disable"},
	};
	if (terminal_anti_alias_type == 0) {
		terminal_anti_alias_type =
			g_enum_register_static("VteTerminalAntiAlias",
					       values);
	}
	return terminal_anti_alias_type;
}

GtkType
vte_terminal_get_type(void)
{
	static GtkType terminal_type = 0;
	static const GTypeInfo terminal_info = {
		sizeof(VteTerminalClass),
		(GBaseInitFunc)NULL,
		(GBaseFinalizeFunc)NULL,

		(GClassInitFunc)vte_terminal_class_init,
		(GClassFinalizeFunc)NULL,
		(gconstpointer)NULL,

		sizeof(VteTerminal),
		0,
		(GInstanceInitFunc)vte_terminal_init,

		(GTypeValueTable*)NULL,
	};

	if (terminal_type == 0) {
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_LIFECYCLE)) {
			fprintf(stderr, "vte_terminal_get_type()\n");
		}
#endif
		terminal_type = g_type_register_static(GTK_TYPE_WIDGET,
						       "VteTerminal",
						       &terminal_info,
						       0);
	}

	return terminal_type;
}

/**
 * vte_terminal_set_audible_bell:
 * @terminal: a #VteTerminal
 * @is_audible: TRUE if the terminal should beep
 *
 * Controls whether or not the terminal will beep when the child outputs the
 * "bl" sequence.
 *
 */
void
vte_terminal_set_audible_bell(VteTerminal *terminal, gboolean is_audible)
{
	g_return_if_fail(VTE_IS_TERMINAL(terminal));
	terminal->pvt->audible_bell = is_audible;
}

/**
 * vte_terminal_get_audible_bell:
 * @terminal: a #VteTerminal
 *
 * Checks whether or not the terminal will beep when the child outputs the
 * "bl" sequence.
 *
 * Returns: TRUE if audible bell is enabled, FALSE if not
 */
gboolean
vte_terminal_get_audible_bell(VteTerminal *terminal)
{
	g_return_val_if_fail(VTE_IS_TERMINAL(terminal), FALSE);
	return terminal->pvt->audible_bell;
}

/**
 * vte_terminal_set_visible_bell:
 * @terminal: a #VteTerminal
 * @is_visible: TRUE if the terminal should flash
 *
 * Controls whether or not the terminal will present a visible bell to the
 * user when the child outputs the "bl" sequence.  The terminal
 * will clear itself to the default foreground color and then repaint itself.
 *
 */
void
vte_terminal_set_visible_bell(VteTerminal *terminal, gboolean is_visible)
{
	g_return_if_fail(VTE_IS_TERMINAL(terminal));
	terminal->pvt->visible_bell = is_visible;
}

/**
 * vte_terminal_get_visible_bell:
 * @terminal: a #VteTerminal
 *
 * Checks whether or not the terminal will present a visible bell to the
 * user when the child outputs the "bl" sequence.  The terminal
 * will clear itself to the default foreground color and then repaint itself.
 *
 * Returns: TRUE if visible bell is enabled, FALSE if not
 */
gboolean
vte_terminal_get_visible_bell(VteTerminal *terminal)
{
	g_return_val_if_fail(VTE_IS_TERMINAL(terminal), FALSE);
	return terminal->pvt->visible_bell;
}

/**
 * vte_terminal_set_allow_bold:
 * @terminal: a #VteTerminal
 * @allow_bold: TRUE if the terminal should attempt to draw bold text
 *
 * Controls whether or not the terminal will attempt to draw bold text by
 * repainting text with a different offset.
 *
 */
void
vte_terminal_set_allow_bold(VteTerminal *terminal, gboolean allow_bold)
{
	g_return_if_fail(VTE_IS_TERMINAL(terminal));
	terminal->pvt->allow_bold = allow_bold;
}

/**
 * vte_terminal_get_allow_bold:
 * @terminal: a #VteTerminal
 *
 * Checks whether or not the terminal will attempt to draw bold text by
 * repainting text with a one-pixel offset.
 *
 * Returns: TRUE if bolding is enabled, FALSE if not
 */
gboolean
vte_terminal_get_allow_bold(VteTerminal *terminal)
{
	g_return_val_if_fail(VTE_IS_TERMINAL(terminal), FALSE);
	return terminal->pvt->allow_bold;
}

/**
 * vte_terminal_set_scroll_background:
 * @terminal: a #VteTerminal
 * @scroll: TRUE if the terminal should scroll the background image along with
 * text.
 *
 * Controls whether or not the terminal will scroll the background image (if
 * one is set) when the text in the window must be scrolled.
 *
 * Since: 0.11
 *
 */
void
vte_terminal_set_scroll_background(VteTerminal *terminal, gboolean scroll)
{
	g_return_if_fail(VTE_IS_TERMINAL(terminal));
	terminal->pvt->scroll_background = scroll;
}

/**
 * vte_terminal_set_scroll_on_output:
 * @terminal: a #VteTerminal
 * @scroll: TRUE if the terminal should scroll on output
 *
 * Controls whether or not the terminal will forcibly scroll to the bottom of
 * the viewable history when the new data is received from the child.
 *
 */
void
vte_terminal_set_scroll_on_output(VteTerminal *terminal, gboolean scroll)
{
	g_return_if_fail(VTE_IS_TERMINAL(terminal));
	terminal->pvt->scroll_on_output = scroll;
}

/**
 * vte_terminal_set_scroll_on_keystroke:
 * @terminal: a #VteTerminal
 * @scroll: TRUE if the terminal should scroll on keystrokes
 *
 * Controls whether or not the terminal will forcibly scroll to the bottom of
 * the viewable history when the user presses a key.  Modifier keys do not
 * trigger this behavior.
 *
 */
void
vte_terminal_set_scroll_on_keystroke(VteTerminal *terminal, gboolean scroll)
{
	g_return_if_fail(VTE_IS_TERMINAL(terminal));
	terminal->pvt->scroll_on_keystroke = scroll;
}

/**
 * vte_terminal_copy_clipboard:
 * @terminal: a #VteTerminal
 *
 * Places the selected text in the terminal in the #GDK_SELECTION_CLIPBOARD
 * selection.
 *
 */
void
vte_terminal_copy_clipboard(VteTerminal *terminal)
{
	g_return_if_fail(VTE_IS_TERMINAL(terminal));
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_SELECTION)) {
		fprintf(stderr, "Copying to CLIPBOARD.\n");
	}
#endif
	if (terminal->pvt->selection != NULL) {
		GtkClipboard *clipboard;
		clipboard = vte_terminal_clipboard_get(terminal,
						       GDK_SELECTION_CLIPBOARD);
		gtk_clipboard_set_text(clipboard, terminal->pvt->selection, -1);
	}
}

/**
 * vte_terminal_paste_clipboard:
 * @terminal: a #VteTerminal
 *
 * Sends the contents of the #GDK_SELECTION_CLIPBOARD selection to the
 * terminal's child.  If necessary, the data is converted from UTF-8 to the
 * terminal's current encoding. It's called on paste menu item, or when
 * user presses Shift+Insert.
 */
void
vte_terminal_paste_clipboard(VteTerminal *terminal)
{
	g_return_if_fail(VTE_IS_TERMINAL(terminal));
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_SELECTION)) {
		fprintf(stderr, "Pasting CLIPBOARD.\n");
	}
#endif
	vte_terminal_paste(terminal, GDK_SELECTION_CLIPBOARD);
}

/**
 * vte_terminal_copy_primary:
 * @terminal: a #VteTerminal
 *
 * Places the selected text in the terminal in the #GDK_SELECTION_PRIMARY
 * selection.
 *
 */
void
vte_terminal_copy_primary(VteTerminal *terminal)
{
	g_return_if_fail(VTE_IS_TERMINAL(terminal));
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_SELECTION)) {
		fprintf(stderr, "Copying to PRIMARY.\n");
	}
#endif
	vte_terminal_copy(terminal, GDK_SELECTION_PRIMARY);
}

/**
 * vte_terminal_paste_primary:
 * @terminal: a #VteTerminal
 *
 * Sends the contents of the #GDK_SELECTION_PRIMARY selection to the terminal's
 * child.  If necessary, the data is converted from UTF-8 to the terminal's
 * current encoding.  The terminal will call also paste the
 * #GDK_SELECTION_PRIMARY selection when the user clicks with the the second
 * mouse button.
 *
 */
void
vte_terminal_paste_primary(VteTerminal *terminal)
{
	g_return_if_fail(VTE_IS_TERMINAL(terminal));
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_SELECTION)) {
		fprintf(stderr, "Pasting PRIMARY.\n");
	}
#endif
	vte_terminal_paste(terminal, GDK_SELECTION_PRIMARY);
}

/**
 * vte_terminal_im_append_menuitems:
 * @terminal: a #VteTerminal
 * @menushell: a GtkMenuShell
 *
 * Appends menu items for various input methods to the given menu.  The
 * user can select one of these items to modify the input method used by
 * the terminal.
 *
 */
void
vte_terminal_im_append_menuitems(VteTerminal *terminal, GtkMenuShell *menushell)
{
	GtkIMMulticontext *context;
	g_return_if_fail(VTE_IS_TERMINAL(terminal));
	g_return_if_fail(GTK_WIDGET_REALIZED(GTK_WIDGET((terminal))));
	context = GTK_IM_MULTICONTEXT(terminal->pvt->im_context);
	gtk_im_multicontext_append_menuitems(context, menushell);
}

/* Set up whatever background we wanted. */
static gboolean
vte_terminal_background_update(gpointer data)
{
	VteTerminal *terminal;
	GtkWidget *widget;
	GdkColormap *colormap;
	GdkColor bgcolor;
	double saturation;

	g_assert(VTE_IS_TERMINAL(data));
	widget = GTK_WIDGET(data);
	terminal = VTE_TERMINAL(data);

	/* If we're not realized yet, don't worry about it, because we get
	 * called when we realize. */
	if (!GTK_WIDGET_REALIZED(widget)) {
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_MISC)) {
			fprintf(stderr, "Can not set background image without "
				"window.\n");
		}
#endif
		return TRUE;
	}

#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_MISC) || _vte_debug_on(VTE_DEBUG_EVENTS)) {
		fprintf(stderr, "Updating background image.\n");
	}
#endif

	/* Set the default background color. */
	bgcolor.red = terminal->pvt->palette[VTE_DEF_BG].red;
	bgcolor.green = terminal->pvt->palette[VTE_DEF_BG].green;
	bgcolor.blue = terminal->pvt->palette[VTE_DEF_BG].blue;
	bgcolor.pixel = 0;
	gtk_widget_ensure_style(widget);
	colormap = gdk_gc_get_colormap(widget->style->fg_gc[GTK_WIDGET_STATE(widget)]);
	if (colormap) {
		gdk_rgb_find_color(colormap, &bgcolor);
	}
	gdk_window_set_background(widget->window, &bgcolor);
	_vte_draw_set_background_color(terminal->pvt->draw, &bgcolor);

	/* If we're transparent, and either have no root image or are being
	 * told to update it, get a new copy of the root window. */
	saturation = terminal->pvt->bg_saturation * 1.0;
	saturation /= VTE_SATURATION_MAX;
	if (terminal->pvt->bg_transparent) {
		_vte_draw_set_background_image(terminal->pvt->draw,
					       VTE_BG_SOURCE_ROOT,
					       NULL,
					       NULL,
					       &terminal->pvt->bg_tint_color,
					       saturation);
	} else
	if (terminal->pvt->bg_file) {
		_vte_draw_set_background_image(terminal->pvt->draw,
					       VTE_BG_SOURCE_FILE,
					       NULL,
					       terminal->pvt->bg_file,
					       &terminal->pvt->bg_tint_color,
					       saturation);
	} else
	if (GDK_IS_PIXBUF(terminal->pvt->bg_pixbuf)) {
		_vte_draw_set_background_image(terminal->pvt->draw,
					       VTE_BG_SOURCE_PIXBUF,
					       terminal->pvt->bg_pixbuf,
					       NULL,
					       &terminal->pvt->bg_tint_color,
					       saturation);
	} else {
		_vte_draw_set_background_image(terminal->pvt->draw,
					       VTE_BG_SOURCE_NONE,
					       NULL,
					       NULL,
					       &terminal->pvt->bg_tint_color,
					       saturation);
	}

	/* Note that the update has finished. */
	if (terminal->pvt->bg_update_pending) {
		terminal->pvt->bg_update_pending = FALSE;
		g_source_remove(terminal->pvt->bg_update_tag);
		terminal->pvt->bg_update_tag = VTE_INVALID_SOURCE;
	}

	/* Force a redraw for everything. */
	_vte_invalidate_all(terminal);

	return FALSE;
}

/* Queue an update of the background image, to be done as soon as we can
 * get to it.  Just bail if there's already an update pending, so that if
 * opaque move tables to screw us, we don't end up with an insane backlog
 * of updates after the user finishes moving us. */
static void
vte_terminal_queue_background_update(VteTerminal *terminal)
{
	if (!terminal->pvt->bg_update_pending) {
		terminal->pvt->bg_update_pending = TRUE;
		terminal->pvt->bg_update_tag =
				g_idle_add_full(VTE_FX_PRIORITY,
						vte_terminal_background_update,
						terminal,
						NULL);
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_EVENTS)) {
			fprintf(stderr, "Queued background update.\n");
		}
#endif
	} else {
#ifdef VTE_DEBUG
		if (_vte_debug_on(VTE_DEBUG_EVENTS)) {
			fprintf(stderr, "Skipping background update.\n");
		}
#endif
	}
}

/**
 * vte_terminal_set_background_saturation:
 * @terminal: a #VteTerminal
 * @saturation: a floating point value between 0.0 and 1.0.
 *
 * If a background image has been set using
 * vte_terminal_set_background_image(),
 * vte_terminal_set_background_image_file(), or
 * vte_terminal_set_background_transparent(), and the saturation value is less
 * than 1.0, the terminal will adjust the colors of the image before drawing
 * the image.  To do so, the terminal will create a copy of the background
 * image (or snapshot of the root window) and modify its pixel values.
 *
 */
void
vte_terminal_set_background_saturation(VteTerminal *terminal, double saturation)
{
	g_return_if_fail(VTE_IS_TERMINAL(terminal));
	terminal->pvt->bg_saturation = CLAMP(saturation * VTE_SATURATION_MAX,
					     0, VTE_SATURATION_MAX);
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_MISC)) {
		fprintf(stderr, "Setting background saturation to %ld/%ld.\n",
			terminal->pvt->bg_saturation,
			(long) VTE_SATURATION_MAX);
	}
#endif
	vte_terminal_queue_background_update(terminal);
}

/**
 * vte_terminal_set_background_tint_color:
 * @terminal: a #VteTerminal
 * @color: a color which the terminal background should be tinted to if its
 * saturation is not 1.0.
 *
 * If a background image has been set using
 * vte_terminal_set_background_image(),
 * vte_terminal_set_background_image_file(), or
 * vte_terminal_set_background_transparent(), and the value set by
 * vte_terminal_set_background_saturation() is less than one, the terminal
 * will adjust the color of the image before drawing the image.  To do so,
 * the terminal will create a copy of the background image (or snapshot of
 * the root window) and modify its pixel values.  The initial tint color
 * is black.
 *
 * Since: 0.11
 *
 */
void
vte_terminal_set_background_tint_color(VteTerminal *terminal,
				       const GdkColor *color)
{
	g_return_if_fail(VTE_IS_TERMINAL(terminal));
	g_return_if_fail(color != NULL);
	terminal->pvt->bg_tint_color = *color;
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_MISC)) {
		fprintf(stderr, "Setting background tint to %d,%d,%d.\n",
			terminal->pvt->bg_tint_color.red >> 8,
			terminal->pvt->bg_tint_color.green >> 8,
			terminal->pvt->bg_tint_color.blue >> 8);
	}
#endif
	vte_terminal_queue_background_update(terminal);
}

/**
 * vte_terminal_set_background_transparent:
 * @terminal: a #VteTerminal
 * @transparent: TRUE if the terminal should fake transparency
 *
 * Sets the terminal's background image to the pixmap stored in the root
 * window, adjusted so that if there are no windows below your application,
 * the widget will appear to be transparent.
 *
 */
void
vte_terminal_set_background_transparent(VteTerminal *terminal,
					gboolean transparent)
{
	g_return_if_fail(VTE_IS_TERMINAL(terminal));
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_MISC)) {
		fprintf(stderr, "Turning background transparency %s.\n",
			transparent ? "on" : "off");
	}
#endif
	/* Save this background type. */
	terminal->pvt->bg_transparent = transparent;

	/* Update the background. */
	vte_terminal_queue_background_update(terminal);
}

/**
 * vte_terminal_set_background_image:
 * @terminal: a #VteTerminal
 * @image: a #GdkPixbuf to use, or #NULL to cancel
 *
 * Sets a background image for the widget.  Text which would otherwise be
 * drawn using the default background color will instead be drawn over the
 * specified image.  If necessary, the image will be tiled to cover the
 * widget's entire visible area. If specified by
 * vte_terminal_set_background_saturation(), the terminal will tint its
 * in-memory copy of the image before applying it to the terminal.
 *
 */
void
vte_terminal_set_background_image(VteTerminal *terminal, GdkPixbuf *image)
{
	g_return_if_fail(VTE_IS_TERMINAL(terminal));

#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_MISC)) {
		fprintf(stderr, "%s background image.\n",
			GDK_IS_PIXBUF(image) ? "Setting" : "Clearing");
	}
#endif

	/* Get a ref to the new image if there is one.  Do it here just in
	 * case we're actually given the same one we're already using. */
	if (GDK_IS_PIXBUF(image)) {
		g_object_ref(G_OBJECT(image));
	}

	/* Unref the previous background image. */
	if (GDK_IS_PIXBUF(terminal->pvt->bg_pixbuf)) {
		g_object_unref(G_OBJECT(terminal->pvt->bg_pixbuf));
	}

	/* Clear a background file name, if one was set. */
	if (terminal->pvt->bg_file) {
		g_free(terminal->pvt->bg_file);
	}
	terminal->pvt->bg_file = NULL;

	/* Set the new background. */
	terminal->pvt->bg_pixbuf = image;

	vte_terminal_queue_background_update(terminal);
}

/**
 * vte_terminal_set_background_image_file:
 * @terminal: a #VteTerminal
 * @path: path to an image file
 *
 * Sets a background image for the widget.  If specified by
 * vte_terminal_set_background_saturation(), the terminal will tint its
 * in-memory copy of the image before applying it to the terminal.
 *
 */
void
vte_terminal_set_background_image_file(VteTerminal *terminal, const char *path)
{
	g_return_if_fail(VTE_IS_TERMINAL(terminal));
#ifdef VTE_DEBUG
	if (_vte_debug_on(VTE_DEBUG_MISC)) {
		fprintf(stderr, "Loading background image from `%s'.\n", path);
	}
#endif
	/* Save this background type. */
	if (terminal->pvt->bg_file) {
		g_free(terminal->pvt->bg_file);
	}
	terminal->pvt->bg_file = path ? g_strdup(path) : NULL;

	/* Turn off other background types. */
	if (GDK_IS_PIXBUF(terminal->pvt->bg_pixbuf)) {
		g_object_unref(G_OBJECT(terminal->pvt->bg_pixbuf));
		terminal->pvt->bg_pixbuf = NULL;
	}

	vte_terminal_queue_background_update(terminal);
}

/**
 * vte_terminal_get_has_selection:
 * @terminal: a #VteTerminal
 *
 * Checks if the terminal currently contains selected text.  Note that this
 * is different from determining if the terminal is the owner of any
 * #GtkClipboard items.
 *
 * Returns: TRUE if part of the text in the terminal is selected.
 */
gboolean
vte_terminal_get_has_selection(VteTerminal *terminal)
{
	g_return_val_if_fail(VTE_IS_TERMINAL(terminal), FALSE);
	return terminal->pvt->has_selection;
}

/**
 * vte_terminal_get_using_xft:
 * @terminal: a #VteTerminal
 *
 * A #VteTerminal can use multiple methods to draw text.  This function
 * allows an application to determine whether or not the current method uses
 * fontconfig to find fonts.  This setting cannot be changed by the caller,
 * but in practice usually matches the behavior of GTK+ itself.
 *
 * Returns: TRUE if the terminal is using fontconfig to find fonts, FALSE if
 * the terminal is using PangoX.
 */
gboolean
vte_terminal_get_using_xft(VteTerminal *terminal)
{
	g_return_val_if_fail(VTE_IS_TERMINAL(terminal), FALSE);
	return _vte_draw_get_using_fontconfig(terminal->pvt->draw);
}

/**
 * vte_terminal_set_cursor_blinks:
 * @terminal: a #VteTerminal
 * @blink: TRUE if the cursor should blink
 *
 * Sets whether or not the cursor will blink.  The length of the blinking cycle
 * is controlled by the "gtk-cursor-blink-time" GTK+ setting.
 *
 */
void
vte_terminal_set_cursor_blinks(VteTerminal *terminal, gboolean blink)
{
	g_return_if_fail(VTE_IS_TERMINAL(terminal));
	terminal->pvt->cursor_blinks = blink;
}

/**
 * vte_terminal_set_scrollback_lines:
 * @terminal: a #VteTerminal
 * @lines: the length of the history buffer
 *
 * Sets the length of the scrollback buffer used by the terminal.  The size of
 * the scrollback buffer will be set to the larger of this value and the number
 * of visible rows the widget can display, so 0 can safely be used to disable
 * scrollback.  Note that this setting only affects the normal screen buffer.
 * For terminal types which have an alternate screen buffer, no scrollback is
 * allowed on the alternate screen buffer.
 *
 */
void
vte_terminal_set_scrollback_lines(VteTerminal *terminal, glong lines)
{
	long highd, high, low, delta, max, next;
	VteScreen *screens[2];
	int i;

	g_return_if_fail(VTE_IS_TERMINAL(terminal));

	/* We require a minimum buffer size. */
	lines = MAX(lines, VTE_SCROLLBACK_MIN);
	lines = MAX(lines, terminal->row_count);

	/* We need to resize both scrollback buffers, and this beats copying
	 * and pasting the same code twice. */
	screens[0] = &terminal->pvt->normal_screen;
	screens[1] = &terminal->pvt->alternate_screen;

	/* We want to do the same thing to both screens, so we use a loop
	 * to avoid cut/paste madness. */
	for (i = 0; i < G_N_ELEMENTS(screens); i++) {
		/* The main screen gets the full scrollback buffer, but the
		 * alternate screen isn't allowed to scroll at all. */
		delta = _vte_ring_delta(screens[i]->row_data);
		max = _vte_ring_max(screens[i]->row_data);
		next = _vte_ring_next(screens[i]->row_data);
		if (screens[i] == &terminal->pvt->alternate_screen) {
			vte_terminal_reset_rowdata(&screens[i]->row_data,
						   terminal->row_count);
		} else {
			vte_terminal_reset_rowdata(&screens[i]->row_data,
						   lines);
		}
		/* Force the offsets to point to valid rows. */
		low = _vte_ring_delta(screens[i]->row_data);
		high = low + MAX(_vte_ring_max(screens[i]->row_data), 1);
		highd = high - terminal->row_count + 1;
		screens[i]->insert_delta = CLAMP(screens[i]->insert_delta,
						 low, highd);
		screens[i]->scroll_delta = CLAMP(screens[i]->scroll_delta,
						 low, highd);
		screens[i]->cursor_current.row = CLAMP(screens[i]->cursor_current.row,
						       low, high);
		/* Clear the matching view. */
		_vte_terminal_match_contents_clear(terminal);
		/* Notify viewers that the contents have changed. */
		_vte_terminal_emit_contents_changed(terminal);
	}
	terminal->pvt->scrollback_lines = lines;

	/* Adjust the scrollbars to the new locations. */
	_vte_terminal_adjust_adjustments(terminal, TRUE);
	_vte_invalidate_all(terminal);
}

/**
 * vte_terminal_set_word_chars:
 * @terminal: a #VteTerminal
 * @spec: a specification
 *
 * When the user double-clicks to start selection, the terminal will extend
 * the selection on word boundaries.  It will treat characters included in @spec
 * as parts of words, and all other characters as word separators.  Ranges of
 * characters can be specified by separating them with a hyphen.
 *
 * As a special case, if @spec is NULL or the empty string, the terminal will
 * treat all graphic non-punctuation non-space characters as word characters.
 */
void
vte_terminal_set_word_chars(VteTerminal *terminal, const char *spec)
{
	VteConv conv;
	gunichar *wbuf;
	char *ibuf, *ibufptr, *obuf, *obufptr;
	gsize ilen, olen;
	VteWordCharRange range;
	int i;

	g_return_if_fail(VTE_IS_TERMINAL(terminal));
	/* Allocate a new range array. */
	if (terminal->pvt->word_chars != NULL) {
		g_array_free(terminal->pvt->word_chars, TRUE);
	}
	terminal->pvt->word_chars = g_array_new(FALSE, TRUE,
						sizeof(VteWordCharRange));
	/* Special case: if spec is NULL, try to do the right thing. */
	if ((spec == NULL) || (strlen(spec) == 0)) {
		return;
	}
	/* Convert the spec from UTF-8 to a string of gunichars . */
	conv = _vte_conv_open(VTE_CONV_GUNICHAR_TYPE, "UTF-8");
	if (conv == ((VteConv) -1)) {
		/* Aaargh.  We're screwed. */
		g_warning(_("_vte_conv_open() failed setting word characters"));
		return;
	}
	ilen = strlen(spec);
	ibuf = ibufptr = g_strdup(spec);
	olen = (ilen + 1) * sizeof(gunichar);
	_vte_buffer_set_minimum_size(terminal->pvt->conv_buffer, olen);
	obuf = obufptr = terminal->pvt->conv_buffer->bytes;
	wbuf = (gunichar*) obuf;
	wbuf[ilen] = '\0';
	_vte_conv(conv, &ibuf, &ilen, &obuf, &olen);
	_vte_conv_close(conv);
	for (i = 0; i < ((obuf - obufptr) / sizeof(gunichar)); i++) {
		/* The hyphen character. */
		if (wbuf[i] == '-') {
			range.start = wbuf[i];
			range.end = wbuf[i];
			g_array_append_val(terminal->pvt->word_chars, range);
#ifdef VTE_DEBUG
			if (_vte_debug_on(VTE_DEBUG_MISC)) {
				fprintf(stderr, "Word charset includes hyphen.\n");
			}
#endif
			continue;
		}
		/* A single character, not the start of a range. */
		if ((wbuf[i] != '-') && (wbuf[i + 1] != '-')) {
			range.start = wbuf[i];
			range.end = wbuf[i];
			g_array_append_val(terminal->pvt->word_chars, range);
#ifdef VTE_DEBUG
			if (_vte_debug_on(VTE_DEBUG_MISC)) {
				fprintf(stderr, "Word charset includes `%lc'.\n",
					(wint_t) wbuf[i]);
			}
#endif
			continue;
		}
		/* The start of a range. */
		if ((wbuf[i] != '-') &&
		    (wbuf[i + 1] == '-') &&
		    (wbuf[i + 2] != '-') &&
		    (wbuf[i + 2] != 0)) {
			range.start = wbuf[i];
			range.end = wbuf[i + 2];
			g_array_append_val(terminal->pvt->word_chars, range);
#ifdef VTE_DEBUG
			if (_vte_debug_on(VTE_DEBUG_MISC)) {
				fprintf(stderr, "Word charset includes range from "
					"`%lc' to `%lc'.\n", (wint_t) wbuf[i],
					(wint_t) wbuf[i + 2]);
			}
#endif
			i += 2;
			continue;
		}
	}
	g_free(ibufptr);
}

/**
 * vte_terminal_set_backspace_binding:
 * @terminal: a #VteTerminal
 * @binding: a #VteTerminalEraseBinding for the backspace key
 *
 * Modifies the terminal's backspace key binding, which controls what
 * string or control sequence the terminal sends to its child when the user
 * presses the backspace key.
 *
 */
void
vte_terminal_set_backspace_binding(VteTerminal *terminal,
				   VteTerminalEraseBinding binding)
{
	g_return_if_fail(VTE_IS_TERMINAL(terminal));
	/* FIXME: should we set the pty mode to match? */
	terminal->pvt->backspace_binding = binding;
}

/**
 * vte_terminal_set_delete_binding:
 * @terminal: a #VteTerminal
 * @binding: a #VteTerminalEraseBinding for the delete key
 *
 * Modifies the terminal's delete key binding, which controls what
 * string or control sequence the terminal sends to its child when the user
 * presses the delete key.
 *
 */
void
vte_terminal_set_delete_binding(VteTerminal *terminal,
				VteTerminalEraseBinding binding)
{
	g_return_if_fail(VTE_IS_TERMINAL(terminal));
	terminal->pvt->delete_binding = binding;
}

/**
 * vte_terminal_set_mouse_autohide:
 * @terminal: a #VteTerminal
 * @setting: TRUE if the autohide should be enabled
 *
 * Changes the value of the terminal's mouse autohide setting.  When autohiding
 * is enabled, the mouse cursor will be hidden when the user presses a key and
 * shown when the user moves the mouse.  This setting can be read using
 * vte_terminal_get_mouse_autohide().
 *
 */
void
vte_terminal_set_mouse_autohide(VteTerminal *terminal, gboolean setting)
{
	g_return_if_fail(VTE_IS_TERMINAL(terminal));
	terminal->pvt->mouse_autohide = setting;
}

/**
 * vte_terminal_get_mouse_autohide:
 * @terminal: a #VteTerminal
 *
 * Determines the value of the terminal's mouse autohide setting.  When
 * autohiding is enabled, the mouse cursor will be hidden when the user presses
 * a key and shown when the user moves the mouse.  This setting can be changed
 * using vte_terminal_set_mouse_autohide().
 *
 * Returns: TRUE if autohiding is enabled, FALSE if not.
 */
gboolean
vte_terminal_get_mouse_autohide(VteTerminal *terminal)
{
	g_return_val_if_fail(VTE_IS_TERMINAL(terminal), FALSE);
	return terminal->pvt->mouse_autohide;
}

/**
 * vte_terminal_reset:
 * @terminal: a #VteTerminal
 * @full: TRUE to reset tabstops
 * @clear_history: TRUE to empty the terminal's scrollback buffer
 *
 * Resets as much of the terminal's internal state as possible, discarding any
 * unprocessed input data, resetting character attributes, cursor state,
 * national character set state, status line, terminal modes (insert/delete),
 * selection state, and encoding.
 *
 */
void
vte_terminal_reset(VteTerminal *terminal, gboolean full, gboolean clear_history)
{
	g_return_if_fail(VTE_IS_TERMINAL(terminal));
	/* Stop processing any of the data we've got backed up. */
	vte_terminal_stop_processing (terminal);

	/* Clear the input and output buffers. */
	if (terminal->pvt->incoming != NULL) {
		_vte_buffer_clear(terminal->pvt->incoming);
	}
	if (terminal->pvt->pending != NULL) {
		g_array_set_size(terminal->pvt->pending, 0);
	}
	if (terminal->pvt->outgoing != NULL) {
		_vte_buffer_clear(terminal->pvt->outgoing);
	}
	/* Reset charset substitution state. */
	if (terminal->pvt->iso2022 != NULL) {
		_vte_iso2022_state_free(terminal->pvt->iso2022);
	}
	terminal->pvt->iso2022 = _vte_iso2022_state_new(NULL,
							&_vte_terminal_codeset_changed_cb,
							(gpointer)terminal);
	_vte_iso2022_state_set_codeset(terminal->pvt->iso2022,
				       terminal->pvt->encoding);
	/* Reset keypad/cursor/function key modes. */
	terminal->pvt->keypad_mode = VTE_KEYMODE_NORMAL;
	terminal->pvt->cursor_mode = VTE_KEYMODE_NORMAL;
	terminal->pvt->sun_fkey_mode = FALSE;
	terminal->pvt->hp_fkey_mode = FALSE;
	terminal->pvt->legacy_fkey_mode = FALSE;
	terminal->pvt->vt220_fkey_mode = FALSE;
	/* Enable meta-sends-escape. */
	terminal->pvt->meta_sends_escape = TRUE;
	/* Disable smooth scroll. */
	terminal->pvt->smooth_scroll = FALSE;
	/* Disable margin bell. */
	terminal->pvt->margin_bell = FALSE;
	/* Enable iso2022/NRC processing. */
	terminal->pvt->nrc_mode = TRUE;
	/* Reset saved settings. */
	if (terminal->pvt->dec_saved != NULL) {
		g_hash_table_destroy(terminal->pvt->dec_saved);
		terminal->pvt->dec_saved = g_hash_table_new(g_direct_hash,
							    g_direct_equal);
	}
	/* Reset the color palette. */
	/* vte_terminal_set_default_colors(terminal); */
	/* Reset the default attributes.  Reset the alternate attribute because
	 * it's not a real attribute, but we need to treat it as one here. */
	terminal->pvt->screen = &terminal->pvt->alternate_screen;
	_vte_terminal_set_default_attributes(terminal);
	terminal->pvt->screen->defaults.alternate = FALSE;
	terminal->pvt->screen = &terminal->pvt->normal_screen;
	_vte_terminal_set_default_attributes(terminal);
	terminal->pvt->screen->defaults.alternate = FALSE;
	/* Clear the scrollback buffers and reset the cursors. */
	if (clear_history) {
		_vte_ring_free(terminal->pvt->normal_screen.row_data, TRUE);
		terminal->pvt->normal_screen.row_data =
			_vte_ring_new(terminal->pvt->scrollback_lines,
				      vte_free_row_data, NULL);
		_vte_ring_free(terminal->pvt->alternate_screen.row_data, TRUE);
		terminal->pvt->alternate_screen.row_data =
			_vte_ring_new(terminal->pvt->scrollback_lines,
				      vte_free_row_data, NULL);
		terminal->pvt->normal_screen.cursor_saved.row = 0;
		terminal->pvt->normal_screen.cursor_saved.col = 0;
		terminal->pvt->normal_screen.cursor_current.row = 0;
		terminal->pvt->normal_screen.cursor_current.col = 0;
		terminal->pvt->normal_screen.scroll_delta = 0;
		terminal->pvt->normal_screen.insert_delta = 0;
		terminal->pvt->alternate_screen.cursor_saved.row = 0;
		terminal->pvt->alternate_screen.cursor_saved.col = 0;
		terminal->pvt->alternate_screen.cursor_current.row = 0;
		terminal->pvt->alternate_screen.cursor_current.col = 0;
		terminal->pvt->alternate_screen.scroll_delta = 0;
		terminal->pvt->alternate_screen.insert_delta = 0;
		_vte_terminal_adjust_adjustments(terminal, TRUE);
	}
	/* Clear the status lines. */
	terminal->pvt->normal_screen.status_line = FALSE;
	if (terminal->pvt->normal_screen.status_line_contents != NULL) {
		g_string_free(terminal->pvt->normal_screen.status_line_contents,
			      TRUE);
	}
	terminal->pvt->normal_screen.status_line_contents = g_string_new("");
	terminal->pvt->alternate_screen.status_line = FALSE;
	if (terminal->pvt->alternate_screen.status_line_contents != NULL) {
		g_string_free(terminal->pvt->alternate_screen.status_line_contents,
			      TRUE);
	}
	terminal->pvt->alternate_screen.status_line_contents = g_string_new("");
	/* Do more stuff we refer to as a "full" reset. */
	if (full) {
		vte_terminal_set_default_tabstops(terminal);
	}
	/* Reset restricted scrolling regions, leave insert mode, make
	 * the cursor visible again. */
	terminal->pvt->normal_screen.scrolling_restricted = FALSE;
	terminal->pvt->normal_screen.sendrecv_mode = TRUE;
	terminal->pvt->normal_screen.insert_mode = FALSE;
	terminal->pvt->normal_screen.linefeed_mode = FALSE;
	terminal->pvt->normal_screen.origin_mode = FALSE;
	terminal->pvt->normal_screen.reverse_mode = FALSE;
	terminal->pvt->alternate_screen.scrolling_restricted = FALSE;
	terminal->pvt->alternate_screen.sendrecv_mode = TRUE;
	terminal->pvt->alternate_screen.insert_mode = FALSE;
	terminal->pvt->alternate_screen.linefeed_mode = FALSE;
	terminal->pvt->alternate_screen.origin_mode = FALSE;
	terminal->pvt->alternate_screen.reverse_mode = FALSE;
	terminal->pvt->cursor_visible = TRUE;
	/* Reset the encoding. */
	vte_terminal_set_encoding(terminal, NULL);
	g_assert(terminal->pvt->encoding != NULL);
	/* Reset selection. */
	vte_terminal_deselect_all(terminal);
	terminal->pvt->has_selection = FALSE;
	terminal->pvt->selecting = FALSE;
	terminal->pvt->selecting_restart = FALSE;
	terminal->pvt->selecting_had_delta = FALSE;
	if (terminal->pvt->selection != NULL) {
		g_free(terminal->pvt->selection);
		terminal->pvt->selection = NULL;
		memset(&terminal->pvt->selection_origin, 0,
		       sizeof(&terminal->pvt->selection_origin));
		memset(&terminal->pvt->selection_last, 0,
		       sizeof(&terminal->pvt->selection_last));
		memset(&terminal->pvt->selection_start, 0,
		       sizeof(&terminal->pvt->selection_start));
		memset(&terminal->pvt->selection_end, 0,
		       sizeof(&terminal->pvt->selection_end));
	}
	/* Reset mouse motion events. */
	terminal->pvt->mouse_send_xy_on_click = FALSE;
	terminal->pvt->mouse_send_xy_on_button = FALSE;
	terminal->pvt->mouse_hilite_tracking = FALSE;
	terminal->pvt->mouse_cell_motion_tracking = FALSE;
	terminal->pvt->mouse_all_motion_tracking = FALSE;
	terminal->pvt->mouse_last_button = 0;
	terminal->pvt->mouse_last_x = 0;
	terminal->pvt->mouse_last_y = 0;
	/* Clear modifiers. */
	terminal->pvt->modifiers = 0;
	/* Cause everything to be redrawn (or cleared). */
	vte_terminal_maybe_scroll_to_bottom(terminal);
	_vte_invalidate_all(terminal);
}

/**
 * vte_terminal_get_status_line:
 * @terminal: a #VteTerminal
 *
 * Some terminal emulations specify a status line which is separate from the
 * main display area, and define a means for applications to move the cursor
 * to the status line and back.
 *
 * Returns: the current contents of the terminal's status line.  For terminals
 * like "xterm", this will usually be the empty string.  The string must not
 * be modified or freed by the caller.
 */
const char *
vte_terminal_get_status_line(VteTerminal *terminal)
{
	g_return_val_if_fail(VTE_IS_TERMINAL(terminal), NULL);
	return terminal->pvt->screen->status_line_contents->str;
}

/**
 * vte_terminal_get_padding:
 * @terminal: a #VteTerminal
 * @xpad: address in which to store left/right-edge padding
 * @ypad: address in which to store top/bottom-edge ypadding
 *
 * Determines the amount of additional space the widget is using to pad the
 * edges of its visible area.  This is necessary for cases where characters in
 * the selected font don't themselves include a padding area and the text
 * itself would otherwise be contiguous with the window border.  Applications
 * which use the widget's #row_count, #column_count, #char_height, and
 * #char_width fields to set geometry hints using
 * gtk_window_set_geometry_hints() will need to add this value to the base
 * size.  The values returned in @xpad and @ypad are the total padding used in
 * each direction, and do not need to be doubled.
 *
 */
void
vte_terminal_get_padding(VteTerminal *terminal, int *xpad, int *ypad)
{
	g_return_if_fail(VTE_IS_TERMINAL(terminal));
	g_return_if_fail(xpad != NULL);
	g_return_if_fail(ypad != NULL);
	*xpad = 2 * VTE_PAD_WIDTH;
	*ypad = 2 * VTE_PAD_WIDTH;
}

/**
 * vte_terminal_get_adjustment:
 * @terminal: a #VteTerminal
 *
 * An accessor function provided for the benefit of language bindings.
 *
 * Returns: the contents of @terminal's adjustment field
 */
GtkAdjustment *
vte_terminal_get_adjustment(VteTerminal *terminal)
{
	g_return_val_if_fail(VTE_IS_TERMINAL(terminal), NULL);
	return terminal->adjustment;
}

/**
 * vte_terminal_get_char_width:
 * @terminal: a #VteTerminal
 *
 * An accessor function provided for the benefit of language bindings.
 *
 * Returns: the contents of @terminal's char_width field
 */
glong
vte_terminal_get_char_width(VteTerminal *terminal)
{
	g_return_val_if_fail(VTE_IS_TERMINAL(terminal), -1);
	return terminal->char_width;
}

/**
 * vte_terminal_get_char_height:
 * @terminal: a #VteTerminal
 *
 * An accessor function provided for the benefit of language bindings.
 *
 * Returns: the contents of @terminal's char_height field
 */
glong
vte_terminal_get_char_height(VteTerminal *terminal)
{
	g_return_val_if_fail(VTE_IS_TERMINAL(terminal), -1);
	return terminal->char_height;
}

/**
 * vte_terminal_get_char_descent:
 * @terminal: a #VteTerminal
 *
 * An accessor function provided for the benefit of language bindings.
 *
 * Returns: the contents of @terminal's char_descent field
 */
glong
vte_terminal_get_char_descent(VteTerminal *terminal)
{
	g_return_val_if_fail(VTE_IS_TERMINAL(terminal), -1);
	return terminal->char_descent;
}

/**
 * vte_terminal_get_char_ascent:
 * @terminal: a #VteTerminal
 *
 * An accessor function provided for the benefit of language bindings.
 *
 * Returns: the contents of @terminal's char_ascent field
 */
glong
vte_terminal_get_char_ascent(VteTerminal *terminal)
{
	g_return_val_if_fail(VTE_IS_TERMINAL(terminal), -1);
	return terminal->char_ascent;
}

/**
 * vte_terminal_get_row_count:
 * @terminal: a #VteTerminal
 *
 * An accessor function provided for the benefit of language bindings.
 *
 * Returns: the contents of @terminal's row_count field
 */
glong
vte_terminal_get_row_count(VteTerminal *terminal)
{
	g_return_val_if_fail(VTE_IS_TERMINAL(terminal), -1);
	return terminal->row_count;
}

/**
 * vte_terminal_get_column_count:
 * @terminal: a #VteTerminal
 *
 * An accessor function provided for the benefit of language bindings.
 *
 * Returns: the contents of @terminal's column_count field
 */
glong
vte_terminal_get_column_count(VteTerminal *terminal)
{
	g_return_val_if_fail(VTE_IS_TERMINAL(terminal), -1);
	return terminal->column_count;
}

/**
 * vte_terminal_get_window_title:
 * @terminal: a #VteTerminal
 *
 * An accessor function provided for the benefit of language bindings.
 *
 * Returns: the contents of @terminal's window_title field
 */
const char *
vte_terminal_get_window_title(VteTerminal *terminal)
{
	g_return_val_if_fail(VTE_IS_TERMINAL(terminal), "");
	return terminal->window_title;
}

/**
 * vte_terminal_get_icon_title:
 * @terminal: a #VteTerminal
 *
 * An accessor function provided for the benefit of language bindings.
 *
 * Returns: the contents of @terminal's icon_title field
 */
const char *
vte_terminal_get_icon_title(VteTerminal *terminal)
{
	g_return_val_if_fail(VTE_IS_TERMINAL(terminal), "");
	return terminal->icon_title;
}

/* We need this bit of glue to ensure that accessible objects will always
 * get signals. */
void
_vte_terminal_accessible_ref(VteTerminal *terminal)
{
	g_return_if_fail(VTE_IS_TERMINAL(terminal));
	terminal->pvt->accessible_emit = TRUE;
}

char *
_vte_terminal_get_selection(VteTerminal *terminal)
{
	g_return_val_if_fail(VTE_IS_TERMINAL(terminal), NULL);

	return g_strdup (terminal->pvt->selection);
}

void
_vte_terminal_get_start_selection(VteTerminal *terminal, long *x, long *y)
{
	struct selection_cell_coords ss;

	g_return_if_fail(VTE_IS_TERMINAL(terminal));

	ss = terminal->pvt->selection_start;

	if (x) {
		*x = ss.x;
	}

	if (y) {
		*y = ss.y;
	}
}

void
_vte_terminal_get_end_selection(VteTerminal *terminal, long *x, long *y)
{
	struct selection_cell_coords se;

	g_return_if_fail(VTE_IS_TERMINAL(terminal));

	se = terminal->pvt->selection_end;

	if (x) {
		*x = se.x;
	}

	if (y) {
		*y = se.y;
	}
}

void
_vte_terminal_select_text(VteTerminal *terminal, long start_x, long start_y, long end_x, long end_y, int start_offset, int end_offset)
{
	g_return_if_fail(VTE_IS_TERMINAL(terminal));

	terminal->pvt->selection_type = selection_type_char;
	terminal->pvt->has_selection = TRUE;
	terminal->pvt->selecting_had_delta = TRUE;
	terminal->pvt->selection_start.x = start_x;
	terminal->pvt->selection_start.y = start_y;
	terminal->pvt->selection_end.x = end_x;
	terminal->pvt->selection_end.y = end_y;
	vte_terminal_copy_primary(terminal);
	_vte_invalidate_cells (terminal, 0,
			      terminal->column_count,
			      MIN (start_y, end_y),
			      MAX (start_y, end_y) -
			      MIN (start_y, end_y) + 1);

	vte_terminal_emit_selection_changed(terminal);
}

void
_vte_terminal_remove_selection(VteTerminal *terminal)
{
	vte_terminal_deselect_all (terminal);
}

static gboolean display_timeout (gpointer data);
static gboolean coalesce_timeout (gpointer data);

static void
add_display_timeout (VteTerminal *terminal)
{
	terminal->pvt->display_timeout =
		g_timeout_add (VTE_DISPLAY_TIMEOUT, display_timeout, terminal);
}

static void
add_coalesce_timeout (VteTerminal *terminal)
{
	terminal->pvt->coalesce_timeout =
		g_timeout_add (VTE_COALESCE_TIMEOUT, coalesce_timeout, terminal);
}

static void
remove_display_timeout (VteTerminal *terminal)
{
	g_source_remove (terminal->pvt->display_timeout);
	terminal->pvt->display_timeout = VTE_DISPLAY_TIMEOUT;
}

static void
remove_coalesce_timeout (VteTerminal *terminal)
{
	g_source_remove (terminal->pvt->coalesce_timeout);
	terminal->pvt->coalesce_timeout = VTE_INVALID_SOURCE;
}

static void
vte_terminal_stop_processing (VteTerminal *terminal)
{
	remove_display_timeout (terminal);
	remove_coalesce_timeout (terminal);
}

static void
vte_terminal_start_processing (VteTerminal *terminal)
{
	if (vte_terminal_is_processing (terminal)) {
		remove_coalesce_timeout (terminal);
		add_coalesce_timeout (terminal);
	}
	else {
		add_coalesce_timeout (terminal);
		add_display_timeout (terminal);
	}
}

static gboolean
vte_terminal_is_processing (VteTerminal *terminal)
{
	return terminal->pvt->coalesce_timeout != VTE_INVALID_SOURCE;
}


/* This function is called every DISPLAY_TIMEOUT ms.
 * It makes sure output is never delayed by more than DISPLAY_TIMEOUT
 */
static gboolean
need_processing (VteTerminal *terminal)
{
	return _vte_buffer_length(terminal->pvt->incoming) > 0 ||
		(terminal->pvt->pending->len > 0);
}
	
static gboolean
display_timeout (gpointer data)
{
	VteTerminal *terminal = data;

	if (need_processing (terminal) && 
	    vte_terminal_process_incoming(terminal)) {
		remove_coalesce_timeout (terminal);
		add_coalesce_timeout (terminal);
		return TRUE;
	}

	remove_coalesce_timeout (terminal);
	terminal->pvt->display_timeout = VTE_INVALID_SOURCE;
	
	return FALSE;
}

/* This function is called whenever data haven't arrived for
 * COALESCE_TIMEOUT ms
 */
static gboolean
coalesce_timeout (gpointer data)
{
	VteTerminal *terminal = data;

	if (need_processing (terminal) &&
	    vte_terminal_process_incoming(terminal)) {
		remove_display_timeout (terminal);
		add_display_timeout (terminal);
		return TRUE;
	}

	remove_display_timeout (terminal);
	terminal->pvt->coalesce_timeout = VTE_INVALID_SOURCE;

	return FALSE;
 }
