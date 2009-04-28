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

#include <config.h>

#include "console.h"

#include "vte-private.h"
#include "marshal.h"

G_DEFINE_TYPE(Console, console_console, VTE_TYPE_TERMINAL)

GtkWidget *
console_console_new(void)
{
	return g_object_new(CONSOLE_TYPE_CONSOLE, NULL);
}

void
console_console_begin_app_output(Console *self)
{
	vte_terminal_feed(VTE_TERMINAL(self), "\033[O", 3);
}

void
console_console_finish_app_output(Console *self)
{
	vte_terminal_feed(VTE_TERMINAL(self), "\033[N", 3);
}

void
console_console_set_command_prompt(Console *self, const gchar *text)
{
	console_controller_set_command_prompt(self->controller, text);
}

void
console_console_feed(Console *self, const char *data, glong length)
{
	vte_terminal_feed(VTE_TERMINAL(self), data, length);
}

void
console_console_set_font_from_string(Console *self, const char *name)
{
	vte_terminal_set_font_from_string(VTE_TERMINAL(self), name);
}

void
console_console_set_mouse_autohide(Console *self, gboolean setting)
{
	vte_terminal_set_mouse_autohide(VTE_TERMINAL(self), setting);
}

static void
console_console_dispose (GObject *gobject)
{
	Console *self = CONSOLE_CONSOLE (gobject);

   	self->controller = NULL;

	G_OBJECT_CLASS (console_console_parent_class)->dispose (gobject);
}

static void
console_console_finalize (GObject *gobject)
{
	G_OBJECT_CLASS (console_console_parent_class)->finalize (gobject);
}

static void
console_console_class_init (ConsoleClass *klass)
{
	GObjectClass *gobject_class;

	gobject_class = G_OBJECT_CLASS (klass);

	gobject_class->dispose = console_console_dispose;
	gobject_class->finalize = console_console_finalize;

	klass->line_received = NULL;

	klass->line_received_signal =
		g_signal_new("line-received",
			     G_OBJECT_CLASS_TYPE(klass),
			     G_SIGNAL_RUN_LAST,
			     G_STRUCT_OFFSET(ConsoleClass, line_received),
			     NULL,
			     NULL,
			     _vte_marshal_VOID__STRING,
			     G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
console_console_init (Console *self)
{
	VteTerminal *terminal = VTE_TERMINAL(self);

	GTK_WIDGET_SET_FLAGS(self, GTK_CAN_FOCUS);

	self->controller = terminal->pvt->controller;
}

