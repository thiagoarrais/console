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

#include <gtk/gtk.h>

#ifndef X_DISPLAY_MISSING
#ifdef HAVE_GL

#include <sys/param.h>
#include <stdio.h>
#include <string.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xutil.h>
#include "buffer.h"
#include "debug.h"
#include "vtedraw.h"
#include "vtefc.h"
#include "vtegl.h"
#include "vteglyph.h"

struct _vte_gl_data
{
	XVisualInfo *visual_info;
	Display *display;
	GLXContext context;
	GdkPixbuf *bgpixbuf;
	GLXDrawable glwindow;
	struct _vte_glyph_cache *cache;
	struct _vte_buffer *buffer;
};

#define _vte_gl_attributes GLX_USE_GL, GLX_DOUBLEBUFFER, GLX_RGBA, None,

static gboolean
_vte_gl_check(struct _vte_draw *draw, GtkWidget *widget)
{
	int attributes[] = {_vte_gl_attributes};
	XVisualInfo *visual_info;
	GLXContext context = NULL;
	GdkDisplay *gdisplay;
	Display *display;
	GdkScreen *gscreen;
	int screen;
	int error, event;
	gboolean direct;

	gdisplay = gtk_widget_get_display (widget);
	display = gdk_x11_display_get_xdisplay (gdisplay);
	gscreen = gtk_widget_get_screen (widget);
	screen = gdk_x11_screen_get_screen_number (gscreen);

	/* Check for GLX. */
	if (!glXQueryExtension(display, &error, &event)) {
#ifdef VTE_DEBUG
		g_warning("Unable to use GLX.\n");
#endif
		return FALSE;
	}

	/* See if a suitable visual exists. */
	visual_info = glXChooseVisual(display, screen, attributes);
	if (visual_info == NULL) {
#ifdef VTE_DEBUG
		g_warning("Unable to find a suitable GLX visual.\n");
#endif
		return FALSE;
	}

	/* Create a GLX context. */
	context = glXCreateContext(display, visual_info, NULL, GL_TRUE);
	if (context == NULL) {
#ifdef VTE_DEBUG
		g_warning("Unable to create a GLX context.\n");
#endif
		return FALSE;
	}

	/* Check if it's a direct rendering context. */
	direct = glXIsDirect(display, context);
#ifdef VTE_DEBUG
	if (!direct) {
		g_warning("Unable to create a direct GLX context.\n");
	}
#endif
	glXDestroyContext(display, context);

	return (direct == True) ? TRUE : FALSE;
}

static void
_vte_gl_create(struct _vte_draw *draw, GtkWidget *widget)
{
	struct _vte_gl_data *data;
	int attributes[] = {_vte_gl_attributes};
	GdkDisplay *gdisplay;
	Display *display;
	GdkScreen *gscreen;
	int screen;

	draw->impl_data = g_slice_new(struct _vte_gl_data);
	data = (struct _vte_gl_data*) draw->impl_data;

	gdisplay = gtk_widget_get_display (widget);
	display = gdk_x11_display_get_xdisplay (gdisplay);
	gscreen = gtk_widget_get_screen (widget);
	screen = gdk_x11_screen_get_screen_number (gscreen);

	data->display = display;

	data->visual_info = glXChooseVisual(display, screen, attributes);
	if (data->visual_info == NULL) {
		g_error("Unable to find a suitable GLX visual.\n");
	}

	data->context = glXCreateContext(display, data->visual_info,
					 NULL, GL_TRUE);
	if (data->context == NULL) {
		g_error("Unable to create a GLX context.\n");
	}

	data->bgpixbuf = NULL;
	data->glwindow = -1;
	data->cache = _vte_glyph_cache_new();
	data->buffer = _vte_buffer_new();

	gtk_widget_set_double_buffered(widget, FALSE);
}

static void
_vte_gl_destroy(struct _vte_draw *draw)
{
	struct _vte_gl_data *data;

	data = (struct _vte_gl_data*) draw->impl_data;

	_vte_buffer_free(data->buffer);

	_vte_glyph_cache_free(data->cache);

	if (data->bgpixbuf != NULL) {
		g_object_unref(data->bgpixbuf);
	}

	glXMakeCurrent(data->display, None, data->context);

	glXDestroyContext(data->display, data->context);

	g_slice_free(struct _vte_gl_data, draw->impl_data);
}

static GdkVisual *
_vte_gl_get_visual(struct _vte_draw *draw)
{
	GdkScreen *gscreen;
	struct _vte_gl_data *data;

	data = (struct _vte_gl_data*) draw->impl_data;
	gscreen = gdk_screen_get_default();
#ifdef VTE_DEBUG
	g_print("Using GLX-capable visual 0x%02lx.\n",
		(long) data->visual_info->visualid);
#endif
	return gdk_x11_screen_lookup_visual(gscreen,
					    data->visual_info->visualid);
}

static GdkColormap *
_vte_gl_get_colormap(struct _vte_draw *draw)
{
	return NULL;
}

static void
_vte_gl_start(struct _vte_draw *draw)
{
	struct _vte_gl_data *data;
	gint width, height;

	data = (struct _vte_gl_data*) draw->impl_data;

	data->glwindow = gdk_x11_drawable_get_xid(draw->widget->window);
	width = draw->widget->allocation.width;
	height = draw->widget->allocation.height;

	glXMakeCurrent(data->display, data->glwindow, data->context);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, width, height, 0, -1, 1);
	glViewport(0, height, width, -height);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

static void
_vte_gl_end(struct _vte_draw *draw)
{
	struct _vte_gl_data *data;

	data = (struct _vte_gl_data*) draw->impl_data;

	glXMakeCurrent(data->display, data->glwindow, data->context);
	glXSwapBuffers(data->display, data->glwindow);

	data->glwindow = -1;
}

static void
_vte_gl_set_background_image(struct _vte_draw *draw,
			     enum VteBgSourceType type,
			     GdkPixbuf *pixbuf,
			     const char *file,
			     const GdkColor *tint,
			     double saturation)
{
	struct _vte_gl_data *data;
	GdkPixbuf *bgpixbuf;
	GdkScreen *screen;

	screen = gtk_widget_get_screen(draw->widget);

	data = (struct _vte_gl_data*) draw->impl_data;
	bgpixbuf = vte_bg_get_pixbuf(vte_bg_get_for_screen(screen),
				     type, pixbuf, file,
				     tint, saturation);
	if (data->bgpixbuf != NULL) {
		g_object_unref(data->bgpixbuf);
	}
	data->bgpixbuf = bgpixbuf;
}

static void
_vte_gl_clear(struct _vte_draw *draw, gint x, gint y, gint width, gint height)
{
	struct _vte_gl_data *data;
	long xstop, ystop, i, j;
	int pixbufw, pixbufh, w, h, channels, stride;
	GLenum format = 0;
	guchar *pixels;

	data = (struct _vte_gl_data*) draw->impl_data;

	glXMakeCurrent(data->display, data->glwindow, data->context);

	if (data->bgpixbuf != NULL) {
		pixbufw = gdk_pixbuf_get_width(data->bgpixbuf);
		pixbufh = gdk_pixbuf_get_height(data->bgpixbuf);
	} else {
		pixbufw = pixbufh = 0;
	}

	if ((pixbufw == 0) || (pixbufh == 0)) {
		glColor4us(draw->bg_color.red,
			   draw->bg_color.green,
			   draw->bg_color.blue,
			   0xFFFF);
		glBegin(GL_POLYGON);
		glVertex2d(x, y);
		glVertex2d(x + width, y);
		glVertex2d(x + width, y + height - 1);
		glVertex2d(x, y + height - 1);
		glEnd();
		return;
	}

	/* Flood fill. */
	xstop = x + width;
	ystop = y + height;

	pixels = gdk_pixbuf_get_pixels(data->bgpixbuf);
	channels = gdk_pixbuf_get_n_channels(data->bgpixbuf);
	stride = gdk_pixbuf_get_rowstride(data->bgpixbuf);

	switch (channels) {
	case 3:
		format = GL_RGB;
		break;
	case 4:
		format = GL_RGBA;
		break;
	default:
		g_assert_not_reached();
		break;
	}

	y = ystop - height;
	j = (draw->scrolly + y) % pixbufh;
	while (y < ystop) {
		x = xstop - width;
		i = (draw->scrollx + x) % pixbufw;

		/* h = MIN(pixbufh - (j % pixbufh), ystop - y); */
		h = 1;
		while (x < xstop) {
			w = MIN(pixbufw - (i % pixbufw), xstop - x);

			glRasterPos2i(x, y);
			glDrawPixels(w, h,
				     format, GL_UNSIGNED_BYTE,
				     pixels + stride * j + channels * i);
			x += w;
			i = 0;
		}
		y += h;
		j = (draw->scrolly + y) % pixbufh;
	}
	glFlush();
}

static void
_vte_gl_fcpattern_disable_rgba(FcPattern *pattern, gpointer data)
{
	int rgba;
	if (FcPatternGetInteger(pattern,
				FC_RGBA, 0, &rgba) != FcResultNoMatch) {
		FcPatternDel(pattern, FC_RGBA);
	}
	FcPatternAddInteger(pattern, FC_RGBA, FC_RGBA_NONE);
}

static void
_vte_gl_set_text_font(struct _vte_draw *draw,
		     const PangoFontDescription *fontdesc,
		     VteTerminalAntiAlias antialias)
{
	struct _vte_gl_data *data;

	data = (struct _vte_gl_data*) draw->impl_data;
	if (data->cache != NULL) {
		_vte_glyph_cache_free(data->cache);
		data->cache = NULL;
	}
	data->cache = _vte_glyph_cache_new();

	_vte_glyph_cache_set_font_description(draw->widget,
					      NULL, data->cache, fontdesc,
					      antialias,
					      _vte_gl_fcpattern_disable_rgba,
					      NULL);
}

static int
_vte_gl_get_text_width(struct _vte_draw *draw)
{
	struct _vte_gl_data *data;

	data = (struct _vte_gl_data*) draw->impl_data;
	return data->cache->width;
}

static int
_vte_gl_get_text_height(struct _vte_draw *draw)
{
	struct _vte_gl_data *data;

	data = (struct _vte_gl_data*) draw->impl_data;
	return data->cache->height;
}

static int
_vte_gl_get_text_ascent(struct _vte_draw *draw)
{
	struct _vte_gl_data *data;

	data = (struct _vte_gl_data*) draw->impl_data;
	return data->cache->ascent;
}

static int
_vte_gl_get_char_width(struct _vte_draw *draw, gunichar c, int columns)
{
	struct _vte_gl_data *data;
	const struct _vte_glyph *glyph;

	data = (struct _vte_gl_data*) draw->impl_data;

	glyph = _vte_glyph_get(data->cache, c);
	if (glyph == NULL)
		return 0;

	return glyph->width;
}

static void
_vte_gl_draw_text(struct _vte_draw *draw,
		  struct _vte_draw_text_request *requests, gsize n_requests,
		  GdkColor *color, guchar alpha)
{
	struct _vte_gl_data *data;
	const struct _vte_glyph *glyph;
	guint16 a, r, g, b;
	guint i, j;
	int k, x, y, w, pad, rows, columns, src, dest;
	guchar *pixels;

	data = (struct _vte_gl_data*) draw->impl_data;

	glXMakeCurrent(data->display, data->glwindow, data->context);

	r = color->red >> 8;
	g = color->green >> 8;
	b = color->blue >> 8;

	rows = 0;
	columns = 0;
	for (i = 0; i < n_requests; i++) {
		glyph = _vte_glyph_get(data->cache, requests[i].c);
		columns += (requests[i].columns * data->cache->width);
		if ((glyph == NULL) ||
		    (glyph->width == 0) ||
		    (glyph->height == 0)) {
			continue;
		}
		rows = MAX(rows, glyph->skip + glyph->height);
	}

	_vte_buffer_set_minimum_size(data->buffer, rows * columns * 4);
	pixels = data->buffer->bytes;
	memset(pixels, 0, rows * columns * 4);
	for (k = 0; k < rows * columns; k++) {
		pixels[k * 4 + 0] = r;
		pixels[k * 4 + 1] = g;
		pixels[k * 4 + 2] = b;
	}

	for (i = j = 0; i < n_requests; i++) {
		glyph = _vte_glyph_get(data->cache, requests[i].c);
		w = requests[i].columns * data->cache->width;
		if ((glyph == NULL) ||
		    (glyph->width == 0) ||
		    (glyph->height == 0)) {
			j += w;
			continue;
		}
		pad = (w - glyph->width) / 2;
		for (y = 0; y < glyph->height; y++)
		for (x = 0; x < glyph->width; x++) {
			src = (y * glyph->width + x) * glyph->bytes_per_pixel;
			a = glyph->bytes[src];
			if (a == 0) {
				continue;
			}
			if (alpha != VTE_DRAW_OPAQUE) {
				a = (a * alpha) >> 8;
			}
			if (a == 0) {
				continue;
			}
			dest = (y + glyph->skip) * columns * 4 +
			       (j + pad + x) * 4 + 3;
			pixels[dest] = a;
		}
		j += w;
	}

	glRasterPos2i(requests[0].x, requests[0].y);
	glPixelZoom(1, -1);
	glDrawPixels(columns, rows, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
}

static gboolean
_vte_gl_draw_has_char(struct _vte_draw *draw, gunichar c)
{
	struct _vte_gl_data *data;

	data = (struct _vte_gl_data*) draw->impl_data;

	if (data->cache != NULL) {
		if (_vte_glyph_get(data->cache, c) != NULL) {
			return TRUE;
		}
	}
	return FALSE;
}

static void
_vte_gl_rectangle(struct _vte_draw *draw,
		  GLenum type,
		  gint x, gint y, gint width, gint height,
		  GdkColor *color, guchar alpha)
{
	struct _vte_gl_data *data;

	data = (struct _vte_gl_data*) draw->impl_data;

	glXMakeCurrent(data->display, data->glwindow, data->context);

	glColor4us(color->red, color->green, color->blue,
		   (alpha == VTE_DRAW_OPAQUE) ? 0xffff : (alpha << 8));
	glBegin(type);
	glVertex2d(x, y);
	glVertex2d(x + width, y);
	glVertex2d(x + width, y + height);
	glVertex2d(x, y + height);
	glEnd();
}

static void
_vte_gl_draw_rectangle(struct _vte_draw *draw,
		       gint x, gint y, gint width, gint height,
		       GdkColor *color, guchar alpha)
{
	_vte_gl_rectangle(draw, GL_LINE_LOOP, x, y, width, height,
			  color, alpha);
}

static void
_vte_gl_fill_rectangle(struct _vte_draw *draw,
		       gint x, gint y, gint width, gint height,
		       GdkColor *color, guchar alpha)
{
	_vte_gl_rectangle(draw, GL_POLYGON, x, y, width, height,
			  color, alpha);
}

const struct _vte_draw_impl _vte_draw_gl = {
	"gl",
	_vte_gl_check,
	_vte_gl_create,
	_vte_gl_destroy,
	_vte_gl_get_visual,
	_vte_gl_get_colormap,
	_vte_gl_start,
	_vte_gl_end,
	NULL, /* set_background_opacity */
	NULL, /* set_background_color */
	_vte_gl_set_background_image,
	TRUE, /* always_requires_clear */
	NULL, /* clip */
	_vte_gl_clear,
	_vte_gl_set_text_font,
	_vte_gl_get_text_width,
	_vte_gl_get_text_height,
	_vte_gl_get_text_ascent,
	_vte_gl_get_char_width,
	NULL, /* get_using_fontconfig */
	_vte_gl_draw_text,
	NULL, /* draw_char */
	_vte_gl_draw_has_char,
	_vte_gl_draw_rectangle,
	_vte_gl_fill_rectangle,
	NULL /* set_scroll */
};

#endif
#endif
