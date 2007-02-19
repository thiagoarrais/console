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

#include <sys/param.h>
#include <string.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <glib.h>
#include <fontconfig/fontconfig.h>
#include <X11/Xft/Xft.h>
#include "debug.h"
#include "vtebg.h"
#include "vtedraw.h"
#include "vtefc.h"
#include "vtexft.h"
#include "vtetree.h"
#include "vte-private.h"

#include <glib/gi18n-lib.h>

#define FONT_INDEX_FUDGE 10
#define CHAR_WIDTH_FUDGE 10

#define DPY_FUDGE 1

struct _vte_xft_font {
	Display *display;
	GPtrArray *patterns;
	GPtrArray *fonts;
	GPtrArray *locked_fonts;
	VteTree *fontmap;
	VteTree *widths;
};

struct _vte_xft_data
{
	struct _vte_xft_font *font;
	Display *display;
	Drawable drawable;
	int x_offs, y_offs;
	Visual *visual;
	Colormap colormap;
	XftDraw *draw;
	GC gc;
	GdkColor color;
	guint16 opacity;
	GdkPixmap *pixmap;
	Pixmap xpixmap;
	gint pixmapw, pixmaph;
	gint scrollx, scrolly;
};

static int
_vte_xft_direct_compare(gconstpointer a, gconstpointer b)
{
	return GPOINTER_TO_INT(a) - GPOINTER_TO_INT(b);
}

static gboolean
_vte_xft_char_exists(struct _vte_xft_font *font, XftFont *ftfont, FcChar32 c)
{
	return XftCharExists(font->display, ftfont, c) == FcTrue;
}

static void
_vte_xft_text_extents(struct _vte_xft_font *font, XftFont *ftfont, FcChar32 c,
		      XGlyphInfo *extents)
{
	XftTextExtents32(font->display, ftfont, &c, 1, extents);
}

static struct _vte_xft_font *
_vte_xft_font_open(GtkWidget *widget, const PangoFontDescription *fontdesc,
		   VteTerminalAntiAlias antialias)
{
	struct _vte_xft_font *font;
	GPtrArray *patterns;

	patterns = g_ptr_array_new();
	if (!_vte_fc_patterns_from_pango_font_desc(widget, fontdesc, antialias,
						   patterns, NULL, NULL)) {
		g_ptr_array_free(patterns, TRUE);
		return NULL;
	}

	font = g_slice_new(struct _vte_xft_font);
	font->display = GDK_DISPLAY_XDISPLAY (gtk_widget_get_display (widget));
	font->patterns = patterns;
	font->fonts = g_ptr_array_new();
	font->locked_fonts = g_ptr_array_new();
	font->fontmap = _vte_tree_new(_vte_xft_direct_compare);
	font->widths = _vte_tree_new(_vte_xft_direct_compare);

	return font;
}

static void
_vte_xft_font_close(struct _vte_xft_font *font)
{
	XftFont *ftfont;
	FcPattern *pattern;
	guint i;

	for (i = 0; i < font->patterns->len; i++) {
		pattern = g_ptr_array_index(font->patterns, i);
		if (pattern != NULL) {
			FcPatternDestroy(pattern);
		}
	}
	g_ptr_array_free(font->patterns, TRUE);

	for (i = 0; i < font->locked_fonts->len; i++) {
		ftfont = g_ptr_array_index(font->locked_fonts, i);
		if (ftfont != NULL) {
			XftUnlockFace(ftfont);
		}
	}
	g_ptr_array_free(font->locked_fonts, TRUE);

	for (i = 0; i < font->fonts->len; i++) {
		ftfont = g_ptr_array_index(font->fonts, i);
		if (ftfont != NULL) {
			XftFontClose(font->display, ftfont);
		}
	}
	g_ptr_array_free(font->fonts, TRUE);

	_vte_tree_destroy(font->fontmap);
	_vte_tree_destroy(font->widths);

	g_slice_free(struct _vte_xft_font, font);
}

static XftFont *
_vte_xft_font_for_char(struct _vte_xft_font *font, gunichar c)
{
	guint i;
	XftFont *ftfont;
	gpointer p = GINT_TO_POINTER(c);

	/* Check if we have a char-to-font entry for it. */
	i = GPOINTER_TO_INT(_vte_tree_lookup(font->fontmap, p));
	if (i != 0) {
		switch (i) {
		/* Checked before, no luck. */
		case -FONT_INDEX_FUDGE:
			return NULL;
		/* Matched before. */
		default:
			i -= FONT_INDEX_FUDGE;
			ftfont = g_ptr_array_index(font->fonts, i);
			if (g_ptr_array_index (font->locked_fonts, i) == NULL) {
				XftLockFace (ftfont);
				g_ptr_array_index(font->locked_fonts, i) = ftfont;
			}
			return ftfont;
		}
	}

	/* Look the character up in the fonts we have. */
	for (i = 0; i < font->fonts->len; i++) {
		ftfont = g_ptr_array_index(font->fonts, i);
		if (_vte_xft_char_exists(font, ftfont, c)) {
			if (g_ptr_array_index (font->locked_fonts, i) == NULL) {
				XftLockFace (ftfont);
				g_ptr_array_index (font->locked_fonts, i) = ftfont;
			}
			_vte_tree_insert(font->fontmap,
					p,
					GINT_TO_POINTER(i + FONT_INDEX_FUDGE));
			return ftfont;
		}
	}

	/* Look the character up in other fonts. */
	for (; i < font->patterns->len; i++) {
		if (g_ptr_array_index(font->patterns, i) == NULL) {
			continue;
		}
		ftfont = XftFontOpenPattern(font->display,
				g_ptr_array_index(font->patterns, i));
		g_ptr_array_index(font->patterns, i) = NULL;
		/* If the font was opened, it owns the pattern. */
		if (ftfont != NULL) {
			g_ptr_array_add(font->fonts, ftfont);
			g_ptr_array_add (font->locked_fonts, NULL);
			if (_vte_xft_char_exists(font, ftfont, c)) {
				XftLockFace (ftfont);
				g_ptr_array_index (font->locked_fonts, i) = ftfont;
				_vte_tree_insert(font->fontmap,
						p,
						GINT_TO_POINTER(i + FONT_INDEX_FUDGE));
				return ftfont;
			}
		}
	}

	/* No match? */
	_vte_tree_insert(font->fontmap,
			p,
			GINT_TO_POINTER(-FONT_INDEX_FUDGE));
	g_warning(_("Can not find appropiate font for character U+%04x.\n"), c);
	return NULL;
}

static int
_vte_xft_char_width(struct _vte_xft_font *font, XftFont *ftfont, gunichar c)
{
	XGlyphInfo extents;
	gpointer p = GINT_TO_POINTER(c);
	int i;

	/* Check if we have a char-to-width entry for it. */
	i = GPOINTER_TO_INT(_vte_tree_lookup(font->widths, p));
	if (i != 0) {
		switch (i) {
		case -CHAR_WIDTH_FUDGE:
			return 0;
			break;
		default:
			return i - CHAR_WIDTH_FUDGE;
			break;
		}
	}

	/* Compute and store the width. */
	memset(&extents, 0, sizeof(extents));
	if (ftfont != NULL) {
		_vte_xft_text_extents(font, ftfont, c, &extents);
	}
	i = extents.xOff + CHAR_WIDTH_FUDGE;
	_vte_tree_insert(font->widths, p, GINT_TO_POINTER(i));
	return extents.xOff;
}

static gboolean
_vte_xft_check(struct _vte_draw *draw, GtkWidget *widget)
{
	/* We can draw onto any widget. */
	return TRUE;
}

static void
_vte_xft_create(struct _vte_draw *draw, GtkWidget *widget)
{
	struct _vte_xft_data *data;
	data = g_slice_new0(struct _vte_xft_data);
	draw->impl_data = data;
	data->drawable = -1;
	data->colormap = -1;
	data->opacity = 0xffff;
	data->xpixmap = -1;
	data->pixmapw = data->pixmaph = -1;
}

static void
_vte_xft_destroy(struct _vte_draw *draw)
{
	struct _vte_xft_data *data;
	data = (struct _vte_xft_data*) draw->impl_data;
	if (data->font != NULL) {
		_vte_xft_font_close(data->font);
	}
	if (data->draw != NULL) {
		XftDrawDestroy(data->draw);
	}
	if (data->gc != NULL) {
		XFreeGC(data->display, data->gc);
	}
	g_slice_free(struct _vte_xft_data, data);
}

static GdkVisual *
_vte_xft_get_visual(struct _vte_draw *draw)
{
	return gtk_widget_get_visual(draw->widget);
}

static GdkColormap *
_vte_xft_get_colormap(struct _vte_draw *draw)
{
	return gtk_widget_get_colormap(draw->widget);
}

static void
_vte_xft_start(struct _vte_draw *draw)
{
	GdkVisual *gvisual;
	GdkColormap *gcolormap;
	GdkDrawable *drawable;

	struct _vte_xft_data *data;
	data = (struct _vte_xft_data*) draw->impl_data;

	gdk_window_get_internal_paint_info(draw->widget->window,
					   &drawable,
					   &data->x_offs,
					   &data->y_offs);

	data->display = gdk_x11_drawable_get_xdisplay(drawable);
	data->drawable = gdk_x11_drawable_get_xid(drawable);
	gvisual = gdk_drawable_get_visual(drawable);
	data->visual = gdk_x11_visual_get_xvisual(gvisual);
	gcolormap = gdk_drawable_get_colormap(drawable);
	data->colormap = gdk_x11_colormap_get_xcolormap(gcolormap);

	gdk_error_trap_push ();

	if (data->draw != NULL) {
		XftDrawDestroy(data->draw);
	}
	data->draw = XftDrawCreate(data->display, data->drawable,
				   data->visual, data->colormap);
	if (data->gc != NULL) {
		XFreeGC(data->display, data->gc);
	}
	data->gc = XCreateGC(data->display, data->drawable, 0, NULL);
}

static void
_vte_xft_end(struct _vte_draw *draw)
{
	struct _vte_xft_data *data;
	struct _vte_xft_font *font;

	data = (struct _vte_xft_data*) draw->impl_data;
	if (data->draw != NULL) {
		XftDrawDestroy(data->draw);
		data->draw = NULL;
	}
	if (data->gc != NULL) {
		XFreeGC(data->display, data->gc);
		data->gc = NULL;
	}
	data->drawable = -1;
	data->x_offs = data->y_offs = 0;

	font = data->font;
	if (font != NULL) {
		guint i;
		for (i = 0; i < font->locked_fonts->len; i++) {
			XftFont *ftfont = g_ptr_array_index(font->locked_fonts, i);
			if (ftfont != NULL) {
				XftUnlockFace (ftfont);
				g_ptr_array_index(font->locked_fonts, i) = NULL;
			}
		}
	}

	gdk_error_trap_pop ();
}

static void
_vte_xft_set_background_color(struct _vte_draw *draw, GdkColor *color,
			      guint16 opacity)
{
	struct _vte_xft_data *data;
	data = (struct _vte_xft_data*) draw->impl_data;
	data->color = *color;
	data->opacity = opacity;
}

static void
_vte_xft_set_background_image(struct _vte_draw *draw,
			      enum VteBgSourceType type,
			      GdkPixbuf *pixbuf,
			      const char *file,
			      const GdkColor *color,
			      double saturation)
{
	struct _vte_xft_data *data;
	GdkPixmap *pixmap;
	GdkScreen *screen;

	data = (struct _vte_xft_data*) draw->impl_data;

	screen = gtk_widget_get_screen (draw->widget);

	data->xpixmap = -1;
	data->pixmapw = data->pixmaph = 0;
	pixmap = vte_bg_get_pixmap(vte_bg_get_for_screen(screen), type,
				   pixbuf, file, color, saturation,
				   _vte_draw_get_colormap(draw, TRUE));
	if (data->pixmap != NULL) {
		g_object_unref(data->pixmap);
	}
	data->pixmap = NULL;
	if (pixmap != NULL) {
		data->pixmap = pixmap;
		data->xpixmap = gdk_x11_drawable_get_xid(pixmap);
		gdk_drawable_get_size(pixmap, &data->pixmapw, &data->pixmaph);
	}
}

static void
_vte_xft_clip(struct _vte_draw *draw,
		GdkRegion *region)
{
	struct _vte_xft_data *data = draw->impl_data;
	GdkRectangle *rect;
	gint i, n;

	gdk_region_get_rectangles(region, &rect, &n);
	if (n>0) {
		XRectangle *xrect = g_new (XRectangle, n);
		for (i=0; i<n; i++) {
			/* we include the offset here as XftDrawSetClipRectangles() has a
			 * byte-sex bug in its offset parameters. Bug 403159.
			 */
			xrect[i].x = rect[i].x - data->x_offs;
			xrect[i].y = rect[i].y - data->y_offs;
			xrect[i].width = rect[i].width;
			xrect[i].height = rect[i].height;
		}
		XftDrawSetClipRectangles(data->draw,
				0, 0, xrect, n);
		g_free (xrect);
	}
	g_free(rect);
}

static void
_vte_xft_clear(struct _vte_draw *draw,
	       gint x, gint y, gint width, gint height)
{
	struct _vte_xft_data *data;
	XRenderColor rcolor;
	XftColor ftcolor;
	gint h, w, txstop, tystop, sx, sy, tx, ty;

	data = (struct _vte_xft_data*) draw->impl_data;

	if (data->pixmap == NULL ||
	    (data->pixmapw <= 0) ||
	    (data->pixmaph <= 0)) {
		rcolor.red = data->color.red * data->opacity / 0xffff;
		rcolor.green = data->color.green * data->opacity / 0xffff;
		rcolor.blue = data->color.blue * data->opacity / 0xffff;
		rcolor.alpha = data->opacity;
		if (XftColorAllocValue(data->display, data->visual,
				       data->colormap, &rcolor, &ftcolor)) {
			XftDrawRect(data->draw, &ftcolor,
				    x - data->x_offs,
				    y - data->y_offs,
				    width, height);
			XftColorFree(data->display, data->visual,
				     data->colormap, &ftcolor);
		}
		return;
	}

	/* Adjust the drawing offsets. */
	tx = x;
	ty = y;
	txstop = x + width;
	tystop = y + height;

	/* Flood fill. */
	sy = (data->scrolly + y) % data->pixmaph;
	while (ty < tystop) {
		h = MIN(data->pixmaph - sy, tystop - ty);
		tx = x;
		sx = (data->scrollx + x) % data->pixmapw;
		while (tx < txstop) {
			w = MIN(data->pixmapw - sx, txstop - tx);
			XCopyArea(data->display,
				  data->xpixmap,
				  data->drawable,
				  data->gc,
				  sx, sy,
				  w, h,
				  tx - data->x_offs, ty - data->y_offs);
			tx += w;
			sx = 0;
		}
		ty += h;
		sy = 0;
	}
}

static void
_vte_xft_set_text_font(struct _vte_draw *draw,
		       const PangoFontDescription *fontdesc,
		       VteTerminalAntiAlias antialias)
{
	struct _vte_xft_font *ft;
	XftFont *font, *prev_font;
	XGlyphInfo extents;
	struct _vte_xft_data *data;
	gunichar wide_chars[] = {VTE_DRAW_DOUBLE_WIDE_CHARACTERS};
	guint i;
	gint n, width, height, min = G_MAXINT, max = G_MININT;
	FcChar32 c;

	data = (struct _vte_xft_data*) draw->impl_data;

	ft = _vte_xft_font_open(draw->widget, fontdesc, antialias);
	if (ft != NULL) {
		if (data->font != NULL) {
			_vte_xft_font_close(data->font);
		}
		data->font = ft;
	}
	if (data->font == NULL) {
		return;
	}

	gdk_error_trap_push ();

	draw->width = 1;
	draw->height = 1;
	draw->ascent = 1;

	n = width = height = 0;
	/* Estimate a typical cell width by looking at single-width
	 * characters. */
	for (i = 0; i < sizeof(VTE_DRAW_SINGLE_WIDE_CHARACTERS) - 1; i++) {
		c = VTE_DRAW_SINGLE_WIDE_CHARACTERS[i];
		font = _vte_xft_font_for_char(data->font, c);
		if (font != NULL) {
			memset(&extents, 0, sizeof(extents));
			_vte_xft_text_extents(data->font, font, c, &extents);
			n++;
			width += extents.xOff;
			if (extents.xOff < min) {
				min = extents.xOff;
			}
			if (extents.xOff > max) {
				max = extents.xOff;
			}
			if (extents.height > height) {
				height = extents.height;
			}
		}
	}
	if (n > 0) {
		draw->width = howmany(width, n);
		draw->height = (font != NULL) ?
			       font->ascent + font->descent : height;
		draw->ascent = (font != NULL) ?
			       font->ascent : height;
	}
	/* Estimate a typical cell width by looking at double-width
	 * characters, and if it's the same as the single width, assume the
	 * single-width stuff is broken. */
	n = width = 0;
	prev_font = NULL;
	for (i = 0; i < G_N_ELEMENTS(wide_chars); i++) {
		c = wide_chars[i];
		font = _vte_xft_font_for_char(data->font, c);
		if (font != NULL) {
			if (n && prev_font != font) {/* font change */
				width = howmany(width, n);
				if (width >= draw->width -1 &&
						width <= draw->width + 1){
					/* add 1 to round up when dividing by 2 */
					draw->width = (draw->width + 1) / 2;
					break;
				}
				n = width = 0;
			}
			memset(&extents, 0, sizeof(extents));
			_vte_xft_text_extents(data->font, font, c, &extents);
			n++;
			width += extents.xOff;
			prev_font = font;
		}
	}
	if (n > 0) {
		width = howmany(width, n);
		if (width >= draw->width -1 &&
				width <= draw->width + 1){
			/* add 1 to round up when dividing by 2 */
			draw->width = (draw->width + 1) / 2;
		}
	}

	gdk_error_trap_pop ();

	_vte_debug_print(VTE_DEBUG_MISC,
			"VteXft font metrics = %dx%d (%d),"
			" width range [%d, %d].\n",
			draw->width, draw->height, draw->ascent,
			min, max);
}

static int
_vte_xft_get_text_width(struct _vte_draw *draw)
{
	return draw->width;
}

static int
_vte_xft_get_text_height(struct _vte_draw *draw)
{
	return draw->height;
}

static int
_vte_xft_get_text_ascent(struct _vte_draw *draw)
{
	return draw->ascent;
}

static int
_vte_xft_get_char_width(struct _vte_draw *draw, gunichar c, int columns)
{
	struct _vte_xft_data *data;
	XftFont *ftfont;

	data = (struct _vte_xft_data*) draw->impl_data;
	if (data->font == NULL) {
		return _vte_xft_get_text_width(draw) * columns;
	}
	ftfont = _vte_xft_font_for_char(data->font, c);
	if (ftfont == NULL) {
		return _vte_xft_get_text_width(draw) * columns;
	}
	return _vte_xft_char_width(data->font, ftfont, c);
}

static gboolean
_vte_xft_get_using_fontconfig(struct _vte_draw *draw)
{
	return TRUE;
}

static void
_vte_xft_draw_text(struct _vte_draw *draw,
		   struct _vte_draw_text_request *requests, gsize n_requests,
		   GdkColor *color, guchar alpha)
{
	XftGlyphSpec glyphs[VTE_DRAW_MAX_LENGTH];
	XRenderColor rcolor;
	XftColor ftcolor;
	struct _vte_xft_data *data;
	gsize i, j;
	gint width, pad;
	XftFont *font, *ft;

	data = (struct _vte_xft_data*) draw->impl_data;
	if (G_UNLIKELY (data->font == NULL)){
		return; /* cannot draw anything */
	}

	/* find the first displayable character ... */
	font = NULL;
	for (i = 0; i < n_requests; i++) {
		if (requests[i].c == ' ') {
			continue;
		}
		font = _vte_xft_font_for_char(data->font, requests[i].c);
		if (G_UNLIKELY (font == NULL)) {
			continue;
		}
		break;
	}
	if (G_UNLIKELY (i == n_requests)) {
		return; /* nothing to see here, please move along */
	}

	rcolor.red = color->red;
	rcolor.green = color->green;
	rcolor.blue = color->blue;
	rcolor.alpha = (alpha == VTE_DRAW_OPAQUE) ?
		0xffff : (alpha << 8);
	if (!XftColorAllocValue(data->display, data->visual,
				data->colormap, &rcolor, &ftcolor)) {
		return;
	}

	/* split the text into runs of the same font, because
	 * "We need to break down the draw request into runs which use the same
	 * font, to work around a bug which appears to be in Xft and which I
	 * haven't pinned down yet." */
	do {
		j = 0;
		do {
			glyphs[j].glyph = XftCharIndex(data->display,
					font, requests[i].c);
			if (G_LIKELY (glyphs[j].glyph != 0)) {
				glyphs[j].x = requests[i].x - data->x_offs;
				width = _vte_xft_char_width(data->font,
						font, requests[i].c);
				if (width != 0) {
					pad = requests[i].columns * draw->width - width;
					pad = CLAMP(pad / 2, 0, draw->width);
					glyphs[j].x += pad;
				}
				glyphs[j].y = requests[i].y - data->y_offs + draw->ascent;
				j++;
			}
			i++;

			/* find the next displayable character ... */
			ft = NULL;
			for (; i < n_requests; i++) {
				if (G_UNLIKELY (requests[i].c == ' ')) {
					continue;
				}
				ft = _vte_xft_font_for_char(data->font,
						requests[i].c);
				if (G_UNLIKELY (ft == NULL)) {
					continue;
				}
				break;
			}
		} while (j < VTE_DRAW_MAX_LENGTH && ft == font);
		if (j > 0) {
			XftDrawGlyphSpec (data->draw,
					&ftcolor, font, glyphs, j);
		}
		font = ft;
	} while (i < n_requests);
	XftColorFree(data->display, data->visual,
			data->colormap, &ftcolor);
}

static gboolean
_vte_xft_draw_char(struct _vte_draw *draw,
		   struct _vte_draw_text_request *request,
		   GdkColor *color, guchar alpha)
{
	struct _vte_xft_data *data;

	data = (struct _vte_xft_data*) draw->impl_data;
	if (data->font != NULL &&
			_vte_xft_font_for_char(data->font, request->c) != NULL) {
		_vte_xft_draw_text(draw, request, 1, color, alpha);
		return TRUE;
	}
	return FALSE;
}

static void
_vte_xft_draw_rectangle(struct _vte_draw *draw,
			gint x, gint y, gint width, gint height,
			GdkColor *color, guchar alpha)
{
	struct _vte_xft_data *data;
	XRenderColor rcolor;
	XftColor ftcolor;

	data = (struct _vte_xft_data*) draw->impl_data;

	rcolor.red = color->red;
	rcolor.green = color->green;
	rcolor.blue = color->blue;
	rcolor.alpha = (alpha == VTE_DRAW_OPAQUE) ? 0xffff : alpha << 8;
	if (XftColorAllocValue(data->display, data->visual,
			       data->colormap, &rcolor, &ftcolor)) {
		XftDrawRect(data->draw, &ftcolor,
			    x - data->x_offs, y - data->y_offs,
			    width, 1);
		XftDrawRect(data->draw, &ftcolor,
			    x - data->x_offs, y - data->y_offs,
			    1, height);
		XftDrawRect(data->draw, &ftcolor,
			    x - data->x_offs, y + height - 1 - data->y_offs,
			    width, 1);
		XftDrawRect(data->draw, &ftcolor,
			    x + width - 1 - data->x_offs, y - data->y_offs,
			    1, height);
		XftColorFree(data->display, data->visual, data->colormap,
			     &ftcolor);
	}
}

static void
_vte_xft_fill_rectangle(struct _vte_draw *draw,
			gint x, gint y, gint width, gint height,
			GdkColor *color, guchar alpha)
{
	struct _vte_xft_data *data;
	XRenderColor rcolor;
	XftColor ftcolor;

	data = (struct _vte_xft_data*) draw->impl_data;

	rcolor.red = color->red;
	rcolor.green = color->green;
	rcolor.blue = color->blue;
	rcolor.alpha = (alpha == VTE_DRAW_OPAQUE) ? 0xffff : alpha << 8;
	if (XftColorAllocValue(data->display, data->visual,
			       data->colormap, &rcolor, &ftcolor)) {
		XftDrawRect(data->draw, &ftcolor,
			    x - data->x_offs, y - data->y_offs, width, height);
		XftColorFree(data->display, data->visual, data->colormap,
			     &ftcolor);
	}
}

static void
_vte_xft_set_scroll(struct _vte_draw *draw, gint x, gint y)
{
	struct _vte_xft_data *data;
	data = (struct _vte_xft_data*) draw->impl_data;
	data->scrollx = x;
	data->scrolly = y;
}

const struct _vte_draw_impl _vte_draw_xft = {
	"Xft",
	_vte_xft_check,
	_vte_xft_create,
	_vte_xft_destroy,
	_vte_xft_get_visual,
	_vte_xft_get_colormap,
	_vte_xft_start,
	_vte_xft_end,
	_vte_xft_set_background_color,
	_vte_xft_set_background_image,
	FALSE,
	_vte_xft_clip,
	_vte_xft_clear,
	_vte_xft_set_text_font,
	_vte_xft_get_text_width,
	_vte_xft_get_text_height,
	_vte_xft_get_text_ascent,
	_vte_xft_get_char_width,
	_vte_xft_get_using_fontconfig,
	_vte_xft_draw_text,
	_vte_xft_draw_char,
	_vte_xft_draw_rectangle,
	_vte_xft_fill_rectangle,
	_vte_xft_set_scroll,
};
