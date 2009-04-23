#include <config.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>

#include "controller.h"

#include "vte.h"
#include "vte-private.h"

static void
console_controller_reset_pending_input(ConsoleController *controller)
{
	InputNode *next_node, *current_node = controller->input_head;
	while(current_node) {
		next_node = current_node->next;
		g_slice_free(InputNode, current_node);
		current_node = next_node;
	}

	InputNode *head_node = g_slice_new(InputNode);
	head_node->previous = head_node;
	head_node->next = NULL;
	head_node->charData = '\0';

	controller->input_head = controller->input_cursor = head_node;
	controller->input_length = controller->input_cursor_position = 0;
}

ConsoleController *
console_controller_new(VteTerminal *terminal)
{
	ConsoleController *controller = g_slice_new0(ConsoleController);

	controller->terminal = terminal;
	controller->user_input_mode = TRUE;
	console_controller_reset_pending_input(controller);

	return controller;
}

void
console_controller_free(ConsoleController *ctrl)
{
	VteCommandHistoryNode *current_cmd, *last_cmd;
	InputNode *current_input, *last_input;

	current_cmd = ctrl->last_cmd;
	while(current_cmd) {
		last_cmd = current_cmd;
		current_cmd = current_cmd->previous;
		g_slice_free(VteCommandHistoryNode, last_cmd);
	}

	current_input = ctrl->input_head;
	while(current_input) {
		last_input = current_input;
		current_input = current_input->next;
		g_slice_free(InputNode, last_input);
	}

	g_slice_free(ConsoleController, ctrl);
}

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
console_controller_stop_user_input(ConsoleController *controller)
{
	controller->user_input_mode = FALSE;
}

void
console_controller_start_user_input(ConsoleController *controller)
{
	controller->user_input_mode = TRUE;
}

void
console_controller_set_command_prompt(ConsoleController *ctrl, const gchar *text)
{
	ctrl->prompt = g_strdup(text);
	ctrl->prompt_length = strlen(text);

	if (ctrl->user_input_mode)
		vte_terminal_feed(ctrl->terminal, text, ctrl->prompt_length);
}

static void
console_controller_store_input(ConsoleController *ctrl, gchar *text, glong length)
{
	glong i;
	InputNode *current_node, *last_node;

	if (!ctrl->user_input_mode) return;

	last_node = ctrl->input_cursor;
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

	ctrl->input_cursor = last_node;
	ctrl->input_length += length;
	ctrl->input_cursor_position += length;
}

void
console_controller_flush_pending_input(ConsoleController *ctrl)
{
	glong i, num_down_steps, cursor_position, num_columns;
	gchar *input_line, *cmd_step_down;
	InputNode *current_node;
	VteCommandHistoryNode *cmd;

	if (!ctrl->user_input_mode) return;

	input_line = (gchar*) g_slice_alloc((ctrl->input_length + 1) * sizeof(gchar));

	current_node = ctrl->input_head->next;
	i = cursor_position = 0;
	while(current_node) {
		if (current_node == ctrl->input_cursor) cursor_position = i;
		input_line[i++] = current_node->charData;
		current_node = current_node->next;
	}
	input_line[i] = '\0';

	num_columns = ctrl->terminal->column_count;
	num_down_steps = ctrl->input_length / num_columns - cursor_position / num_columns;
	if (num_down_steps > 0) {
		cmd_step_down = g_strnfill(num_down_steps, '\n');
		vte_terminal_feed(ctrl->terminal, "\033[O", 3);
		vte_terminal_feed(ctrl->terminal, cmd_step_down, num_down_steps);
		vte_terminal_feed(ctrl->terminal, "\033[N", 3);
		g_free(cmd_step_down);
	}

	cmd = g_slice_new0(VteCommandHistoryNode);
	if (ctrl->last_cmd) {
		ctrl->last_cmd->next = cmd;
		cmd->previous = ctrl->last_cmd;
	} else {
		cmd->previous = cmd;
	}
	cmd->data = input_line;
	ctrl->last_cmd = cmd;

	console_controller_emit_line_received(ctrl->terminal, input_line, ctrl->input_length);
	console_controller_reset_pending_input(ctrl);
	vte_terminal_feed(ctrl->terminal, ctrl->prompt, ctrl->prompt_length);
}

void console_controller_cursor_left(ConsoleController *ctrl) {
	if (ctrl->user_input_mode) {
		ctrl->input_cursor = ctrl->input_cursor->previous;
		if (ctrl->input_cursor_position > 0) ctrl->input_cursor_position--;
	}
}

void console_controller_cursor_right(ConsoleController *ctrl) {
	InputNode *cursor = ctrl->input_cursor;

	if (!ctrl->user_input_mode) return;

	if (cursor->next) ctrl->input_cursor = cursor->next;
	else console_controller_store_input(ctrl, (gchar*) " ", 1);

	ctrl->input_cursor_position++;
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
console_controller_reprint_suffix(ConsoleController *ctrl)
{
	gchar *suffix, *suffix_cmd, *backspace;

	glong bksplen, avlen, suflen = 0;
	InputNode *cursor = ctrl->input_cursor->next;

	if (cursor) {
		avlen = ctrl->input_length + 1;
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
		vte_terminal_feed(ctrl->terminal, suffix_cmd, suflen + bksplen);
		if (suffix_cmd != suffix) g_slice_free1((suflen + bksplen) * sizeof(gchar), suffix_cmd);
		g_slice_free1(avlen * sizeof(gchar), suffix);
		g_slice_free1(bksplen * sizeof(gchar), backspace);
	}
}

void console_controller_delete_current_char(ConsoleController *ctrl)
{
	InputNode *deleted_node = ctrl->input_cursor->next;

	if (!ctrl->user_input_mode) return;

	if (deleted_node) {
		--ctrl->input_length;
		if (deleted_node->previous)
      			deleted_node->previous->next = deleted_node->next;
		if (deleted_node->next)
			deleted_node->next->previous = deleted_node->previous;

		vte_terminal_feed(ctrl->terminal, "\033[0J", 4);
		console_controller_reprint_suffix(ctrl);

		g_slice_free(InputNode, deleted_node);
	}
}

void
console_controller_user_input(ConsoleController *ctrl, gchar *text)
{
	const int length = strlen(text);

	vte_terminal_feed(ctrl->terminal, text, length);
	console_controller_reprint_suffix(ctrl);
	console_controller_store_input(ctrl, text, length);
}

static void
console_controller_clear_input(ConsoleController *ctrl)
{
	gchar *cmdstr;
	glong cmdlen;

	if (ctrl->input_cursor_position == 0) return;

	cmdlen = slice_sprintnum(&cmdstr, "\033[O\033[%dD", ctrl->input_cursor_position);
	vte_terminal_feed(ctrl->terminal, cmdstr, cmdlen);
	g_slice_free1(cmdlen * sizeof(gchar), cmdstr);

	cmdlen = slice_sprintnum(&cmdstr, "\033[%dP\033[0J\033[N", ctrl->input_cursor_position);
	vte_terminal_feed(ctrl->terminal, cmdstr, cmdlen);
	g_slice_free1(cmdlen * sizeof(gchar), cmdstr);

	console_controller_reset_pending_input(ctrl);
}

void
console_controller_command_history_back(ConsoleController *ctrl)
{
	VteCommandHistoryNode *history = ctrl->cmd_history;
	console_controller_clear_input(ctrl);
	if (!history)
		ctrl->cmd_history = history = ctrl->last_cmd;
	else
		ctrl->cmd_history = history = history->previous;

	if (history) console_controller_user_input(ctrl, history->data);
}

void
console_controller_command_history_forward(ConsoleController *ctrl)
{
	console_controller_clear_input(ctrl);
	if (!ctrl->cmd_history) return;
	VteCommandHistoryNode *history = ctrl->cmd_history->next;
	ctrl->cmd_history = history;
	if (history) console_controller_user_input(ctrl, history->data);
}

void
console_controller_cursor_home(ConsoleController *ctrl)
{
	gchar *cmdstr;
	glong cmdlen;

	cmdlen = slice_sprintnum(&cmdstr, "\033[%dD", ctrl->input_cursor_position);
	vte_terminal_feed(ctrl->terminal, cmdstr, cmdlen);

	g_slice_free1(cmdlen * sizeof(gchar), cmdstr);
}

void
console_controller_cursor_end(ConsoleController *ctrl)
{
	gchar *cmdstr;
	glong cmdlen, num_backsteps = ctrl->input_length - ctrl->input_cursor_position;

	if (0 == num_backsteps) return;

	cmdlen = slice_sprintnum(&cmdstr, "\033[%dC", num_backsteps);
	vte_terminal_feed(ctrl->terminal, cmdstr, cmdlen);

	g_slice_free1(cmdlen * sizeof(gchar), cmdstr);
}

gboolean
console_controller_check_cursor_at_beginning(ConsoleController *ctrl)
{
	InputNode *cursor = ctrl->input_cursor;
	return (cursor == cursor->previous);
}
