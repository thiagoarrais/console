/*
 * Copyright (C) 2002 Red Hat, Inc.
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

#ifndef vte_debug_h_included
#define vte_debug_h_included

typedef enum {
	VTE_DEBUG_MISC		= 1 << 0,
	VTE_DEBUG_PARSE		= 1 << 1,
	VTE_DEBUG_IO		= 1 << 2,
	VTE_DEBUG_UPDATES	= 1 << 3,
	VTE_DEBUG_EVENTS	= 1 << 4,
	VTE_DEBUG_SIGNALS	= 1 << 5,
	VTE_DEBUG_SELECTION	= 1 << 6,
} VteDebugFlags;

void vte_debug_parse_string(const char *string);
gboolean vte_debug_on(VteDebugFlags flags);

#endif