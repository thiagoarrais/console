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

#include "../config.h"
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <glib.h>
#include <glib-object.h>
#include "debug.h"
#include "marshal.h"
#include "reaper.h"

#ifdef HAVE_LOCALE_H
#include <locale.h>
#endif
#include <glib/gi18n-lib.h>


static VteReaper *singleton_reaper = NULL;
struct reaper_info {
	int signum;
	pid_t pid;
	int status;
};

G_DEFINE_TYPE(VteReaper, vte_reaper, G_TYPE_OBJECT)

static void
vte_reaper_signal_handler(int signum)
{
	struct reaper_info info;
	int status;

	/* This might become more general-purpose in the future, but for now
	 * just forget about signals other than SIGCHLD. */
	info.signum = signum;
	if (signum != SIGCHLD) {
		return;
	}

	if ((singleton_reaper != NULL) && (singleton_reaper->iopipe[0] != -1)) {
		info.pid = waitpid(-1, &status, WNOHANG);
		if (info.pid != -1) {
			info.status = status;
			if (write(singleton_reaper->iopipe[1], "", 0) == 0) {
				write(singleton_reaper->iopipe[1],
				      &info, sizeof(info));
			}
		}
	}
}

static gboolean
vte_reaper_emit_signal(GIOChannel *channel, GIOCondition condition,
		       gpointer data)
{
	struct reaper_info info;
	if (condition != G_IO_IN) {
		return FALSE;
	}
	g_assert(data == singleton_reaper);
	read(singleton_reaper->iopipe[0], &info, sizeof(info));
	if (info.signum == SIGCHLD) {
		_vte_debug_print(VTE_DEBUG_SIGNALS,
				"Reaper emitting child-exited signal.\n");
		g_signal_emit_by_name(data, "child-exited",
				      info.pid, info.status);
	}
	return TRUE;
}

#if GLIB_CHECK_VERSION(2,4,0)
static void
vte_reaper_child_watch_cb(GPid pid, gint status, gpointer data)
{
	_vte_debug_print(VTE_DEBUG_SIGNALS,
			"Reaper emitting child-exited signal.\n");
	g_signal_emit_by_name(data, "child-exited", pid, status);
	g_spawn_close_pid (pid);
}
#endif

/**
 * vte_reaper_add_child:
 * @pid: the ID of a child process which will be monitored
 *
 * Ensures that child-exited signals will be emitted when @pid exits.  This is
 * necessary for correct operation when running with glib versions >= 2.4.
 *
 * Returns: the new source ID
 *
 * Since 0.11.11
 */
int
vte_reaper_add_child(GPid pid)
{
#if GLIB_CHECK_VERSION(2,4,0)
	return g_child_watch_add_full(G_PRIORITY_HIGH,
				      pid,
				      vte_reaper_child_watch_cb,
				      vte_reaper_get(),
				      (GDestroyNotify)g_object_unref);
#endif
	return VTE_INVALID_SOURCE;
}

static void
vte_reaper_init(VteReaper *reaper)
{
	struct sigaction action;
	int ret;

	/* Open the signal pipe. */
	ret = pipe(reaper->iopipe);
	if (ret == -1) {
		g_error(_("Error creating signal pipe."));
	}

	/* Create the channel. */
	reaper->channel = g_io_channel_unix_new(reaper->iopipe[0]);

#if GLIB_CHECK_VERSION(2,4,0)
	if ((glib_major_version > 2) || /* 3.x and later */
	    ((glib_major_version == 2) && (glib_minor_version >= 4))) {/* 2.4 */
		return;
	}
#endif

	/* Add the channel to the source list. */
	g_io_add_watch_full(reaper->channel,
			    G_PRIORITY_HIGH,
			    G_IO_IN,
			    vte_reaper_emit_signal,
			    reaper,
			    NULL);

	/* Set the signal handler. */
	action.sa_handler = vte_reaper_signal_handler;
	sigemptyset(&action.sa_mask);
	action.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	sigaction(SIGCHLD, &action, NULL);
	_vte_debug_print(VTE_DEBUG_SIGNALS,
			"Hooked SIGCHLD signal in reaper.\n");
}

static GObject*
vte_reaper_constructor (GType                  type,
                        guint                  n_construct_properties,
                        GObjectConstructParam *construct_properties)
{
  if (singleton_reaper) {
	  return g_object_ref (singleton_reaper);
  } else {
	  GObject *obj;
	  obj = G_OBJECT_CLASS (vte_reaper_parent_class)->constructor (type, n_construct_properties, construct_properties);
	  singleton_reaper = VTE_REAPER (obj);
	  return obj;
  }
}


static void
vte_reaper_finalize(GObject *reaper)
{
	struct sigaction action, old_action;

	/* Reset the signal handler if we still have it hooked. */
	action.sa_handler = SIG_DFL;
	sigemptyset(&action.sa_mask);
	action.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	sigaction(SIGCHLD, NULL, &old_action);
	if (old_action.sa_handler == vte_reaper_signal_handler) {
		sigaction(SIGCHLD, &action, NULL);
	}
	_vte_debug_print(VTE_DEBUG_SIGNALS,
			"Unhooked SIGCHLD signal in reaper.\n");

	/* Remove the channel from the source list. */
	g_source_remove_by_user_data(reaper);

	/* Remove the channel. */
	g_io_channel_unref(VTE_REAPER(reaper)->channel);

	/* Close the pipes. */
	close(VTE_REAPER(reaper)->iopipe[1]);
	close(VTE_REAPER(reaper)->iopipe[0]);

	G_OBJECT_CLASS(vte_reaper_parent_class)->finalize(reaper);
	singleton_reaper = NULL;
}

static void
vte_reaper_class_init(VteReaperClass *klass)
{
	GObjectClass *gobject_class;

	bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
#ifdef HAVE_DECL_BIND_TEXTDOMAIN_CODESET
	bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
#endif

	klass->child_exited_signal = g_signal_new("child-exited",
						  G_OBJECT_CLASS_TYPE(klass),
						  G_SIGNAL_RUN_LAST,
						  0,
						  NULL,
						  NULL,
						  _vte_marshal_VOID__INT_INT,
						  G_TYPE_NONE,
						  2, G_TYPE_INT, G_TYPE_INT);

	gobject_class = G_OBJECT_CLASS(klass);
	gobject_class->constructor = vte_reaper_constructor;
	gobject_class->finalize = vte_reaper_finalize;
}

/**
 * vte_reaper_get:
 *
 * Finds the address of the global #VteReaper object, creating the object if
 * necessary.
 *
 * Returns: the global #VteReaper object, which should not be unreffed.
 */
VteReaper *
vte_reaper_get(void)
{
	return g_object_new(VTE_TYPE_REAPER, NULL);
}

#ifdef REAPER_MAIN
GMainContext *context;
GMainLoop *loop;
pid_t child;

static void
child_exited(GObject *object, int pid, int status, gpointer data)
{
	g_print("[parent] Child with pid %d exited with code %d, "
		"was waiting for %d.\n", pid, status, GPOINTER_TO_INT(data));
	if (child == pid) {
		g_print("[parent] Quitting.\n");
		g_main_loop_quit(loop);
	}
}

int
main(int argc, char **argv)
{
	VteReaper *reaper;
	pid_t p, q;

	_vte_debug_init();

	g_type_init();
	context = g_main_context_default();
	loop = g_main_loop_new(context, FALSE);
	reaper = vte_reaper_get();

	g_print("[parent] Forking.\n");
	p = fork();
	switch (p) {
		case -1:
			g_print("[parent] Fork failed.\n");
			g_assert_not_reached();
			break;
		case 0:
			g_print("[child]  Going to sleep.\n");
			sleep(10);
			g_print("[child]  Quitting.\n");
			_exit(30);
			break;
		default:
			g_print("[parent] Starting to wait for %d.\n", p);
			child = p;
			g_signal_connect(reaper,
					 "child-exited",
					 G_CALLBACK(child_exited),
					 GINT_TO_POINTER(child));
			break;
	}

	g_print("[parent] Forking.\n");
	q = fork();
	switch (q) {
		case -1:
			g_print("[parent] Fork failed.\n");
			g_assert_not_reached();
			break;
		case 0:
			g_print("[child]  Going to sleep.\n");
			sleep(5);
			_exit(5);
			break;
		default:
			g_print("[parent] Not waiting for %d.\n", q);
			break;
	}


	g_main_loop_run(loop);

	g_object_unref(reaper);

	return 0;
}
#endif
