#include <config.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>

#include "controller.h"

#include "vte.h"
#include "vte-private.h"

/* Emit a "line-received" signal. */
static void
console_controller_emit_line_received(VteTerminal *terminal, const gchar *text, guint length)
{
	const char *result = NULL;
	char *wrapped = NULL;

	_vte_debug_print(VTE_DEBUG_SIGNALS,
			"Emitting `line-received' of %d bytes.\n", length);

	if (length == (guint)-1) {
		length = strlen(text);
		result = text;
	} else {
		result = wrapped = g_slice_alloc(length + 1);
		memcpy(wrapped, text, length);
		wrapped[length] = '\0';
	}

	g_signal_emit_by_name(terminal, "line-received", result, length);

	if(wrapped)
		g_slice_free1(length+1, wrapped);
}

void
console_controller_stop_user_input(VteTerminal *terminal)
{
  terminal->pvt->user_input_mode = FALSE;
}

void
console_controller_start_user_input(VteTerminal *terminal)
{
  terminal->pvt->user_input_mode = TRUE;
}

static void
console_controller_store_input(VteTerminal *terminal, gchar *text, glong length)
{
	glong i;
	InputNode *current_node, *last_node;
	VteTerminalPrivate *pvt = terminal->pvt;

	if (!pvt->user_input_mode) return;

	last_node = pvt->input_cursor;
  	for(i = 0; i < length; ++i) {
		InputNode *next_node = last_node->next;
		current_node = g_slice_new0(InputNode);
		current_node->charData = text[i];
		current_node->previous = last_node;
		last_node->next = current_node;
		if (next_node) {
			current_node->next = next_node;
			next_node->previous = current_node;
		}
		last_node = current_node;
	}

	terminal->pvt->input_cursor = last_node;
	terminal->pvt->input_length += length;
	terminal->pvt->input_cursor_position += length;
}

void console_controller_reset_pending_input(VteTerminal *terminal)
{
	InputNode *next_node, *current_node = terminal->pvt->input_head;
	while(current_node) {
		next_node = current_node->next;
		g_slice_free(InputNode, current_node);
		current_node = next_node;
	}

	InputNode *head_node = g_slice_new(InputNode);
	head_node->previous = head_node;
	head_node->next = NULL;
	head_node->charData = '\0';

	terminal->pvt->input_head = terminal->pvt->input_cursor = head_node;
	terminal->pvt->input_length = terminal->pvt->input_cursor_position = 0;
}

void
console_controller_flush_pending_input(VteTerminal *terminal)
{
	glong i;
	gchar *input_line;
	InputNode *current_node;
	VteCommandHistoryNode *cmd;
	VteTerminalPrivate *pvt = terminal->pvt;

	if (!pvt->user_input_mode) return;

	input_line = (gchar*) g_slice_alloc((pvt->input_length + 1) * sizeof(gchar));

	current_node = pvt->input_head->next;
	i = 0;
	while(current_node) {
		input_line[i++] = current_node->charData;
		current_node = current_node->next;
	}
	input_line[i] = '\0';

	cmd = g_slice_new0(VteCommandHistoryNode);
	if (pvt->last_cmd) {
		pvt->last_cmd->next = cmd;
		cmd->previous = pvt->last_cmd;
	} else {
		cmd->previous = cmd;
	}
	cmd->data = input_line;
	pvt->last_cmd = cmd;

	console_controller_emit_line_received(terminal, input_line, pvt->input_length);
	console_controller_reset_pending_input(terminal);
}

void console_controller_cursor_left(VteTerminal *terminal) {
	if (terminal->pvt->user_input_mode) {
		terminal->pvt->input_cursor = terminal->pvt->input_cursor->previous;
		if (terminal->pvt->input_cursor_position > 0) terminal->pvt->input_cursor_position--;
	}
}

void console_controller_cursor_right(VteTerminal *terminal) {
	InputNode *cursor = terminal->pvt->input_cursor;

	if (!terminal->pvt->user_input_mode) return;

	if (cursor->next) terminal->pvt->input_cursor = cursor->next;
	else console_controller_store_input(terminal, " ", 1);

	terminal->pvt->input_cursor_position++;
}

static glong
slice_sprintnum(gchar **output, const gchar *format, const glong number)
{
	glong outputlen = strlen(format); //strlen - 1 (for the '%' sign) + 1 (for the null terminator)
	glong tmplen = number;
	while((tmplen = tmplen / 10) > 0) outputlen++;

	*output = (gchar*) g_slice_alloc(outputlen * sizeof(gchar));

	g_sprintf(*output, format, number);

	return outputlen;
}

static void
console_controller_reprint_suffix(VteTerminal *terminal)
{
	gchar *suffix, *suffix_cmd, *backspace;

	glong bksplen, avlen, suflen = 0;
	VteTerminalPrivate *pvt = terminal->pvt;
	InputNode *cursor = pvt->input_cursor->next;

	if (cursor) {
		avlen = pvt->input_length + 1;
		suffix = (gchar*) g_slice_alloc(avlen * sizeof(gchar));
		while(cursor) {
			suffix[suflen++] = cursor->charData;
			cursor = cursor->next;
		}
		suffix[suflen] = '\0';

		bksplen = slice_sprintnum(&backspace, "\033[O\033[%dD\033[N", suflen);
		if (suflen + bksplen > avlen) {
			suffix_cmd = (gchar*) g_slice_alloc((suflen + bksplen) * sizeof(gchar));
			g_stpcpy(suffix_cmd, suffix);
		} else {
			suffix_cmd = suffix;
		}
		g_stpcpy(suffix_cmd + suflen, backspace);
	}

	if (suflen) {
		vte_terminal_feed(terminal, suffix_cmd, suflen + bksplen);
		if (suffix_cmd != suffix) g_slice_free1((suflen + bksplen) * sizeof(gchar), suffix_cmd);
		g_slice_free1(avlen * sizeof(gchar), suffix);
		g_slice_free1(bksplen * sizeof(gchar), backspace);
	}
}

void console_controller_delete_current_char(VteTerminal *terminal)
{
	InputNode *deleted_node = terminal->pvt->input_cursor->next;

	if (!terminal->pvt->user_input_mode) return;

	if (deleted_node) {
		--terminal->pvt->input_length;
		if (deleted_node->previous)
      			deleted_node->previous->next = deleted_node->next;
		if (deleted_node->next)
			deleted_node->next->previous = deleted_node->previous;

		vte_terminal_feed(terminal, "\033[0J", 4);
		console_controller_reprint_suffix(terminal);

		g_slice_free(InputNode, deleted_node);
	}
}

void
console_controller_user_input(VteTerminal *terminal, gchar *text)
{
	const int length = strlen(text);

	vte_terminal_feed(terminal, text, length);
	console_controller_reprint_suffix(terminal);
	console_controller_store_input(terminal, text, length);
}

static void
console_controller_clear_input(VteTerminal *terminal)
{
	gchar *cmdstr;
	glong cmdlen;

	if (terminal->pvt->input_cursor_position == 0) return;

	cmdlen = slice_sprintnum(&cmdstr, "\033[O\033[%dD", terminal->pvt->input_cursor_position);
	vte_terminal_feed(terminal, cmdstr, cmdlen);
	g_slice_free1(cmdlen * sizeof(gchar), cmdstr);

	cmdlen = slice_sprintnum(&cmdstr, "\033[%dP\033[0J\033[N", terminal->pvt->input_cursor_position);
	vte_terminal_feed(terminal, cmdstr, cmdlen);
	g_slice_free1(cmdlen * sizeof(gchar), cmdstr);

	console_controller_reset_pending_input(terminal);
}

void
console_controller_command_history_back(VteTerminal *terminal)
{
	VteCommandHistoryNode *history = terminal->pvt->cmd_history;
	console_controller_clear_input(terminal);
	if (!history)
		terminal->pvt->cmd_history = history = terminal->pvt->last_cmd;
	else
		terminal->pvt->cmd_history = history = history->previous;

	if (history) console_controller_user_input(terminal, history->data);
}

void
console_controller_command_history_forward(VteTerminal *terminal)
{
	console_controller_clear_input(terminal);
	if (!terminal->pvt->cmd_history) return;
	VteCommandHistoryNode *history = terminal->pvt->cmd_history->next;
	terminal->pvt->cmd_history = history;
	if (history) console_controller_user_input(terminal, history->data);
}

void
console_controller_cursor_home(VteTerminal *terminal)
{
	gchar *cmdstr;
	glong cmdlen;

	cmdlen = slice_sprintnum(&cmdstr, "\033[%dD", terminal->pvt->input_cursor_position);
	vte_terminal_feed(terminal, cmdstr, cmdlen);

	g_slice_free1(cmdlen * sizeof(gchar), cmdstr);
}

void
console_controller_cursor_end(VteTerminal *terminal)
{
	gchar *cmdstr;
	glong cmdlen, num_backsteps = terminal->pvt->input_length - terminal->pvt->input_cursor_position;

	if (0 == num_backsteps) return;

	cmdlen = slice_sprintnum(&cmdstr, "\033[%dC", num_backsteps);
	vte_terminal_feed(terminal, cmdstr, cmdlen);

	g_slice_free1(cmdlen * sizeof(gchar), cmdstr);
}
