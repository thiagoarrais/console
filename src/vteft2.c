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

#ident "$Id$"

#include "../config.h"

#include <sys/param.h>
#include <string.h>
#include <gtk/gtk.h>
#include <glib.h>
#include <fontconfig/fontconfig.h>
#include "debug.h"
#include "vtebg.h"
#include "vtedraw.h"
#include "vtefc.h"
#include "vteglyph.h"
#include "vtergb.h"

#define FONT_INDEX_FUDGE 10
#define CHAR_WIDTH_FUDGE 10

struct _vte_ft2_data
{
	struct _vte_glyph_cache *cache;
	struct _vte_rgb_buffer *rgb;
	GdkColor color;
	GdkPixbuf *pixbuf;
	gint scrollx, scrolly;
	gint left, right, top, bottom;
};

static gboolean
_vte_ft2_check(struct _vte_draw *draw, GtkWidget *widget)
{
	/* We can draw onto any widget. */
	return TRUE;
}

static void
_vte_ft2_create(struct _vte_draw *draw, GtkWidget *widget)
{
	struct _vte_ft2_data *data;
	data = g_slice_new0(struct _vte_ft2_data);
	draw->impl_data = data;
	data->rgb = NULL;
	memset(&data->color, 0, sizeof(data->color));
	data->pixbuf = NULL;
	data->scrollx = data->scrolly = 0;
}

static void
_vte_ft2_destroy(struct _vte_draw *draw)
{
	struct _vte_ft2_data *data;
	data = (struct _vte_ft2_data*) draw->impl_data;
	if (data->cache != NULL) {
		_vte_glyph_cache_free(data->cache);
		data->cache = NULL;
	}
	if (data->rgb != NULL) {
		_vte_rgb_buffer_free(data->rgb);
	}
	memset(&data->color, 0, sizeof(data->color));
	if (GDK_IS_PIXBUF(data->pixbuf)) {
		g_object_unref(G_OBJECT(data->pixbuf));
		data->pixbuf = NULL;
	}
	data->scrollx = data->scrolly = 0;
	g_slice_free(struct _vte_ft2_data, data);
}

static GdkVisual *
_vte_ft2_get_visual(struct _vte_draw *draw)
{
	return gtk_widget_get_visual(draw->widget);
}

static GdkColormap *
_vte_ft2_get_colormap(struct _vte_draw *draw)
{
	return gtk_widget_get_colormap(draw->widget);
}

static void
_vte_ft2_start(struct _vte_draw *draw)
{
	struct _vte_ft2_data *data;
	guint width, height;
	data = (struct _vte_ft2_data*) draw->impl_data;

	gdk_window_get_geometry(draw->widget->window,
				NULL, NULL, &width, &height, NULL);
	if (data->rgb == NULL) {
		data->rgb = _vte_rgb_buffer_new(width, height);
	} else {
		_vte_rgb_buffer_resize(data->rgb, width, height);
	}
	data->left = data->right = data->top = data->bottom = -1;
}

static void
_vte_ft2_end(struct _vte_draw *draw)
{
	struct _vte_ft2_data *data;
	guint width, height;
	GtkWidget *widget;
	GtkStateType state;
	data = (struct _vte_ft2_data*) draw->impl_data;
	widget = draw->widget;
	gdk_window_get_geometry(widget->window,
				NULL, NULL, &width, &height, NULL);
	gtk_widget_ensure_style(widget);
	state = GTK_WIDGET_STATE(widget);
	if ((data->left == -1) &&
	    (data->right == -1) &&
	    (data->top == -1) &&
	    (data->bottom == -1)) {
		_vte_rgb_draw_on_drawable(widget->window,
					  widget->style->fg_gc[state],
					  0, 0,
					  width, height,
					  data->rgb,
					  0, 0);
	} else {
		_vte_rgb_draw_on_drawable(widget->window,
					  widget->style->fg_gc[state],
					  data->left, data->top,
					  data->right - data->left + 1,
					  data->bottom - data->top + 1,
					  data->rgb,
					  data->left, data->top);
	}
}

static void
_vte_ft2_set_background_color(struct _vte_draw *draw, GdkColor *color)
{
	struct _vte_ft2_data *data;
	data = (struct _vte_ft2_data*) draw->impl_data;
	data->color = *color;
}

static void
_vte_ft2_set_background_image(struct _vte_draw *draw,
			      enum VteBgSourceType type,
			      GdkPixbuf *pixbuf,
			      const char *file,
			      const GdkColor *color,
			      double saturation)
{
	struct _vte_ft2_data *data;
	GdkPixbuf *bgpixbuf;

	data = (struct _vte_ft2_data*) draw->impl_data;

	bgpixbuf = vte_bg_get_pixbuf(vte_bg_get(), type, pixbuf, file,
				     color, saturation);
	if (GDK_IS_PIXBUF(data->pixbuf)) {
		g_object_unref(G_OBJECT(data->pixbuf));
	}
	data->pixbuf = bgpixbuf;
}

static void
update_bbox(struct _vte_ft2_data *data, gint x, gint y, gint width, gint height)
{
	data->left = (data->left == -1) ?
		     x : MIN(data->left, x);
	data->right = (data->right == -1) ?
		      x + width - 1 : MAX(data->right, x + width - 1);
	data->top = (data->top == -1) ?
		    y : MIN(data->top, y);
	data->bottom = (data->bottom == -1) ?
		       y + height - 1 : MAX(data->bottom, y + height - 1);
}

static void
_vte_ft2_clear(struct _vte_draw *draw,
	       gint x, gint y, gint width, gint height)
{
	struct _vte_ft2_data *data;
	data = (struct _vte_ft2_data*) draw->impl_data;

	if (GDK_IS_PIXBUF(data->pixbuf)) {
		/* Tile a pixbuf in. */
		_vte_rgb_draw_pixbuf(data->rgb, x, y, width, height,
				     data->pixbuf,
				     data->scrollx + x, data->scrolly + y);
	} else {
		/* The simple case is a solid color. */
		_vte_rgb_draw_color(data->rgb, x, y, width, height,
				    &data->color);
	}
	update_bbox(data, x, y, width, height);
}

static void
_vte_ft2_set_text_font(struct _vte_draw *draw,
		       const PangoFontDescription *fontdesc,
		       VteTerminalAntiAlias anti_alias)
{
	struct _vte_ft2_data *data;

	data = (struct _vte_ft2_data*) draw->impl_data;

	if (data->cache != NULL) {
		_vte_glyph_cache_free(data->cache);
		data->cache = NULL;
	}
	data->cache = _vte_glyph_cache_new();
	_vte_glyph_cache_set_font_description(draw->widget, NULL,
					      data->cache, fontdesc, anti_alias,
					      NULL, NULL);
}

static int
_vte_ft2_get_text_width(struct _vte_draw *draw)
{
	struct _vte_ft2_data *data;
	data = (struct _vte_ft2_data*) draw->impl_data;
	return data->cache->width;
}

static int
_vte_ft2_get_text_height(struct _vte_draw *draw)
{
	struct _vte_ft2_data *data;
	data = (struct _vte_ft2_data*) draw->impl_data;
	return data->cache->height;
}

static int
_vte_ft2_get_text_ascent(struct _vte_draw *draw)
{
	struct _vte_ft2_data *data;
	data = (struct _vte_ft2_data*) draw->impl_data;
	return data->cache->ascent;
}

static int
_vte_ft2_get_char_width(struct _vte_draw *draw, gunichar c, int columns)
{
	struct _vte_ft2_data *data;
	const struct _vte_glyph *glyph;

	data = (struct _vte_ft2_data*) draw->impl_data;

	glyph = _vte_glyph_get(data->cache, c);
	if (glyph != NULL) {
		return glyph->width;
	}

	return _vte_ft2_get_text_width(draw) * columns;
}

static gboolean
_vte_ft2_get_using_fontconfig(struct _vte_draw *draw)
{
	return TRUE;
}

static void
_vte_ft2_draw_text(struct _vte_draw *draw,
		   struct _vte_draw_text_request *requests, gsize n_requests,
		   GdkColor *color, guchar alpha)
{
	struct _vte_ft2_data *data;
	int i;

	data = (struct _vte_ft2_data*) draw->impl_data;

	for (i = 0; i < n_requests; i++) {
		_vte_glyph_draw(data->cache, requests[i].c, color,
				requests[i].x, requests[i].y,
				requests[i].columns,
				0,
				data->rgb);
		update_bbox(data, requests[i].x, requests[i].y,
			    data->cache->width * requests[i].columns,
			    data->cache->height);
	}
}

static gboolean
_vte_ft2_draw_char(struct _vte_draw *draw,
		   struct _vte_draw_text_request *request,
		   GdkColor *color, guchar alpha)
{
	struct _vte_ft2_data *data;

	data = (struct _vte_ft2_data*) draw->impl_data;

	if (data->cache != NULL) {
		if (_vte_glyph_get(data->cache, request->c) != NULL) {
			_vte_ft2_draw_text(draw, request, 1, color, alpha);
			return TRUE;
		}
	}
	return FALSE;
}

static void
_vte_ft2_draw_rectangle(struct _vte_draw *draw,
			gint x, gint y, gint width, gint height,
			GdkColor *color, guchar alpha)
{
	struct _vte_ft2_data *data;

	data = (struct _vte_ft2_data*) draw->impl_data;

	_vte_rgb_draw_color(data->rgb,
			    x, y,
			    width, 1,
			    color);
	_vte_rgb_draw_color(data->rgb,
			    x, y,
			    1, height,
			    color);
	_vte_rgb_draw_color(data->rgb,
			    x, y + height - 1,
			    width, 1,
			    color);
	_vte_rgb_draw_color(data->rgb,
			    x + width - 1, y,
			    1, height,
			    color);
	update_bbox(data, x, y, width, height);
}

static void
_vte_ft2_fill_rectangle(struct _vte_draw *draw,
			gint x, gint y, gint width, gint height,
			GdkColor *color, guchar alpha)
{
	struct _vte_ft2_data *data;

	data = (struct _vte_ft2_data*) draw->impl_data;

	_vte_rgb_draw_color(data->rgb, x, y, width, height, color);
	update_bbox(data, x, y, width, height);
}

static void
_vte_ft2_set_scroll(struct _vte_draw *draw, gint x, gint y)
{
	struct _vte_ft2_data *data;
	data = (struct _vte_ft2_data*) draw->impl_data;
	data->scrollx = x;
	data->scrolly = y;
}

struct _vte_draw_impl _vte_draw_ft2 = {
	"VteFT2", "VTE_USE_FT2",
	_vte_ft2_check,
	_vte_ft2_create,
	_vte_ft2_destroy,
	_vte_ft2_get_visual,
	_vte_ft2_get_colormap,
	_vte_ft2_start,
	_vte_ft2_end,
	_vte_ft2_set_background_color,
	_vte_ft2_set_background_image,
	FALSE,
	_vte_ft2_clear,
	_vte_ft2_set_text_font,
	_vte_ft2_get_text_width,
	_vte_ft2_get_text_height,
	_vte_ft2_get_text_ascent,
	_vte_ft2_get_char_width,
	_vte_ft2_get_using_fontconfig,
	_vte_ft2_draw_text,
	_vte_ft2_draw_char,
	_vte_ft2_draw_rectangle,
	_vte_ft2_fill_rectangle,
	_vte_ft2_set_scroll,
};
