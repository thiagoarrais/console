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

#ifndef vte_vtebg_included
#define vte_vtebg_included

#include <gtk/gtk.h>

G_BEGIN_DECLS

struct VteBgPrivate;

typedef struct _VteBg VteBg;
struct _VteBg {
	GObject parent;
	GdkPixmap *root_pixmap;
	struct VteBgNative *native;
	struct VteBgPrivate *pvt;
};

typedef struct _VteBgClass VteBgClass;
struct _VteBgClass {
	GObjectClass parent_class;
	guint root_pixmap_changed;
};

#define VTE_TYPE_BG vte_bg_get_type()
#define VTE_BG(obj)	       (GTK_CHECK_CAST((obj), VTE_TYPE_BG, VteBg))
#define VTE_BG_CLASS(klass)    (GTK_CHECK_CLASS_CAST((klass), VTE_TYPE_BG, VteBgClass)
#define VTE_IS_BG(obj)	       GTK_CHECK_TYPE((obj), VTE_TYPE_BG)
#define VTE_IS_BG_CLASS(klass) GTK_CHECK_CLASS_TYPE((klass), VTE_TYPE_BG)
#define VTE_BG_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS((obj), VTE_TYPE_BG, VteBgClass))

GType vte_bg_get_type(void);

VteBg *vte_bg_get(void);

enum VteBgSourceType {
	VTE_BG_SOURCE_NONE,
	VTE_BG_SOURCE_ROOT,
	VTE_BG_SOURCE_PIXBUF,
	VTE_BG_SOURCE_FILE
};

GdkPixmap *vte_bg_get_pixmap(VteBg *bg, enum VteBgSourceType source_type,
			     GdkPixbuf *source_pixbuf,
			     const char *source_file,
			     const GdkColor *tint, double saturation,
			     GdkColormap *colormap);

GdkPixbuf *vte_bg_get_pixbuf(VteBg *bg, enum VteBgSourceType source_type,
			     GdkPixbuf *source_pixbuf,
			     const char *source_file,
			     const GdkColor *tint, double saturation);

G_END_DECLS

#endif
