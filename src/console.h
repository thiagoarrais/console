/*
 * Copyright (C) 2009 Thiago Arrais
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

#ifndef console_console_h_included
#define console_console_h_included

#include <glib-object.h>

#include "vte.h"
#include "controller.h"

/* The widget's type. */
GType console_console_get_type(void);

#define CONSOLE_TYPE_CONSOLE		(console_console_get_type())
#define CONSOLE_CONSOLE(obj)		(G_TYPE_CHECK_INSTANCE_CAST((obj),\
							CONSOLE_TYPE_CONSOLE,\
							Console))
#define CONSOLE_CONSOLE_CLASS(klass)	G_TYPE_CHECK_CLASS_CAST((klass),\
							     CONSOLE_TYPE_CONSOLE,\
							     ConsoleClass)
#define CONSOLE_IS_CONSOLE(obj)		G_TYPE_CHECK_INSTANCE_TYPE((obj),\
						       CONSOLE_TYPE_CONSOLE)
#define CONSOLE_IS_CONSOLE_CLASS(klass)	G_TYPE_CHECK_CLASS_TYPE((klass),\
							     CONSOLE_TYPE_CONSOLE)
#define CONSOLE_CONSOLE_GET_CLASS(obj)	(G_TYPE_INSTANCE_GET_CLASS ((obj), CONSOLE_TYPE_CONSOLE, ConsoleClass))

typedef struct _Console        Console;
typedef struct _ConsoleClass   ConsoleClass;

struct _Console {
	VteTerminal parent;

	/*< private >*/
	ConsoleController *controller;
};

struct _ConsoleClass {
	VteTerminalClass parent_class;

	/* Default signal handlers. */
	void (*line_received)(Console* self, gchar *text, guint size);

	/*< private > */
	guint line_received_signal;
};

GtkWidget *console_console_new(void);

/* Mark a block of data (sent through console_console_feed) as app data that
 * should not be interpreted as user input */
void console_console_begin_app_output(Console *self);
void console_console_finish_app_output(Console *self);

void console_console_set_command_prompt(Console *self, const gchar *text);

void console_console_feed(Console *self, const char *data, glong length);

#endif
