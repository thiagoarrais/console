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

#ifndef vte_ring_h_included
#define vte_ring_h_included

#ident "$Id$"

#include <glib.h>

G_BEGIN_DECLS

typedef struct _VteRing VteRing;
typedef void (*VteRingFreeFunc)(gpointer freeing, gpointer data);

struct _VteRing {
	VteRingFreeFunc free;
	gpointer user_data;
	gpointer *array;
	glong delta, length, max;
};

#define vte_ring_contains(__ring, __position) \
	((__position >= (__ring)->delta) && \
	 (__position < (__ring)->delta + (__ring)->length))
#define vte_ring_delta(__ring) ((__ring)->delta)
#define vte_ring_length(__ring) ((__ring)->length)
#define vte_ring_next(__ring) ((__ring)->delta + (__ring)->length)
#define vte_ring_max(__ring) ((__ring)->max)
#define vte_ring_at(__ring, __position) \
	((__ring)->array[__position % (__ring)->max] ? \
	 (__ring)->array[__position % (__ring)->max] : \
	 (g_error("NULL at %ld(->%ld) delta %ld, length %ld, max %ld next %ld" \
		  " at %d\n", \
		  __position, __position % (__ring)->max, \
		  (__ring)->delta, (__ring)->length, (__ring)->max, \
		  (__ring)->delta + (__ring)->length, \
		  __LINE__), NULL))
#define vte_ring_index(__ring, __cast, __position) \
	(__cast) vte_ring_at(__ring, __position)

VteRing *vte_ring_new(glong max_elements,
		      VteRingFreeFunc free,
		      gpointer data);
VteRing *vte_ring_new_with_delta(glong max_elements, glong delta,
				 VteRingFreeFunc free, gpointer data);
void vte_ring_insert(VteRing *ring, glong position, gpointer data);
void vte_ring_remove(VteRing *ring, glong position, gboolean free_element);
void vte_ring_append(VteRing *ring, gpointer data);
void vte_ring_free(VteRing *ring, gboolean free_elements);

G_END_DECLS

#endif
